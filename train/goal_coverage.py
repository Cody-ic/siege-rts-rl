"""Measure actual goal exposure in full-length prepared-city episodes, without training."""
import argparse
from dataclasses import asdict
import time
import numpy as np
import torch
import rts_native as R
from checkpointing import atomic_json, contract, load_weights, sha256
from ppo import Cfg, Observer, Policy, make_env, policy_forward, episode_map


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--checkpoint',required=True)
    ap.add_argument('--output',required=True)
    ap.add_argument('--maps',default='01001000,01004000,01005000,01006001')
    ap.add_argument('--seed',type=int,default=1)
    ap.add_argument('--levels',default='4,8')
    args=ap.parse_args()
    torch.set_num_threads(1)
    maps=tuple(f'game/data/maps/pool/gen_{i}.json' for i in args.maps.split(','))
    levels=tuple(int(x) for x in args.levels.split(','))
    cfg=Cfg(envs=8,threads=4,torch_threads=1,device='cpu',map_pool=maps,
            seed=args.seed,roster='mixed',levels=levels,curriculum=(1.,),tactical_goals='known-economy',
            defender_prepare_ticks=900,max_ticks=2400)
    net=Policy(R.obs.K,R.obs.CHANNEL_COUNT,R.obs.SELF_COUNT,R.obs.GLOBAL_COUNT,R.obs.ACTION_COUNT)
    net.load_state_dict(load_weights(args.checkpoint));net.eval()
    started=time.perf_counter()
    env=make_env(cfg,cfg.envs,1.)
    obs=Observer(env)
    done=np.zeros(cfg.envs,np.uint8);finished=np.zeros(cfg.envs,bool)
    actions=np.zeros((cfg.envs,obs.mu),np.uint8)
    tally=np.zeros((cfg.envs,R.obs.TALLY_FIELDS),np.float32)
    totals=np.zeros_like(tally,dtype=np.float64)
    rows=[dict(environment=i,observations=0,economy_observations=0,first_goal_tick=None,
               peak_known_targets=0,assigned_squad_observations=0,live_squad_observations=0)
          for i in range(cfg.envs)]
    for i,row in enumerate(rows):
        path=episode_map(cfg,i); spawns=R.map_sites(path)['spawns']; episode_round=i//len(maps)
        row.update(map=path,world_seed=cfg.seed*1000+i,
                   level=levels[(episode_round//len(spawns))%len(levels)],
                   spawn_index=(cfg.seed+episode_round)%len(spawns))
    with torch.inference_mode():
        for step in range(cfg.max_ticks//cfg.ticks_per_step):
            observation=obs.read()
            for i,(known,assigned) in enumerate(env.goal_diagnostics):
                if finished[i]:continue
                row=rows[i];row['observations']+=1
                row['live_squad_observations']+=int(env.unit_counts[i])
                row['assigned_squad_observations']+=assigned
                row['peak_known_targets']=max(row['peak_known_targets'],known)
                if assigned:
                    row['economy_observations']+=1
                    if row['first_goal_tick'] is None:row['first_goal_tick']=step*cfg.ticks_per_step
            dist,_=policy_forward(net,observation,'cpu')
            actions.fill(0)
            if dist is not None:actions.reshape(-1)[observation[0]]=dist.logits.argmax(-1).numpy().astype(np.uint8)
            env.step(actions,done);env.take_tally(tally)
            totals[~finished]+=tally[~finished]
            for i in np.flatnonzero((done!=0)&~finished):
                rows[i]['end']=str(env.episode_ends[i]);rows[i]['ticks']=(step+1)*cfg.ticks_per_step
            finished|=done!=0
            if finished.all():break
    for row,total in zip(rows,totals):row['tally']=dict(zip(R.obs.TALLY_NAMES,total.tolist()))
    report=dict(config=asdict(cfg),contract=contract(R,cfg),checkpoint_sha256=sha256(args.checkpoint),
                policy='frozen_argmax',rows=rows,complete=bool(finished.all()),seconds=time.perf_counter()-started,
                scope='Development goal coverage, not independent policy improvement evaluation')
    atomic_json(args.output,report)
    print({'complete':report['complete'],'exposed_episodes':sum(r['economy_observations']>0 for r in rows),
           'episodes':len(rows),'seconds':report['seconds']},flush=True)


if __name__=='__main__':main()
