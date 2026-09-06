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
import time
from dataclasses import dataclass

import numpy as np
import torch
import torch.nn as nn

import rts_native as R

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
}


@dataclass
class Cfg:
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
    max_grad_norm: float = 0.5
    seed: int = 1
    map_path: str = "game/data/maps/pool/gen_01001000.json"
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

    def forward(self, cells, self_v, glob):
        # cells: (N, C, K, K)；self_v: (N, S)；glob: (N, G)
        z = self.conv(cells)
        h = self.trunk(torch.cat([z, self_v, glob], dim=1))
        return self.actor(h), self.critic(h).squeeze(-1)


def make_worlds(cfg: Cfg, n: int) -> list:
    """造 n 个局面。

    **编成由这一侧给**：`World` 自己不生波（生波在 `game::DemoBattle`，那是
    演示的循环、按占位曲线生兵）。这里先用一个固定编成——**宏观层还没上**，
    而 `CLAUDE.md` 的上线顺序是「战术层必须先跑通，不要两层同时上」。

    编队按 `game::squad_cap_of` 的规格：`Ghoul` 每队 3 个。
    """
    gh = R.obs.UNIT_TYPE_NAMES.index("Ghoul")
    squads, per = 9, 3      # 9 支 × 3 = 27 个单位，编队数 9 < MAX_UNITS_PER_ENV
    out = []
    for i in range(n):
        atk = []
        for q in range(squads):
            for m in range(per):
                # 摆在同一带上、逐队错开，避免生成在同一坐标
                atk.append((gh, 20.5 + m + q * 0.25, 30.5 + q, 1, q))
        out.append(R.make_world_init(cfg.map_path, cfg.stats_path,
                                     seed=cfg.seed * 1000 + i,
                                     nominal_level=1, attackers=atk))
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    for f, t in (("envs", int), ("rollout", int), ("total-steps", int),
                 ("lr", float), ("seed", int), ("device", str)):
        ap.add_argument(f"--{f}", type=t, default=None)
    a = ap.parse_args()
    cfg = Cfg()
    for f in ("envs", "rollout", "lr", "seed", "device"):
        if getattr(a, f) is not None:
            setattr(cfg, f, getattr(a, f))
    if a.total_steps is not None:
        cfg.total_steps = a.total_steps

    torch.manual_seed(cfg.seed)
    np.random.seed(cfg.seed)

    # ——布局一律从模块读（见文件头）——
    K, C = R.obs.K, R.obs.CHANNEL_COUNT
    SELF_N, GLOB_N = R.obs.SELF_COUNT, R.obs.GLOBAL_COUNT
    MU, NA = R.obs.MAX_UNITS_PER_ENV, R.obs.ACTION_COUNT
    TF = R.obs.TALLY_FIELDS
    print(f"obs 版本 {R.obs.VERSION} 指纹 {R.obs.LAYOUT_FINGERPRINT:#x}")
    print(f"K={K} 通道={C} 自身={SELF_N} 全局={GLOB_N} agent上限={MU} 动作={NA}")
    print(f"战果列: {R.obs.TALLY_NAMES}")
    print(f"设备 {cfg.device}  局数 {cfg.envs}  rollout {cfg.rollout}")

    # 奖励权重按 C++ 给的列序排成一个向量。**照名字对齐，不按位置猜**——
    # 位置错了不会报错，只会让「击杀数」被当成「自身损失」。
    w = np.array([REWARD_W[n] for n in R.obs.TALLY_NAMES], dtype=np.float32)

    env = R.BatchedEnv(make_worlds(cfg, cfg.envs), side=R.Side.Attacker,
                       ticks_per_step=cfg.ticks_per_step, threads=0)

    # ——缓冲区由 Python 持有、反复复用**（不是每步 new 一块）。
    cells = np.zeros((cfg.envs, MU, K, K, C), dtype=np.float32)
    self_v = np.zeros((cfg.envs, MU, SELF_N), dtype=np.float32)
    glob = np.zeros((cfg.envs, GLOB_N), dtype=np.float32)
    masks = np.zeros((cfg.envs, MU), dtype=np.uint16)
    tally = np.zeros((cfg.envs, TF), dtype=np.float32)
    acts_np = np.zeros((cfg.envs, MU), dtype=np.uint8)
    done_np = np.zeros((cfg.envs,), dtype=np.uint8)

    net = Policy(K, C, SELF_N, GLOB_N, NA).to(cfg.device)
    opt = torch.optim.Adam(net.parameters(), lr=cfg.lr, eps=1e-5)

    N = cfg.envs * MU     # 一步里有多少个 agent 决策
    dev = cfg.device

    def observe() -> tuple:
        env.observe(cells, self_v, glob)
        env.action_masks(masks)
        # (N, C, K, K)：torch 的卷积要 channel-first，而 C++ 写的是
        # channel-last（那一侧 `kObsCellFloats` 的布局是 K*K*C）。
        c = torch.from_numpy(cells).to(dev).view(N, K, K, C).permute(0, 3, 1, 2)
        s = torch.from_numpy(self_v).to(dev).view(N, SELF_N)
        # 全局标量对同一局的所有 agent 相同 ⇒ 广播到每一行。
        g = torch.from_numpy(glob).to(dev).unsqueeze(1).expand(-1, MU, -1).reshape(N, GLOB_N)
        # 掩码：uint16 位图 → (N, NA) 的 bool
        mk = torch.from_numpy(masks.astype(np.int32)).to(dev).view(N, 1)
        bits = torch.arange(NA, device=dev).view(1, NA)
        return c, s, g, ((mk >> bits) & 1).bool()

    # rollout 缓冲
    T = cfg.rollout
    buf_c = torch.zeros((T, N, C, K, K), device=dev)
    buf_s = torch.zeros((T, N, SELF_N), device=dev)
    buf_g = torch.zeros((T, N, GLOB_N), device=dev)
    buf_m = torch.zeros((T, N, NA), dtype=torch.bool, device=dev)
    buf_a = torch.zeros((T, N), dtype=torch.long, device=dev)
    buf_lp = torch.zeros((T, N), device=dev)
    buf_v = torch.zeros((T, N), device=dev)
    buf_r = torch.zeros((T, cfg.envs), device=dev)
    buf_d = torch.zeros((T, cfg.envs), device=dev)

    step_count, t_start = 0, time.perf_counter()
    ep_ret = np.zeros((cfg.envs,), dtype=np.float64)
    recent: list[float] = []

    while step_count < cfg.total_steps:
        for t in range(T):
            c, s, g, mk = observe()
            with torch.no_grad():
                logits, value = net(c, s, g)
                # **掩掉非法动作**（CLAUDE.md 明令要做掩码）。
                logits = logits.masked_fill(~mk, float("-inf"))
                dist = torch.distributions.Categorical(logits=logits)
                act = dist.sample()
                buf_lp[t] = dist.log_prob(act)
            buf_c[t], buf_s[t], buf_g[t], buf_m[t] = c, s, g, mk
            buf_a[t], buf_v[t] = act, value

            acts_np[:] = act.view(cfg.envs, MU).to("cpu").numpy().astype(np.uint8)
            env.step(acts_np, done_np)
            env.take_tally(tally)

            rew = (tally * w).sum(axis=1)
            buf_r[t] = torch.from_numpy(rew).to(dev)
            buf_d[t] = torch.from_numpy(done_np.astype(np.float32)).to(dev)
            ep_ret += rew
            step_count += cfg.envs

            # 终局的局重置。**重置时机归 train/**（`BatchedEnv::reset_one` 的
            # 注释：要按波次分层采样，重置成哪一波是训练侧的决定）。
            for i in np.nonzero(done_np)[0]:
                recent.append(float(ep_ret[i]))
                ep_ret[i] = 0.0
                env.reset_one(int(i), make_worlds(cfg, 1)[0])

        # ——GAE。奖励是**逐局**的，而 agent 是逐编队的 ⇒ 把局级奖励广播到
        #   该局的所有 agent。这是「共享策略 + 团队奖励」的标准做法，也是
        #   `CLAUDE.md`「奖励：以本波战果为主」那句的直接后果（战果是全队的）。
        with torch.no_grad():
            c, s, g, _ = observe()
            _, next_v = net(c, s, g)
        adv = torch.zeros_like(buf_v)
        last = torch.zeros(N, device=dev)
        for t in reversed(range(T)):
            r = buf_r[t].unsqueeze(1).expand(-1, MU).reshape(N)
            d = buf_d[t].unsqueeze(1).expand(-1, MU).reshape(N)
            nv = next_v if t == T - 1 else buf_v[t + 1]
            delta = r + cfg.gamma * nv * (1.0 - d) - buf_v[t]
            last = delta + cfg.gamma * cfg.gae_lambda * (1.0 - d) * last
            adv[t] = last
        ret = adv + buf_v

        # ——更新——
        b_c = buf_c.reshape(T * N, C, K, K)
        b_s = buf_s.reshape(T * N, SELF_N)
        b_g = buf_g.reshape(T * N, GLOB_N)
        b_m = buf_m.reshape(T * N, NA)
        b_a = buf_a.reshape(T * N)
        b_lp = buf_lp.reshape(T * N)
        b_adv = adv.reshape(T * N)
        b_ret = ret.reshape(T * N)
        idx = np.arange(T * N)
        mb = (T * N) // cfg.minibatches
        for _ in range(cfg.epochs):
            np.random.shuffle(idx)
            for start in range(0, T * N, mb):
                j = torch.from_numpy(idx[start:start + mb]).to(dev)
                logits, v = net(b_c[j], b_s[j], b_g[j])
                logits = logits.masked_fill(~b_m[j], float("-inf"))
                dist = torch.distributions.Categorical(logits=logits)
                lp = dist.log_prob(b_a[j])
                ratio = (lp - b_lp[j]).exp()
                a_mb = b_adv[j]
                a_mb = (a_mb - a_mb.mean()) / (a_mb.std() + 1e-8)
                pg = -torch.min(ratio * a_mb,
                                ratio.clamp(1 - cfg.clip, 1 + cfg.clip) * a_mb).mean()
                vloss = 0.5 * (v - b_ret[j]).pow(2).mean()
                loss = pg + cfg.vf_coef * vloss - cfg.ent_coef * dist.entropy().mean()
                opt.zero_grad()
                loss.backward()
                nn.utils.clip_grad_norm_(net.parameters(), cfg.max_grad_norm)
                opt.step()

        dt = time.perf_counter() - t_start
        mean_ret = float(np.mean(recent[-50:])) if recent else float("nan")
        print(f"step {step_count:>9,}  {step_count / dt:>9,.0f} env-step/s  "
              f"回报(近50局) {mean_ret:>10.2f}  完成 {len(recent)} 局",
              flush=True)

    torch.save(net.state_dict(), "train/ppo_attacker.pt")
    print("权重已存 train/ppo_attacker.pt")
    # **策略权重是跨平台通用的**（回放文件不是——CLAUDE.md 明令不要建立
    # 依赖跨平台回放的工作流）。所以「服务器上发现精彩局 → 在 Windows 上
    # 重现」的正确做法是拿这份权重**重跑**，不是搬回放。


if __name__ == "__main__":
    main()
