# 攻方 RL 训练与恢复

## 守方与冻结攻方联合训练

### 原生宏观推理（尚未接入游戏界面）

`export_macro.py` 现在导出 `defender-macro-onnx-v2`，清单保存图文件与数值表的
SHA256 和 FNV64 内容身份。C++ `game::MacroPolicy` 读取清单，核对观测通道、
合法命令注册表、精度、图内容及数值表，再以只读守方视图产生一条合法命令。
Python `native.DefenderPolicy(directory, stats_path).decide(campaign)` 调用同一 C++ 实现。
旧 v1 导出保留，但需重新导出 v2 才能使用此加载器；原训练绑定目录不要覆盖。

2026-09-09 实测 2 地图 × 2 种子 × 16 局面的 C++ / PyTorch 命令及执行后状态一致，
并验证观测版本、周期、维度、选择模式和损坏图文件拒绝。
记录见 `docs/rl-results/2026-09-09-macro-native-runtime.json`。
这些局面主要选择等待及建造，不能代替所有动作、完整对局与多分辨率的覆盖。
原生默认 conditional-greedy；传入 `native.PolicyRng(seed)` 到
`policy.decide(campaign, rng)` 则按合法动作概率采样。随机流由每场对局持有，
可通过 `rng.state` 保存/恢复四个 uint32；拒绝全零状态，成功决策后才提交四次随机抽取。
每个条件分布使用稳定 softmax 和逆累积分布采样。此随机算法与 PyTorch 不同，
不能将相同整数种子下的两套命令序列或前面的胜率直接等同。
采样分布与异常输入 13 项断言通过；两个独立进程完成前 64 条重放、后 64 条新命令及
随机状态/对局状态完全一致，覆盖六种命令。
记录见 `docs/rl-results/2026-09-09-macro-native-sampling.json`。
正式 `GameShell` / `DemoBattle` 已支持可选守方模型，每场对局使用独立随机流。
`BattleArchive` v5 保存守方模型身份和随机状态；快照损坏时按原模型重放，
自动命令不重复写入玩家事件。普通存档仍为 v3，仅攻方模型存档仍为 v4。
真实模型检查可在 ONNX 测试构建后运行：

```text
macro_game_check MAP.json STATS.json MODEL_DIRECTORY FRESH_SAVE_PATH.json
```

已验证快照恢复及日志回放继续到 1800 tick、40 次状态对照一致，以及错误模型/随机状态拒绝。
游戏现支持 `--defender-policy <v2导出目录>`，可与攻方 `--rl-policy` 同时使用：

```text
build-onnx/render/Release/rts_render.exe --battle --rl-policy runs/prepared-final-20260909/export/attacker.onnx --defender-policy runs/macro-joint-20260909/export-native-v2
```

路径为本地已验证产物；模型不在 Git 中，其他机器需取得模型包。
HUD 显示 `Defender AI`，有攻方模型时同时显示受控编队数量。双方模型在真实游戏中
同时运行到 1200 tick 的截图已核对。默认存档目录增加
`rl/<攻方身份或script>/defender/<守方身份>`，旧的仅攻方路径保持不变。
显式 `--save-dir` 仍尊重调用者指定的目录，测试时请使用独立位置。
仓库 `play.bat` 仍启动普通游戏，不默认让 AI 替玩家经营。
本地联合试玩包为 `.review/rl-joint-preview-20260909.zip`，含 521 个校验文件：
`play.bat` 玩家对抗攻方模型，`watch-ai.bat` 双方 AI，`play-script.bat` 普通脚本。
已从仓库外工作目录启动包内程序，自动定位包内素材并运行到 1200 tick。
打包工具 `tools/package_rl_preview.py` 检查模型与数值表、逐文件验证 ZIP，输出不可覆盖。
该验证仍使用当前 Windows 主机，不等于全新系统运行库兼容性验收。

原生完整对局验证：`macro_game_evaluate` 直接运行正式 `DemoBattle`，
`native_macro_evaluate.py` 按冻结协议运行，`native_macro_compare.py` 检查完整成对结果。
四图各两新种子上，原模型平均 3.625 波、新模型 4.0 波，六波完成数由 0/8 到 2/8；
三组改善、两组退步、三组持平。记录见 `docs/rl-results/2026-09-09-macro-native-evaluation.json`。
这与 Python 采样评估分开记录；暂不能宣称普遍提升或掌握 70 波。

宏观 PPO 采样复用下一次决策已经算出的价值，只在非终止批次末尾额外推理一次。
跨波和败局仍截断回报。原实现与优化版在两次更新、32 决策和三次完整败局中，
模型、优化器、固定参考、随机状态及对局完全一致；8 项训练回归测试通过。
单次本地对照约 13.68/13.40 秒，且有评估同时运行，不能据此宣称稳定的整体提速。
旧实验恢复须使用其保存的源码版本（本轮联合训练为 `d5020e3`），不要修改合约绕过检查。

`macro_train.py --attacker-model path/to/attacker.onnx` 使用已有攻方战术模型陪练。
绑定需同时开启 `RTS_BUILD_BINDINGS=ON`、`RTS_WITH_ONNX=ON`；Windows 构建会把
ONNX Runtime DLL 和许可证复制到绑定旁。默认不指定模型时仍为脚本对手。
每次训练只加载一次模型，跨地图、重置和状态分支共享推理实例，世界状态各自独立。
模型 SHA256 与原生模型身份进入检查点；更换对手或原地替换模型必须新建训练目录。

`macro_evaluate.py` 默认使用检查点记录的对手并核对身份，也可显式
`--attacker-model path/to/other.onnx` 做交叉评估。报告记录实际对手，
`macro_compare.py` 拒绝把不同对手的结果当作同条件提升。
旧宏观权重的原生指纹与新绑定不同时，默认仍拒绝加载。新训练可显式指定
`--init-checkpoint old/latest.pt --init-from-simulation <旧检查点原生SHA256>`：
仅导入模型权重，优化器、随机状态和对局重新初始化，并在新检查点记录来源、目标指纹。
其余观测通道、动作、数值表和模型源码仍严格核对；旧目录恢复不允许改版本。
这是显式迁移学习，不代表两个仿真版本普遍等价，不要修改旧检查点指纹。
`macro_transfer_probe.py` 可分别用旧/新绑定记录同一模型的确定性输出。
本次 2 地图 × 2 种子 × 9 局面（0–2000 tick）的观测、合法命令、模型输出和状态哈希
逐项完全一致，见 `docs/rl-results/2026-09-09-macro-transfer.json`。

真实模型集成检查（单独运行，不能以空模型代替）：

```text
python tests/macro_opponent_check.py --model path/to/attacker.onnx --output fresh-evidence.json
```

2026-09-09 本地 CPU 验证：攻方模型确实改变战斗，分支推进到 1800 tick 一致，
守方两次小更新到 2000 tick 的连续训练与恢复训练，模型参数、随机状态和对局状态完全一致；
换回脚本对手续训被拒绝。这是联合训练接口与恢复验证，尚不是协同演化胜率提升证据。

当前是 **IPPO 风格的共享编队策略**：13 个离散动作、带迷雾的局部观测，默认连接 `DefenderScript + DefenderMacro`。训练与游戏共用原生仿真，热路径不回调 Python，也不加载渲染器。

这仍是战术层训练。默认保留 9 支一级亡灵步兵的对照设置；可选多地图、五种战斗兵种和多等级采样，并能导出 ONNX 在游戏内推理。多地图混兵结果仍在验证，宏观编成学习尚未完成，不能把这轮结果称为完整攻方 AI。已验证的稳定性与部署边界见 [实验记录](../docs/rl-stability-and-deployment.md)。

## 从服务器重启恢复

服务器重启通常不会删除磁盘文件。**先检查已有权重，不要直接从零重跑或覆盖训练目录。** `screen` 能应对 SSH 断线，不能保存断电时的内存。

当前学习器合约为 v5：加入编队阵亡时的势函数奖励边界修正、可选冻结参考策略，以及多地图/兵种/等级设置。较旧的完整检查点必须配原源码继续，或用 `--init-weights` 在新目录明确迁移；不能把改变奖励或训练分布后的运行称为原实验的无缝续跑。

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

## 可选：示范初始化后再接 PPO

当策略存在固定方向偏好、多个入口长期无法接触建筑时，可先用同一套局部观测和合法动作收集 flow 规则示范，学习基础行进与接敌，再**另开 PPO 运行目录**。示范只参与初始化；PPO 和游戏推理不调用老师、不替换网络动作，13 个动作、观测布局、奖励与检查点合约保持原样。示范损失不训练价值函数，后续仍需 PPO 学习战果和价值。

以下是本轮已验证的 CPU 试验参数；示范工具本身仅运行于 CPU。它默认处理五种距离、每种 4 个环境各 300 次决策，共 **6000 teacher env-step**，每 4 次决策采样一次有效编队观测，本图得到 12,252 条示范。`--steps` 是每个环境、每种距离的决策数，并非完整对局数。

```bash
$PY train/demonstrations.py collect --out-dir runs/navigation-data
$PY train/demonstrations.py fit --data runs/navigation-data/demonstrations.npz \
    --run-dir runs/navigation-fit --epochs 12

# 仅在冻结评估确认初始化已能从各入口推进后，采用本轮的实际距离对照参数。
$PY train/ppo.py --run-dir runs/navigation-ppo \
    --init-weights runs/navigation-fit/policy.pt --device cpu \
    --envs 16 --threads 2 --torch-threads 2 --curriculum 1.0 --total-steps 32768
```

`--curriculum 1.0` 是这次对照的显式设置，**没有改动原训练的默认课程**。随机初始化对照使用相同参数并去掉 `--init-weights`，但两组只有 PPO 步数相同：示范组另付了示范收集与拟合成本，不能称为总算力等价。先用 `evaluate.py` 在实际距离、每个入口、两种冻结执行方式下检查结果；示范拟合准确率并不是游戏表现。

中断后，收集命令可以原样重跑；完整距离分块会复用，只有未写完的分块需要重采。拟合使用：

```bash
$PY train/demonstrations.py fit --data runs/navigation-data/demonstrations.npz \
    --run-dir runs/navigation-fit --epochs 12 --resume
```

拟合每个完整 epoch 保存模型、Adam、CPU/NumPy 随机状态和已完成轮数，`--epochs` 是累计目标；最新文件损坏时明确回退 `previous.pt`。强杀最多丢掉尚未保存的拟合轮次，SIGINT/SIGTERM 在轮次结束保存退出。运行目录有系统互斥锁，数据、来源代码和拟合参数改变时拒绝混用旧结果；线程数和累计轮数可调整。完成后重跑不会重新拟合。最终数据与临时分块分开，拟合器拒绝把分块当作完整数据。

`navigation-fit/latest.pt` 是**示范拟合检查点**，不是 PPO 续训存档。交给 PPO 的是导出的 `policy.pt`，旁边的 `initialization.json` 记录数据、老师代码和权重校验值。PPO 开始后按原有方式使用 `navigation-ppo/latest.pt` 与 `--resume auto`。更换机器、原生绑定或平台时仍按原合约检查，必要时显式使用权重热启动；不要把 Windows 的完整训练状态冒充服务器原环境续跑。

本轮同图与另一张地图的结果、成本及限制见 [示范初始化试验](../docs/rl-demonstration-pilot.md)。这仍是单训练种子的初筛，没有达到完整游戏 AI 的交付条件。

### 可选：学习通过破口继续推进

`flow` 老师优先攻击射程内的建筑、墙和敌军，可能在已有通路时继续拆邻墙。新增显式选项 `flow_breach`：优先攻击射程内的非墙建筑，否则沿局部方向场前进，由游戏原有的碰撞破坏机制处理实际挡路的建筑；没有可选行进方向时才回退到原攻击规则。它会放过部分旁侧城墙和近身敌军，不是适用于所有兵种的最优战术，也不取消经济消耗战的价值。

```bash
$PY train/evaluate.py --policy flow_breach --episodes 32 --frac 1 \
    --output runs/breach-baseline.json
$PY train/demonstrations.py collect --out-dir runs/breach-data --teacher flow_breach
$PY train/demonstrations.py fit --data runs/breach-data/demonstrations.npz \
    --run-dir runs/breach-fit --epochs 12
```

后续仍用导出的 `policy.pt` 显式初始化新的 PPO 目录。老师选择写入收集计划与初始化说明；同一目录改换老师会拒绝执行。`flow` 仍是默认值和原对照，网络推理不调用任何老师。新版本更改了示范/评估源码指纹，旧示范目录应保留原版本恢复，或在新目录重新收集；已有 PPO 检查点合约未改，仍可按原方式续训。结果、对照与局限见 [破口推进试验](../docs/rl-breach-pilot.md)。

**示范后接 PPO 也可能退步。** 将初始化权重和冻结评估报告单独保留，在短预算后用相同地图、入口、种子与执行方式复测，胜局、拆毁价值和接触率一起比较。`latest.pt` 只表示最近一次可恢复状态，不代表最佳部署模型。本轮新老师初始化的突破成绩在默认学习率短训后下降，因此不能未经复测就扩大到百万步，或用最新权重覆盖已经验证的初始化产物。

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

### 按入口与动作定位推进问题

评估现在始终输出 `by_spawn`：每个入口的局数、胜局、接触建筑局数、建筑伤害与累计推进量。`rows` 保留每局的入口下标和世界种子；入口坐标见 `spawns`。同一地图的入口轮换不能当作多地图泛化。

```bash
export CUDA_VISIBLE_DEVICES=  # 在 CPU 上诊断，避免辅助路径初始化 CUDA
$PY train/evaluate.py --checkpoint runs/example/latest.pt --device cpu \
    --episodes 32 --seed 100001 --frac 1 --policy frozen_argmax \
    --diagnostics --output runs/diagnostic-argmax.json
$PY train/evaluate.py --checkpoint runs/example/latest.pt --device cpu \
    --episodes 32 --seed 100001 --frac 1 --policy frozen_sample \
    --diagnostics --output runs/diagnostic-sample.json
```

两种方式都冻结权重。默认 `frozen_argmax` 不变；`frozen_sample` 按合法动作的概率抽样，每局使用独立随机流，在环境槽位重用时按新局编号重置，避免并行批大小和其他局的结束顺序改变其随机数。不同设备、PyTorch 版本和浮点运算形状仍可能造成概率末位差异，不能承诺跨平台逐位相同。一次采样评估也不足以估计随机策略的完整分布。

开启 `--diagnostics` 后，每局增加动作次数、没有合法移动/没有导航方向的次数、顺着/背离导航方向移动的次数、射程内存在合法攻击目标却未选攻击的次数、首次建筑接触决策步与平均最大动作概率。计数单位是**编队决策次数**，不是游戏 tick 或逐个士兵的次数。`trace` 默认每 25 次决策记录一次累计建筑伤害、累计推进量、存活编队数与距离势；`--trace-every` 可调整间隔。首次接触时间可用决策步乘 `ticks_per_step` 换算为报告所在决策拍末的 tick。

这是状态与动作诊断，**不是单位坐标回放**。推进量来自原生 `progress`（存活匹配单位到堡垒距离变化之和），距离势是当前存活攻方到堡垒的负距离和，死亡也会改变它；不要把距离势的变化直接解释成行军距离。顺着流场也不必然是战术上的最佳选择，存在攻击机会时选择移动也可能是合理战术，需要连同战果判断。

诊断不改奖励、课程、网络、原生观察布局与检查点格式。报告记录评估器及诊断模块的哈希；实验计划也校验这两个文件。升级评估代码后，旧 `experiments.py` 运行目录会拒绝混用新协议，应保留原版本续跑，或单独调用评估器写入新报告路径。完整检查点仍按原有合约验证；旧权重若跨规则或跨平台复测，必须明确注明条件，不能伪装成同环境成绩。

逐次训练指标记录采样/更新耗时、有效 agent-step、胜利/全灭/超时、战果、熵、近似 KL 与裁剪比例。默认近似 KL 超过 0.03 时停止本次后续更新；非有限 loss 或梯度立即失败并保留上一次检查点。全灭立即结束 episode，不再空转到超时；GAE 通过稳定编队标识匹配后继，不把死亡编队的价值接到下一行编队上。

## 性能与验证

### 完整多波环境（宏观训练基础）

新原生接口 `rts_native.TrainingCampaign(map_path, stats_path, seed)` 直接运行游戏的
完整波次循环，而非固定九队编成的单波近似。`advance_scripted(max_ticks)` 使用真实
守方宏观脚本；`advance(max_ticks, commands)` 接受玩家级命令，自动微操仍由游戏执行层负责。
命令为 `(kind, slot, what, level)`，`kind` 从 `COMMAND_KIND_NAMES` 查询。

两者在跨波或败局时提前返回，返回实际推进 tick 数、波次边界、败局状态及双方战果增量。
跨波不清空城市、经济、迷雾或幸存者。`fork()` 在内存中复制完整游戏与脚本状态，
可用同一局势对比不同决策；它不是跨启动的磁盘检查点。`diagnostic_state_hash` 只用于
确定性核验，禁止当成策略观测。

`defender_observation()` 返回独立持有内存的 float32 数组 `(cells, global)`，形状由
`macro_obs.GRID`、`CELL_NAMES` 和 `GLOBAL_NAMES` 确定。区域统计只包含守方单位和
当前可见的敌方单位；迷雾外敌人的数量、血量和等级均不进入观测。资源点位置沿用地图公开信息。
单位/建筑数量及血量比例之和除以 16，等级之和除以 32；可见/探索覆盖率为区域格子比例，
资源点数量除以 16。全局资源除以 1000、波次除以 70、等级及人口除以 32，血量保留比例。
这些值不裁剪，后期可以超过 1；布局版本为 `macro_obs.VERSION`。

布局 v2 新增 `defender_detail()`：完整地图分辨率的 HWC float32 数组，通道表来自
`macro_obs.DETAIL_NAMES`。包含地形可建位、存活障碍、三类资源点、每种己方建筑的独热位、
血量比例、等级/32、完工/升级/施工/训练状态，以及可见/已探索位。其余标志均为 0/1。
不包含敌方单位信息；地形与资源点沿用游戏公开地图信息。相邻城墙不会再合并成同一区域。
训练、示范与评估都先将此数组转为 float16 再送入网络，网络运算仍为 float32；
这是为减少保存观测的内存，转换时点进入契约，保证采样和 PPO 重算使用相同输入。
v1 权重不能直接冒充 v2 继续训练，旧模块和 Python 文件另存于 `.review/macro-v1-425a7cf`。

`command_mask(commands)` 返回与输入候选命令等长的布尔数组，复用玩家命令的地形、
建造位置、资源、人口与等级限制，并遵守提前召唤的时机限制。它不会执行命令或预留资源；
连续选择多条命令时应逐条执行并重新检查，不能把同一份掩码当成整组命令都可支付。
编码越界会抛异常，编码合法但当前无法执行则返回 false。

`candidates()` 返回当前完整合法命令集的 int32 `[N,4]` 数组，与 `advance` 的命令编码相同。
按 `(kind, slot, what, level)` 排序，包含等待、合法提前召唤、全部可建格、建筑操作、
所有可负担等级的招募及清野，不按脚本偏好截断候选。`map_shape` 返回 `(height,width)`，
命令位置是 `slot = y * width + x`；无位置命令的 slot 不应当作地图坐标。
这是单次决策的快照，推进后必须重新生成。

初始 `01001000` 地图（170×170）实测有 173,671 个候选，Windows Release / CPU 连续
20 次生成的中位数约 35 ms。这说明不能直接将它们全部送入大型逐候选网络；后续策略应
采用条件化操作/类型/等级/位置选择，保留全部合法选择的同时避免候选展开的网络成本。

`train/macro_policy.py` 提供条件化宏观网络：依次选择操作、类型、等级、位置，
每一阶段从当前前缀对应的合法值中采样。位置头复用区域特征、坐标与逐格卷积特征，
不逐个命令运行网络。逐格卷积覆盖邻近城防，既影响全局决策，也影响具体位置评分。
`decide` 返回命令、对数概率、状态价值和四阶段的合法值集合；PPO 应保存原始观测及这些
集合，用 `rescore` 重算概率，不能从推进后的局势重建旧动作掩码。位置仍覆盖完整地图。
`path_entropy` 只是选中路径上的条件熵之和，不是精确联合熵；`greedy` 是逐级最大值，
也不保证整个命令具有全局最高概率。

v1 网络默认约 15.5 万参数。此前 173,671 候选局势下，CPU 单线程预热后一次选择约
11.8 ms（不含候选生成）；这是旧版单次部件测量，不是 v2 或训练吞吐基准。候选原表占 2.78 MB，
保存的条件集合大小随所选分支变化：采集建筑分支很小，普通建造仍需保存全部可用位置。
测试覆盖小动作集联合概率归一化、候选乱序不改变概率、采样与重算一致、梯度更新、
权重/优化器/随机状态恢复后的下一步精确一致，以及真实游戏连续六次合法命令执行。

`macro_train.py` 提供 CPU 守方 PPO 训练入口，使用完整多波环境，按败局/完成目标波数
轮换地图与种子。每个波次结束时截断 RL 回报，但保留实体城市、资源和幸存者；
只有败局或完成目标波数才重建对局。当前奖励仅为守过一波 +1、败局 -1，
不为建造、拆除或等待反复发奖励；按实际推进 tick 调整折扣与 GAE。

```powershell
$env:PYTHONPATH = (Resolve-Path build-campaign/bindings/Release).Path
python train/macro_train.py --maps game/data/maps/pool/gen_01001000.json game/data/maps/pool/gen_01008000.json --run-dir runs/my-defender-macro --updates 100
```

Python 需安装匹配原生模块 ABI 的 PyTorch 与 NumPy。`--updates` 是累计目标，
重复命令自动续跑，可增大目标；训练配置、源码、原生模块或地图数值改变时拒绝原目录续训。
使用独立目录，避免与既有攻方训练产物混放。当前为 CPU 单线程，尚未实现并行守方采样。

每轮更新完成后原子保存权重、优化器、随机状态、统计和当前对局从开局以来的命令日志。

从已有兼容模型开始新实验可用 `--init-checkpoint PATH`，它保留模型权重，重新建立优化器、
训练进度和指定种子的对局；这是热启动，不是原实验的精确续跑。地图池可改变，观测、数值、
原生模块和策略实现必须兼容。同目录继续运行则仍从已提交检查点精确恢复。

`--gamma` 和 `--gae-lambda` 可显式调整反馈跨度，进入配置与恢复契约；默认仍为 .995/.95。
更长的 `--rollout` 保留更多连续决策供更新，同时增加观测内存和未提交采样的重跑成本。
训练统计新增实际仿真 tick 总数及每个结束对局的地图、种子、守过波数和败局状态，
不能把重复更新次数当成独立对局数。

为保护已学策略，可在新实验加 `--anchor-weight 1`。参考模型固定为初始化模型，
训练及恢复都保存在检查点内，不会随新模型更新。约束使用同一局势、同一合法命令支持上的
完整命令概率，通过旧策略到当前策略的重要性权重估计 `KL(当前 || 固定参考)`；
梯度保留在权重中。测试枚举小动作集，验证其期望值和梯度都等于直接计算的 KL。
这是采样估计，不是逐局精确 KL，也不是保证成绩不会下降。未开启约束时日志里的
`reference_kl_estimate=0` 表示未计算，不能理解为与初始策略完全一致。
约束权重必须非负且有限，开启时必须提供初始化检查点；恢复必须有保存的参考模型。

`--detach-critic-features` 可隔离价值分支的梯度：价值头继续训练，但价值误差不反向改写
动作策略的特征。动作目标仍可更新编码器，推理数值与模型结构不变。设置进入恢复配置，
恢复时重新安装梯度隔离，不依赖未序列化的临时钩子。默认不开启，实际收益需对照评估。
`macro_critic_probe.py` 可在真实局势上只执行价值更新，测量选中命令的对数概率漂移；
这是受控机制诊断，不是完整 PPO 退化原因或守城提升的证明。

恢复时先重放命令，再核对完整状态哈希，完全匹配才继续；无需用户重新手动打波次。
长局重放仍有时间成本，尚未替换为宏观训练专用二进制快照。进程中断时未提交的整轮采样
会从上一检查点重跑；不会把半轮更新混入成果。目录互斥锁和上一代检查点回退复用现有训练设施。
小规模测试已验证连续两轮与退出后续训的参数、随机状态和对局哈希完全一致。

冻结守方评估入口为 `macro_evaluate.py`，例如：

```powershell
python train/macro_evaluate.py --checkpoint runs/my-defender-macro/latest.pt --maps game/data/maps/pool/gen_01007000.json game/data/maps/pool/gen_01009000.json --output runs/my-defender-evaluation.json
```

评估比较现有脚本、按训练种子重建的初始网络和冻结训练网络，不更新参数。默认评估前六波、
种子 101/102；超时单独记录，不算成功。记录模型/地图/数值/原生模块身份并拒绝覆盖已有结果，
仅所有案例完成后 `complete` 才为 true。中断时已写入的逐局结果保留但不会标记完整。
默认沿用模型训练决策间隔，`--period 20` 可另做频率诊断；现有脚本仍保持每 20 tick
批量决策和辅助驻墙指令，因此脚本对照是产品基线，不是控制能力完全一致的算法消融。

可用 `--arms script learned` 评估基线与模型，再对后续检查点仅用 `--arms learned`，
避免重复运行和重复统计同一脚本案例。`macro_compare.py BEFORE.json AFTER.json` 按地图/种子
配对两个完整报告，核对原生模块、数值表、地图、种子、波次、时间上限和动作模式/频率一致；
缺局、重复案例或协议不同会拒绝汇总。它报告实际差值，不自动将较新的检查点视为更优模型。

`macro_imitation.py` 采集单命令脚本示范并训练相同宏观网络。`teacher_command()` 查询只读，
每次只选现有宏观脚本提议中的第一条合法命令，不额外下驻墙/调兵指令；没有合法提议时等待。
教师内部仍是现有脚本的局势读取方式，是离线特权教师；学生只接收过滤迷雾后的观测。

```powershell
python train/macro_imitation.py --maps game/data/maps/pool/gen_01001000.json game/data/maps/pool/gen_01008000.json --run-dir runs/my-macro-imitation --decisions 200 --epochs 8
```

可在新实验目录加 `--active-fraction 0.5`，将有效操作和等待两类样本的抽样概率质量
各设为一半。使用有放回重采样，不删除等待动作，也不只缩放逐样本 Adam 的损失。
该设置进入恢复契约，修改后必须使用新目录；不指定时保留原始样本比例。
不同抽样分布下记录的平均负对数似然不可直接横向比较。

示范数据原子写入 `demonstrations.pt`，训练每轮保存 `fit.pt`（优化器和随机状态齐全）。
采集被中断时会重新采集；训练被中断时从上一完整轮恢复。最终 `latest.pt` 是可供宏观 PPO
读取的初始化，PPO 优化器特意重新初始化，不沿用模仿学习的 Adam 动量；来源信息持续保留。
已进入 PPO 的目录拒绝再次运行模仿学习，防止覆盖后续成果。配置与源码身份不匹配同样拒绝。
首次试跑的未修订采集源码另保留在对应运行目录，用于复现已有冻结结果。

守方离线 ONNX 导出入口为 `export_macro.py`（需 onnx/onnxruntime）：

```powershell
python train/export_macro.py --checkpoint runs/my-macro-imitation/latest.pt --maps game/data/maps/pool/gen_01001000.json game/data/maps/pool/gen_01008000.json --output runs/my-macro-export
```

导出目录不可覆盖。`encoder.onnx` 输入区域 HWC、全局向量和逐格 HWC，输出区域 CHW、
上下文向量、逐格 CHW 和状态价值；地图高宽动态。`decoder.onnx` 接收缓存上下文、
候选格区域/逐格特征、坐标与三个已选前缀，输出操作/类型/等级/候选位置分数。
依次选择操作、类型、等级、位置；合法值仍由原生命令规则筛选，不在图内硬编码。
无位置命令使用 65535，局部特征置零、坐标为 `(0,0,1)`；普通格为
`((x+.5)/width,(y+.5)/height,0)`。逐格输入先 float16 舍入再转回 float32，与训练一致。

工具检查两张真实地图各四个决策的 Torch/ORT 输出及最终合法命令相同，并检查 41×53
动态分辨率。结束时再次核对源检查点未变，才写入带模型、图文件与源码校验值的清单。
这只是离线推理文件；当前尚未把守方宏观模型接入 C++ 游戏运行时，清单明确记录此边界。

当前仍需改善基础策略、更多局面训练和与攻方模型联合陪练。可恢复训练循环不代表守方
已学会守城。按新版守方设计，宏观模型不再承担
调兵，单位微操由现有脚本负责。不能把跑通这个环境称为宏观 RL 已完成。

### 一条命令运行完整实验

在已经构建原生绑定、配置好 `PYTHONPATH` 的仓库根目录运行：

```bash
python train/pipeline.py --run-dir runs/prepared-mixed \
  --map-pool game/data/maps/pool/gen_01001000.json,game/data/maps/pool/gen_01004000.json,game/data/maps/pool/gen_01005000.json,game/data/maps/pool/gen_01006001.json \
  --eval-maps game/data/maps/pool/gen_01007000.json,game/data/maps/pool/gen_01008000.json,game/data/maps/pool/gen_01009000.json \
  --levels 1,4,8,16 --prepare-ticks 900 --total-steps 65536
```

默认使用 CPU；有可用且获准使用的 GPU 时才显式加 `--device cuda`。
先采集完整距离的破口示范，再做行为克隆和带参考约束的 PPO，最后逐图逐等级
分别测试最大概率动作和按概率采样。`summary.json` 汇总结果，各阶段保留独立日志。
运行目录保存数值表、地图、原生模块、源码和参数合约；换电脑需保留对应源码与构建环境。

重启后重复同一命令：采集复用已完成分片，初始化和 PPO 恢复检查点，已完成的评估
只在模型哈希相同时复用。输入或源码改变会明确拒绝复用，避免把不同实验拼成一次运行。
原生对局在检查点恢复时仍会重新开局，并记录重置数，不声称逐 tick 无损续局。
这个入口完成的是攻方战术训练实验，不会自动部署模型或宣称胜过脚本；真实多波对照
与当前已知薄弱地图见 [稳定性与交付记录](../docs/rl-stability-and-deployment.md)。

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

## 经济目标、实际覆盖与价值网络对照

攻方训练新增三个显式目标模式：`keep` 保留堡垒目标；`known-economy` 让所有编队使用迷雾记忆中的
经济建筑；`split-economy` 只给固定的一部分食尸鬼/重骑分派经济任务，其他编队保留主攻。
没有已知经济建筑时回退堡垒，队长阵亡不改变同队任务。后者仍是训练侧采样规则，
并未实现窥使跨波情报、可学习的编成和分兵；不能当作攻方宏观 RL 已完成。

`--value-features` 默认 `shared`。`detached` 仅阻断价值损失进入动作特征；实验选项 `independent`
使用独立特征、价值头、Adam 和裁剪，动作网络与游戏推理格式不变。目前没有证据支持将其设为默认。
在已配置好原生绑定与 Python 环境的仓库根目录，可以新建对照 run（示例为 PowerShell）：

```powershell
python train/ppo.py --run-dir runs/my-independent-trial --init-weights runs/my-baseline/policy.pt `
  --device cpu --envs 8 --threads 4 --torch-threads 1 --total-steps 32768 `
  --map-pool game/data/maps/pool/gen_01001000.json,game/data/maps/pool/gen_01004000.json `
  --roster mixed --levels 1,4,8,16 --curriculum 1.0 --defender-prepare-ticks 900 `
  --tactical-goals split-economy --value-features independent --reference-coef 1 --lr 0.00001
python train/ppo.py --run-dir runs/my-independent-trial --resume auto
```

示例中的基线权重需换成实际保留的文件；目录应新建。其余参数使用当前默认值，
这不是已验证的最优配置。检查点保存 `value_model`/`value_optimizer`；缺失该状态的 independent
检查点不能续跑。旧训练恢复必须使用原源码、数据与原生版本，不能为方便续训跳过合同核验。

`state.json` 的 `progress.completed_coverage` 按地图、等级、入口、课程阶段累计真实终局和步数，
恢复后继续累计。配置写了四档等级不代表四档均有完整对局；未完成的局中环境不会记成已覆盖。
先检查实际覆盖，再冻结最终检查点做同环境对照，保留原始基线，不把训练胜率当作泛化成绩。

## 轮换冻结攻方陪练

守方宏观 PPO 的 `--attacker-pool script PATH_1.onnx PATH_2.onnx` 可以轮换冻结攻方陪练，
与 `--attacker-model` 互斥。每个对手先覆盖一轮全部训练地图，再切换到下个对手，
一轮所有对手结束后循环；只在完整城市对局结束时切换，不在波次中途换模型。
检查点保存池成员内容身份、顺序、调度规则和当前对手索引；恢复时更改成员或顺序会拒绝，
每次提交检查点也会检查尚未轮到的模型文件是否改变。完成记录列出实际 `opponent_index`。
未指定池时仍保持原单一对手行为。

该选项只提供冻结对手轮换，不会自动训练或替换池成员，也不等于完整协同演化已经完成。
旧训练目录仍需其原版本恢复；不要用新源码覆盖后放宽校验。实测恢复证据见
[轮换陪练验证](../docs/rl-results/2026-09-09-macro-league-recovery.json)。

## 在模型走偏的局面上补充离线标签

`demonstrations.py collect` 可同时接受 `--tactical-goals split-economy` 与
`--behavior-checkpoint PATH`：冻结模型负责行动，教师只提供当前公开观测下的合法标签。
省略行为权重仍使用原教师采集。模型内容身份进入采集计划，更换行为模型需新目录。
`fit --init-weights PATH` 可从已有模型拟合，之后普通 `--resume` 恢复拟合状态，
无需重复提供初始化路径。该过程是模仿纠正，不能计作 PPO 收益。

导出拟合检查点时，必须给 `export_policy.py` 额外传入
`--demonstrations PATH_TO_EXACT_DATASET`，数据 SHA 与拟合检查点引用必须匹配，才可恢复目标语义。
使用完整 `latest.pt` 或其冻结副本；只剩 `policy.pt` 时没有足够元数据证明新目标模式。
导出验证只确认动作一致性，不会自动采用模型。当前失败实验、完整对照和未完成范围见
[稳定性与交付记录](../docs/rl-stability-and-deployment.md)。
