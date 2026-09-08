"""Optional integration test: PYTHONPATH=<bindings-dir> python tests/ppo_smoke_test.py.

Requires numpy and CPU torch. Uses real native worlds, optimizer and checkpoints;
only the training size and promotion threshold are reduced for a bounded test.
"""
import contextlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

import numpy as np
import torch
import rts_native as R

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'train'))
import ppo
from evaluate import evaluate
from learning import potential_reward


class NativeTrainingTests(unittest.TestCase):
    def config(self):
        cfg = ppo.Cfg(envs=2, ticks_per_step=600, rollout=3, total_steps=18,
                      epochs=1, minibatches=2, device='cpu', defender=False, threads=1,
                      promote_window=2, promote_at=0.0, promote_outcome_at=0.0)
        cfg.map_path = str(ROOT / cfg.map_path)
        cfg.stats_path = str(ROOT / cfg.stats_path)
        return cfg

    def test_native_potential_and_full_horizon_after_reset(self):
        cfg = self.config()
        env = R.BatchedEnv(ppo.make_worlds(cfg,2,1.0), ticks_per_step=600, threads=1)
        actions = np.zeros((2,R.obs.MAX_UNITS_PER_ENV),dtype=np.uint8)
        done = np.zeros(2,dtype=np.uint8)
        for episode in range(2):
            initial = env.potentials
            previous = initial
            discounted = np.zeros(2)
            for t in range(4):
                env.step(actions,done)
                self.assertEqual(done.tolist(), [int(t == 3)]*2)
                self.assertEqual(env.episode_ends,
                                 [R.EpisodeEnd.Timeout if t == 3 else R.EpisodeEnd.Running]*2)
                after = env.potentials
                discounted += cfg.gamma**t * np.array([
                    potential_reward(a,b,bool(d),cfg.gamma)
                    for a,b,d in zip(previous,after,done)])
                previous = after
            np.testing.assert_allclose(discounted,-np.array(initial),atol=1e-8)
            for i, world in enumerate(ppo.make_worlds(cfg,2,1.0,start=2+episode*2)):
                env.reset_one(i,world)
            self.assertEqual(env.episode_ends, [R.EpisodeEnd.Running]*2)

    def test_real_ppo_updates_and_bootstraps_before_promotion(self):
        torch.set_num_threads(1)
        cfg = self.config()
        events = []
        real_env = R.BatchedEnv

        class TracedEnv:
            def __init__(self,*a,**kw):
                self.env = real_env(*a,**kw)
                self.step_no = 0
            def __getattr__(self,name):
                return getattr(self.env,name)
            def step(self,*a):
                self.step_no += 1
                return self.env.step(*a)
            def observe(self,*a):
                events.append(('observe',self.step_no))
                return self.env.observe(*a)
            def reset_one(self,*a):
                events.append(('reset',self.step_no))
                return self.env.reset_one(*a)

        initial = {}
        real_policy = ppo.Policy
        class TracedPolicy(real_policy):
            def __init__(self,*a):
                super().__init__(*a)
                initial.update({k:v.clone() for k,v in self.state_dict().items()})

        with tempfile.TemporaryDirectory() as folder:
            previous = Path.cwd()
            try:
                os.chdir(folder)
                Path('train').mkdir()
                output = io.StringIO()
                with patch.object(ppo,'Cfg',lambda:cfg), patch.object(R,'BatchedEnv',TracedEnv), \
                     patch.object(ppo,'Policy',TracedPolicy), patch.object(sys,'argv',['ppo','--run-dir','train']), \
                     contextlib.redirect_stdout(output):
                    ppo.main()
                weights = torch.load('train/policy.pt',weights_only=True)
                self.assertTrue(all(torch.isfinite(v).all() for v in weights.values()))
                self.assertTrue(any(not torch.equal(weights[k],v) for k,v in initial.items()))
                self.assertIn('完整批次 2 局',output.getvalue())
                self.assertIn('胜利 0 / 超时 2 / 全灭 0 / 升档中断 2',output.getvalue())
                self.assertIn('完成局均长 4.0步',output.getvalue())
                metadata = json.loads(Path('train/state.json').read_text(encoding='utf-8'))
                self.assertEqual(metadata['config']['reward_mode'], cfg.reward_mode)
                self.assertEqual(metadata['progress']['wins'] + metadata['progress']['timeouts'], 2)
                self.assertEqual(metadata['progress']['completed_steps'], 8)
                # Done at step 4, promotion at rollout boundary 6. GAE must see
                # the old curriculum's successor before the forced reset.
                self.assertEqual(events.count(('reset',6)),2)
                self.assertLess(events.index(('observe',6)),events.index(('reset',6)))
                report = evaluate('train/policy.pt', cfg, episodes=3, frac=.15)
                self.assertEqual(report['wins'] + report['timeouts'], 3)
                self.assertEqual([r['episode'] for r in report['rows']], [0, 1, 2])
                self.assertTrue(all(0 < r['steps'] <= 4 for r in report['rows']))
                # Evaluation must not change the saved checkpoint.
                again = torch.load('train/policy.pt', weights_only=True)
                self.assertTrue(all(torch.equal(weights[k], v) for k,v in again.items()))
                cfg.envs = 1
                serial = evaluate('train/policy.pt', cfg, episodes=3, frac=.15)
                self.assertEqual(report['rows'], serial['rows'])
            finally:
                os.chdir(previous)

    def test_victory_mode_real_training_and_evaluation(self):
        cfg = self.config()
        cfg.reward_mode = 'victory'
        cfg.win_reward = 1.0
        with patch.object(self, 'config', lambda: cfg):
            self.test_real_ppo_updates_and_bootstraps_before_promotion()


if __name__ == '__main__':
    unittest.main()
