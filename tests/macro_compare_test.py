import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'train'))
from macro_compare import compare


class MacroCompareTests(unittest.TestCase):
    def report(self):
        return dict(complete=True,simulation='native',stats_sha256='stats',maps={'map':'sha'},
            seeds=[1],max_wave=6,max_ticks=10000,mode='sample',policy_period=20,checkpoint_sha256='model',
            cases=[dict(arm='learned',map='map',seed=1,waves_survived=2,completed=False)])

    def test_pairs_and_rejects_incomplete_duplicate_or_mismatched_results(self):
        before=self.report();after=copy.deepcopy(before)
        after['cases'][0]['waves_survived']=3
        result=compare(before,after)
        self.assertEqual(result['improved'],1)
        self.assertEqual(result['after_mean_waves'],3)
        for mutate in (lambda r:r.update(complete=False),lambda r:r.update(policy_period=500),
                       lambda r:r['cases'].append(r['cases'][0]),lambda r:r.update(cases=[])):
            broken=copy.deepcopy(after);mutate(broken)
            with self.assertRaises(ValueError):
                compare(before,broken)


if __name__=='__main__':
    unittest.main()
