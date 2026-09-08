"""Bounded component benchmark; not a claim about end-to-end training speed."""
import argparse
import platform
import statistics
import time

import numpy as np
import torch
import rts_native as R
from checkpointing import atomic_json
from ppo import Cfg, Observer, Policy, make_env


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--device', default='cpu')
    ap.add_argument('--envs', type=int, default=16)
    ap.add_argument('--threads', type=int, default=4)
    ap.add_argument('--torch-threads', type=int, default=1)
    ap.add_argument('--repeats', type=int, default=10)
    ap.add_argument('--output', default='runs/benchmark.json')
    args = ap.parse_args()
    if args.envs < 1 or args.repeats < 2:
        raise ValueError('envs >= 1 and repeats >= 2 required')
    torch.set_num_threads(args.torch_threads)
    torch.manual_seed(1)
    cfg = Cfg(envs=args.envs,threads=args.threads,device=args.device)
    obs = Observer(make_env(cfg,cfg.envs,.15))
    rows,c,s,g,legal = obs.read()
    dense = (obs.cells.reshape(-1,obs.k,obs.k,obs.c),
             obs.own.reshape(-1,R.obs.SELF_COUNT),np.repeat(obs.glob,obs.mu,axis=0))
    net = Policy(obs.k,obs.c,R.obs.SELF_COUNT,R.obs.GLOBAL_COUNT,R.obs.ACTION_COUNT).to(args.device).eval()
    def forward(data):
        return net(torch.from_numpy(data[0]).to(args.device).permute(0,3,1,2),
                   torch.from_numpy(data[1]).to(args.device),torch.from_numpy(data[2]).to(args.device))
    def measure(fn):
        times = []
        for _ in range(2):
            fn()
        if args.device.startswith('cuda'):
            torch.cuda.synchronize()
        for _ in range(args.repeats):
            start = time.perf_counter()
            fn()
            if args.device.startswith('cuda'):
                torch.cuda.synchronize()
            times.append(time.perf_counter()-start)
        return statistics.median(times)
    with torch.inference_mode():
        dense_s = measure(lambda:forward(dense))
        live_s = measure(lambda:forward((c,s,g)))
        dense_logits,dense_v = forward(dense)
        compact_logits,compact_v = forward((c,s,g))
        torch.testing.assert_close(dense_logits[rows],compact_logits)
        torch.testing.assert_close(dense_v[rows],compact_v)
    ghoul = R.obs.UNIT_TYPE_NAMES.index('Ghoul')
    attackers = [(ghoul,20.5,20.5,1,0)]
    factory = R.WorldFactory(cfg.map_path,cfg.stats_path)
    cached = lambda:factory.make(seed=7,attackers=attackers)
    uncached = lambda:R.make_world_init(cfg.map_path,cfg.stats_path,seed=7,attackers=attackers)
    # Same native initial state is the correctness condition for this optimization.
    a,b = R.BatchedEnv([cached()]),R.BatchedEnv([uncached()])
    assert a.state_hash(0)==b.state_hash(0)
    cached_s,uncached_s = measure(cached),measure(uncached)
    result = dict(platform=platform.platform(),torch=str(torch.__version__),device=args.device,
                  envs=args.envs,repeats=args.repeats,torch_threads=args.torch_threads,
                  dense_rows=args.envs*obs.mu,live_rows=len(rows),
                  observation_storage_reduction=1-len(rows)/(args.envs*obs.mu),
                  dense_forward_seconds=dense_s,live_forward_seconds=live_s,
                  forward_speedup=dense_s/live_s,
                  parse_each_reset_seconds=uncached_s,cached_reset_seconds=cached_s,
                  factory_speedup=uncached_s/cached_s,
                  scope='component microbenchmark only; includes inference transfers, excludes native steps and PPO updates')
    atomic_json(args.output,result)
    print(result)


if __name__=='__main__':
    main()
