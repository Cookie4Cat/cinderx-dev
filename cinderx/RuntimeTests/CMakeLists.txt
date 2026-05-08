# Copyright (c) Meta Platforms, Inc. and affiliates.

include(FetchContent)
include(GoogleTest)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
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
