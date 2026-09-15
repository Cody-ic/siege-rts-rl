"""Real native C++ macro inference parity and model-integrity rejection checks."""
import argparse
import json
from pathlib import Path
import shutil
import sys
import tempfile

import numpy as np
import torch
import rts_native as native
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'train'))
from checkpointing import atomic_json,sha256
from macro_evaluate import load_policy


def check(checkpoint,directory,output,source_simulation):
    if Path(output).exists():
        raise ValueError('Use a new evidence file')
    torch.set_num_threads(1)
    stats=str(ROOT/'game/data/stats_placeholder.json')
    model,_,_=load_policy(checkpoint,stats,source_simulation)
    runtime=native.DefenderPolicy(directory,stats)
    rows=[]
    for name in ('gen_01001000.json','gen_01006001.json'):
        for seed in (1,2):
            world=native.TrainingCampaign(str(ROOT/'game/data/maps/pool'/name),stats,seed)
            for _ in range(16):
                tick=world.tick
                before=world.diagnostic_state_hash
                command=runtime.decide(world)
                assert world.diagnostic_state_hash==before
                assert world.command_mask([command])[0]
                with torch.no_grad():
                    expected=model.decide(*world.defender_observation(),world.candidates(),world.map_shape,
                                          greedy=True,detail=world.defender_detail().astype(np.float16))
                assert command==expected.command,(name,seed,world.tick,command,expected.command)
                branch=world.fork()
                world.advance(runtime.period,[command]);branch.advance(runtime.period,[expected.command])
                assert world.diagnostic_state_hash==branch.diagnostic_state_hash
                rows.append(dict(map=name,seed=seed,tick=tick,command=list(command)))
                world.advance_scripted(200)
    rejected=[]
    with tempfile.TemporaryDirectory() as folder:
        root=Path(folder)/'model';shutil.copytree(directory,root)
        manifest_path=root/'manifest.json'
        original=json.loads(manifest_path.read_text())
        for key,value in [('obs_version',999),('period',0),('hidden',0),('selection','sample')]:
            altered=dict(original);altered[key]=value
            manifest_path.write_text(json.dumps(altered))
            try:
                native.DefenderPolicy(str(root),stats)
            except RuntimeError:
                rejected.append(key)
            else:
                raise AssertionError(f'Accepted incompatible {key}')
        manifest_path.write_text(json.dumps(original))
        graph=root/'decoder.onnx';data=bytearray(graph.read_bytes());data[-1]^=1;graph.write_bytes(data)
        try:
            native.DefenderPolicy(str(root),stats)
        except RuntimeError:
            rejected.append('graph-content')
        else:
            raise AssertionError('Accepted altered graph')
    report=dict(checkpoint_sha256=sha256(checkpoint),manifest_sha256=sha256(Path(directory)/'manifest.json'),
                simulation=native.SIMULATION_FINGERPRINT,check_sha256=sha256(__file__),
                decisions=len(rows),cases=rows,exact_commands=True,read_only=True,matched_transition_hashes=True,
                rejected=rejected,game_frontend_integrated=False)
    atomic_json(output,report)
    print(report)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--checkpoint',required=True);parser.add_argument('--directory',required=True)
    parser.add_argument('--output',required=True);parser.add_argument('--source-simulation',required=True)
    args=parser.parse_args()
    check(args.checkpoint,args.directory,args.output,args.source_simulation)
