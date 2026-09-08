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
import demonstrations as demos
from checkpointing import (atomic_write, load_auto, load_training, restore_rng,
                           repair_metrics, rng_state, run_lock, save_run, sha256)
from evaluate import evaluate, flow_actions, breach_actions
from diagnostics import EpisodeDiagnostics, episode_rng, sample_actions
from rollout import advantages, RolloutStorage


class RuntimeTests(unittest.TestCase):
    def test_prepared_city_reset_and_full_combat_horizon(self):
        cfg=self.cfg(roster='mixed',defender_prepare_ticks=90,max_ticks=12)
        env=ppo.make_env(cfg,1,1.)
        initial=env.state_hash(0)
        obs=ppo.Observer(env).read()
        self.assertEqual(len(obs[2]),9)
        np.testing.assert_array_equal(obs[2][:,-2],np.ones(9,np.float32))
        actions=np.zeros((1,ppo.R.obs.MAX_UNITS_PER_ENV),np.uint8)
        done=np.zeros(1,np.uint8)
        env.step(actions,done)
        self.assertEqual(done[0],0) # preparation must not consume the episode limit
        env.step(actions,done)
        self.assertEqual(done[0],1)
        env.reset_one(0,ppo.make_worlds(cfg,1,1.)[0])
        self.assertEqual(env.state_hash(0),initial)
        cold=ppo.make_env(self.cfg(roster='mixed'),1,1.)
        self.assertNotEqual(cold.state_hash(0),initial)
        with self.assertRaises(ValueError):
            ppo.validate(self.cfg(defender=False,defender_prepare_ticks=90))

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

    def collect_demo(self, folder, *extra):
        with contextlib.redirect_stdout(io.StringIO()):
            demos.main(['collect','--out-dir',str(folder),'--envs','2','--threads','1',
                        '--steps','3','--stride','1','--fractions','0.15,0.3',*extra])
        return Path(folder)/'demonstrations.npz'

    def fit_demo(self, data, folder, *extra):
        with contextlib.redirect_stdout(io.StringIO()):
            return demos.main(['fit','--data',str(data),'--run-dir',str(folder),
                               '--epochs','2','--batch-size','32','--torch-threads','1',*extra])

    def test_detached_value_features_do_not_change_forward_or_actor_gradients(self):
        net = ppo.Policy(R.obs.K,R.obs.CHANNEL_COUNT,R.obs.SELF_COUNT,R.obs.GLOBAL_COUNT,R.obs.ACTION_COUNT)
        c = torch.randn(3,R.obs.CHANNEL_COUNT,R.obs.K,R.obs.K)
        s,g = torch.randn(3,R.obs.SELF_COUNT),torch.randn(3,R.obs.GLOBAL_COUNT)
        logits,value = net(c,s,g)
        detached_logits,detached_value = net(c,s,g,detach_value=True)
        torch.testing.assert_close(logits,detached_logits,rtol=0,atol=0)
        torch.testing.assert_close(value,detached_value,rtol=0,atol=0)
        detached_value.square().mean().backward()
        self.assertIsNotNone(net.critic.weight.grad)
        self.assertTrue(all(p.grad is None for m in (net.conv,net.trunk,net.actor) for p in m.parameters()))
        net.zero_grad(set_to_none=True)
        detached_logits[:,0].sum().backward()
        self.assertGreater(float(net.trunk[0].weight.grad.abs().sum()),0)
        self.assertGreater(float(net.actor.weight.grad.abs().sum()),0)

    def test_map_pool_defenders_match_separate_maps_before_and_after_reset(self):
        paths=tuple(str(ROOT/'game/data/maps/pool'/name) for name in ('gen_01001000.json','gen_01002000.json'))
        cfg=self.cfg(map_pool=paths,seed=3)
        worlds=ppo.make_worlds(cfg,2,1.)
        env=ppo.make_env(cfg,2,1.)
        def single(world,episode):
            return R.BatchedEnv([world],defender_map=ppo.episode_map(cfg,episode),
                defender_seed=cfg.seed*31+7,defender_macro_period=cfg.defender_macro_period,threads=1)
        singles=[single(world,i) for i,world in enumerate(worlds)]
        for step in range(80):
            if step==30:
                world=ppo.make_worlds(cfg,1,1.,7)[0]
                env.reset_one(0,world);singles[0]=single(world,7)
            combined=ppo.Observer(env).read()
            separate=[ppo.Observer(e).read() for e in singles]
            for field in range(1,5):
                np.testing.assert_array_equal(combined[field],np.concatenate([s[field] for s in separate]))
            env.step(np.zeros((2,R.obs.MAX_UNITS_PER_ENV),np.uint8),np.zeros(2,np.uint8))
            for one in singles:one.step(np.zeros((1,R.obs.MAX_UNITS_PER_ENV),np.uint8),np.zeros(1,np.uint8))

    def test_independent_value_updates_cannot_change_actor_or_its_rng(self):
        net=ppo.Policy(R.obs.K,R.obs.CHANNEL_COUNT,R.obs.SELF_COUNT,R.obs.GLOBAL_COUNT,R.obs.ACTION_COUNT)
        rng=torch.get_rng_state().clone()
        value_net=ppo.IndependentValue(net)
        self.assertTrue(torch.equal(rng,torch.get_rng_state()))
        c=torch.randn(3,R.obs.CHANNEL_COUNT,R.obs.K,R.obs.K)
        s=torch.randn(3,R.obs.SELF_COUNT);g=torch.randn(3,R.obs.GLOBAL_COUNT)
        actor_before,value_before=net(c,s,g)
        torch.testing.assert_close(value_net(c,s,g),value_before,rtol=0,atol=0)
        opt=torch.optim.Adam(value_net.parameters(),lr=0.001)
        opt.zero_grad();(value_net(c,s,g)-1000).square().mean().backward()
        torch.nn.utils.clip_grad_norm_(value_net.parameters(),.5);opt.step()
        torch.testing.assert_close(net(c,s,g)[0],actor_before,rtol=0,atol=0)
        self.assertTrue(all(p.grad is None for p in net.parameters()))
        self.assertFalse(torch.equal(value_net(c,s,g),value_before))

    def test_mixed_roster_full_health_and_map_spawn_level_cartesian_coverage(self):
        paths=tuple(str(ROOT/'game/data/maps/pool'/name) for name in ('gen_01001000.json','gen_01002000.json'))
        cfg=self.cfg(map_pool=paths,roster='mixed',levels=(1,4),seed=1)
        from types import SimpleNamespace
        def capture(path,stats):
            return SimpleNamespace(make=lambda **kw:dict(path=path,**kw)),R.map_sites(path)
        with patch.object(ppo,'world_factory',side_effect=capture):
            worlds=ppo.make_worlds(cfg,40,1.)
        for path in paths:
            actual={(tuple(w['attackers'][0][1:3]),w['nominal_level']) for w in worlds if w['path']==path}
            expected={((x-.5,y-.5),level) for x,y in R.map_sites(path)['spawns'] for level in (1,4)}
            self.assertEqual(actual,expected)
        # Start at a level-4 episode and inspect the actual native roster.
        env=ppo.make_env(cfg,1,1.,16)
        _,c,own,g,legal=ppo.Observer(env).read()
        self.assertEqual(len(own),9)
        self.assertEqual(set(own[:,:R.obs.SELF_COUNT-3].argmax(-1)),{5,6,7,8,10})
        np.testing.assert_array_equal(own[:,-2],np.ones(9,np.float32))
        self.assertTrue(np.all(own[:,-3]>1/1000))

    def test_mixed_pool_training_and_demonstrations_resume(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder=Path(temporary)
            pool=','.join(str(ROOT/'game/data/maps/pool'/name) for name in ('gen_01001000.json','gen_01002000.json'))
            self.run_train(folder/'ppo',['--map-pool',pool,'--roster','mixed','--levels','1,4',
                                        '--stop-after-updates','1'])
            self.run_train(folder/'ppo',['--resume','auto'])
            saved=load_training(folder/'ppo/latest.pt')
            self.assertEqual(saved['config']['roster'],'mixed')
            self.assertEqual(len(saved['contract']['map_pool_sha256']),2)
            data=self.collect_demo(folder/'data','--map-pool',pool,'--roster','mixed','--levels','1,4')
            self.fit_demo(data,folder/'fit')
            self.fit_demo(data,folder/'fit','--resume')

    def test_potential_shaping_telescopes_at_squad_death_and_world_end(self):
        # Squad 10 dies early; 20 survives compaction and the world ends later.
        keys=np.array([[[10,20]],[[20,0]],[[20,0]],[[0,0]]])
        gamma=.9
        phi=torch.tensor([-10.,-8.,-5.,0.],dtype=torch.float64)
        next_phi=phi[1:,None]
        rewards=(gamma*phi[1:]-phi[:-1])[:,None]
        dones=torch.tensor([[0.],[0.],[1.]])
        values=torch.zeros((3,2),dtype=torch.float64)
        result=advantages(values,rewards,dones,keys,torch.zeros(2),gamma,1.,next_phi)
        torch.testing.assert_close(result[0],torch.tensor([10.,10.],dtype=torch.float64))
        self.assertAlmostEqual(float(result[1,0]),8.)
        legacy=advantages(values,rewards,dones,keys,torch.zeros(2),gamma,1.)
        self.assertAlmostEqual(float(legacy[0,0]),2.8)
        # Economic rewards retain their original per-agent stopping semantics.
        task=torch.tensor([[3.],[4.],[5.]],dtype=torch.float64)
        combined=advantages(values,rewards+task,dones,keys,torch.zeros(2),gamma,1.,next_phi)
        unshaped=advantages(values,task,dones,keys,torch.zeros(2),gamma,1.)
        torch.testing.assert_close(combined-result,unshaped)

    def test_potential_boundary_is_preserved_for_live_rollout_bootstrap(self):
        keys=np.array([[[1]],[[1]]])
        args=(torch.zeros((1,1)),torch.tensor([[2.8]]),torch.zeros((1,1)),keys,
              torch.tensor([7.]),.9,.95)
        torch.testing.assert_close(advantages(*args,next_potential=torch.tensor([[-8.]])),advantages(*args))

    def test_reference_is_frozen_self_contained_and_restored_after_source_removal(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder=Path(temporary)
            net=ppo.Policy(R.obs.K,R.obs.CHANNEL_COUNT,R.obs.SELF_COUNT,R.obs.GLOBAL_COUNT,R.obs.ACTION_COUNT)
            source=folder/'initial.pt'
            torch.save(net.state_dict(),source)
            self.run_train(folder/'run',['--init-weights',str(source),'--reference-coef','1',
                                        '--value-features','detached','--stop-after-updates','1'])
            first=load_training(folder/'run/latest.pt')
            source.unlink()
            self.run_train(folder/'run',['--resume','auto'])
            last=load_training(folder/'run/latest.pt')
            for key,value in net.state_dict().items():
                torch.testing.assert_close(value,first['reference_model'][key],rtol=0,atol=0)
                torch.testing.assert_close(value,last['reference_model'][key],rtol=0,atol=0)
            self.assertTrue(any(not torch.equal(v,last['model'][k]) for k,v in net.state_dict().items()))
            rows=[json.loads(l) for l in (folder/'run/metrics.jsonl').read_text().splitlines()]
            self.assertTrue(all(np.isfinite(r['reference_kl']) for r in rows))
            last.pop('reference_model')
            save_run(folder/'run',last)
            with self.assertRaisesRegex(ValueError,'Missing reference'):
                self.run_train(folder/'run',['--resume','auto','--total-steps','18'])

    def test_reference_regularization_requires_explicit_source_and_valid_settings(self):
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaisesRegex(ValueError,'explicit --init-weights'):
                self.run_train(folder,['--reference-coef','1'])
            self.assertFalse((Path(folder)/'latest.pt').exists())
        for config in (self.cfg(reference_coef=float('nan')),self.cfg(reference_coef=-1),
                       self.cfg(value_features='typo')):
            with self.assertRaises(ValueError):ppo.validate(config)

    def test_demonstrations_reuse_completed_shards_and_reject_illegal_labels(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            data = self.collect_demo(folder)
            before = {p.name:sha256(p) for p in folder.glob('*.npz')}
            with patch.object(demos,'make_env',side_effect=AssertionError('must use cached shards')):
                self.collect_demo(folder)
                data.unlink()  # Simulate interruption before final dataset publication.
                self.collect_demo(folder)
            self.assertEqual(before,{p.name:sha256(p) for p in folder.glob('*.npz')})
            arrays, metadata = demos.load_dataset(data)
            self.assertTrue(np.all(arrays[3][np.arange(len(arrays[4])),arrays[4]]))
            with self.assertRaisesRegex(ValueError,'incomplete collection part'):
                self.fit_demo(folder/'part-0.npz',folder/'bad-fit')
            arrays[3][:] = False
            invalid = folder/'invalid.npz'
            demos.save_dataset(invalid,arrays,metadata)
            with self.assertRaisesRegex(ValueError,'illegal action'):
                demos.load_dataset(invalid)

    def test_student_collection_uses_student_actions_and_teacher_labels(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            net=ppo.Policy(R.obs.K,R.obs.CHANNEL_COUNT,R.obs.SELF_COUNT,R.obs.GLOBAL_COUNT,R.obs.ACTION_COUNT)
            with torch.no_grad():
                net.actor.weight.zero_();net.actor.bias.zero_();net.actor.bias[0]=100
            model=root/'student.pt';torch.save(net.state_dict(),model)
            captured=[];original=demos.make_env
            class Recorded:
                def __init__(self,env):self.env=env
                def __getattr__(self,name):return getattr(self.env,name)
                def step(self,actions,done):
                    captured.append(actions.copy());return self.env.step(actions,done)
            with patch.object(demos,'make_env',side_effect=lambda *a,**k:Recorded(original(*a,**k))):
                data=self.collect_demo(root/'data','--behavior-checkpoint',str(model),'--tactical-goals','split-economy')
            arrays,meta=demos.load_dataset(data)
            self.assertTrue(all(np.all(a==0) for a in captured))
            self.assertTrue(np.any(arrays[-1]!=0))
            self.assertEqual(meta['behavior_sha256'],sha256(model))
            self.fit_demo(data,root/'fit','--epochs','1','--init-weights',str(model))
            saved=demos.read_fit(root/'fit/latest.pt')
            torch.testing.assert_close(saved['model']['critic.weight'],net.state_dict()['critic.weight'],rtol=0,atol=0)
            self.fit_demo(data,root/'fit','--resume')
            self.assertEqual(demos.read_fit(root/'fit/latest.pt')['plan']['initialization_sha256'],sha256(model))
            with torch.no_grad():net.actor.bias[0]=99
            torch.save(net.state_dict(),model)
            with self.assertRaisesRegex(ValueError,'plan changed'):
                self.collect_demo(root/'data','--behavior-checkpoint',str(model),'--tactical-goals','split-economy')

    def test_demonstration_fit_resume_matches_uninterrupted_model_and_optimizer(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            data = self.collect_demo(folder/'data')
            self.fit_demo(data,folder/'full')
            self.fit_demo(data,folder/'resumed','--epochs','1')
            self.fit_demo(data,folder/'resumed','--resume')
            full = demos.read_fit(folder/'full/latest.pt')
            resumed = demos.read_fit(folder/'resumed/latest.pt')
            self.assertEqual(resumed['epoch'],2)
            self.assertEqual(full['history'],resumed['history'])
            for key,value in full['model'].items():
                torch.testing.assert_close(value,resumed['model'][key],rtol=0,atol=0)
            for key,state in full['optimizer']['state'].items():
                for field,value in state.items():
                    torch.testing.assert_close(value,resumed['optimizer']['state'][key][field],rtol=0,atol=0)
            checksum = sha256(folder/'resumed/latest.pt')
            self.fit_demo(data,folder/'resumed','--resume')
            self.assertEqual(checksum,sha256(folder/'resumed/latest.pt'))
            with self.assertRaisesRegex(ValueError,'changed'):
                self.fit_demo(data,folder/'resumed','--resume','--lr','0.01')

    def test_demonstration_fit_recovers_previous_and_exports_a_real_ppo_warm_start(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            data = self.collect_demo(folder/'data')
            self.fit_demo(data,folder/'fit')
            expected = demos.read_fit(folder/'fit/latest.pt')
            (folder/'fit/latest.pt').write_bytes(b'crash-truncated')
            with self.assertWarns(UserWarning):
                self.fit_demo(data,folder/'fit','--resume')
            actual = demos.read_fit(folder/'fit/latest.pt')
            for key,value in expected['model'].items():
                torch.testing.assert_close(value,actual['model'][key],rtol=0,atol=0)
            policy = folder/'fit/policy.pt'
            self.run_train(folder/'ppo',['--init-weights',str(policy),'--total-steps','6'])
            learned = load_training(folder/'ppo/latest.pt')
            self.assertEqual(learned['initialization']['sha256'],sha256(policy))
            self.assertEqual(learned['progress']['updates'],1)
            self.assertTrue(learned['optimizer']['state'])
            self.assertTrue(any(not torch.equal(v,learned['model'][k]) for k,v in actual['model'].items()))

    def test_demonstration_sigterm_saves_at_epoch_boundary(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            data = self.collect_demo(folder/'data')
            clip = torch.nn.utils.clip_grad_norm_
            sent = [False]
            def stop_once(*args,**kwargs):
                if not sent[0]:
                    sent[0] = True
                    signal.raise_signal(signal.SIGTERM)
                return clip(*args,**kwargs)
            with patch.object(torch.nn.utils,'clip_grad_norm_',side_effect=stop_once):
                self.assertEqual(self.fit_demo(data,folder/'fit'),128+signal.SIGTERM)
            self.assertEqual(demos.read_fit(folder/'fit/latest.pt')['epoch'],1)
            self.assertEqual(json.loads((folder/'fit/initialization.json').read_text())['status'],'paused')
            self.fit_demo(data,folder/'fit','--resume')
            self.assertEqual(demos.read_fit(folder/'fit/latest.pt')['epoch'],2)

    def test_breach_teacher_keeps_open_route_and_has_legal_fallbacks(self):
        cells = np.zeros((5,R.obs.K,R.obs.K,R.obs.CHANNEL_COUNT),np.float32)
        names = R.obs.ACTION_NAMES
        channel = [c[0] for c in R.obs.CHANNELS].index('flow_di')
        cells[:,R.obs.K//2,R.obs.K//2,channel] = 1
        cells[2] = 0  # No flow: retain an available attack, rather than stall.
        legal = np.zeros((5,R.obs.ACTION_COUNT),bool)
        stop, move, wall, bld, near = [names.index(n) for n in
                                     ('Stop','MoveSE','AtkWall','AtkBld','AtkNear')]
        legal[:,stop] = True
        legal[:3,move] = True
        legal[:3,wall] = True
        legal[1,bld] = True
        legal[3,near] = True  # Flow present but no legal movement.
        observation = (np.arange(5),cells,np.zeros((5,R.obs.SELF_COUNT),np.float32),
                       np.zeros((5,R.obs.GLOBAL_COUNT),np.float32),legal)
        before = tuple(a.copy() for a in observation)
        np.testing.assert_array_equal(flow_actions(observation),[wall,bld,wall,near,stop])
        chosen = breach_actions(observation)
        np.testing.assert_array_equal(chosen,[move,bld,wall,near,stop])
        self.assertEqual(chosen.dtype,np.uint8)
        self.assertTrue(np.all(legal[np.arange(5),chosen]))
        for old,new in zip(before,observation):
            np.testing.assert_array_equal(old,new)
        self.assertEqual(breach_actions(tuple(a[:0] for a in observation)).shape,(0,))

    def test_breach_demonstrations_record_teacher_and_reject_switch_on_resume(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            data = self.collect_demo(folder/'data','--teacher','flow_breach')
            arrays,metadata = demos.load_dataset(data)
            self.assertEqual(metadata['teacher'],'flow_breach')
            self.assertTrue(np.all(arrays[3][np.arange(len(arrays[4])),arrays[4]]))
            with self.assertRaisesRegex(ValueError,'plan changed'):
                self.collect_demo(folder/'data')
            self.fit_demo(data,folder/'fit','--epochs','1')
            self.fit_demo(data,folder/'fit','--resume')
            exported = json.loads((folder/'fit/initialization.json').read_text())
            self.assertEqual(exported['teacher'],'flow_breach')
            self.assertEqual(exported['epochs'],2)
            # Frozen rules are selectable without a model; learned actions must
            # never be silently replaced by either teacher.
            cfg = self.cfg(max_ticks=12)
            report = evaluate(None,cfg,2,.15,'flow_breach')
            self.assertEqual(report['policy'],'flow_breach')
            self.assertEqual(report['episodes'],2)
            with patch('evaluate.breach_actions',side_effect=AssertionError('teacher in inference')), \
                 patch('evaluate.flow_actions',side_effect=AssertionError('teacher in inference')):
                for mode in ('frozen_argmax','frozen_sample'):
                    learned = evaluate(folder/'fit/policy.pt',cfg,2,.15,mode)
                    self.assertEqual(learned['policy'],mode)

    def test_cuda_alias_resolves_current_device_and_preserves_explicit_index(self):
        class DeviceSelected(Exception):
            pass

        # Exercise the CLI startup with PyTorch's real device-index validation.
        # Stop before allocating GPU tensors, so this regression also runs on CPU CI.
        for device, expected in [('cuda', 1), ('cuda:2', 2)]:
            with self.subTest(device=device), tempfile.TemporaryDirectory() as folder:
                with patch.object(torch.cuda, 'is_available', return_value=True), \
                     patch.object(torch.cuda, 'current_device', return_value=1), \
                     patch.object(torch.cuda, 'set_device', side_effect=torch.cuda._get_device_index) as select, \
                     patch.object(torch.cuda, 'mem_get_info', side_effect=DeviceSelected):
                    with self.assertRaises(DeviceSelected):
                        self.run_train(folder, ['--device', device])
                    self.assertEqual(torch.cuda._get_device_index(select.call_args.args[0]), expected)

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

    def test_fresh_episode_restart_matches_uninterrupted_training(self):
        def equal(a,b):
            if isinstance(a,torch.Tensor):
                torch.testing.assert_close(a,b,rtol=0,atol=0)
            elif isinstance(a,dict):
                self.assertEqual(a.keys(),b.keys())
                for key in a:equal(a[key],b[key])
            elif isinstance(a,(list,tuple)):
                self.assertEqual(type(a),type(b));self.assertEqual(len(a),len(b))
                for x,y in zip(a,b):equal(x,y)
            else:self.assertEqual(a,b)
        for mode,features in (('keep','shared'),('known-economy','shared'),('split-economy','shared'),('split-economy','independent')):
            with self.subTest(mode=mode,features=features), tempfile.TemporaryDirectory() as folder:
                root=Path(folder)
                config=['--total-steps','24','--max-ticks','18','--curriculum','1.0',
                        '--tactical-goals',mode,'--value-features',features,'--roster','mixed','--levels','1,4',
                        '--map-pool','game/data/maps/pool/gen_01001000.json,game/data/maps/pool/gen_01004000.json']
                self.run_train(root/'control',config)
                self.run_train(root/'resume',[*config,'--stop-after-updates','2'])
                before=load_training(root/'resume/latest.pt')
                self.assertEqual(before['progress']['active_partial_episodes'],0)
                self.assertTrue(all(i is not None for i in before['progress']['fresh_episode_indices']))
                self.run_train(root/'resume',['--resume','auto','--total-steps','24'])
                a=load_training(root/'control/latest.pt');b=load_training(root/'resume/latest.pt')
                for key in ('model','optimizer','value_model','value_optimizer','rng','config','contract'):equal(a[key],b[key])
                for value in (a,b):value['progress'].pop('elapsed_seconds')
                equal(a['progress'],b['progress'])
                coverage=list(a['progress']['completed_coverage'].values())
                self.assertEqual(sum(x['completed'] for x in coverage),a['progress']['completed'])
                self.assertEqual(sum(x['decisions'] for x in coverage),a['progress']['completed_steps'])

    def test_completed_coverage_records_actual_levels_and_entrances(self):
        with tempfile.TemporaryDirectory() as folder:
            self.run_train(folder,['--total-steps','32','--max-ticks','6','--curriculum','1.0',
                '--roster','mixed','--levels','1,4',
                '--map-pool','game/data/maps/pool/gen_01001000.json,game/data/maps/pool/gen_01004000.json'])
            p=load_training(Path(folder)/'latest.pt')['progress']
            rows=list(p['completed_coverage'].values())
            self.assertEqual(p['completed'],32)
            self.assertEqual(len(rows),14) # maps have four and three entrances
            self.assertEqual({x['level'] for x in rows},{1,4})
            self.assertTrue(all(x['completed']>0 and x['decisions']==x['completed'] for x in rows))
            for path in {x['map'] for x in rows}:
                selected=[x for x in rows if x['map']==path]
                expected={(level,spawn) for level in (1,4) for spawn in range(len(R.map_sites(path)['spawns']))}
                self.assertEqual({(x['level'],x['spawn_index']) for x in selected},expected)
                self.assertEqual(sum(x['completed'] for x in selected),16)
            for name in ('completed','wins','timeouts','eliminated'):
                self.assertEqual(sum(x[name] for x in rows),p[name])
            metrics=[json.loads(line) for line in (Path(folder)/'metrics.jsonl').read_text().splitlines()]
            self.assertEqual(metrics[-1]['completed_coverage'],p['completed_coverage'])

    def test_missing_independent_critic_recovers_previous_generation(self):
        with tempfile.TemporaryDirectory() as folder:
            self.run_train(folder,['--value-features','independent','--stop-after-updates','1'])
            latest=Path(folder)/'latest.pt'
            state=load_training(latest);state.pop('value_model');torch.save(state,latest)
            with self.assertWarns(UserWarning):saved,path=load_auto(folder)
            self.assertEqual(path.name,'previous.pt')
            self.assertIsInstance(saved['value_model'],dict)
            self.assertEqual(saved['progress']['env_steps'],0)

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

    def test_sampled_evaluation_has_episode_rng_and_read_only_diagnostics(self):
        cfg = self.cfg(max_ticks=120)
        with tempfile.TemporaryDirectory() as folder:
            self.run_train(folder,['--stop-after-updates','1'])
            checkpoint = Path(folder)/'policy.pt'
            checksum = sha256(checkpoint)
            two = evaluate(checkpoint,cfg,3,.15,'frozen_sample',diagnostics=True,trace_every=7)
            cfg.envs = 1
            one = evaluate(checkpoint,cfg,3,.15,'frozen_sample',diagnostics=True,trace_every=7)
            plain = evaluate(checkpoint,cfg,3,.15,'frozen_sample')
            for batched, row, without_probe in zip(two['rows'],one['rows'],plain['rows']):
                # CPU GEMM batch shapes can change the last few probability bits;
                # actions, battle tallies and trace state must still match exactly.
                batched_probe = dict(batched['diagnostics'])
                single_probe = dict(row['diagnostics'])
                self.assertAlmostEqual(batched_probe.pop('mean_top_probability'),
                                       single_probe.pop('mean_top_probability'),places=6)
                self.assertEqual(batched_probe,single_probe)
                self.assertEqual({k:v for k,v in batched.items() if k!='diagnostics'},without_probe)
                probe = row['diagnostics']
                self.assertEqual({k:v for k,v in row.items() if k!='diagnostics'},without_probe)
                self.assertEqual(sum(probe['action_counts'].values()),probe['agent_decisions'])
                self.assertEqual(probe['trace'][-1]['step'],row['steps'])
                self.assertLessEqual(probe['attack_ignored'],probe['attack_available'])
                self.assertLessEqual(probe['moves_toward_flow']+probe['moves_against_flow'],probe['moves_with_flow'])
            self.assertEqual(sum(r['episodes'] for r in one['by_spawn']),3)
            self.assertEqual(sum(r['hit_buildings'] for r in one['by_spawn']),
                             sum(r['tally']['dmg_to_blds']>0 for r in one['rows']))
            self.assertEqual(checksum,sha256(checkpoint))

    def test_episode_sampling_never_selects_masked_actions(self):
        # Zeros include the first and last action, guarding CDF edge handling.
        probs = np.tile([0.,.25,0.,.75,0.],(6,1))
        rows = np.array([0,1,2,32,33,34])
        combined = sample_actions(probs,rows,32,[episode_rng(7,4),episode_rng(7,9)])
        separate = np.concatenate([sample_actions(probs[:3],rows[:3],32,[episode_rng(7,i)])
                                   for i in (4,9)])
        np.testing.assert_array_equal(combined,separate)
        self.assertTrue(np.all(np.isin(combined,[1,3])))

    def test_diagnostics_identifies_direction_and_ignored_attack(self):
        cells = np.zeros((3,R.obs.K,R.obs.K,R.obs.CHANNEL_COUNT),np.float32)
        channels = [c[0] for c in R.obs.CHANNELS]
        names = R.obs.ACTION_NAMES
        cells[:,R.obs.K//2,R.obs.K//2,channels.index('flow_di')] = 1
        legal = np.ones((3,R.obs.ACTION_COUNT),bool)
        actions = np.array([names.index('MoveSE'),names.index('MoveNW'),names.index('AtkBld')],np.uint8)
        probe = EpisodeDiagnostics(-100,5)
        probe.before_step(cells,legal,actions)
        totals = np.zeros(R.obs.TALLY_FIELDS)
        probe.after_step(1,totals,-90,3,False)
        totals[R.obs.TALLY_NAMES.index('dmg_to_blds')] = 12
        probe.after_step(2,totals,-85,3,True)
        result = probe.report()
        self.assertEqual(result['moves_toward_flow'],1)
        self.assertEqual(result['moves_against_flow'],1)
        self.assertEqual(result['attack_ignored'],2)
        self.assertEqual(result['first_building_contact_step'],2)

    def test_process_restart_after_forced_kill(self):
        # Actual process termination: latest is a previous completed update, never
        # an optimizer halfway through its next minibatch.
        with tempfile.TemporaryDirectory() as folder:
            self.run_train(folder,['--stop-after-updates','1'])
            # Windows venv python.exe can be a redirector which starts another
            # process. Kill the actual interpreter, not just that launcher.
            interpreter = sys._base_executable if os.name == 'nt' else sys.executable
            command = [interpreter,str(ROOT/'train/ppo.py'),'--run-dir',folder,
                       '--resume','auto','--total-steps','1000000']
            env = dict(os.environ,PYTHONPATH=os.pathsep.join([str(Path(R.__file__).parent),*sys.path]),
                       PYTHONUNBUFFERED='1',PYTHONIOENCODING='utf-8')
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
                    process.wait(timeout=30)
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
