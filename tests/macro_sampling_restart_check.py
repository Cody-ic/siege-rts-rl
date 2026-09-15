"""Two-process native policy RNG restore, including full command-log replay."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

import rts_native as native
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'train'))
from checkpointing import atomic_json,sha256


def child(directory,phase,path):
    stats=str(ROOT/'game/data/stats_placeholder.json')
    policy=native.DefenderPolicy(directory,stats)
    world=native.TrainingCampaign(str(ROOT/'game/data/maps/pool/gen_01006001.json'),stats,4)
    rng=native.PolicyRng(57)
    def advance(count):
        commands=[]
        for _ in range(count):
            command=policy.decide(world,rng)
            assert world.command_mask([command])[0]
            world.advance(policy.period,[command]);commands.append(list(command))
        return commands
    if phase=='write':
        prefix=advance(64)
        state=dict(identity=policy.identity,rng=rng.state,commands=prefix,world_hash=world.diagnostic_state_hash)
        tail=advance(64)
        state.update(expected_tail=tail,expected_rng=rng.state,expected_hash=world.diagnostic_state_hash)
        atomic_json(path,state)
    else:
        saved=json.loads(Path(path).read_text())
        assert saved['identity']==policy.identity
        for command in saved['commands']:
            world.advance(policy.period,[tuple(command)])
        assert world.diagnostic_state_hash==saved['world_hash']
        rng.state=saved['rng']
        assert advance(64)==saved['expected_tail']
        assert list(rng.state)==saved['expected_rng']
        assert world.diagnostic_state_hash==saved['expected_hash']
        prior=list(rng.state)
        try:
            rng.state=[0,0,0,0]
        except ValueError:
            pass
        else:
            raise AssertionError('Accepted zero RNG')
        assert list(rng.state)==prior
        policy.decide(world)  # Greedy must not consume the external stream.
        assert list(rng.state)==prior


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory',required=True);parser.add_argument('--output',required=True)
    parser.add_argument('--phase',choices=['write','read'])
    args=parser.parse_args()
    if args.phase:
        child(args.directory,args.phase,args.output)
    else:
        if Path(args.output).exists():raise ValueError('Use a new evidence file')
        with tempfile.TemporaryDirectory() as folder:
            state=str(Path(folder)/'state.json')
            for phase in ('write','read'):
                subprocess.run([sys.executable,__file__,'--directory',args.directory,
                                '--output',state,'--phase',phase],check=True)
            saved=json.loads(Path(state).read_text())
            report=dict(simulation=native.SIMULATION_FINGERPRINT,check_sha256=sha256(__file__),
                model_identity=saved['identity'],replayed_commands=64,matched_new_commands=64,
                exact_rng_and_world=True,distinct_command_kinds=sorted(set(c[0] for c in saved['commands']+saved['expected_tail'])),
                game_save_integrated=False,scope='two independent Python hosts invoking C++ inference; explicit RNG and command replay, not game save format')
            atomic_json(args.output,report);print(report)
