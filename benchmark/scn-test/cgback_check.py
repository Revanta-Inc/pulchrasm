"""
Reproduce the "[INFO]" log lines that calculate_metrics_v1.py parses for the
chirality and ring scores, using cgback 1.0.0's own check functions
(penetration.py / hydrogen.py), i.e. exactly what
  cgback -v --skip-sampling --skip-add-hydrogen --fix-structure-max-iterations 0 X.pdb
reports at "Iteration: 0".  Only the torch-dependent model loading is skipped;
it does not influence these checks (fix_structure_max_iterations 0 => no fixing).
"""
import sys, pathlib
import os; sys.path.insert(0, os.environ.get('CGBACK_SRC', '/tmp/cgsrc'))
from cgback.parser import system_from_pdb_path
from cgback.hydrogen import calculate_num_chiral_centers, find_incorrect_chiral_centers
from cgback.penetration import find_residues_with_rings, build_ring_neighbor_list, initialize_ring_descriptors, find_penetrations
from cgback.clash import build_clash_neighbor_list, find_clashes

def check(pdb: pathlib.Path) -> list[str]:
    system = system_from_pdb_path(pdb)
    ring_idx = find_residues_with_rings(system)
    n_chiral = calculate_num_chiral_centers(system)
    clash_nl = build_clash_neighbor_list(system)
    ring_nl = build_ring_neighbor_list(system, ring_idx)
    descr = initialize_ring_descriptors(system, ring_idx)
    pens = find_penetrations(system, descr, ring_nl)
    _, n_bad_chiral = find_incorrect_chiral_centers(system)
    clashes = find_clashes(system, clash_nl)
    n_bad_rings = len(set(descr[p[0]].residue_index for p in pens))
    n_bad_clash = len(set(i for pair in clashes for i in pair))
    n_res = len(system); n_ring = len(ring_idx)
    ring_score = n_bad_rings / n_ring * 100.0 if n_ring else 0.0
    clash_score = n_bad_clash / n_res * 100.0
    chir_score = n_bad_chiral / n_chiral * 100.0 if n_chiral else 0.0
    return [
        f"[INFO] Input: '{pdb.name}'",
        f"[INFO] Number of residues with rings: {n_ring}",
        f"[INFO] Number of chiral centers: {n_chiral}",
        # column layout of the cgback build used for the paper (no clash column);
        # calculate_metrics_v1.py indexes the fields by '|' position.
        f"[INFO] Iteration: {0:6} | Ring score: {ring_score:6.2f}% | Chirality score: {chir_score:6.2f}% | Bad rings: {n_bad_rings:7} | Bad centers: {n_bad_chiral:7}",
        f"[INFO] (cgback 1.0.0 clash check, informational) Clash score: {clash_score:6.2f}% | Bad residues: {n_bad_clash:7}",
    ]

if __name__ == '__main__':
    for p in sys.argv[1:]:
        p = pathlib.Path(p)
        lines = check(p)
        p.with_suffix('.log').write_text('\n'.join(lines) + '\n')
        print(lines[-1])
