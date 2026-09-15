"""Versioned attacker reward recipes; raw native tallies never contain prices."""
import json
import math
from pathlib import Path

UNIT_TYPES = ('Archer','Spear','Ranger','Scout','Mason','Ghoul','Shade','Knight','Phoenix','Wraith','Ram')
LEGACY_WEIGHTS = {
    "dmg_to_units": 0.002,   # shaping，小
    "dmg_to_blds": 0.004,    # shaping，小（比对单位略高：拆墙才是目的）
    "units_killed": 0.05,    # shaping，小
    "blds_destroyed": 0.0,   # 已由 bld_value 表达，别重复计一次
    "bld_value": 1.0,        # **有原则的推导**：= 重建成本，见上
    "scouts_killed": 3.0,    # 适中常量，见上
    "losses": -0.001,        # 负但小，见上
    "scout_units_killed": 0.0,  # audit only; calibrate before assigning rewards
    "masons_killed": 0.0,  # audit only; calibrate before assigning rewards
    "phoenix_losses": 0.0,  # audit only; calibrate before assigning rewards
    "enemy_unit_gold": 0.0,  # audit only; calibrate before assigning rewards
    "opponent_repair_wood_spent": 0.0,  # audit only; calibrate before assigning rewards
    "friendly_unit_damage": 0.0,  # no positive credit for friendly fire
    "friendly_units_killed": 0.0,  # own combat deaths remain charged through losses
    "progress": 0.0,       # 只作位移日志，不把未折扣的距离差当奖励
}
NEW_FIELDS = tuple(f'{side}_{kind}_levels' for side in ('enemy','own') for kind in UNIT_TYPES) + (
    'destroyed_stone','destroyed_wood','destroyed_income_stone','destroyed_income_wood','destroyed_income_gold')
LEGACY_WEIGHTS.update(dict.fromkeys(NEW_FIELDS, 0.0))
# Starting exchange values, not measured contextual threat. Defender combat
# anchors use level-one recruit prices; support and attacker values are hypotheses.
DEFAULT = dict(unit_scale=0.1, stone=1.0, wood=1.0, gold=1.0,
    repair_scale=1.0, income_periods=2.0,
    unit_values=dict(zip(UNIT_TYPES,(60.,70.,90.,30.,60.,50.,70.,100.,200.,40.,140.))))

def recipe(name='legacy', config_path=''):
    if name not in ('legacy','attrition-v1'):
        raise ValueError('Unknown reward profile')
    if name=='legacy':
        if config_path: raise ValueError('Legacy recipe does not accept candidate parameters')
        return dict(name=name, version=1, weights=dict(LEGACY_WEIGHTS), parameters={})
    parameters=dict(DEFAULT, unit_values=dict(DEFAULT['unit_values']))
    if config_path:
        supplied=json.loads(Path(config_path).read_text(encoding='utf-8'))
        if not isinstance(supplied,dict) or set(supplied)-set(DEFAULT):
            raise ValueError('Unknown reward parameter')
        parameters.update(supplied)
    values=parameters['unit_values']
    if not isinstance(values,dict) or set(values)!=set(UNIT_TYPES):
        raise ValueError('unit_values must specify every registered type exactly once')
    numbers=[v for k,v in parameters.items() if k!='unit_values']+list(values.values())
    if any(isinstance(v,bool) or not isinstance(v,(int,float)) or not math.isfinite(v) or v<0 for v in numbers):
        raise ValueError('Reward parameters must be finite nonnegative numbers')
    if parameters['unit_scale']==0 or any(v==0 for v in values.values()):
        raise ValueError('Candidate unit exchange must have nonzero prices')
    w=dict.fromkeys(LEGACY_WEIGHTS,0.0)
    for kind,value in values.items():
        w[f'enemy_{kind}_levels']=parameters['unit_scale']*value
        w[f'own_{kind}_levels']=-parameters['unit_scale']*value
    w['destroyed_stone']=parameters['stone'];w['destroyed_wood']=parameters['wood']
    w['opponent_repair_wood_spent']=parameters['repair_scale']*parameters['wood']
    for resource in ('stone','wood','gold'):
        w[f'destroyed_income_{resource}']=parameters['income_periods']*parameters[resource]
    return dict(name=name,version=1,weights=w,parameters=parameters)

def weights_for(names,name='legacy',config_path=''):
    result=recipe(name,config_path)
    if set(names)!=set(result['weights']):
        raise ValueError('Native tally layout does not match reward recipe')
    return [result['weights'][key] for key in names]
