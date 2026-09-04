/*
 * WebAssembly entry point for PULCHRA.
 *
 * pulchra.c is included (see pulchra.upstream.patch for the small local
 * changes) with its main() renamed, so that we can expose a stable exported
 * symbol `pulchra_run` that takes an argc/argv pair exactly like the
 * command-line tool.  All rotamer / backbone statistics (nco_data.h,
 * rot_data_coords.h, rot_data_idx.h) are compiled into the wasm data segment,
 * so no external data files are needed.
 *
 * The module is built as a standalone WASI reactor (no Emscripten JavaScript
 * runtime).  PULCHRA's file I/O (fopen/fgets/fprintf on the input, output,
 * initial-coordinates and trajectory files) is redirected to in-memory
 * "virtual files" below, so the host only has to provide stdout (fd_write)
 * and a clock.
 *
 * PULCHRA was written as a one-shot process and keeps its working state in C
 * globals that it never frees.  pulchra_reset() returns every one of them to
 * its pristine state, which lets a single wasm instance (with the 3.4 MB of
 * statistical tables already in memory) process any number of structures one
 * after another, with results identical to a fresh process.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <malloc.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

/* ------------------------------------------------------------------------
 * In-memory virtual files.
 *
 * The host stores the input (and optionally the initial-coordinates file)
 * with pulchra_vfs_set(), runs PULCHRA, and reads the rebuilt PDB and the
 * trajectory back with pulchra_vfs_data()/pulchra_vfs_size().  fopen() calls
 * inside pulchra.c are redirected here (see the #define after this block);
 * the returned FILE* is a regular stdio stream backed by the buffer, so
 * fgets/fprintf/feof/fclose in pulchra.c work unchanged.
 * --------------------------------------------------------------------- */
typedef struct {
  char *name;
  char *buf;
  size_t len, cap;
} vfile_t;

#define VFS_MAX 8
static vfile_t vfs[VFS_MAX];
static int vfs_n = 0;

static vfile_t *vfs_find(const char *name) {
  int i;
  for (i = 0; i < vfs_n; i++)
    if (!strcmp(vfs[i].name, name))
      return &vfs[i];
  return NULL;
}

static vfile_t *vfs_create(const char *name) {
  vfile_t *f;
  if (vfs_n == VFS_MAX)
    return NULL;
  f = &vfs[vfs_n++];
  f->name = strdup(name);
  f->buf = NULL;
  f->len = f->cap = 0;
  return f;
}

/* Remove all virtual files. */
EMSCRIPTEN_KEEPALIVE
void pulchra_vfs_clear(void) {
  int i;
  for (i = 0; i < vfs_n; i++) {
    free(vfs[i].name);
    free(vfs[i].buf);
  }
  vfs_n = 0;
}

/* Create/replace a virtual file.  `data` must come from malloc(); ownership
 * passes to the virtual file system (it is freed by pulchra_vfs_clear()).
 * Returns 0 on success, -1 if the table is full. */
EMSCRIPTEN_KEEPALIVE
int pulchra_vfs_set(const char *name, char *data, int len) {
  vfile_t *f = vfs_find(name);
  if (!f)
    f = vfs_create(name);
  if (!f)
    return -1;
  free(f->buf);
  f->buf = data;
  f->len = f->cap = (size_t)len;
  return 0;
}

/* Pointer to a virtual file's bytes (NULL if it does not exist). */
EMSCRIPTEN_KEEPALIVE
const char *pulchra_vfs_data(const char *name) {
  vfile_t *f = vfs_find(name);
  return f ? f->buf : NULL;
}

/* Number of virtual files, and the name of the i-th one (NULL if out of
 * range).  Lets the host collect every file a run produced. */
EMSCRIPTEN_KEEPALIVE
int pulchra_vfs_count(void) {
  return vfs_n;
}

EMSCRIPTEN_KEEPALIVE
const char *pulchra_vfs_name(int i) {
  return (i >= 0 && i < vfs_n) ? vfs[i].name : NULL;
}

/* Size of a virtual file in bytes (-1 if it does not exist). */
EMSCRIPTEN_KEEPALIVE
int pulchra_vfs_size(const char *name) {
  vfile_t *f = vfs_find(name);
  return f ? (int)f->len : -1;
}

typedef struct {
  vfile_t *f;
  size_t pos;
} vcookie_t;

static ssize_t vfs_do_read(vcookie_t *k, char *buf, size_t n) {
  size_t avail = k->pos < k->f->len ? k->f->len - k->pos : 0;
  if (n > avail)
    n = avail;
  memcpy(buf, k->f->buf + k->pos, n);
  k->pos += n;
  return (ssize_t)n;
}

static ssize_t vfs_do_write(vcookie_t *k, const char *buf, size_t n) {
  vfile_t *f = k->f;
  if (k->pos + n > f->cap) {
    size_t cap = f->cap ? f->cap : 4096;
    while (cap < k->pos + n)
      cap *= 2;
    f->buf = realloc(f->buf, cap);
    f->cap = cap;
  }
  memcpy(f->buf + k->pos, buf, n);
  k->pos += n;
  if (k->pos > f->len)
    f->len = k->pos;
  return (ssize_t)n;
}

static long vfs_do_seek(vcookie_t *k, long off, int whence) {
  long base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (long)k->pos : (long)k->f->len;
  if (base + off < 0)
    return -1;
  k->pos = (size_t)(base + off);
  return (long)k->pos;
}

#ifdef __APPLE__ /* native test builds: BSD funopen() instead of fopencookie() */
static int vfs_read_cb(void *c, char *b, int n) { return (int)vfs_do_read(c, b, (size_t)n); }
static int vfs_write_cb(void *c, const char *b, int n) { return (int)vfs_do_write(c, b, (size_t)n); }
static fpos_t vfs_seek_cb(void *c, fpos_t off, int whence) { return (fpos_t)vfs_do_seek(c, (long)off, whence); }
static int vfs_close_cb(void *c) { free(c); return 0; }
static FILE *vfs_open_stream(vcookie_t *k, const char *mode) {
  return funopen(k, vfs_read_cb, mode[0] == 'r' ? NULL : vfs_write_cb, vfs_seek_cb, vfs_close_cb);
}
#else
static ssize_t vfs_read_cb(void *c, char *b, size_t n) { return vfs_do_read(c, b, n); }
static ssize_t vfs_write_cb(void *c, const char *b, size_t n) { return vfs_do_write(c, b, n); }
static int vfs_seek_cb(void *c, off64_t *off, int whence) {
  long r = vfs_do_seek(c, (long)*off, whence);
  if (r < 0)
    return -1;
  *off = r;
  return 0;
}
static int vfs_close_cb(void *c) { free(c); return 0; }
static FILE *vfs_open_stream(vcookie_t *k, const char *mode) {
  cookie_io_functions_t io = {vfs_read_cb, vfs_write_cb, vfs_seek_cb, vfs_close_cb};
  return fopencookie(k, mode, io);
}
#endif

static FILE *pulchra_fopen(const char *name, const char *mode) {
  vfile_t *f = vfs_find(name);
  vcookie_t *k;
  FILE *fp;
  if (mode[0] == 'r') {
    if (!f)
      return NULL; /* -> FILE_NOT_FOUND in pulchra.c */
  } else {
    if (!f)
      f = vfs_create(name);
    if (!f)
      return NULL;
    if (mode[0] == 'w')
      f->len = 0;
  }
  k = (vcookie_t *)calloc(1, sizeof(vcookie_t));
  k->f = f;
  k->pos = mode[0] == 'a' ? f->len : 0;
  fp = vfs_open_stream(k, mode);
  if (!fp)
    free(k);
  return fp;
}

#define fopen pulchra_fopen

/*
 * glibc-compatible rand()/srand().
 *
 * PULCHRA's C-alpha optimisation is seeded with srand(1234) and draws from
 * rand().  Emscripten links against musl, whose rand() is a different
 * generator, so results would differ from the reference Linux build.  This is
 * a re-implementation of glibc's default TYPE_3 additive feedback generator
 * (random_r.c), which makes the wasm build reproduce the Linux binary
 * bit-for-bit.
 */
#define GLIBC_RAND_DEG 31
#define GLIBC_RAND_SEP 3
static int32_t glibc_rand_state[GLIBC_RAND_DEG];
static int glibc_rand_f, glibc_rand_r_idx;
static int glibc_rand_initialised = 0;

static void pulchra_glibc_srand(unsigned int seed) {
  int i;
  int32_t word;
  if (seed == 0) seed = 1;
  glibc_rand_state[0] = (int32_t)seed;
  word = (int32_t)seed;
  for (i = 1; i < GLIBC_RAND_DEG; i++) {
    /* word = (16807 * word) % 2147483647 without overflow (Schrage) */
    int32_t hi = word / 127773;
    int32_t lo = word % 127773;
    word = 16807 * lo - 2836 * hi;
    if (word < 0) word += 2147483647;
    glibc_rand_state[i] = word;
  }
  glibc_rand_f = GLIBC_RAND_SEP;
  glibc_rand_r_idx = 0;
  glibc_rand_initialised = 1;
  for (i = 0; i < 10 * GLIBC_RAND_DEG; i++) {
    /* discard, exactly as glibc does */
    glibc_rand_state[glibc_rand_f] = (int32_t)((uint32_t)glibc_rand_state[glibc_rand_f] +
                                               (uint32_t)glibc_rand_state[glibc_rand_r_idx]);
    glibc_rand_f = (glibc_rand_f + 1) % GLIBC_RAND_DEG;
    glibc_rand_r_idx = (glibc_rand_r_idx + 1) % GLIBC_RAND_DEG;
  }
}

static int pulchra_glibc_rand(void) {
  uint32_t val;
  if (!glibc_rand_initialised) pulchra_glibc_srand(1);
  val = (uint32_t)glibc_rand_state[glibc_rand_f] + (uint32_t)glibc_rand_state[glibc_rand_r_idx];
  glibc_rand_state[glibc_rand_f] = (int32_t)val;
  glibc_rand_f = (glibc_rand_f + 1) % GLIBC_RAND_DEG;
  glibc_rand_r_idx = (glibc_rand_r_idx + 1) % GLIBC_RAND_DEG;
  return (int)(val >> 1);
}

#define rand pulchra_glibc_rand
#define srand pulchra_glibc_srand

#define main pulchra_main
#include "pulchra.c"
#undef main
#undef rand
#undef srand
#undef fopen

/*
 * Return all of PULCHRA's mutable global state to the values it has in a
 * freshly started process.  Safe to call at any time, including twice in a
 * row and before the first run.
 */
EMSCRIPTEN_KEEPALIVE
void pulchra_reset(void) {
  int i, x, y, z;

  /* --- per-structure scratch data ------------------------------------- */
  if (GRID) {
    for (x = 0; x < GRID_X + 1; x++) {
      for (y = 0; y < GRID_Y + 1; y++) {
        for (z = 0; z < GRID_Z + 1; z++) {
          atom_list *l = GRID[x][y][z];
          while (l) {
            atom_list *n = l->next;
            free(l);
            l = n;
          }
        }
        free(GRID[x][y]);
      }
      free(GRID[x]);
    }
    free(GRID);
    GRID = NULL;
  }
  GRID_X = GRID_Y = GRID_Z = 0;

  /* contiguous cell slab mirrored from the grid (fast_grid.h); its atom
   * pointers dangle once the chain is deleted below.  The memo tables in
   * fast_tables.h are pure functions of the constant statistical tables and
   * are deliberately kept across runs. */
  free(fg_cells);
  free(fg_x);
  free(fg_y);
  free(fg_z);
  free(fg_atom);
  fg_cells = NULL;
  fg_x = fg_y = fg_z = NULL;
  fg_atom = NULL;
  fg_ready = 0;
  fg_xg = fg_yg = fg_zg = fg_ncells = fg_nslots = 0;

  /* RBINS / X_COORDS / SORTED_ROTAMERS are sized from chain_length, which is
   * not modified after they are allocated (ca_optimize() may shrink it, but
   * runs before prepare_rbins()). */
  if (RBINS) {
    for (i = 0; i < chain_length + 1; i++)
      free(RBINS[i]);
    free(RBINS);
    RBINS = NULL;
  }
  if (X_COORDS) {
    for (i = 0; i < chain_length + 10; i++)
      free(X_COORDS[i]);
    free(X_COORDS);
    X_COORDS = NULL;
  }
  C_ALPHA = NULL; /* points into X_COORDS */
  if (SORTED_ROTAMERS) {
    for (i = 0; i < chain_length + 1; i++) {
      int j;
      for (j = 0; j < 10; j++)
        free(SORTED_ROTAMERS[i][j]);
      free(SORTED_ROTAMERS[i]);
    }
    free(SORTED_ROTAMERS);
    SORTED_ROTAMERS = NULL;
  }

  if (chain) {
    if (chain->seq) { free(chain->seq); chain->seq = NULL; } /* not freed by delete_mol */
    if (chain->r14) { free(chain->r14); chain->r14 = NULL; }
    delete_mol(chain);
    chain = NULL;
  }
  chain_length = 0;

  /* --- option switches: upstream defaults (pulchra.c lines 59-79) ------ */
  _VERBOSE = 0;
  _BB_REARRANGE = 1;
  _BB_OPTIMIZE = 0;
  _CA_OPTIMIZE = 1;
  _CA_RANDOM = 0;
  _CA_ITER = 100;
  _CA_TRAJECTORY = 0;
  _CISPRO = 0;
  _CHIRAL = 1;
  _CENTER_CHAIN = 0;
  _REBUILD_BB = 1;
  _REBUILD_SC = 1;
  _REBUILD_H = 0;
  _PDB_SG = 0;
  _TIME_SEED = 0;
  _XVOLUME = 1;
  _XVOL_ITER = 3;
  _PRESERVE = 1;
  _CA_START_DIST = 3.0;
  _CA_XVOL_DIST = 3.5;
  _SG_XVOL_DIST = 1.6;
  CA_K = 10.0;
  CA_ANGLE_K = 20.0;
  CA_START_K = 0.01;
  CA_XVOL_K = 10.00;

  /* The random generator is re-seeded unconditionally at the top of every
   * run (srand(1234) or srand(time(NULL))), AA_NUMS is rebuilt there too, and
   * rotamers[] is never populated (read_rotamers() is dead code in the
   * standalone version), so nothing else needs resetting. */
}

/*
 * Run one reconstruction with a command line exactly like the CLI tool's.
 * The instance is left clean afterwards, so this may be called repeatedly.
 */
EMSCRIPTEN_KEEPALIVE
int pulchra_run(int argc, char **argv) {
  int rc;
  pulchra_reset(); /* defensive: also clears state left by a failed run */
  rc = pulchra_main(argc, argv);
  fflush(NULL);
  pulchra_reset(); /* release the structure's memory right away */
  return rc;
}

EMSCRIPTEN_KEEPALIVE
double pulchra_version(void) {
  return PULCHRA_VERSION;
}

/* Bytes currently handed out by malloc(); used by the test-suite to prove
 * that repeated runs do not leak. */
EMSCRIPTEN_KEEPALIVE
double pulchra_heap_used(void) {
#ifdef __EMSCRIPTEN__
  struct mallinfo mi = mallinfo();
  return (double)mi.uordblks;
#else
  return -1;
#endif
}

/*
 * Direct access to PULCHRA's global switches.  The command-line parser in
 * pulchra.c can only *set* some of these (e.g. `-f` forces _PRESERVE=1 and
 * `-h` forces _REBUILD_H=0), and never exposes the iteration counts.  This
 * setter lets the JavaScript wrapper reach every knob.  Returns 1 if the name
 * was recognised, 0 otherwise.
 */
EMSCRIPTEN_KEEPALIVE
int pulchra_set_option(const char *name, double value) {
  if (!strcmp(name, "VERBOSE"))        { _VERBOSE        = (int)value; return 1; }
  if (!strcmp(name, "BB_REARRANGE"))   { _BB_REARRANGE   = (int)value; return 1; }
  if (!strcmp(name, "BB_OPTIMIZE"))    { _BB_OPTIMIZE    = (int)value; return 1; }
  if (!strcmp(name, "CA_OPTIMIZE"))    { _CA_OPTIMIZE    = (int)value; return 1; }
  if (!strcmp(name, "CA_RANDOM"))      { _CA_RANDOM      = (int)value; return 1; }
  if (!strcmp(name, "CA_ITER"))        { _CA_ITER        = (int)value; return 1; }
  if (!strcmp(name, "CA_TRAJECTORY"))  { _CA_TRAJECTORY  = (int)value; return 1; }
  if (!strcmp(name, "CISPRO"))         { _CISPRO         = (int)value; return 1; }
  if (!strcmp(name, "CHIRAL"))         { _CHIRAL         = (int)value; return 1; }
  if (!strcmp(name, "CENTER_CHAIN"))   { _CENTER_CHAIN   = (int)value; return 1; }
  if (!strcmp(name, "REBUILD_BB"))     { _REBUILD_BB     = (int)value; return 1; }
  if (!strcmp(name, "REBUILD_SC"))     { _REBUILD_SC     = (int)value; return 1; }
  if (!strcmp(name, "REBUILD_H"))      { _REBUILD_H      = (int)value; return 1; }
  if (!strcmp(name, "PDB_SG"))         { _PDB_SG         = (int)value; return 1; }
  if (!strcmp(name, "TIME_SEED"))      { _TIME_SEED      = (int)value; return 1; }
  if (!strcmp(name, "XVOLUME"))        { _XVOLUME        = (int)value; return 1; }
  if (!strcmp(name, "XVOL_ITER"))      { _XVOL_ITER      = (int)value; return 1; }
  if (!strcmp(name, "PRESERVE"))       { _PRESERVE       = (int)value; return 1; }
  if (!strcmp(name, "CA_START_DIST"))  { _CA_START_DIST  = value;      return 1; }
  return 0;
}

/*
 * Run with a list of option overrides applied *between* the reset and argv
 * parsing.  Because the flag parser only ever assigns constants, an override
 * for which the caller did NOT also pass the corresponding flag behaves
 * exactly like "the flag was/wasn't given".
 */
EMSCRIPTEN_KEEPALIVE
int pulchra_run_with_options(int argc, char **argv, int nopts,
                             const char **names, const double *values) {
  int i, rc;
  pulchra_reset();
  for (i = 0; i < nopts; i++)
    pulchra_set_option(names[i], values[i]);
  rc = pulchra_main(argc, argv);
  fflush(NULL);
  pulchra_reset();
  return rc;
}
