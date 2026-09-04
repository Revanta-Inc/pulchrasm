/*
 * fast_grid.h -- contiguous cell-array layout for PULCHRA's excluded-volume
 * conflict search (get_conflicts).
 *
 * Upstream get_conflicts() walks, for every atom of a residue, the 5x5x5 block
 * of 6 A grid cells around the atom's ORIGINAL cell, following a linked list
 * of atom_list nodes in each cell and dereferencing atom_type structs to fetch
 * coordinates.  For a 600-residue protein that is ~32 M list-node visits per
 * run; the time is pure pointer chasing.  This file keeps the exact same
 * search semantics but changes the data layout:
 *
 *   1. every grid cell owns a contiguous slab of (x, y, z, atom*) entries
 *      (structure-of-arrays), mirrored from the upstream linked lists at
 *      allocate_grid() time;
 *   2. add_replace() -- the only routine that moves atoms while the grid is in
 *      use -- writes the new coordinates through to the slab and expands the
 *      cell's bounding box;
 *   3. get_conflicts() tests the distance FIRST (identical arithmetic, so the
 *      identical rounding), and only evaluates the name/flag "merged" rules
 *      for the rare pairs inside the cut-off;
 *   4. a cell whose current bounding box is at least cut-off away from the
 *      probe atom is skipped.  This is exact: IEEE rounding is monotone, so
 *      the box distance computed the same way is <= every pair distance in it.
 *
 * Because the neighbourhood, the arithmetic and the counting predicate are
 * unchanged, the returned count is bit-for-bit identical to upstream (verified
 * on the 39-protein CGBack test set and with the PULCHRA_GRID_SELFCHECK build).
 *
 * Compile with -DPULCHRA_ORIG_GRID to get the untouched upstream code path, or
 * with -DPULCHRA_GRID_SELFCHECK to run both and abort on any disagreement.
 */

#ifndef PULCHRA_FAST_GRID_H
#define PULCHRA_FAST_GRID_H

static int fg_ready = 0;
static int fg_xg, fg_yg, fg_zg, fg_ncells, fg_nslots;
/* One 64-byte record per cell: bbox + slot range, so the rejection test
 * touches a single cache line. */
typedef struct {
  real lo[3];
  real hi[3];
  int start;   /* first slot of this cell */
  int count;   /* number of slots         */
  int pad[2];
} fg_cell_rec;
static fg_cell_rec *fg_cells = NULL;
static real *fg_x = NULL, *fg_y = NULL, *fg_z = NULL; /* SoA coordinates */
static atom_type **fg_atom = NULL;/* owning atom of each slot           */

static inline int fg_cell(int x, int y, int z) {
  return (x * (fg_yg + 1) + y) * (fg_zg + 1) + z;
}

static void fg_bbox_add(int c, real x, real y, real z) {
  real *lo = fg_cells[c].lo, *hi = fg_cells[c].hi;
  if (x < lo[0]) lo[0] = x;
  if (y < lo[1]) lo[1] = y;
  if (z < lo[2]) lo[2] = z;
  if (x > hi[0]) hi[0] = x;
  if (y > hi[1]) hi[1] = y;
  if (z > hi[2]) hi[2] = z;
}

/* Build the slabs from the finished upstream linked-list grid. */
static void fast_grid_build(atom_list ****grid, int xgrid, int ygrid, int zgrid) {
  int x, y, z, c, s;
  atom_list *l;

  /* Degenerate grids (non-finite coordinates make allocate_grid() compute
   * INT_MIN dimensions and a NULL grid) stay on the upstream code path, whose
   * clamped loops simply visit nothing.  Exact for every valid structure. */
  if (!grid || xgrid < 0 || ygrid < 0 || zgrid < 0 ||
      (long long)(xgrid + 1) * (ygrid + 1) * (zgrid + 1) > (1LL << 26)) {
    fg_ready = 0;
    return;
  }

  fg_xg = xgrid; fg_yg = ygrid; fg_zg = zgrid;
  fg_ncells = (xgrid + 1) * (ygrid + 1) * (zgrid + 1);
  fg_cells = (fg_cell_rec *)calloc(sizeof(fg_cell_rec) * fg_ncells, 1);

  fg_nslots = 0;
  for (x = 0; x <= xgrid; x++)
    for (y = 0; y <= ygrid; y++)
      for (z = 0; z <= zgrid; z++) {
        c = fg_cell(x, y, z);
        for (l = grid[x][y][z]; l; l = l->next) fg_cells[c].count++;
        fg_cells[c].start = fg_nslots;
        fg_nslots += fg_cells[c].count;
        fg_cells[c].lo[0] = fg_cells[c].lo[1] = fg_cells[c].lo[2] = 1e30;
        fg_cells[c].hi[0] = fg_cells[c].hi[1] = fg_cells[c].hi[2] = -1e30;
      }

  fg_x = (real *)calloc(sizeof(real) * (fg_nslots + 1), 1);
  fg_y = (real *)calloc(sizeof(real) * (fg_nslots + 1), 1);
  fg_z = (real *)calloc(sizeof(real) * (fg_nslots + 1), 1);
  fg_atom = (atom_type **)calloc(sizeof(atom_type *) * (fg_nslots + 1), 1);

  for (x = 0; x <= xgrid; x++)
    for (y = 0; y <= ygrid; y++)
      for (z = 0; z <= zgrid; z++) {
        c = fg_cell(x, y, z);
        s = fg_cells[c].start;
        for (l = grid[x][y][z]; l; l = l->next, s++) {
          atom_type *a = l->atom;
          fg_x[s] = a->x; fg_y[s] = a->y; fg_z[s] = a->z;
          fg_atom[s] = a;
          a->gslot = s + 1; /* 0 means "not in grid" */
          fg_bbox_add(c, a->x, a->y, a->z);
        }
      }
  fg_ready = 1;
}

/* Called from add_replace() after an atom's coordinates were (possibly) changed. */
static inline void fast_grid_update(atom_type *a) {
  int s, c;
  if (!fg_ready || a->gslot <= 0) return;
  s = a->gslot - 1;
  fg_x[s] = a->x; fg_y[s] = a->y; fg_z[s] = a->z;
  c = fg_cell(a->gx, a->gy, a->gz);
  fg_bbox_add(c, a->x, a->y, a->z);
}

/* Same predicate as upstream get_conflicts(), evaluated only for close pairs. */
static inline int fg_merged(res_type *res, atom_type *atom, atom_type *atom2) {
  int merged = 0;
  if (res == atom2->res) { // self-xvol
    if (atom->flag & FLAG_SIDECHAIN && atom2->flag & FLAG_SIDECHAIN)
      merged = 1;
    if (atom->flag & FLAG_BACKBONE && atom2->flag & FLAG_BACKBONE)
      merged = 1;
    if (atom->name[0] == 'C' && atom->name[1] == 'A' &&
        atom2->name[0] == 'C' && atom2->name[1] == 'B')
      merged = 1;
    if (atom->name[0] == 'C' && atom->name[1] == 'B' &&
        atom2->name[0] == 'C' && atom2->name[1] == 'A')
      merged = 1;
    if (res->name[0] == 'P') {
      if (atom->name[0] == 'C' && atom->name[1] == 'D' &&
          atom2->name[0] == 'N' && atom2->name[1] == ' ')
        merged = 1;
      if (atom->name[0] == 'N' && atom->name[1] == ' ' &&
          atom2->name[0] == 'C' && atom2->name[1] == 'D')
        merged = 1;
    }
  } else if (res->next == atom2->res || res == atom2->res->next) {
    if (atom->name[0] == 'C' && atom->name[1] == ' ' &&
        atom2->name[0] == 'N' && atom2->name[1] == ' ')
      merged = 1;
    if (atom->name[0] == 'N' && atom->name[1] == ' ' &&
        atom2->name[0] == 'C' && atom2->name[1] == ' ')
      merged = 1;
  }
  if (atom->flag & FLAG_BACKBONE && atom2->flag & FLAG_BACKBONE)
    merged = 1; // for now
  if (atom->flag & FLAG_SCM || atom2->flag & FLAG_SCM)
    merged = 1; // for now
  return merged;
}

static int get_conflicts_fast(res_type *res, int xgrid, int ygrid, int zgrid) {
  atom_type *atom, *atom2;
  int ii, jj, kk, s, s_end, con, gi0, gi1, gj0, gj1, gk0, gk1;
  real ax, ay, az, dx, dy, dz, dd, bx, by, bz, bd;
  real rlo[3], rhi[3];
  const real cut2 = _SG_XVOL_DIST * _SG_XVOL_DIST;
  const fg_cell_rec *cr;

  if (!res->atoms) return 0;

  /* Residue-level bounds: union of the per-atom 5x5x5 neighbourhoods (in
   * original-cell space) and bbox of the atoms' CURRENT positions. */
  atom = res->atoms;
  gi0 = gi1 = atom->gx; gj0 = gj1 = atom->gy; gk0 = gk1 = atom->gz;
  rlo[0] = rhi[0] = atom->x; rlo[1] = rhi[1] = atom->y; rlo[2] = rhi[2] = atom->z;
  for (atom = atom->next; atom; atom = atom->next) {
    if (atom->gx < gi0) gi0 = atom->gx;
    if (atom->gx > gi1) gi1 = atom->gx;
    if (atom->gy < gj0) gj0 = atom->gy;
    if (atom->gy > gj1) gj1 = atom->gy;
    if (atom->gz < gk0) gk0 = atom->gz;
    if (atom->gz > gk1) gk1 = atom->gz;
    if (atom->x < rlo[0]) rlo[0] = atom->x;
    if (atom->x > rhi[0]) rhi[0] = atom->x;
    if (atom->y < rlo[1]) rlo[1] = atom->y;
    if (atom->y > rhi[1]) rhi[1] = atom->y;
    if (atom->z < rlo[2]) rlo[2] = atom->z;
    if (atom->z > rhi[2]) rhi[2] = atom->z;
  }
  gi0 -= 2; gi1 += 2; gj0 -= 2; gj1 += 2; gk0 -= 2; gk1 += 2;
  if (gi0 < 0) gi0 = 0;
  if (gi1 >= xgrid) gi1 = xgrid - 1;
  if (gj0 < 0) gj0 = 0;
  if (gj1 >= ygrid) gj1 = ygrid - 1;
  if (gk0 < 0) gk0 = 0;
  if (gk1 >= zgrid) gk1 = zgrid - 1;

  con = 0;
  for (ii = gi0; ii <= gi1; ii++)
    for (jj = gj0; jj <= gj1; jj++)
      for (kk = gk0; kk <= gk1; kk++) {
        cr = &fg_cells[fg_cell(ii, jj, kk)];
        if (cr->count == 0) continue;

        /* residue bbox vs cell bbox: box-box distance is a lower bound of
         * every atom/atom2 distance in the pair of boxes (monotone rounding) */
        bx = rhi[0] < cr->lo[0] ? cr->lo[0] - rhi[0] : (rlo[0] > cr->hi[0] ? rlo[0] - cr->hi[0] : 0.0);
        bx *= bx;
        if (bx >= cut2) continue;
        by = rhi[1] < cr->lo[1] ? cr->lo[1] - rhi[1] : (rlo[1] > cr->hi[1] ? rlo[1] - cr->hi[1] : 0.0);
        by *= by;
        if (bx + by >= cut2) continue;
        bz = rhi[2] < cr->lo[2] ? cr->lo[2] - rhi[2] : (rlo[2] > cr->hi[2] ? rlo[2] - cr->hi[2] : 0.0);
        bz *= bz;
        if (bx + by + bz >= cut2) continue;

        for (atom = res->atoms; atom; atom = atom->next) {
          /* this cell must lie in THIS atom's own 5x5x5 neighbourhood */
          if (ii < atom->gx - 2 || ii > atom->gx + 2 ||
              jj < atom->gy - 2 || jj > atom->gy + 2 ||
              kk < atom->gz - 2 || kk > atom->gz + 2)
            continue;
          ax = atom->x; ay = atom->y; az = atom->z;

          /* exact per-atom bounding-box rejection (see header comment) */
          bx = ax < cr->lo[0] ? cr->lo[0] - ax : (ax > cr->hi[0] ? ax - cr->hi[0] : 0.0);
          bx *= bx;
          if (bx >= cut2) continue;
          by = ay < cr->lo[1] ? cr->lo[1] - ay : (ay > cr->hi[1] ? ay - cr->hi[1] : 0.0);
          by *= by;
          if (bx + by >= cut2) continue;
          bz = az < cr->lo[2] ? cr->lo[2] - az : (az > cr->hi[2] ? az - cr->hi[2] : 0.0);
          bz *= bz;
          bd = bx + by + bz;
          if (bd >= cut2) continue;

          s_end = cr->start + cr->count;
          for (s = cr->start; s < s_end; s++) {
            dx = ax - fg_x[s];
            dx *= dx;
            dy = ay - fg_y[s];
            dy *= dy;
            dz = az - fg_z[s];
            dz *= dz;
            dd = dx + dy + dz;
            if (dd < cut2) {
              atom2 = fg_atom[s];
              if (atom2 && atom2->res && !fg_merged(res, atom, atom2))
                con++;
            }
          }
        }
      }
  return con;
}

#endif /* PULCHRA_FAST_GRID_H */
