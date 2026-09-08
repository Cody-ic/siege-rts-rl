# Optional CPU-only inference. Training builds keep this dependency disabled.
option(RTS_WITH_ONNX "Build CPU inference for exported tactical policies" OFF)
set(RTS_ONNX_ROOT "" CACHE PATH "Existing ONNX Runtime 1.29.0 CPU SDK directory")
if(RTS_WITH_ONNX)
    if(NOT RTS_ONNX_ROOT)
        if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT CMAKE_GENERATOR_PLATFORM MATCHES "ARM")
            set(_ort_archive "onnxruntime-win-x64-1.29.0.zip")
            set(_ort_sha "c9b4b7086b529ad814f428c1bad028e20a25d7dc0699836775faace4ab5b78b2")
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
            set(_ort_archive "onnxruntime-linux-x64-1.29.0.tgz")
            set(_ort_sha "c3fddc4f139a045b0c4902c57410f0694f1c2fdf9b6939fbe38b1aeae7cd14ba")
        else()
            message(FATAL_ERROR "Supply RTS_ONNX_ROOT for this platform")
        endif()
        FetchContent_Declare(rts_onnx_sdk
            URL "https://github.com/microsoft/onnxruntime/releases/download/v1.29.0/${_ort_archive}"
            URL_HASH "SHA256=${_ort_sha}" TIMEOUT 120)
        FetchContent_MakeAvailable(rts_onnx_sdk)
        set(RTS_ONNX_ROOT "${rts_onnx_sdk_SOURCE_DIR}")
    endif()
    unset(_ort_include CACHE)
    unset(_ort_library CACHE)
    find_path(_ort_include onnxruntime_cxx_api.h PATHS "${RTS_ONNX_ROOT}/include" NO_DEFAULT_PATH REQUIRED)
    find_library(_ort_library NAMES onnxruntime PATHS "${RTS_ONNX_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
    add_library(rts_onnx SHARED IMPORTED GLOBAL)
    set_target_properties(rts_onnx PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_ort_include}"
        INTERFACE_INCLUDE_DIRECTORIES "${_ort_include}")
    if(WIN32)
        set_target_properties(rts_onnx PROPERTIES IMPORTED_IMPLIB "${_ort_library}"
            IMPORTED_LOCATION "${RTS_ONNX_ROOT}/lib/onnxruntime.dll")
    else()
        set_target_properties(rts_onnx PROPERTIES IMPORTED_LOCATION "${_ort_library}")
    endif()
endif()

function(rts_copy_onnx_runtime target)
    if(RTS_WITH_ONNX)
        if(WIN32)
            add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:rts_onnx>" "$<TARGET_FILE_DIR:${target}>" VERBATIM)
        endif()
        add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${RTS_ONNX_ROOT}/LICENSE" "$<TARGET_FILE_DIR:${target}>/ONNX-Runtime-LICENSE.txt" VERBATIM)
        add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${RTS_ONNX_ROOT}/ThirdPartyNotices.txt" "$<TARGET_FILE_DIR:${target}>/ONNX-Runtime-ThirdPartyNotices.txt" VERBATIM)
    endif()
endfunction()
