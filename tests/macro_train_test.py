from pathlib import Path
import sys
import tempfile
import unittest

import torch

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'train'))
from macro_train import Config,train,returns,reference_penalty,isolate_critic
from macro_policy import MacroPolicy
import numpy as np
from checkpointing import load_auto


class MacroTrainTests(unittest.TestCase):
    def test_value_only_update_does_not_change_actor_when_isolated(self):
        torch.manual_seed(8)
        policy=MacroPolicy(3,2,11,hidden=16)
        isolate_critic(policy)
        obs=(np.zeros((4,4,3),np.float32),np.zeros(2,np.float32))
        candidates=np.array([(0,65535,0,1),(1,0,1,1),(1,3,1,1)])
        before={name:value.clone() for name,value in policy.state_dict().items()}
        decision=policy.decide(*obs,candidates,(2,2),greedy=True)
        optimizer=torch.optim.Adam(policy.parameters(),lr=.01)
        (decision.value-1).square().backward();optimizer.step()
        after=policy.decide(*obs,candidates,(2,2),command=decision.command)
        torch.testing.assert_close(decision.log_prob,after.log_prob,rtol=0,atol=0)
        for name,value in policy.state_dict().items():
            if not name.startswith('critic.'):
                torch.testing.assert_close(value,before[name],rtol=0,atol=0)
        self.assertNotEqual(float(decision.value.detach()),float(after.value.detach()))
    def test_reference_estimator_matches_exact_joint_kl_value_and_gradient(self):
        logits=torch.tensor([-.2,.3,.7],requires_grad=True)
        logp=logits.log_softmax(0)
        old=torch.tensor([.3,.3,.4]);reference=torch.tensor([.4,.5,.1])
        estimated=(old*reference_penalty(logp,old.log(),reference.log())).sum()
        exact=(logp.exp()*(logp-reference.log())).sum()
        torch.testing.assert_close(estimated,exact)
        gradient=torch.autograd.grad(estimated,logits,retain_graph=True)[0]
        torch.testing.assert_close(gradient,torch.autograd.grad(exact,logits)[0])

    def test_fixed_reference_survives_exact_resume(self):
        cfg=self.config()
        cfg.detach_critic_features=True
        with tempfile.TemporaryDirectory() as folder:
            initial=Path(folder)/'initial'
            train(cfg,initial,0)
            origin,_=load_auto(initial)
            cfg.anchor_weight=1.
            full=Path(folder)/'full';split=Path(folder)/'split'
            train(cfg,full,2,initial/'latest.pt')
            train(cfg,split,1,initial/'latest.pt');train(cfg,split,2)
            a,_=load_auto(full);b,_=load_auto(split)
            self.assertEqual(a['progress'],b['progress'])
            self.assertEqual(a['campaign'],b['campaign'])
            for name,value in a['model'].items():
                torch.testing.assert_close(value,b['model'][name],rtol=0,atol=0)
                torch.testing.assert_close(a['reference_model'][name],origin['model'][name],rtol=0,atol=0)
                torch.testing.assert_close(b['reference_model'][name],origin['model'][name],rtol=0,atol=0)
    def config(self):
        return Config((str(ROOT/'game/data/maps/pool/gen_01001000.json'),),
                      str(ROOT/'game/data/stats_placeholder.json'),rollout=2,period=20,hidden=16,epochs=1)

    def test_committed_resume_matches_uninterrupted_model_and_campaign(self):
        cfg=self.config()
        with tempfile.TemporaryDirectory() as folder:
            full=Path(folder)/'full';split=Path(folder)/'split'
            train(cfg,full,2)
            train(cfg,split,1)
            train(cfg,split,2)
            a,_=load_auto(full);b,_=load_auto(split)
            self.assertEqual(a['progress'],b['progress'])
            self.assertEqual(a['campaign'],b['campaign'])
            torch.testing.assert_close(a['rng']['torch'],b['rng']['torch'],rtol=0,atol=0)
            for name,value in a['model'].items():
                torch.testing.assert_close(value,b['model'][name],rtol=0,atol=0)
            cfg.period+=1
            with self.assertRaisesRegex(ValueError,'inputs changed'):
                train(cfg,split,3)

    def test_wave_terminal_cuts_future_advantage(self):
        cfg=self.config()
        rows=[dict(ticks=20,terminal=True,reward=1,value=.2,next_value=999),
              dict(ticks=20,terminal=False,reward=0,value=.3,next_value=.4)]
        _,targets=returns(rows,cfg)
        self.assertAlmostEqual(float(targets[0]),1,places=6)
        self.assertAlmostEqual(float(targets[1]),cfg.gamma*.4,places=6)

    def test_long_horizon_preserves_delayed_credit_without_crossing_wave_boundary(self):
        cfg=self.config();cfg.gamma=.999;cfg.gae_lambda=.99
        rows=[dict(ticks=20,terminal=False,reward=0,value=0,next_value=0) for _ in range(120)]
        rows[-1].update(terminal=True,reward=1)
        _,targets=returns(rows,cfg)
        self.assertAlmostEqual(float(targets[0]),(.999*.99)**119,places=6)
        self.assertGreater(float(targets[0]),.25)
        rows[60]['terminal']=True
        _,cut=returns(rows,cfg)
        self.assertEqual(float(cut[0]),0)


if __name__=='__main__':
    unittest.main()
