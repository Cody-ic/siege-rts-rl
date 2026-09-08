"""CPU defender macro PPO, complete campaigns and deterministic command-log recovery.

Checkpoints commit whole updates. A interrupted rollout is replayed from the last
commit, never counted twice. Wave boundaries terminate RL returns but keep the city.
"""
import argparse
import copy
import math
from dataclasses import asdict, dataclass
from pathlib import Path
import random

import numpy as np
import torch
import rts_native as native

from checkpointing import load_auto, restore_rng, rng_state, run_lock, save_run, sha256
from macro_policy import MacroPolicy
from macro_evaluate import load_policy
from macro_opponent import load_opponent, check_opponent

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
    anchor_weight: float = 0.
    detach_critic_features: bool = False
    attacker_model: str = ''
    init_from_simulation: str = ''


def contract(cfg, opponent=None):
    return dict(kind='defender-macro-ppo-v1', simulation=native.SIMULATION_FINGERPRINT,
                build=native.BUILD_MODE, obs_version=native.macro_obs.VERSION,
                cells=list(native.macro_obs.CELL_NAMES), globals=list(native.macro_obs.GLOBAL_NAMES),
                detail=list(native.macro_obs.DETAIL_NAMES),detail_storage='float16-before-inference',
                commands=list(native.COMMAND_KIND_NAMES), maps=[sha256(p) for p in cfg.maps],
                stats=sha256(cfg.stats), opponent=opponent or dict(kind='script'),
                sources={name:sha256(ROOT/'train'/name) for name in
                    ('macro_train.py','macro_policy.py','macro_opponent.py','checkpointing.py')})


def campaign(cfg, episode, opponent=None):
    return native.TrainingCampaign(cfg.maps[episode % len(cfg.maps)], cfg.stats, cfg.seed + episode,
                                  attacker=opponent)


def bootstrap(policy, obs,detail):
    with torch.no_grad():
        _, context,_ = policy.encode_state(*obs,detail)
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


def reference_penalty(current_logp,old_logp,reference_logp):
    """Importance-corrected nonnegative KL estimator for a sampled full command.

    Under old-policy samples, E[p/q * (r/p - 1 - log(r/p))] = KL(p || r).
    Keep gradients through p/q; detaching it changes the objective's gradient.
    All three probabilities must use the same state's full-command legal support.
    """
    delta=reference_logp-current_logp
    return (current_logp-old_logp).exp()*(torch.expm1(delta)-delta)


def isolate_critic(policy):
    """Keep value-head learning from rewriting the actor's pretrained features.

    The actor can still update its encoder through the policy objective. Values
    and inference graphs are unchanged; this only changes gradient routing.
    Re-register on resume, since hooks are intentionally not in state_dict.
    """
    return policy.critic.register_forward_pre_hook(lambda module,args:tuple(x.detach() for x in args))


def train(cfg, folder, updates,init_checkpoint=None):
    if not cfg.maps or min(cfg.rollout,cfg.period,cfg.epochs,cfg.hidden,cfg.max_wave)<1 or updates<0:
        raise ValueError('positive training dimensions and a nonempty map pool required')
    if not 0<cfg.gamma<=1 or not 0<cfg.gae_lambda<=1 or not 0<cfg.clip<1 or cfg.learning_rate<=0:
        raise ValueError('invalid PPO hyperparameters')
    if not math.isfinite(cfg.anchor_weight) or cfg.anchor_weight<0:
        raise ValueError('anchor weight must be finite and nonnegative')
    if cfg.init_from_simulation and (len(cfg.init_from_simulation)!=64 or
            any(c not in '0123456789abcdef' for c in cfg.init_from_simulation)):
        raise ValueError('Expected exact source simulation SHA256 for weight transfer')
    torch.set_num_threads(1)
    random.seed(cfg.seed);np.random.seed(cfg.seed);torch.manual_seed(cfg.seed)
    opponent,opponent_identity=load_opponent(cfg.attacker_model,cfg.stats)
    identity = contract(cfg,opponent_identity)
    with run_lock(folder):
        saved,_ = load_auto(folder)
        policy = MacroPolicy(len(native.macro_obs.CELL_NAMES),len(native.macro_obs.GLOBAL_NAMES),
                             len(native.COMMAND_KIND_NAMES),cfg.hidden,len(native.macro_obs.DETAIL_NAMES))
        optimizer = torch.optim.Adam(policy.parameters(),lr=cfg.learning_rate)
        progress = dict(updates=0,env_steps=0,episode=0,waves_survived=0,defeats=0,history=[])
        commands = []
        initialization='random-defender'
        reference=None
        if saved:
            if saved['config']!=asdict(cfg) or saved['contract']!=identity:
                raise ValueError('Macro run inputs changed; use a new directory')
            policy.load_state_dict(saved['model']);optimizer.load_state_dict(saved['optimizer'])
            progress=saved['progress'];commands=saved['campaign']['commands']
            initialization=saved['initialization']
            if init_checkpoint and (not isinstance(initialization,dict) or
                    initialization.get('checkpoint_sha256')!=sha256(init_checkpoint)):
                raise ValueError('Resume initialization differs from the committed run')
        elif init_checkpoint:
            initialized,_,_=load_policy(init_checkpoint,cfg.stats,cfg.init_from_simulation or None)
            policy.load_state_dict(initialized.state_dict())
            initialization=dict(kind='macro-weights-warm-start',checkpoint_sha256=sha256(init_checkpoint),
                                source_simulation=cfg.init_from_simulation or native.SIMULATION_FINGERPRINT,
                                target_simulation=native.SIMULATION_FINGERPRINT)
        elif cfg.init_from_simulation:
            raise ValueError('Cross-simulation weight transfer requires an initialization checkpoint')
        if cfg.anchor_weight:
            if not saved and not init_checkpoint:
                raise ValueError('Anchored training requires an initialization checkpoint')
            reference=copy.deepcopy(policy).eval()
            if saved:
                if saved.get('reference_model') is None:
                    raise ValueError('Anchored checkpoint has no fixed reference model')
                reference.load_state_dict(saved['reference_model'])
            reference.requires_grad_(False)
        if cfg.detach_critic_features:
            isolate_critic(policy)
        progress.setdefault('simulation_ticks',0)
        progress.setdefault('finished_campaigns',[])
        # Model construction for warm-start validation must not shift rollout RNG.
        if not saved:
            random.seed(cfg.seed);np.random.seed(cfg.seed);torch.manual_seed(cfg.seed)
        world=campaign(cfg,progress['episode'],opponent)
        for command in commands:
            world.advance(cfg.period,[tuple(command)])
        if saved:
            if world.diagnostic_state_hash!=saved['campaign']['hash']:
                raise ValueError('Campaign recovery hash mismatch; refusing to continue')
            restore_rng(saved['rng'])

        def commit():
            check_opponent(cfg.attacker_model,opponent_identity)
            save_run(folder,dict(format=1,config=asdict(cfg),contract=identity,
                progress=progress,status='ready',initialization=initialization,
                model=policy.state_dict(),optimizer=optimizer.state_dict(),rng=rng_state(),
                reference_model=reference.state_dict() if reference is not None else None,
                campaign=dict(commands=commands,hash=world.diagnostic_state_hash,
                              tick=world.tick,wave=world.wave)))
        if not saved:
            commit()
        while progress['updates']<updates:
            rows=[]
            for _ in range(cfg.rollout):
                obs=world.defender_observation()
                detail=world.defender_detail().astype(np.float16)
                with torch.no_grad():
                    decision=policy.decide(*obs,world.candidates(),world.map_shape,detail=detail)
                    reference_logp=(float(reference.rescore(*obs,decision.command,decision.domains,
                        world.map_shape,detail=detail).log_prob) if reference is not None else 0.)
                # The policy is fixed throughout a rollout. This is exactly the
                # preceding nonterminal row's bootstrap, without encoding the
                # same full-resolution city a second time. Never cross a wave.
                if rows and not rows[-1]['terminal']:
                    rows[-1]['next_value']=float(decision.value)
                transition=world.advance(cfg.period,[decision.command])
                commands.append(decision.command)
                terminal=transition['wave_advanced'] or transition['defeated']
                # Sparse outcome only: no repeated reward for building/demolishing or waiting.
                reward=float(transition['wave_advanced'])-float(transition['defeated'])
                rows.append(dict(obs=obs,detail=detail,command=decision.command,domains=decision.domains,
                    shape=world.map_shape,log_prob=float(decision.log_prob),reference_logp=reference_logp,value=float(decision.value),
                    reward=reward,terminal=terminal,ticks=transition['ticks'],
                    next_value=0.))
                progress['waves_survived']+=int(transition['wave_advanced'])
                progress['defeats']+=int(transition['defeated'])
                progress['simulation_ticks']+=transition['ticks']
                if transition['defeated'] or world.wave>cfg.max_wave:
                    progress['finished_campaigns'].append(dict(
                        map=cfg.maps[progress['episode']%len(cfg.maps)],seed=cfg.seed+progress['episode'],
                        ticks=world.tick,waves_survived=world.wave-1,defeated=transition['defeated']))
                    progress['episode']+=1
                    world=campaign(cfg,progress['episode'],opponent);commands=[]
            if rows and not rows[-1]['terminal']:
                rows[-1]['next_value']=bootstrap(policy,world.defender_observation(),
                                                world.defender_detail().astype(np.float16))
            advantages,targets=returns(rows,cfg)
            losses=[]
            penalties=[]
            for _ in range(cfg.epochs):
                optimizer.zero_grad()
                # Accumulate one rollout gradient; don't retain all spatial graphs in memory.
                for index,row in enumerate(rows):
                    d=policy.rescore(*row['obs'],row['command'],row['domains'],row['shape'],detail=row['detail'])
                    ratio=(d.log_prob-row['log_prob']).exp()
                    surrogate=torch.minimum(ratio*advantages[index],
                        ratio.clamp(1-cfg.clip,1+cfg.clip)*advantages[index])
                    penalty=(reference_penalty(d.log_prob,row['log_prob'],row['reference_logp'])
                             if reference is not None else d.log_prob.new_zeros(()))
                    loss=(-surrogate+.5*(d.value-targets[index]).square()+cfg.anchor_weight*penalty)/len(rows)
                    if not torch.isfinite(loss):
                        raise ValueError('Non-finite macro PPO loss; last committed checkpoint preserved')
                    loss.backward();losses.append(float(loss.detach()));penalties.append(float(penalty.detach()))
                torch.nn.utils.clip_grad_norm_(policy.parameters(),.5,error_if_nonfinite=True)
                optimizer.step()
            progress['updates']+=1;progress['env_steps']+=len(rows)
            progress['history'].append(dict(update=progress['updates'],loss=sum(losses)/cfg.epochs,
                reward=sum(r['reward'] for r in rows),tick=world.tick,wave=world.wave,
                episode=progress['episode'],reference_kl_estimate=sum(penalties)/len(penalties)))
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
    parser.add_argument('--init-checkpoint')
    parser.add_argument('--anchor-weight',type=float,default=0.)
    parser.add_argument('--gamma',type=float,default=.995)
    parser.add_argument('--gae-lambda',type=float,default=.95)
    parser.add_argument('--detach-critic-features',action='store_true')
    parser.add_argument('--attacker-model',default='',help='Frozen tactical ONNX opponent; default is script')
    parser.add_argument('--init-from-simulation',default='',
                        help='Explicit source fingerprint for weights-only transfer into a NEW run')
    args=parser.parse_args()
    train(Config(tuple(str(Path(p).resolve()) for p in args.maps),str(Path(args.stats).resolve()),
                 seed=args.seed,rollout=args.rollout,period=args.period,anchor_weight=args.anchor_weight,
                 gamma=args.gamma,gae_lambda=args.gae_lambda,detach_critic_features=args.detach_critic_features,
                 attacker_model=str(Path(args.attacker_model).resolve()) if args.attacker_model else '',
                 init_from_simulation=args.init_from_simulation),
          args.run_dir,args.updates,args.init_checkpoint)
