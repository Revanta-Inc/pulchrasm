/*
 * fast_tables.h -- exact accelerators for PULCHRA's statistical-table lookups.
 *
 * 1. Rotamer index (rebuild_sidechains).  Upstream scans all ~19 900 rows of
 *    rot_stat_idx for every residue and keeps the rows whose first column
 *    equals the residue type.  The table is sorted by type, so we compute the
 *    [first, last] row range of every type once and scan only that range.
 *    Rows are still visited in ascending order and the type test is kept, so
 *    the insertion into SORTED_ROTAMERS (including tie-breaking) is unchanged.
 *    If the table were ever not contiguous per type we fall back to a full scan.
 *
 * 2. Backbone fragments (rebuild_backbone).  Upstream scans nco_stat /
 *    nco_stat_pro (108-byte structs) reading only the three int bins of each.
 *    We copy the bins into a contiguous int array once and run the *same* loop
 *    (same hit formula, same '<' test, same early exit) over the compact copy.
 */

#ifndef PULCHRA_FAST_TABLES_H
#define PULCHRA_FAST_TABLES_H


/* ---- memoisation ------------------------------------------------------
 * Every lookup below is a pure function of (residue type, bin13_1, bin13_2,
 * bin14), and consecutive residues in regular secondary structure share the
 * same bins, so caching results is exact.  Keys outside 0..255 per component
 * bypass the cache (never happens with the shipped tables: bins are <= 73). */
#define FT_HASH_SIZE 4096 /* power of two */
typedef struct { unsigned key; int valid; int bestpos; } ft_memo1;
typedef struct { unsigned key; int valid; real hit[10]; int pos[10]; } ft_memo10;

static inline int ft_key(unsigned *key, int a, int b, int c, int d) {
  if ((unsigned)a > 255u || (unsigned)b > 255u || (unsigned)c > 255u || (unsigned)d > 255u) return 0;
  *key = ((unsigned)a << 24) | ((unsigned)b << 16) | ((unsigned)c << 8) | (unsigned)d;
  return 1;
}
static inline unsigned ft_hash(unsigned k) {
  k ^= k >> 16; k *= 0x7feb352du; k ^= k >> 15; k *= 0x846ca68bu; k ^= k >> 16;
  return k & (FT_HASH_SIZE - 1);
}
/* linear probing; on a full table the caller just recomputes */
static inline ft_memo1 *ft_find1(ft_memo1 *t, unsigned key) {
  unsigned h = ft_hash(key), i;
  for (i = 0; i < 8; i++) {
    ft_memo1 *e = &t[(h + i) & (FT_HASH_SIZE - 1)];
    if (!e->valid || e->key == key) return e;
  }
  return NULL;
}
static inline ft_memo10 *ft_find10(ft_memo10 *t, unsigned key) {
  unsigned h = ft_hash(key), i;
  for (i = 0; i < 8; i++) {
    ft_memo10 *e = &t[(h + i) & (FT_HASH_SIZE - 1)];
    if (!e->valid || e->key == key) return e;
  }
  return NULL;
}
static ft_memo1 nco_memo[FT_HASH_SIZE], nco_pro_memo[FT_HASH_SIZE];
static ft_memo10 rot_memo[FT_HASH_SIZE];

/* ---- rotamer type ranges -------------------------------------------- */
static int rt_ranges_ready = 0;
static int rt_first[64], rt_last[64]; /* inclusive; last < first => none */
static int rt_contiguous = 1;
static int rt_nrows = 0;

static void rot_type_ranges_build(void) {
  int j, t, prev = -1;
  for (t = 0; t < 64; t++) { rt_first[t] = 0; rt_last[t] = -1; }
  for (j = 0; rot_stat_idx[j][0] >= 0; j++) {
    t = rot_stat_idx[j][0];
    if (t >= 64) { rt_contiguous = 0; continue; }
    if (rt_last[t] < 0) {
      rt_first[t] = j;
    } else if (rt_last[t] != j - 1) {
      rt_contiguous = 0; /* type appears in more than one block */
    }
    rt_last[t] = j;
    prev = t;
  }
  (void)prev;
  rt_nrows = j;
  rt_ranges_ready = 1;
}

/* Row range to scan for a residue type (inclusive of first, exclusive of end). */
static inline void rot_type_range(int type, int *first, int *end) {
  if (!rt_ranges_ready) rot_type_ranges_build();
  if (rt_contiguous && type >= 0 && type < 64 && rt_last[type] >= 0) {
    *first = rt_first[type];
    *end = rt_last[type] + 1;
  } else {
    *first = 0;
    *end = rt_nrows;
  }
}

/* ---- backbone fragment bins ----------------------------------------- */
static int *nco_bins_flat = NULL, *nco_pro_bins_flat = NULL;
static int nco_n = 0, nco_pro_n = 0;

static int *nco_flatten(const nco_struct *tab, int *n_out) {
  int n = 0, j, *flat;
  while (tab[n].bins[0] >= 0) n++;
  flat = (int *)malloc(sizeof(int) * 3 * (n + 1));
  for (j = 0; j <= n; j++) {
    flat[3 * j] = tab[j].bins[0];
    flat[3 * j + 1] = tab[j].bins[1];
    flat[3 * j + 2] = tab[j].bins[2];
  }
  *n_out = n;
  return flat;
}

/*
 * Identical to the upstream loop
 *   j=0; besthit=1000; bestpos=0;
 *   do { hit=...; if (hit<besthit){besthit=hit;bestpos=j;} j++; }
 *   while (tab[j].bins[0] >= 0 && hit > 1e-3);
 * evaluated over the flattened bins.
 */
static inline int nco_find(const int *flat, int b13_1, int b13_2, int b14) {
  int j = 0, bestpos = 0;
  real hit, besthit = 1000.;
  do {
    hit = abs(flat[3 * j] - b13_1) + abs(flat[3 * j + 1] - b13_2) +
          0.2 * abs(flat[3 * j + 2] - b14);
    if (hit < besthit) {
      besthit = hit;
      bestpos = j;
    }
    j++;
  } while (flat[3 * j] >= 0 && hit > 1e-3);
  return bestpos;
}

static inline int nco_find_memo(ft_memo1 *memo, const int *flat, int b13_1, int b13_2, int b14) {
  unsigned key; ft_memo1 *e = NULL; int r;
  if (ft_key(&key, 0, b13_1, b13_2, b14)) {
    e = ft_find1(memo, key);
    if (e && e->valid) return e->bestpos;
  }
  r = nco_find(flat, b13_1, b13_2, b14);
  if (e) { e->key = key; e->valid = 1; e->bestpos = r; }
  return r;
}

static inline int nco_stat_find(int b13_1, int b13_2, int b14) {
  if (!nco_bins_flat) nco_bins_flat = nco_flatten(nco_stat, &nco_n);
  return nco_find_memo(nco_memo, nco_bins_flat, b13_1, b13_2, b14);
}

static inline int nco_stat_pro_find(int b13_1, int b13_2, int b14) {
  if (!nco_pro_bins_flat) nco_pro_bins_flat = nco_flatten(nco_stat_pro, &nco_pro_n);
  return nco_find_memo(nco_pro_memo, nco_pro_bins_flat, b13_1, b13_2, b14);
}

/*
 * Ten closest rotamer rows for (type, bins), exactly as the upstream
 * insertion loop in rebuild_sidechains() computes them into
 * SORTED_ROTAMERS[i][0..9][0..1]  (column 0 = hit, column 1 = row index).
 */
static void rot_sorted_compute(int type, int b13_1, int b13_2, int b14, real *hit10, int *pos10) {
  int j, k, l, rt_end;
  real hit;
  for (j = 0; j < 10; j++) { hit10[j] = 500.; pos10[j] = 0; }
  rot_type_range(type, &j, &rt_end);
  for (; j < rt_end; j++) {
    if (rot_stat_idx[j][0] == type) {
      hit = abs(rot_stat_idx[j][1] - b13_1) + abs(rot_stat_idx[j][2] - b13_2) +
            0.2 * abs(rot_stat_idx[j][3] - b14);
      if (hit < hit10[9]) {
        k = 9;
        while (k >= 0 && hit < hit10[k]) k--;
        k++;
        for (l = 9; l > k; l--) { hit10[l] = hit10[l - 1]; pos10[l] = pos10[l - 1]; }
        hit10[k] = hit;
        pos10[k] = j;
      }
    }
  }
}

static inline void rot_sorted_lookup(int type, int b13_1, int b13_2, int b14, real *hit10, int *pos10) {
  unsigned key; ft_memo10 *e = NULL; int j;
  if (ft_key(&key, type, b13_1, b13_2, b14)) {
    e = ft_find10(rot_memo, key);
    if (e && e->valid) {
      for (j = 0; j < 10; j++) { hit10[j] = e->hit[j]; pos10[j] = e->pos[j]; }
      return;
    }
  }
  rot_sorted_compute(type, b13_1, b13_2, b14, hit10, pos10);
  if (e) { e->key = key; e->valid = 1; for (j = 0; j < 10; j++) { e->hit[j] = hit10[j]; e->pos[j] = pos10[j]; } }
}

#endif /* PULCHRA_FAST_TABLES_H */
