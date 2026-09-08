"""Pair completed frozen macro reports; reject mismatched protocols or missing cases."""
import argparse
import json
from pathlib import Path


def compare(before,after):
    if not before.get('complete') or not after.get('complete'):
        raise ValueError('Both evaluation reports must be complete')
    for key in ('simulation','stats_sha256','maps','seeds','max_wave','max_ticks','mode','policy_period'):
        if key not in before or before[key]!=after.get(key):
            raise ValueError(f'Evaluation protocol mismatch: {key}')
    expected={(path,seed) for path in before['maps'] for seed in before['seeds']}
    if not expected or len(before['seeds'])!=len(set(before['seeds'])):
        raise ValueError('Evaluation maps and unique seeds must be nonempty')
    def cases(report,arm):
        rows=[row for row in report['cases'] if row['arm']==arm]
        indexed={(row['map'],row['seed']):row for row in rows}
        if len(indexed)!=len(rows) or set(indexed)!=expected:
            raise ValueError(f'Missing or duplicated {arm} cases')
        return indexed
    left,right=cases(before,'learned'),cases(after,'learned')
    pairs=[]
    for key in sorted(expected):
        a,b=left[key],right[key]
        pairs.append(dict(map=key[0],seed=key[1],before_waves=a['waves_survived'],
            after_waves=b['waves_survived'],delta=b['waves_survived']-a['waves_survived'],
            before_completed=a['completed'],after_completed=b['completed']))
    return dict(cases=len(pairs),before_checkpoint=before['checkpoint_sha256'],
        after_checkpoint=after['checkpoint_sha256'],
        before_completed=sum(row['before_completed'] for row in pairs),
        after_completed=sum(row['after_completed'] for row in pairs),
        before_mean_waves=sum(row['before_waves'] for row in pairs)/len(pairs),
        after_mean_waves=sum(row['after_waves'] for row in pairs)/len(pairs),
        improved=sum(row['delta']>0 for row in pairs),worsened=sum(row['delta']<0 for row in pairs),pairs=pairs)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('before');parser.add_argument('after')
    args=parser.parse_args()
    print(json.dumps(compare(json.loads(Path(args.before).read_text(encoding='utf-8')),
                             json.loads(Path(args.after).read_text(encoding='utf-8'))),indent=2))
