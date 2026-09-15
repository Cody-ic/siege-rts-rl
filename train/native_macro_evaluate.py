"""Execute a frozen protocol using the actual game's C++ battle controller."""
import argparse
import json
from pathlib import Path
import subprocess
from checkpointing import atomic_json,sha256


def evaluate(plan_path,arm,output):
    if Path(output).exists():raise ValueError('Use a new evaluation output')
    plan=json.loads(Path(plan_path).read_text(encoding='utf-8'))
    def verify():
        for path,digest in plan['files'].items():
            if sha256(path)!=digest:raise ValueError('Frozen evaluation input changed: '+path)
    verify()
    model=plan['models'][arm]
    report=dict(plan_sha256=sha256(plan_path),arm=arm,mode='native-game-categorical',complete=False,cases=[])
    for path in plan['maps']:
        for seed in plan['seeds']:
            print(f'{arm}: {Path(path).name}, seed {seed}',flush=True)
            result=subprocess.run([plan['runner'],path,plan['stats'],plan['attacker'],model,
                str(seed),str(plan['max_wave']),str(plan['max_ticks'])],check=True,capture_output=True,text=True)
            row=json.loads(result.stdout)
            assert row['map']==path and row['seed']==seed
            if row['policy_period']!=plan['period']:raise ValueError('Native model cadence differs')
            report['cases'].append(row)
            atomic_json(output,report)
            print(row,flush=True)
    verify()
    report['complete']=True;atomic_json(output,report)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plan',required=True);parser.add_argument('--arm',required=True)
    parser.add_argument('--output',required=True)
    args=parser.parse_args();evaluate(args.plan,args.arm,args.output)
