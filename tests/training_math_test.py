"""Regression tests runnable on the default build, without torch or numpy."""
import ast
import math
from dataclasses import dataclass
from pathlib import Path
import sys
import unittest
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'train'))
from learning import CompletionWindow, potential_reward, task_reward


class RewardTests(unittest.TestCase):
    def test_explicit_win_and_economic_compatibility(self):
        self.assertEqual(task_reward(110, False, 'economic', 2000), 110)
        self.assertEqual(task_reward(9.6, True, 'economic', 2000), 2009.6)
        self.assertEqual(task_reward(110, False, 'victory', 1), 0)
        self.assertEqual(task_reward(9.6, True, 'victory', 1), 1)
        for weight in (0, -1, float('inf'), float('nan')):
            with self.assertRaises(ValueError):
                task_reward(0, False, 'victory', weight)

    def test_victory_mode_rebuilds_timeout_and_delayed_win(self):
        gamma = .99
        def trajectory(economics, won):
            # Different paths and lengths must still have identical PBRS offset.
            phi = [-20] + [-3] * (len(economics)-1) + [0]
            return sum(gamma**t * (task_reward(e, won and t == len(economics)-1,
                                              'victory', 1)
                                  + .02*potential_reward(phi[t], phi[t+1],
                                                         t == len(economics)-1, gamma))
                       for t, e in enumerate(economics))
        immediate = trajectory([0], True)
        self.assertGreater(immediate, trajectory([110, 0], True))
        self.assertAlmostEqual(trajectory([0]*400, False),
                               trajectory([100000]*400, False))
        self.assertGreater(trajectory([100000]*399+[0], True),
                           trajectory([100000]*400, False))

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


class PreparationTests(unittest.TestCase):
    def test_preparation_requires_full_distance_before_world_creation(self):
        source = Path(__file__).resolve().parents[1] / 'train/ppo.py'
        tree = ast.parse(source.read_text(encoding='utf-8'))
        nodes = [n for n in tree.body if isinstance(n, (ast.ClassDef, ast.FunctionDef))
                 and n.name in ('Cfg', 'validate', 'make_env')]
        calls = []
        native = SimpleNamespace(Side=SimpleNamespace(Attacker=0),
                                 BatchedEnv=lambda *a, **kw: calls.append(kw))
        namespace = {'dataclass': dataclass, 'np': SimpleNamespace(isfinite=math.isfinite),
                     'torch': SimpleNamespace(cuda=SimpleNamespace(is_available=lambda: False)),
                     'task_reward': task_reward, 'R': native,
                     'make_worlds': lambda cfg, n, *args: [None]*n}
        exec(compile(ast.Module(body=nodes, type_ignores=[]), str(source), 'exec'), namespace)
        cfg = namespace['Cfg']()
        namespace['validate'](cfg)
        cfg.defender_prepare_ticks = 900
        with self.assertRaisesRegex(ValueError, 'curriculum='):
            namespace['validate'](cfg)
        for frac in (.15, .75, float('nan')):
            with self.assertRaisesRegex(ValueError, 'frac='):
                namespace['make_env'](cfg, 1, frac)
        self.assertEqual(calls, [])
        cfg.curriculum = (1.0,)
        namespace['validate'](cfg)
        namespace['make_env'](cfg, 1, 1.0)
        self.assertEqual(calls[0]['defender_prepare_ticks'], 900)
        cfg.defender_prepare_ticks = 0
        namespace['make_env'](cfg, 1, .15)
        self.assertEqual(len(calls), 2)


class ResetSamplingTests(unittest.TestCase):
    def test_each_map_covers_its_own_spawn_level_cycle(self):
        source = Path(__file__).resolve().parents[1] / 'train/ppo.py'
        tree = ast.parse(source.read_text(encoding='utf-8'))
        nodes = [n for n in tree.body if isinstance(n, ast.FunctionDef)
                 and n.name in ('episode_map', 'episode_spec')]
        namespace = {'world_factory': lambda path, _: (None, {'spawns': range(3 if path == 'a' else 5)})}
        exec(compile(ast.Module(body=nodes, type_ignores=[]), str(source), 'exec'), namespace)
        cfg = SimpleNamespace(seed=1, map_pool=('a', 'b'), stats_path='', levels=(1, 7))
        cases = [namespace['episode_spec'](cfg, i) for i in range(60)]
        for path, count in (('a', 3), ('b', 5)):
            expected = {(path, level, spawn) for level in cfg.levels for spawn in range(count)}
            self.assertEqual({case for case in cases if case[0] == path}, expected)
            self.assertEqual(len({cases.count(case) for case in expected}), 1)

    def test_single_resets_keep_seed_and_direction_diversity(self):
        # Execute the production factory without importing torch.
        source = Path(__file__).resolve().parents[1] / 'train/ppo.py'
        tree = ast.parse(source.read_text(encoding='utf-8'))
        functions = [n for n in tree.body if isinstance(n,ast.FunctionDef)
                     and n.name in ('make_worlds','episode_map','episode_spec')]
        native = SimpleNamespace(
            obs=SimpleNamespace(UNIT_TYPE_NAMES=['Ghoul']),
            map_sites=lambda _: {'spawns':[(0,0),(100,0),(100,100),(0,100)], 'keep':(50,50)},
            make_world_init=lambda *a, **kw: kw)
        namespace = {'R':native, 'Cfg':object,
                     'world_factory':lambda *_: (SimpleNamespace(make=native.make_world_init),native.map_sites(''))}
        exec(compile(ast.Module(body=functions, type_ignores=[]), str(source), 'exec'), namespace)
        cfg = SimpleNamespace(seed=1,map_path='',stats_path='',map_pool=(),roster='ghouls',levels=(1,))
        make = namespace['make_worlds']
        batch = make(cfg, 4, start=8)
        singles = [make(cfg, 1, start=i)[0] for i in range(8,12)]
        self.assertEqual(batch, singles)
        self.assertEqual(len({w['seed'] for w in singles}), 4)
        self.assertEqual(len({w['attackers'][0][1:3] for w in singles}), 4)


if __name__ == '__main__':
    unittest.main()
