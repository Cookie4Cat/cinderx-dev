# Copyright (c) Meta Platforms, Inc. and affiliates.

include(FetchContent)
include(GoogleTest)

cinderx_declare_dependency(
  googletest
  https://github.com/google/googletest.git
  v1.17.0
)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

file(GLOB RUNTIME_TEST_SOURCES CONFIGURE_DEPENDS
  ${PROJECT_SOURCE_DIR}/RuntimeTests/*_test.cpp
  ${PROJECT_SOURCE_DIR}/RuntimeTests/fixtures.cpp
  ${PROJECT_SOURCE_DIR}/RuntimeTests/main.cpp
  ${PROJECT_SOURCE_DIR}/RuntimeTests/testutil.cpp
)

if(PY_VERSION VERSION_LESS 3.12)
  # 3.11 移植期构建排除清单（首版，M9 预演盘点）：以下测试源直接使用
  # 3.12+ 专有 C API（PyUnstable_Code_New 等）或覆盖尚未在 3.11 点亮的
  # 层（前端 hir / M4、deopt 与 gen_asm / M3+M6、OSR / 3.14 特性），按
  # 设计书 §4.3 三层点亮节奏随里程碑逐个收编。lir_test 属版本无关层却
  # 依赖 3.12 API 构造 code 对象，收编优先级最高。
  set(CINDERX_RUNTIME_TEST_EXCLUDES_311
    bytecode_test.cpp
    deopt_test.cpp
    gen_asm_test.cpp
    hir_test.cpp
    lir_target_select_test.cpp
    lir_test.cpp
    osr_entry_test.cpp
    osr_exit_degrade_test.cpp
    osr_hot_loop_detection_test.cpp
    # 链接期排除：引用 Static Python classloader thunk 符号
    # （_PyClassLoader_PropertyThunk_* 等），3.11 构建中该子系统为空壳；
    # 收编方式应为按符号拆分文件而非整文件点亮。
    sanity_test.cpp
  )
  foreach(_excluded ${CINDERX_RUNTIME_TEST_EXCLUDES_311})
    list(REMOVE_ITEM RUNTIME_TEST_SOURCES
         "${PROJECT_SOURCE_DIR}/RuntimeTests/${_excluded}")
  endforeach()
endif()

add_executable(runtime_tests ${RUNTIME_TEST_SOURCES} ${CINDERX_LIB_SOURCES})

target_link_libraries(
  runtime_tests
  PRIVATE
  Python::Python
  borrowed
  cached-properties
  common
  immortalize
  interpreter
  jit
  parallel-gc
  static-python
  asmjit::asmjit
  fmt::fmt
  gtest
  # dlsym（Jit/context.cpp）：旧 glibc（如 manylinux2014 的 2.17）中
  # 位于独立的 libdl，需显式链接；新 glibc 已并入 libc，此项为空。
  ${CMAKE_DL_LIBS}
)

if(ENABLE_DISASSEMBLER)
  target_link_libraries(runtime_tests PRIVATE capstone_static)
endif()

set(CINDERX_RUNTIME_TESTS_PYTHONLIB
  "${CMAKE_BINARY_DIR}/runtime_tests_pythonpath"
)
file(REMOVE_RECURSE "${CINDERX_RUNTIME_TESTS_PYTHONLIB}")
file(COPY "${PROJECT_SOURCE_DIR}/PythonLib/"
     DESTINATION "${CINDERX_RUNTIME_TESTS_PYTHONLIB}")
file(COPY "${PROJECT_SOURCE_DIR}/PythonLib/opcodes/${PY_VERSION}/opcode.py"
     DESTINATION "${CINDERX_RUNTIME_TESTS_PYTHONLIB}/cinderx")
file(TO_CMAKE_PATH
     "${CINDERX_RUNTIME_TESTS_PYTHONLIB}"
     CINDERX_RUNTIME_TESTS_PYTHONLIB)
target_compile_definitions(
  runtime_tests
  PRIVATE
  CINDERX_RUNTIME_TESTS_CMAKE
  BAKED_IN_PYTHONPATH="${CINDERX_RUNTIME_TESTS_PYTHONLIB}"
)

if(NOT APPLE)
  target_link_options(runtime_tests PRIVATE -Wl,--export-dynamic)
endif()

gtest_discover_tests(
  runtime_tests
  DISCOVERY_TIMEOUT 120
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
)
