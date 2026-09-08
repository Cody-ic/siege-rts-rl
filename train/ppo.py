"""攻方战术层的 PPO（CleanRL 单文件风格）。

**这是第一份真正的训练循环**；此前 `train/` 只有 `smoke.py`（证明链路通，不学习）。

## 三条不变量在这份脚本里长什么样

1. **热路径不回调 Python**：一次 `env.step()` 推进 `ticks_per_step` 个 tick，
   奖励在 C++ 侧累计、`take_tally()` 一次读走。这份脚本里**没有任何** per-unit
   或 per-tick 的 Python 回调。
2. 渲染层不参与：`RTS_BUILD_RENDER` 默认 OFF，服务器上根本不配置 `render/`。
3. **in-process**：`import rts_native`，不走 IPC、不序列化状态。

## 布局一律从模块读，不硬编码

`obs.hpp` 文件头写了为什么：两侧不一致时**不会有任何东西报错**，reshape 照样
成功、网络照样收敛——收敛到一个把「墙」当成「敌方血量」的表示上。

## agent = 编队，不是单位

`CLAUDE.md`「攻方 RL 控制的是每一支编队」。所以张量的第二维是
`obs.MAX_UNITS_PER_ENV`（= 编队数上限 32），而**场上单位数约是它的 2.6 倍**
——一支编队 1–3 个同兵种。动作摊给队里每个成员由 C++ 侧做
（`BatchedEnv::step`）。

## 奖励的权重在这里，不在 C++ 侧

C++ 只给「发生了什么」（`take_tally` 的 7 个计数）。**权重是训练超参**，
所以它们是本文件的常量。取值依据见 `REWARD_W` 那段。

跑法（服务器）：

    RTS_BINDINGS=1 tools/server/build.sh Release
    PYTHONPATH=build-Release/bindings PYTHONIOENCODING=utf-8 \
        /data0/am_data/miniforge3/bin/python train/ppo.py --total-steps 200000
"""
from __future__ import annotations

import argparse
from collections import deque
from functools import lru_cache
import json
import time
from dataclasses import asdict, dataclass
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn

import rts_native as R
from learning import CompletionWindow, potential_reward, task_reward

# ——奖励权重。**只有这一处有权重**，C++ 侧只给计数——
#
# 依据（`CLAUDE.md`「RL 设计决策」）：
#
#   * `bld_value` 取 1.0 —— 它已经是**重建成本**（石 + 木的基础造价），
#     而那条规定说「摧毁建筑的即时奖励设为该建筑的重建成本……这个权重等于
#     给玩家造成的实际资源损失，是**有原则的推导而非手工试凑**」。所以它是
#     唯一一个不需要标定的权重，取 1 就是照原文。
#   * `scouts_killed` 是「适中常量」——原文明写它「无法像重建成本那样有原则地
#     推导，给一个适中常量即可」。
#   * 伤害与击杀是 **shaping**，`CLAUDE.md`:「shaping 项**权重必须小**，
#     否则会训出『在城外反复换血但永不推进』的退化策略」。所以它们比
#     `bld_value` 小两三个数量级，而不是「差不多大小」。
#   * `losses` 为负，但**也小**：攻方是亡灵、「不在乎伤亡」，用命换缺口是
#     正当打法（花名册里 `Ram` 那条）。惩罚太重会训出畏战。
REWARD_W = {
    "dmg_to_units": 0.002,   # shaping，小
    "dmg_to_blds": 0.004,    # shaping，小（比对单位略高：拆墙才是目的）
    "units_killed": 0.05,    # shaping，小
    "blds_destroyed": 0.0,   # 已由 bld_value 表达，别重复计一次
    "bld_value": 1.0,        # **有原则的推导**：= 重建成本，见上
    "scouts_killed": 3.0,    # 适中常量，见上
    "losses": -0.001,        # 负但小，见上
    "progress": 0.0,       # 只作位移日志，不把未折扣的距离差当奖励
}


# Phi = 当前存活攻方到堡垒的负距离和。死亡造成的势跳变保留，
# episode 终局（包括任务规定的超时）势归零，折扣累计才会消去整条路径。
POTENTIAL_W = 0.02


@dataclass
class Cfg:
    threads: int = 8
    torch_threads: int = 1
    max_ticks: int = 2400
    save_every: int = 1
    target_kl: float = 0.03
    envs: int = 256
    ticks_per_step: int = 6      # 决策频率：CLAUDE.md 要求 4–8 tick 一次
    rollout: int = 64
    total_steps: int = 200_000
    lr: float = 3e-4
    gamma: float = 0.99
    gae_lambda: float = 0.95
    clip: float = 0.2
    epochs: int = 4
    minibatches: int = 4
    ent_coef: float = 0.01
    vf_coef: float = 0.5
    value_features: str = "shared"  # detached isolates critic gradients from policy features
    reference_coef: float = 0.0     # optional KL(reference || policy), explicit warm starts only
    max_grad_norm: float = 0.5
    seed: int = 1
    # Keep the documented attrition objective by default. Victory-only is an
    # explicit comparison, not a silent rewrite of the game design contract.
    reward_mode: str = "economic"
    win_reward: float = 2000.0  # tunable starting value, NOT an anti-delay bound
    # ——课程学习（2026-09-06）——
    #
    # **不做课程就学不动，这是算出来的**：攻方在集结点离 `keep` 50 格，
    # `Ghoul` 0.08 格/tick、每决策位移 0.48 格、episode 400 个决策 ⇒
    # 随机游走的期望位移只有 `√400 × 0.48` = **9.6 格**，差一个数量级。
    # PPO 初期就是随机策略，拿不到第一次奖励就没有梯度（实测 40 万步
    # 回报恒 0.00，而同一局面用定向策略 400 步能打出 8515 点建筑伤害）。
    #
    # 而 `CLAUDE.md` 本来就铺好了这条路：「**波数即难度轴，天然构成课程
    # 学习**，训练时按波次分层采样，不需要手工设计 curriculum」；
    # `BatchedEnvInit::worlds` 的注释也明写「各局可以不同——按波次分层采样
    # 要的正是『同一批里混着不同波数的局面』」。
    #
    # 阶梯按**到 keep 的距离**（沿集结点→keep 的直线插值）：
    # 0.15 ⇒ 7.5 格（随机游走够得到）、之后每档靠上一档学到的
    # 「朝目标走」迁移过去。
    curriculum: tuple = (0.15, 0.3, 0.5, 0.75, 1.0)
    # 升档判据：至少 `promote_window` 局（保留完整终局批次）中 `promote_at` 比例**真的打到了
    # 建筑**（`dmg_to_blds > 0`）。
    #
    # ⚠️ **判据不能是「拿到过正奖励」，那个版本已经坦过一次。**
    # 加了 `progress` 那一列之后，「往前走了几格」本身就是正奖励
    # ⇒ 这个判据退化成「动了吗」，而那是轻而易举的。实测后果：
    # 1300 局之内一路升到第 5 档（满距离）、「有战果 100%」，
    # 而**建筑伤在大多数 rollout 里是 0** —— 课程被跳过了。
    #
    # 交接 §3 R 写的就是「判据看**有没有战果**，不是 loss」，而那一条对
    # shaping 奖励同样成立：**shaping 不是战果**。
    promote_at: float = 0.6
    # Contact alone doesn't establish combat mastery. Require a completed-episode
    # fraction with a destroyed building OR victory; attrition remains legitimate.
    promote_outcome_at: float = 0.25
    promote_window: int = 100
    map_path: str = "game/data/maps/pool/gen_01001000.json"
    map_pool: tuple = ()  # Optional ordered training pool; world seed selects map.
    roster: str = "ghouls"  # ghouls (legacy control) or mixed combat squads
    tactical_goals: str = "keep"  # known-economy uses only fog-remembered harvesting buildings
    levels: tuple = (1,)
    # ——**接不接真正的守方**（2026-09-07）——
    #
    # 默认 **接**。不接时对侧一动不动，而那是 40M 那轮的局面：
    # 训练图里**一个守方单位都没有** ⇒ `enemy_*` 那几条观测通道
    # 十万局零梯度（`配平工作交接.md` §2.14.3d）。
    #
    # **守方单位不靠摆进去，靠 `DefenderMacro` 自己招**（它会下 `Train`
    # 命令）——那才是真实路径：玩家征兵，而不是地图文件给兵。
    # 所以这里只给地图路径，不用代 `make_world_init` 摆人。
    defender: bool = True
    # 守方宏观层多少个决策拍跑一次。它每次要扫城区 (2R+1)² 格 +
    # 资源点，而它的动作是波次级的（建造要几百 tick）⇒ 不必逐拍跑。
    # **单兵那一半不受它节流**（登墙意愿必须每拍重发）。
    defender_macro_period: int = 4
    defender_prepare_ticks: int = 0  # optional real construction before attackers spawn
    stats_path: str = "game/data/stats_placeholder.json"
    device: str = "cuda" if torch.cuda.is_available() else "cpu"


class Policy(nn.Module):
    """**共享策略网络**（parameter sharing）——所有 agent 共用一个网络。

    `CLAUDE.md`「RL 设计决策（已定，勿擅自更改）」第一条。兵种以 one-hot
    输入区分，而那个 one-hot 已经在自身向量里（`kObsSelfCount = 兵种数 + 3`）。

    观测是 `K×K×C` 的空间张量 + 自身向量 + 全局标量。空间那部分走两层
    卷积——不是为了性能，是因为**平移等变**：同一个「敌人在左前方」的局面
    在网格里的位置会变，全连接要为每个位置各学一遍。
    """

    def __init__(self, k: int, channels: int, self_n: int, glob_n: int, n_act: int):
        super().__init__()
        self.conv = nn.Sequential(
            nn.Conv2d(channels, 32, 3, padding=1), nn.ReLU(),
            nn.Conv2d(32, 32, 3, stride=2, padding=1), nn.ReLU(),
            nn.Flatten(),
        )
        conv_out = 32 * ((k + 1) // 2) ** 2
        self.trunk = nn.Sequential(
            nn.Linear(conv_out + self_n + glob_n, 256), nn.ReLU(),
            nn.Linear(256, 256), nn.ReLU(),
        )
        self.actor = nn.Linear(256, n_act)
        self.critic = nn.Linear(256, 1)

    def forward(self, cells, self_v, glob, *, detach_value=False):
        # cells: (N, C, K, K)；self_v: (N, S)；glob: (N, G)
        z = self.conv(cells)
        h = self.trunk(torch.cat([z, self_v, glob], dim=1))
        return self.actor(h), self.critic(h.detach() if detach_value else h).squeeze(-1)


@lru_cache(maxsize=8)
def world_factory(map_path, stats_path):
    return R.WorldFactory(map_path, stats_path), R.map_sites(map_path)


def episode_map(cfg, episode):
    paths=cfg.map_pool or (cfg.map_path,)
    return paths[(cfg.seed*1000+episode)%len(paths)]


def make_worlds(cfg: Cfg, n: int, frac: float = 1.0, start: int = 0) -> list:
    """造 n 个局面。

    **编成由这一侧给**：`World` 自己不生波（生波在 `game::DemoBattle`，那是
    演示的循环、按占位曲线生兵）。这里先用一个固定编成——**宏观层还没上**，
    而 `CLAUDE.md` 的上线顺序是「战术层必须先跑通，不要两层同时上」。

    编队按 `game::squad_cap_of` 的规格：`Ghoul` 每队 3 个。

    ⚠️ **必须摆在集结点上，而坐标要从地图读**（`R.map_sites`）。
    我第一版写死了 (20.5, 30.5) 那一带——那是 170×170 图的**空角落**，
    离 `keep` 65 格、离最近的集结点 100 多格。后果是 12000 tick 里
    战果恒零、掩码里连一个攻击位都没亮过，而训练**照样跑得很顺**
    （env-step/s 好看、loss 在降、回报恒 nan 因为一局都没结束）。
    这是「摆错地方不报错」那一类静默失败，所以坐标不许写死。
    """
    names=['Ghoul']*9 if cfg.roster=='ghouls' else ['Ghoul']*3+['Shade']*2+['Knight']*2+['Ram','Phoenix']
    types=[R.obs.UNIT_TYPE_NAMES.index(name) for name in names]
    out = []
    for i in range(start, start + n):
        path=episode_map(cfg,i)
        factory,sites=world_factory(path,cfg.stats_path)
        spawns=list(sites['spawns'])
        if not spawns:raise ValueError(f'{path}: no attacker spawns')
        # Traverse maps, then entrances, then levels. Using i % count for all
        # three would permanently pair a map with one entrance/level.
        episode_round=i//max(1,len(cfg.map_pool))
        level=cfg.levels[(episode_round//len(spawns))%len(cfg.levels)]
        # 每局挑一个集结点（轮换）。**宏观层还没上**，所以这里是轮换而不是
        # 决策——CLAUDE.md「战术层必须先跑通，不要两层同时上」。
        sx, sy = spawns[(cfg.seed + episode_round) % len(spawns)]
        # **课程**：把出生点沿「集结点 → keep」的直线拉近 `frac` 倍。
        # frac = 1.0 是真实距离；小 frac 让随机策略也能撞到目标、拿到
        # 第一次奖励。行军距离是训练侧的课程旋钮，不是设计改动——
        # 真实对局里它恒等于 1.0（`地图与场景设计.md` 的 D=40 是结构约束）。
        kx, ky = sites["keep"]
        sx = kx + (sx - kx) * frac
        sy = ky + (sy - ky) * frac
        atk = []
        for q,unit_type in enumerate(types):
            per=3 if cfg.roster=='ghouls' else R.squad_cap(unit_type)
            for m in range(per):
                # 7×7 环上错开落位（同 `spawn_wave` 的做法）：同坐标生成会
                # 让单位挤在一起。
                dx = (q % 3) - 1 + m * 0.3
                dy = (q // 3) - 1
                atk.append((unit_type, sx + 0.5 + dx, sy + 0.5 + dy, level, q))
        out.append(factory.make(seed=cfg.seed * 1000 + i,
                                     nominal_level=level, attackers=atk))
    return out


def validate(cfg):
    if cfg.tactical_goals not in ('keep','known-economy'):
        raise ValueError('tactical_goals must be keep or known-economy')
    if not 0 <= cfg.defender_prepare_ticks <= 2400 or (cfg.defender_prepare_ticks and not cfg.defender):
        raise ValueError('defender_prepare_ticks requires a defender and must be in [0,2400]')
    if cfg.roster not in ('ghouls','mixed'):
        raise ValueError('roster must be ghouls or mixed')
    if not cfg.levels or any(not isinstance(lv,int) or lv<1 or lv>1000 for lv in cfg.levels):
        raise ValueError('levels must be integers in [1,1000]')
    if len(set(cfg.levels))!=len(cfg.levels) or len(set(cfg.map_pool))!=len(cfg.map_pool):
        raise ValueError('Duplicate map/level entries bias the sampling distribution')
    if cfg.value_features not in ('shared','detached'):
        raise ValueError('value_features must be shared or detached')
    if not np.isfinite(cfg.reference_coef) or cfg.reference_coef < 0:
        raise ValueError('reference_coef must be finite and nonnegative')
    for name in ('envs', 'ticks_per_step', 'rollout', 'total_steps', 'epochs',
                 'minibatches', 'torch_threads', 'max_ticks', 'promote_window',
                 'defender_macro_period', 'save_every'):
        if getattr(cfg, name) < 1:
            raise ValueError(f'{name} must be positive')
    if cfg.threads < 0 or not 0 <= cfg.promote_at <= 1 or not 0 <= cfg.promote_outcome_at <= 1 or not 0 < cfg.gamma <= 1:
        raise ValueError('invalid threads, promote_at or gamma')
    for name in ('lr', 'clip', 'gae_lambda', 'ent_coef', 'vf_coef', 'max_grad_norm', 'target_kl'):
        if not np.isfinite(getattr(cfg, name)) or getattr(cfg, name) < 0:
            raise ValueError(f'{name} must be finite and nonnegative')
    if not cfg.curriculum or any(not 0 < f <= 1 for f in cfg.curriculum):
        raise ValueError('curriculum fractions must be in (0,1]')
    if tuple(sorted(set(cfg.curriculum))) != tuple(cfg.curriculum):
        raise ValueError('curriculum must be strictly increasing')
    task_reward(0, False, cfg.reward_mode, cfg.win_reward)


def make_env(cfg, n, frac, start=0, episode_indices=None):
    maps={'defender_maps':list(cfg.map_pool)} if cfg.defender and cfg.map_pool else {}
    if cfg.tactical_goals != 'keep': maps['tactical_goals']=cfg.tactical_goals
    worlds=(make_worlds(cfg,n,frac,start) if episode_indices is None else
            [make_worlds(cfg,1,frac,index)[0] for index in episode_indices])
    if len(worlds)!=n: raise ValueError('Episode index count must match environments')
    return R.BatchedEnv(worlds, side=R.Side.Attacker,
                        defender_map=cfg.map_path if cfg.defender and not cfg.map_pool else '',
                        defender_seed=cfg.seed * 31 + 7,
                        defender_macro_period=cfg.defender_macro_period,
                        defender_prepare_ticks=cfg.defender_prepare_ticks,
                        ticks_per_step=cfg.ticks_per_step, threads=cfg.threads,
                        max_ticks_per_episode=cfg.max_ticks,**maps)


class Observer:
    def __init__(self, env):
        self.env = env
        self.n, self.mu = env.batch_size, R.obs.MAX_UNITS_PER_ENV
        self.k, self.c = R.obs.K, R.obs.CHANNEL_COUNT
        self.cells = np.zeros((self.n, self.mu, self.k, self.k, self.c), np.float32)
        self.own = np.zeros((self.n, self.mu, R.obs.SELF_COUNT), np.float32)
        self.glob = np.zeros((self.n, R.obs.GLOBAL_COUNT), np.float32)
        self.masks = np.zeros((self.n, self.mu), np.uint16)
        self.keys = np.zeros((self.n, self.mu), np.int64)
        self.bits = (1 << np.arange(R.obs.ACTION_COUNT)).astype(np.uint16)

    def read(self):
        self.env.observe(self.cells, self.own, self.glob)
        self.env.action_masks(self.masks)
        self.env.agent_keys(self.keys)
        rows = np.flatnonzero(self.keys.reshape(-1))
        # Native buffers stay at the public 32-row layout; only live rows leave CPU.
        return (rows, self.cells.reshape(-1, self.k, self.k, self.c)[rows],
                self.own.reshape(-1, R.obs.SELF_COUNT)[rows],
                self.glob[rows // self.mu],
                (self.masks.reshape(-1)[rows, None] & self.bits) != 0)


def policy_forward(net, observation, device):
    rows, c, s, g, legal = observation
    if len(rows) == 0:
        return None, torch.empty(0, device=device)
    logits, value = net(torch.from_numpy(c).to(device).permute(0, 3, 1, 2),
                        torch.from_numpy(s).to(device), torch.from_numpy(g).to(device))
    logits = logits.masked_fill(~torch.from_numpy(legal).to(device), float('-inf'))
    return torch.distributions.Categorical(logits=logits), value


def train(cfg, args, saved):
    import random
    import signal
    from checkpointing import FORMAT, atomic_json, contract, load_weights, repair_metrics, restore_rng, rng_state, save_run, sha256
    from rollout import RolloutStorage, advantages

    validate(cfg)
    torch.set_num_threads(cfg.torch_threads)
    torch.manual_seed(cfg.seed)
    np.random.seed(cfg.seed)
    random.seed(cfg.seed)
    signature = contract(R, cfg)
    if saved and signature != saved['contract']:
        raise ValueError('Checkpoint contract differs (map/stats/native/observation/learner). '
                         'Use a new run directory and --init-weights for an explicit warm start.')
    folder = Path(args.run_dir)
    if saved:
        repair_metrics(folder,saved['progress']['env_steps'])
    dev = cfg.device
    if torch.device(dev).type == 'cuda':
        if not torch.cuda.is_available():
            raise ValueError('CUDA requested but unavailable')
        requested = torch.device(dev)
        torch.cuda.set_device(requested.index if requested.index is not None else torch.cuda.current_device())
        free, total = torch.cuda.mem_get_info()
        print(f'GPU free {free / 2**30:.1f}/{total / 2**30:.1f} GiB', flush=True)

    net = Policy(R.obs.K, R.obs.CHANNEL_COUNT, R.obs.SELF_COUNT,
                 R.obs.GLOBAL_COUNT, R.obs.ACTION_COUNT).to(dev)
    opt = torch.optim.Adam(net.parameters(), lr=cfg.lr, eps=1e-5)
    initialization = saved.get('initialization') if saved else (
        {'kind':'warm_start', 'path':str(Path(args.init_weights).resolve()),
         'sha256':sha256(args.init_weights)} if args.init_weights else {'kind':'random', 'seed':cfg.seed})
    stage = cfg.curriculum.index(args.init_frac) if args.init_frac is not None else 0
    step_count = updates = wins = timeouts = eliminated = completed = completed_steps = 0
    curriculum_resets = resume_resets = stage_episodes = 0
    next_episode = 0
    elapsed_before = 0.0
    stage_ret = deque(maxlen=50)
    stage_hits = CompletionWindow(cfg.promote_window)
    stage_wins = CompletionWindow(cfg.promote_window)
    stage_outcomes = CompletionWindow(cfg.promote_window)
    if saved:
        net.load_state_dict(saved['model'])
        opt.load_state_dict(saved['optimizer'])
        p = saved['progress']
        stage, step_count, updates = p['stage'], p['env_steps'], p['updates']
        wins, timeouts, eliminated = p['wins'], p['timeouts'], p['eliminated']
        completed, completed_steps = p['completed'], p['completed_steps']
        curriculum_resets = p['curriculum_resets']
        resume_resets = p['resume_resets']
        elapsed_before = p['elapsed_seconds']
        next_episode = p['next_episode']
        stage_episodes = p['stage_episodes']
        stage_ret.extend(p['recent_returns'])
        stage_hits.load_state_dict(p['stage_hits'])
        stage_wins.load_state_dict(p['stage_wins'])
        stage_outcomes.load_state_dict(p['stage_outcomes'])
        if step_count >= cfg.total_steps:
            print(f'Already reached {step_count:,} / {cfg.total_steps:,} steps; checkpoint unchanged.')
            return 0
        resume_resets += p['active_partial_episodes']
        print(f'Resume {step_count:,} steps, stage {stage + 1}; '
              f'restart {p["active_partial_episodes"]} unfinished native episodes.', flush=True)
    elif args.init_weights:
        net.load_state_dict(load_weights(args.init_weights))
        print('Warm start: policy weights retained; new optimizer, counters and curriculum. '
              'Old rewards and win rates are not carried into this run.', flush=True)

    reference = None
    if cfg.reference_coef:
        import copy
        if not saved and not args.init_weights:
            raise ValueError('Reference regularization requires explicit --init-weights')
        # Deepcopy consumes no RNG; paired runs retain the same sampling stream.
        reference = copy.deepcopy(net).eval().requires_grad_(False)
        if saved:
            if not isinstance(saved.get('reference_model'),dict):
                raise ValueError('Missing reference policy in resumable checkpoint')
            reference.load_state_dict(saved['reference_model'])
        if any(not torch.isfinite(v).all() for v in reference.state_dict().values()):
            raise ValueError('Nonfinite reference policy')

    frac = cfg.curriculum[stage]
    world_factory.cache_clear()
    fresh=saved['progress'].get('fresh_episode_indices',[None]*cfg.envs) if saved else [None]*cfg.envs
    if len(fresh)!=cfg.envs: raise ValueError('Checkpoint episode index count mismatch')
    episode_indices=[]
    for index in fresh:
        if index is None:
            index=next_episode
            next_episode+=1
        elif not isinstance(index,int) or isinstance(index,bool) or not 0<=index<next_episode:
            raise ValueError('Invalid checkpoint fresh episode index')
        episode_indices.append(index)
    env = make_env(cfg, cfg.envs, frac, episode_indices=episode_indices)
    obs = Observer(env)
    mu, n, T = obs.mu, cfg.envs * obs.mu, cfg.rollout
    capacity = sum(env.unit_counts)
    if capacity == 0:
        raise ValueError('No live agents in initial roster')
    storage = RolloutStorage(T, capacity, obs.k, obs.c, R.obs.SELF_COUNT,
                             R.obs.GLOBAL_COUNT, R.obs.ACTION_COUNT,
                             pin=torch.device(dev).type == 'cuda')
    print(f'Reward={cfg.reward_mode}; defender={cfg.defender}; stage={stage+1} frac={frac}; '
          f'envs={cfg.envs}; native_threads={cfg.threads}; torch_threads={cfg.torch_threads}', flush=True)
    print(f'Rollout observation storage {storage.bytes / 2**20:.1f} MiB; '
          f'{capacity}/{n} live rows at start.', flush=True)
    buf_a = torch.zeros((T, n), dtype=torch.long, device=dev)
    buf_lp = torch.zeros((T, n), device=dev)
    buf_v = torch.zeros((T, n), device=dev)
    buf_r = torch.zeros((T, cfg.envs), device=dev)
    buf_d = torch.zeros((T, cfg.envs), device=dev)
    buf_next_potential = torch.zeros((T, cfg.envs), device=dev)
    keys = np.zeros((T + 1, cfg.envs, mu), np.int64)
    actions = np.zeros((cfg.envs, mu), np.uint8)
    done = np.zeros(cfg.envs, np.uint8)
    tally = np.zeros((cfg.envs, R.obs.TALLY_FIELDS), np.float32)
    weights = np.array([REWARD_W[name] for name in R.obs.TALLY_NAMES], np.float32)
    dmg_index = R.obs.TALLY_NAMES.index('dmg_to_blds')
    value_index = R.obs.TALLY_NAMES.index('bld_value')
    ep_ret, ep_hit = np.zeros(cfg.envs), np.zeros(cfg.envs)
    ep_value = np.zeros(cfg.envs)
    ep_steps = np.zeros(cfg.envs, np.int64)
    phi = np.asarray(env.potentials)
    if saved:
        restore_rng(saved['rng'])  # after constructing the new network and environments
    t_start, session_start_step, session_updates = time.perf_counter(), step_count, 0
    stop = [0]
    handlers = {}
    def request_stop(signum, _frame):
        stop[0] = signum
    for sig in (signal.SIGINT, signal.SIGTERM):
        handlers[sig] = signal.signal(sig, request_stop)

    def progress():
        return dict(env_steps=step_count, updates=updates, stage=stage, frac=frac,
                    wins=wins, timeouts=timeouts, eliminated=eliminated,
                    completed=completed, completed_steps=completed_steps,
                    curriculum_resets=curriculum_resets, resume_resets=resume_resets,
                    active_partial_episodes=int(np.count_nonzero(ep_steps)),
                    next_episode=next_episode, stage_episodes=stage_episodes,
                    fresh_episode_indices=[index if ep_steps[i]==0 else None for i,index in enumerate(episode_indices)],
                    recent_returns=list(stage_ret), stage_hits=stage_hits.state_dict(),
                    stage_wins=stage_wins.state_dict(),
                    stage_outcomes=stage_outcomes.state_dict(),
                    elapsed_seconds=elapsed_before + time.perf_counter() - t_start)

    def save(status):
        save_run(folder, dict(format=FORMAT, config=asdict(cfg), contract=signature,
                              initialization=initialization,
                              model=net.state_dict(), optimizer=opt.state_dict(),
                              reference_model=reference.state_dict() if reference is not None else None,
                              progress=progress(), rng=rng_state(), status=status))

    atomic_json(folder / 'config.json', asdict(cfg))
    # A recoverable initial point exists even if the first rollout crashes.
    if not saved:
        save('running')
    try:
        while step_count < cfg.total_steps and not stop[0]:
            sample_start = time.perf_counter()
            roll_tally = np.zeros(R.obs.TALLY_FIELDS, np.float64)
            buf_v.zero_()
            # The final update uses only collected samples, with no uninitialized tail.
            horizon = min(T, max(1, (cfg.total_steps - step_count + cfg.envs - 1) // cfg.envs))
            for t in range(horizon):
                observation = obs.read()
                rows = observation[0]
                keys[t] = obs.keys
                storage.store(t, *observation)
                with torch.no_grad():
                    dist, value = policy_forward(net, observation, dev)
                    actions.fill(0)
                    if dist is not None:
                        act = dist.sample()
                        buf_a[t, rows], buf_v[t, rows] = act, value
                        buf_lp[t, rows] = dist.log_prob(act)
                        actions.reshape(-1)[rows] = act.cpu().numpy().astype(np.uint8)
                env.step(actions, done)
                env.take_tally(tally)
                ends = env.episode_ends
                won = np.array([e == R.EpisodeEnd.KeepDestroyed for e in ends])
                after = np.asarray(env.potentials)
                # Vectorized form of the same PBRS and task_reward equations.
                rew = (tally @ weights) if cfg.reward_mode == 'economic' else np.zeros(cfg.envs)
                rew = rew + won * cfg.win_reward + POTENTIAL_W * (cfg.gamma * after * (1-done) - phi)
                buf_r[t] = torch.from_numpy(rew).to(dev)
                buf_d[t] = torch.from_numpy(done.astype(np.float32)).to(dev)
                buf_next_potential[t] = torch.from_numpy(POTENTIAL_W * after * (1-done)).to(dev)
                roll_tally += tally.sum(0)
                ep_ret += rew
                ep_hit += tally[:, dmg_index]
                ep_value += tally[:, value_index]
                ep_steps += 1
                step_count += cfg.envs
                finished = np.flatnonzero(done)
                stage_hits.add(ep_hit[finished])
                stage_wins.add(won[finished])
                stage_outcomes.add(won[finished] | (ep_value[finished] > 0))
                for i in finished:
                    wins += int(won[i])
                    timeouts += int(ends[i] == R.EpisodeEnd.Timeout)
                    eliminated += int(ends[i] == R.EpisodeEnd.AttackersEliminated)
                    completed += 1
                    stage_episodes += 1
                    completed_steps += int(ep_steps[i])
                    stage_ret.append(float(ep_ret[i]))
                    ep_ret[i] = ep_hit[i] = ep_steps[i] = 0
                    ep_value[i] = 0
                    env.reset_one(int(i), make_worlds(cfg, 1, frac, next_episode)[0])
                    episode_indices[int(i)]=next_episode
                    next_episode += 1
                phi = np.asarray(env.potentials)
            with torch.no_grad():
                end_obs = obs.read()
                keys[horizon] = obs.keys
                _, value = policy_forward(net, end_obs, dev)
                next_v = torch.zeros(n, device=dev)
                next_v[end_obs[0]] = value
            if torch.device(dev).type == 'cuda':
                torch.cuda.synchronize()
            sample_seconds = time.perf_counter() - sample_start
            update_start = time.perf_counter()
            adv = advantages(buf_v[:horizon], buf_r[:horizon], buf_d[:horizon],
                             keys[:horizon+1], next_v, cfg.gamma, cfg.gae_lambda,
                             buf_next_potential[:horizon])
            ret = adv + buf_v[:horizon]
            packed, native = storage.indices(horizon, n)
            count = len(packed)
            # A dead squad's in-flight projectile can still settle this episode.
            # Zero actor samples during that bounded tail are legitimate.
            b_adv, b_ret = adv.reshape(-1), ret.reshape(-1)
            a, lp = buf_a.reshape(-1), buf_lp.reshape(-1)
            # Tiny final batches are valid: population std avoids the singleton NaN.
            minibatch = max(1, (count + cfg.minibatches - 1) // cfg.minibatches)
            order = np.arange(count)
            losses, kls, clips, entropies = [], [], [], []
            policy_losses, value_losses, reference_kls = [], [], []
            stopped_kl = False
            for epoch in range(cfg.epochs):
                np.random.shuffle(order)
                for offset in range(0, count, minibatch):
                    chosen = order[offset:offset+minibatch]
                    jc = torch.from_numpy(packed[chosen])
                    j = torch.as_tensor(native[chosen], device=dev)
                    cells = storage.c.flatten(0, 1)[jc].to(dev).permute(0, 3, 1, 2)
                    own = storage.s.flatten(0, 1)[jc].to(dev)
                    glob = storage.g.flatten(0, 1)[jc].to(dev)
                    legal = storage.m.flatten(0, 1)[jc].to(dev)
                    logits, value = net(cells,own,glob,detach_value=cfg.value_features=='detached')
                    logits = logits.masked_fill(~legal, float('-inf'))
                    dist = torch.distributions.Categorical(logits=logits)
                    logratio = dist.log_prob(a[j]) - lp[j]
                    ratio = logratio.exp()
                    kl = ((ratio - 1) - logratio).mean()
                    # Stop before applying another oversized update.
                    if cfg.target_kl and kl.item() > cfg.target_kl:
                        stopped_kl = True
                        break
                    advantage = b_adv[j]
                    advantage = (advantage-advantage.mean()) / (advantage.std(unbiased=False)+1e-8)
                    pg = -torch.min(ratio * advantage,
                                    ratio.clamp(1-cfg.clip, 1+cfg.clip) * advantage).mean()
                    vf = .5 * (value-b_ret[j]).square().mean()
                    entropy = dist.entropy().mean()
                    loss = pg + cfg.vf_coef * vf - cfg.ent_coef * entropy
                    ref_kl = None
                    if reference is not None:
                        with torch.no_grad():
                            ref_logits,_ = reference(cells,own,glob)
                            ref_dist = torch.distributions.Categorical(logits=ref_logits.masked_fill(~legal,float('-inf')))
                        ref_kl = torch.distributions.kl_divergence(ref_dist,dist).mean()
                        loss = loss + cfg.reference_coef * ref_kl
                    if not torch.isfinite(loss):
                        raise FloatingPointError('Non-finite PPO loss; last good checkpoint retained')
                    opt.zero_grad(set_to_none=True)
                    loss.backward()
                    nn.utils.clip_grad_norm_(net.parameters(), cfg.max_grad_norm, error_if_nonfinite=True)
                    opt.step()
                    losses.append(float(loss.detach()))
                    kls.append(float(kl.detach()))
                    clips.append(float(((ratio-1).abs() > cfg.clip).float().mean().detach()))
                    entropies.append(float(entropy.detach()))
                    policy_losses.append(float(pg.detach()))
                    value_losses.append(float(vf.detach()))
                    if ref_kl is not None:
                        reference_kls.append(float(ref_kl.detach()))
                if stopped_kl:
                    break
            # GAE and update finished against old task before a curriculum reset.
            trained_stage, trained_frac = stage, frac
            promoted = False
            if (stage_hits.ready and stage + 1 < len(cfg.curriculum)
                    and stage_hits.rate >= cfg.promote_at
                    and stage_outcomes.rate >= cfg.promote_outcome_at):
                stage += 1
                frac = cfg.curriculum[stage]
                promoted = True
                print(f'  ↑ 升档：第 {stage+1} 档 frac={frac}（完整批次 {stage_hits.count} 局）', flush=True)
                curriculum_resets += int(np.count_nonzero(ep_steps))
                stage_ret.clear()
                stage_hits.clear()
                stage_wins.clear()
                stage_outcomes.clear()
                stage_episodes = 0
                for i, world in enumerate(make_worlds(cfg, cfg.envs, frac, next_episode)):
                    episode_indices[i]=next_episode+i
                    env.reset_one(i, world)
                next_episode += cfg.envs
                ep_steps.fill(0)
                ep_ret.fill(0)
                ep_hit.fill(0)
                ep_value.fill(0)
                phi = np.asarray(env.potentials)
            updates += 1
            session_updates += 1
            if torch.device(dev).type == 'cuda':
                torch.cuda.synchronize()
            update_seconds = time.perf_counter() - update_start
            elapsed = time.perf_counter() - t_start
            row = dict(env_steps=step_count, updates=updates, trained_stage=trained_stage,
                       trained_frac=trained_frac, stage=stage, frac=frac,
                       sample_seconds=sample_seconds, update_seconds=update_seconds,
                       env_steps_per_second=(step_count-session_start_step)/max(elapsed,1e-9),
                       live_agent_steps=count, observation_bytes=storage.bytes,
                       loss=float(np.mean(losses)) if losses else None,
                       policy_loss=float(np.mean(policy_losses)) if policy_losses else None,
                       value_loss=float(np.mean(value_losses)) if value_losses else None,
                       reference_kl=float(np.mean(reference_kls)) if reference_kls else None,
                       reference_coef=cfg.reference_coef,
                       approx_kl=float(np.mean(kls)) if kls else None,
                       clip_fraction=float(np.mean(clips)) if clips else None,
                       entropy=float(np.mean(entropies)) if entropies else None,
                       kl_early_stop=stopped_kl, wins=wins, timeouts=timeouts,
                       optimizer_minibatches=len(losses),
                       eliminated=eliminated, completed=completed,
                       hit_rate=stage_hits.rate, window_win_rate=stage_wins.rate,
                       outcome_rate=stage_outcomes.rate,
                       window_count=stage_hits.count,
                       mean_return=float(np.mean(stage_ret)) if stage_ret else None,
                       tally=dict(zip(R.obs.TALLY_NAMES, roll_tally.tolist())))
            with (folder/'metrics.jsonl').open('a', encoding='utf-8') as log:
                log.write(json.dumps(row, ensure_ascii=False, allow_nan=False)+'\n')
            print(f'step {step_count:,}  {row["env_steps_per_second"]:,.0f} env-step/s  '
                  f'档{stage+1}(f={frac:.2f}) 打到建筑 {stage_hits.rate:.0%} '
                  f'胜利 {wins} / 超时 {timeouts} / 全灭 {eliminated} / 升档中断 {curriculum_resets} '
                  f'完成局均长 {completed_steps/max(1,completed):.1f}步 '
                  f'[采样 {sample_seconds:.2f}s / 更新 {update_seconds:.2f}s]', flush=True)
            limited = args.stop_after_updates and session_updates >= args.stop_after_updates
            finished = step_count >= cfg.total_steps
            if finished or stop[0] or limited or promoted or updates % cfg.save_every == 0:
                save('complete' if finished else 'paused' if stop[0] or limited else 'running')
            if limited:
                break
        if stop[0]:
            save('paused')
            print('Stop requested; checkpoint saved at completed update boundary.', flush=True)
        return 128 + stop[0] if stop[0] else 0
    finally:
        for sig, handler in handlers.items():
            signal.signal(sig, handler)


def main():
    from dataclasses import fields
    from checkpointing import load_auto, load_training, run_lock
    ap = argparse.ArgumentParser(description='Resumable attacker IPPO, native scripted defender, no rendering')
    default = Cfg()
    for field in fields(default):
        name, value = field.name, getattr(default, field.name)
        if name == 'defender':
            ap.add_argument('--no-defender', action='store_true', default=None)
        elif name == 'curriculum':
            ap.add_argument('--curriculum', type=lambda s: tuple(float(x) for x in s.split(',')))
        elif name == 'map_pool':
            ap.add_argument('--map-pool',type=lambda s:tuple(s.split(',')))
        elif name == 'levels':
            ap.add_argument('--levels',type=lambda s:tuple(int(x) for x in s.split(',')))
        else:
            ap.add_argument('--'+name.replace('_','-'), type=type(value), default=None)
    ap.add_argument('--run-dir', default='runs/attacker')
    source = ap.add_mutually_exclusive_group()
    source.add_argument('--resume', nargs='?', const='auto')
    source.add_argument('--init-weights')
    ap.add_argument('--init-frac', type=float)
    ap.add_argument('--stop-after-updates', type=int, default=0)
    args = ap.parse_args()
    with run_lock(args.run_dir):
        saved = None
        if args.resume == 'auto':
            saved, _ = load_auto(args.run_dir)
        elif args.resume:
            saved = load_training(args.resume)
        elif any((Path(args.run_dir)/n).exists() for n in ('latest.pt','previous.pt','metrics.jsonl','policy.pt')):
            raise ValueError('Run directory contains a previous run; use --resume auto or a new --run-dir')
        cfg = Cfg(**saved['config']) if saved else default
        cfg.curriculum = tuple(cfg.curriculum)
        cfg.map_pool = tuple(cfg.map_pool)
        cfg.levels = tuple(cfg.levels)
        runtime = {'total_steps', 'device', 'threads', 'torch_threads', 'save_every'}
        for field in fields(default):
            name = field.name
            value = (not args.no_defender) if name == 'defender' and args.no_defender is not None else getattr(args, name, None)
            if value is not None:
                if saved and name not in runtime and value != getattr(cfg, name):
                    raise ValueError(f'Cannot change {name} while resuming; use --init-weights in a new run')
                setattr(cfg, name, value)
        if saved and args.init_frac is not None:
            raise ValueError('--init-frac is only for a new run')
        if args.stop_after_updates < 0:
            raise ValueError('--stop-after-updates must be nonnegative')
        return train(cfg, args, saved)


if __name__ == '__main__':
    raise SystemExit(main())
