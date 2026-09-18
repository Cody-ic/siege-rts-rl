# 证据与复现说明

[通关复盘](README.md) · [逐波分析](waves.md) · [突破代价](breakthrough.md) · [逐条事件](events.md) · [核对表](supporting-tables.md) · [证据与复现](evidence.md)

## 本PR的材料范围

提交主报告、29波逐波分析、1795条事件、突破代价专题、整理后的核对表及三张图。原始存档、逐tick日志、实验输出和临时观测程序按仓库约定保留在本地证据ZIP，未随PR上传。

仅凭本PR可以检查整理结果和固定版本源码，不能完整重跑对局。完整复现需要取得本地证据包；下面的raw/、runtime/、analysis/路径均指该ZIP内目录，指纹不代表材料已公开。

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

本机已完成上述重放与统计。此次续分析没有重新跑长局，也没有运行新的改规则反事实；第55波防空和第69波破墙结论来自已经验证的原局日志。未来做策略对照时，固定旧玩家输入只适合短时间局部诊断，不能代表会抢修、补墙、换兵的玩家。

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
