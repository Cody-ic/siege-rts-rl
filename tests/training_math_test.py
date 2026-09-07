"""Regression tests runnable on the default build, without torch or numpy."""
import ast
from pathlib import Path
import sys
import unittest
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'train'))
from learning import CompletionWindow, potential_reward


class RewardTests(unittest.TestCase):
    def discounted(self, path, gamma=.99):
        return sum(gamma**t * potential_reward(a, b, t == len(path)-2, gamma)
                   for t, (a, b) in enumerate(zip(path, path[1:])))

    def test_round_trip_cannot_beat_standing_still(self):
        for gamma in (.9, .99, 1.0):
            self.assertAlmostEqual(self.discounted([-10, -9, -10, -10], gamma),
                                   self.discounted([-10, -10, -10, -10], gamma))

    def test_death_and_terminal_distance_do_not_change_shaping_return(self):
        # Losing a unit changes the potential, but cannot create net reward.
        for path in ([-40, -39, -38], [-40, 0, 0], [-40, -40, -40], [-40, -20, -1]):
            self.assertAlmostEqual(self.discounted(path), 40.0)

    def test_terminal_ignores_next_world_and_resets_start_independently(self):
        self.assertEqual(potential_reward(-7, -10000, True, .99), 7)
        self.assertAlmostEqual(potential_reward(-100, -99, False, .99), 1.99)

    def test_partial_rollout_bootstrap_cancels_endpoint_potential(self):
        # A rollout boundary is not a terminal boundary. V' = V - Phi.
        p0, pn, gamma = -40, -32, .99
        path = [p0, -37, pn]
        shaped = sum(gamma**t * potential_reward(a, b, False, gamma)
                     for t, (a, b) in enumerate(zip(path, path[1:])))
        self.assertAlmostEqual(shaped + gamma**2 * (-pn), -p0)


class WindowTests(unittest.TestCase):
    def test_whole_batch_not_last_hundred(self):
        damage = [0]*156 + [1]*100
        for samples in (damage, list(reversed(damage))):
            w = CompletionWindow(100)
            w.add(samples)
            self.assertTrue(w.ready)
            self.assertEqual(w.count, 256)
            self.assertAlmostEqual(w.rate, 100/256)
            self.assertLess(w.rate, .6)

    def test_minimum_window_and_whole_oldest_batch_eviction(self):
        w = CompletionWindow(100)
        w.add([1]*60)
        self.assertFalse(w.ready)
        w.add([0]*50)
        self.assertEqual(w.count, 110)
        w.add([1]*70)
        self.assertEqual(w.count, 120)
        self.assertAlmostEqual(w.rate, 70/120)
        w.clear()
        w.add([])
        self.assertFalse(w.ready)
        self.assertEqual(w.rate, 0)


class ResetSamplingTests(unittest.TestCase):
    def test_single_resets_keep_seed_and_direction_diversity(self):
        # Execute the production factory without importing torch.
        source = Path(__file__).resolve().parents[1] / 'train/ppo.py'
        tree = ast.parse(source.read_text(encoding='utf-8'))
        fn = next(n for n in tree.body if isinstance(n, ast.FunctionDef)
                  and n.name == 'make_worlds')
        native = SimpleNamespace(
            obs=SimpleNamespace(UNIT_TYPE_NAMES=['Ghoul']),
            map_sites=lambda _: {'spawns':[(0,0),(100,0),(100,100),(0,100)], 'keep':(50,50)},
            make_world_init=lambda *a, **kw: kw)
        namespace = {'R':native, 'Cfg':object}
        exec(compile(ast.Module(body=[fn], type_ignores=[]), str(source), 'exec'), namespace)
        cfg = SimpleNamespace(seed=1,map_path='',stats_path='')
        make = namespace['make_worlds']
        batch = make(cfg, 4, start=8)
        singles = [make(cfg, 1, start=i)[0] for i in range(8,12)]
        self.assertEqual(batch, singles)
        self.assertEqual(len({w['seed'] for w in singles}), 4)
        self.assertEqual(len({w['attackers'][0][1:3] for w in singles}), 4)


if __name__ == '__main__':
    unittest.main()
