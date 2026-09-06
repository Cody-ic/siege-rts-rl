# `train/`

RL 训练侧的入口目录。目前只有一份冒烟脚本，**不是**训练脚本——训练循环
（CleanRL 单文件风格 PPO）还没开工，这份 README 记的是「现在有什么、
下一步是什么」，不是完整方案。

## 文件

- `smoke.py`：证明不变量 3（`bindings/` in-process 调 C++，不走 IPC）
  真的落地——Python 经 `rts_native`（`bindings/` 编出来的扩展模块）驱动
  `rts::BatchedEnv`，拿到打包好的观测张量，量一次吞吐，对一次状态哈希。
  **它不做任何学习**，只验证链路通。

## 怎么跑

先按 `bindings/CMakeLists.txt` 文件头把 `RTS_BUILD_BINDINGS=ON` 编出来
（需要一个**带开发头**的 Python，未必是系统 `python3`）：

```bash
# 服务器（Linux/GCC）：见 tools/server/build.sh 的 RTS_BINDINGS=1 口子
RTS_BINDINGS=1 tools/server/build.sh Release
PYTHONPATH=build-Release/bindings /data0/am_data/miniforge3/bin/python train/smoke.py
```

**本机 Windows/MSVC 也验证过能跑**（2026-08-31，python.org 发行版 3.13，
自带开发头与 numpy）：

```bash
cmake -B build-bindings -DRTS_BUILD_BINDINGS=ON \
      -DPython3_EXECUTABLE=<你的 python.exe 路径>
cmake --build build-bindings --config Release --target rts_native
PYTHONPATH=build-bindings/bindings/Release PYTHONIOENCODING=utf-8 \
      <你的 python.exe 路径> train/smoke.py
```

`PYTHONIOENCODING=utf-8` 是 Git Bash 中文输出乱码那个老问题的同一个成因
（`CLAUDE.md`「确定性禁令有一条可执行的守卫」附近那段），不是这里新引入的坑。
Windows 商店版 Python 没有开发头，编不过；要用 python.org 官方安装包
或 miniforge/conda 这类自带头文件的发行版。

## 实测吞吐（2026-09-06，第一次记录）

A800 80GB + 2×Xeon Gold 6542Y（96 线程），Release/GCC，`ticks_per_step=6`：

| 局数 | 线程 | 单位/局 | tick/s | **agent-step/s** |
|---|---|---|---|---|
| 8 | 4 | 40 | 17,926 | 119,509 |
| 64 | 32 | 40 | 40,315 | 268,765 |
| 128 | 64 | 40 | 50,205 | 334,703 |
| 256 | 96 | 40 | 70,288 | 468,584 |
| **512** | **96** | **40** | **95,913** | **639,418** |

**并行局数是主要杠杆**（8 → 512 局，吞吐 ×5.4）；线程打满 96 之后靠更多局
继续摊薄开销。按 1e8 agent-step 算，512 局并行下约 **43 分钟** ⇒
**机时不是瓶颈**（印证 #22 那条「要估的是 `BatchedEnv` 吞吐，不是 GPU 机时」）。

⚠️ 上表是**纯环境**吞吐。`ppo.py` 实跑是 ~1,600–2,200 env-step/s，因为那是
`envs × 每步` 的口径且含前向/反向——两个数不可直接比。

## `ppo.py`：跑得通，但**还学不动**，原因已定位

第一份真正的训练循环（CleanRL 单文件风格）。链路全通：观测 → 掩码 → 采样 →
`step` → `take_tally` → GAE → PPO 更新 → 存权重。**48 条 ctest 全绿。**

**但 40 万步回报恒 0.00**，而这不是「训练不够久」。逐条排查掉的：

| 查了什么 | 结论 |
|---|---|
| 奖励管道 | ✅ 通。定向策略（一路朝 keep 走）400 步打出建筑伤害 8515、拆 5 座 |
| episode 会不会结束 | ✅ 会（加了 `max_ticks_per_episode` 之后） |
| 编成摆位 | ✅ 修过一次（原先摆在图的空角落，离 keep 65 格） |
| 方向场 | ✅ 修过一次（`observe` 原先传 `nullptr`，14 条通道只有 4 条非零） |
| **探索** | ❌ **这是根因**，见下 |

**根因是探索，不是实现。** 算术：

- 攻方在集结点，离 `keep` **50 格**
- `Ghoul` 速度 0.08 格/tick，`ticks_per_step=6` ⇒ 每个决策位移 0.48 格
- episode 上界 2400 tick = 400 个决策
- 随机游走的期望位移 ≈ `√400 × 0.48` = **9.6 格**

⇒ **差一个数量级。** PPO 初期就是随机策略，它要先「偶然走到 50 格外」才能拿到
第一次奖励——而那个概率极低。这是稀疏奖励下的经典探索失败。

**三条候选出路**（都还没做，需要拍板）：

1. **课程学习**：先从离 keep 近的局面训起，逐步拉远。`CLAUDE.md` 本来就说
   「波数即难度轴，天然构成课程学习，训练时按波次分层采样」——而
   `BatchedEnvInit::worlds` 的注释也明写「各局可以不同，按波次分层采样要的
   正是『同一批里混着不同波数的局面』」。**这一条最贴既有设计。**
2. **给行军一个小 shaping**（如「离 keep 更近」给微小正奖励）。**风险明确**：
   `CLAUDE.md`「shaping 项**权重必须小**，否则会训出『在城外反复换血但永不
   推进』的退化策略」——而这一条恰恰是奖励「推进」，方向相反，但仍要小心
   它会不会盖过战果项。
3. **训练时缩短行军距离**（把攻方摆在离城更近处）。最省事，但它改变的是
   任务本身 ⇒ 学出来的策略在真实距离上未必成立。

## 已知局限（不回避）

- **PPO 学不动**，见上（探索问题，非实现缺陷）。
- ~~没有训练脚本~~ **已有**（`ppo.py`）。`smoke.py` 之后的下一步是一份真正的 PPO 循环（网络结构、
  超参、CleanRL 风格的单文件实现），那是一次独立的、需要新设计决策的工作，
  没有随这份骨架一起做。
- **`守方决策层` 与宏观层暂未接入**：`smoke.py` 里手摆的攻方编成只为冒烟，
  真正的编成来自宏观层（尚未开工，见 `README.md`「当前未认领的工作」第 8 项）。
- 首版由 @Cody-ic 在训练服务器（Linux/GCC）上调通，含一处 `-fPIC` 链接失败
  与一处 GIL 释放顺序导致的 SIGSEGV，均已修复（提交历史见 `bindings/`）。
  MSVC 侧此前从未编译过，合并时补了一处遗漏的编译选项才通过（见
  `bindings/CMakeLists.txt` 该处注释）——这正是「两套工具链尽早互相暴露对方
  漏洞」的一次真实案例，不是假设。
