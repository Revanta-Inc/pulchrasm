/* Checks that the tables decoded by pulchra_data.c from packed_tables.h are
 * bit-identical to the original headers.  Exit status 0 = identical.
 *   cc -O1 -I.. -o verify_tables verify_tables.c ../pulchra_data.c */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
typedef struct { int bins[3]; float data[8][3]; } nco_struct;
#define nco_stat orig_nco_stat
#define nco_stat_pro orig_nco_stat_pro
#define rot_stat_coords orig_rot_stat_coords
#define rot_stat_idx orig_rot_stat_idx
#include "../nco_data.h"
#include "../rot_data_coords.h"
#include "../rot_data_idx.h"
#undef nco_stat
#undef nco_stat_pro
#undef rot_stat_coords
#undef rot_stat_idx
#define nco_struct nco_struct_decoded_dummy /* pulchra_common.h defines the same struct */
#include "../pulchra_common.h"
#undef nco_struct

int main(void) {
  size_t n_nco = sizeof(orig_nco_stat) / sizeof(orig_nco_stat[0]);
  size_t n_pro = sizeof(orig_nco_stat_pro) / sizeof(orig_nco_stat_pro[0]);
  size_t n_coord = sizeof(orig_rot_stat_coords) / sizeof(orig_rot_stat_coords[0]);
  size_t n_idx = sizeof(orig_rot_stat_idx) / sizeof(orig_rot_stat_idx[0]);
  int bad = 0;
  bad += memcmp(nco_stat, orig_nco_stat, n_nco * sizeof(nco_struct)) != 0;
  bad += memcmp(nco_stat_pro, orig_nco_stat_pro, n_pro * sizeof(nco_struct)) != 0;
  bad += memcmp(rot_stat_coords, orig_rot_stat_coords, n_coord * 3 * sizeof(float)) != 0;
  bad += memcmp(rot_stat_idx, orig_rot_stat_idx, n_idx * 6 * sizeof(int)) != 0;
  if (bad) {
    size_t i; int k;
    for (i = 0; i < n_coord; i++) for (k = 0; k < 3; k++)
      if (memcmp(&rot_stat_coords[i][k], &orig_rot_stat_coords[i][k], 4)) { printf("coord[%zu][%d]: %.9g vs %.9g\n", i, k, rot_stat_coords[i][k], orig_rot_stat_coords[i][k]); if (--bad < -20) break; }
    printf("TABLES DIFFER\n");
    return 1;
  }
  printf("decoded tables bit-identical to originals: nco %zu, nco_pro %zu, coords %zu, idx %zu rows\n", n_nco, n_pro, n_coord, n_idx);
  return 0;
}
