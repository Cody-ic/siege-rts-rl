# rts_core：全部游戏逻辑。
#
# 硬性要求（CLAUDE.md 不变量 1 与 3）：headless。不链接任何图形库、不依赖 Python、
# 不在 tick 热路径内向上回调。它必须能在完全无 GUI 的训练服务器上跑完整局游戏。
# 因此这个 target 的依赖列表**应当长期保持为空**——往里加东西前先确认它不破坏这条。

add_library(rts_core STATIC
    src/rng.cpp
)

target_include_directories(rts_core PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/include"
)

rts_set_warnings(rts_core)
