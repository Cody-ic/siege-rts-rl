"""CPU flow demonstrations -> resumable imitation fit -> explicit PPO warm start.

The teacher sees the public observation and legal actions only. It is never called
by PPO or game inference. Fitting initializes actor features; it does not train a
value function or establish a winning strategy.
"""
import argparse
from dataclasses import asdict
import json
import os
from pathlib import Path
import signal
import warnings

if __name__ == '__main__':
    os.environ['CUDA_VISIBLE_DEVICES'] = ''

import numpy as np
import torch
import torch.nn.functional as F
import rts_native as R
from checkpointing import atomic_json, atomic_write, contract, load_weights, run_lock, sha256
from evaluate import flow_actions, breach_actions
from ppo import Cfg, Observer, Policy, make_env, make_worlds, policy_forward

FORMAT = 'rts-demonstrations-1'
ARRAYS = ('cells', 'own', 'glob', 'legal', 'labels')


def signature(cfg):
    root = Path(__file__).parent
    return dict(contract=contract(R, cfg), source={name:sha256(root/name) for name in
                ('demonstrations.py', 'evaluate.py', 'diagnostics.py')})


def save_dataset(path, data, metadata):
    atomic_write(path, lambda stream: np.savez_compressed(
        stream, **dict(zip(ARRAYS, data)), metadata=np.array(json.dumps(metadata, sort_keys=True))))


def load_dataset(path):
    with np.load(path, allow_pickle=False) as packed:
        metadata = json.loads(str(packed['metadata']))
        data = tuple(packed[name] for name in ARRAYS)
    cfg = Cfg(**metadata['config'])
    if metadata['signature'] != signature(cfg):
        raise ValueError('Demonstration contract/source changed; collect in a new directory')
    c, own, glob, legal, labels = data
    n = len(labels)
    expected = ((n,R.obs.K,R.obs.K,R.obs.CHANNEL_COUNT),
                (n,R.obs.SELF_COUNT),(n,R.obs.GLOBAL_COUNT),(n,R.obs.ACTION_COUNT),(n,))
    if n < 1 or any(a.shape != shape for a,shape in zip(data,expected)):
        raise ValueError('Invalid demonstration shapes or empty dataset')
    if any(a.dtype != np.float32 or not np.isfinite(a).all() for a in (c,own,glob)):
        raise ValueError('Demonstrations must contain finite float32 observations')
    if labels.dtype != np.uint8 or legal.dtype != np.bool_ or np.any(labels >= R.obs.ACTION_COUNT):
        raise ValueError('Invalid action labels or masks')
    if not np.all(legal[np.arange(n), labels]):
        raise ValueError('Teacher selected an illegal action')
    return data, metadata


def collect(args):
    if min(args.envs,args.threads,args.steps,args.stride) < 1 or args.seed < 0:
        raise ValueError('Positive collection sizes/threads and nonnegative seed required')
    fractions = [float(f) for f in args.fractions.split(',')]
    if not fractions or fractions != sorted(set(fractions)) or not all(0 < f <= 1 for f in fractions):
        raise ValueError('Fractions must be increasing, unique and in (0,1]')
    cfg = Cfg(envs=args.envs,threads=args.threads,torch_threads=1,device='cpu',seed=args.seed,
              map_path=args.map_path,stats_path=args.stats_path,map_pool=tuple(args.map_pool.split(',')) if args.map_pool else (),
              roster=args.roster,levels=tuple(int(x) for x in args.levels.split(',')),
              defender_prepare_ticks=args.defender_prepare_ticks,tactical_goals=args.tactical_goals,
              curriculum=tuple(fractions))
    from ppo import validate
    validate(cfg)
    # JSON normalization makes tuple/list config fields identical after a restart.
    plan = json.loads(json.dumps(dict(config=asdict(cfg),signature=signature(cfg),
                    steps=args.steps,stride=args.stride,fractions=fractions,teacher=args.teacher,
                    behavior_sha256=sha256(args.behavior_checkpoint) if args.behavior_checkpoint else None)))
    behavior=None
    if args.behavior_checkpoint:
        torch.set_num_threads(1)
        behavior=Policy(R.obs.K,R.obs.CHANNEL_COUNT,R.obs.SELF_COUNT,R.obs.GLOBAL_COUNT,R.obs.ACTION_COUNT)
        behavior.load_state_dict(load_weights(args.behavior_checkpoint))
        behavior.eval().requires_grad_(False)
    folder = Path(args.out_dir)
    with run_lock(folder):
        plan_path, destination = folder/'plan.json', folder/'demonstrations.npz'
        if plan_path.exists():
            if json.loads(plan_path.read_text(encoding='utf-8')) != plan:
                raise ValueError('Collection plan changed; use a new directory')
        elif destination.exists() or list(folder.glob('part-*.npz')):
            raise ValueError('Existing collection has no plan; refusing to overwrite')
        else:
            atomic_json(plan_path,plan)
        if destination.exists():
            _, meta = load_dataset(destination)
            if meta != plan:
                raise ValueError('Final dataset does not match collection plan')
            print('Collection already complete:',destination,flush=True)
            return destination
        parts = []
        for index, frac in enumerate(fractions):
            path = folder/f'part-{index}.npz'
            metadata = dict(plan,part=index)
            if path.exists():
                data, previous = load_dataset(path)
                if previous != metadata:
                    raise ValueError('Collection part does not match plan')
            else:
                env = make_env(cfg,cfg.envs,frac)
                obs = Observer(env)
                actions = np.zeros((cfg.envs,obs.mu),np.uint8)
                done = np.zeros(cfg.envs,np.uint8)
                samples, episode = [], cfg.envs
                for step in range(args.steps):
                    observation = obs.read()
                    rows,c,own,glob,legal = observation
                    labels = (breach_actions if args.teacher == 'flow_breach' else flow_actions)(observation)
                    if step % args.stride == 0 and len(rows):
                        samples.append(tuple(a.copy() for a in (c,own,glob,legal,labels)))
                    actions.fill(0)
                    chosen=labels
                    if behavior is not None:
                        with torch.inference_mode():dist,_=policy_forward(behavior,observation,'cpu')
                        if dist is not None:chosen=dist.logits.argmax(-1).numpy().astype(np.uint8)
                    actions.reshape(-1)[rows] = chosen
                    env.step(actions,done)
                    for slot in np.flatnonzero(done):
                        env.reset_one(int(slot),make_worlds(cfg,1,frac,episode)[0])
                        episode += 1
                data = tuple(np.concatenate([sample[k] for sample in samples]) for k in range(5))
                save_dataset(path,data,metadata)
            parts.append(data)
            print(f'Collected frac={frac:g}: {len(data[-1])} examples',flush=True)
        joined = tuple(np.concatenate([part[k] for part in parts]) for k in range(5))
        save_dataset(destination,joined,plan)
        print(f'Saved {len(joined[-1])} examples; {len(fractions)*args.steps*args.envs} collection env-steps',flush=True)
        return destination


def read_fit(path):
    saved = torch.load(path,map_location='cpu',weights_only=True)
    required = {'format','plan','model','optimizer','epoch','history','cpu_rng','numpy_rng'}
    if not isinstance(saved,dict) or not required <= saved.keys() or saved['format'] != FORMAT:
        raise ValueError('Not an imitation fitting checkpoint')
    if any(not torch.isfinite(v).all() for v in saved['model'].values()):
        raise ValueError('Nonfinite model in fitting checkpoint')
    return saved


def load_fit(folder):
    errors = []
    for name in ('latest.pt','previous.pt'):
        path = folder/name
        if not path.exists():
            continue
        try:
            saved = read_fit(path)
        except Exception as exc:
            errors.append(f'{name}: {exc}')
            continue
        if errors:
            warnings.warn('Using previous fitting checkpoint: '+'; '.join(errors))
        return saved
    raise ValueError('No readable fitting checkpoint; refusing to restart silently: '+'; '.join(errors))


def save_fit(folder, saved):
    pending, latest, previous = folder/'pending.pt',folder/'latest.pt',folder/'previous.pt'
    atomic_write(pending,lambda stream:torch.save(saved,stream))
    read_fit(pending)
    if latest.exists():
        try:
            read_fit(latest)
        except Exception:
            pass  # Never rotate a corrupt latest over the known-good fallback.
        else:
            os.replace(latest,previous)
    os.replace(pending,latest)
    if os.name != 'nt':
        fd = os.open(folder,os.O_RDONLY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)


def fit(args):
    if min(args.epochs,args.batch_size,args.torch_threads) < 1 or args.seed < 0 or not np.isfinite(args.lr) or args.lr <= 0:
        raise ValueError('Positive fitting sizes/lr and nonnegative seed required')
    folder = Path(args.run_dir)
    with run_lock(folder):
        data, metadata = load_dataset(args.data)
        if 'part' in metadata:
            raise ValueError('Fit the final demonstrations.npz, not an incomplete collection part')
        c,own,glob,legal,labels = data
        initialization=sha256(args.init_weights) if args.init_weights else None
        plan = dict(dataset_sha256=sha256(args.data),signature=metadata['signature'],
                    seed=args.seed,batch_size=args.batch_size,lr=args.lr,initialization_sha256=initialization)
        if args.resume:
            saved = load_fit(folder)
            if not args.init_weights:plan['initialization_sha256']=saved['plan'].get('initialization_sha256')
            if saved['plan'] != plan:
                raise ValueError('Fitting data/config/source changed; use a new run directory')
        else:
            if any((folder/n).exists() for n in ('latest.pt','previous.pt','policy.pt','initialization.json')):
                raise ValueError('Existing fit; use --resume or a new run directory')
            saved = None
        torch.set_num_threads(args.torch_threads)
        torch.manual_seed(args.seed)
        rng = np.random.default_rng(args.seed)
        net = Policy(R.obs.K,R.obs.CHANNEL_COUNT,R.obs.SELF_COUNT,R.obs.GLOBAL_COUNT,R.obs.ACTION_COUNT)
        opt = torch.optim.Adam(net.parameters(),lr=args.lr)
        history, start = [], 0
        if saved:
            net.load_state_dict(saved['model']); opt.load_state_dict(saved['optimizer'])
            start, history = saved['epoch'], saved['history']
            torch.set_rng_state(saved['cpu_rng'])
            rng.bit_generator.state = saved['numpy_rng']
        elif args.init_weights:
            net.load_state_dict(load_weights(args.init_weights))
        def checkpoint(epoch):
            save_fit(folder,dict(format=FORMAT,plan=plan,model=net.state_dict(),optimizer=opt.state_dict(),
                                 epoch=epoch,history=history,cpu_rng=torch.get_rng_state(),numpy_rng=rng.bit_generator.state))
        if not saved:
            checkpoint(0)
        stop, handlers = [0], {}
        def request_stop(signum,_frame):
            stop[0] = signum
        for sig in (signal.SIGINT,signal.SIGTERM):
            handlers[sig] = signal.signal(sig,request_stop)
        epoch_done = start
        try:
            for epoch in range(start,args.epochs):
                order = rng.permutation(len(labels))
                correct, loss_sum = 0, 0.
                for begin in range(0,len(order),args.batch_size):
                    at = order[begin:begin+args.batch_size]
                    logits,_ = net(torch.from_numpy(c[at]).permute(0,3,1,2),
                                   torch.from_numpy(own[at]),torch.from_numpy(glob[at]))
                    logits = logits.masked_fill(~torch.from_numpy(legal[at]),float('-inf'))
                    target = torch.from_numpy(labels[at].astype(np.int64))
                    loss = F.cross_entropy(logits,target)
                    if not torch.isfinite(loss):
                        raise ValueError('Nonfinite imitation loss; previous checkpoint retained')
                    opt.zero_grad(); loss.backward()
                    norm = torch.nn.utils.clip_grad_norm_(net.parameters(),.5)
                    if not torch.isfinite(norm):
                        raise ValueError('Nonfinite imitation gradient; previous checkpoint retained')
                    opt.step()
                    correct += int((logits.argmax(-1)==target).sum())
                    loss_sum += float(loss.detach())*len(at)
                epoch_done = epoch+1
                history.append(dict(epoch=epoch_done,loss=loss_sum/len(labels),accuracy=correct/len(labels)))
                checkpoint(epoch_done)
                print('Fit',history[-1],flush=True)
                if stop[0]:
                    break
        finally:
            for sig, handler in handlers.items():
                signal.signal(sig,handler)
        atomic_write(folder/'policy.pt',lambda stream:torch.save(net.state_dict(),stream))
        atomic_json(folder/'initialization.json',dict(kind='flow_demonstrations',teacher=metadata['teacher'],plan=plan,
                    epochs=epoch_done,examples=len(labels),collection_env_steps=metadata['steps']*metadata['config']['envs']*len(metadata['fractions']),
                    behavior_sha256=metadata['behavior_sha256'],
                    history=history,policy_sha256=sha256(folder/'policy.pt'),
                    status='complete' if epoch_done >= args.epochs else 'paused'))
        print('Exported',folder/'policy.pt','for a NEW PPO run with --init-weights',flush=True)
        return 128+stop[0] if stop[0] else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    commands = ap.add_subparsers(dest='command',required=True)
    collect_ap = commands.add_parser('collect',help='Collect/reuse immutable per-distance teacher shards on CPU')
    collect_ap.add_argument('--out-dir',required=True)
    for name,default in (('envs',4),('threads',2),('steps',300),('stride',4),('seed',1)):
        collect_ap.add_argument('--'+name,type=int,default=default)
    collect_ap.add_argument('--fractions',default='0.15,0.3,0.5,0.75,1')
    collect_ap.add_argument('--teacher',choices=('flow','flow_breach'),default='flow',
                            help='Explicit teacher choice; flow preserves the original attack-first baseline')
    collect_ap.add_argument('--map-path',default=Cfg().map_path)
    collect_ap.add_argument('--stats-path',default=Cfg().stats_path)
    collect_ap.add_argument('--map-pool',default='')
    collect_ap.add_argument('--roster',choices=('ghouls','mixed'),default='ghouls')
    collect_ap.add_argument('--levels',default='1')
    collect_ap.add_argument('--defender-prepare-ticks',type=int,default=0)
    collect_ap.add_argument('--tactical-goals',choices=('keep','known-economy','split-economy'),default='keep')
    collect_ap.add_argument('--behavior-checkpoint',help='Frozen policy controls collection; teacher supplies labels only')
    fit_ap = commands.add_parser('fit',help='Fit on CPU; restore optimizer and epoch checkpoints with --resume')
    fit_ap.add_argument('--data',required=True)
    fit_ap.add_argument('--run-dir',required=True)
    fit_ap.add_argument('--resume',action='store_true')
    fit_ap.add_argument('--init-weights',help='Explicit actor warm start in a new fitting directory')
    for name,default in (('epochs',12),('batch-size',256),('torch-threads',2),('seed',1)):
        fit_ap.add_argument('--'+name,type=int,default=default)
    fit_ap.add_argument('--lr',type=float,default=3e-4)
    args = ap.parse_args(argv)
    if args.command == 'collect':
        collect(args)
        return 0
    return fit(args)


if __name__ == '__main__':
    raise SystemExit(main())
