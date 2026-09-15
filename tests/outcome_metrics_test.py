import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'train'))
from outcome_metrics import summarize_outcomes


class OutcomeMetricsTest(unittest.TestCase):
    def test_recoverable_damage_is_not_destruction(self):
        rows = [dict(tally=dict(dmg_to_blds=1930, blds_destroyed=0, units_killed=0,
                               opponent_repair_wood_spent=12)),
                dict(tally=dict(dmg_to_blds=615, blds_destroyed=1, units_killed=2,
                               opponent_repair_wood_spent=0))]
        summary = summarize_outcomes(rows)
        self.assertEqual(summary['damage_without_destruction_episodes'], 1)
        self.assertEqual(summary['mean']['blds_destroyed'], .5)
        self.assertEqual(summary['mean']['opponent_repair_wood_spent'], 6)
        self.assertIsNone(summary['mean']['phoenix_losses'])

    def test_missing_legacy_fields_are_not_zero(self):
        rows = [dict(tally=dict(dmg_to_blds=0, blds_destroyed=0, phoenix_losses=1)),
                dict(tally=dict(dmg_to_blds=0, blds_destroyed=0))]
        self.assertIsNone(summarize_outcomes(rows)['mean']['phoenix_losses'])
        self.assertIsNone(summarize_outcomes(rows)['mean']['destroyed_stone'])

    def test_new_raw_exchange_fields_are_averaged_without_prices(self):
        rows = [dict(tally=dict(dmg_to_blds=0, blds_destroyed=0,
                               enemy_Mason_levels=level, destroyed_stone=30))
                for level in (1, 3)]
        means = summarize_outcomes(rows)['mean']
        self.assertEqual(means['enemy_Mason_levels'], 2)
        self.assertEqual(means['destroyed_stone'], 30)


if __name__ == '__main__':
    unittest.main()
