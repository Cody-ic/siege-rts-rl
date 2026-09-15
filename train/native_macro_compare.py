"""Pair native-game evaluations only when their complete frozen protocols match."""
import argparse
import json
from pathlib import Path
from checkpointing import sha256


def compare(plan,before,after,plan_sha):
    expected={(path,seed) for path in plan['maps'] for seed in plan['seeds']}
    if not expected or len(expected)!=len(plan['maps'])*len(plan['seeds']):
        raise ValueError('Nonempty unique map-seed protocol required')
    indexed=[]
    for arm,report in (('before',before),('after',after)):
        if not report.get('complete') or report.get('arm')!=arm or report.get('plan_sha256')!=plan_sha:
            raise ValueError('Incomplete or mismatched native evaluation')
        if report.get('mode')!='native-game-categorical':raise ValueError('Native sampling mode differs')
        rows=report['cases'];lookup={(r['map'],r['seed']):r for r in rows}
        if len(rows)!=len(lookup) or set(lookup)!=expected:raise ValueError('Missing or duplicate native case')
        if len({r['defender_identity'] for r in rows})!=1 or len({r['attacker_identity'] for r in rows})!=1:
            raise ValueError('Mixed model identities in one evaluation arm')
        for row in rows:
            if row['policy_period']!=plan['period']:raise ValueError('Cadence mismatch')
            if row['waves_survived']!=row['wave']-1 or not 0<=row['tick']<=plan['max_ticks']:
                raise ValueError('Invalid native outcome')
            if row['completed']!=(row['wave']>plan['max_wave']):raise ValueError('Completion mismatch')
            if row['completed'] and row['defeated']:raise ValueError('Contradictory final state')
            if row['timeout']!=(not row['completed'] and not row['defeated']):raise ValueError('Timeout mismatch')
            if row['timeout'] and row['tick']!=plan['max_ticks']:raise ValueError('Premature native timeout')
        indexed.append(lookup)
    left,right=indexed
    pairs=[]
    for key in sorted(expected):
        a,b=left[key],right[key]
        if a['attacker_identity']!=b['attacker_identity']:raise ValueError('Attacker differs')
        pairs.append(dict(map=key[0],seed=key[1],before=a['waves_survived'],after=b['waves_survived'],
                          delta=b['waves_survived']-a['waves_survived']))
    return dict(cases=len(pairs),before_mean=sum(p['before'] for p in pairs)/len(pairs),
        after_mean=sum(p['after'] for p in pairs)/len(pairs),
        before_completed=sum(r['completed'] for r in left.values()),
        after_completed=sum(r['completed'] for r in right.values()),
        improved=sum(p['delta']>0 for p in pairs),worsened=sum(p['delta']<0 for p in pairs),pairs=pairs)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('plan');parser.add_argument('before');parser.add_argument('after')
    args=parser.parse_args()
    plan,before,after=[json.loads(Path(p).read_text(encoding='utf-8')) for p in (args.plan,args.before,args.after)]
    print(json.dumps(compare(plan,before,after,sha256(args.plan)),indent=2))
