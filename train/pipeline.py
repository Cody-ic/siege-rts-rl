"""Resumable collection -> imitation -> PPO -> per-map frozen evaluation.

Run from the repository root with rts_native on PYTHONPATH. Repeating the same
command resumes completed stages; changed sources/settings require a new folder.
No GPU is selected unless --device cuda is explicitly supplied.
"""
import argparse
from dataclasses import asdict
import json
from pathlib import Path
import subprocess
import sys

from checkpointing import atomic_json, contract, run_lock, sha256
from ppo import Cfg, R, validate


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run-dir',required=True)
    parser.add_argument('--map-pool',required=True,help='Comma-separated training maps')
    parser.add_argument('--eval-maps',required=True,help='Comma-separated evaluation maps')
    parser.add_argument('--levels',default='1,4,8,16')
    parser.add_argument('--stats-path',default=Cfg.stats_path)
    parser.add_argument('--device',default='cpu')
    parser.add_argument('--train-profiles',default='',
                        help='Comma-separated scripted defender styles rotated during PPO (empty keeps the single original script)')
    parser.add_argument('--eval-profiles',default='',
                        help='Comma-separated styles to evaluate against, one report per style (empty evaluates the original script only)')
    for name,default in [('envs',16),('threads',2),('torch-threads',2),
                         ('prepare-ticks',900),('collect-steps',600),('fit-epochs',16),
                         ('total-steps',65536),('eval-episodes',32),('seed',1)]:
        parser.add_argument('--'+name,type=int,default=default)
    args=parser.parse_args()
    if min(args.collect_steps,args.fit_epochs,args.eval_episodes)<1:
        parser.error('Collection, fitting and evaluation counts must be positive')
    maps=tuple(args.map_pool.split(','));evaluations=tuple(args.eval_maps.split(','))
    if len(set(evaluations))!=len(evaluations) or any(not Path(p).is_file() for p in (*maps,*evaluations)):
        parser.error('Map paths must exist; evaluation maps must be unique')
    train_profiles=tuple(p for p in args.train_profiles.split(',') if p)
    eval_profiles=tuple(p for p in args.eval_profiles.split(',') if p)
    if len(set(eval_profiles))!=len(eval_profiles):
        parser.error('Evaluation styles must be unique')
    cfg=Cfg(map_path=maps[0],map_pool=maps,roster='mixed',
            levels=tuple(int(n) for n in args.levels.split(',')),stats_path=args.stats_path,
            device=args.device,envs=args.envs,threads=args.threads,torch_threads=args.torch_threads,
            defender_prepare_ticks=args.prepare_ticks,total_steps=args.total_steps,seed=args.seed,
            defender_profiles=train_profiles,
            curriculum=(1.,),value_features='detached',reference_coef=1.)
    validate(cfg)
    folder=Path(args.run_dir)
    plan=json.loads(json.dumps(dict(config=asdict(cfg),contract=contract(R,cfg),
        evaluation_maps={p:sha256(p) for p in evaluations},evaluation_profiles=list(eval_profiles),
        collect_steps=args.collect_steps,fit_epochs=args.fit_epochs,eval_episodes=args.eval_episodes,
        source={name:sha256(Path(__file__).parent/name) for name in
                ('pipeline.py','demonstrations.py','evaluate.py','diagnostics.py')})))

    def run(stage,command):
        print(stage,flush=True)
        with (folder/(stage+'.log')).open('a',encoding='utf-8') as log:
            subprocess.run([sys.executable,*command],check=True,stdout=log,stderr=subprocess.STDOUT)

    with run_lock(folder):
        manifest=folder/'pipeline.json'
        if manifest.exists() and json.loads(manifest.read_text())!=plan:
            raise ValueError('Pipeline inputs changed; use the archived source or a new run directory')
        atomic_json(manifest,plan)
        shared=['--map-pool',args.map_pool,'--map-path',maps[0],'--stats-path',args.stats_path,
                '--roster','mixed','--levels',args.levels,'--defender-prepare-ticks',str(args.prepare_ticks)]
        run('collect',['train/demonstrations.py','collect','--out-dir',str(folder/'data'),
            '--envs',str(args.envs),'--threads',str(args.threads),'--seed',str(args.seed),
            '--steps',str(args.collect_steps),'--fractions','1','--teacher','flow_breach',*shared])
        fit=['train/demonstrations.py','fit','--data',str(folder/'data/demonstrations.npz'),
             '--run-dir',str(folder/'fit'),'--epochs',str(args.fit_epochs),
             '--torch-threads',str(args.torch_threads),'--seed',str(args.seed)]
        if any((folder/'fit'/name).exists() for name in ('latest.pt','previous.pt')):fit+=['--resume']
        run('fit',fit)
        training=['train/ppo.py','--run-dir',str(folder/'ppo'),'--device',args.device,
                  '--envs',str(args.envs),'--threads',str(args.threads),'--torch-threads',str(args.torch_threads),
                  '--seed',str(args.seed),'--total-steps',str(args.total_steps),*shared,
                  '--curriculum','1','--value-features','detached','--reference-coef','1']
        # Demonstrations are collected against the original script on purpose:
        # both arms of a style A/B then start from identical imitation weights and
        # differ only in what PPO faced.
        if train_profiles:training+=['--defender-profiles',','.join(train_profiles)]
        if any((folder/'ppo'/name).exists() for name in ('latest.pt','previous.pt')):
            training+=['--resume','auto']
        else:training+=['--init-weights',str(folder/'fit/policy.pt')]
        run('ppo',training)
        checkpoint=folder/'ppo/policy.pt';model_sha=sha256(checkpoint)
        summary=[]
        for index,path in enumerate(evaluations):
            for level in cfg.levels:
                for mode in ('frozen_argmax','frozen_sample'):
                    # No eval styles: one report against the original script, as before.
                    # With styles: one report per style so the weakest style is visible
                    # instead of being averaged away (same reason maps are not pooled).
                    for profile in (eval_profiles or (None,)):
                        stage=f'eval-{index}-level{level}-{mode}'+(f'-{profile}' if profile else '')
                        target=folder/(stage+'.json')
                        if target.exists():
                            report=json.loads(target.read_text())
                            if report['sha256']['checkpoint']!=model_sha:
                                raise ValueError('Cached evaluation belongs to different weights')
                        else:
                            command=['train/evaluate.py','--checkpoint',str(checkpoint),
                                '--map-path',path,'--stats-path',args.stats_path,'--roster','mixed',
                                '--levels',str(level),'--defender-prepare-ticks',str(args.prepare_ticks),
                                '--episodes',str(args.eval_episodes),'--envs',str(args.envs),
                                '--threads',str(args.threads),'--torch-threads',str(args.torch_threads),
                                '--seed',str(100001+index*100000),'--policy',mode,'--output',str(target),
                                '--defender-profiles',profile or '']
                            run(stage,command)
                            report=json.loads(target.read_text())
                        summary.append(dict(map=path,training_map=path in maps,level=level,mode=mode,
                            defender_profile=profile,
                            wins=report['wins'],episodes=report['episodes'],hit_rate=report['hit_rate'],
                            mean_building_value=report['mean_building_value'],report=str(target)))
                        atomic_json(folder/'summary.json',dict(model_sha256=model_sha,results=summary))
        print('Pipeline complete. Inspect per-map results before exporting or deploying.',flush=True)


if __name__=='__main__':
    main()
