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
            selected=evaluate(model,[map_path],stats,[103],Path(folder)/'learned-only.json',
                              max_wave=1,max_ticks=1,arms=['learned'])
            self.assertEqual([row['arm'] for row in selected['cases']],['learned'])
            # Teacher cadence follows the checkpoint unless explicitly overridden.
            # The deployed script still has its independent internal cadence.
            for period in (None,20):
                teacher=evaluate(model,[map_path],stats,[103],Path(folder)/f'teacher-{period}.json',
                                 max_wave=1,max_ticks=40,period=period,arms=['teacher'])
                cadence=cfg.period if period is None else period
                self.assertEqual(teacher['teacher'],dict(period=cadence,commands_per_decision=1,unit_orders=False))
                self.assertEqual(teacher['script_internal_period'],20)
                expected=evaluate_case(map_path,stats,103,'teacher',cadence,1,40)
                expected['arm']='teacher'
                self.assertEqual(teacher['cases'],[expected])
                self.assertTrue(teacher['cases'][0]['timeout'])
            self.assertEqual(before,sha256(model))
            with self.assertRaisesRegex(ValueError,'arms'):
                evaluate(model,[map_path],stats,[103],Path(folder)/'invalid.json',arms=[])
            with self.assertRaisesRegex(ValueError,'already exists'):
                evaluate(model,[map_path],stats,[103],output,max_wave=1,max_ticks=1)

    def test_script_and_teacher_report_reproducible_terminal_results(self):
        args=(str(ROOT/'game/data/maps/pool/gen_01009000.json'),
              str(ROOT/'game/data/stats_placeholder.json'),101,None,500,1,10000)
        a=evaluate_case(*args);b=evaluate_case(*args)
        self.assertTrue(a['completed'])
        self.assertFalse(a['timeout'])
        self.assertEqual(a,b)
        teacher_args=(*args[:3],'teacher',*args[4:])
        teacher=evaluate_case(*teacher_args)
        # The teacher emits only one command per 500 ticks. Stronger attackers
        # can defeat it; evaluation correctness must not require a teacher win.
        self.assertFalse(teacher['timeout'])
        self.assertNotEqual(teacher['completed'],teacher['defeated'])
        self.assertEqual(teacher['completed'],teacher['waves_survived']>=1)
        if teacher['defeated']:self.assertEqual(teacher['keep_hp_fraction'],0.)
        self.assertGreater(teacher['commands'].get('Build',0),0)
        self.assertEqual(teacher,evaluate_case(*teacher_args))


if __name__=='__main__':
    unittest.main()
