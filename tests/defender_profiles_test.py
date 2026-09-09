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


class ProfileTests(unittest.TestCase):
    def setUp(self):torch.set_num_threads(1)
    def config(self):
        return ppo.Cfg(envs=6,threads=2,roster='mixed',levels=(1,),
            map_pool=tuple(str(ROOT/'game/data/maps/pool'/f'gen_{m}.json') for m in ('01001000','01004000')),
            defender_prepare_ticks=900,defender_profiles=('balanced','fortified','mobile'))

    def test_profile_assignment_survives_batching_and_reset(self):
        cfg=self.config();batch=ppo.make_env(cfg,6,1.)
        singles=[]
        for i in range(6):
            selected=cfg.defender_profiles[((cfg.seed*1000+i)//2)%3]
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
            self.assertEqual({r['defender_profile'] for r in rows},set(cfg.defender_profiles))
            self.assertEqual(sum(r['completed'] for r in rows),a['progress']['completed'])
            with self.assertRaises(ValueError):run(resumed,['--resume','auto','--defender-profiles','mobile,balanced'])


if __name__=='__main__':unittest.main()
