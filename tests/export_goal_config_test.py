import json
from pathlib import Path
import sys
import tempfile
import unittest
import numpy as np

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'train'))
from checkpointing import sha256
from export_policy import checkpoint_config


class GoalConfigTests(unittest.TestCase):
    def test_imitation_recovers_split_semantics_only_from_exact_dataset(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'data.npz'
            metadata=dict(config={'tactical_goals':'split-economy','defender_prepare_ticks':900},signature={'test':'contract'})
            np.savez(path,metadata=np.array(json.dumps(metadata)))
            source=dict(format='rts-demonstrations-1',plan=dict(dataset_sha256=sha256(path),signature=metadata['signature']))
            self.assertEqual(checkpoint_config(source,path),metadata['config'])
            with self.assertRaisesRegex(ValueError,'requires'):checkpoint_config(source)
            source['plan']['signature']={'different':'contract'}
            with self.assertRaisesRegex(ValueError,'signature'):checkpoint_config(source,path)
            source['plan']['dataset_sha256']='changed'
            with self.assertRaisesRegex(ValueError,'do not match'):checkpoint_config(source,path)

    def test_ppo_and_legacy_cannot_import_unrelated_dataset_semantics(self):
        source=dict(format=1,config={'tactical_goals':'known-economy'})
        self.assertEqual(checkpoint_config(source),source['config'])
        self.assertEqual(checkpoint_config({'actor.weight':'legacy'}),{})
        with self.assertRaisesRegex(ValueError,'only for'):checkpoint_config(source,'unused.npz')


if __name__=='__main__':unittest.main()
