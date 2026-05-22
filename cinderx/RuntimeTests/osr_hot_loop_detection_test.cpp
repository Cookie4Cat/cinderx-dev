// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

// clang-format off
// This needs to come before pycore frame headers so borrowed symbols are
// remapped consistently with the rest of RuntimeTests.
#include "cinderx/UpstreamBorrow/borrowed.h" // @donotremove
// clang-format on

#include "cinderx/Common/ref.h"
#include "cinderx/Common/log.h"
#include "cinderx/Interpreter/cinder_opcode.h"
#include "cinderx/Jit/bytecode.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/osr_capi.h"
#include "cinderx/Jit/pyjit.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include "internal/pycore_frame.h"

#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace jit {
void syncOSRFlags();
} // namespace jit

namespace {

constexpr int kOSRRunningState = 1;

struct TryOSRHookState {
  int return_code{0};
  long result_value{0};
  int calls{0};
  uint32_t last_oparg{0};
  bool clear_counters_during_call{false};
};

int tryOSRTestHook(
    PyThreadState*,
    _PyInterpreterFrame* frame,
    _Py_CODEUNIT*,
    uint32_t oparg,
    PyObject** result,
    void* data) {
  auto* state = static_cast<TryOSRHookState*>(data);
  state->calls++;
  state->last_oparg = oparg;
  if (state->clear_counters_during_call) {
    Ci_OSR_ClearBackedgeCountersForTesting(_PyFrame_GetCode(frame));
  }
  if (state->return_code == 1) {
    *result = PyLong_FromLong(state->result_value);
  } else if (state->return_code == -1) {
    PyErr_SetString(PyExc_RuntimeError, "osr test exception");
  }
  return state->return_code;
}

void setOSRFlags(int enabled, int capable, int state) {
  cinderx_osr_enabled = enabled;
  cinderx_osr_capable = capable;
  cinderx_osr_state = state;
}

class ScopedOSRFlags {
 public:
  ScopedOSRFlags()
      : enabled_{cinderx_osr_enabled},
        capable_{cinderx_osr_capable},
        state_{cinderx_osr_state} {}

  ~ScopedOSRFlags() {
    setOSRFlags(enabled_, capable_, state_);
  }

 private:
  int enabled_;
  int capable_;
  int state_;
};

class ScopedJitConfig {
 public:
  ScopedJitConfig() : config_{jit::getConfig()} {}

  ~ScopedJitConfig() {
    jit::getMutableConfig() = config_;
    jit::syncOSRFlags();
  }

 private:
  jit::Config config_;
};

class ScopedOSRTestHook {
 public:
  explicit ScopedOSRTestHook(TryOSRHookState* state) {
    Ci_OSR_SetTestTryOSRHook(tryOSRTestHook, state);
  }

  ~ScopedOSRTestHook() {
    Ci_OSR_ClearTestTryOSRHook();
  }
};

bool isSupportedOSRBuild() {
#if defined(CINDER_AARCH64) && !defined(Py_GIL_DISABLED)
  return true;
#else
  return false;
#endif
}

BorrowedRef<PyCodeObject> codeFromFunc(BorrowedRef<PyFunctionObject> func) {
  return reinterpret_cast<PyCodeObject*>(PyFunction_GET_CODE(func));
}

Ref<PyCodeObject> makeCodeObject(const std::vector<unsigned char>& bc) {
  auto bytecode = Ref<>::steal(PyBytes_FromStringAndSize(
      reinterpret_cast<const char*>(bc.data()), bc.size()));
  JIT_CHECK(bytecode.get() != nullptr, "failed to create bytecode bytes");
  auto filename = Ref<>::steal(PyUnicode_FromString("osr_test.py"));
  auto funcname = Ref<>::steal(PyUnicode_FromString("test"));
  auto consts = Ref<>::steal(PyTuple_New(1));
  Py_INCREF(Py_None);
  PyTuple_SET_ITEM(consts.get(), 0, Py_None);
  auto empty_tuple = Ref<>::steal(PyTuple_New(0));
  auto empty_bytes = Ref<>::steal(PyBytes_FromString(""));
  return Ref<PyCodeObject>::steal(PyUnstable_Code_New(
      /*argcount=*/0,
      /*kwonlyargcount=*/0,
      /*nlocals=*/0,
      /*stacksize=*/0,
      /*flags=*/0,
      bytecode,
      consts,
      /*names=*/empty_tuple,
      /*varnames=*/empty_tuple,
      /*freevars=*/empty_tuple,
      /*cellvars=*/empty_tuple,
      filename,
      funcname,
      /*_unused_qualname=*/funcname,
      /*firstlineno=*/0,
      /*linetable=*/empty_bytes,
      /*_unused_exceptiontable=*/empty_bytes));
}

long longFromGlobal(RuntimeTest& test, const char* name) {
  Ref<> value = test.getGlobal(name);
  JIT_CHECK(PyLong_CheckExact(value.get()), "expected '{}' to be an int", name);
  return PyLong_AsLong(value.get());
}

std::string stringFromGlobal(RuntimeTest& test, const char* name) {
  Ref<> value = test.getGlobal(name);
  JIT_CHECK(
      PyUnicode_CheckExact(value.get()), "expected '{}' to be a str", name);
  const char* utf8 = PyUnicode_AsUTF8(value.get());
  JIT_CHECK(utf8 != nullptr, "failed to decode '{}'", name);
  return utf8;
}

std::string readSourceFile(const char* path) {
  std::ifstream file{path};
  JIT_CHECK(file.is_open(), "failed to open {}", path);
  return std::string{
      std::istreambuf_iterator<char>{file},
      std::istreambuf_iterator<char>{}};
}

std::string sliceFrom(const std::string& text, const std::string& marker) {
  size_t start = text.find(marker);
  JIT_CHECK(start != std::string::npos, "missing marker '{}'", marker);
  return text.substr(start);
}

std::string sliceBetween(
    const std::string& text,
    const std::string& start_marker,
    const std::string& end_marker) {
  size_t start = text.find(start_marker);
  JIT_CHECK(start != std::string::npos, "missing marker '{}'", start_marker);
  size_t end = text.find(end_marker, start + start_marker.size());
  JIT_CHECK(end != std::string::npos, "missing marker '{}'", end_marker);
  return text.substr(start, end - start);
}

void replaceFrameFuncobj(_PyInterpreterFrame* frame, PyObject* replacement) {
  Ci_STACK_CLOSE(frame->f_funcobj);
  frame->f_funcobj = PyStackRef_FromPyObjectNew(replacement);
}

class InterpreterFrameHolder {
 public:
  explicit InterpreterFrameHolder(PyFunctionObject* func)
      : tstate_{PyThreadState_Get()}, func_{func}, code_{codeFromFunc(func)} {
    JIT_CHECK(
        jit::getConfig().frame_mode == jit::FrameMode::kNormal,
        "OSR eligibility tests construct normal interpreter frames");
    frame_ = Cix_PyThreadState_PushFrame(tstate_, jit::jitFrameGetSize(code_));
    JIT_CHECK(frame_ != nullptr, "failed to push test interpreter frame");
    jit::jitFrameInit(
        tstate_,
        frame_,
        func_,
        code_,
        0,
        FRAME_OWNED_BY_THREAD,
        nullptr,
        nullptr);
  }

  ~InterpreterFrameHolder() {
    if (frame_ == nullptr) {
      return;
    }
    jit::jitFrameClearExceptCode(frame_);
    Cix_PyThreadState_PopFrame(tstate_, frame_);
  }

  _PyInterpreterFrame* get() const {
    return frame_;
  }

 private:
  PyThreadState* tstate_;
  PyFunctionObject* func_;
  BorrowedRef<PyCodeObject> code_;
  _PyInterpreterFrame* frame_{nullptr};
};

class ScopedFrameMode {
 public:
  explicit ScopedFrameMode(jit::FrameMode frame_mode)
      : old_frame_mode_{jit::getConfig().frame_mode} {
    jit::getMutableConfig().frame_mode = frame_mode;
  }

  ~ScopedFrameMode() {
    jit::getMutableConfig().frame_mode = old_frame_mode_;
  }

 private:
  jit::FrameMode old_frame_mode_;
};

class ScopedOSRThreshold {
 public:
  explicit ScopedOSRThreshold(uint32_t threshold)
      : old_threshold_{jit::getConfig().osr_backedge_threshold} {
    jit::getMutableConfig().osr_backedge_threshold = threshold;
  }

  ~ScopedOSRThreshold() {
    jit::getMutableConfig().osr_backedge_threshold = old_threshold_;
  }

 private:
  uint32_t old_threshold_;
};

class OSRDetectionTest : public RuntimeTest {};

} // namespace

TEST(OSRFlagTest, IsEnabledRequiresEnabledFlag) {
  ScopedOSRFlags flags;
  setOSRFlags(0, 1, kOSRRunningState);

  EXPECT_FALSE(Ci_OSR_IsEnabled());
}

TEST(OSRFlagTest, IsEnabledRequiresCapableFlag) {
  ScopedOSRFlags flags;
  setOSRFlags(1, 0, kOSRRunningState);

  EXPECT_FALSE(Ci_OSR_IsEnabled());
}

TEST(OSRFlagTest, IsEnabledRequiresRunningState) {
  ScopedOSRFlags flags;
  setOSRFlags(1, 1, 0);

  EXPECT_FALSE(Ci_OSR_IsEnabled());
}

TEST(OSRFlagTest, IsEnabledReflectsSupportedBuildWhenAllFlagsAreSet) {
  ScopedOSRFlags flags;
  setOSRFlags(1, 1, kOSRRunningState);

  EXPECT_EQ(Ci_OSR_IsEnabled(), isSupportedOSRBuild());
}

TEST(OSRConfigTest, BackedgeThresholdReadsMutableConfig) {
  ScopedOSRThreshold threshold{7};

  EXPECT_EQ(Ci_OSR_GetBackedgeThreshold(), 7);
}

TEST(OSRConfigTest, SyncOSRFlagsReflectsMutableConfig) {
  ScopedJitConfig config;
  ScopedOSRFlags flags;
  jit::getMutableConfig().osr_enabled = true;
  jit::getMutableConfig().osr_capable = true;
  jit::getMutableConfig().state = jit::State::kRunning;

  jit::syncOSRFlags();

  EXPECT_EQ(cinderx_osr_enabled, 1);
  EXPECT_EQ(cinderx_osr_capable, 1);
  EXPECT_EQ(cinderx_osr_state, kOSRRunningState);

  jit::getMutableConfig().state = jit::State::kPaused;
  jit::syncOSRFlags();

  EXPECT_EQ(cinderx_osr_state, 0);
  EXPECT_FALSE(Ci_OSR_IsEnabled());
}

TEST(OSRGeneratedCodeTest, SpecializeJumpBackwardAlwaysRoutesToJit) {
  std::string generated =
      readSourceFile("Interpreter/3.14/Includes/generated_cases.c.h");
  std::string jump_backward = sliceBetween(
      generated, "PREDICTED_JUMP_BACKWARD:;", "TARGET(JUMP_BACKWARD_JIT)");

  EXPECT_NE(
      jump_backward.find("this_instr->op.code = JUMP_BACKWARD_JIT"),
      std::string::npos);
  EXPECT_EQ(jump_backward.find("ENABLE_SPECIALIZATION"), std::string::npos);
  EXPECT_EQ(jump_backward.find("JUMP_BACKWARD_NO_JIT"), std::string::npos);
  EXPECT_EQ(jump_backward.find("tstate->interp->jit ?"), std::string::npos);
}

TEST(OSRGeneratedCodeTest, JitCaseRunsOSRBeforeTier2Optimizer) {
  std::string generated =
      readSourceFile("Interpreter/3.14/Includes/generated_cases.c.h");
  std::string jit_case = sliceBetween(
      generated,
      "TARGET(JUMP_BACKWARD_JIT)",
      "TARGET(JUMP_BACKWARD_NO_INTERRUPT)");

  size_t osr_check = jit_case.find("Ci_OSR_IsEnabled()");
  size_t tier2 = jit_case.find("_Py_BackoffCounter");
  ASSERT_NE(osr_check, std::string::npos);
  ASSERT_NE(tier2, std::string::npos);
  EXPECT_LT(osr_check, tier2);

  EXPECT_NE(jit_case.find("CI_OSR_BACKEDGE_INCREMENT"), std::string::npos);
  EXPECT_NE(
      jit_case.find("CI_OSR_COMPUTE_JUMP_TARGET_INDEX"), std::string::npos);
  EXPECT_NE(jit_case.find("CI_OSR_TRY_OSR"), std::string::npos);
  EXPECT_EQ(
      jit_case.find("stack_pointer = _PyFrame_GetStackPointer(frame);\n"
                    "                                    if (osr_rc"),
      std::string::npos);
  EXPECT_NE(jit_case.find("_Py_LeaveRecursiveCallPy"), std::string::npos);
}

TEST_F(OSRDetectionTest, BackedgeCountersAreCreatedLazilyPerCodeObject) {
  Ref<PyFunctionObject> first(compileStockAndGet(
      R"(
def first():
  return 1
)",
      "first"));
  Ref<PyFunctionObject> second(compileStockAndGet(
      R"(
def second():
  return 2
)",
      "second"));
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);

  BorrowedRef<PyCodeObject> first_code = codeFromFunc(first);
  BorrowedRef<PyCodeObject> second_code = codeFromFunc(second);

  EXPECT_EQ(Ci_OSR_GetBackedgeCounters(first_code), nullptr);
  EXPECT_EQ(Ci_OSR_GetBackedgeCounters(second_code), nullptr);

  Ci_BackedgeCounters* first_counters =
      Ci_OSR_GetOrCreateBackedgeCounters(first_code);
  ASSERT_NE(first_counters, nullptr);
  EXPECT_EQ(Ci_OSR_GetBackedgeCounters(first_code), first_counters);
  EXPECT_EQ(Ci_OSR_GetOrCreateBackedgeCounters(first_code), first_counters);

  Ci_BackedgeCounters* second_counters =
      Ci_OSR_GetOrCreateBackedgeCounters(second_code);
  ASSERT_NE(second_counters, nullptr);
  EXPECT_NE(first_counters, second_counters);
}

TEST_F(OSRDetectionTest, BackedgeEntryStartsCountingAndIncrements) {
  Ref<PyFunctionObject> func(compileStockAndGet(
      R"(
def test():
  return 1
)",
      "test"));
  ASSERT_NE(func, nullptr);

  Ci_BackedgeCounters* counters =
      Ci_OSR_GetOrCreateBackedgeCounters(codeFromFunc(func));
  ASSERT_NE(counters, nullptr);

  Ci_BackedgeEntry* entry =
      Ci_OSR_BackedgeCountersFindOrCreate(counters, 12, 4);
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(Ci_OSR_BackedgeGetCount(entry), 0);
  EXPECT_EQ(Ci_OSR_BackedgeGetState(entry), CI_OSR_BACKEDGE_COUNTING);

  EXPECT_EQ(Ci_OSR_BackedgeIncrement(entry), 1);
  EXPECT_EQ(Ci_OSR_BackedgeIncrement(entry), 2);
  EXPECT_EQ(Ci_OSR_BackedgeGetCount(entry), 2);

  Ci_OSR_BackedgeSetCount(entry, 99);
  EXPECT_EQ(Ci_OSR_BackedgeGetCount(entry), 99);
  Ci_OSR_BackedgeSetState(entry, CI_OSR_BACKEDGE_FAILED_PERMANENT);
  EXPECT_EQ(
      Ci_OSR_BackedgeGetState(entry), CI_OSR_BACKEDGE_FAILED_PERMANENT);
}

TEST_F(OSRDetectionTest, BackedgeEntryLookupIsPerSourceIndex) {
  Ref<PyFunctionObject> func(compileStockAndGet(
      R"(
def test():
  return 1
)",
      "test"));
  ASSERT_NE(func, nullptr);

  Ci_BackedgeCounters* counters =
      Ci_OSR_GetOrCreateBackedgeCounters(codeFromFunc(func));
  ASSERT_NE(counters, nullptr);

  Ci_BackedgeEntry* first =
      Ci_OSR_BackedgeCountersFindOrCreate(counters, 12, 4);
  ASSERT_NE(first, nullptr);
  Ci_OSR_BackedgeSetCount(first, 5);

  Ci_BackedgeEntry* same_source =
      Ci_OSR_BackedgeCountersFindOrCreate(counters, 12, 4);
  EXPECT_EQ(same_source, first);
  EXPECT_EQ(Ci_OSR_BackedgeGetCount(same_source), 5);

  Ci_BackedgeEntry* different_source =
      Ci_OSR_BackedgeCountersFindOrCreate(counters, 14, 6);
  ASSERT_NE(different_source, nullptr);
  EXPECT_NE(different_source, first);
}

TEST_F(OSRDetectionTest, BackedgeCountersRejectEntriesPastFixedCapacity) {
  Ref<PyFunctionObject> func(compileStockAndGet(
      R"(
def test():
  return 1
)",
      "test"));
  ASSERT_NE(func, nullptr);

  Ci_BackedgeCounters* counters =
      Ci_OSR_GetOrCreateBackedgeCounters(codeFromFunc(func));
  ASSERT_NE(counters, nullptr);

  for (uint32_t i = 0; i < CI_OSR_MAX_BACKEDGES; i++) {
    ASSERT_NE(
        Ci_OSR_BackedgeCountersFindOrCreate(counters, i * 2, i), nullptr);
  }

  EXPECT_EQ(
      Ci_OSR_BackedgeCountersFindOrCreate(
          counters, CI_OSR_MAX_BACKEDGES * 2, CI_OSR_MAX_BACKEDGES),
      nullptr);
  EXPECT_FALSE(PyErr_Occurred());
}

TEST_F(OSRDetectionTest, DisabledOSRHotLoopDoesNotCreateCounters) {
  ScopedOSRFlags flags;
  setOSRFlags(0, 0, 0);
  TryOSRHookState hook_state;
  ScopedOSRTestHook hook{&hook_state};
  Ref<PyFunctionObject> func(compileStockAndGet(
      R"(
def test(n):
  while n:
    n -= 1
  return n
)",
      "test"));
  ASSERT_NE(func, nullptr);
  Ref<> arg = Ref<>::steal(PyLong_FromLong(3));
  ASSERT_NE(arg, nullptr);

  Ref<> result = Ref<>::steal(PyObject_CallOneArg(
      reinterpret_cast<PyObject*>(func.get()), arg.get()));

  ASSERT_NE(result, nullptr);
  EXPECT_EQ(PyLong_AsLong(result.get()), 0);
  EXPECT_EQ(hook_state.calls, 0);
  EXPECT_EQ(Ci_OSR_GetBackedgeCounters(codeFromFunc(func)), nullptr);
}

TEST_F(OSRDetectionTest, HotWhileLoopBelowThresholdDoesNotCallTryOSR) {
  if (!isSupportedOSRBuild()) {
    GTEST_SKIP() << "OSR hot-loop detection MVP only runs on supported builds";
  }
  ScopedOSRFlags flags;
  ScopedOSRThreshold threshold{3};
  setOSRFlags(1, 1, kOSRRunningState);
  TryOSRHookState hook_state;
  ScopedOSRTestHook hook{&hook_state};

  runStockCode(R"(
def test(n):
  while n:
    n -= 1
  return n

result = test(2)
)");

  EXPECT_EQ(hook_state.calls, 0);
  EXPECT_EQ(longFromGlobal(*this, "result"), 0);
}

TEST_F(OSRDetectionTest, HotWhileLoopAtThresholdCallsTryOSRAndContinues) {
  if (!isSupportedOSRBuild()) {
    GTEST_SKIP() << "OSR hot-loop detection MVP only runs on supported builds";
  }
  ScopedOSRFlags flags;
  ScopedOSRThreshold threshold{3};
  setOSRFlags(1, 1, kOSRRunningState);
  TryOSRHookState hook_state;
  hook_state.return_code = 0;
  ScopedOSRTestHook hook{&hook_state};

  runStockCode(R"(
def test(n):
  while n:
    n -= 1
  return n

result = test(3)
)");

  EXPECT_EQ(hook_state.calls, 1);
  EXPECT_EQ(longFromGlobal(*this, "result"), 0);
}

TEST_F(OSRDetectionTest, TryOSRReturnOnePushesResultToPythonCaller) {
  if (!isSupportedOSRBuild()) {
    GTEST_SKIP() << "OSR hot-loop detection MVP only runs on supported builds";
  }
  ScopedOSRFlags flags;
  ScopedOSRThreshold threshold{1};
  setOSRFlags(1, 1, kOSRRunningState);
  TryOSRHookState hook_state;
  hook_state.return_code = 1;
  hook_state.result_value = 42;
  ScopedOSRTestHook hook{&hook_state};

  runStockCode(R"(
def test(n):
  while n:
    n -= 1
  return -1

result = test(3)
)");

  EXPECT_EQ(hook_state.calls, 1);
  EXPECT_EQ(longFromGlobal(*this, "result"), 42);
}

TEST_F(OSRDetectionTest, TryOSRReturnMinusOnePropagatesToPythonCaller) {
  if (!isSupportedOSRBuild()) {
    GTEST_SKIP() << "OSR hot-loop detection MVP only runs on supported builds";
  }
  ScopedOSRFlags flags;
  ScopedOSRThreshold threshold{1};
  setOSRFlags(1, 1, kOSRRunningState);
  TryOSRHookState hook_state;
  hook_state.return_code = -1;
  ScopedOSRTestHook hook{&hook_state};

  runStockCode(R"(
def test(n):
  while n:
    n -= 1
  return -1

try:
  test(3)
except RuntimeError as exc:
  result = str(exc)
)");

  EXPECT_EQ(hook_state.calls, 1);
  EXPECT_EQ(stringFromGlobal(*this, "result"), "osr test exception");
}

TEST_F(OSRDetectionTest, TryOSRReturnMinusOneFromEntryFrameReturnsNull) {
  if (!isSupportedOSRBuild()) {
    GTEST_SKIP() << "OSR hot-loop detection MVP only runs on supported builds";
  }
  ScopedOSRFlags flags;
  ScopedOSRThreshold threshold{1};
  setOSRFlags(1, 1, kOSRRunningState);
  TryOSRHookState hook_state;
  hook_state.return_code = -1;
  ScopedOSRTestHook hook{&hook_state};
  Ref<PyFunctionObject> func(compileStockAndGet(
      R"(
def test(n):
  while n:
    n -= 1
  return -1
)",
      "test"));
  ASSERT_NE(func, nullptr);
  Ref<> arg = Ref<>::steal(PyLong_FromLong(3));
  ASSERT_NE(arg, nullptr);

  Ref<> result = Ref<>::steal(PyObject_CallOneArg(
      reinterpret_cast<PyObject*>(func.get()), arg.get()));

  EXPECT_EQ(result, nullptr);
  EXPECT_EQ(hook_state.calls, 1);
  EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_RuntimeError));
  PyErr_Clear();
}

TEST_F(OSRDetectionTest, TryOSRClearingCountersDoesNotAffectCurrentLoop) {
  if (!isSupportedOSRBuild()) {
    GTEST_SKIP() << "OSR hot-loop detection MVP only runs on supported builds";
  }
  ScopedOSRFlags flags;
  ScopedOSRThreshold threshold{1};
  setOSRFlags(1, 1, kOSRRunningState);
  TryOSRHookState hook_state;
  hook_state.return_code = 0;
  hook_state.clear_counters_during_call = true;
  ScopedOSRTestHook hook{&hook_state};

  runStockCode(R"(
def test(n):
  while n:
    n -= 1
  return n

result = test(3)
)");

  EXPECT_GE(hook_state.calls, 1);
  EXPECT_EQ(longFromGlobal(*this, "result"), 0);
}

TEST_F(OSRDetectionTest, HotForLoopDoesNotCallTryOSR) {
  if (!isSupportedOSRBuild()) {
    GTEST_SKIP() << "OSR hot-loop detection MVP only runs on supported builds";
  }
  ScopedOSRFlags flags;
  ScopedOSRThreshold threshold{1};
  setOSRFlags(1, 1, kOSRRunningState);
  TryOSRHookState hook_state;
  ScopedOSRTestHook hook{&hook_state};

  runStockCode(R"(
def test(n):
  total = 0
  for i in range(n):
    total += i
  return total

result = test(4)
)");

  EXPECT_EQ(hook_state.calls, 0);
  EXPECT_EQ(longFromGlobal(*this, "result"), 6);
}

TEST_F(OSRDetectionTest, ComputeJumpTargetMatchesBytecodeInstructionTarget) {
  Ref<PyFunctionObject> func(compileStockAndGet(
      R"(
def test(n):
  while n:
    n -= 1
  return n
)",
      "test"));
  ASSERT_NE(func, nullptr);

  BorrowedRef<PyCodeObject> code = codeFromFunc(func);
  std::optional<jit::BytecodeInstruction> backward_jump;
  for (jit::BytecodeInstruction bci : jit::BytecodeInstructionBlock{code}) {
    if (bci.opcode() == JUMP_BACKWARD) {
      backward_jump = bci;
      break;
    }
  }
  ASSERT_TRUE(backward_jump.has_value());

  uint32_t source_idx = backward_jump->opcodeIndex().value();
  uint32_t target_idx = backward_jump->getJumpTarget().asIndex().value();
  uint32_t oparg = backward_jump->oparg();

  EXPECT_EQ(
      Ci_OSR_ComputeJumpTargetIndex(code, source_idx, oparg), target_idx);
}

TEST_F(
    OSRDetectionTest,
    ComputeJumpTargetHandlesExtendedArgAndInlineCacheEntry) {
  std::vector<unsigned char> bc;
  for (int i = 0; i < 260; i++) {
    bc.push_back(NOP);
    bc.push_back(0);
  }
  bc.push_back(EXTENDED_ARG);
  bc.push_back(1);
  bc.push_back(JUMP_BACKWARD);
  bc.push_back(2);
  bc.push_back(CACHE);
  bc.push_back(0);

  Ref<PyCodeObject> code = makeCodeObject(bc);
  ASSERT_NE(code, nullptr);

  std::optional<jit::BytecodeInstruction> backward_jump;
  for (jit::BytecodeInstruction bci : jit::BytecodeInstructionBlock{code}) {
    if (bci.opcode() == JUMP_BACKWARD) {
      backward_jump = bci;
      break;
    }
  }
  ASSERT_TRUE(backward_jump.has_value());
  ASSERT_EQ(backward_jump->oparg(), 0x0102);

  uint32_t source_idx = backward_jump->opcodeIndex().value();
  uint32_t target_idx = backward_jump->getJumpTarget().asIndex().value();

  EXPECT_EQ(target_idx, 5);
  EXPECT_EQ(
      Ci_OSR_ComputeJumpTargetIndex(code, source_idx, backward_jump->oparg()),
      target_idx);
}

TEST_F(OSRDetectionTest, IsEligibleAcceptsNormalFunctionWithEmptyStack) {
  ScopedFrameMode frame_mode{jit::FrameMode::kNormal};
  Ref<PyFunctionObject> func(compileStockAndGet(
      R"(
def test():
  return 1
)",
      "test"));
  ASSERT_NE(func, nullptr);
  InterpreterFrameHolder frame{func};

  EXPECT_TRUE(
      Ci_OSR_IsEligible(PyThreadState_Get(), frame.get(), codeFromFunc(func)));
}

TEST_F(OSRDetectionTest, IsEligibleRejectsGeneratorCode) {
  ScopedFrameMode frame_mode{jit::FrameMode::kNormal};
  Ref<PyFunctionObject> normal_func(compileStockAndGet(
      R"(
def normal():
  return 1
)",
      "normal"));
  Ref<PyFunctionObject> generator_func(compileStockAndGet(
      R"(
def generator():
  yield 1
)",
      "generator"));
  ASSERT_NE(normal_func, nullptr);
  ASSERT_NE(generator_func, nullptr);
  InterpreterFrameHolder frame{normal_func};

  EXPECT_FALSE(Ci_OSR_IsEligible(
      PyThreadState_Get(), frame.get(), codeFromFunc(generator_func)));
}

TEST_F(OSRDetectionTest, IsEligibleRejectsCoroutineAndAsyncGeneratorCode) {
  ScopedFrameMode frame_mode{jit::FrameMode::kNormal};
  Ref<PyFunctionObject> normal_func(compileStockAndGet(
      R"(
def normal():
  return 1
)",
      "normal"));
  Ref<PyFunctionObject> coroutine_func(compileStockAndGet(
      R"(
async def coroutine():
  return 1
)",
      "coroutine"));
  Ref<PyFunctionObject> async_generator_func(compileStockAndGet(
      R"(
async def async_generator():
  yield 1
)",
      "async_generator"));
  ASSERT_NE(normal_func, nullptr);
  ASSERT_NE(coroutine_func, nullptr);
  ASSERT_NE(async_generator_func, nullptr);
  InterpreterFrameHolder frame{normal_func};

  EXPECT_FALSE(Ci_OSR_IsEligible(
      PyThreadState_Get(), frame.get(), codeFromFunc(coroutine_func)));
  EXPECT_FALSE(Ci_OSR_IsEligible(
      PyThreadState_Get(), frame.get(), codeFromFunc(async_generator_func)));
}

TEST_F(OSRDetectionTest, IsEligibleRejectsNonFunctionFrame) {
  ScopedFrameMode frame_mode{jit::FrameMode::kNormal};
  Ref<PyFunctionObject> func(compileStockAndGet(
      R"(
def test():
  return 1
)",
      "test"));
  ASSERT_NE(func, nullptr);
  InterpreterFrameHolder frame{func};

  replaceFrameFuncobj(frame.get(), Py_None);

  EXPECT_FALSE(
      Ci_OSR_IsEligible(PyThreadState_Get(), frame.get(), codeFromFunc(func)));
}

TEST_F(OSRDetectionTest, IsEligibleRejectsNonNormalFrameMode) {
  ScopedFrameMode frame_mode{jit::FrameMode::kNormal};
  Ref<PyFunctionObject> func(compileStockAndGet(
      R"(
def test():
  return 1
)",
      "test"));
  ASSERT_NE(func, nullptr);
  InterpreterFrameHolder frame{func};

  {
    ScopedFrameMode non_normal_frame_mode{jit::FrameMode::kLightweight};
    EXPECT_FALSE(Ci_OSR_IsEligible(
        PyThreadState_Get(), frame.get(), codeFromFunc(func)));
  }
}

TEST_F(OSRDetectionTest, IsEligibleRejectsEscapedFrameObject) {
  ScopedFrameMode frame_mode{jit::FrameMode::kNormal};
  Ref<PyFunctionObject> func(compileStockAndGet(
      R"(
def test():
  return 1
)",
      "test"));
  ASSERT_NE(func, nullptr);
  InterpreterFrameHolder frame{func};
  ASSERT_NE(_PyFrame_GetFrameObject(frame.get()), nullptr);

  EXPECT_FALSE(
      Ci_OSR_IsEligible(PyThreadState_Get(), frame.get(), codeFromFunc(func)));
}

TEST_F(OSRDetectionTest, IsEligibleRejectsNonEmptyOperandStack) {
  ScopedFrameMode frame_mode{jit::FrameMode::kNormal};
  Ref<PyFunctionObject> func(compileStockAndGet(
      R"(
def test():
  return 1
)",
      "test"));
  ASSERT_NE(func, nullptr);
  InterpreterFrameHolder frame{func};

  _PyStackRef* stackbase = _PyFrame_Stackbase(frame.get());
  stackbase[0] = PyStackRef_FromPyObjectNew(Py_None);
  frame.get()->stackpointer = stackbase + 1;

  EXPECT_FALSE(
      Ci_OSR_IsEligible(PyThreadState_Get(), frame.get(), codeFromFunc(func)));
}
