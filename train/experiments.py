"""Restartable, sequential A/B/C screening. No parallel GPU jobs and no silent failures."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

from checkpointing import atomic_json, atomic_write, contract, load_auto, run_lock, sha256
from ppo import Cfg
import rts_native as R

ROOT = Path(__file__).resolve().parents[1]
LEGS = (('economic-defender', 'economic', True),
        ('victory-defender', 'victory', True),
        ('economic-empty', 'economic', False))


def checked(command, log_path):
    log_path.parent.mkdir(parents=True, exist_ok=True)
    print('Running:', ' '.join(str(x) for x in command), flush=True)
    with log_path.open('a', encoding='utf-8') as log:
        log.write('\nCOMMAND '+json.dumps(command)+'\n')
        log.flush()
        child = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                               env=dict(os.environ, PYTHONIOENCODING='utf-8', PYTHONUNBUFFERED='1'))
    if child.returncode:
        raise RuntimeError(f'Exit {child.returncode}; retained all checkpoints. See {log_path}')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--run-root', default='runs/defender-ab')
    ap.add_argument('--milestones', default='200000,1000000')
    ap.add_argument('--seeds', default='1')
    ap.add_argument('--envs', type=int, default=128)
    ap.add_argument('--threads', type=int, default=8)
    ap.add_argument('--torch-threads', type=int, default=1)
    ap.add_argument('--device', default='cuda')
    ap.add_argument('--episodes', type=int, default=32)
    ap.add_argument('--eval-envs', type=int, default=8)
    ap.add_argument('--eval-seed', type=int, default=100001)
    ap.add_argument('--max-ticks', type=int, default=2400)
    ap.add_argument('--ticks-per-step', type=int, default=6)
    ap.add_argument('--rollout', type=int, default=64)
    args = ap.parse_args()
    with run_lock(args.run_root):
        run(args)


def run(args):
    milestones = [int(x) for x in args.milestones.split(',')]
    seeds = [int(x) for x in args.seeds.split(',')]
    if not milestones or milestones != sorted(set(milestones)) or min(milestones) < 1:
        raise ValueError('milestones must be positive and strictly increasing')
    if not seeds or len(set(seeds)) != len(seeds):
        raise ValueError('provide unique seeds')
    root = Path(args.run_root).resolve()
    root.mkdir(parents=True,exist_ok=True)
    plan = {k:v for k,v in vars(args).items() if k not in
            ('run_root','milestones','threads','torch_threads','device')}
    plan['contract'] = contract(R,Cfg())
    plan['evaluation_sha256'] = sha256(ROOT/'train/evaluate.py')
    plan_path = root/'plan.json'
    if plan_path.exists():
        if json.loads(plan_path.read_text(encoding='utf-8')) != plan:
            raise ValueError('Experiment configuration changed; use a new run-root')
    else:
        atomic_json(plan_path,plan)
    results = []
    # Baselines diagnose feasibility early. Their outputs don't control a learned policy.
    for defender in (True, False):
        output = root/f'baseline-{"defender" if defender else "empty"}.json'
        if output.exists():
            previous = json.loads(output.read_text(encoding='utf-8'))
            if previous['contract'] == plan['contract']:
                continue
        checked([sys.executable, 'train/evaluate.py', '--policy', 'flow',
                 '--episodes', str(args.episodes), '--envs', str(args.eval_envs),
                 '--seed', str(args.eval_seed), '--threads', str(args.threads),
                 '--max-ticks', str(args.max_ticks), '--ticks-per-step', str(args.ticks_per_step),
                 '--output', str(output)] + ([] if defender else ['--no-defender']), root/'baseline.log')
    for steps in milestones:
        for seed in seeds:
            for tag, reward, defender in LEGS:
                folder = root/f'{tag}-seed{seed}'
                marker = folder/f'milestone-{steps}.json'
                if marker.exists():
                    records = json.loads(marker.read_text(encoding='utf-8'))
                    for record in records:
                        if sha256(record['report']) != record['report_sha256']:
                            raise ValueError(f'Evaluation report changed: {record["report"]}')
                    results.extend(records)
                    atomic_json(root/'summary.json',results)
                    continue
                checked([sys.executable, 'train/ppo.py', '--run-dir', str(folder), '--resume', 'auto',
                         '--total-steps', str(steps), '--envs', str(args.envs), '--seed', str(seed),
                         '--device', args.device, '--threads', str(args.threads),
                         '--torch-threads', str(args.torch_threads), '--rollout', str(args.rollout),
                         '--max-ticks', str(args.max_ticks), '--ticks-per-step', str(args.ticks_per_step),
                         '--reward-mode', reward, '--win-reward', '2000'] +
                        ([] if defender else ['--no-defender']), folder/'train.log')
                checkpoint, path = load_auto(folder)
                if checkpoint is None or checkpoint['progress']['env_steps'] < steps:
                    raise RuntimeError(f'{tag} stopped before milestone; rerun the same command to resume')
                step_count = checkpoint['progress']['env_steps']
                frac = checkpoint['progress']['frac']
                stage_results = []
                # Evaluate at real deployment distance AND current curriculum distance.
                for distance in sorted({1.0, frac}):
                    output = folder/f'eval-{step_count}-f{distance:g}.json'
                    checked([sys.executable, 'train/evaluate.py', '--checkpoint', str(path),
                             '--episodes', str(args.episodes), '--envs', str(args.eval_envs),
                             '--seed', str(args.eval_seed), '--frac', str(distance),
                             '--device', args.device, '--threads', str(args.threads),
                             '--output', str(output)], folder/'evaluate.log')
                    report = json.loads(output.read_text(encoding='utf-8'))
                    record = dict(tag=tag, seed=seed, requested_steps=steps, steps=step_count,
                                        frac=distance, wins=report['wins'], episodes=report['episodes'],
                                        win_rate=report['win_rate'], win_rate_95ci=report['win_rate_95ci'],
                                        hit_rate=report['hit_rate'], mean_damage=report['mean_building_damage'],
                                        report=str(output), report_sha256=sha256(output),
                                        checkpoint_sha256=sha256(path))
                    results.append(record)
                    stage_results.append(record)
                    atomic_json(root/'summary.json', results)
                    if distance == 1.0:
                        score = [report['win_rate'],report['mean_building_value'],report['mean_building_damage']]
                        best = folder/'best.json'
                        previous = json.loads(best.read_text(encoding='utf-8')) if best.exists() else None
                        if previous is None or score > previous['score']:
                            with run_lock(folder):
                                atomic_write(folder/'best.pt',lambda stream: stream.write(path.read_bytes()))
                                atomic_json(best,dict(score=score,report=str(output),steps=step_count,
                                                     checkpoint_sha256=sha256(folder/'best.pt')))
                atomic_json(marker,stage_results)
                print(f'Completed {tag} seed {seed}, {step_count:,} steps; reports in {folder}', flush=True)
    print('Screening complete. Review held-out victories, damage, and flow baselines before extending budgets.')
    print('One training seed is a pilot, not proof of improvement. Repeat promising comparisons across seeds.')


if __name__=='__main__':
    main()
