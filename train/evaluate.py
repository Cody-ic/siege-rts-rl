"""Frozen evaluation on held-out seeds, independent of batch size.

The flow diagnostic reads the same local observation and action masks as the policy.
"""
import argparse
import math
from pathlib import Path
import time

import numpy as np
import torch
import rts_native as R
from checkpointing import atomic_json, contract, load_training, load_weights, sha256
from ppo import Cfg, Observer, Policy, make_env, make_worlds, policy_forward
from diagnostics import (MOVE_DELTAS, EpisodeDiagnostics, episode_rng,
                         sample_actions, summarize_spawns)


def wilson(wins, n):
    z = 1.959963984540054
    p = wins / n
    denominator = 1 + z*z/n
    centre = (p + z*z/(2*n)) / denominator
    half = z * math.sqrt(p*(1-p)/n + z*z/(4*n*n)) / denominator
    return [max(0, centre-half), min(1, centre+half)]


def flow_actions(observation):
    rows, cells, _, _, legal = observation
    names = R.obs.ACTION_NAMES
    move_names = tuple(MOVE_DELTAS)
    directions = np.array(list(MOVE_DELTAS.values()))
    channels = [c[0] for c in R.obs.CHANNELS]
    centre = R.obs.K//2
    vectors = cells[:, centre, centre][:, [channels.index('flow_di'),channels.index('flow_dj')]]
    result = np.zeros(len(rows), np.uint8)
    for i in range(len(rows)):
        for name in ('AtkBld', 'AtkWall', 'AtkNear'):
            action = names.index(name)
            if legal[i, action]:
                result[i] = action
                break
        else:
            if np.any(vectors[i]):
                score = directions @ vectors[i] / np.linalg.norm(directions, axis=1)
                for direction in np.argsort(-score, kind='stable'):
                    action = names.index(move_names[direction])
                    if legal[i, action]:
                        result[i] = action
                        break
    return result


def breach_actions(observation):
    """Keep moving through a breach instead of attacking every nearby wall.

    Only public observations/masks are read. The existing native movement action
    handles structures actually blocking the destination (try_bump_attack).
    This is a separate diagnostic/teacher, not an override for learned actions.
    """
    rows, cells, own, glob, legal = observation
    move_mask = legal.copy()
    for i, name in enumerate(R.obs.ACTION_NAMES):
        if name.startswith('Atk'):
            move_mask[:, i] = False
    walk = flow_actions((rows, cells, own, glob, move_mask))
    fallback = flow_actions(observation)
    bld = R.obs.ACTION_NAMES.index('AtkBld')
    stop = R.obs.ACTION_NAMES.index('Stop')
    return np.where(legal[:, bld], bld,
                    np.where(walk != stop, walk, fallback)).astype(np.uint8)


def evaluate(checkpoint, cfg, episodes, frac, policy='frozen_argmax', *, diagnostics=False, trace_every=25):
    if episodes < 1 or cfg.envs < 1 or not 0 < frac <= 1 or cfg.max_ticks < 1:
        raise ValueError('positive episodes/envs/max_ticks and 0 < frac <= 1 required')
    if policy not in ('frozen_argmax', 'frozen_sample', 'flow', 'flow_breach'):
        raise ValueError('unknown evaluation policy')
    if trace_every < 1:
        raise ValueError('trace_every must be positive')
    torch.set_num_threads(cfg.torch_threads)
    n = min(episodes, cfg.envs)
    env = make_env(cfg, n, frac)
    obs = Observer(env)
    net = None
    if policy in ('frozen_argmax', 'frozen_sample'):
        if not checkpoint:
            raise ValueError('--checkpoint required for frozen policy')
        net = Policy(R.obs.K, R.obs.CHANNEL_COUNT, R.obs.SELF_COUNT,
                     R.obs.GLOBAL_COUNT, R.obs.ACTION_COUNT).to(cfg.device)
        net.load_state_dict(load_weights(checkpoint))
        net.eval()
    done = np.zeros(n, np.uint8)
    actions = np.zeros((n, obs.mu), np.uint8)
    tally = np.zeros((n, R.obs.TALLY_FIELDS), np.float32)
    totals = np.zeros_like(tally, dtype=np.float64)
    steps = np.zeros(n, np.int64)
    indices, next_index, rows = list(range(n)), n, []
    generators = [episode_rng(cfg.seed, i) for i in indices]
    spawns = list(R.map_sites(cfg.map_path)['spawns'])
    probes = ([EpisodeDiagnostics(p, trace_every) for p in env.potentials] if diagnostics else None)
    names = {R.EpisodeEnd.KeepDestroyed:'keep_destroyed', R.EpisodeEnd.Timeout:'timeout',
             R.EpisodeEnd.AttackersEliminated:'attackers_eliminated'}
    started = time.perf_counter()
    with torch.inference_mode():
        while len(rows) < episodes:
            observation = obs.read()
            actions.fill(0)
            probabilities = None
            if net is not None:
                dist, _ = policy_forward(net, observation, cfg.device)
                selected = np.empty(0, np.uint8)
                if dist is not None:
                    if diagnostics or policy == 'frozen_sample':
                        probabilities = dist.probs.cpu().numpy()
                    if policy == 'frozen_sample':
                        selected = sample_actions(probabilities, observation[0], obs.mu, generators)
                    else:
                        selected = dist.logits.argmax(-1).cpu().numpy().astype(np.uint8)
            else:
                selected = (breach_actions if policy == 'flow_breach' else flow_actions)(observation)
            actions.reshape(-1)[observation[0]] = selected
            if probes is not None:
                for i, probe in enumerate(probes):
                    if indices[i] is not None:
                        where = observation[0] // obs.mu == i
                        probe.before_step(observation[1][where], observation[4][where], selected[where],
                                          probabilities[where] if probabilities is not None else None)
            env.step(actions, done)
            env.take_tally(tally)
            totals += tally
            ends = env.episode_ends
            potentials = env.potentials if probes is not None else None
            live_squads = env.unit_counts if probes is not None else None
            for i in range(n):
                if indices[i] is None:
                    continue
                steps[i] += 1
                if probes is not None:
                    probes[i].after_step(int(steps[i]), totals[i], potentials[i], live_squads[i], done[i])
                if not done[i]:
                    continue
                row = {'episode':indices[i], 'world_seed':cfg.seed*1000+indices[i],
                       'spawn_index':(cfg.seed+indices[i]) % len(spawns),
                       'steps':int(steps[i]), 'end':names[ends[i]],
                       'tally':dict(zip(R.obs.TALLY_NAMES, totals[i].tolist()))}
                if probes is not None:
                    row['diagnostics'] = probes[i].report()
                rows.append(row)
                if next_index < episodes:
                    env.reset_one(i, make_worlds(cfg, 1, frac, next_index)[0])
                    generators[i] = episode_rng(cfg.seed, next_index)
                    if probes is not None:
                        probes[i] = EpisodeDiagnostics(env.potentials[i], trace_every)
                    indices[i], next_index = next_index, next_index+1
                    steps[i] = 0
                    totals[i].fill(0)
                else:
                    indices[i] = None
                    generators[i] = None
    wins = sum(row['end']=='keep_destroyed' for row in rows)
    return {'episodes':episodes, 'wins':wins,
            'timeouts':sum(row['end']=='timeout' for row in rows),
            'eliminated':sum(row['end']=='attackers_eliminated' for row in rows),
            'win_rate':wins/episodes, 'win_rate_95ci':wilson(wins, episodes),
            'hit_rate':sum(r['tally']['dmg_to_blds']>0 for r in rows)/episodes,
            'mean_building_damage':float(np.mean([r['tally']['dmg_to_blds'] for r in rows])),
            'mean_building_value':float(np.mean([r['tally']['bld_value'] for r in rows])),
            'seed':cfg.seed, 'frac':frac, 'ticks_per_step':cfg.ticks_per_step,
            'max_ticks':cfg.max_ticks, 'policy':policy, 'device':cfg.device,
            'defender':'scripted' if cfg.defender else 'none',
            'defender_macro_period':cfg.defender_macro_period,
            'spawns':[list(s) for s in spawns], 'by_spawn':summarize_spawns(rows),
            'diagnostics':diagnostics, 'trace_every':trace_every if diagnostics else None,
            'evaluator_sha256':{name:sha256(Path(__file__).parent/name)
                                for name in ('evaluate.py','diagnostics.py')},
            'contract':contract(R,cfg), 'seconds':time.perf_counter()-started,
            'sha256':{'checkpoint':sha256(checkpoint) if checkpoint else None,
                      'map':sha256(cfg.map_path), 'stats':sha256(cfg.stats_path)},
            'rows':sorted(rows, key=lambda row:row['episode'])}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--checkpoint')
    ap.add_argument('--policy', choices=('frozen_argmax','frozen_sample','flow','flow_breach'), default='frozen_argmax')
    ap.add_argument('--diagnostics', action='store_true')
    ap.add_argument('--trace-every', type=int, default=25)
    ap.add_argument('--episodes', type=int, default=32)
    ap.add_argument('--envs', type=int, default=8)
    ap.add_argument('--frac', type=float, default=1)
    ap.add_argument('--seed', type=int, default=100001)
    ap.add_argument('--device', default='cpu')
    ap.add_argument('--threads', type=int, default=4)
    ap.add_argument('--torch-threads', type=int, default=1)
    ap.add_argument('--output', default='runs/evaluation.json')
    ap.add_argument('--no-defender', action='store_true', default=None)
    ap.add_argument('--map-path')
    ap.add_argument('--stats-path')
    ap.add_argument('--max-ticks', type=int)
    ap.add_argument('--ticks-per-step', type=int)
    args = ap.parse_args()
    cfg, saved = Cfg(), None
    if args.checkpoint:
        data = torch.load(args.checkpoint, map_location='cpu', weights_only=True)
        if isinstance(data, dict) and 'format' in data:
            saved = load_training(args.checkpoint)
            cfg = Cfg(**saved['config'])
        else:
            print('Legacy policy-only file: opponent/map settings must be supplied explicitly.')
    for name in ('envs','seed','device','threads','torch_threads','map_path','stats_path','max_ticks','ticks_per_step'):
        value = getattr(args,name)
        if value is not None:
            if saved and name in ('map_path','stats_path','max_ticks','ticks_per_step') and value != getattr(cfg,name):
                raise ValueError(f'Evaluation {name} differs from training checkpoint')
            setattr(cfg,name,value)
    if args.no_defender is not None:
        if saved and cfg.defender:
            raise ValueError('Cannot evaluate a scripted-defender checkpoint against an empty city')
        cfg.defender = False
    if saved and contract(R,cfg) != saved['contract']:
        raise ValueError('Evaluation contract differs from checkpoint; rebuild/use matching data and learner')
    result = evaluate(args.checkpoint,cfg,args.episodes,args.frac,args.policy,
                      diagnostics=args.diagnostics,trace_every=args.trace_every)
    atomic_json(args.output,result)
    print(f'Frozen evaluation: wins {result["wins"]}/{result["episodes"]}, '
          f'hit buildings {result["hit_rate"]:.0%}, mean damage {result["mean_building_damage"]:.1f}')
    for spawn in result['by_spawn']:
        print(f'  spawn {spawn["spawn_index"]}: wins {spawn["wins"]}/{spawn["episodes"]}, '
              f'hit buildings {spawn["hit_buildings"]}/{spawn["episodes"]}, '
              f'mean damage {spawn["mean_building_damage"]:.1f}')
    print(args.output)


if __name__=='__main__':
    main()
