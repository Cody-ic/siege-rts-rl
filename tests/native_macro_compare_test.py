import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'train'))
from native_macro_compare import compare


class NativeCompareTests(unittest.TestCase):
    def test_complete_pairs_and_rejects_invalid_evidence(self):
        plan=dict(maps=['map'],seeds=[1],period=20,max_wave=6,max_ticks=10000)
        row=dict(map='map',seed=1,waves_survived=3,wave=4,tick=9000,completed=False,defeated=True,
                 timeout=False,policy_period=20,attacker_identity='same',defender_identity='model')
        before=dict(complete=True,arm='before',plan_sha256='plan',mode='native-game-categorical',cases=[row])
        after=copy.deepcopy(before);after['arm']='after';after['cases'][0].update(waves_survived=4,wave=5)
        self.assertEqual(compare(plan,before,after,'plan')['improved'],1)
        for mutate in (lambda r:r.update(complete=False),lambda r:r.update(plan_sha256='wrong'),
                       lambda r:r['cases'].append(r['cases'][0]),lambda r:r.update(cases=[]),
                       lambda r:r['cases'][0].update(attacker_identity='other'),
                       lambda r:r['cases'][0].update(timeout=True)):
            bad=copy.deepcopy(after);mutate(bad)
            with self.assertRaises(ValueError):compare(plan,before,bad,'plan')

if __name__=='__main__':unittest.main()
