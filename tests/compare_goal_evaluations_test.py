import copy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'train'))
from compare_goal_evaluations import compare


class ComparisonTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)
        names=['baseline','shared','detached']
        models={n:str(self.root/f'{n}.model') for n in names}
        for name,path in models.items():Path(path).write_text(name)
        self.plan=dict(arms=names,levels=[1],seed=1,envs=2,maps=['example'],prepare_ticks=900,
                       max_ticks=2400,selection='frozen_argmax',models=models,
                       files={p:hashlib.sha256(Path(p).read_bytes()).hexdigest() for p in models.values()})
        cfg=dict(seed=1,levels=[1],envs=2,map_pool=['game/data/maps/pool/gen_example.json'],
                 defender_prepare_ticks=900,max_ticks=2400,tactical_goals='known-economy')
        rows=[dict(environment=i,level=1,world_seed=1000+i,map=cfg['map_pool'][0],spawn_index=i,
                   end='EpisodeEnd.Timeout',ticks=2400,tally=dict(bld_value=100,losses=20)) for i in range(2)]
        self.reports={name:dict(config=copy.deepcopy(cfg),contract={'source':'test'},complete=True,
                               policy='frozen_argmax',checkpoint_sha256=self.plan['files'][models[name]],
                               rows=copy.deepcopy(rows)) for name in names}

    def run_comparison(self):
        plan=self.root/'plan.json';plan.write_text(json.dumps(self.plan))
        for name,report in self.reports.items():
            (self.root/f'lowlevel1-{name}.json').write_text(json.dumps(report))
        return compare(plan,self.root)

    def test_three_arms_keep_original_baseline(self):
        self.reports['detached']['rows'][0]['end']='EpisodeEnd.KeepDestroyed'
        result=self.run_comparison()['levels']['1']
        self.assertEqual(result['summary']['baseline']['wins'],0)
        self.assertEqual(result['summary']['detached']['wins'],1)
        self.assertEqual(result['pairs']['shared'][0]['win_delta'],0)
        self.assertEqual(result['pairs']['detached'][0]['win_delta'],1)

    def test_rejects_changed_model_bytes(self):
        Path(self.plan['models']['shared']).write_text('replaced')
        with self.assertRaisesRegex(ValueError,'Changed input'):self.run_comparison()

    def test_goal_mode_must_match_explicit_plan_for_every_arm(self):
        self.plan['tactical_goals']='split-economy'
        with self.assertRaisesRegex(ValueError,'Wrong goal mode'):self.run_comparison()
        for report in self.reports.values():report['config']['tactical_goals']='split-economy'
        self.assertEqual(self.run_comparison()['levels']['1']['summary']['baseline']['wins'],0)
        self.reports['shared']['config']['tactical_goals']='known-economy'
        with self.assertRaisesRegex(ValueError,'Mismatched'):self.run_comparison()

    def test_rejects_wrong_checkpoint_report(self):
        self.reports['shared']['checkpoint_sha256']='wrong'
        with self.assertRaisesRegex(ValueError,'Wrong checkpoint'):self.run_comparison()

    def test_rejects_missing_or_duplicate_case(self):
        self.reports['detached']['rows'][1]=self.reports['detached']['rows'][0]
        with self.assertRaisesRegex(ValueError,'Duplicate'):self.run_comparison()

    def test_rejects_unpaired_entrance(self):
        self.reports['shared']['rows'][0]['spawn_index']=9
        with self.assertRaisesRegex(ValueError,'Unpaired'):self.run_comparison()

    def test_rejects_nonfinite_battle_value(self):
        self.reports['shared']['rows'][0]['tally']['bld_value']=float('nan')
        with self.assertRaisesRegex(ValueError,'Invalid battle tally'):self.run_comparison()


if __name__=='__main__':unittest.main()
