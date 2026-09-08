"""One-time audited KL-only migration; preserves optimizer/RNG in a new directory."""
import argparse
import copy
import hashlib
from pathlib import Path
import subprocess
import torch
import rts_native as R
from checkpointing import ROOT, contract, load_training, save_run, atomic_json, sha256
from ppo import Cfg


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--checkpoint',required=True,type=Path)
    parser.add_argument('--output',required=True,type=Path)
    args=parser.parse_args()
    saved=load_training(args.checkpoint)
    old=copy.deepcopy(saved['contract'])
    current=contract(R,Cfg(**saved['config']))
    old_base={k:v for k,v in old.items() if k!='learner_sha256'}
    new_base={k:v for k,v in current.items() if k!='learner_sha256'}
    if old_base!=new_base:raise ValueError('Migration cannot change native, map, stats or observation contract')
    expected_names={'ppo.py','learning.py','rollout.py','checkpointing.py'}
    if set(old['learner_sha256'])!=expected_names:raise ValueError('Not the supported pre-KL checkpoint contract')
    for name in sorted(expected_names):
        raw=subprocess.check_output(['git','show',f'7c31118:train/{name}'],cwd=ROOT)
        normalized=raw.replace(b'\r\n',b'\n')
        hashes={hashlib.sha256(x).hexdigest() for x in (normalized,normalized.replace(b'\n',b'\r\n'))}
        if old['learner_sha256'][name] not in hashes:raise ValueError(f'Unsupported source version: {name}')
        expected=normalized.decode('utf-8')
        if name=='ppo.py':
            expected=expected.replace('from learning import CompletionWindow, potential_reward, task_reward\n',
                                      'from learning import CompletionWindow, potential_reward, task_reward\nfrom stable_kl import masked_reference_kl\n')
            expected=expected.replace("                            ref_dist = torch.distributions.Categorical(logits=ref_logits.masked_fill(~legal,float('-inf')))\n",'')
            expected=expected.replace('ref_kl = torch.distributions.kl_divergence(ref_dist,dist).mean()',
                                      'ref_kl = masked_reference_kl(ref_logits,logits,legal).mean()')
        if name=='checkpointing.py':
            expected=expected.replace("('ppo.py', 'learning.py', 'rollout.py', 'checkpointing.py')",
                                      "('ppo.py', 'learning.py', 'rollout.py', 'checkpointing.py', 'stable_kl.py')")
        if (ROOT/'train'/name).read_text(encoding='utf-8')!=expected:
            raise ValueError(f'Current source contains changes beyond the audited KL fix: {name}')
    migration=dict(kind='stable-reference-kl-only',source_checkpoint=str(args.checkpoint.resolve()),
                   source_sha256=sha256(args.checkpoint),old_contract=old,new_contract=current,
                   boundary='Model, Adam, RNG and progress retained; active native episodes still restart')
    saved['contract']=current
    saved['initialization']=dict(previous=saved['initialization'],migration=migration)
    args.output.mkdir(parents=True,exist_ok=False)
    save_run(args.output,saved)
    loaded=load_training(args.output/'latest.pt')
    def same(a,b):
        if isinstance(a,torch.Tensor):return torch.equal(a,b)
        if isinstance(a,dict):return a.keys()==b.keys() and all(same(a[k],b[k]) for k in a)
        if isinstance(a,(tuple,list)):return type(a)==type(b) and len(a)==len(b) and all(same(x,y) for x,y in zip(a,b))
        return a==b
    migration['retained']={key:same(saved[key],loaded[key]) for key in
                           ('model','optimizer','reference_model','rng','progress','config')}
    if not all(migration['retained'].values()):raise ValueError('Migration round-trip mismatch')
    atomic_json(args.output/'migration.json',migration)
    print(migration['retained'])


if __name__=='__main__':main()
