import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'train'))
import rts_native as R
from attacker_campaign_evaluate import evaluate_case


class CampaignEvaluationTest(unittest.TestCase):
    def test_wave_limit_stops_at_boundary_with_persistent_city(self):
        row = evaluate_case('game/data/maps/pool/gen_01007000.json',
                            'game/data/stats_placeholder.json', 101, None, 1, 12000)
        self.assertEqual(row['end'], 'wave_limit')
        self.assertEqual(row['final_wave'], 2)
        self.assertEqual(len(row['waves']), 1)
        self.assertTrue(row['waves'][0]['wave_complete'])
        self.assertGreater(row['waves'][0]['end_tick'], row['waves'][0]['start_tick'])
        self.assertNotEqual(row['initial_hash'], row['waves'][0]['state_hash'])

    def test_observation_limit_does_not_claim_defeat(self):
        map_path = 'game/data/maps/pool/gen_01007000.json'
        stats = 'game/data/stats_placeholder.json'
        row = evaluate_case(map_path, stats, 101, None, 6, 901)
        self.assertEqual(row['end'], 'tick_limit')
        self.assertFalse(row['keep_destroyed'])
        self.assertEqual(row['final_tick'], 901)
        world = R.TrainingCampaign(map_path, stats, 101)
        totals = {}
        while world.tick < 901:
            result = world.advance_scripted(min(20, 901-world.tick))
            for key, value in result['defender_tally'].items():
                totals[key] = totals.get(key, 0) + value
        self.assertEqual(row['waves'][-1]['state_hash'], world.diagnostic_state_hash)
        self.assertEqual(row['waves'][-1]['defender'], totals)
        self.assertIn('repair_wood_spent', totals)


if __name__ == '__main__':
    unittest.main()
