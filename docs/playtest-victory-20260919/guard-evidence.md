# 第70—91波续局证据与复现

[续局复盘](guard-route.md) · [逐波分析](guard-waves.md) · [核对表](guard-tables.md) · [1577条事件](guard-events.md) · [原结局证据](evidence.md)

## 材料范围与分支

本次在PR中新增续局报告、22波逐波分析、1577条事件、核对表及4张坐标/统计图。原始存档、CSV、快照和临时观测源码保存在本地证据ZIP，未上传；仅凭PR可审查整理结果和规则源码，不能完整重放对局。

这是此前第70波回档后选择Guard的续局，独立于原报告的Release结局。新存档与旧档地图、数值、种子及2843条输入前缀完全相同；当前累计4099条输入。新报告观察区间含1256条玩家输入、29次征兵完成、17次守军阵亡、275次建筑/工地被敌军击毁，合计1577条事件。

## 冻结输入

- 规则：v1.0.1，提交 `fb0c36d6f121d24521a618c801393f17ad71f748`。
- 平台：Windows x64 / MSVC14.44；Release；20Hz。
- 地图：`gen_01009000`，种子 `8709371129856055178`。
- 冻结时间：2026-09-19 02:41:14，本地存档修改时间02:35:48；后续游玩未混入。
- 起点：第70波，tick160800；终点：第92波起点，tick209499，`choice=1`（Guard），`attempt=4`。attempt不用于推断失败次数。
- 终点世界哈希：`7198304080585817914`；归档快照哈希：`15533977933458490644`。
- 战术策略和守方策略身份均为空；这是脚本攻方分析，不能用来评定RL模型胜率。

两个独立执行入口都匹配终点世界哈希：`wave41_analyze` 的 `replay_verified=true`、`snapshot_verified=true`，以及 `deep_report actual` 的 `verified=true`。有效伤害、击毁、收入与游戏累计计数交叉核对。

## 观测口径

- 原始伤害/收入逐事件记录；单位位置每6tick采样，建筑每20tick采样，攻方决策按其8tick周期记录。位置极值与施工完成时刻属于采样值。
- 战斗只计 `160800 ≤ tick < 209499`，第92波新生成的敌军不计入第91波出战数。
- 建筑统计默认含工地；终点箭楼327、防空63均完工，城墙503中502完工。敌毁275中252完工、23工地。
- 敌毁、主动拆除、取消命令独立记账。输入不保证执行成功；同tick多命令不能仅凭库存变化逐一推断退款。经济净支出直接按收入与期末库存差核算。
- 塔下等待按两次决策间隔，并在死亡和波次边界截断。“等待关联承伤”按最近决策匹配，不是反事实可避免伤害；塔射程重叠也不等于所有塔正在攻击同一目标。
- 远矿未入经济情报/目标列表和零伤害可核验，不能扩大为从未进入任何单位视野。
- 集结区外围方环只核验静态建造条件及当前占位，未测试实际施工、包围后的战斗或新的出生规则。仍有空地不等于存在安全、有用的扩张路线。
- 图为坐标示意及统计图，非实机截图。全局91波曲线的前69波沿用已验证原路线；本次新增的细粒度日志仅覆盖70—91波。

## 本地证据包与指纹

文件：`试玩分析/2026-09-19_第70至91波/第70至91波_原始证据.zip`。共120个材料文件，另有逐文件SHA256清单；约21.97 MiB，CRC及逐文件SHA256核验通过。

ZIP SHA256：`8183a7fe584b459d8b8bf8cf218f75d7d21183f2f0ca17a213b58076c5145ea1`。

| 文件 | SHA256 |
|---|---|
| campaign.json | d98bd0640d83a3549b9d0c765b451cf991aeb8d0d5ae102153b4e3bf569245a6 |
| replay_report.json | cb83d2a99c71adc1aea5cd117f392db3e0f3085e03df700824f9d411a0ba2668 |
| result.json | ece40a0b4a05d38e5773c388ee45cc1b1781551c5060c2ba5c9be0eaefc943ca |
| damage.csv | 3884a64a72dce915987b3b43ce91831b079a13b0025847152647e3839a351dd3 |
| expansion_audit.py | ea56d6d3455591eaec99e23b2372c75fe7b16e363e866e7fe35c2a02c7e727ab |
| source_verification.json | 156d7eefc7a66c0e8ed35460b92a912baacf0d2c464dd1613cc1b100efb92789 |

本次对照了game、rts_core、cmake及根CMake中的83个相关版本化源码/配置文件。观测目录仅根CMake、demo_driver.cpp、mechanics.cpp与规则提交不同，另有观测头和独立分析入口；实际重放关闭所有行为实验开关。完整差异核对在 `raw/source_verification.json`。观测源码虽包含此前实验开关，哈希匹配仅证明本次actual执行与原局一致，不代表开启任何开关后仍与原规则等价。

## 复现步骤

1. 取得上述本地ZIP，验证SHA256、解压后验证 `SHA256.json`。在新的隔离目录检出上述规则提交。
2. 将 `runtime/` 内对应CMake、观测头、两个修改的实现文件以及7个独立C++分析入口按原路径覆盖/添加至隔离源码。补丁也有归档，整文件覆盖与应用补丁二选一。
3. 在MSVC环境配置Release headless构建，关闭渲染、bindings和测试，构建 `wave41_analyze`、`deep_report`。不要使用实时存档作输出位置。
4. 设RAW为解压的 `raw/`，OUT为新建空输出目录，执行 `wave41_analyze.exe RAW/campaign.json OUT RAW/snapshot_160800.json`。
5. 在另一空目录中执行 `deep_report.exe RAW/campaign.json RAW/snapshot_160800.json actual`；此程序向工作目录写CSV。核对两个输出的终点世界哈希均为 `7198304080585817914`。
6. 分析依赖Python、pandas、numpy、matplotlib。把所需日志、快照和脚本放在同一工作目录，修改脚本顶部输出路径后依次运行 `analyze_deep.py`、`detail_victory.py`、`extended_stats.py`、`remote_economy.py`、`expansion_audit.py`、`figures_wave90.py`。Windows可用 `python -X utf8`。
7. `extended_stats.py` 生成91波总览时还读取旧69波总览，证据包 `prior/` 已附，需相应调整路径。`write_guard_tables.py` 仅生成文档，另需调整DOC路径；`prepare_analytics.py` 是本机迁移旧脚本的辅助程序，不是复现步骤。

本轮没有新增改规则对照实验。此前的35组局部对照及其独立证据包继续见[原实验说明](counterfactuals.md)，不能把它们记作第70—91波的实验。
