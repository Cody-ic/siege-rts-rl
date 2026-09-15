import sys
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'train'))
import rts_native as R
import attacker_campaign_evaluate as campaign
from attacker_campaign_evaluate import evaluate_case
from checkpointing import sha256


class CampaignEvaluationTest(unittest.TestCase):
    @unittest.skipUnless(R.FrozenAttacker.runtime_available(), 'Requires ONNX runtime')
    def test_frozen_policy_coverage_starts_with_assault(self):
        stats = 'game/data/stats_placeholder.json'
        policy = R.FrozenAttacker('game/testdata/rl_constant.onnx', stats)
        world = R.TrainingCampaign('game/data/maps/pool/gen_01007000.json', stats, 101,
                                   attacker=policy)
        preparation = world.advance_scripted(900)
        self.assertEqual(sum(preparation['controller_coverage'].values()), 0)
        combat = world.advance_scripted(20)
        self.assertGreater(combat['controller_coverage']['model_eligible_unit_ticks'], 0)
        self.assertEqual(combat['controller_coverage']['script_combat_unit_ticks'], 0)

    def test_city_snapshot_uses_native_units_and_is_read_only(self):
        world = R.TrainingCampaign('game/data/maps/pool/gen_01007000.json',
                                   'game/data/stats_placeholder.json', 101)
        before = world.diagnostic_state_hash
        city = world.diagnostic_city()
        self.assertEqual(city['stone'], 120)
        self.assertEqual(city['wood'], 120)
        self.assertEqual(city['wave'], 1)
        self.assertEqual(city['keep_level'], 1)
        self.assertIs(city['assault'], False)
        obs = dict(zip(R.macro_obs.GLOBAL_NAMES, world.defender_observation()[1].tolist()))
        for name in ('stone', 'wood', 'gold'):
            self.assertIsInstance(city[name], int)
            self.assertAlmostEqual(city[name]/1000, obs[name], places=6)
        for name in ('keep_level', 'unit_level_cap', 'population', 'population_cap'):
            self.assertIsInstance(city[name], int)
            self.assertAlmostEqual(city[name]/32, obs[name], places=6)
        self.assertEqual(before, world.diagnostic_state_hash)

    def test_wave_limit_stops_at_boundary_with_persistent_city(self):
        row = evaluate_case('game/data/maps/pool/gen_01007000.json',
                            'game/data/stats_placeholder.json', 101, None, 1, 12000)
        self.assertEqual(row['end'], 'wave_limit')
        self.assertEqual(row['final_wave'], 2)
        self.assertEqual(len(row['waves']), 1)
        self.assertTrue(row['waves'][0]['wave_complete'])
        coverage = row['waves'][0]['controller_coverage']
        self.assertEqual(coverage['mode'], 'script_only')
        self.assertEqual(coverage['model_eligible_unit_ticks'], 0)
        self.assertGreater(coverage['script_combat_unit_ticks'], 0)
        self.assertGreater(row['waves'][0]['end_tick'], row['waves'][0]['start_tick'])
        self.assertNotEqual(row['initial_hash'], row['waves'][0]['state_hash'])

    def test_frozen_file_paths_preserve_native_campaign_state(self):
        map_path = 'game/data/maps/pool/gen_01007000.json'
        stats = 'game/data/stats_placeholder.json'
        direct = evaluate_case(map_path, stats, 101, None, 1, 1)
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder)/'report'
            args = ['evaluate', '--model', 'game/testdata/rl_constant.onnx', '--maps', map_path,
                    '--stats', stats, '--output', str(output), '--seeds', '101',
                    '--max-waves', '1', '--max-ticks', '1']
            # Exercise native worlds and file freezing without requiring ONNX.
            with patch.object(sys, 'argv', args), patch.object(R, 'FrozenAttacker', return_value=None):
                campaign.main()
            report = json.loads((output/'report.json').read_text(encoding='utf-8'))
            self.assertTrue(report['complete'])
            self.assertEqual(len(report['rows']), 2)
            for row in report['rows']:
                self.assertEqual(row['initial_hash'], direct['initial_hash'])
                self.assertEqual(row['waves'], direct['waves'])

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
        self.assertEqual(row['waves'][-1]['city'], world.diagnostic_city())
        self.assertIn('repair_wood_spent', totals)


class CampaignInputTest(unittest.TestCase):
    def test_coverage_distinguishes_fallback_from_absent_combat(self):
        self.assertIsNone(campaign.summarize_coverage({})['model_eligible_fraction'])
        self.assertEqual(campaign.summarize_coverage({})['mode'], 'no_combat')
        counts = dict(model_eligible_unit_ticks=10, script_combat_unit_ticks=30)
        self.assertEqual(campaign.summarize_coverage(counts)['mode'], 'mixed')
        self.assertEqual(campaign.summarize_coverage(counts)['model_eligible_fraction'], .25)
        self.assertEqual(campaign.summarize_coverage(dict(model_eligible_unit_ticks=10))['mode'],
                         'model_supported')

    def run_evaluation(self, root, mutate=None):
        sources = {name:root/name for name in ('map.json', 'stats.json', 'model.onnx')}
        for name, path in sources.items():
            path.write_bytes(name.encode('ascii'))
        output = root/'report'
        calls = []
        def evaluate(map_path, stats, seed, attacker, *limits):
            self.assertEqual(Path(map_path).parent.resolve(), (output/'inputs').resolve())
            self.assertEqual(Path(map_path).read_bytes(), b'map.json')
            self.assertEqual(Path(stats).read_bytes(), b'stats.json')
            calls.append((seed, attacker is not None))
            if mutate and len(calls) == 2:
                target = sources[mutate] if mutate != 'snapshot' else Path(map_path)
                target.write_bytes(b'changed between seed pairs')
            return dict(map=map_path, seed=seed, initial_hash=seed,
                        end='wave_limit', final_wave=7)
        def policy(model, stats):
            self.assertEqual(Path(model).parent.resolve(), (output/'inputs').resolve())
            self.assertEqual(Path(model).read_bytes(), b'model.onnx')
            self.assertEqual(Path(stats).read_bytes(), b'stats.json')
            return object()
        args = ['evaluate', '--model', str(sources['model.onnx']), '--maps', str(sources['map.json']),
                '--stats', str(sources['stats.json']), '--output', str(output)]
        with patch.object(sys, 'argv', args), patch.object(R, 'FrozenAttacker', policy), \
             patch.object(campaign, 'evaluate_case', evaluate):
            if mutate:
                with self.assertRaisesRegex(ValueError, 'Evaluation input changed'):
                    campaign.main()
            else:
                campaign.main()
        return json.loads((output/'report.json').read_text(encoding='utf-8')), calls

    def test_unchanged_inputs_complete_with_hashed_snapshots(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            report, calls = self.run_evaluation(root)
            self.assertTrue(report['complete'])
            self.assertEqual(len(calls), 4)
            self.assertEqual(report['report_version'], 3)
            self.assertEqual(report['native_build_mode'], R.BUILD_MODE)
            self.assertEqual({r['map'] for r in report['rows']}, {str(root/'map.json')})
            for item in report['inputs']:
                self.assertEqual(item['sha256'], sha256(item['snapshot']))
                self.assertEqual(item['sha256'], sha256(item['source']))

    def test_changed_input_never_completes_or_runs_next_seed_pair(self):
        for name in ('map.json', 'stats.json', 'model.onnx', 'snapshot'):
            with self.subTest(input=name), tempfile.TemporaryDirectory() as folder:
                report, calls = self.run_evaluation(Path(folder), mutate=name)
                self.assertFalse(report['complete'])
                self.assertEqual(len(calls), 2)
                self.assertEqual(len(report['rows']), 1)


if __name__ == '__main__':
    unittest.main()
