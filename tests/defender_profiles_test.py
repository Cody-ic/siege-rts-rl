import contextlib
import copy
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import numpy as np
import torch
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'train'))
import ppo
from checkpointing import load_training
from evaluate import evaluate


class ProfileTests(unittest.TestCase):
    def setUp(self):torch.set_num_threads(1)
    def config(self):
        return ppo.Cfg(envs=6,threads=2,roster='mixed',levels=(1,),
            map_pool=tuple(str(ROOT/'game/data/maps/pool'/f'gen_{m}.json') for m in ('01001000','01004000')),
            defender_prepare_ticks=900,defender_profiles=('balanced','fortified','mobile'))

    def test_profiles_do_not_lock_to_entrance_or_level_cycles(self):
        cfg=self.config();cfg.levels=(1,4,8,16)
        cfg.map_pool=tuple(str(ROOT/'game/data/maps/pool'/f'gen_{m}.json') for m in ('01001000','01004000','01005000','01006001'))
        expected={(p,lv,spawn,profile) for p in cfg.map_pool for lv in cfg.levels
                  for spawn in range(len(ppo.R.map_sites(p)['spawns'])) for profile in range(3)}
        actual={(*ppo.episode_spec(cfg,i),ppo.R.defender_profile_index(cfg.seed*1000+i,4,3)) for i in range(3072)}
        self.assertEqual(actual,expected)
        for seed in range(1000,1100,4):
            self.assertEqual(len({ppo.R.defender_profile_index(seed+i,4,3) for i in range(4)}),1)
        with self.assertRaises(Exception):ppo.R.defender_profile_index(1,0,3)

    def test_profile_assignment_survives_batching_and_reset(self):
        cfg=self.config();batch=ppo.make_env(cfg,6,1.)
        singles=[]
        for i in range(6):
            selected=cfg.defender_profiles[ppo.R.defender_profile_index(cfg.seed*1000+i,2,3)]
            single_cfg=copy.copy(cfg);single_cfg.defender_profiles=(selected,)
            single=ppo.make_env(single_cfg,1,1.,start=i)
            singles.append(single)
            self.assertEqual(batch.state_hash(i),single.state_hash(0))
        actions=np.zeros((6,ppo.R.obs.MAX_UNITS_PER_ENV),np.uint8)
        done=np.zeros(6,np.uint8);one_done=np.zeros(1,np.uint8)
        for _ in range(200):
            batch.step(actions,done)
            for i,single in enumerate(singles):
                single.step(actions[:1],one_done)
                self.assertEqual(batch.state_hash(i),single.state_hash(0))
        batch.reset_one(0,ppo.make_worlds(cfg,1,1.,start=9)[0])
        fresh=ppo.make_env(cfg,1,1.,start=9)
        for _ in range(6):
            batch.step(actions,done);fresh.step(actions[:1],one_done)
            self.assertEqual(batch.state_hash(0),fresh.state_hash(0))

    def test_profiles_change_real_city_and_balanced_preserves_default(self):
        cfg=self.config();states={}
        for profile in ((),('balanced',),('fortified',),('mobile',)):
            cfg.defender_profiles=profile
            env=ppo.make_env(cfg,1,1.)
            # Initial construction priorities consume the same resources. Allow
            # the real economy and combat to reach discretionary recruitment.
            actions=np.zeros((1,ppo.R.obs.MAX_UNITS_PER_ENV),np.uint8)
            done=np.zeros(1,np.uint8)
            for _ in range(200):env.step(actions,done)
            states[profile]=env.state_hash(0)
        self.assertEqual(states[()],states[('balanced',)])
        self.assertEqual(len(set(states.values())),3)
        cfg.defender_profiles=('unknown',)
        with self.assertRaises(ValueError):ppo.validate(cfg)
        with self.assertRaises(Exception):ppo.make_env(cfg,1,1.)

    def test_real_training_resume_and_profile_coverage(self):
        def run(folder,extra):
            args=['ppo','--run-dir',str(folder),'--device','cpu','--envs','2','--threads','1',
                  '--rollout','3','--epochs','1','--minibatches','2',*extra]
            with patch.object(sys,'argv',args),contextlib.redirect_stdout(io.StringIO()):ppo.main()
        def equal(a,b):
            if isinstance(a,torch.Tensor):torch.testing.assert_close(a,b,rtol=0,atol=0)
            elif isinstance(a,dict):
                self.assertEqual(a.keys(),b.keys())
                for k in a:equal(a[k],b[k])
            elif isinstance(a,(list,tuple)):
                self.assertEqual(len(a),len(b))
                for x,y in zip(a,b):equal(x,y)
            else:self.assertEqual(a,b)
        cfg=self.config()
        args=['--total-steps','24','--max-ticks','18','--curriculum','1.0',
              '--roster','mixed','--levels','1,4','--map-pool',','.join(cfg.map_pool),
              '--defender-profiles',','.join(cfg.defender_profiles),'--defender-prepare-ticks','90']
        with tempfile.TemporaryDirectory() as directory:
            full=Path(directory)/'full';resumed=Path(directory)/'resumed'
            run(full,args);run(resumed,[*args,'--stop-after-updates','2'])
            run(resumed,['--resume','auto','--total-steps','24'])
            a=load_training(full/'latest.pt');b=load_training(resumed/'latest.pt')
            for k in ('model','optimizer','rng','config','contract'):equal(a[k],b[k])
            for r in (a,b):r['progress'].pop('elapsed_seconds')
            equal(a['progress'],b['progress'])
            rows=list(a['progress']['completed_coverage'].values())
            expected_profiles={cfg.defender_profiles[ppo.R.defender_profile_index(cfg.seed*1000+i,2,3)]
                               for i in range(a['progress']['completed'])}
            self.assertEqual({r['defender_profile'] for r in rows},expected_profiles)
            self.assertEqual(sum(r['completed'] for r in rows),a['progress']['completed'])
            with self.assertRaises(ValueError):run(resumed,['--resume','auto','--defender-profiles','mobile,balanced'])

    def test_frozen_evaluation_records_the_style_that_defended(self):
        # A single-style evaluation must face only that style, and a multi-style
        # one must label each row with the selector the native side actually used.
        cfg=self.config();cfg.map_pool=();cfg.map_path=str(ROOT/'game/data/maps/pool/gen_01001000.json')
        cfg.envs=2;cfg.max_ticks=18;cfg.defender_prepare_ticks=90
        cfg.defender_profiles=('fortified',)
        single=evaluate(None,cfg,3,1.,'flow')
        self.assertEqual(single['defender_profiles'],['fortified'])
        self.assertEqual([r['defender_profile'] for r in single['rows']],['fortified']*3)
        self.assertEqual([(s['defender_profile'],s['episodes']) for s in single['by_profile']],[('fortified',3)])
        cfg.defender_profiles=('balanced','fortified','mobile')
        mixed=evaluate(None,cfg,6,1.,'flow')
        expected=[cfg.defender_profiles[ppo.R.defender_profile_index(cfg.seed*1000+i,1,3)] for i in range(6)]
        self.assertEqual([r['defender_profile'] for r in mixed['rows']],expected)
        self.assertEqual(sum(s['episodes'] for s in mixed['by_profile']),6)
        cfg.defender_profiles=()
        plain=evaluate(None,cfg,2,1.,'flow')
        self.assertEqual(plain['by_profile'],[])
        self.assertNotIn('defender_profile',plain['rows'][0])


if __name__=='__main__':unittest.main()
