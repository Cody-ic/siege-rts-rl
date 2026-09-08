"""Real native + CPU PyTorch tests. Run with bindings on PYTHONPATH."""
import contextlib
import io
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import numpy as np
import torch
import rts_native as R

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'train'))
import ppo
from checkpointing import (atomic_write, load_auto, load_training, restore_rng,
                           repair_metrics, rng_state, run_lock, save_run, sha256)
from evaluate import evaluate, flow_actions
from rollout import advantages, RolloutStorage


class RuntimeTests(unittest.TestCase):
    def setUp(self):
        torch.set_num_threads(1)

    def cfg(self, **kw):
        return ppo.Cfg(envs=2, threads=1, rollout=3, epochs=1, minibatches=2,
                       total_steps=12, device='cpu',
                       map_path=str(ROOT/'game/data/maps/pool/gen_01001000.json'),
                       stats_path=str(ROOT/'game/data/stats_placeholder.json'), **kw)

    def run_train(self, folder, extra=()):
        args = ['ppo','--run-dir',str(folder),'--device','cpu','--envs','2',
                '--threads','1','--rollout','3','--epochs','1','--minibatches','2',
                '--total-steps','12',*extra]
        with patch.object(sys,'argv',args), contextlib.redirect_stdout(io.StringIO()):
            return ppo.main()

    def test_resume_keeps_optimizer_curriculum_and_counts_interruptions(self):
        with tempfile.TemporaryDirectory() as folder:
            self.run_train(folder, ['--stop-after-updates','1'])
            before = load_training(Path(folder)/'latest.pt')
            self.assertEqual(before['progress']['env_steps'],6)
            self.assertTrue(before['optimizer']['state'])
            self.run_train(folder, ['--resume','auto'])
            after = load_training(Path(folder)/'latest.pt')
            self.assertEqual(after['progress']['env_steps'],12)
            self.assertEqual(after['progress']['updates'],2)
            self.assertEqual(after['progress']['resume_resets'],2)
            self.assertGreater(after['progress']['next_episode'], before['progress']['next_episode'])
            self.assertEqual(after['status'],'complete')
            for key, old in before['optimizer']['state'].items():
                self.assertGreater(after['optimizer']['state'][key]['step'], old['step'])
            self.assertTrue(any(not torch.equal(v,after['model'][k]) for k,v in before['model'].items()))
            with self.assertRaisesRegex(ValueError,'Cannot change reward_mode'):
                self.run_train(folder,['--resume','auto','--reward-mode','victory'])
            # Completion is idempotent; it must not save a falsely incremented state.
            checksum = sha256(Path(folder)/'latest.pt')
            self.run_train(folder,['--resume','auto'])
            self.assertEqual(checksum,sha256(Path(folder)/'latest.pt'))

    def test_atomic_save_and_corrupt_latest_fallback(self):
        with tempfile.TemporaryDirectory() as folder:
            self.run_train(folder,['--stop-after-updates','1'])
            path = Path(folder)/'latest.pt'
            checksum = sha256(path)
            payload = load_training(path)
            def fail(stream):
                stream.write(b'partial-write')
                raise OSError('simulated disk failure')
            with self.assertRaises(OSError):
                atomic_write(path,fail)
            self.assertEqual(checksum,sha256(path))
            path.write_bytes(b'truncated')
            with self.assertWarns(UserWarning):
                recovered, source = load_auto(folder)
            self.assertEqual(source.name,'previous.pt')
            self.assertEqual(recovered['progress']['env_steps'],0)
            with self.assertWarns(UserWarning):
                save_run(folder,payload)
            self.assertEqual(load_training(path)['progress']['env_steps'],6)
            self.assertEqual(load_training(Path(folder)/'previous.pt')['progress']['env_steps'],0)

    def test_no_silent_overwrite_and_lock_release(self):
        with tempfile.TemporaryDirectory() as folder:
            with run_lock(folder):
                with self.assertRaises(RuntimeError):
                    with run_lock(folder):
                        pass
            with run_lock(folder):
                pass
            (Path(folder)/'state.json').write_text('{}')
            with self.assertRaisesRegex(ValueError,'no checkpoint'):
                load_auto(folder)

    def test_rng_roundtrip(self):
        state = rng_state()
        expected = (np.random.random(8),torch.rand(8))
        restore_rng(state)
        np.testing.assert_array_equal(expected[0],np.random.random(8))
        self.assertTrue(torch.equal(expected[1],torch.rand(8)))

    def test_torn_metrics_and_uncommitted_updates_are_removed(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'metrics.jsonl'
            path.write_bytes(b'{"env_steps":6}\n{"env_steps":12}\n{"env_steps":')
            repair_metrics(folder,6)
            self.assertEqual(path.read_bytes(),b'{"env_steps":6}\n')
            path.write_bytes(b'{"env_steps":6}')
            repair_metrics(folder,6)
            self.assertEqual(path.read_bytes(),b'{"env_steps":6}\n')

    def test_completion_window_roundtrip_preserves_promotion_decision(self):
        from learning import CompletionWindow
        before = CompletionWindow(3)
        before.add([1,0])
        before.add([1,1])
        after = CompletionWindow(3)
        after.load_state_dict(before.state_dict())
        self.assertEqual(after.ready,before.ready)
        self.assertEqual(after.rate,before.rate)
        after.add([0,0])
        before.add([0,0])
        self.assertEqual(after.state_dict(),before.state_dict())

    def test_checkpoint_after_completed_native_episodes_is_weights_only_safe(self):
        with tempfile.TemporaryDirectory() as folder:
            self.run_train(folder,['--max-ticks','12','--stop-after-updates','1'])
            saved = load_training(Path(folder)/'latest.pt')
            self.assertGreater(saved['progress']['completed'],0)
            self.assertTrue(saved['progress']['stage_hits']['batches'])
            self.run_train(folder,['--resume','auto','--max-ticks','12'])

    def test_old_weights_are_warm_start_only(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            self.run_train(root/'source',['--stop-after-updates','1'])
            weights = root/'source/policy.pt'
            with self.assertRaisesRegex(ValueError,'policy-only'):
                load_training(weights)
            self.run_train(root/'warm',['--init-weights',str(weights),'--stop-after-updates','1'])
            warm = load_training(root/'warm/latest.pt')
            self.assertEqual(warm['progress']['env_steps'],6)
            self.assertEqual(warm['progress']['resume_resets'],0)
            self.assertEqual(warm['initialization']['sha256'],sha256(weights))
            self.assertTrue(all(torch.isfinite(v).all() for v in warm['model'].values()))

    def test_identity_gae_never_bootstraps_from_different_squad(self):
        values = torch.tensor([[10.,20.,30.]])
        keys = np.array([[[1,2,3]],[[1,3,0]]])
        result = advantages(values,torch.tensor([[1.]]),torch.zeros(1,1), keys,
                            torch.tensor([11.,33.,0.]),1,1)
        torch.testing.assert_close(result,torch.tensor([[2.,-19.,4.]]))
        terminal = advantages(values,torch.tensor([[1.]]),torch.ones(1,1),keys,
                              torch.tensor([11.,33.,0.]),1,1)
        torch.testing.assert_close(terminal,torch.tensor([[-9.,-19.,-29.]]))

    def test_live_forward_matches_dense_and_storage_reduction(self):
        cfg = self.cfg()
        env = ppo.make_env(cfg,2,.15)
        obs = ppo.Observer(env)
        current = obs.read()
        net = ppo.Policy(R.obs.K,R.obs.CHANNEL_COUNT,R.obs.SELF_COUNT,
                         R.obs.GLOBAL_COUNT,R.obs.ACTION_COUNT)
        with torch.no_grad():
            dist, value = ppo.policy_forward(net,current,'cpu')
            logits, dense_value = net(torch.from_numpy(obs.cells.reshape(-1,obs.k,obs.k,obs.c)).permute(0,3,1,2),
                                       torch.from_numpy(obs.own.reshape(-1,R.obs.SELF_COUNT)),
                                       torch.from_numpy(np.repeat(obs.glob,obs.mu,axis=0)))
        rows, _,_,_, legal = current
        dense = torch.distributions.Categorical(logits=logits[rows].masked_fill(~torch.from_numpy(legal),float('-inf')))
        torch.testing.assert_close(dist.probs,dense.probs)
        torch.testing.assert_close(value,dense_value[rows])
        small = RolloutStorage(3,len(rows),obs.k,obs.c,R.obs.SELF_COUNT,R.obs.GLOBAL_COUNT,R.obs.ACTION_COUNT)
        large = RolloutStorage(3,2*obs.mu,obs.k,obs.c,R.obs.SELF_COUNT,R.obs.GLOBAL_COUNT,R.obs.ACTION_COUNT)
        self.assertAlmostEqual(small.bytes/large.bytes,9/32)
        actions = flow_actions(current)
        self.assertTrue(np.all(legal[np.arange(len(rows)),actions]))

    def test_defender_resets_and_episode_seed_does_not_depend_on_batch_slot(self):
        cfg = self.cfg()
        batch = ppo.make_env(cfg,2,.15)
        actions = np.zeros((2,R.obs.MAX_UNITS_PER_ENV),np.uint8)
        done = np.zeros(2,np.uint8)
        for _ in range(75):
            batch.step(actions,done)
        # Reuse a slot with a populated macro brain, compare to a fresh environment.
        batch.reset_one(1,ppo.make_worlds(cfg,1,.15,start=13)[0])
        fresh = ppo.make_env(cfg,1,.15,start=13)
        fresh_done = np.zeros(1,np.uint8)
        for _ in range(75):
            batch.step(actions,done)
            fresh.step(actions[:1],fresh_done)
            self.assertEqual(batch.state_hash(1),fresh.state_hash(0))

    def test_frozen_evaluation_is_batch_independent_with_real_defender(self):
        cfg = self.cfg(max_ticks=240)
        with tempfile.TemporaryDirectory() as folder:
            self.run_train(folder,['--stop-after-updates','1'])
            checkpoint = Path(folder)/'policy.pt'
            checksum = sha256(checkpoint)
            two = evaluate(checkpoint,cfg,3,.15)
            cfg.envs = 1
            one = evaluate(checkpoint,cfg,3,.15)
            self.assertEqual(two['rows'],one['rows'])
            self.assertEqual(two['wins']+two['timeouts']+two['eliminated'],3)
            self.assertEqual(checksum,sha256(checkpoint))

    def test_process_restart_after_forced_kill(self):
        # Actual process termination: latest is a previous completed update, never
        # an optimizer halfway through its next minibatch.
        with tempfile.TemporaryDirectory() as folder:
            self.run_train(folder,['--stop-after-updates','1'])
            command = [sys.executable,str(ROOT/'train/ppo.py'),'--run-dir',folder,
                       '--resume','auto','--total-steps','1000000']
            env = dict(os.environ,PYTHONPATH=str(Path(R.__file__).parent),PYTHONUNBUFFERED='1',PYTHONIOENCODING='utf-8')
            process = subprocess.Popen(command,cwd=ROOT,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,encoding='utf-8')
            killed_after_update = False
            try:
                # Synchronize on the first actual update's output, not a timed guess.
                for line in process.stdout:
                    if line.startswith('step '):
                        killed_after_update = True
                        process.kill()
                        break
                process.wait(timeout=30)
            finally:
                if process.poll() is None:
                    process.kill()
                process.stdout.close()
            self.assertTrue(killed_after_update,'child must reach a real training update before it is killed')
            saved,_ = load_auto(folder)
            self.assertGreaterEqual(saved['progress']['env_steps'],6)
            target = saved['progress']['env_steps']+6
            self.run_train(folder,['--resume','auto','--total-steps',str(target)])
            self.assertEqual(load_training(Path(folder)/'latest.pt')['progress']['env_steps'],target)

    def test_sigterm_saves_a_completed_update_before_exit(self):
        real_env = R.BatchedEnv
        class StopDuringSample:
            def __init__(self,*a,**kw):
                self.env = real_env(*a,**kw)
                self.sent = False
            def __getattr__(self,name):
                return getattr(self.env,name)
            def step(self,*a):
                self.env.step(*a)
                if not self.sent:
                    self.sent = True
                    signal.raise_signal(signal.SIGTERM)
        with tempfile.TemporaryDirectory() as folder, patch.object(R,'BatchedEnv',StopDuringSample):
            result = self.run_train(folder)
            self.assertEqual(result,128+signal.SIGTERM)
            saved = load_training(Path(folder)/'latest.pt')
            self.assertEqual(saved['status'],'paused')
            self.assertEqual(saved['progress']['env_steps'],6)
            self.assertEqual(saved['progress']['updates'],1)


if __name__=='__main__':
    unittest.main()
