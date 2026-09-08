"""Frozen full-campaign defender comparison. No updates or training-directory writes."""
import argparse
from collections import Counter
from pathlib import Path

import torch
import numpy as np
import rts_native as native

from checkpointing import atomic_json, load_training, sha256
from macro_policy import MacroPolicy


def load_policy(path, stats):
    data=load_training(path)
    identity=data['contract']
    expected=dict(kind='defender-macro-ppo-v1',simulation=native.SIMULATION_FINGERPRINT,
        build=native.BUILD_MODE,obs_version=native.macro_obs.VERSION,
        cells=list(native.macro_obs.CELL_NAMES),globals=list(native.macro_obs.GLOBAL_NAMES),
        detail=list(native.macro_obs.DETAIL_NAMES),detail_storage='float16-before-inference',
        commands=list(native.COMMAND_KIND_NAMES),stats=sha256(stats))
    for key,value in expected.items():
        if identity.get(key)!=value:
            raise ValueError(f'Frozen macro policy contract differs: {key}')
    if identity['sources']['macro_policy.py']!=sha256(Path(__file__).with_name('macro_policy.py')):
        raise ValueError('Macro policy implementation differs from training')
    cfg=data['config']
    torch.manual_seed(cfg['seed'])
    initial=MacroPolicy(len(native.macro_obs.CELL_NAMES),len(native.macro_obs.GLOBAL_NAMES),
                        len(native.COMMAND_KIND_NAMES),cfg['hidden'],len(native.macro_obs.DETAIL_NAMES)).eval()
    learned=MacroPolicy(len(native.macro_obs.CELL_NAMES),len(native.macro_obs.GLOBAL_NAMES),
                        len(native.COMMAND_KIND_NAMES),cfg['hidden'],len(native.macro_obs.DETAIL_NAMES)).eval()
    learned.load_state_dict(data['model'])
    return learned,initial,cfg


def evaluate_case(map_path,stats,seed,policy,period,max_wave,max_ticks,greedy=False):
    if min(period,max_wave,max_ticks)<1:
        raise ValueError('positive evaluation limits required')
    world=native.TrainingCampaign(str(map_path),str(stats),seed)
    torch.manual_seed(seed)
    commands=Counter()
    defeated=False
    while world.tick<max_ticks and world.wave<=max_wave and not defeated:
        duration=min(period,max_ticks-world.tick)
        if policy is None:
            result=world.advance_scripted(duration)
        elif policy=='teacher':
            command=world.teacher_command()
            if not world.command_mask([command])[0]:
                raise ValueError('Teacher selected an illegal command')
            commands[native.COMMAND_KIND_NAMES[command[0]]]+=1
            result=world.advance(duration,[command])
        else:
            with torch.no_grad():
                decision=policy.decide(*world.defender_observation(),world.candidates(),
                                       world.map_shape,greedy=greedy,detail=world.defender_detail().astype(np.float16))
            if not world.command_mask([decision.command])[0]:
                raise ValueError('Policy selected an illegal command')
            commands[native.COMMAND_KIND_NAMES[decision.command[0]]]+=1
            result=world.advance(duration,[decision.command])
        defeated=result['defeated']
    global_values=world.defender_observation()[1]
    keep=float(global_values[list(native.macro_obs.GLOBAL_NAMES).index('keep_hp_fraction')])
    return dict(map=str(map_path),seed=seed,tick=world.tick,wave=world.wave,
        waves_survived=world.wave-1,defeated=defeated,completed=world.wave>max_wave,
        timeout=not defeated and world.wave<=max_wave,keep_hp_fraction=keep,
        commands=dict(commands),state_hash=world.diagnostic_state_hash)


def evaluate(checkpoint,maps,stats,seeds,output,max_wave=6,max_ticks=100000,greedy=False,period=None,
             arms=('script','initial','learned')):
    output=Path(output)
    if output.exists():
        raise ValueError('Evaluation output already exists; use a new file')
    if not arms or len(set(arms))!=len(arms) or not set(arms)<= {'script','initial','learned'}:
        raise ValueError('Evaluation arms must be nonempty, unique and known')
    torch.set_num_threads(1)
    learned,initial,cfg=load_policy(checkpoint,stats)
    selected_period=cfg['period'] if period is None else period
    if selected_period<1:
        raise ValueError('positive evaluation period required')
    report=dict(checkpoint_sha256=sha256(checkpoint),simulation=native.SIMULATION_FINGERPRINT,
        evaluator_sha256=sha256(__file__),stats_sha256=sha256(stats),
        maps={str(p):sha256(p) for p in maps},seeds=list(seeds),max_wave=max_wave,max_ticks=max_ticks,
        mode='conditional-greedy' if greedy else 'sample',policy_period=selected_period,
        training_period=cfg['period'],
        script_internal_period=20,training_maps=cfg['maps'],arms=list(arms),complete=False,cases=[])
    # Script is the deployed macro baseline with its normal 20-tick cadence.
    # Initial and learned networks use identical architecture and decision cadence.
    for path in maps:
        for seed in seeds:
            for name,policy in (('script',None),('initial',initial),('learned',learned)):
                if name not in arms:
                    continue
                row=evaluate_case(path,stats,seed,policy,selected_period,max_wave,max_ticks,greedy)
                row['arm']=name
                report['cases'].append(row)
                atomic_json(output,report)
                print(row,flush=True)
    if sha256(checkpoint)!=report['checkpoint_sha256']:
        raise ValueError('Checkpoint changed during evaluation; results are not a frozen comparison')
    report['complete']=True
    atomic_json(output,report)
    return report


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--checkpoint',required=True)
    parser.add_argument('--maps',nargs='+',required=True)
    parser.add_argument('--stats',default=str(Path(__file__).resolve().parents[1]/'game/data/stats_placeholder.json'))
    parser.add_argument('--seeds',nargs='+',type=int,default=[101,102])
    parser.add_argument('--output',required=True)
    parser.add_argument('--max-wave',type=int,default=6)
    parser.add_argument('--max-ticks',type=int,default=100000)
    parser.add_argument('--greedy',action='store_true')
    parser.add_argument('--period',type=int,help='Explicit cadence diagnostic; defaults to checkpoint training period')
    parser.add_argument('--arms',nargs='+',choices=['script','initial','learned'],default=['script','initial','learned'])
    args=parser.parse_args()
    evaluate(args.checkpoint,[str(Path(p).resolve()) for p in args.maps],args.stats,args.seeds,
             args.output,args.max_wave,args.max_ticks,args.greedy,args.period,args.arms)
