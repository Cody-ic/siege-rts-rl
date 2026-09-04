# `tools/calibration_runner/`

《攻守实力模型与平衡分析.md》§7 的校准 runner（2026-09-02 落地）。

`DemoBattle` + `DefenderScript` 能无渲染跑——这个目录就是 §7 说的那个 runner：
多地图 × 多种子跑 20 波（或跑到堡垒陷落），逐波记录校准清单上的每一项，
输出机器可读的 JSON。它是**旁观者**：只读 `WorldView` 与 `World` 的只读访问器，
不往对局里写任何东西；同一份输入两次运行输出字节一致（确定性纪律）。

## 构建与运行

```bash
# 顶层 CMake 无条件构建它（链接 game + rts_core，无第三方依赖）
cmake --build build --config Release --target calibration_runner

# 默认：池图全部 12 张 × 3 种子 × 20 波，JSON 到 stdout、耗时摘要到 stderr
./build/tools/calibration_runner/Release/calibration_runner.exe

# 常用形态：出文件、只跑一张图一个种子
./build/tools/calibration_runner/Release/calibration_runner.exe \
    --maps game/data/maps/pool --seeds 1 --max-waves 20 \
    --out build/calib_1.json
```

选项见 `--help`。默认值：`--maps <data>/maps/pool`、`--seeds 1,2,3`、
`--max-waves 20`、`--max-ticks 0`（不限）、`--limit 0`（全图）、`--out` 空 = stdout。
`--limit 1` 只跑字典序第一张图，是快速本地跑的常用形态。

## 每波记什么（对应 §7 清单逐项）

| §7 要的数 | 输出字段 | 怎么来的（埋点还是推理） |
|---|---|---|
| 门/缺口首次被打开的时刻 | `breach`（tick、相对开打 tick、`Wall`/`Gate`、位置） | 推理：本波第一段墙/门被杀死的 tick。地图**自带**的设计缺口单列在顶层 `maps[].gaps`，不算「打开」 |
| `Ram` 到墙下的血量 | `ram_events[].hp / max_hp / hp_frac` | 推理：「到墙下」= 第一次贴墙段承诺出手（脚本够得着墙上弓手会先打人，所以按**位置**判、不按动作种类判） |
| 它挨了几座塔的火 | `ram_events[].towers_firing`（去重塔数）/ `volleys_inbound`（发数） | 推理：到墙那一刻在途 `Tower` 齐射中落点离它 2.5 格以内的发数，按弹丸发射位置去重数塔 |
| 塔的每发 AOE 实际命中数 | `volley_hits`（逐发） | **埋点**：`World::volley_hits()`，齐射弹丸落地时记一行圈内实际挨打的地面单位数（miss 过滤后）。仓库唯一的 runner 埋点，不进 `state_hash`、不进回放 |
| `Phoenix` 的实际目标序列 | `phoenix_events`（tick、`Unit`/`Bld`、目标类型、位置）+ `phoenix_died_tick` | 推理：每次新承诺出手（前摇从 0 跳升）的目标种类，落点反解出目标类型 |
| 门边 13 格墙上有几名弓手 | `gate_windows[].at_assault_start / at_breach / at_wave_end` | 推理：每座门沿墙环切比雪夫距离 ≤6 的墙格窗口（直线段上恰好 13 格，如实记 `window_cells`），数已驻守弓手 |
| 每波守方损失（弓/塔/Flak） | `defender_start / defender_end / defender_loss`、`bld_start / bld_end` | 推理：波首波末两次快照的差 |
| 堡垒血量 | `keep_hp_start / keep_hp_end` | 推理：快照 |
| 波长 | `start_tick / end_tick / length_ticks / build_ticks / assault_start_tick` | 推理：首次看到本波（建造期开始）到首次看到下一波的 tick 跨度 |

另有 `attacker_start / attacker_end`（本波编成）与 `nominal_level`——重跑 §3 表的
直接输入。`rams_died_en_route` 记没到墙下就死的锤数（§3 的「打死它在半路」）。

## 输出格式

顶层 `schema: "calibration_runner/1"`，之后是 `maps`（门/缺口/环半径等静态信息）与
`runs`（每局 `map_file`、`seed`、`defeated`、`truncated`、`final_wave`、`total_ticks`、
`waves`）。除 schema 外不含挂钟时间，输出确定性。字段全表见 `main.cpp` 的
`write_wave()`。

## 测试

- ctest `calibration_runner_smoke`：跑真实二进制（一张池图、1 种子、2 波上限、
  6000 tick 上限），`check_smoke.py` 钉输出格式与关键字段（只钉形状与自洽，
  不钉数值）。
- 齐射命中埋点本身由 `tests/projectile_test.cpp` 的 `[proj]` 用例兜底
  （「齐射命中日志：每发记圈内实际挨打数，单体弹丸不记」）。

## 「多种子」实测等于「同一局跑三遍」（2026-09-03 实测）

**36/36 局：同一张图的三个种子给出完全相同的陷落波，一局不差。**（#135 首批就报过
「种子几乎不产生方差」、33/36 局逐 tick 相同；补上守方宏观层之后是 36/36。）

```
gen_01002000  [3, 3, 3]      gen_01004000  [5, 5, 5]
gen_01009000  [6, 6, 6]      其余九张      [4, 4, 4]
```

**这不是 bug**，是「攻方现在没有任何掷点」的直接后果：编成按 `WaveCurve` 曲线算、
主攻方向按 `wave % n_spawn` 取余、目标选择走 flow field——一条随机分支都没有。
而 demo 的确定性本身是设计要求（`kWorldHashTag`、回放测试都靠它）。

两条实用后果：

- **`--seeds 1,2,3` 现在读作「同一局跑三次」，跑 12 局就是全部信息**，
  省三分之二的时间。要方差请换图，不是换种子。
- **「中位存活波数」这个评估指标目前测不出它该测的东西**：中位数要有分布才有意义，
  而现在每张图是一个确定的数，所谓中位只反映**地图池的构成**、不反映策略强弱。
  真正的方差要等 RL 策略进来，或者给脚本攻方补上随机化
  （`守方AI与协同演化.md` 里「一族参数化脚本」那条「随机化那一半还没做」）。

## 已知局限（如实写）

- **现行池图的设计缺口没有被封（首批 36 局的主发现，2026-09-02）**：demo 守方
  不做经济、不封缺口（§1.6 的「第 1 波花 30 石封缺口」是玩家动作），实测攻方
  36/36 局全部从缺口进城、陷落波中位 2，`Ram`/`Phoenix` 只在 1 局里出场——
  校准对象（封缺口后的门前攻防）基本不存在。拟合 `aoe_mult`/`arch_conc` 前要
  先解决封缺口，见 `攻守实力模型与平衡分析.md` §7.1 的下一步。
- **塔火代理是「到墙那一刻的在途齐射」**：`Tower` 齐射落点锁定在承诺那一刻，
  而 `Ram` 到墙前后 1 秒只挪约 1 格，所以「落点离它近」是「在打它」的可靠代理；
  但它只数得着飞行中的弹（`proj_speed 0.35`，约 1 秒窗），已经落地的那批塔火
  不在内——因此 `towers_firing` 是**下界**。
- **波末弓手数在下一波建造期开始那一刻数**：波间只生攻方，守方状态与上一 tick
  相同，语义等价。
- **demo 守方没有补员**：脚本守方不征兵、不建塔（DefenderScript 只发单兵动作），
  所以 `defender_loss` 恒等于被杀数——§1.6 假设的「合理但不操作」玩家（波间补弓、
  建塔）不在这里，拟合 `arch_conc` 时要把「无补员」与「模型有补员」对齐口径。
- 第 1 波可能拖到几千到一万多 tick（demo_test 记过：残血 Ghoul 磨堡垒，守方脚本
  没人手猎杀它；实测最长一波 6271 tick），每局耗时以它为上限；`--max-ticks` 可截断。
