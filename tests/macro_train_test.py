from pathlib import Path
import sys
import tempfile
import unittest

import torch

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'train'))
from macro_train import Config,train,returns
from checkpointing import load_auto


class MacroTrainTests(unittest.TestCase):
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


if __name__=='__main__':
    unittest.main()
