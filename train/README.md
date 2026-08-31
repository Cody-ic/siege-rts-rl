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

## 已知局限（不回避）

- **没有训练脚本**。`smoke.py` 之后的下一步是一份真正的 PPO 循环（网络结构、
  超参、CleanRL 风格的单文件实现），那是一次独立的、需要新设计决策的工作，
  没有随这份骨架一起做。
- **`守方决策层` 与宏观层暂未接入**：`smoke.py` 里手摆的攻方编成只为冒烟，
  真正的编成来自宏观层（尚未开工，见 `README.md`「当前未认领的工作」第 8 项）。
- 首版由 @Cody-ic 在训练服务器（Linux/GCC）上调通，含一处 `-fPIC` 链接失败
  与一处 GIL 释放顺序导致的 SIGSEGV，均已修复（提交历史见 `bindings/`）。
  MSVC 侧此前从未编译过，合并时补了一处遗漏的编译选项才通过（见
  `bindings/CMakeLists.txt` 该处注释）——这正是「两套工具链尽早互相暴露对方
  漏洞」的一次真实案例，不是假设。
