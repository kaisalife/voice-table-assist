# VTA Native Service —— CMake 构建（NDK r26+ / c++17）
#
# 前置（方案 §7.1）：
#   1. Android NDK r26+，cmake -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake
#      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29
#   2. sherpa-onnx 源码（锁 v1.12.x）放在 third_party/sherpa-onnx/（或用 SHERPA_ONNX_DIR 指向已构建安装目录）
#   3. onnxruntime-android ≥ 1.28.0 的 .so + 头文件（ONNXRUNTIME_DIR 指向解包目录）
#      ⚠️ 钉死 ≥1.28.0：1.27.0 在骁龙 8 Elite Gen 5 上 zipformer encoder 静默算错（方案 §14.4）
#
# 产物：libvta-service.so（含 main 的可执行 vta-service 二选一；APK 内用 so + stub service）
cmake_minimum_required(VERSION 3.22)
project(vta-service CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- 依赖路径 ----
set(SHERPA_ONNX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sherpa-onnx" CACHE PATH "sherpa-onnx 源码目录")
set(ONNXRUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime" CACHE PATH "onnxruntime-android 解包目录（含 headers/ 与 jni/arm64-v8a/libonnxruntime.so）")

# ---- onnxruntime（预编译 so + C++ 头文件；ABI 自适应：arm64-v8a / x86_64）----
# target `onnxruntime` 由 sherpa-onnx 的 cmake/onnxruntime.cmake 创建（IMPORTED SHARED），
# 这里只传 prebuilt 路径（环境变量），不重复建 target。
if(NOT DEFINED ANDROID_ABI)
    set(ANDROID_ABI "arm64-v8a")
endif()
set(ORT_INCLUDE "${ONNXRUNTIME_DIR}/headers")

# ---- sherpa-onnx（源码编译静态库；仅 online 识别所需子集）----
# 用 prebuilt ort 时通过环境变量传给 sherpa-onnx 的 cmake：
#   SHERPA_ONNXRUNTIME_INCLUDE_DIR / SHERPA_ONNXRUNTIME_LIB_DIR
set(ENV{SHERPA_ONNXRUNTIME_INCLUDE_DIR} "${ORT_INCLUDE}")
set(ENV{SHERPA_ONNXRUNTIME_LIB_DIR} "${ONNXRUNTIME_DIR}/jni/${ANDROID_ABI}")
# 精简选项（-D 传入）：SHERPA_ONNX_ENABLE_JNI=OFF SHERPA_ONNX_ENABLE_WEBSOCKET=OFF
#   SHERPA_ONNX_ENABLE_PORTAUDIO=OFF SHERPA_ONNX_ENABLE_BINARY=OFF
set(SHERPA_ONNX_ENABLE_JNI OFF CACHE BOOL "" FORCE)
set(SHERPA_ONNX_ENABLE_WEBSOCKET OFF CACHE BOOL "" FORCE)
set(SHERPA_ONNX_ENABLE_PORTAUDIO OFF CACHE BOOL "" FORCE)
set(SHERPA_ONNX_ENABLE_BINARY OFF CACHE BOOL "" FORCE)
set(SHERPA_ONNX_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SHERPA_ONNX_ENABLE_C_API ON CACHE BOOL "" FORCE)
# 用我们提供的 prebuilt ort（third_party/onnxruntime，1.28.0），不让 sherpa 自己下载
set(SHERPA_ONNX_USE_PRE_INSTALLED_ONNXRUNTIME_IF_AVAILABLE ON CACHE BOOL "" FORCE)
add_subdirectory(${SHERPA_ONNX_DIR} sherpa-onnx-build EXCLUDE_FROM_ALL)

# ---- AIDL（NDK backend）----
# 用 NDK 自带 aidl 工具生成 com.vta.* 的 C++ 桩到 build/aidl-gen
find_program(AIDL_EXE aidl
    HINTS "$ENV{ANDROID_HOME}/build-tools/34.0.0" "$ENV{ANDROID_SDK}/build-tools/34.0.0"
          "D:/Android/Sdk/build-tools/34.0.0" "D:/Android/Sdk/build-tools/36.0.0")
set(AIDL_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/aidl-gen")
file(MAKE_DIRECTORY "${AIDL_GEN_DIR}")
set(AIDL_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/aidl/com/vta/IVtaService.aidl
    ${CMAKE_CURRENT_SOURCE_DIR}/aidl/com/vta/PcmFrame.aidl
    ${CMAKE_CURRENT_SOURCE_DIR}/aidl/com/vta/SessionState.aidl
    ${CMAKE_CURRENT_SOURCE_DIR}/aidl/com/vta/TableInfo.aidl
    ${CMAKE_CURRENT_SOURCE_DIR}/aidl/com/vta/CellHit.aidl)
set(AIDL_GEN_SRCS "")
if(AIDL_EXE)
    foreach(f ${AIDL_SRCS})
        get_filename_component(name ${f} NAME_WE)
        # ndk backend：cpp 落 <out>/com/vta/*.cpp，头落 <header_out>/aidl/com/vta/*.h
        set(out "${AIDL_GEN_DIR}/com/vta/${name}.cpp")
        add_custom_command(
            OUTPUT "${out}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${AIDL_GEN_DIR}/com/vta"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${AIDL_GEN_DIR}/aidl/com/vta"
            COMMAND ${AIDL_EXE} --lang=ndk -I${CMAKE_CURRENT_SOURCE_DIR}/aidl
                    -o ${AIDL_GEN_DIR} --header_out ${AIDL_GEN_DIR} ${f}
            DEPENDS ${f}
            COMMENT "aidl ${name}")
        list(APPEND AIDL_GEN_SRCS "${out}")
    endforeach()
else()
    message(FATAL_ERROR "aidl 工具未找到：请设置 ANDROID_HOME 或在 Soong 构建中改用 Android.bp")
endif()

# ---- VTA 核心源码 ----
set(VTA_SRCS
    jni/common/json.cc
    jni/common/strings.cc
    jni/text/chinese_numeral.cc
    jni/text/cell_phrase_generator.cc
    jni/text/merge_text.cc
    jni/text/pinyin_table.cc
    jni/text/sound_aligner.cc
    jni/text/triple_extractor.cc
    jni/tests/mmi_kernel_test.cc
    jni/embed/vtx1.cc
    jni/embed/embedder.cc
    jni/homophone/homophone_replacer.cc
    jni/homophone/domain_correction.cc
    jni/ner/raner_engine.cc
    jni/denoise/gtcrn_denoiser.cc
    jni/asr/recognizer.cc
    jni/tables/table_registry.cc
    jni/tables/voice_resource.cc
    jni/tables/table_vector_manager.cc
    jni/session/interaction_session.cc
    jni/session/voice_session.cc
    jni/service/audio_capture.cc
    jni/service/engine_host.cc
    jni/service/vta_config.cc
    jni/service/vta_service_impl.cc
    jni/service/selftest_runner.cc
    ${AIDL_GEN_SRCS})

add_library(vta-core STATIC ${VTA_SRCS})
set_target_properties(vta-core PROPERTIES POSITION_INDEPENDENT_CODE ON)
# sherpa-onnx 的 FetchContent 依赖头（kaldi-decoder 等）不会经 INTERFACE 传导到外部目标，手动补齐
foreach(_dep kaldi_decoder kaldi_native_fbank openfst kaldifst simple_sentencepiece)
    if(EXISTS "${CMAKE_BINARY_DIR}/_deps/${_dep}-src")
        list(APPEND VTA_DEP_INCLUDES "${CMAKE_BINARY_DIR}/_deps/${_dep}-src")
    endif()
endforeach()
# openfst 的 fst/*.h 在 src/include 下
if(EXISTS "${CMAKE_BINARY_DIR}/_deps/openfst-src/src/include")
    list(APPEND VTA_DEP_INCLUDES "${CMAKE_BINARY_DIR}/_deps/openfst-src/src/include")
endif()
# Eigen
if(EXISTS "${CMAKE_BINARY_DIR}/_deps/eigen-src/Eigen")
    list(APPEND VTA_DEP_INCLUDES "${CMAKE_BINARY_DIR}/_deps/eigen-src")
endif()
target_include_directories(vta-core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/jni
    ${AIDL_GEN_DIR}
    ${AIDL_GEN_DIR}/aidl
    ${ORT_INCLUDE}
    ${SHERPA_ONNX_DIR}
    ${VTA_DEP_INCLUDES})
target_link_libraries(vta-core PUBLIC
    # sherpa-onnx-build 子目录里创建的 imported target 对父目录不可见，直接链 .so 全路径
    "${ONNXRUNTIME_DIR}/jni/${ANDROID_ABI}/libonnxruntime.so"
    sherpa-onnx-core
    binder_ndk
    aaudio
    log)

# 可执行（调试/root 设备直跑；APK 部署见 Android.bp 注释）
add_executable(vta-service jni/service/main.cc)
target_link_libraries(vta-service PRIVATE vta-core)

# APK 用的共享库（进程内 JNI 直调；jar 侧 java/com/vta/service/NativeBridge.java）
add_library(vta-jni SHARED jni/service/jni_bridge.cc)
target_link_libraries(vta-jni PRIVATE vta-core)
set_target_properties(vta-jni PROPERTIES OUTPUT_NAME "vta-service")

# ---- 主机端纯逻辑自测（无需 sherpa-onnx / ORT / Android）----
# 验证 C++ 翻译层与 C# 行为一致（§8.1）：中文数字/文本合并/三元组/领域纠错/同音纠正/
# VTX1 round-trip（含读 C# 写的 cell_index.bin）/registry.json/语音资源生成。
option(VTA_HOST_TEST "Build host-side pure-logic test (no sherpa/ORT)" OFF)
if(VTA_HOST_TEST)
    set(HOST_TEST_SRCS
        jni/common/json.cc
        jni/common/strings.cc
        jni/text/chinese_numeral.cc
        jni/text/cell_phrase_generator.cc
        jni/text/merge_text.cc
        jni/text/pinyin_table.cc
        jni/text/sound_aligner.cc
        jni/text/triple_extractor.cc
        jni/embed/vtx1.cc
        jni/homophone/homophone_replacer.cc
        jni/homophone/domain_correction.cc
        jni/tables/table_registry.cc
        jni/tables/voice_resource.cc
        jni/tests/host_test.cc)
    add_executable(vta-host-test ${HOST_TEST_SRCS})
    target_include_directories(vta-host-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/jni)
    if(WIN32)
        target_link_libraries(vta-host-test PRIVATE ws2_32)  # windows.h 传递依赖兜底
    endif()
endif()
