add_executable(scan_bench main.cpp)
target_link_libraries(scan_bench PRIVATE game nlohmann_json_hdr)
rts_set_warnings(scan_bench)
rts_copy_onnx_runtime(scan_bench)
if(RTS_BUILD_TESTS)
    add_test(NAME scan_bench_check COMMAND scan_bench --check
        --stats "${PROJECT_SOURCE_DIR}/game/data/stats_placeholder.json")
endif()
