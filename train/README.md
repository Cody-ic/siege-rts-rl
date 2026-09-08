# 攻方 RL 训练与恢复

当前是 **IPPO 风格的共享编队策略**：13 个离散动作、带迷雾的局部观测，默认连接 `DefenderScript + DefenderMacro`。训练与游戏共用原生仿真，热路径不回调 Python，也不加载渲染器。

这仍是战术层训练：当前固定 9 支亡灵步兵编队、单张地图、一级单位。宏观编成、混兵训练和游戏内模型推理尚未接入，不能把这轮结果称为完整攻方 AI。

## 从服务器重启恢复

服务器重启通常不会删除磁盘文件。**先检查已有权重，不要直接从零重跑或覆盖训练目录。** `screen` 能应对 SSH 断线，不能保存断电时的内存。

2026-09-08 在服务器使用 `torch.load(..., weights_only=True, map_location='cpu')` 只读验证：两组带守方的归档各保留 40,009,728 env-step；空城对照最后保存于 12,288,000 步，日志止于 12,845,056 步，最后 557,056 步没有写入该权重文件。三份文件均能加载，各含 12 个张量且参数全部有限，已有学习成果没有因重启全部丢失。这些旧文件没有优化器与课程状态，只能用于显式热启动或对照评估；可加载也不代表策略已学会获胜。

先构建本分支的绑定，Python 必须有 torch、numpy 和开发头：

```bash
RTS_BINDINGS=1 RTS_TRAINING_TESTS=1 tools/server/build.sh Release
export PYTHONPATH="$PWD/build-Release/bindings"
export PYTHONIOENCODING=utf-8
PY=/data0/am_data/miniforge3/bin/python
```

新实验，每次 PPO 更新结束都会保存：

```bash
$PY train/ppo.py --run-dir runs/attacker-v2 --device cuda \
    --envs 128 --threads 8 --torch-threads 1 --total-steps 1000000
```

服务器重启、SSH 断开或程序失败后，使用**同一个目录**：

```bash
$PY train/ppo.py --run-dir runs/attacker-v2 --resume auto --total-steps 1000000
```

`--total-steps` 是累计目标，不是再加这么多步。已达到目标时不重新训练。`--resume auto` 对空目录允许新开；目录有旧记录却找不到可读检查点时会报错，避免悄悄归零。运行目录受系统文件锁保护；退出或重启自动释放。

检查点保存网络、Adam 状态、随机数状态、累计步数、课程档位、完整终局统计窗口和下一局种子。**未结束的原生对局会用新种子重新生成**，数量记入 `resume_resets`；不把它们计为超时或胜利。这是从完整 PPO 更新继续学习，不承诺与从未中断的训练逐位相同。

- `latest.pt`：恢复依据，完整单文件状态。
- `previous.pt`：上一份可读检查点；最新文件损坏时 `auto` 明确提示并回退。
- `policy.pt`：只含权重的便利导出；恢复训练用 `latest.pt`。
- `state.json`、`metrics.jsonl`：可读状态和逐次更新指标。崩溃留下的半行和超出检查点的指标会在恢复时清理。

先写同目录临时文件、刷新到磁盘，再替换正式文件。默认 `--save-every 1`；强制关机最多丢掉最近一次成功保存后的工作，不保证捕获突然断电那一刻。SIGINT / SIGTERM 在当前采样及更新结束后保存退出；若服务器终止宽限很短，仍依赖上一份周期性保存。

恢复校验观测布局、动作数、原生仿真指纹、地图、数值表和学习器源码。更换奖励、对手、课程或训练规则必须开新实验；可调整累计目标、设备、CPU 线程及保存频率。

## 旧权重仍可利用

```bash
$PY train/ppo.py --run-dir runs/warm-economic-v2 \
    --init-weights /path/to/archived-economic.pt --device cuda \
    --envs 128 --threads 8 --total-steps 1000000
```

这是**热启动**：保留已学权重，新建优化器、计数与课程。旧权重缺少的状态无法凭空恢复。默认回到近距离课程做适应；只有已核实原档位和能力时才使用 `--init-frac 0.5` 等已有课程档位。不要把旧日志步数手填成新实验已完成步数。

旧文件也可能来自旧数值表或旧守方代码，结构相同不代表行为相同。旧实验保留为历史对照，新规则下重新评估；不要覆盖服务器归档。

## 先短实验，再增加预算

```bash
tools/server/run_defender_ab.sh --device cuda --envs 128 --threads 8
```

默认依次跑每组 20 万、100 万步，**每个阶段都做冻结评估**。总计约 300 万训练 env-step，而不是一上来串行跑 1.2 亿。实际步数可能向上取整不到一个环境批次。

| 分组 | 奖励 | 对手 | 胜利奖励 |
|---|---|---|---|
| economic-defender | 经济战果 + 胜利 + PBRS | 真实脚本守方 | 2000 |
| victory-defender | 胜利 + PBRS | 真实脚本守方 | 2000 |
| economic-empty | 经济战果 + 胜利 + PBRS | 空城对照 | 2000 |

三组使用独立目录。A/B 保持胜利奖励尺度一致，避免同时改变奖励模式与尺度；A/C 只变对手开关。全流程串行，不同时抢 GPU。任何训练或评估失败都会停止并返回非零；重跑原命令会恢复未完成阶段，已完成阶段按带哈希的记录跳过。

`summary.json` 汇总真实距离与当前课程距离的胜率、95% Wilson 区间、接触率和建筑伤害。`best.pt` 保存验证集上表现更好的检查点，按真实距离的胜率、拆毁价值、伤害依次比较；`latest.pt` 始终负责续训。

这些固定种子用于阶段验证与选择，**不是最终测试集**。单个训练种子只是初筛。表现稳定后再用 `--seeds 1,2,3` 在新目录复核，并用另一套未参与选择的评估种子（例如 200001）做最终测试：

```bash
$PY train/evaluate.py --checkpoint runs/defender-ab/economic-defender-seed1/best.pt \
    --episodes 128 --seed 200001 --frac 1 --device cuda --output runs/final-test.json
```

确认值得继续后可显式增加阶段预算：

```bash
tools/server/run_defender_ab.sh --milestones 200000,1000000,4000000 --envs 128 --threads 8
```

默认升档需要最近完整终局窗口中至少 60% 打到建筑，且至少 25% **拆毁过建筑或赢下对局**。单纯移动或擦伤建筑不足以升档，拆毁收益仍保留消耗战的合法性。`--promote-outcome-at 0` 可用于旧判据对照，但属于新实验配置，不能偷偷改正在续训的实验。

## 判断卡在哪里

先跑读同一套局部观测与合法动作的简单规则基线：

```bash
$PY train/evaluate.py --policy flow --episodes 32 --frac 1 --output runs/flow-full.json
$PY train/evaluate.py --policy flow --episodes 32 --frac 0.15 --output runs/flow-near.json
```

它只是诊断，不会接管策略动作。2026-09-08 本地 8 局小样本中，近距离 4/8 胜，真实距离 0/8 胜，但两者均 100% 接触建筑。该结果说明有必要分别看课程能力与部署距离；**不能据此断言真实距离不可获胜**。

逐次训练指标记录采样/更新耗时、有效 agent-step、胜利/全灭/超时、战果、熵、近似 KL 与裁剪比例。默认近似 KL 超过 0.03 时停止本次后续更新；非有限 loss 或梯度立即失败并保留上一次检查点。全灭立即结束 episode，不再空转到超时；GAE 通过稳定编队标识匹配后继，不把死亡编队的价值接到下一行编队上。

## 性能与验证

```bash
$PY train/benchmark.py --device cuda --envs 16 --output runs/benchmark.json
```

只把真实编队送入网络并存入观测缓冲；32 行原生接口不变。当前 9 编队配置的观测缓冲减少 **71.875%**。地图与数值表通过 `WorldFactory` 读取一次，重置复用解析结果；每局世界仍独立。

本地 Windows / CPU / torch 2.14 / 单线程、16 局、10 次中位数：完整行前向 17.91 ms，有效行前向 4.61 ms；建局工厂从逐次解析 1.833 ms 到复用 0.043 ms。**这是部件微基准，不是整轮训练提速 3.9 倍或服务器 GPU 测速。** 端到端吞吐必须查看服务器相同配置的采样/更新日志。不要仅凭 GPU 空闲就把共享服务器 CPU 线程开满。

2026-09-08 服务器补验：Linux / GCC 13.3 / Python 3.12 / PyTorch 2.8.0+cu128 / A800 80GB，基础版本 **54/54 CTest 通过**；默认 `cuda` 设备选择修复另通过全部 15 项运行时测试及真实 GPU 启动。GPU 检查点从 64 步恢复到 128 步，优化器继续更新，8 局未结束对局按约定重新生成；旧的 40,009,728 步经济权重成功热启动完成一次更新，三份旧权重的 SHA256 均未改变。

同机 GPU 部件基准（16 局，10 次中位数）：完整行前向 0.842 ms，有效行前向 0.442 ms，约 1.90 倍；建局工厂约 12.14 倍。端到端短跑使用 64 局、4 个仿真线程、1 个 PyTorch 线程、每次采样 64 步，共两次更新 / 8,192 env-step，计入训练计时约 **21.39 秒 / 383 env-step/s**，不含进程启动和初始建局。采样 18.63 秒、更新 2.72 秒，采样占二者合计约 87%。这只是近距离课程开头的短测，尚无完整终局，不能据此评价胜率、外推全程耗时或声称相对旧版的整体提速。下一轮可先用 `--envs 64 --threads 4` 做 20 万步初筛，再根据同机测量调整并行度。

训练测试可加入 CTest：

```bash
cmake -S . -B build-Release -DRTS_BUILD_BINDINGS=ON -DRTS_TEST_TRAINING=ON \
    -DPython3_EXECUTABLE="$PY" -DRTS_TRAIN_PYTHON="$PY"
cmake --build build-Release -j4
ctest --test-dir build-Release --output-on-failure
```

覆盖实际训练与恢复、强杀进程、部分写入、损坏回退、互斥运行、旧权重热启动、随机状态、课程窗口、编队身份 GAE、真假行前向一致、守方重置、固定策略评估与批大小无关。主机没有 PyTorch 时保留普通 C++ 测试；显式开启训练测试后缺依赖会报错。

保存格式参考 [PyTorch 官方检查点说明](https://docs.pytorch.org/tutorials/beginner/saving_loading_models.html)，性能取舍参考 [官方性能指南](https://docs.pytorch.org/tutorials/recipes/recipes/tuning_guide.html)。当前保留 float32，未在未验证的 GPU 上默认启用混合精度或编译优化。
