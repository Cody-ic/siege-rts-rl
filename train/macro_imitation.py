"""Resumable script imitation for defender macro initialization; CPU only.

Teacher emits one legal command per step, without script-only unit orders.
Teacher internals retain the existing script's information; student inputs are
strictly the fog-filtered native observation. This is offline privileged teaching.
"""
import argparse
from dataclasses import asdict
from pathlib import Path

import numpy as np
import torch
import rts_native as native

from checkpointing import atomic_write,load_auto,restore_rng,rng_state,run_lock,save_run,sha256
from macro_train import Config,campaign,contract
from macro_policy import MacroPolicy


def sample_epoch(rows,active_fraction):
    """Resample waiting/active strata; weighting one-sample Adam losses is insufficient."""
    if active_fraction is None:
        return torch.randperm(len(rows)).tolist()
    if not 0<active_fraction<1:
        raise ValueError('active sample fraction must be between zero and one')
    active=torch.tensor([row['command'][0]!=native.COMMAND_KIND_NAMES.index('None') for row in rows])
    count=int(active.sum())
    if count in (0,len(rows)):
        return torch.randperm(len(rows)).tolist()
    weights=torch.where(active,active_fraction/count,(1-active_fraction)/(len(rows)-count))
    return torch.multinomial(weights,len(rows),replacement=True).tolist()


def run(cfg,folder,decisions,epochs,active_fraction=None):
    folder=Path(folder)
    if decisions<1 or epochs<1:
        raise ValueError('positive collection and epoch budgets required')
    torch.set_num_threads(1);torch.manual_seed(cfg.seed)
    identity=dict(config=asdict(cfg),contract=contract(cfg),decisions_per_map=decisions,
                  imitation_source=sha256(__file__),active_fraction=active_fraction)
    with run_lock(folder):
        existing,_=load_auto(folder)
        if existing and (existing['progress']['updates']>0 or
                         not isinstance(existing['initialization'],dict) or
                         existing['initialization'].get('kind')!='single-command-script-imitation'):
            raise ValueError('Refusing to overwrite a PPO or unrelated run with imitation')
        policy=MacroPolicy(len(native.macro_obs.CELL_NAMES),len(native.macro_obs.GLOBAL_NAMES),
                           len(native.COMMAND_KIND_NAMES),cfg.hidden,len(native.macro_obs.DETAIL_NAMES))
        data_path=folder/'demonstrations.pt'
        if data_path.exists():
            data=torch.load(data_path,weights_only=True)
            if data['identity']!=identity:
                raise ValueError('Demonstration inputs changed')
        else:
            rows=[];summaries=[]
            for index,path in enumerate(cfg.maps):
                world=native.TrainingCampaign(path,cfg.stats,cfg.seed+index)
                resets=0
                for _ in range(decisions):
                    obs=world.defender_observation();command=world.teacher_command()
                    detail=world.defender_detail().astype(np.float16)
                    with torch.no_grad():
                        d=policy.decide(*obs,world.candidates(),world.map_shape,command=command,detail=detail)
                    rows.append(dict(cells=torch.from_numpy(obs[0]),global_values=torch.from_numpy(obs[1]),
                        command=command,detail=torch.from_numpy(detail),domains=[torch.from_numpy(x) for x in d.domains],shape=world.map_shape))
                    result=world.advance(cfg.period,[command])
                    if result['defeated']:
                        resets+=1
                        world=native.TrainingCampaign(path,cfg.stats,cfg.seed+index+resets*len(cfg.maps))
                summaries.append(dict(map=path,decisions=decisions,resets=resets,last_wave=world.wave))
                print(summaries[-1],flush=True)
            data=dict(identity=identity,rows=rows,summaries=summaries)
            atomic_write(data_path,lambda stream:torch.save(data,stream))
        optimizer=torch.optim.Adam(policy.parameters(),lr=cfg.learning_rate)
        fit_path=folder/'fit.pt';completed=0;history=[]
        if fit_path.exists():
            fit=torch.load(fit_path,weights_only=True)
            if fit['identity']!=identity or fit['data_sha256']!=sha256(data_path):
                raise ValueError('Imitation checkpoint inputs changed')
            policy.load_state_dict(fit['model']);optimizer.load_state_dict(fit['optimizer'])
            completed=fit['epoch'];history=fit['history'];restore_rng(fit['rng'])
        rows=data['rows']
        while completed<epochs:
            total=0.
            for index in sample_epoch(rows,active_fraction):
                row=rows[index]
                domains=[x.numpy() for x in row['domains']]
                decision=policy.rescore(row['cells'],row['global_values'],row['command'],domains,row['shape'],detail=row['detail'])
                loss=-decision.log_prob
                optimizer.zero_grad();loss.backward()
                torch.nn.utils.clip_grad_norm_(policy.parameters(),1.,error_if_nonfinite=True)
                optimizer.step();total+=float(loss.detach())
            completed+=1
            history.append(dict(epoch=completed,mean_nll=total/len(rows)))
            atomic_write(fit_path,lambda stream:torch.save(dict(identity=identity,data_sha256=sha256(data_path),
                model=policy.state_dict(),optimizer=optimizer.state_dict(),rng=rng_state(),epoch=completed,
                history=history),stream))
            print(history[-1],flush=True)
        # A fresh PPO optimizer is intentional: imitation Adam moments are not PPO moments.
        fresh_optimizer=torch.optim.Adam(policy.parameters(),lr=cfg.learning_rate)
        world=campaign(cfg,0)
        save_run(folder,dict(format=1,config=asdict(cfg),contract=contract(cfg),model=policy.state_dict(),
            optimizer=fresh_optimizer.state_dict(),rng=rng_state(),status='ready',
            initialization=dict(kind='single-command-script-imitation',epochs=completed,
                data_sha256=sha256(data_path),source_sha256=identity['imitation_source'],history=history),
            progress=dict(updates=0,env_steps=0,episode=0,waves_survived=0,defeats=0,history=[]),
            campaign=dict(commands=[],hash=world.diagnostic_state_hash,tick=world.tick,wave=world.wave)))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--maps',nargs='+',required=True)
    parser.add_argument('--stats',default=str(Path(__file__).resolve().parents[1]/'game/data/stats_placeholder.json'))
    parser.add_argument('--run-dir',required=True)
    parser.add_argument('--decisions',type=int,default=200)
    parser.add_argument('--epochs',type=int,default=8)
    parser.add_argument('--active-fraction',type=float,help='Optional active/waiting resampling, e.g. 0.5')
    args=parser.parse_args()
    run(Config(tuple(str(Path(p).resolve()) for p in args.maps),str(Path(args.stats).resolve()),period=20),
        args.run_dir,args.decisions,args.epochs,args.active_fraction)
