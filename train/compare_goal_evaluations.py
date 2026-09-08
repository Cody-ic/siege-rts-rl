"""Verify the frozen low-level plan and compare every planned map/seed pair."""
import argparse
import hashlib
import json
from pathlib import Path


def require(condition, message):
    if not condition: raise ValueError(message)


def compare(plan_path, directory):
    plan=json.loads(plan_path.read_text(encoding='utf-8'))
    for path,digest in plan['files'].items():
        require(hashlib.sha256(Path(path).read_bytes()).hexdigest()==digest,f'Changed input: {path}')
    levels={}
    for level in plan['levels']:
        reports={name:json.loads((directory/f'lowlevel{level}-{name}.json').read_text(encoding='utf-8'))
                 for name in ('baseline','candidate')}
        a,b=reports.values()
        require(a['config']==b['config'] and a['contract']==b['contract'],'Mismatched evaluation environments')
        paired=[]
        for name,report in reports.items():
            cfg=report['config']
            require(report['complete'] and report['policy']==plan['selection'],'Incomplete or wrong policy mode')
            require(cfg['seed']==plan['seed'] and cfg['levels']==[level],'Wrong seed or level')
            require(cfg['envs']==plan['envs'] and len(report['rows'])==plan['envs'],'Missing cases')
            require(cfg['map_pool']==[f'game/data/maps/pool/gen_{x}.json' for x in plan['maps']],'Wrong maps')
            require(cfg['defender_prepare_ticks']==plan['prepare_ticks'] and cfg['max_ticks']==plan['max_ticks'],
                    'Wrong preparation or battle time')
            require(cfg['tactical_goals']=='known-economy','Wrong goal mode')
            model=('runs/prepared-final-20260909/candidate.pt' if name=='baseline' else
                   'runs/economy-goal-pilot-20260909/frozen-update16.pt')
            require(report['checkpoint_sha256']==plan['files'][model],'Wrong checkpoint')
            require([r['environment'] for r in report['rows']]==list(range(plan['envs'])),'Duplicate or reordered cases')
            for i,row in enumerate(report['rows']):
                require(row['level']==level and row['world_seed']==plan['seed']*1000+i,'Wrong case identity')
                require(row['map']==cfg['map_pool'][(plan['seed']*1000+i)%len(plan['maps'])],'Wrong case map')
                require(row['end'] in ('EpisodeEnd.KeepDestroyed','EpisodeEnd.Timeout','EpisodeEnd.AttackersEliminated'),
                        'Unknown terminal state')
                require(0<row['ticks']<=plan['max_ticks'],'Invalid duration')
        for x,y in zip(a['rows'],b['rows']):
            require(all(x[k]==y[k] for k in ('map','world_seed','level','spawn_index')),'Unpaired cases')
            win=lambda r:int(r['end']=='EpisodeEnd.KeepDestroyed')
            paired.append(dict(map=x['map'],seed=x['world_seed'],win_delta=win(y)-win(x),
                               building_value_delta=y['tally']['bld_value']-x['tally']['bld_value'],
                               losses_delta=y['tally']['losses']-x['tally']['losses']))
        summary={name:dict(wins=sum(r['end']=='EpisodeEnd.KeepDestroyed' for r in report['rows']),
                           building_value=sum(r['tally']['bld_value'] for r in report['rows']),
                           losses=sum(r['tally']['losses'] for r in report['rows']))
                 for name,report in reports.items()}
        levels[str(level)]=dict(summary=summary,pairs=paired,reports=reports)
    return dict(plan_sha256=hashlib.sha256(plan_path.read_bytes()).hexdigest(),plan_files_verified=True,levels=levels)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--plan',required=True,type=Path)
    p.add_argument('--directory',required=True,type=Path)
    p.add_argument('--output',required=True,type=Path)
    args=p.parse_args()
    result=compare(args.plan,args.directory)
    with args.output.open('x',encoding='utf-8') as f:json.dump(result,f,indent=2)
    print(json.dumps({level:r['summary'] for level,r in result['levels'].items()}))
