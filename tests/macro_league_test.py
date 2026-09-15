from pathlib import Path
import sys
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'train'))
from macro_league import OpponentPool


class PoolTests(unittest.TestCase):
    def test_every_map_meets_every_opponent_before_repeating(self):
        with patch('macro_league.load_opponent',side_effect=lambda p,s:(p,dict(kind=p))):
            pool=OpponentPool(('', 'old.onnx', 'new.onnx'),'stats')
        cases=[(i%4,pool.index(i,4),pool.select(i,4)) for i in range(24)]
        self.assertEqual(len({(m,o) for m,o,_ in cases[:12]}),12)
        self.assertEqual(cases[:12],cases[12:])
        self.assertEqual([o for _,o,_ in cases[:12]],[0]*4+[1]*4+[2]*4)
        with self.assertRaises(ValueError):pool.index(-1,4)
        with self.assertRaises(ValueError):pool.index(0,0)

    def test_every_member_is_checked_including_inactive_opponent(self):
        with patch('macro_league.load_opponent',side_effect=lambda p,s:(p,dict(kind=p))):
            pool=OpponentPool(('', 'old.onnx'),'stats')
        with patch('macro_league.check_opponent') as check:
            pool.check()
            self.assertEqual([c.args[0] for c in check.call_args_list],['','old.onnx'])
        with patch('macro_league.check_opponent',side_effect=ValueError('changed')):
            with self.assertRaisesRegex(ValueError,'changed'):pool.check()

    def test_invalid_pool_rejected_before_loading(self):
        for paths in ((),('', ''),('same.onnx','same.onnx')):
            with self.assertRaises(ValueError):OpponentPool(paths,'stats')


if __name__=='__main__':unittest.main()
