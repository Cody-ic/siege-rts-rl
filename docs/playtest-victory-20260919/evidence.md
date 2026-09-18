# 证据与复现说明

[通关复盘](README.md) · [逐波分析](waves.md) · [突破代价](breakthrough.md) · [策略对照](counterfactuals.md) · [实验核对表](counterfactual-tables.md) · [逐条事件](events.md) · [核对表](supporting-tables.md) · [证据与复现](evidence.md)

## 本PR的材料范围

提交主报告、29波逐波分析、1795条事件、突破代价专题、35组局部对照及整理后的核对表，共五张图。原始存档、逐tick日志、实验输出和临时观测程序按仓库约定保留在本地证据ZIP，未随PR上传。

仅凭本PR可以检查整理结果和固定版本源码，不能完整重跑对局。原局复现需要取得第一份证据包，局部对照需要另取第二份突破实验证据包；下文分别说明。raw/、runtime/、analysis/均指对应ZIP内目录，指纹不代表材料已公开。

## 输入与分析范围

本报告使用回档前冻结的第70波解放结局存档，分析成功路线第1—69波；第70波没有发生战斗。随后为继续守城而进行的回档，以及之后新玩的波次，都没有混入本次数据。

- 项目：Cody-ic/siege-rts-rl；规则版本：v1.0.1，提交fb0c36d6f121d24521a618c801393f17ad71f748。
- 冻结时刻：tick160800；20Hz，共134分钟模拟时间；地图gen_01009000；种子8709371129856055178。
- 存档格式6，世界版本World/16，平台msvc-1944/win/x64；脚本攻方，开发者模式关闭。
- 世界状态哈希：14010302102068610454；快照哈希：5317481788821054292。
- campaign.json的SHA256：507cb16b45385d7494ec2a7ecc2d89a7a0bad4e4af079d0bf7683abf17534249。
- 详细观测新增区间：95979≤tick<160800，即第41—69波。

## 证据链核验

前两段记录来自已完成的第1—30波和第31—40波分析。其地图、数值、种子与当前档案相同，旧输入逐条等于新档的对应前缀；两个衔接快照tick74073、95979分别逐字节一致。三个区间均有成功重放及状态哈希结果。

本次普通逐tick重放和带详细观测的重放都匹配最终世界哈希。重新汇总确认：29波、1795条事件、333处敌毁建筑/工地、1650378守方对敌伤害、267701敌方对建筑伤害；汇总收入与期末库存变化互相核对。

1795条事件由1325条玩家输入、76次征兵完成、61次守军阵亡、333次建筑/工地敌毁组成。输入可能失败或与同tick其他操作合并，不能按输入条数当作实际完成数量。原存档累计2843条输入；前段档案包含第41波开头部分准备操作，因此新增详细区间输入不是简单用两份存档总条数相减。

## 观测精度与口径

- damage.csv记录每次实际扣血，amount为原始攻击伤害，damage为有效扣血；溢出伤害不算输出。target_kind的1/2为单位/建筑，src_kind的0/1为单位/建筑来源。
- commits.csv记录单位攻击前摇开始；不是每条前摇都必定命中。
- decisions.csv每8tick记录决策；“塔下等待”是处在塔射程内并执行pace_wait，不表示每一条采样都被塔选为目标。
- units.csv每6tick（0.3秒）采样，buildings.csv每20tick（1秒）采样；极短事件与升级精确完成时刻不能仅由这些采样推出。伤亡使用逐次扣血和逐tick边界记录。
- 单位·秒是多单位的状态累计，不是玩家经历的现实秒数；没有测量暂停与阅读时间。
- 引擎在波次切换tick可能先增wave，本报告按真实波次起点重新归属逐击与决策；进攻时长沿用普通重放的assault_ticks/20，与旧段总览一致。
- 伤亡的最后一击与此前承伤分开记录；被玩家主动拆除的受损塔不计入敌军直接击毁。
- 建筑计数含工地。所谓“城外塔”按开局城圈计，不代表后来没有外墙保护。
- 原始src_uid对某些弹丸来源为0，此时以记录的类型、位置和目标核验，不将0误当作一个持久实体。

## 两条看似退款不匹配的记录

tick104071取消应返190石、70木；同tick另有采石场升级花16石、12木，所以库存净增174石、58木。

tick136394取消墙应返30石、10木；同tick另一处建墙花30石、10木，所以库存净变化为0。

专题统计中的cancel_checks.match仅比较“退款额”与“该tick净账”，这两条为false不代表退款失败。净账还包含同时发生的收入与支出。

## 原始证据包

本地归档“原始证据与分析工具.zip”包含：

- raw/codex-victory-analysis-20260919：冻结档、原始CSV、逐波快照、重放结果与Python统计脚本。
- raw/codex-wave31-analysis、raw/codex-wave41-analysis：前段冻结档、重放摘要、衔接快照。
- runtime：观测源码补丁、修改后源码副本、关键原规则源码，以及与发布提交的比对结果。
- analysis：报告、附录、CSV、图表及指纹；manifest.json列出包内每个文件的SHA256。

对发布提交的167个C++/头文件/CMake文件进行了内容比对，已有文件差异仅CMakeLists.txt、game/src/demo_driver.cpp、rts_core/src/mechanics.cpp，其余为新增观测工具。补丁可应用于所列基线。实际重放模式不启用工具中保留的策略实验开关，最终状态哈希一致。

## 取得证据包后的复现步骤

1. 在独立目录检出所列提交，应用runtime/runtime_observation.patch。不要应用到正在玩的安装目录。
2. Windows初始化MSVC编译环境，使用CMake配置Release、关闭renderer/bindings和测试；构建wave41_analyze及deep_report。原始快照具有平台约束，使用同类MSVC环境验证；不能用跨编译器哈希不一致推断游戏行为不同。
3. 设WORK为解压后的raw/codex-victory-analysis-20260919，并新建两个空输出目录。运行wave41_analyze.exe WORK/campaign.json OUTPUT_REPLAY WORK/snapshot_95979.json。
4. 在另一空目录OUTPUT_DETAIL中运行deep_report.exe WORK/campaign.json WORK/snapshot_95979.json actual。该程序向工作目录写CSV；切勿把工作目录设为实时存档目录。
5. 检查replay_report.json中的replay_verified和result.json中的verified均为true，终点哈希均为14010302102068610454。
6. Python脚本依赖pandas、numpy、matplotlib。脚本与对应CSV放在同一目录，前段目录保持相邻；把脚本顶部O改成所需报告目录，按analyze_deep.py、detail_victory.py、special_victory.py、figures_victory.py顺序生成统计。finalize_victory.py用于核验两处实例并补充成稿，含文档插入操作，不应反复运行在已经补充过的主报告上。

本机已完成上述原局重放与统计；第55波实际防空突破和第69波实际破墙周期来自原局日志。随后运行的改规则局部对照使用第二份独立证据包，见下节。固定旧玩家输入只适合短时间局部诊断，不能代表会抢修、补墙、换兵的玩家。

## 归档指纹

本地证据ZIP共149个材料文件，约19.25 MiB；CRC和逐文件SHA-256均通过校验。

ZIP SHA-256：4f36b6ac3b20e47217f031ede21ff8cc3e688bdd0df1e2781d5cb922dfdb78d3。

| 材料 | SHA-256 |
| --- | --- |
| campaign.json | 507cb16b45385d7494ec2a7ecc2d89a7a0bad4e4af079d0bf7683abf17534249 |
| replay_report.json | 4a5725eb2cd9263e65af8fc133b53558981492b8accf4b4aec8c1d8aa6ba668a |
| result.json | e488ff0b6db486be5a565824b727e666a2869ff6f7a762a816c31c7aa0090e4c |
| damage.csv | d4c2602d292337ef4c5134b856294468bf2e54e22ebb892c553b5b9ca1e640cf |
| units.csv | f733a1d86cc92493b58f4bd3355da2ab83754078ada1ffdb053b7688a013d832 |
| runtime_observation.patch | 0daa9eb36449a49bef6ff36f6ed008950651d54de78b0b7ee1b654387f315973 |

关键结论的源数据摘录已转为[核对表](supporting-tables.md)，包括第55波防空逐击记录和第69波破墙前摇/伤害记录。

<a id="counterfactual-evidence"></a>
## 第二份证据包：突破策略局部对照

### 材料与版本

本轮使用v1.0.1规则提交fb0c36d6f121d24521a618c801393f17ad71f748，冻结成功路线的第49、55、58、60、69波准备期快照。地图gen_01009000，种子8709371129856055178；MSVC14.44、Release、固定20Hz。没有使用回档后正在游玩的存档。

主矩阵为5波×7种模式=35组，含5组原规则和30组改动对照；新增编队组合探针另在第69波做一次原规则核验，共运行36次。基线均精确匹配实际下一波起点哈希；完整结果见[基线核对表](counterfactual-tables.md#baselines)。

每个波次在开始时，同种地面攻方单位等级一致，分别为63、74、79、82、99级。实验中实际等级流场沿用原缓存分组，仅适用于这些已核对局面；若推广到同兵种同档但等级不同的混编，需要把代表能力纳入缓存键，不能直接搬用此探针实现。

### 实验开关

- flow_timing：只对攻方破坏代价使用首次前摇＋(命中数−1)×max(前摇,冷却,1)。是理想持续交战估计，不包含当前剩余冷却、接近、维修与其他单位。
- flow_level：攻方地面流场传入实际单位等级；守方路径能力不变。
- flow_both：同时采用上述两项。
- flak_in_range：不死鸟AtkBld选择器优先射程内完工防空，再按最近距离选；射程外、AtkWeak、原有撤退/袭扰顺序不变。这临时改变动作语义，只用于诊断。
- exposed_no_pace：有完工箭楼射程覆盖时，跳过原等队形Stop，仍按原流场前进；更早的攻击/集结/侦查分支照旧。
- flow_both_exposed：组合校正等级/周期与塔下不停。

全部开关默认为关闭。正式仓库仅增加分析文档和图片；游戏没有安装实验二进制。与基线比对167个C++/头文件/CMake文件，已有文件差异仅五个：CMakeLists、demo_driver、mechanics、flow.cpp、flow.hpp；其他为新增观测工具，补丁应用检查通过。

### 时间和操作边界

从各波准备期开始，推进到该波结束；安全上限300秒，本轮未触发。统计秒数包含准备期，与旧报告“进攻秒数”不能直接混用。玩家输入按原时间戳回放，但不会超过原局下一波的起点；新波次提前结束则立即停下。守方自主行为一直运行。

其中七组因提前结束各少执行2条原玩家输入；延长的组在原输入结束后最多多运行15.6秒，没有新的玩家操作。已额外比较双方共同时间窗口；所有新出现的堡垒命中均早于原局终点。相同操作在不同战场可能失败或作用改变，这种响应偏差仍未消除。

damage.csv和commits.csv逐次记录伤害与攻击前摇；decisions.csv每8tick采样，units.csv每6tick、buildings.csv每20tick采样。塔火覆盖是几何射程统计，不代表每座塔均以该单位为当前目标；单位路径和进入城圈人数按位置采样观察。材料毁损取引擎累计量差值，伤害用实际扣血，不把溢出算输出。

案例基于已知原局选择，不用于独立样本显著性检验。组合对照是在第一轮结果显示等待与路线相互作用后追加的；没有预先固定的随机评估集。

### 本地证据包与复现

“突破实验原始证据.zip”保留全部36次运行的结果、日志、逐击记录和快照；提交到PR的仅为整理后的文档、表格与图片。仅凭PR无法完整重跑，需要取得本地证据包。

包内raw/codex-victory-analysis-20260919包含输入存档及五对起止快照；raw/codex-breakthrough-experiments-20260919包含cases.json、运行/统计脚本、runs/主矩阵和validation/额外基线。runtime/包含可应用到基线提交的补丁、修改源码副本与版本核验。

在独立目录检出基线，应用补丁，使用初始化的MSVC环境以Release构建breakthrough_probe和breakthrough_probe2。构建脚本中的工具链/依赖路径为本机配置，迁移时需调整；Python脚本依赖pandas、numpy和matplotlib，报告输出O也需改为本机目标目录。

单组命令形状：breakthrough_probe.exe campaign.json snapshot_START.json MODE snapshot_EXPECTED_END.json。执行目录必须是一个新的空输出目录，因为CSV和result.json写在工作目录；编队两组改用breakthrough_probe2.exe。原规则MODE为baseline，并要求result.json的verified为true。

完整复跑可按cases.json依次运行七种模式，随后运行analyze_experiments.py和report_experiments.py。原运行脚本遇到已存在的输出目录会退出，迁移或重跑前应指定新的实验目录，避免覆盖证据。SHA-256指纹用于辨认材料，不表示原始材料已经公开。

该包与前面的原局归档分开保存，共450个材料文件，约23.36 MiB，CRC和逐文件SHA-256已校验。

ZIP SHA-256：ee14abbf2da6ac6ef952f99ce783e09c23791fe39d8598bfdd6c9506f35eef8e。
