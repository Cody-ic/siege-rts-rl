# 全量扫描与精确空间查询对比

这是独立、只读的 Release 基准程序。用于比较扩建、多据点所需的数据查询，不修改游戏规则、玩家存档或正式程序。结论与测量条件见 [实验报告](../../docs/scan-performance.md)。

## 构建和运行

从仓库根目录运行；Windows 先初始化 C++ 编译环境。

```sh
cmake -S . -B build-scan -DCMAKE_BUILD_TYPE=Release -DRTS_BUILD_SCAN_BENCH=ON
cmake --build build-scan --config Release --target scan_bench
build-scan/tools/scan_bench/scan_bench --check
build-scan/tools/scan_bench/scan_bench --seed 7 --samples 41 --output build-scan/scan7.json
```

Windows 可执行文件带 `.exe`；Visual Studio 多配置生成器还需在可执行文件前加 `Release/`。开启 `RTS_BUILD_TESTS` 时，`ctest --test-dir build-scan -C Release -R scan_bench_check --output-on-failure` 会运行一致性检查。工具默认不构建。

`--stats PATH` 指定数值表。`--archive PATH` 可选读取当前版本支持的玩家存档副本，仅测世界中工匠的静态决策成本；使用新建脚本，不继承玩家指令和脚本历史，不代表原对局后续走向。玩家存档不应提交仓库。

## 对比口径

- `full_entities`：每个合法观察者遍历全部候选实体，精确判定其所在格是否可见。
- `shared_grid`：先清空整张可见格表，合并所有观察者的真实视野，再完整扫描实体；只跳过已确认可见的格子，保留相同的半径和地形视线判断。
- `buckets4/8/16`：每轮重新建立不同尺寸的空间桶，包含建桶和分配开销；只排除不可能位于视野内的桶，剩余对象仍做相同的精确判定。
- `jobs`：比较每名工匠重复扫描建筑，与整批工匠共享一份完整工地列表。
- `worker_inputs`：镜像当前脚本的通行表、威胁字段和工地列表准备步骤，对比逐工匠重建与每批重建一次；不包含工匠起点特例、实际寻路或选工地。
- `current_worker_script`：直接调用现有 `DefenderScript::decide`，包含完整工匠决策，用来避免把低成本查询误当成整段AI的性能。

计时前预热3轮，轮换算法顺序，使用相同输入，校验与结果消费不计入时间。每个输出按规范槽位排序，因此分桶迭代顺序不改变结果。工匠整批决策每种子测5次，其余测指定次数。

一致性检查包含视野边缘、地形遮挡、空中观察、空列表、建筑/单位删除、槽位身份变化、观察者增删、对象跨桶移动与地形变化；完整实体扫描作为简单基准，并有独立的明确预期边界用例。计时中也逐次比较结果。

这些是几何可见性查询原型，不含情报老化、侦查传输、记忆删除的完整机制；给定实体的可见性相同，不意味着完整迷雾地图也可被实体查询替代。实际接入时必须先确定观察者的信息权限，不能将窥使尚未交付的视野混入普通编队。

合成场景不等同于真实第90波录像。255×255加压地图接近当前格数上限；256×256会超过 `Command::slot` 的有效范围，工具不把不受支持的地图当成运行结果。原始JSON输出应留在构建目录，不进Git。
