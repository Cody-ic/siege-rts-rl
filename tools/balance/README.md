# `tools/balance/`

数值设计与平衡工具（纯 Python，无 C++ 依赖）。

`CLAUDE.md`「平衡工具：成本产出矩阵」要求每格记录 1v1 对抗的期望资源盈亏，
作为调数值的依据与答辩材料。这个目录是它的**最小可行实现**——不是最终方案。

## 文件

- `combat_math.py`：逐字复现 `rts_core/include/rts/combat_math.hpp` 与
  `rts_core/src/mechanics.cpp` 里的战斗公式（`apply_permille` /
  `level_permille` / `charge_permille` / `anti_charge_permille`），
  用于在没有 `bindings/`/`train/` 的情况下快速验证数值改动。
  **改引擎侧公式后，这里要同步改**，否则两边会漂移。
- `cost_output_matrix.py`：读 `game/data/stats_placeholder.json`，对
  `CLAUDE.md`「克制二部图」里点名的每一对关系跑闭式 1v1 交换比。

## 用法

```bash
py tools/balance/cost_output_matrix.py
```

改了 `stats_placeholder.json` 里任何单位/建筑的数值后，先跑一遍这个脚本，
看有没有哪条克制关系被验证成"未通过"，再提交。

## 已知局限（不回避）

闭式 TTK 近似**不是**真实批量仿真：忽略了接敌时间、路径绕行、混战中的
目标切换、AOE 命中密集度随实际站位变化等因素。它只保证"克制方向对不对"
这个一阶结论，不保证具体数字精确。一旦 `bindings/`+`train/` 落地，
应该换成 `守方AI与协同演化.md` 第 8 节写的两步法：黑箱优化（CMA-ES/
贝叶斯优化）在矩阵上搜候选数值 → 协同演化在完整博弈均衡下验收。

`COUNTER_GRAPH` 里标 `False`（跳过数值验证）的几条不是没验证，是**这个
工具的模型验证不了**——风筝（靠射程+速度差）、免费仇恨窗口（靠射程差）、
位置性克制（Flak vs Phoenix）都需要移动/站位建模，不是纯 DPS 对拼能测的。
详见《数值设计与成本产出矩阵.md》第 8 节。
