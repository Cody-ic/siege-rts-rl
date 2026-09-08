"""Run the real three-leg experiment scheduler twice with tiny bounded budgets."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import rts_native as R

ROOT = Path(__file__).resolve().parents[1]


class SuiteTest(unittest.TestCase):
    def test_completed_stages_are_not_rerun_or_overwritten(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            env = dict(os.environ,PYTHONPATH=str(Path(R.__file__).parent),
                       PYTHONIOENCODING='utf-8',PYTHONUNBUFFERED='1')
            command = [sys.executable,'train/experiments.py','--run-root',folder,
                       '--milestones','4,8','--envs','2','--threads','1','--device','cpu',
                       '--episodes','1','--eval-envs','1','--max-ticks','6','--rollout','2']
            def run():
                result = subprocess.run(command,cwd=ROOT,env=env,capture_output=True,
                                        text=True,encoding='utf-8',timeout=120)
                if result.returncode:
                    logs = '\n'.join(p.read_text(encoding='utf-8')[-3000:] for p in root.glob('*/*.log'))
                    self.fail(result.stdout+result.stderr+logs)
            run()
            files = list(root.glob('*/latest.pt'))+list(root.glob('*/train.log'))+list(root.glob('*/eval-*.json'))
            self.assertEqual(len(list(root.glob('*/latest.pt'))),3)
            self.assertEqual(len(list(root.glob('*/best.pt'))),3)
            before = {str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
            run()
            self.assertEqual(before,{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in files})
            reports = json.loads((root/'summary.json').read_text(encoding='utf-8'))
            self.assertEqual(len(reports),12)
            self.assertEqual({r['steps'] for r in reports},{4,8})
            self.assertEqual({r['tag'] for r in reports},
                             {'economic-defender','victory-defender','economic-empty'})


if __name__=='__main__':
    unittest.main()
