"""Real CLI regression for explicit full-distance prepared environments."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

class PreparedCliTests(unittest.TestCase):
    def run_cli(self,*args):
        return subprocess.run([sys.executable,*args],cwd=ROOT,capture_output=True,text=True,encoding='utf-8',timeout=90)

    def test_collect_uses_requested_distance(self):
        with tempfile.TemporaryDirectory() as folder:
            out=Path(folder)/'data'
            result=self.run_cli('train/demonstrations.py','collect','--out-dir',str(out),
                '--envs','1','--threads','1','--steps','1','--fractions','1','--defender-prepare-ticks','1')
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertEqual(json.loads((out/'plan.json').read_text())['config']['curriculum'],[1.0])

    def test_evaluation_uses_requested_distance(self):
        with tempfile.TemporaryDirectory() as folder:
            out=Path(folder)/'report.json'
            args=['train/evaluate.py','--output',str(out),'--policy','flow','--episodes','1',
                  '--envs','1','--threads','1','--max-ticks','6','--defender-prepare-ticks','1']
            result=self.run_cli(*args,'--frac','1')
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertEqual(json.loads(out.read_text())['episodes'],1)
            result=self.run_cli(*args,'--frac','.5')
            self.assertNotEqual(result.returncode,0)
            self.assertIn('requires curriculum=(1.0,)',result.stderr)

if __name__=='__main__':unittest.main()
