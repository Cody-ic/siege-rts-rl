"""Controlled value-only update probe; no claim about full PPO causality."""
import argparse
import copy
from pathlib import Path

import numpy as np
import torch
import rts_native as native

from checkpointing import atomic_json,sha256
from macro_evaluate import load_policy
from macro_train import isolate_critic


def probe(checkpoint,map_path,stats,output):
    if Path(output).exists():
        raise ValueError('Probe output already exists')
    torch.set_num_threads(1)
    initial,_,_=load_policy(checkpoint,stats)
    world=native.TrainingCampaign(str(map_path),str(stats),31)
    rows=[]
    with torch.no_grad():
        for _ in range(8):
            obs=world.defender_observation();detail=world.defender_detail().astype(np.float16)
            d=initial.decide(*obs,world.candidates(),world.map_shape,detail=detail,greedy=True)
            rows.append((obs,detail,d.command,d.domains,world.map_shape))
            world.advance_scripted(100)
    def measure(policy):
        with torch.no_grad():
            return np.array([[float(d.log_prob),float(d.value)] for obs,detail,command,domains,shape in rows
                for d in [policy.rescore(*obs,command,domains,shape,detail=detail)]])
    before=measure(initial)
    report=dict(checkpoint_sha256=sha256(checkpoint),simulation=native.SIMULATION_FINGERPRINT,
        map=str(map_path),map_sha256=sha256(map_path),states=8,updates=3,target_value=1.,
        learning_rate=.0003,interpretation='controlled value-only updates; selected command log-probability drift, not full KL',arms={})
    for name in ('shared','isolated'):
        policy=copy.deepcopy(initial)
        if name=='isolated':isolate_critic(policy)
        optimizer=torch.optim.Adam(policy.parameters(),lr=.0003)
        for _ in range(3):
            optimizer.zero_grad()
            for obs,detail,command,domains,shape in rows:
                d=policy.rescore(*obs,command,domains,shape,detail=detail)
                ((d.value-1).square()/len(rows)).backward()
            optimizer.step()
        after=measure(policy)
        report['arms'][name]=dict(max_selected_logp_change=float(np.abs(after[:,0]-before[:,0]).max()),
            max_value_change=float(np.abs(after[:,1]-before[:,1]).max()),
            before=before.tolist(),after=after.tolist())
    atomic_json(output,report)
    return report


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--checkpoint',required=True);parser.add_argument('--map',required=True)
    parser.add_argument('--stats',default=str(Path(__file__).resolve().parents[1]/'game/data/stats_placeholder.json'))
    parser.add_argument('--output',required=True);args=parser.parse_args()
    print(probe(args.checkpoint,args.map,args.stats,args.output)['arms'])
