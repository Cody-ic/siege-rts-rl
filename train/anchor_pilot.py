"""GPU-only paired reference-KL pilot after the value-scale regression."""
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

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run-dir', required=True)
    parser.add_argument('--init-weights', required=True)
    parser.add_argument('--init-sha256', required=True)
    args = parser.parse_args()
    initial = Path(args.init_weights).resolve()
    if sha(initial) != args.init_sha256:
        raise ValueError('Initialization hash mismatch')
    import torch
    import rts_native as native
    from checkpointing import contract
    from ppo import Cfg
    if not torch.cuda.is_available():
        raise RuntimeError('This pilot requires CUDA; do not silently fall back to CPU')
    out = Path(args.run_dir).resolve()
    out.mkdir(parents=True, exist_ok=False)
    shutil.copyfile(initial, out / 'baseline.pt')

    def write(name, value):
        target = out / name
        temporary = target.with_suffix(target.suffix + '.tmp')
        temporary.write_text(json.dumps(value, indent=2, ensure_ascii=False), encoding='utf-8')
        os.replace(temporary, target)

    maps = [f'game/data/maps/pool/gen_{s}.json' for s in ('01001000', '01004000', '01005000', '01006001')]
    evaluation_maps = [f'game/data/maps/pool/gen_{s}.json' for s in ('01007000', '01008000', '01009000')]
    common = ['--device', 'cuda', '--envs', '64', '--threads', '4', '--torch-threads', '1',
              '--seed', '1', '--total-steps', '262144', '--map-pool', ','.join(maps),
              '--map-path', maps[0], '--roster', 'mixed', '--levels', '1,4,8,16',
              '--defender-prepare-ticks', '900', '--curriculum', '1',
              '--tactical-goals', 'keep', '--value-features', 'detached',
              '--vf-coef', '0.00005', '--ent-coef', '0', '--lr', '0.0003']
    arms = {'anchored': 1.0, 'control': 0.0}
    write('plan.json', dict(
        created=datetime.datetime.now(datetime.timezone.utc).isoformat(),
        source_commit=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
        source_sha256={str(p.relative_to(ROOT)): sha(p) for p in (ROOT / 'train').glob('*.py')},
        input_sha256={p: sha(ROOT / p) for p in maps + evaluation_maps + ['game/data/stats_placeholder.json']},
        initialization_sha256=sha(out / 'baseline.pt'), torch_version=torch.__version__,
        gpu=torch.cuda.get_device_name(0), native_contract=contract(native, Cfg()),
        common_arguments=common, arms=arms, execution='Sequential, entirely CUDA; fresh optimizers',
        hypothesis='Reference KL may preserve the initialization behavior when critic clipping pressure is reduced.',
        evaluation=dict(maps=evaluation_maps, levels=[1, 4], episodes_per_case=32,
                        seeds=[100001, 200001, 300001], policy='frozen_argmax',
                        role='Reused diagnostic panel, not an untouched holdout'),
        gate='Anchored must not regress wins or mean building value in any case versus baseline or matched control; improve at least one case versus each.',
        checkpoint_selection='Final 262144-step checkpoint only', automatic_deployment=False,
        adoption='Requires independent training seeds, untouched holdout and current-game multiwave validation.'))

    def stage(name, command):
        with (out / (name + '.log')).open('w', encoding='utf-8') as log:
            child = subprocess.Popen([sys.executable, *command], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
            write('status.json', dict(status='running', stage=name, pid=os.getpid(), child_pid=child.pid))
            print(name, child.pid, flush=True)
            if child.wait():
                raise RuntimeError(f'{name} failed; inspect its log')

    try:
        for arm, weight in arms.items():
            stage('train-' + arm, ['train/ppo.py', '--run-dir', str(out / arm),
                  '--init-weights', str(out / 'baseline.pt'), '--reference-coef', str(weight), *common])
        rows = []
        for arm in ('baseline', 'control', 'anchored'):
            checkpoint = out / 'baseline.pt' if arm == 'baseline' else out / arm / 'policy.pt'
            for i, path in enumerate(evaluation_maps):
                for level in (1, 4):
                    name = f'eval-{arm}-{i}-L{level}'
                    report = out / (name + '.json')
                    stage(name, ['train/evaluate.py', '--checkpoint', str(checkpoint), '--map-path', path,
                          '--roster', 'mixed', '--levels', str(level), '--defender-prepare-ticks', '900',
                          '--episodes', '32', '--envs', '16', '--threads', '4', '--torch-threads', '1',
                          '--seed', str(100001 + i * 100000), '--policy', 'frozen_argmax', '--output', str(report)])
                    result = json.loads(report.read_text(encoding='utf-8'))
                    if result['sha256']['checkpoint'] != sha(checkpoint):
                        raise ValueError('Evaluation checkpoint changed')
                    rows.append(dict(arm=arm, map=path, level=level, wins=result['wins'],
                                     episodes=result['episodes'], value=result['mean_building_value'],
                                     report_sha256=sha(report)))
                    write('evaluation-progress.json', rows)
        checks = {}
        for baseline in ('baseline', 'control'):
            pairs = [(r, next(b for b in rows if b['arm'] == baseline and b['map'] == r['map'] and b['level'] == r['level']))
                     for r in rows if r['arm'] == 'anchored']
            checks[baseline] = all(r['wins'] >= b['wins'] and r['value'] >= b['value'] for r, b in pairs) and any(
                r['wins'] > b['wins'] or r['value'] > b['value'] for r, b in pairs)
        write('summary.json', dict(rows=rows, gate_by_baseline=checks, pilot_gate_passed=all(checks.values()), deployed=False))
        write('status.json', dict(status='complete', pid=os.getpid(), pilot_gate_passed=all(checks.values()), deployed=False))
    except Exception as error:
        write('status.json', dict(status='failed', pid=os.getpid(), error=str(error)))
        traceback.print_exc()
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
