"""Run a frozen, sequential PPO value-loss-weight experiment and paired evaluation."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import traceback

ROOT=Path(__file__).resolve().parents[1]
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--run-dir',required=True)
    ap.add_argument('--init-weights',required=True)
    ap.add_argument('--init-sha256',required=True)
    args=ap.parse_args()
    out=Path(args.run_dir).resolve();initial=Path(args.init_weights).resolve()
    if sha(initial)!=args.init_sha256:raise ValueError('Initialization hash mismatch')
    out.mkdir(parents=True,exist_ok=False)
    shutil.copyfile(initial,out/'baseline.pt')
    def write(name,value):
        dest=out/name;temp=dest.with_suffix(dest.suffix+'.tmp')
        temp.write_text(json.dumps(value,indent=2,ensure_ascii=False),encoding='utf-8');os.replace(temp,dest)
    maps=[f'game/data/maps/pool/gen_{s}.json' for s in ('01001000','01004000','01005000','01006001')]
    heldout=[f'game/data/maps/pool/gen_{s}.json' for s in ('01007000','01008000','01009000')]
    common=['--device','cpu','--envs','64','--threads','4','--torch-threads','1','--seed','1',
            '--total-steps','262144','--map-pool',','.join(maps),'--map-path',maps[0],
            '--roster','mixed','--levels','1,4,8,16','--defender-prepare-ticks','900',
            '--curriculum','1','--value-features','detached','--reference-coef','0','--ent-coef','0']
    arms={'reduced':0.00005,'control':0.5}
    import torch
    import rts_native as native
    sys.path.insert(0,str(ROOT/'train'))
    from checkpointing import contract
    from ppo import Cfg
    plan=dict(created=datetime.datetime.now(datetime.timezone.utc).isoformat(),
        source_commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
        source_sha256={str(p.relative_to(ROOT)):sha(p) for p in (ROOT/'train').glob('*.py')},
        input_sha256={p:sha(ROOT/p) for p in maps+heldout+['game/data/stats_placeholder.json']},
        initialization_sha256=sha(out/'baseline.pt'),torch_version=torch.__version__,
        native_contract=contract(native,Cfg()),training_arguments=common,arms=arms,
        seed=1,total_steps_per_arm=262144,execution='sequential CPU; no GPU; independent new optimizers',
        hypothesis='Reducing critic contribution to the shared clipping budget may prevent policy regression. This does not implement return normalization or prove critic calibration.',
        evaluation=dict(maps=heldout,levels=[1,4],episodes_per_case=32,seeds=[100001,200001,300001],policy='frozen_argmax'),
        gate='Reduced must not regress wins or mean building value in any of six cases against either initialization or control, and must improve at least one case against each. Pilot evidence only; independent training seeds and real multiwave validation required before adoption.',
        checkpoint_selection='final 262144-step checkpoint only',automatic_deployment=False)
    write('plan.json',plan)
    def stage(name,command):
        with (out/(name+'.log')).open('w',encoding='utf-8') as log:
            child=subprocess.Popen([sys.executable,*command],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT)
            write('status.json',dict(status='running',stage=name,pid=os.getpid(),child_pid=child.pid))
            print(name,child.pid,flush=True)
            code=child.wait()
            if code:raise RuntimeError(f'{name} exited {code}; inspect its log')
    try:
        for arm,weight in arms.items():
            stage('train-'+arm,['train/ppo.py','--run-dir',str(out/arm),
                '--init-weights',str(out/'baseline.pt'),'--vf-coef',str(weight),*common])
        rows=[]
        for arm in ('baseline','control','reduced'):
            checkpoint=out/'baseline.pt' if arm=='baseline' else out/arm/'policy.pt'
            for i,path in enumerate(heldout):
                for level in (1,4):
                    name=f'eval-{arm}-{i}-L{level}';report=out/(name+'.json')
                    stage(name,['train/evaluate.py','--checkpoint',str(checkpoint),
                        '--map-path',path,'--roster','mixed','--levels',str(level),
                        '--defender-prepare-ticks','900','--episodes','32','--envs','16',
                        '--threads','4','--torch-threads','1','--seed',str(100001+i*100000),
                        '--policy','frozen_argmax','--output',str(report)])
                    result=json.loads(report.read_text(encoding='utf-8'))
                    if result['sha256']['checkpoint']!=sha(checkpoint):raise ValueError('Evaluation checkpoint changed')
                    rows.append(dict(arm=arm,map=path,level=level,wins=result['wins'],episodes=result['episodes'],
                                     value=result['mean_building_value'],report_sha256=sha(report)))
                    write('evaluation-progress.json',rows)
        checks={}
        for baseline in ('baseline','control'):
            pairs=[(r,next(b for b in rows if b['arm']==baseline and b['map']==r['map'] and b['level']==r['level']))
                   for r in rows if r['arm']=='reduced']
            checks[baseline]=all(r['wins']>=b['wins'] and r['value']>=b['value'] for r,b in pairs) and any(
                r['wins']>b['wins'] or r['value']>b['value'] for r,b in pairs)
        write('summary.json',dict(rows=rows,gate_by_baseline=checks,pilot_gate_passed=all(checks.values()),deployed=False))
        write('status.json',dict(status='complete',pid=os.getpid(),pilot_gate_passed=all(checks.values()),deployed=False))
    except Exception as error:
        write('status.json',dict(status='failed',pid=os.getpid(),error=str(error)));traceback.print_exc();return 1
    return 0

if __name__=='__main__':sys.exit(main())
