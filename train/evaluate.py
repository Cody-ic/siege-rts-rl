"""Frozen argmax policy evaluation; no updates or curriculum promotions.

PYTHONPATH=<bindings-dir> python train/evaluate.py --checkpoint train/ppo_attacker.pt
Run from the repository root. Seeds are episode-indexed, independent of batch size.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
import torch
import rts_native as R
from ppo import Cfg, Policy, make_worlds


def evaluate(checkpoint, cfg, episodes, frac):
    if episodes < 1 or cfg.envs < 1 or not 0 < frac <= 1:
        raise ValueError('positive episodes/envs and 0 < frac <= 1 required')
    n = min(episodes, cfg.envs)
    # **评估必须与训练同一个对手**。对着空城量出来的胜率不能
    # 拿来评价一个对着真守方训练的策略，反之亦然 ⇒ 它跟着 `cfg.defender`。
    # 输出里会带上这一项，不然两份 `evaluation.json` 看不出区别。
    env = R.BatchedEnv(make_worlds(cfg, n, frac), ticks_per_step=cfg.ticks_per_step,
                       defender_map=cfg.map_path if cfg.defender else "",
                       defender_seed=cfg.seed * 31 + 7,
                       defender_macro_period=cfg.defender_macro_period)
    k, c, mu = R.obs.K, R.obs.CHANNEL_COUNT, R.obs.MAX_UNITS_PER_ENV
    net = Policy(k, c, R.obs.SELF_COUNT, R.obs.GLOBAL_COUNT, R.obs.ACTION_COUNT).to(cfg.device)
    net.load_state_dict(torch.load(checkpoint, map_location=cfg.device, weights_only=True))
    net.eval()
    cells = np.zeros((n, mu, k, k, c), np.float32)
    own = np.zeros((n, mu, R.obs.SELF_COUNT), np.float32)
    glob = np.zeros((n, R.obs.GLOBAL_COUNT), np.float32)
    masks = np.zeros((n, mu), np.uint16)
    done = np.zeros(n, np.uint8)
    steps = np.zeros(n, np.int64)
    indices = list(range(n))
    next_index = n
    rows = []
    with torch.inference_mode():
        while len(rows) < episodes:
            env.observe(cells, own, glob)
            env.action_masks(masks)
            logits, _ = net(torch.from_numpy(cells.reshape(-1, k, k, c)).to(cfg.device).permute(0, 3, 1, 2),
                            torch.from_numpy(own.reshape(-1, R.obs.SELF_COUNT)).to(cfg.device),
                            torch.from_numpy(np.repeat(glob, mu, axis=0)).to(cfg.device))
            legal = (masks.reshape(-1, 1) & (1 << np.arange(R.obs.ACTION_COUNT))) != 0
            logits.masked_fill_(~torch.from_numpy(legal).to(cfg.device), float('-inf'))
            actions = logits.argmax(-1).cpu().numpy().reshape(n, mu).astype(np.uint8)
            env.step(actions, done)
            ends = env.episode_ends
            for i in range(n):
                if indices[i] is None:
                    continue
                steps[i] += 1
                if not done[i]:
                    continue
                rows.append({'episode': indices[i], 'steps': int(steps[i]),
                             'end': 'keep_destroyed' if ends[i] == R.EpisodeEnd.KeepDestroyed else 'timeout'})
                if next_index < episodes:
                    env.reset_one(i, make_worlds(cfg, 1, frac, next_index)[0])
                    indices[i], next_index = next_index, next_index + 1
                    steps[i] = 0
                else:
                    indices[i] = None
    wins = sum(row['end'] == 'keep_destroyed' for row in rows)
    return {'episodes': episodes, 'wins': wins, 'timeouts': episodes-wins,
            'win_rate': wins/episodes, 'seed': cfg.seed, 'frac': frac,
            'ticks_per_step': cfg.ticks_per_step, 'max_ticks': env.max_ticks_per_episode,
            'policy': 'frozen_argmax', 'device': cfg.device,
            'defender': 'scripted' if cfg.defender else 'none',
            'defender_macro_period': cfg.defender_macro_period,
            'sha256': {name: hashlib.sha256(Path(path).read_bytes()).hexdigest()
                       for name, path in [('checkpoint', checkpoint), ('map', cfg.map_path), ('stats', cfg.stats_path)]},
            'rows': sorted(rows, key=lambda row: row['episode'])}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--checkpoint', required=True)
    ap.add_argument('--episodes', type=int, default=32)
    ap.add_argument('--envs', type=int, default=8)
    ap.add_argument('--frac', type=float, default=1.0)
    ap.add_argument('--seed', type=int, default=100001)
    ap.add_argument('--device', default='cpu')
    ap.add_argument('--output', default='train/evaluation.json')
    # **对手必须与训练时一致**。默认接守方；要重现归档的 40M
    # 那个读数（它跑在空城上）就给 `--no-defender`。
    ap.add_argument('--no-defender', action='store_true')
    args = ap.parse_args()
    cfg = Cfg(envs=args.envs, seed=args.seed, device=args.device)
    if args.no_defender:
        cfg.defender = False
    result = evaluate(args.checkpoint, cfg, args.episodes, args.frac)
    Path(args.output).write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(f"胜利 {result['wins']} / 超时 {result['timeouts']}，冻结策略胜率 {result['win_rate']:.1%}")
    print(args.output)


if __name__ == '__main__':
    main()
