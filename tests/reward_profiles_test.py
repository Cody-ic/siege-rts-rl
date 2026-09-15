import json
from pathlib import Path
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'train'))
from reward_profiles import recipe, weights_for, LEGACY_WEIGHTS, UNIT_TYPES

class RewardTests(unittest.TestCase):
    def setUp(self): self.w=recipe('attrition-v1')['weights']
    def pay(self,**events): return sum(self.w[k]*v for k,v in events.items())
    def test_legacy_preserves_original_exchange(self):
        w=recipe()['weights']
        self.assertAlmostEqual(w['dmg_to_units']*240+w['units_killed'],.53)
        self.assertAlmostEqual(w['losses']*190,-.19)
        self.assertEqual(w['enemy_unit_gold'],0)
    def test_symmetric_type_and_level_pricing(self):
        for kind in UNIT_TYPES:
            self.assertGreater(self.w[f'enemy_{kind}_levels'],0)
            self.assertEqual(self.w[f'enemy_{kind}_levels'],-self.w[f'own_{kind}_levels'])
        self.assertEqual(self.pay(enemy_Ranger_levels=4),36)
        self.assertEqual(self.pay(own_Phoenix_levels=4),-80)
        self.assertGreater(self.pay(enemy_Mason_levels=1),self.pay(enemy_Scout_levels=1))
    def test_damage_and_repair_not_double_counted(self):
        self.assertEqual(self.pay(dmg_to_blds=100000),0)
        self.assertEqual(self.pay(dmg_to_blds=100000,opponent_repair_wood_spent=7),7)
        self.assertEqual(self.pay(destroyed_stone=30,destroyed_wood=40,bld_value=70,blds_destroyed=1),70)
        self.assertEqual(self.pay(enemy_Mason_levels=1,units_killed=1,scouts_killed=1,masons_killed=1,enemy_unit_gold=40),6)
    def test_tradeoff_and_production_examples(self):
        self.assertLess(self.pay(own_Phoenix_levels=1),0)
        self.assertGreater(self.pay(own_Phoenix_levels=1,destroyed_stone=90,destroyed_wood=30),0)
        self.assertGreater(self.pay(destroyed_wood=70,destroyed_income_wood=9),self.pay(destroyed_wood=70))
        # A bounded illustrative trajectory, not a universal no-farming proof.
        self.assertGreater(2000, .99**100*(2000+self.pay(enemy_Ranger_levels=10)))
    def test_config_errors_fail_closed(self):
        with self.assertRaises(ValueError):weights_for(['wrong'],'attrition-v1')
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'reward.json'
            for data in ({'typo':1},{'unit_scale':-1},{'unit_values':{}},{'gold':float('nan')}):
                p.write_text(json.dumps(data))
                with self.assertRaises(ValueError):recipe('attrition-v1',str(p))
            p.write_text(json.dumps({'unit_scale':.2}))
            self.assertEqual(recipe('attrition-v1',str(p))['weights']['enemy_Ranger_levels'],18)
            with self.assertRaises(ValueError):recipe('legacy',str(p))

if __name__=='__main__':unittest.main()
