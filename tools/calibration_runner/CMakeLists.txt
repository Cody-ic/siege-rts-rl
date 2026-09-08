# tools/calibration_runner：攻守实力模型 §7 的校准 runner（2026-09-02）。
#
# 无渲染、无第三方依赖（JSON 输出手写），只链接 game（game 已 PUBLIC 链接
# rts_core）。**无条件构建**，与 game/ 同一条理由：挂它名字的 ctest 要它在场，
# 而「默认构建里被测、不在默认构建里静默消失」是本仓反复执行的纪律
# （见 tests/CMakeLists.txt 的 calibration_runner_smoke 条目）。
add_executable(calibration_runner main.cpp)

target_link_libraries(calibration_runner PRIVATE game)
rts_copy_onnx_runtime(calibration_runner)
rts_set_warnings(calibration_runner)

# 默认数值表与地图目录：与 tests/ 同一个编译期常量（game/CMakeLists.txt
# 里 REAL_PATH 规范化过的 GAME_DATA_DIR），不依赖 ctest 的工作目录。
target_compile_definitions(calibration_runner PRIVATE
    GAME_DATA_DIR="${GAME_DATA_DIR}")
