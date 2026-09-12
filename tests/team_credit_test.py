import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

import numpy as np
import torch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'train'))
import ppo
import team_credit as team
from checkpointing import load_training, load_auto


class TeamCreditTests(unittest.TestCase):
    def setUp(self):
        torch.set_num_threads(1)

    def test_death_and_zero_actor_tail_do_not_cut_team_credit(self):
        rewards = torch.tensor([[0.], [0.], [100.]])
        adv = team.advantages(torch.zeros_like(rewards), rewards, torch.zeros_like(rewards),
            torch.tensor([[False], [False], [True]]), torch.zeros_like(rewards).bool(), .99, .95)
        torch.testing.assert_close(adv[:, 0], torch.tensor([88.454025, 94.05, 100.]))
        live = torch.tensor([[True, True], [False, True], [False, False]])
        samples = adv.expand(-1, 2)[live]
        self.assertEqual(len(samples), 3)
        torch.testing.assert_close(samples[0], samples[1])

    def test_truncation_bootstraps_but_does_not_leak_reset_rewards(self):
        result = team.advantages(torch.zeros(2, 1), torch.tensor([[0.], [999.]]),
            torch.tensor([[10.], [999.]]), torch.tensor([[False], [True]]),
            torch.tensor([[True], [False]]), .99, .95)
        torch.testing.assert_close(result[:, 0], torch.tensor([9.9, 999.]))

    def test_rollout_cut_and_wave_change_keep_continuation(self):
        result = team.advantages(torch.zeros(2, 1), torch.zeros(2, 1),
            torch.tensor([[0.], [10.]]), torch.zeros(2, 1).bool(),
            torch.zeros(2, 1).bool(), .99, .95)
        torch.testing.assert_close(result[:, 0], torch.tensor([9.31095, 9.9]))

    def test_team_potential_telescopes_across_squad_death(self):
        phi = torch.tensor([[-5.], [-2.], [-3.], [0.]])
        rewards = .99 * phi[1:] - phi[:-1]
        result = team.advantages(torch.zeros(3, 1), rewards, torch.zeros(3, 1),
            torch.tensor([[False], [False], [True]]), torch.zeros(3, 1).bool(), .99, 1.)
        self.assertAlmostEqual(result[0, 0].item(), 5., places=5)

    def test_pooling_ignores_padding_preserves_globals_and_rng(self):
        cfg = ppo.Cfg(envs=1, threads=1, device='cpu')
        env = ppo.make_env(cfg, 1, .15)
        obs = ppo.Observer(env)
        obs.read()
        expected = team.features(obs)
        permutation = np.arange(obs.mu)[::-1]
        obs.keys = obs.keys[:, permutation].copy()
        obs.cells = obs.cells[:, permutation].copy()
        obs.own = obs.own[:, permutation].copy()
        np.testing.assert_allclose(team.features(obs), expected)
        obs.cells[obs.keys == 0] = 1000
        obs.own[obs.keys == 0] = 1000
        np.testing.assert_allclose(team.features(obs), expected)
        obs.keys.fill(0)
        empty = team.features(obs)
        self.assertTrue(np.isfinite(empty).all())
        self.assertEqual(empty[0, -1], 0)
        np.testing.assert_array_equal(empty[:, -(obs.glob.shape[1]+1):-1], obs.glob)
        rng = torch.get_rng_state().clone()
        net = team.TeamValue(obs.c, obs.own.shape[-1], obs.glob.shape[-1])
        self.assertTrue(torch.equal(rng, torch.get_rng_state()))
        self.assertTrue(torch.isfinite(net(torch.from_numpy(empty))).all())

    def run_train(self, folder, *extra):
        argv = ['ppo', '--run-dir', str(folder), '--device', 'cpu', '--envs', '2',
                '--threads', '1', '--rollout', '3', '--epochs', '1', '--minibatches', '2',
                '--max-ticks', '18', '--curriculum', '1.0', '--total-steps', '12',
                '--credit-assignment', 'team', '--value-features', 'independent', *extra]
        with patch.object(sys, 'argv', argv), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(ppo.main(), 0)

    def test_native_training_updates_team_critic_and_resumes_exactly_at_boundary(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            self.run_train(root/'full')
            self.run_train(root/'split', '--stop-after-updates', '1')
            first = load_training(root/'split/latest.pt')
            initial = load_training(root/'split/previous.pt')
            self.assertTrue(any(not torch.equal(first['team_model'][k], v)
                                for k, v in initial['team_model'].items()))
            self.assertFalse(torch.equal(first['model']['actor.weight'], initial['model']['actor.weight']))
            self.assertTrue(torch.equal(first['model']['critic.weight'], initial['model']['critic.weight']))
            self.assertEqual(first['progress']['active_partial_episodes'], 0)
            self.run_train(root/'split', '--resume', 'auto')
            full, split = [load_training(root/p/'latest.pt') for p in ('full', 'split')]
            for part in ('model', 'team_model'):
                for key in full[part]:
                    torch.testing.assert_close(full[part][key], split[part][key], rtol=0, atol=0)
            metrics = [json.loads(s) for s in (root/'full/metrics.jsonl').read_text().splitlines()]
            self.assertEqual(metrics[-1]['team_value_samples'], 6)
            self.assertTrue(np.isfinite(metrics[-1]['team_value_loss']))
            split.pop('team_model')
            torch.save(split, root/'split/latest.pt')
            with self.assertWarns(UserWarning):
                recovered, path = load_auto(root/'split')
            self.assertEqual(path.name, 'previous.pt')
            self.assertIsInstance(recovered['team_model'], dict)

    def test_rejects_ambiguous_configuration(self):
        with self.assertRaisesRegex(ValueError, 'team credit requires'):
            ppo.validate(ppo.Cfg(credit_assignment='team'))


if __name__ == '__main__':
    unittest.main()
