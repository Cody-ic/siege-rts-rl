from pathlib import Path
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'train'))
from macro_train import Config,train
from macro_evaluate import evaluate,evaluate_case
from checkpointing import sha256


class MacroEvaluateTests(unittest.TestCase):
    def test_timeout_is_not_success_and_frozen_model_stays_unchanged(self):
        map_path=str(ROOT/'game/data/maps/pool/gen_01009000.json')
        stats=str(ROOT/'game/data/stats_placeholder.json')
        with tempfile.TemporaryDirectory() as folder:
            cfg=Config((map_path,),stats,hidden=16)
            train(cfg,Path(folder)/'training',0)
            model=Path(folder)/'training/latest.pt'
            before=sha256(model)
            output=Path(folder)/'evaluation.json'
            result=evaluate(model,[map_path],stats,[103],output,max_wave=1,max_ticks=1)
            self.assertTrue(result['complete'])
            self.assertEqual(len(result['cases']),3)
            for row in result['cases']:
                self.assertTrue(row['timeout'])
                self.assertFalse(row['completed'])
                self.assertFalse(row['defeated'])
            self.assertEqual(before,sha256(model))
            with self.assertRaisesRegex(ValueError,'already exists'):
                evaluate(model,[map_path],stats,[103],output,max_wave=1,max_ticks=1)

    def test_script_completes_wave_with_reproducible_hash(self):
        args=(str(ROOT/'game/data/maps/pool/gen_01009000.json'),
              str(ROOT/'game/data/stats_placeholder.json'),101,None,500,1,10000)
        a=evaluate_case(*args);b=evaluate_case(*args)
        self.assertTrue(a['completed'])
        self.assertFalse(a['timeout'])
        self.assertEqual(a,b)


if __name__=='__main__':
    unittest.main()
