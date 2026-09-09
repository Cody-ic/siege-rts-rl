"""Real script/ONNX rotation across campaign boundaries and exact resume."""
import argparse
from pathlib import Path
import sys
import tempfile
import torch
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'train'))
from checkpointing import load_auto,atomic_json,sha256
from macro_train import Config,train


def equal(a,b):
    if isinstance(a,torch.Tensor):torch.testing.assert_close(a,b,rtol=0,atol=0)
    elif isinstance(a,dict):
        assert a.keys()==b.keys()
        for k in a:equal(a[k],b[k])
    elif isinstance(a,(tuple,list)):
        assert len(a)==len(b)
        for x,y in zip(a,b):equal(x,y)
    else:assert a==b,(a,b)


def check(model,output):
    cfg=Config((str(ROOT/'game/data/maps/pool/gen_01001000.json'),),
               str(ROOT/'game/data/stats_placeholder.json'),hidden=16,epochs=1,
               rollout=1,period=10000,max_wave=1,attacker_pool=('',str(Path(model).resolve())))
    with tempfile.TemporaryDirectory() as folder:
        full=Path(folder)/'full';split=Path(folder)/'split'
        train(cfg,full,2);train(cfg,split,1);train(cfg,split,2)
        a,_=load_auto(full);b,_=load_auto(split)
        for key in ('model','optimizer','campaign','progress','contract','config'):
            equal(a[key],b[key])
        equal(a['rng']['torch'],b['rng']['torch'])
        finished=a['progress']['finished_campaigns']
        assert [r['opponent_index'] for r in finished]==[0,1],finished
        assert a['campaign']['opponent_index']==0
        cfg.attacker_pool=tuple(reversed(cfg.attacker_pool))
        try:train(cfg,split,3)
        except ValueError as error:assert 'inputs changed' in str(error)
        else:raise AssertionError('Changed opponent order accepted')
        report=dict(exact_resume=True,rotation_crossed=True,changed_order_rejected=True,
                    contract=a['contract'],completed_campaigns=finished,
                    check_sha256=sha256(__file__),scope='Two one-wave campaigns, real script and frozen ONNX; recovery evidence, not policy quality')
        atomic_json(output,report)
        print(report)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--model',required=True);p.add_argument('--output',required=True)
    args=p.parse_args()
    if Path(args.output).exists():raise ValueError('Use a new output')
    check(args.model,args.output)
