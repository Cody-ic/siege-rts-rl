"""Explicit integration check with a real ONNX opponent (model path required)."""
import argparse
from pathlib import Path
import sys
import tempfile

import torch
import rts_native as native

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'train'))
from checkpointing import load_auto,atomic_json,sha256
from macro_train import Config,train
from macro_opponent import load_opponent


def check(model,output):
    stats=str(ROOT/'game/data/stats_placeholder.json')
    map_path=str(ROOT/'game/data/maps/pool/gen_01001000.json')
    opponent,identity=load_opponent(model,stats)
    world=native.TrainingCampaign(map_path,stats,1,attacker=opponent)
    script=native.TrainingCampaign(map_path,stats,1)
    # Pass preparation and verify the supplied policy actually changes battle.
    for _ in range(80):
        world.advance_scripted(20);script.advance_scripted(20)
    assert world.diagnostic_state_hash!=script.diagnostic_state_hash
    branch=world.fork()
    del opponent  # Both campaigns must retain shared native ownership.
    for _ in range(10):
        assert world.advance_scripted(20)==branch.advance_scripted(20)
        assert world.diagnostic_state_hash==branch.diagnostic_state_hash
    cfg=Config((map_path,),stats,rollout=2,period=500,hidden=16,epochs=1,
               attacker_model=str(Path(model).resolve()))
    with tempfile.TemporaryDirectory() as folder:
        full=Path(folder)/'full';split=Path(folder)/'split'
        train(cfg,full,2);train(cfg,split,1);train(cfg,split,2)
        a,_=load_auto(full);b,_=load_auto(split)
        assert a['progress']==b['progress'] and a['campaign']==b['campaign']
        assert a['contract']['opponent']==identity
        torch.testing.assert_close(a['rng']['torch'],b['rng']['torch'],rtol=0,atol=0)
        for name,value in a['model'].items():
            torch.testing.assert_close(value,b['model'][name],rtol=0,atol=0)
        cfg.attacker_model=''
        try:
            train(cfg,split,3)
        except ValueError as error:
            assert 'inputs changed' in str(error)
        else:
            raise AssertionError('Changed opponent was accepted on resume')
        report=dict(simulation=native.SIMULATION_FINGERPRINT,opponent=identity,
                    check_sha256=sha256(__file__),fork_tick=world.tick,
                    learned_campaign=a['campaign'],exact_resume=True,
                    policy_changes_battle=True,changed_opponent_rejected=True)
    atomic_json(output,report)
    print(report)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--model',required=True);parser.add_argument('--output',required=True)
    args=parser.parse_args()
    if Path(args.output).exists():
        raise ValueError('Use a new evidence file')
    check(args.model,args.output)
