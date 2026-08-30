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

    n = 8
    worlds = [
        R.make_world_init(
            "game/data/demo_skirmish.json",
            "game/data/stats_placeholder.json",
            seed=i,
            nominal_level=1,
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
