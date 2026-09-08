"""Generate deterministic cross-build evidence without modifying source weights.

Run once with the original binding, once with the target binding. Equality only
supports the recorded states, not universal equivalence or training performance.
"""
import argparse
import hashlib
from pathlib import Path

import numpy as np
import torch
import rts_native as native
from checkpointing import atomic_json,sha256
from macro_evaluate import load_policy


def digest(array):
    array=np.ascontiguousarray(array)
    return dict(shape=list(array.shape),dtype=str(array.dtype),
                sha256=hashlib.sha256(array.tobytes()).hexdigest())


def probe(checkpoint,stats,maps,seeds,output,source_simulation=None):
    if Path(output).exists():
        raise ValueError('Use a new probe output')
    torch.set_num_threads(1)
    model,_,_=load_policy(checkpoint,stats,source_simulation)
    report=dict(simulation=native.SIMULATION_FINGERPRINT,checkpoint_sha256=sha256(checkpoint),
                stats_sha256=sha256(stats),probe_sha256=sha256(__file__),
                maps={p:sha256(p) for p in maps},seeds=seeds,cases=[])
    for path in maps:
        for seed in seeds:
            world=native.TrainingCampaign(path,stats,seed)
            for index in range(9):
                obs=world.defender_observation()
                detail=world.defender_detail().astype(np.float16)
                candidates=world.candidates()
                with torch.no_grad():
                    d=model.decide(*obs,candidates,world.map_shape,greedy=True,detail=detail)
                report['cases'].append(dict(map=path,seed=seed,tick=world.tick,
                    state_hash=world.diagnostic_state_hash,cells=digest(obs[0]),globals=digest(obs[1]),
                    detail=digest(detail),candidates=digest(candidates),command=list(d.command),
                    log_prob=float(d.log_prob),value=float(d.value)))
                world.advance_scripted(250)
    if sha256(checkpoint)!=report['checkpoint_sha256']:
        raise ValueError('Checkpoint changed during probe')
    atomic_json(output,report)
    print(f'{len(report["cases"])} states recorded: {output}')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--checkpoint',required=True);parser.add_argument('--stats',required=True)
    parser.add_argument('--maps',nargs='+',required=True)
    parser.add_argument('--seeds',nargs='+',type=int,default=[31,32])
    parser.add_argument('--output',required=True);parser.add_argument('--source-simulation')
    args=parser.parse_args()
    probe(args.checkpoint,args.stats,args.maps,args.seeds,args.output,args.source_simulation)
