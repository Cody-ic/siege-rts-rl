"""Paired product-script / frozen-attacker evaluation with persistent cities."""
import argparse
from collections import Counter
from pathlib import Path

import rts_native as R
from checkpointing import atomic_json, atomic_write, sha256


def evaluate_case(map_path, stats, seed, attacker, max_waves, max_ticks):
    world = R.TrainingCampaign(map_path, stats, seed, attacker=attacker)
    initial_hash = world.diagnostic_state_hash
    waves = []
    attack, defense = Counter(), Counter()
    start = world.tick
    defeated = False
    while world.wave <= max_waves and world.tick < max_ticks and not defeated:
        wave = world.wave
        result = world.advance_scripted(min(20, max_ticks - world.tick))
        attack.update(result['attacker_tally'])
        defense.update(result['defender_tally'])
        defeated = result['defeated']
        if result['wave_advanced'] or defeated or world.tick == max_ticks:
            waves.append(dict(wave=wave, start_tick=start, end_tick=world.tick,
                keep_destroyed=defeated, wave_complete=result['wave_advanced'],
                attacker=dict(attack), defender=dict(defense),
                city=world.diagnostic_city(),
                state_hash=world.diagnostic_state_hash))
            start = world.tick
            attack.clear()
            defense.clear()
    return dict(map=map_path, seed=seed, initial_hash=initial_hash,
        keep_destroyed=defeated, end=('keep_destroyed' if defeated else
            'wave_limit' if world.wave > max_waves else 'tick_limit'),
        final_wave=world.wave, final_tick=world.tick, waves=waves)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--model', required=True)
    parser.add_argument('--maps', nargs='+', required=True)
    parser.add_argument('--stats', default='game/data/stats_placeholder.json')
    parser.add_argument('--seeds', nargs='+', type=int, default=[101, 102])
    parser.add_argument('--max-waves', type=int, default=6)
    parser.add_argument('--max-ticks', type=int, default=24000)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    if min(args.max_waves, args.max_ticks) < 1:
        parser.error('positive limits required')
    if len(set(args.maps)) != len(args.maps) or len(set(args.seeds)) != len(args.seeds):
        parser.error('duplicate maps/seeds are not additional evidence')
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=False)
    inputs = []

    def freeze(source, name):
        target = output / 'inputs' / name
        payload = Path(source).read_bytes()
        atomic_write(target, lambda stream: stream.write(payload))
        item = dict(source=str(Path(source).resolve()), snapshot=str(target), sha256=sha256(target))
        inputs.append(item)
        return item

    model = freeze(args.model, 'policy.onnx')
    stats = freeze(args.stats, 'stats.json')
    maps = {path:freeze(path, f'map-{i:03d}.json') for i,path in enumerate(args.maps)}

    def verify_inputs():
        for item in inputs:
            for path in (item['source'], item['snapshot']):
                if sha256(path) != item['sha256']:
                    raise ValueError(f'Evaluation input changed: {path}')

    report = dict(complete=False, rows=[], report_version=2, model_sha256=model['sha256'],
        simulation=R.SIMULATION_FINGERPRINT, native_build_mode=R.BUILD_MODE,
        stats_sha256=stats['sha256'], inputs=inputs,
        maps={p:item['sha256'] for p,item in maps.items()}, seeds=args.seeds,
        max_waves=args.max_waves, max_ticks=args.max_ticks,
        evaluator_sha256=sha256(__file__),
        scope='Product controller comparison: normal cadence, script fallback outside model support',
        city_units='Resources, population, levels and HP are native integers; keep_hp_fraction is a ratio',
        interpretation='Wave/tick limits are censored, not final attacker losses; damage is not destruction')
    atomic_json(output / 'report.json', report)
    verify_inputs()
    policy = R.FrozenAttacker(model['snapshot'], stats['snapshot'])
    for path in args.maps:
        for seed in args.seeds:
            initial_hash = None
            for arm, attacker in (('script', None), ('rl', policy)):
                verify_inputs()
                row = evaluate_case(maps[path]['snapshot'], stats['snapshot'], seed,
                                    attacker, args.max_waves, args.max_ticks)
                verify_inputs()
                if initial_hash is not None and row['initial_hash'] != initial_hash:
                    raise ValueError('Paired initial city states differ')
                initial_hash = row['initial_hash']
                row['map'] = path
                row['arm'] = arm
                report['rows'].append(row)
                atomic_json(output / 'report.json', report)
                print(path, seed, arm, row['end'], row['final_wave'], flush=True)
    verify_inputs()
    report['complete'] = True
    atomic_json(output / 'report.json', report)


if __name__ == '__main__':
    main()
