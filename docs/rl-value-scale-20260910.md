# Value-loss scaling pilot: rejected

Server artifacts: `/home/yys/Cody-ic/runs/value-scale-20260910` on the configured yys training server. Source commit: `300f18a`; initialization SHA-256: `f05c7c5d9452a26c9e8e1507d88a13737e3353a565cb87b7e5bb7d37e9c1fefe`.

Both arms completed 262,144 steps. Each used 25 CPU updates followed by CUDA resume; unfinished episodes reset at the switch. This was a matched interrupted run, not bitwise uninterrupted training. Baseline and final weights were evaluated with frozen argmax on three maps × two levels × 32 episodes.

| Map suffix / level | Baseline wins | Control wins | Reduced wins |
|---|---:|---:|---:|
| 01007000 / 1 | 24 | 24 | 0 |
| 01007000 / 4 | 24 | 24 | 0 |
| 01008000 / 1 | 6 | 6 | 0 |
| 01008000 / 4 | 12 | 12 | 0 |
| 01009000 / 1 | 8 | 8 | 0 |
| 01009000 / 4 | 32 | 32 | 0 |
| Total / 192 | 106 | 106 | 0 |

The reduced value coefficient (0.00005 versus 0.5) failed both gates; neither candidate was deployed. Control preserved wins but regressed building value on 01009000/L1 (302.5 → 275). Training win counters were not evidence of frozen-policy performance.

Next diagnostic: `train/anchor_pilot.py` compares reference KL coefficients 1 and 0 with the reduced value coefficient, identical original initialization and otherwise matched parameters, entirely on CUDA. Each arm runs 262,144 steps with final-checkpoint-only evaluation. The previous evaluation cases are now a reused diagnostic panel, not an untouched holdout. The controller records source/input hashes and reports per-case results; it never deploys weights. Independent seeds, untouched cases and current-game multiwave validation remain required before adoption.
