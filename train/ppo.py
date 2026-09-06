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
    # 升档判据：近 `promote_window` 局里有 `promote_at` 比例拿到过正奖励。
    # **用成功率而不是固定步数**——固定步数会在学不会时硬推到下一档，
    # 而那正好回到「拿不到第一次奖励」那个死结。
    promote_at: float = 0.6
    promote_window: int = 100
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


def make_worlds(cfg: Cfg, n: int, frac: float = 1.0) -> list:
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
    gh = R.obs.UNIT_TYPE_NAMES.index("Ghoul")
    sites = R.map_sites(cfg.map_path)
    spawns = list(sites["spawns"])
    if not spawns:
        raise SystemExit(f"{cfg.map_path} 没有集结点——攻方无处生成")
    squads, per = 9, 3      # 9 支 × 3 = 27 个单位，编队数 9 < MAX_UNITS_PER_ENV
    out = []
    for i in range(n):
        # 每局挑一个集结点（轮换）。**宏观层还没上**，所以这里是轮换而不是
        # 决策——CLAUDE.md「战术层必须先跑通，不要两层同时上」。
        sx, sy = spawns[(cfg.seed + i) % len(spawns)]
        # **课程**：把出生点沿「集结点 → keep」的直线拉近 `frac` 倍。
        # frac = 1.0 是真实距离；小 frac 让随机策略也能撞到目标、拿到
        # 第一次奖励。行军距离是训练侧的课程旋钮，不是设计改动——
        # 真实对局里它恒等于 1.0（`地图与场景设计.md` 的 D=40 是结构约束）。
        kx, ky = sites["keep"]
        sx = kx + (sx - kx) * frac
        sy = ky + (sy - ky) * frac
        atk = []
        for q in range(squads):
            for m in range(per):
                # 7×7 环上错开落位（同 `spawn_wave` 的做法）：同坐标生成会
                # 让单位挤在一起。
                dx = (q % 3) - 1 + m * 0.3
                dy = (q // 3) - 1
                atk.append((gh, sx + 0.5 + dx, sy + 0.5 + dy, 1, q))
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

    # **开跑前看一眼 GPU 还剩多少。** 这台机器四人共用，而
    # `训练服务器环境.md` 明写「GPU 空闲不等于 GPU 归你，跑之前先
    # `nvidia-smi`」。实测撞过一次：别人占 66.7 GiB、只剩 1.2 GiB，
    # 于是 rollout 缓冲一分配就 `OutOfMemoryError`——**而那时已经跑了
    # 五分钟、课程都升到第 4 档了**，日志末尾才是那条异常。早查早报。
    if cfg.device != "cpu" and torch.cuda.is_available():
        free_b, total_b = torch.cuda.mem_get_info()
        print(f"GPU 空闲 {free_b / 2**30:.1f} / {total_b / 2**30:.1f} GiB")
        if free_b < 4 * 2**30:
            print("  ⚠️ 空闲不足 4 GiB —— 别人在用。要么等，要么 --device cpu")

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

    # ——课程：从最近那一档起步——
    stage = 0
    frac = cfg.curriculum[stage]
    print(f"课程 {cfg.curriculum}  起于第 1 档 frac={frac}")
    env = R.BatchedEnv(make_worlds(cfg, cfg.envs, frac), side=R.Side.Attacker,
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

    ar_mu = np.arange(MU, dtype=np.int32).reshape(1, MU)

    def observe() -> tuple:
        env.observe(cells, self_v, glob)
        env.action_masks(masks)
        # **哪些行是真的 agent。** `unit_counts` 是「这一局当前有几个属于
        # `side` 的活 agent」，而张量里**前几段有效**（`observe` 的契约）
        # ⇒ 判据是「行号 < 该局的 agent 数」，不是去猜掩码。
        #
        # 不拿「掩码只剩 Stop」当判据：那与「一个真的 agent 恰好无路可走」
        # 不可区分，而后者在被围住时真的会发生。
        live_np = ar_mu < np.asarray(env.unit_counts, dtype=np.int32).reshape(-1, 1)
        # (N, C, K, K)：torch 的卷积要 channel-first，而 C++ 写的是
        # channel-last（那一侧 `kObsCellFloats` 的布局是 K*K*C）。
        c = torch.from_numpy(cells).to(dev).view(N, K, K, C).permute(0, 3, 1, 2)
        s = torch.from_numpy(self_v).to(dev).view(N, SELF_N)
        # 全局标量对同一局的所有 agent 相同 ⇒ 广播到每一行。
        g = torch.from_numpy(glob).to(dev).unsqueeze(1).expand(-1, MU, -1).reshape(N, GLOB_N)
        # 掩码：uint16 位图 → (N, NA) 的 bool
        mk = torch.from_numpy(masks.astype(np.int32)).to(dev).view(N, 1)
        bits = torch.arange(NA, device=dev).view(1, NA)
        return c, s, g, ((mk >> bits) & 1).bool(), torch.from_numpy(live_np.reshape(N))

    # ——rollout 缓冲——
    #
    # ⚠️ **观测那三块放 CPU（pinned），不放 GPU。** `buf_c` 是
    # `T×N×C×K×K×4` 字节 = 64×4096×14×15×15×4 ≈ **3.1 GiB**，而这台机器
    # 四人共用：实测跑的时候别人占着 66.7 GiB，只剩 1.2 GiB ⇒ 直接
    # `torch.OutOfMemoryError`。（`训练服务器环境.md` 明写「GPU 空闲不等于
    # GPU 归你」。）
    #
    # 而它**本来就不需要常驻显存**：更新时按 minibatch 取一小片，
    # `pin_memory` + `non_blocking` 的搬运在这个尺寸上开销可忽略。
    # 小的那几块（动作/回报/价值）留在 GPU——它们参与 GAE 的逐步递推。
    T = cfg.rollout
    pin = dev != "cpu"
    buf_c = torch.zeros((T, N, C, K, K), pin_memory=pin)
    buf_s = torch.zeros((T, N, SELF_N), pin_memory=pin)
    buf_g = torch.zeros((T, N, GLOB_N), pin_memory=pin)
    buf_m = torch.zeros((T, N, NA), dtype=torch.bool, pin_memory=pin)
    # **哪些行是真 agent。** padding 行占多数（编成 9 支 vs `MAX_UNITS_PER_ENV`
    # 32 ⇒ 28% 有效），把它们喂进更新是**纯浪费 + 稀释**：
    #   * 策略项梯度恒 0（掩码只剩 Stop ⇒ log_prob ≡ 0，与参数无关）
    #   * 价值项却不是 0 —— 它拿全零观测去拟合局级回报，占掉七成价值梯度
    # 所以更新时只取 live 行。见更新那一段。
    buf_live = torch.zeros((T, N), dtype=torch.bool, pin_memory=pin)
    buf_a = torch.zeros((T, N), dtype=torch.long, device=dev)
    buf_lp = torch.zeros((T, N), device=dev)
    buf_v = torch.zeros((T, N), device=dev)
    buf_r = torch.zeros((T, cfg.envs), device=dev)
    buf_d = torch.zeros((T, cfg.envs), device=dev)

    step_count, t_start = 0, time.perf_counter()
    ep_ret = np.zeros((cfg.envs,), dtype=np.float64)
    # **两个列表，别合成一个。** `stage_ret` 是**本档**的回报（升档时清空，
    # 因为换了任务、旧成功率不代表现在）；`all_ret` 是全程累计（只增，用于
    # 显示「一共跑了多少局」）。
    #
    # 合成一个会自相矛盾：我第一版只有 `recent`，升档时 clear ⇒ 日志里
    # 「完成 0 局」与紧邻一行的「上一档成功率 89%」同时出现，而「有战果」
    # 那一列此后恒 0%（每次刚清空就统计）。
    stage_ret: list[float] = []
    all_ret: list[float] = []

    while step_count < cfg.total_steps:
        t_roll = 0.0
        t_upd = 0.0
        _t0 = time.perf_counter()
        for t in range(T):
            c, s, g, mk, lv = observe()
            with torch.no_grad():
                logits, value = net(c, s, g)
                # **掩掉非法动作**（CLAUDE.md 明令要做掩码）。
                logits = logits.masked_fill(~mk, float("-inf"))
                dist = torch.distributions.Categorical(logits=logits)
                act = dist.sample()
                buf_lp[t] = dist.log_prob(act)
            # 搬回 CPU 存（见上面那段：观测缓冲不常驻显存）。
            buf_c[t] = c.to("cpu", non_blocking=True)
            buf_s[t] = s.to("cpu", non_blocking=True)
            buf_g[t] = g.to("cpu", non_blocking=True)
            buf_m[t] = mk.to("cpu", non_blocking=True)
            buf_live[t] = lv
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
                stage_ret.append(float(ep_ret[i]))
                all_ret.append(float(ep_ret[i]))
                ep_ret[i] = 0.0
                env.reset_one(int(i), make_worlds(cfg, 1, frac)[0])

        # ——GAE。奖励是**逐局**的，而 agent 是逐编队的 ⇒ 把局级奖励广播到
        #   该局的所有 agent。这是「共享策略 + 团队奖励」的标准做法，也是
        #   `CLAUDE.md`「奖励：以本波战果为主」那句的直接后果（战果是全队的）。
        # ——升档判据：近 `promote_window` 局里有多少比例拿到过正奖励——
        #
        # **用成功率而不是固定步数**：固定步数会在学不会时硬推到下一档，
        # 而那正好回到「拿不到第一次奖励」那个死结。
        if len(stage_ret) >= cfg.promote_window and stage + 1 < len(cfg.curriculum):
            win = stage_ret[-cfg.promote_window:]
            rate = sum(1 for r in win if r > 0.0) / len(win)
            if rate >= cfg.promote_at:
                stage += 1
                frac = cfg.curriculum[stage]
                stage_ret.clear()   # 换了任务，旧成功率不再代表现在
                print(f"  ↑ 升档：第 {stage + 1}/{len(cfg.curriculum)} 档 "
                      f"frac={frac}（上一档成功率 {rate:.0%}）", flush=True)
                # 全批重置到新距离。**不等旧 episode 自然结束**：那些局面
                # 还在旧课程上，混着两档会让「成功率」这个判据失去意义。
                for i in range(cfg.envs):
                    env.reset_one(i, make_worlds(cfg, 1, frac)[0])
                ep_ret[:] = 0.0

        with torch.no_grad():
            c, s, g, _, _ = observe()
            _, next_v = net(c, s, g)
        t_roll = time.perf_counter() - _t0
        _t0 = time.perf_counter()
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
        # 观测那几块留在 CPU，按 minibatch 搬（见缓冲区那段注释）。
        b_c = buf_c.reshape(T * N, C, K, K)
        b_s = buf_s.reshape(T * N, SELF_N)
        b_g = buf_g.reshape(T * N, GLOB_N)
        b_m = buf_m.reshape(T * N, NA)
        b_a = buf_a.reshape(T * N)
        b_lp = buf_lp.reshape(T * N)
        b_adv = adv.reshape(T * N)
        b_ret = ret.reshape(T * N)
        # **只更新真 agent 的那些行**（见 `buf_live` 那段注释）。编成 9 支、
        # 上限 32 ⇒ 大约七成的行是 padding，滤掉它们既省算力也不再稀释
        # 价值头的梯度。**这不是近似**：那些行的策略梯度本来恒为 0。
        idx = np.nonzero(buf_live.reshape(T * N).numpy())[0]
        n_live = len(idx)
        if n_live == 0:
            raise SystemExit("一个 live agent 都没有 —— 编成或 unit_counts 坏了")
        mb = max(1, n_live // cfg.minibatches)
        for _ in range(cfg.epochs):
            np.random.shuffle(idx)
            for start in range(0, n_live, mb):
                jc = torch.from_numpy(idx[start:start + mb])   # CPU 侧索引
                j = jc.to(dev)
                logits, v = net(b_c[jc].to(dev, non_blocking=True),
                                b_s[jc].to(dev, non_blocking=True),
                                b_g[jc].to(dev, non_blocking=True))
                logits = logits.masked_fill(
                    ~b_m[jc].to(dev, non_blocking=True), float("-inf"))
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

        t_upd = time.perf_counter() - _t0
        dt = time.perf_counter() - t_start
        mean_ret = float(np.mean(stage_ret[-50:])) if stage_ret else float("nan")
        win = stage_ret[-cfg.promote_window:]
        rate = (sum(1 for r in win if r > 0.0) / len(win)) if win else 0.0
        print(f"step {step_count:>9,}  {step_count / dt:>8,.0f} env-step/s  "
              f"档{stage + 1}(f={frac:.2f})  回报 {mean_ret:>9.2f}  "
              f"有战果 {rate:>4.0%}  本档 {len(stage_ret)} 局  "
              f"累计 {len(all_ret)} 局  "
              f"[采样 {t_roll:.1f}s / 更新 {t_upd:.1f}s  "
              f"live {n_live / (T * N):.0%}]", flush=True)

    torch.save(net.state_dict(), "train/ppo_attacker.pt")
    print("权重已存 train/ppo_attacker.pt")
    # **策略权重是跨平台通用的**（回放文件不是——CLAUDE.md 明令不要建立
    # 依赖跨平台回放的工作流）。所以「服务器上发现精彩局 → 在 Windows 上
    # 重现」的正确做法是拿这份权重**重跑**，不是搬回放。


if __name__ == "__main__":
    main()
