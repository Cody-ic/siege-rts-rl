# 第31—40波：证据与复现说明

[主报告](README.md) · [逐条事件](events.md) · [猎骑机制核查](ranger.md) · [补充核对表](supporting-tables.md) · [证据与复现](evidence.md)

## 本PR的材料范围

本PR提交主报告、444条事件、猎骑专题、补充核对表和三张图。下文的 `data/`、`experiments/`、`tools/`、`provenance/` 均指作者本地证据ZIP内的路径，未随PR提交；这些原始存档、日志、实验输出和临时工具按仓库约定保留在提交之外。

仅凭本PR可以复核整理结果和指定版本的源码，不能独立完整重跑实验。完整重跑需先取得本地证据包。指纹用于辨认材料，不表示材料已公开。

## 输入与统计边界

冻结存档为第41波准备期，tick `96610`，种子 `8709371129856055178`，状态哈希 `3200466901981574840`。文件SHA-256为 `2e242b8353558139dc0be4d4603c068ff85e9caa311ffd1072b326d9505719ff`。状态哈希与文件摘要是两种不同校验值。

主报告区间为 `[74073,95979)`，即第31波开始至第41波开始，共1095.30秒模拟时间。冻结时第41波已经过31.55秒，这段只在正文末尾单列。暂停期间的现实时间不计入。

新旧存档的地图、数值、种子和旧存档已有输入前缀一致。起点 `snapshot_74073.json` 来自上次已校验的重放。本次摘要重放和逐tick观测均从此起点运行到96610，终态与新存档一致。证据包的 `provenance/` 保留上一份存档、校验结果和起点快照，可核对这条来源链。

存档为v6、World/16、Stats/13，平台为 `msvc-1944/win/x64`。学习策略身份为空，本报告分析的是脚本攻方。

## 规则与隔离

规则固定为1.0.1源码提交 `fb0c36d6f121d24521a618c801393f17ad71f748`，没有使用主工作区旧分支的规则解释本局。

对基础提交中的167个C++/CMake文件逐一比对，原有文件只有 `CMakeLists.txt`、`game/src/demo_driver.cpp`、`rts_core/src/mechanics.cpp` 不同，分别用于挂接分析程序、记录决策和记录伤害；另新增观测头文件及独立分析程序。实验开关默认关闭。完整差异和新增程序均收录在 `tools/runtime_observation.patch` 与 `instrumented_source/`。

观测重放及两个单波基线均通过状态哈希核验。所有运行都在临时副本进行，没有改动正在玩的程序或实时存档。玩家随后继续游玩不会改变这些冻结证据。

## 文件与精度

| 文件 | 含义 |
|---|---|
| `data/campaign.json` | 冻结存档，含地图、数值、输入与终态 |
| `data/input_identity.json` | 文件摘要、起点及新旧输入前缀核验 |
| `data/replay_report.json` | 波次边界、输入、征兵和阵亡，以及终态校验 |
| `data/result.json` | 逐tick观测结果；累计量从恢复起点重新计数 |
| `data/snapshot_*.json` | 各波准备期/进攻期起点及终点快照 |
| `data/damage.csv` | 每次伤害、来源、带世代号的目标身份、血量、原始伤害和实际扣血 |
| `data/commits.csv` | 攻击前摇开始时的目标；不等于实际命中 |
| `data/decisions.csv` | 每次AI决策，通常间隔8tick，含分支、动作与撤退判据 |
| `data/units.csv` | 每6tick，即0.3秒的单位状态采样 |
| `data/buildings.csv` | 每20tick，即1秒的建筑状态；升级完成时刻有最多1秒采样误差 |
| `data/orders.csv` | 输入执行前目标建筑状态；去重后与实际资源变化核对 |
| `data/income.csv` | 资源入账，含来源建筑位置 |
| `data/ledger.csv` | 每tick资源净变化，可能混合收入、开销与退款 |
| `experiments/` | 第36、40波各一组基线和撤退优先实验，共4次运行 |
| `analysis/` | 完整报告、逐条附录、统计表与三张图 |

伤害按模拟tick区间归波，避免波号先更新、弹丸后结算导致错归。征兵日志在更新后记录，因此按前一个tick所属区间归波。AI生成新波指令可能发生在边界更新内，不死鸟专题同时核对引擎波号与阶段。附录同tick内的排序只为展示，不声称代表引擎内部顺序。

建筑身份含世代号，原地重建不与旧塔混账。工匠位置是采样轨迹，没有记录内部寻路的全部候选路线，不能据此证明路线最优或认定寻路故障。

## 已核验的关键量

- 摘要和观测两条重放的终态均为 `3200466901981574840`。
- 第36、40波基线分别匹配实际下一波起点。
- 主区间伤害明细与独立累计量一致：守方对敌429826、攻方对建筑68227、攻方对守军3832。
- 343条输入、27次出兵、23次守军阵亡、51次敌军摧毁建筑/工地，合计444条事件。
- 11处取消工地均为1血，退款800石/310木；16次拆除退款656石/324木。一次拆除同tick还有8木维修支出，已区分。
- 7名最终被锤击杀的枪卫，合计承伤4886，己方4013、敌方873；各自记录承伤覆盖完整生命池。
- 六座主动拆除塔此前在本区间的伤害来源均为不死鸟。这里只确认事件与损失，未把全部拆除认定为下一击必死。

本地完整指纹表为 `证据指纹.json`，包内同份内容为 `manifest.json`。最新证据包共103个材料文件，已逐一核对SHA-256，并通过ZIP CRC校验。下节列出关键材料摘要。

## 实验的含义与限制

`baseline` 使用原规则；`phoenix_retreat_first` 仅把既有不死鸟撤退检查移至矿区袭扰分支之前，没有同时调整准备期、血量、伤害或配额。第36波起止tick为85009→87204，第40波为93726→95979。

实验沿用该波原有时间戳上的玩家输入，进入下一波即结束。实验提前结束则不再执行余下输入；即使延后，也不执行原记录下一波的输入。玩家没有针对反事实局面重新操作。后续命中、随机数消费、目标和命令有效性可以随轨迹变化。

两组实验都验证了存活并在下一波实际生成不死鸟；未运行修正后的连续三波，因此不声称已测得长期胜率提升。第40波总建筑伤害变化大于不死鸟自身变化，也不能全算成一次不死鸟攻击的差额。高级工匠的血量比较只是固定命中条件下的算术，不是完整逃跑对照。

## 取得本地证据包后的复现步骤

使用独立Windows MSVC x64检出，在初始化编译环境后应用补丁并构建：

```text
git checkout fb0c36d6f121d24521a618c801393f17ad71f748
git apply <证据目录>/tools/runtime_observation.patch
cmake -S . -B build-audit -G Ninja -DCMAKE_BUILD_TYPE=Release -DRTS_BUILD_TESTS=OFF -DRTS_WITH_ONNX=OFF
cmake --build build-audit --target wave41_analyze deep_report
```

下列输入使用解压目录中的绝对路径；输出目录预先建立，每次运行使用不同的空目录，避免覆盖证据：

```text
wave41_analyze.exe <证据目录>/data/campaign.json <摘要输出目录> <证据目录>/data/snapshot_74073.json
deep_report.exe <证据目录>/data/campaign.json <证据目录>/data/snapshot_74073.json actual
deep_report.exe <证据目录>/data/campaign.json <证据目录>/data/snapshot_85009.json baseline <证据目录>/data/snapshot_87204.json
deep_report.exe <证据目录>/data/campaign.json <证据目录>/data/snapshot_85009.json phoenix_retreat_first <证据目录>/data/snapshot_87204.json
```

`deep_report.exe` 在当前工作目录写日志。第40波将上述实验起止快照替换为93726和95979。摘要程序应得到 `replay_verified: true`，观测及基线程序应得到 `verified: true`。跨编译器或浮点平台不保证位级一致。

整理顺序为 `analyze_deep.py` → `detail_analysis.py` → `make_figures.py`，依赖pandas、NumPy、Matplotlib。脚本默认从其所在目录读取数据；移机后把 `P` 指向解压后的 `data/`，把输出路径 `O`/`OUT` 改为新的空目录，并将实验目录路径指向解压后的 `experiments/`。正文结合源码、实验与玩家解释人工撰写，不由统计自动推断。

基础规则：[1.0.1源码](https://github.com/Cody-ic/siege-rts-rl/tree/fb0c36d6f121d24521a618c801393f17ad71f748)。

## 本地归档指纹

证据ZIP的SHA-256：`dbc91e42eab91b6116cad95f4f8693aa07e911aeefd1c49881be93b01225900c`。

| 归档内路径 | SHA-256 |
| --- | --- |
| `data/campaign.json` | `2e242b8353558139dc0be4d4603c068ff85e9caa311ffd1072b326d9505719ff` |
| `data/replay_report.json` | `3e7f8d9e8a196d20cc47e9b9a2609899537dc942b210da4d52593bbc95d0b26e` |
| `data/result.json` | `2c5f93ebe02bc9726aaf24e5d36ffee03dbb9a0f4e688818f65fd447dae9f45e` |
| `data/damage.csv` | `a17119cbc0eb875f7172da159b499ba66343f705fc573c7746d007822b994afa` |
| `data/units.csv` | `bff87f6f7191ddc5f71b295f08ddc496eb2562ba4ca2c57087114c9f0863887d` |
| `tools/runtime_observation.patch` | `0daa9eb36449a49bef6ff36f6ed008950651d54de78b0b7ee1b654387f315973` |
| `experiments/wave36_baseline/result.json` | `7f1b5d973ee2a3e4fba195523882f1ee4775aa2ff2839007abcb0af62431cf10` |
| `experiments/wave36_phoenix_retreat_first/result.json` | `af2170e75a01a8070c5f416e0f79d8abecdeb1a5f0e15f9fdf14443359278d8c` |
| `experiments/wave40_baseline/result.json` | `6e3bcec1d03fe2f9b0ad88b47670dbe77224f1dab4ef77b1171b1f798f0b33a6` |
| `experiments/wave40_phoenix_retreat_first/result.json` | `763ebf5970c1743f0b74a1e5b02b0cf8fdda8105156480a12dfa651ea1beb404` |

猎骑补充核查沿用同一份已校验的伤害和单位状态记录，结合未改动的基础版本守方脚本；未运行猎骑行为修正的对照。包内同时保留该补充核查的本地原稿。
