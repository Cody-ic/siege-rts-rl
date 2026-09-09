"""Compare the defender-style A/B (control vs profiles) against its frozen plan.

Reads each arm's pipeline output, checks that every report was produced by the
planned evaluator and the arm's own final weights, then applies the plan's gate
per evaluation style and per seed pair. Every per-case regression is kept.
"""
import argparse
import hashlib
import json
from pathlib import Path


def require(condition, message):
    if not condition: raise ValueError(message)


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def load_arm(root, arm, seed, plan):
    folder=root/f'{arm}-s{seed}'
    manifest=json.loads((folder/'pipeline.json').read_text(encoding='utf-8'))
    summary=json.loads((folder/'summary.json').read_text(encoding='utf-8'))
    model=sha256(folder/'ppo/policy.pt')
    require(summary['model_sha256']==model,f'{arm}-s{seed}: summary belongs to other weights')
    cfg=manifest['config']
    require(tuple(cfg['defender_profiles'])==tuple(plan['arms'][arm]['train_profiles']),
            f'{arm}-s{seed}: trained against unplanned styles')
    require(cfg['seed']==seed,f'{arm}-s{seed}: wrong training seed')
    p=plan['pipeline']
    require(list(cfg['map_pool'])==p['map_pool'] and list(cfg['levels'])==p['levels']
            and cfg['total_steps']==p['total_steps'] and cfg['defender_prepare_ticks']==p['prepare_ticks'],
            f'{arm}-s{seed}: pipeline inputs differ from plan')
    require(manifest['evaluation_profiles']==p['eval_profiles'],f'{arm}-s{seed}: evaluation styles differ from plan')
    for name in ('pipeline.py','evaluate.py','demonstrations.py','diagnostics.py'):
        require(manifest['source'][name]==plan['files'][f'train/{name}'],f'{arm}-s{seed}: {name} differs from planned source')
    results={}
    for entry in summary['results']:
        # Reports live in the arm folder by construction; resolve by name so a run
        # tree copied off the server still compares.
        report_path=folder/Path(entry['report']).name
        require(report_path.is_file(),f'{arm}-s{seed}: missing report {report_path.name}')
        report=json.loads(report_path.read_text(encoding='utf-8'))
        style=entry['defender_profile']
        require(style in p['eval_profiles'],f'{arm}-s{seed}: unplanned style {style}')
        require(report['sha256']['checkpoint']==model,f'{arm}-s{seed}: report from other weights')
        require(report['sha256']['map']==plan['files'][entry['map']],f'{arm}-s{seed}: map changed')
        require(report['policy']==entry['mode'] and report['episodes']==p['eval_episodes'],'Wrong mode or episode count')
        require(report['defender_profiles']==[style] and all(r['defender_profile']==style for r in report['rows']),
                f'{arm}-s{seed}: report did not face only {style}')
        require(report['defender_prepare_ticks']==p['prepare_ticks'] and report['evaluator_sha256']['evaluate.py']==plan['files']['train/evaluate.py'],
                f'{arm}-s{seed}: evaluator or preparation differs from plan')
        key=(entry['map'],entry['level'],entry['mode'],style)
        require(key not in results,f'{arm}-s{seed}: duplicate case {key}')
        results[key]=dict(wins=report['wins'],episodes=report['episodes'],hit_rate=report['hit_rate'],
                          value=report['mean_building_value'])
    expected={(m,l,mode,s) for m in p['eval_maps'] for l in p['levels'] for mode in p['eval_modes'] for s in p['eval_profiles']}
    require(set(results)==expected,f'{arm}-s{seed}: missing cases {sorted(expected-set(results))[:3]}')
    return dict(model_sha256=model,config=cfg,results=results)


def compare(plan_path, root):
    plan=json.loads(plan_path.read_text(encoding='utf-8'))
    for path,digest in plan['files'].items():
        require(sha256(path)==digest,f'Changed input: {path}')
    p=plan['pipeline'];arms=list(plan['arms']);seeds=plan['training_seeds']
    require(arms==['control','profiles'],'Plan arms must be control and profiles')
    loaded={(arm,seed):load_arm(root,arm,seed,plan) for arm in arms for seed in seeds}
    for seed in seeds:
        a=dict(loaded[('control',seed)]['config']);b=dict(loaded[('profiles',seed)]['config'])
        a.pop('defender_profiles');b.pop('defender_profiles')
        require(a==b,f'seed {seed}: arms differ in something other than defender styles')
    out=dict(plan_sha256=sha256(plan_path),models={f'{arm}-s{seed}':v['model_sha256'] for (arm,seed),v in loaded.items()},
             by_style={},regressions=[],gate={})
    passed=True;weakest=None
    for mode in p['eval_modes']:
        for style in p['eval_profiles']:
            per_seed={}
            for seed in seeds:
                c=loaded[('control',seed)]['results'];q=loaded[('profiles',seed)]['results']
                keys=[(m,l,mode,style) for m in p['eval_maps'] for l in p['levels']]
                cw=sum(c[k]['wins'] for k in keys);qw=sum(q[k]['wins'] for k in keys)
                n=sum(c[k]['episodes'] for k in keys)
                cv=sum(c[k]['value'] for k in keys)/len(keys);qv=sum(q[k]['value'] for k in keys)/len(keys)
                ch=sum(c[k]['hit_rate'] for k in keys)/len(keys);qh=sum(q[k]['hit_rate'] for k in keys)/len(keys)
                per_seed[str(seed)]=dict(control=dict(wins=cw,episodes=n,mean_building_value=cv,hit_rate=ch),
                                         profiles=dict(wins=qw,episodes=n,mean_building_value=qv,hit_rate=qh),
                                         win_delta=qw-cw,value_delta=qv-cv,
                                         passes=(qw>=cw and qv>=cv))
                for k in keys:
                    if q[k]['wins']<c[k]['wins'] or q[k]['value']<c[k]['value']:
                        out['regressions'].append(dict(seed=seed,map=k[0],level=k[1],mode=mode,style=style,
                            control=c[k],profiles=q[k]))
            out['by_style'][f'{mode}/{style}']=per_seed
            if mode==p['eval_modes'][0]:   # primary gate: first listed mode (frozen_argmax)
                style_pass=all(v['passes'] for v in per_seed.values())
                out['gate'][style]=style_pass
                passed=passed and style_pass
                delta=min(v['win_delta'] for v in per_seed.values())
                if weakest is None or delta<weakest[1]: weakest=(style,delta)
    out['gate_primary_mode']=p['eval_modes'][0]
    out['passed']=passed
    out['weakest_style']=dict(style=weakest[0],min_win_delta=weakest[1]) if weakest else None
    out['scope']=('Held-out maps, frozen final policy.pt per arm, one report per style. '
                  'A pass here is a development signal at 65,536 PPO steps, not deployment evidence; '
                  'per-case regressions are listed regardless of the overall verdict.')
    return out


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--plan',required=True,type=Path)
    ap.add_argument('--root',required=True,type=Path,help='Directory holding control-s*/ and profiles-s*/')
    ap.add_argument('--output',required=True,type=Path)
    args=ap.parse_args()
    result=compare(args.plan,args.root)
    args.output.write_text(json.dumps(result,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
    print('PASS' if result['passed'] else 'FAIL',
          f"weakest style {result['weakest_style']}",f"{len(result['regressions'])} per-case regressions")
    for key,per_seed in result['by_style'].items():
        for seed,v in per_seed.items():
            print(f"  {key:26} seed {seed}: wins {v['control']['wins']}->{v['profiles']['wins']}/{v['control']['episodes']}"
                  f"  value {v['control']['mean_building_value']:.0f}->{v['profiles']['mean_building_value']:.0f}"
                  f"  {'ok' if v['passes'] else 'regress'}")


if __name__=='__main__':
    main()
