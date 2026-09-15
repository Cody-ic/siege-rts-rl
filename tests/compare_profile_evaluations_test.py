import copy
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'train'))
from compare_profile_evaluations import compare


def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()


class ProfileComparisonTests(unittest.TestCase):
    STYLES=['balanced','fortified'];MODES=['frozen_argmax','frozen_sample']

    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.cwd=os.getcwd();os.chdir(self.root);self.addCleanup(os.chdir,self.cwd)
        (self.root/'train').mkdir();(self.root/'game').mkdir()
        for name in ('pipeline.py','evaluate.py','demonstrations.py','diagnostics.py'):
            (self.root/'train'/name).write_text(name)
        (self.root/'game/m.json').write_text('map');(self.root/'game/stats.json').write_text('stats')
        self.plan=dict(arms={'control':{'train_profiles':[]},'profiles':{'train_profiles':self.STYLES}},
            training_seeds=[1,2],
            pipeline=dict(map_pool=['game/m.json'],eval_maps=['game/m.json'],levels=[1],total_steps=64,
                          prepare_ticks=900,eval_episodes=4,eval_profiles=self.STYLES,eval_modes=self.MODES),
            files={f'train/{n}':sha(f'train/{n}') for n in ('pipeline.py','evaluate.py','demonstrations.py','diagnostics.py')})
        self.plan['files']['game/m.json']=sha('game/m.json')
        self.runs=self.root/'runs';self.wins={}   # (arm,seed,mode,style) -> wins
        for arm in ('control','profiles'):
            for seed in (1,2):
                for mode in self.MODES:
                    for style in self.STYLES:self.wins[(arm,seed,mode,style)]=2
        self.value_bump={}

    def write_tree(self):
        for arm in ('control','profiles'):
            for seed in (1,2):
                folder=self.runs/f'{arm}-s{seed}';(folder/'ppo').mkdir(parents=True,exist_ok=True)
                (folder/'ppo/policy.pt').write_bytes(f'{arm}{seed}'.encode());model=sha(folder/'ppo/policy.pt')
                cfg=dict(defender_profiles=self.plan['arms'][arm]['train_profiles'],seed=seed,map_pool=['game/m.json'],
                         levels=[1],total_steps=64,defender_prepare_ticks=900,lr=3e-4)
                (folder/'pipeline.json').write_text(json.dumps(dict(config=cfg,evaluation_profiles=self.STYLES,
                    source={n:self.plan['files'][f'train/{n}'] for n in ('pipeline.py','evaluate.py','demonstrations.py','diagnostics.py')})))
                results=[]
                for mode in self.MODES:
                    for style in self.STYLES:
                        wins=self.wins[(arm,seed,mode,style)]
                        value=100.0+self.value_bump.get((arm,seed,mode,style),0)
                        name=f'eval-0-level1-{mode}-{style}.json'
                        report=dict(wins=wins,episodes=4,hit_rate=1.0,mean_building_value=value,policy=mode,
                                    defender_profiles=[style],defender_prepare_ticks=900,
                                    rows=[dict(defender_profile=style) for _ in range(4)],
                                    evaluator_sha256={'evaluate.py':self.plan['files']['train/evaluate.py']},
                                    sha256=dict(checkpoint=model,map=self.plan['files']['game/m.json'],stats='x'))
                        (folder/name).write_text(json.dumps(report))
                        results.append(dict(map='game/m.json',level=1,mode=mode,defender_profile=style,
                            wins=wins,episodes=4,hit_rate=1.0,mean_building_value=value,report=f'/somewhere/else/{name}'))
                (folder/'summary.json').write_text(json.dumps(dict(model_sha256=model,results=results)))
        plan=self.root/'plan.json';plan.write_text(json.dumps(self.plan));return plan

    def test_pass_when_profiles_never_regress(self):
        self.wins[('profiles',1,'frozen_argmax','fortified')]=3
        result=compare(self.write_tree(),self.runs)
        self.assertTrue(result['passed']);self.assertEqual(result['regressions'],[])
        self.assertEqual(result['gate'],{'balanced':True,'fortified':True})
        self.assertEqual(result['weakest_style'],dict(style='balanced',min_win_delta=0))
        self.assertEqual(result['by_style']['frozen_argmax/fortified']['1']['win_delta'],1)

    def test_single_style_regression_fails_gate_and_is_listed(self):
        self.wins[('profiles',2,'frozen_argmax','fortified')]=1
        self.wins[('profiles',1,'frozen_argmax','balanced')]=4   # average would look fine
        result=compare(self.write_tree(),self.runs)
        self.assertFalse(result['passed'])
        self.assertEqual(result['gate'],{'balanced':True,'fortified':False})
        self.assertEqual(result['weakest_style'],dict(style='fortified',min_win_delta=-1))
        self.assertEqual([(r['seed'],r['style']) for r in result['regressions']],[(2,'fortified')])

    def test_value_regression_counts_even_with_equal_wins(self):
        self.value_bump[('profiles',1,'frozen_argmax','balanced')]=-5
        result=compare(self.write_tree(),self.runs)
        self.assertFalse(result['gate']['balanced']);self.assertEqual(len(result['regressions']),1)

    def test_secondary_mode_does_not_gate(self):
        self.wins[('profiles',1,'frozen_sample','balanced')]=0
        result=compare(self.write_tree(),self.runs)
        self.assertTrue(result['passed']);self.assertEqual(len(result['regressions']),1)

    def test_rejects_arms_that_differ_beyond_styles(self):
        plan=self.write_tree()
        folder=self.runs/'control-s1';m=json.loads((folder/'pipeline.json').read_text());m['config']['lr']=1e-5
        (folder/'pipeline.json').write_text(json.dumps(m))
        with self.assertRaisesRegex(ValueError,'differ in something other'):compare(plan,self.runs)

    def test_rejects_report_from_other_weights_or_other_style(self):
        plan=self.write_tree()
        f=self.runs/'profiles-s1/eval-0-level1-frozen_argmax-balanced.json';r=json.loads(f.read_text())
        good=copy.deepcopy(r);r['sha256']['checkpoint']='0'*64;f.write_text(json.dumps(r))
        with self.assertRaisesRegex(ValueError,'other weights'):compare(plan,self.runs)
        good['rows'][0]['defender_profile']='fortified';f.write_text(json.dumps(good))
        with self.assertRaisesRegex(ValueError,'did not face only'):compare(plan,self.runs)

    def test_rejects_missing_case_and_changed_source(self):
        plan=self.write_tree()
        (self.runs/'control-s2/eval-0-level1-frozen_sample-fortified.json').unlink()
        with self.assertRaisesRegex(ValueError,'missing report'):compare(plan,self.runs)
        (self.root/'train/evaluate.py').write_text('changed')
        with self.assertRaisesRegex(ValueError,'Changed input'):compare(plan,self.runs)


if __name__=='__main__':unittest.main()
