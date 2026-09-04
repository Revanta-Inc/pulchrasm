
typedef struct {
  int bins[3];
  float data[8][3];
} nco_struct;

/* The statistical tables.  They are shipped packed (int16 milli-Angstrom,
 * int8 indices; see tools/pack_tables.c) and decoded into these arrays at
 * start-up by pulchra_data.c, bit-identical to the original float tables. */
extern nco_struct *nco_stat;
extern nco_struct *nco_stat_pro;
extern float (*rot_stat_coords)[3];
extern int (*rot_stat_idx)[6];
