from pathlib import Path
import sys
import tempfile
import unittest

import torch

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'train'))
from macro_train import Config,train
from macro_imitation import run,sample_epoch
from checkpointing import load_auto,sha256


class MacroImitationTests(unittest.TestCase):
    def test_resampling_gives_active_commands_the_requested_mass(self):
        rows=[{'command':(0,65535,0,1)} for _ in range(900)]+[{'command':(1,0,0,1)} for _ in range(100)]
        torch.manual_seed(9)
        selected=sample_epoch(rows,.5)
        fraction=sum(index>=900 for index in selected)/len(selected)
        self.assertGreater(fraction,.44);self.assertLess(fraction,.56)

    def test_epoch_resume_and_ppo_handoff_preserve_origin(self):
        cfg=Config((str(ROOT/'game/data/maps/pool/gen_01001000.json'),),
            str(ROOT/'game/data/stats_placeholder.json'),period=20,hidden=16,rollout=2,epochs=1)
        with tempfile.TemporaryDirectory() as directory:
            full=Path(directory)/'full';split=Path(directory)/'split'
            run(cfg,full,4,2,.5)
            run(cfg,split,4,1,.5);run(cfg,split,4,2,.5)
            a,_=load_auto(full);b,_=load_auto(split)
            for name,value in a['model'].items():
                torch.testing.assert_close(value,b['model'][name],rtol=0,atol=0)
            origin=b['initialization']
            train(cfg,split,1)
            trained,_=load_auto(split)
            self.assertEqual(trained['initialization'],origin)
            before=sha256(split/'latest.pt')
            with self.assertRaisesRegex(ValueError,'Refusing to overwrite'):
                run(cfg,split,4,3)
            self.assertEqual(before,sha256(split/'latest.pt'))


if __name__=='__main__':
    unittest.main()
