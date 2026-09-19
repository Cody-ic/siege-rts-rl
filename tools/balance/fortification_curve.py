"""Print the candidate curve with engine integer rounding; this is not a battle simulation."""
import argparse
import csv
import json
import sys
from pathlib import Path

from combat_math import apply_permille, level_permille, fortification_hp_permille, fortification_upgrade_cost


def rows(stats, building, max_level):
    b, g = stats['buildings'][building], stats['global']
    stone, wood = b['cost_stone'], b['cost_wood']
    previous = 0
    for level in range(1, max_level + 1):
        hp = apply_permille(b['max_hp'], [fortification_hp_permille(
            level, g['fortification_hp_permille_per_level'], g['fortification_linear_until_level'])])
        step = [0, 0]
        if level > 1:
            step = [fortification_upgrade_cost(b['upgrade_cost_' + material], level - 1,
                g['fortification_linear_until_level'], g['fortification_price_step_permille'],
                g['fortification_price_cap_level']) for material in ('stone', 'wood')]
            stone += step[0]
            wood += step[1]
        old = apply_permille(b['max_hp'], [level_permille(level, g['hp_permille_per_level'])])
        yield (level, old, hp, hp - previous if level > 1 else 0, *step, stone, wood)
        previous = hp


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stats', type=Path, default=Path(__file__).resolve().parents[2] / 'game/data/stats_placeholder.json')
    parser.add_argument('--building', choices=('Wall', 'Gate', 'Fence'), default='Wall')
    parser.add_argument('--max-level', type=int, default=30)
    args = parser.parse_args()
    if not 1 <= args.max_level <= 10000:
        parser.error('--max-level must be in [1, 10000]')
    writer = csv.writer(sys.stdout, lineterminator='\n')
    writer.writerow(('level', 'old_hp', 'new_hp', 'hp_increment', 'upgrade_stone',
                     'upgrade_wood', 'total_stone', 'total_wood'))
    writer.writerows(rows(json.loads(args.stats.read_text(encoding='utf-8')), args.building, args.max_level))


if __name__ == '__main__':
    main()
