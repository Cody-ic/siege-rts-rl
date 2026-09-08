"""CPU defender macro PPO, complete campaigns and deterministic command-log recovery.

Checkpoints commit whole updates. A interrupted rollout is replayed from the last
commit, never counted twice. Wave boundaries terminate RL returns but keep the city.
"""
import argparse
from dataclasses import asdict, dataclass
from pathlib import Path
import random

import numpy as np
import torch
import rts_native as native

from checkpointing import load_auto, restore_rng, rng_state, run_lock, save_run, sha256
from macro_policy import MacroPolicy

ROOT = Path(__file__).resolve().parents[1]


@dataclass
class Config:
    maps: tuple
    stats: str
    seed: int = 1
    rollout: int = 64
    period: int = 100
    epochs: int = 2
    hidden: int = 64
    learning_rate: float = .0003
    gamma: float = .995
    gae_lambda: float = .95
    clip: float = .2
    max_wave: int = 70


def contract(cfg):
    return dict(kind='defender-macro-ppo-v1', simulation=native.SIMULATION_FINGERPRINT,
                build=native.BUILD_MODE, obs_version=native.macro_obs.VERSION,
                cells=list(native.macro_obs.CELL_NAMES), globals=list(native.macro_obs.GLOBAL_NAMES),
                commands=list(native.COMMAND_KIND_NAMES), maps=[sha256(p) for p in cfg.maps],
                stats=sha256(cfg.stats), sources={name:sha256(ROOT/'train'/name) for name in
                    ('macro_train.py','macro_policy.py','checkpointing.py')})


def campaign(cfg, episode):
    return native.TrainingCampaign(cfg.maps[episode % len(cfg.maps)], cfg.stats, cfg.seed + episode)


def bootstrap(policy, obs):
    with torch.no_grad():
        _, context = policy.encode(*obs)
        return float(policy.critic(context).squeeze())


def returns(rows, cfg):
    """Time-scaled GAE; a wave boundary has zero bootstrap, without resetting city."""
    carry = 0.
    advantages = []
    for row in reversed(rows):
        discount = cfg.gamma ** (row['ticks'] / cfg.period)
        trace = cfg.gae_lambda ** (row['ticks'] / cfg.period)
        alive = not row['terminal']
        delta = row['reward'] + discount * alive * row['next_value'] - row['value']
        carry = delta + discount * trace * alive * carry
        advantages.append(carry)
    advantages.reverse()
    targets = torch.tensor([a + row['value'] for a,row in zip(advantages,rows)], dtype=torch.float32)
    adv = torch.tensor(advantages, dtype=torch.float32)
    if len(adv)>1:
        adv = (adv-adv.mean())/(adv.std(unbiased=False)+1e-8)
    return adv, targets


def train(cfg, folder, updates):
    if not cfg.maps or min(cfg.rollout,cfg.period,cfg.epochs,cfg.hidden,cfg.max_wave)<1 or updates<0:
        raise ValueError('positive training dimensions and a nonempty map pool required')
    if not 0<cfg.gamma<=1 or not 0<cfg.gae_lambda<=1 or not 0<cfg.clip<1 or cfg.learning_rate<=0:
        raise ValueError('invalid PPO hyperparameters')
    torch.set_num_threads(1)
    random.seed(cfg.seed);np.random.seed(cfg.seed);torch.manual_seed(cfg.seed)
    identity = contract(cfg)
    with run_lock(folder):
        saved,_ = load_auto(folder)
        policy = MacroPolicy(len(native.macro_obs.CELL_NAMES),len(native.macro_obs.GLOBAL_NAMES),
                             len(native.COMMAND_KIND_NAMES),cfg.hidden)
        optimizer = torch.optim.Adam(policy.parameters(),lr=cfg.learning_rate)
        progress = dict(updates=0,env_steps=0,episode=0,waves_survived=0,defeats=0,history=[])
        commands = []
        if saved:
            if saved['config']!=asdict(cfg) or saved['contract']!=identity:
                raise ValueError('Macro run inputs changed; use a new directory')
            policy.load_state_dict(saved['model']);optimizer.load_state_dict(saved['optimizer'])
            progress=saved['progress'];commands=saved['campaign']['commands']
        world=campaign(cfg,progress['episode'])
        for command in commands:
            world.advance(cfg.period,[tuple(command)])
        if saved:
            if world.diagnostic_state_hash!=saved['campaign']['hash']:
                raise ValueError('Campaign recovery hash mismatch; refusing to continue')
            restore_rng(saved['rng'])

        def commit():
            save_run(folder,dict(format=1,config=asdict(cfg),contract=identity,
                progress=progress,status='ready',initialization='random-defender',
                model=policy.state_dict(),optimizer=optimizer.state_dict(),rng=rng_state(),
                campaign=dict(commands=commands,hash=world.diagnostic_state_hash,
                              tick=world.tick,wave=world.wave)))
        if not saved:
            commit()
        while progress['updates']<updates:
            rows=[]
            for _ in range(cfg.rollout):
                obs=world.defender_observation()
                with torch.no_grad():
                    decision=policy.decide(*obs,world.candidates(),world.map_shape)
                transition=world.advance(cfg.period,[decision.command])
                commands.append(decision.command)
                terminal=transition['wave_advanced'] or transition['defeated']
                # Sparse outcome only: no repeated reward for building/demolishing or waiting.
                reward=float(transition['wave_advanced'])-float(transition['defeated'])
                rows.append(dict(obs=obs,command=decision.command,domains=decision.domains,
                    shape=world.map_shape,log_prob=float(decision.log_prob),value=float(decision.value),
                    reward=reward,terminal=terminal,ticks=transition['ticks'],
                    next_value=0. if terminal else bootstrap(policy,world.defender_observation())))
                progress['waves_survived']+=int(transition['wave_advanced'])
                progress['defeats']+=int(transition['defeated'])
                if transition['defeated'] or world.wave>cfg.max_wave:
                    progress['episode']+=1
                    world=campaign(cfg,progress['episode']);commands=[]
            advantages,targets=returns(rows,cfg)
            losses=[]
            for _ in range(cfg.epochs):
                optimizer.zero_grad()
                # Accumulate one rollout gradient; don't retain all spatial graphs in memory.
                for index,row in enumerate(rows):
                    d=policy.rescore(*row['obs'],row['command'],row['domains'],row['shape'])
                    ratio=(d.log_prob-row['log_prob']).exp()
                    surrogate=torch.minimum(ratio*advantages[index],
                        ratio.clamp(1-cfg.clip,1+cfg.clip)*advantages[index])
                    loss=(-surrogate+.5*(d.value-targets[index]).square())/len(rows)
                    if not torch.isfinite(loss):
                        raise ValueError('Non-finite macro PPO loss; last committed checkpoint preserved')
                    loss.backward();losses.append(float(loss.detach()))
                torch.nn.utils.clip_grad_norm_(policy.parameters(),.5,error_if_nonfinite=True)
                optimizer.step()
            progress['updates']+=1;progress['env_steps']+=len(rows)
            progress['history'].append(dict(update=progress['updates'],loss=sum(losses)/cfg.epochs,
                reward=sum(r['reward'] for r in rows),tick=world.tick,wave=world.wave,
                episode=progress['episode']))
            commit()
            print(progress['history'][-1],flush=True)
        return progress


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--maps',nargs='+',required=True)
    parser.add_argument('--stats',default=str(ROOT/'game/data/stats_placeholder.json'))
    parser.add_argument('--run-dir',required=True)
    parser.add_argument('--updates',type=int,default=100)
    parser.add_argument('--seed',type=int,default=1)
    parser.add_argument('--rollout',type=int,default=64)
    parser.add_argument('--period',type=int,default=100)
    args=parser.parse_args()
    train(Config(tuple(str(Path(p).resolve()) for p in args.maps),str(Path(args.stats).resolve()),
                 seed=args.seed,rollout=args.rollout,period=args.period),args.run_dir,args.updates)
