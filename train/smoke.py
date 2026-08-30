"""绑定层冒烟：证明 Python 侧真的能驱动仿真、拿到张量。

**它不是训练脚本**，是「不变量 3 落地了」的可执行证据：Python 经 pybind11
in-process 直接调 C++，不走 IPC、不做状态序列化。

跑法（服务器上）：

    PYTHONPATH=build-py/bindings /data0/am_data/miniforge3/bin/python train/smoke.py

**注意那个 Python 不是系统 `python3`**：系统那个没有 `Python.h`（编不了绑定），
也没有 torch。带开发头 + torch 2.8.0+cu128 的是 miniforge 那份。
"""
import time

import numpy as np

import rts_native as R


def main() -> None:
    # **一律从模块读布局，不硬编码。** `obs.hpp` 文件头写了为什么：
    # 两侧不一致时不会有任何东西报错，reshape 照样成功、网络照样收敛，
    # 收敛到一个把「墙」当成「敌方血量」的表示上。
    print(f"obs 版本 {R.obs.VERSION}  指纹 {R.obs.LAYOUT_FINGERPRINT:#x}")
    print(f"K={R.obs.K}  通道={R.obs.CHANNEL_COUNT}  自身={R.obs.SELF_COUNT}  "
          f"全局={R.obs.GLOBAL_COUNT}  每局最多={R.obs.MAX_UNITS_PER_ENV}")
    print("前六条通道:", [c[0] for c in R.obs.CHANNELS[:6]])
    print("动作:", R.obs.ACTION_NAMES)

    GHOUL = R.obs.UNIT_TYPE_NAMES.index("Ghoul")
    n = 8
    worlds = [
        R.make_world_init(
            "game/data/demo_skirmish.json",
            "game/data/stats_placeholder.json",
            seed=i,
            nominal_level=1,
            # **本波编成由这一侧给** —— `World` 自己不生波（生波在
            # `game::DemoBattle` 里，那是演示的循环）。不给的话这批局面
            # 永远没有攻方单位，实测推 2400 tick 仍然是 0。
            # 这里手摆几个 Ghoul 只为冒烟；真正的编成来自宏观层。
            attackers=[(GHOUL, 20.5 + k, 18.5, 1) for k in range(3 + i % 3)],
        )
        for i in range(n)
    ]
    env = R.BatchedEnv(worlds, side=R.Side.Attacker, ticks_per_step=6, threads=4)

    mu = R.obs.MAX_UNITS_PER_ENV
    # **缓冲区由 Python 持有、反复复用**（见 module.cpp 文件头）：
    # 每步 new 一块在这个规模上是 1.2 MB 的垃圾。
    cells = np.zeros((n, mu, R.obs.K, R.obs.K, R.obs.CHANNEL_COUNT), dtype=np.float32)
    selfv = np.zeros((n, mu, R.obs.SELF_COUNT), dtype=np.float32)
    glob = np.zeros((n, R.obs.GLOBAL_COUNT), dtype=np.float32)
    done = np.zeros(n, dtype=np.uint8)

    env.observe(cells, selfv, glob)
    print(f"批 {env.batch_size}  各局单位数 {env.unit_counts}")
    print(f"cells 非零 {int((cells != 0).sum())}  自身向量非零 {int((selfv != 0).sum())}")

    # **这里必须停下来看一眼那个 0。** demo 地图开局**没有攻方单位**
    # （它们是每波生成的），所以刚才那次 observe 一个单位都没打包过——
    # 于是「跑通了」只证明了调用链通，没证明张量对。
    #
    # 初版就停在这儿，吞吐数字因此虚高了一个量级。**空批的吞吐不是吞吐。**
    if sum(env.unit_counts) == 0:
        print("⚠ 开局没有攻方单位（每波才生成），下面先推到有兵再测吞吐")
        acts0 = np.zeros((n, mu), dtype=np.uint8)
        for _ in range(400):                      # 推到进攻阶段
            env.step(acts0, done)
            if sum(env.unit_counts) > 0:
                break
        env.observe(cells, selfv, glob)
        print(f"  推进后各局单位数 {env.unit_counts}")
        print(f"  cells 非零 {int((cells != 0).sum())}  "
              f"自身向量非零 {int((selfv != 0).sum())}")

    acts = np.full((n, mu), R.action.AtkNear, dtype=np.uint8)
    steps = 50
    t0 = time.perf_counter()
    for _ in range(steps):
        env.observe(cells, selfv, glob)
        env.step(acts, done)
    dt = time.perf_counter() - t0
    ticks = steps * n * 6
    print(f"{steps} 步 × {n} 局 × 6 tick = {ticks} tick，用时 {dt:.2f}s "
          f"⇒ {ticks / dt:,.0f} tick/秒")
    print("done:", done.tolist())
    print("前三局哈希:", [f"{env.state_hash(i):#018x}" for i in range(3)])


if __name__ == "__main__":
    main()
