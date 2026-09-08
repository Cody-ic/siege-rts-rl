"""Inspect actual first-minibatch PPO gradients without changing its update."""
import argparse
from dataclasses import asdict
from pathlib import Path
from types import SimpleNamespace
import torch
import rts_native as R
from checkpointing import atomic_json, contract, sha256
from ppo import Cfg, train


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--mode',choices=['shared','detached'],required=True)
    p.add_argument('--run-dir',type=Path,required=True)
    p.add_argument('--checkpoint',required=True)
    args=p.parse_args()
    if args.run_dir.exists():raise FileExistsError(args.run_dir)
    cfg=Cfg(envs=8,threads=4,torch_threads=1,rollout=64,total_steps=512,epochs=1,minibatches=4,
            device='cpu',roster='mixed',levels=(1,4,8,16),curriculum=(1.,),
            tactical_goals='known-economy',defender_prepare_ticks=900,lr=1e-5,gamma=.995,
            ent_coef=.005,reference_coef=1.,value_features=args.mode,
            map_pool=tuple(f'game/data/maps/pool/gen_{m}.json' for m in ('01001000','01004000','01005000','01006001')))
    records=[]
    def observer(net,actor_objective,value_objective):
        if records:return
        parameters=[v for k,v in net.named_parameters() if k.startswith(('conv.','trunk.'))]
        def flat(loss):
            gradients=torch.autograd.grad(loss,parameters,retain_graph=True,allow_unused=True)
            return torch.cat([(torch.zeros_like(p) if g is None else g).flatten() for p,g in zip(parameters,gradients)])
        actor=flat(actor_objective);value=flat(value_objective);combined=actor+value
        norm=lambda x:float(torch.linalg.vector_norm(x))
        na,nv=norm(actor),norm(value)
        records.append(dict(actor_objective=float(actor_objective.detach()),value_objective=float(value_objective.detach()),
                            actor_norm=na,value_norm=nv,combined_norm=norm(combined),
                            value_to_actor_ratio=nv/na if na else None,
                            gradient_cosine=float(torch.dot(actor,value)/(na*nv)) if na and nv else None,
                            all_finite=bool(torch.isfinite(combined).all())))
    options=SimpleNamespace(run_dir=args.run_dir,init_weights=args.checkpoint,init_frac=None,stop_after_updates=1)
    train(cfg,options,None,gradient_observer=observer)
    if not records or not records[0]['all_finite']:raise RuntimeError('Missing or invalid gradient observation')
    result=dict(config=asdict(cfg),contract=contract(R,cfg),checkpoint_sha256=sha256(args.checkpoint),
                observations=records,scope='First real PPO minibatch; local gradient magnitude/direction, not proof of full training causality')
    atomic_json(args.run_dir/'gradient-probe.json',result)
    print(records)


if __name__=='__main__':main()
