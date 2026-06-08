// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Common/code.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Jit/behavior_classifier.h"
#include "cinderx/Jit/config.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <optional>

using namespace jit;

namespace {

BorrowedRef<PyCodeObject> codeFromFunc(Ref<>& func) {
  JIT_CHECK(PyFunction_Check(func), "expected a Python function");
  auto pyfunc = reinterpret_cast<PyFunctionObject*>(func.get());
  return BorrowedRef<PyCodeObject>{pyfunc->func_code};
}

class ScopedAutoJitConfig {
 public:
  ScopedAutoJitConfig()
      : compile_after_n_calls_{getMutableConfig().compile_after_n_calls},
        auto_classify_{getMutableConfig().auto_classify},
        enable_startup_init_policy_{
            getMutableConfig().enable_startup_init_policy} {}

  ~ScopedAutoJitConfig() {
    getMutableConfig().compile_after_n_calls = compile_after_n_calls_;
    getMutableConfig().auto_classify = auto_classify_;
    getMutableConfig().enable_startup_init_policy =
        enable_startup_init_policy_;
  }

 private:
  std::optional<uint32_t> compile_after_n_calls_;
  bool auto_classify_;
  bool enable_startup_init_policy_;
};

} // namespace

TEST(BehaviorClassifierTest, StructureKeyPackRoundTripsAllFields) {
  StructureKey key{
      Family::Mixed,
      encodeMixedShape(WorkDim::Dynamic, WorkDim::Dispatch),
      3,
      true,
      true,
      true,
      static_cast<uint8_t>(kRiskDynamic | kRiskException | kRiskHugeCode),
      2,
      static_cast<uint8_t>(
          activeDimMaskFor(WorkDim::Compute) |
          activeDimMaskFor(WorkDim::Object) |
          activeDimMaskFor(WorkDim::Dispatch)),
  };

  uint32_t payload = key.pack();
  EXPECT_EQ(payload & kSkeyValidBit, 0);
  EXPECT_EQ(payload & ~kSkeyPayloadMask, 0);

  StructureKey decoded = StructureKey::unpack(payload);
  EXPECT_EQ(decoded.family, Family::Mixed);
  EXPECT_EQ(
      decoded.mixed_shape,
      encodeMixedShape(WorkDim::Dynamic, WorkDim::Dispatch));
  EXPECT_EQ(decoded.loop_score, 3);
  EXPECT_TRUE(decoded.is_suspendable);
  EXPECT_TRUE(decoded.is_static);
  EXPECT_TRUE(decoded.is_synthetic);
  EXPECT_TRUE(decoded.highRisk());
  EXPECT_EQ(decoded.risk_reason, key.risk_reason);
  EXPECT_EQ(decoded.code_size_bucket, 2);
  EXPECT_EQ(decoded.active_dim_mask, key.active_dim_mask);
  EXPECT_TRUE(decoded.computeHint());
  EXPECT_FALSE(decoded.computeDominantHint());
  EXPECT_EQ(decoded.activeDimCount(), 3);
}

TEST(BehaviorClassifierTest, OpcodeClassGoldenExamples) {
  EXPECT_EQ(opcodeClassOf(LOAD_GLOBAL), OpcodeClass::Dynamic);
  EXPECT_EQ(opcodeClassOf(CALL), OpcodeClass::Dispatch);
  EXPECT_EQ(opcodeClassOf(BUILD_STRING), OpcodeClass::Dynamic);
  EXPECT_EQ(opcodeClassOf(TO_BOOL), OpcodeClass::Control);
  EXPECT_EQ(opcodeClassOf(SEND), OpcodeClass::Suspend);
  EXPECT_EQ(opcodeClassOf(LOAD_ATTR), OpcodeClass::Object);
  EXPECT_EQ(opcodeClassOf(PRIMITIVE_BINARY_OP), OpcodeClass::Compute);
  EXPECT_EQ(opcodeClassOf(CACHE), OpcodeClass::Ignored);
  EXPECT_EQ(opcodeClassOf(LOAD_CONST), OpcodeClass::Neutral);
  EXPECT_EQ(opcodeClassOf(-1), OpcodeClass::Invalid);
}

TEST(BehaviorClassifierTest, ComputeThresholdWarmsUpSteadyStateStartupLikeWork) {
  GateContext ctx{false};

  StructureKey trivial{Family::Trivial};
  auto trivial_decision = computeThreshold(trivial, ctx, 2);
  EXPECT_EQ(trivial_decision.limit, 4);
  EXPECT_EQ(trivial_decision.branch_reason, BranchReason::LowRoi);

  StructureKey static_trivial{Family::Trivial};
  static_trivial.is_static = true;
  auto static_decision = computeThreshold(static_trivial, ctx, 2);
  EXPECT_EQ(static_decision.limit, 2);
  EXPECT_EQ(static_decision.branch_reason, BranchReason::None);

  StructureKey suspendable_trivial{Family::Trivial};
  suspendable_trivial.is_suspendable = true;
  auto suspendable_trivial_decision =
      computeThreshold(suspendable_trivial, ctx, 2);
  EXPECT_EQ(suspendable_trivial_decision.limit, 1000);
  EXPECT_EQ(
      suspendable_trivial_decision.branch_reason, BranchReason::LowRoi);

  StructureKey risky_trivial{Family::Trivial};
  risky_trivial.risk_reason = kRiskHugeCode;
  auto risky_trivial_decision = computeThreshold(risky_trivial, ctx, 2);
  EXPECT_EQ(risky_trivial_decision.limit, 2);
  EXPECT_EQ(risky_trivial_decision.branch_reason, BranchReason::None);

  StructureKey risk_dispatch{Family::CallDispatcher};
  risk_dispatch.risk_reason = kRiskDynamic;
  auto risk_decision = computeThreshold(risk_dispatch, ctx, 2);
  EXPECT_GE(risk_decision.limit, 65536);
  EXPECT_EQ(risk_decision.branch_reason, BranchReason::RiskDefer);

  StructureKey suspendable{Family::AsyncStateMachine};
  suspendable.is_suspendable = true;
  suspendable.risk_reason = kRiskSuspend;
  auto suspendable_decision = computeThreshold(suspendable, ctx, 2);
  EXPECT_GE(suspendable_decision.limit, 65536);
  EXPECT_EQ(suspendable_decision.branch_reason, BranchReason::RiskDefer);

  StructureKey huge_code{Family::ObjectManipulator};
  huge_code.risk_reason = kRiskHugeCode;
  huge_code.code_size_bucket = 3;
  auto huge_code_decision = computeThreshold(huge_code, ctx, 2);
  EXPECT_GE(huge_code_decision.limit, 65536);
  EXPECT_EQ(huge_code_decision.branch_reason, BranchReason::RiskDefer);

  StructureKey hot_loop{Family::NumericLoop};
  hot_loop.loop_score = 2;
  hot_loop.risk_reason = kRiskHugeCode;
  auto loop_decision = computeThreshold(hot_loop, ctx, 2);
  EXPECT_EQ(loop_decision.limit, 2);
  EXPECT_EQ(loop_decision.branch_reason, BranchReason::None);
}

TEST(BehaviorClassifierTest, StartupContextPreservesStartupReasonForImportLikeWork) {
  ScopedAutoJitConfig config_guard;
  getMutableConfig().enable_startup_init_policy = true;

  GateContext startup{true};
  StructureKey dispatcher{Family::CallDispatcher};
  auto startup_decision = computeThreshold(dispatcher, startup, 2);
  EXPECT_EQ(startup_decision.limit, 1000);
  EXPECT_EQ(startup_decision.branch_reason, BranchReason::LowRoi);

  GateContext steady_state{false};
  auto steady_state_decision = computeThreshold(dispatcher, steady_state, 2);
  EXPECT_EQ(steady_state_decision.limit, 1000);
  EXPECT_EQ(steady_state_decision.branch_reason, BranchReason::LowRoi);

  StructureKey loop{Family::NumericLoop};
  loop.loop_score = 2;
  auto loop_decision = computeThreshold(loop, startup, 2);
  EXPECT_EQ(loop_decision.limit, 2);
  EXPECT_EQ(loop_decision.branch_reason, BranchReason::None);

  StructureKey suspendable{Family::AsyncStateMachine};
  suspendable.is_suspendable = true;
  suspendable.risk_reason = kRiskSuspend;
  auto suspendable_decision = computeThreshold(suspendable, startup, 2);
  EXPECT_GE(suspendable_decision.limit, 65536);
  EXPECT_EQ(suspendable_decision.branch_reason, BranchReason::RiskDefer);

  StructureKey static_dispatcher{Family::CallDispatcher};
  static_dispatcher.is_static = true;
  auto static_decision = computeThreshold(static_dispatcher, startup, 2);
  EXPECT_EQ(static_decision.limit, 2);
  EXPECT_EQ(static_decision.branch_reason, BranchReason::None);
}

TEST(BehaviorClassifierTest, StartupPolicyDefersRiskyImportWork) {
  ScopedAutoJitConfig config_guard;
  getMutableConfig().enable_startup_init_policy = true;

  GateContext startup{true};
  StructureKey risky_branch{Family::BranchFSM};
  risky_branch.loop_score = 3;
  risky_branch.risk_reason = kRiskException | kRiskHugeCode;
  risky_branch.code_size_bucket = 3;
  auto risky_decision = computeThreshold(risky_branch, startup, 2);
  EXPECT_GE(risky_decision.limit, 65536);
  EXPECT_EQ(risky_decision.branch_reason, BranchReason::RiskDefer);

  StructureKey numeric_loop{Family::NumericLoop};
  numeric_loop.loop_score = 3;
  numeric_loop.risk_reason = kRiskHugeCode;
  numeric_loop.code_size_bucket = 3;
  auto numeric_decision = computeThreshold(numeric_loop, startup, 2);
  EXPECT_EQ(numeric_decision.limit, 2);
  EXPECT_EQ(numeric_decision.branch_reason, BranchReason::None);
}

TEST(BehaviorClassifierTest, ImportWindowDefersHighCostNonnumericWork) {
  ScopedAutoJitConfig config_guard;
  getMutableConfig().enable_startup_init_policy = true;

  GateContext import_window{true};
  GateContext steady_state{false};

  StructureKey large_branch{Family::BranchFSM};
  large_branch.loop_score = 2;
  large_branch.code_size_bucket = 2;
  large_branch.active_dim_mask = activeDimMaskFor(WorkDim::Control) |
      activeDimMaskFor(WorkDim::Dispatch);
  auto large_branch_decision =
      computeThreshold(large_branch, import_window, 2);
  EXPECT_GE(large_branch_decision.limit, 65536);
  EXPECT_EQ(large_branch_decision.branch_reason, BranchReason::StartupInit);

  auto steady_state_decision = computeThreshold(large_branch, steady_state, 2);
  EXPECT_EQ(steady_state_decision.limit, 1000);
  EXPECT_EQ(steady_state_decision.branch_reason, BranchReason::LowRoi);

  StructureKey medium_branch{Family::BranchFSM};
  medium_branch.loop_score = 1;
  medium_branch.code_size_bucket = 1;
  medium_branch.active_dim_mask = activeDimMaskFor(WorkDim::Control) |
      activeDimMaskFor(WorkDim::Dispatch);
  auto medium_branch_decision =
      computeThreshold(medium_branch, import_window, 2);
  EXPECT_GE(medium_branch_decision.limit, 65536);
  EXPECT_EQ(medium_branch_decision.branch_reason, BranchReason::StartupInit);

  auto post_import_medium_branch =
      computeThreshold(medium_branch, steady_state, 2);
  EXPECT_EQ(post_import_medium_branch.limit, 2);
  EXPECT_EQ(post_import_medium_branch.branch_reason, BranchReason::None);

  StructureKey risky_object{Family::ObjectManipulator};
  risky_object.loop_score = 1;
  risky_object.risk_reason = kRiskHugeCode;
  risky_object.code_size_bucket = 3;
  risky_object.active_dim_mask = activeDimMaskFor(WorkDim::Object);
  auto risky_object_decision =
      computeThreshold(risky_object, import_window, 2);
  EXPECT_GE(risky_object_decision.limit, 65536);
  EXPECT_EQ(risky_object_decision.branch_reason, BranchReason::RiskDefer);

  StructureKey low_cost_dispatcher{Family::CallDispatcher};
  low_cost_dispatcher.code_size_bucket = 0;
  low_cost_dispatcher.active_dim_mask = activeDimMaskFor(WorkDim::Dispatch);
  auto low_cost_decision =
      computeThreshold(low_cost_dispatcher, import_window, 2);
  EXPECT_EQ(low_cost_decision.limit, 1000);
  EXPECT_EQ(low_cost_decision.branch_reason, BranchReason::LowRoi);

  StructureKey numeric_loop{Family::NumericLoop};
  numeric_loop.loop_score = 2;
  numeric_loop.risk_reason = kRiskHugeCode;
  numeric_loop.code_size_bucket = 3;
  numeric_loop.active_dim_mask = activeDimMaskFor(WorkDim::Compute) |
      activeDimMaskFor(WorkDim::Control);
  auto numeric_loop_decision =
      computeThreshold(numeric_loop, import_window, 2);
  EXPECT_EQ(numeric_loop_decision.limit, 2);
  EXPECT_EQ(numeric_loop_decision.branch_reason, BranchReason::None);

  StructureKey compute_mixed{Family::Mixed};
  compute_mixed.mixed_shape =
      encodeMixedShape(WorkDim::Compute, WorkDim::Object);
  compute_mixed.loop_score = 2;
  compute_mixed.risk_reason = kRiskHugeCode;
  compute_mixed.code_size_bucket = 2;
  compute_mixed.active_dim_mask = activeDimMaskFor(WorkDim::Compute) |
      activeDimMaskFor(WorkDim::Object) | activeDimMaskFor(WorkDim::Control);
  auto compute_mixed_decision =
      computeThreshold(compute_mixed, import_window, 2);
  EXPECT_EQ(compute_mixed_decision.limit, 2);
  EXPECT_EQ(compute_mixed_decision.branch_reason, BranchReason::None);

  StructureKey compute_object{Family::ObjectManipulator};
  compute_object.loop_score = 2;
  compute_object.risk_reason = kRiskHugeCode;
  compute_object.code_size_bucket = 2;
  compute_object.active_dim_mask = activeDimMaskFor(WorkDim::Object) |
      activeDimMaskFor(WorkDim::Compute) | activeDimMaskFor(WorkDim::Control);
  auto compute_object_decision =
      computeThreshold(compute_object, import_window, 2);
  EXPECT_GE(compute_object_decision.limit, 65536);
  EXPECT_EQ(compute_object_decision.branch_reason, BranchReason::RiskDefer);

  StructureKey small_object{Family::ObjectManipulator};
  small_object.loop_score = 1;
  small_object.code_size_bucket = 0;
  small_object.active_dim_mask = activeDimMaskFor(WorkDim::Object);
  auto small_object_decision =
      computeThreshold(small_object, import_window, 2);
  EXPECT_EQ(small_object_decision.limit, 2);
  EXPECT_EQ(small_object_decision.branch_reason, BranchReason::None);

  auto post_import_risky_object =
      computeThreshold(risky_object, steady_state, 2);
  EXPECT_EQ(post_import_risky_object.limit, 2);
  EXPECT_EQ(post_import_risky_object.branch_reason, BranchReason::None);
}

TEST(BehaviorClassifierTest, SteadyStateAllowsStructuredNonBranchWork) {
  GateContext steady_state{false};

  StructureKey branch_big{Family::BranchFSM};
  branch_big.loop_score = 3;
  branch_big.risk_reason = kRiskException | kRiskHugeCode;
  branch_big.code_size_bucket = 2;
  auto branch_decision = computeThreshold(branch_big, steady_state, 2);
  EXPECT_EQ(branch_decision.limit, 1000);
  EXPECT_EQ(branch_decision.branch_reason, BranchReason::LowRoi);

  StructureKey huge_only_loop{Family::ObjectManipulator};
  huge_only_loop.loop_score = 3;
  huge_only_loop.risk_reason = kRiskHugeCode;
  huge_only_loop.code_size_bucket = 3;
  auto huge_only_decision = computeThreshold(huge_only_loop, steady_state, 2);
  EXPECT_EQ(huge_only_decision.limit, 2);
  EXPECT_EQ(huge_only_decision.branch_reason, BranchReason::None);

  StructureKey numeric_big{Family::NumericLoop};
  numeric_big.loop_score = 3;
  numeric_big.risk_reason = kRiskException | kRiskHugeCode;
  numeric_big.code_size_bucket = 2;
  auto numeric_decision = computeThreshold(numeric_big, steady_state, 2);
  EXPECT_EQ(numeric_decision.limit, 2);
  EXPECT_EQ(numeric_decision.branch_reason, BranchReason::None);
}

TEST(BehaviorClassifierTest, SteadyStateWarmsUpLargeBranchStateMachines) {
  GateContext steady_state{false};

  StructureKey branch_loop{Family::BranchFSM};
  branch_loop.loop_score = 3;
  branch_loop.code_size_bucket = 1;
  branch_loop.risk_reason = kRiskException;
  auto branch_decision = computeThreshold(branch_loop, steady_state, 2);
  EXPECT_EQ(branch_decision.limit, 1000);
  EXPECT_EQ(branch_decision.branch_reason, BranchReason::LowRoi);

  StructureKey low_risk_branch_loop{Family::BranchFSM};
  low_risk_branch_loop.loop_score = 3;
  low_risk_branch_loop.code_size_bucket = 1;
  auto low_risk_branch_decision =
      computeThreshold(low_risk_branch_loop, steady_state, 2);
  EXPECT_EQ(low_risk_branch_decision.limit, 2);
  EXPECT_EQ(low_risk_branch_decision.branch_reason, BranchReason::None);

  StructureKey large_low_risk_branch_loop{Family::BranchFSM};
  large_low_risk_branch_loop.loop_score = 3;
  large_low_risk_branch_loop.code_size_bucket = 2;
  auto large_low_risk_branch_decision =
      computeThreshold(large_low_risk_branch_loop, steady_state, 2);
  EXPECT_EQ(large_low_risk_branch_decision.limit, 1000);
  EXPECT_EQ(
      large_low_risk_branch_decision.branch_reason, BranchReason::LowRoi);

  StructureKey numeric_loop{Family::NumericLoop};
  numeric_loop.loop_score = 3;
  numeric_loop.code_size_bucket = 1;
  auto numeric_decision = computeThreshold(numeric_loop, steady_state, 2);
  EXPECT_EQ(numeric_decision.limit, 2);
  EXPECT_EQ(numeric_decision.branch_reason, BranchReason::None);

  StructureKey object_loop{Family::ObjectManipulator};
  object_loop.loop_score = 1;
  object_loop.code_size_bucket = 3;
  auto object_decision = computeThreshold(object_loop, steady_state, 2);
  EXPECT_EQ(object_decision.limit, 2);
  EXPECT_EQ(object_decision.branch_reason, BranchReason::None);
}

TEST(
    BehaviorClassifierTest,
    SteadyStateRiskDefersExpectedExceptionLoopShape) {
  GateContext steady_state{false};

  StructureKey tuple_memo_miss{Family::BranchFSM};
  tuple_memo_miss.loop_score = 2;
  tuple_memo_miss.code_size_bucket = 1;
  tuple_memo_miss.risk_reason = kRiskException;
  tuple_memo_miss.active_dim_mask = activeDimMaskFor(WorkDim::Control);
  auto tuple_memo_miss_decision =
      computeThreshold(tuple_memo_miss, steady_state, 2);
  EXPECT_GE(tuple_memo_miss_decision.limit, 65536);
  EXPECT_EQ(tuple_memo_miss_decision.branch_reason, BranchReason::RiskDefer);

  StructureKey dispatching_exception_loop{Family::BranchFSM};
  dispatching_exception_loop.loop_score = 2;
  dispatching_exception_loop.code_size_bucket = 1;
  dispatching_exception_loop.risk_reason = kRiskException;
  dispatching_exception_loop.active_dim_mask =
      activeDimMaskFor(WorkDim::Control) |
      activeDimMaskFor(WorkDim::Dispatch);
  auto dispatching_exception_loop_decision =
      computeThreshold(dispatching_exception_loop, steady_state, 2);
  EXPECT_EQ(dispatching_exception_loop_decision.limit, 1000);
  EXPECT_EQ(
      dispatching_exception_loop_decision.branch_reason, BranchReason::LowRoi);
}

TEST(BehaviorClassifierTest, SteadyStateWarmsUpTinyStartupLikeWork) {
  GateContext steady_state{false};

  StructureKey tiny_branch{Family::BranchFSM};
  tiny_branch.loop_score = 0;
  tiny_branch.code_size_bucket = 0;
  auto tiny_branch_decision = computeThreshold(tiny_branch, steady_state, 2);
  EXPECT_EQ(tiny_branch_decision.limit, 1000);
  EXPECT_EQ(tiny_branch_decision.branch_reason, BranchReason::LowRoi);

  StructureKey tiny_object{Family::ObjectManipulator};
  tiny_object.loop_score = 0;
  tiny_object.code_size_bucket = 0;
  auto tiny_object_decision = computeThreshold(tiny_object, steady_state, 2);
  EXPECT_EQ(tiny_object_decision.limit, 2);
  EXPECT_EQ(tiny_object_decision.branch_reason, BranchReason::None);

  StructureKey medium_object{Family::ObjectManipulator};
  medium_object.loop_score = 0;
  medium_object.code_size_bucket = 1;
  auto medium_object_decision =
      computeThreshold(medium_object, steady_state, 2);
  EXPECT_GE(medium_object_decision.limit, 65536);
  EXPECT_EQ(medium_object_decision.branch_reason, BranchReason::LowRoi);

  StructureKey tiny_numeric{Family::NumericLoop};
  tiny_numeric.loop_score = 0;
  tiny_numeric.code_size_bucket = 0;
  auto tiny_numeric_decision = computeThreshold(tiny_numeric, steady_state, 2);
  EXPECT_EQ(tiny_numeric_decision.limit, 2);
  EXPECT_EQ(tiny_numeric_decision.branch_reason, BranchReason::None);
}

class BehaviorClassifierRuntimeTest : public RuntimeTest {};

TEST_F(
    BehaviorClassifierRuntimeTest,
    AutoClassifyCompilesTinyObjectHelpersOnPythonCallPath) {
  ScopedAutoJitConfig config_guard;
  getMutableConfig().compile_after_n_calls = 2;
  getMutableConfig().auto_classify = true;
  getMutableConfig().enable_startup_init_policy = false;

  runStockCode(R"(
import cinderx.jit as jit

class Grid:
    def __init__(self):
        self.width = 1
        self.data = [1]

    def _idx(self, x, y):
        return y * self.width + x

    def __getitem__(self, x_y):
        x, y = x_y
        return self.data[self._idx(x, y)]

grid = Grid()
for _ in range(16):
    Grid.__getitem__(grid, (0, 0))

assert jit.is_jit_compiled(Grid.__getitem__), jit.count_interpreted_calls(Grid.__getitem__)
)");
}

TEST_F(
    BehaviorClassifierRuntimeTest,
    AutoClassifyCompilesLowRiskSparseBranchLoops) {
  ScopedAutoJitConfig config_guard;
  getMutableConfig().compile_after_n_calls = 2;
  getMutableConfig().auto_classify = true;
  getMutableConfig().enable_startup_init_policy = false;

  runStockCode(R"(
from array import array
import time
import cinderx.jit as jit

def sparse_like(M, y, val, row, col, x, num_iterations):
    range_it = range(num_iterations)
    t0 = time.perf_counter()

    for _ in range_it:
        for r in range(M):
            sa = 0.0
            for i in range(row[r], row[r + 1]):
                sa += x[col[i]] * val[i]
            y[r] = sa

    return time.perf_counter() - t0

N = 16
nr = 4
nz = N * nr
x = array("d", [0]) * N
y = array("d", [0]) * N
val = array("d", [0]) * nz
col = array("i", [0]) * nz
row = array("i", [0]) * (N + 1)
for r in range(N):
    row[r + 1] = row[r] + nr

for _ in range(7):
    sparse_like(N, y, val, row, col, x, 16)

assert jit.is_jit_compiled(sparse_like), jit.count_interpreted_calls(sparse_like)
)");
}

TEST_F(BehaviorClassifierRuntimeTest, DerivesTrivialForThinFunction) {
  Ref<> func = compileStockAndGet(
      R"(
def thin(x):
    return x
)",
      "thin");

  auto key = deriveStructureKey(codeFromFunc(func));
  ASSERT_TRUE(key.has_value());
  EXPECT_EQ(key->family, Family::Trivial);
  EXPECT_EQ(key->mixed_shape, kMixedShapeNone);
  EXPECT_EQ(key->loop_score, 0);
  EXPECT_FALSE(key->highRisk());
}

TEST_F(
    BehaviorClassifierRuntimeTest,
    DerivesLowRiskBranchForSparseMatMultShape) {
  Ref<> func = compileStockAndGet(
      R"(
import time

def sparse_like(M, y, val, row, col, x, num_iterations):
    range_it = range(num_iterations)
    t0 = time.perf_counter()

    for _ in range_it:
        for r in range(M):
            sa = 0.0
            for i in range(row[r], row[r + 1]):
                sa += x[col[i]] * val[i]
            y[r] = sa

    return time.perf_counter() - t0
)",
      "sparse_like");

  auto key = deriveStructureKey(codeFromFunc(func));
  ASSERT_TRUE(key.has_value());
  EXPECT_EQ(key->family, Family::BranchFSM);
  EXPECT_GT(key->loop_score, 0);
  EXPECT_EQ(key->code_size_bucket, 1);
  EXPECT_FALSE(key->highRisk());
}

TEST_F(
    BehaviorClassifierRuntimeTest,
    DetectsSuspendableFunctionsForEarlyAutoJitDefer) {
  ScopedAutoJitConfig config_guard;
  getMutableConfig().enable_startup_init_policy = true;

  Ref<> thin = compileStockAndGet(
      R"(
def thin(x):
    return x
)",
      "thin");
  GateContext steady_state{false};
  GateContext startup{true};
  EXPECT_FALSE(
      shouldDeferSuspendableAutoJitWithoutStructureKey(
          codeFromFunc(thin), steady_state));
  EXPECT_FALSE(
      shouldDeferSuspendableAutoJitWithoutStructureKey(
          codeFromFunc(thin), startup));

  Ref<> gen = compileStockAndGet(
      R"(
def gen():
    yield 1
)",
      "gen");
  EXPECT_TRUE(
      shouldDeferSuspendableAutoJitWithoutStructureKey(
          codeFromFunc(gen), startup));
  EXPECT_FALSE(
      shouldDeferSuspendableAutoJitWithoutStructureKey(
          codeFromFunc(gen), steady_state));
}

TEST_F(
    BehaviorClassifierRuntimeTest,
    GetOrComputeStructureKeyCachesInCodeExtra) {
  Ref<> func = compileStockAndGet(
      R"(
def thin(x):
    return x
)",
      "thin");

  auto code = codeFromFunc(func);
  CodeExtra* extra = codeExtra(code);
  ASSERT_NE(extra, nullptr);
  EXPECT_EQ(Ci_code_extra_load_skey_acquire(extra) & kSkeyValidBit, 0u);

  auto first = getOrComputeStructureKey(code, extra);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->family, Family::Trivial);

  uint32_t cached_word = Ci_code_extra_load_skey_acquire(extra);
  EXPECT_NE(cached_word & kSkeyValidBit, 0u);

  auto second = getOrComputeStructureKey(code, extra);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->pack(), first->pack());
}

TEST_F(BehaviorClassifierRuntimeTest, AutoClassifyDefersThinFunctions) {
  ScopedAutoJitConfig config_guard;
  getMutableConfig().compile_after_n_calls = 2;
  getMutableConfig().auto_classify = true;
  getMutableConfig().enable_startup_init_policy = false;

  runStockCode(R"(
import cinderx.jit as jit

def thin(x):
    return x

assert not jit.is_jit_compiled(thin)
thin(1)
thin(2)
assert not jit.is_jit_compiled(thin)

thin(3)
thin(4)
assert not jit.is_jit_compiled(thin)

for value in range(5, 33):
    thin(value)
    assert jit.is_jit_compiled(thin)

assert jit.is_jit_compiled(thin)
)");
}

TEST_F(BehaviorClassifierRuntimeTest, AutoClassifyAllowsLoopFreeNumericWork) {
  ScopedAutoJitConfig config_guard;
  getMutableConfig().compile_after_n_calls = 2;
  getMutableConfig().auto_classify = true;
  getMutableConfig().enable_startup_init_policy = false;

  runStockCode(R"(
import cinderx.jit as jit

def straight_compute(a, b):
    return (a + b) * 2

for value in range(16):
    straight_compute(value, value + 1)

assert jit.is_jit_compiled(straight_compute)
)");
}

TEST_F(BehaviorClassifierRuntimeTest, AutoClassifyAllowsGeneratorsInSteadyState) {
  ScopedAutoJitConfig config_guard;
  getMutableConfig().compile_after_n_calls = 2;
  getMutableConfig().auto_classify = true;
  getMutableConfig().enable_startup_init_policy = false;

  runStockCode(R"(
import cinderx.jit as jit

def gen():
    yield 1

assert not jit.is_jit_compiled(gen)
gen()
gen()
assert not jit.is_jit_compiled(gen)
for _ in range(1100):
    gen()
assert jit.is_jit_compiled(gen)
)");
}

TEST_F(BehaviorClassifierRuntimeTest, ImportDepthDefersStartupLikeFunctions) {
  ScopedAutoJitConfig config_guard;
  getMutableConfig().compile_after_n_calls = 2;
  getMutableConfig().auto_classify = true;
  getMutableConfig().enable_startup_init_policy = true;

  runStockCode(R"(
import _cinderx
import cinderx.jit as jit

def callback():
    return 1

def dispatch(func):
    return func()

_cinderx._autojit_import_enter()
try:
    dispatch(callback)
    dispatch(callback)
    dispatch(callback)
    assert not jit.is_jit_compiled(dispatch)
finally:
    _cinderx._autojit_import_leave()

assert _cinderx._autojit_import_depth() == 0
dispatch(callback)
assert not jit.is_jit_compiled(dispatch)
assert jit.count_interpreted_calls(dispatch) == 4
)");
}

TEST_F(BehaviorClassifierRuntimeTest, DefaultImportProviderTracksFindAndLoadDepth) {
  runStockCode(R"(
import os
import sys
os.environ.pop("CINDERX_AUTOJIT_IMPORT_PROVIDER", None)
import cinderx
import _cinderx
bootstrap = sys.modules["importlib._bootstrap"]

observed_depths = []

class ProbeFinder:
    def find_spec(self, fullname, path=None, target=None):
        if fullname == "autojit_probe_missing_default":
            observed_depths.append(_cinderx._autojit_import_depth())
        return None

finder = ProbeFinder()
sys.meta_path.insert(0, finder)
try:
    try:
        __import__("autojit_probe_missing_default")
    except ModuleNotFoundError:
        pass
finally:
    sys.meta_path.remove(finder)

assert getattr(
    bootstrap._find_and_load,
    "_cinderx_autojit_import_provider",
    None,
) == "find_and_load"
assert observed_depths, observed_depths
assert all(depth > 0 for depth in observed_depths), observed_depths
assert _cinderx._autojit_import_depth() == 0
)");
}

TEST_F(BehaviorClassifierRuntimeTest, ImportProviderOffLeavesDepthZero) {
  runStockCode(R"(
import os
import sys
os.environ["CINDERX_AUTOJIT_IMPORT_PROVIDER"] = "off"
import cinderx
import _cinderx

observed_depths = []

class ProbeFinder:
    def find_spec(self, fullname, path=None, target=None):
        if fullname == "autojit_probe_missing_off":
            observed_depths.append(_cinderx._autojit_import_depth())
        return None

finder = ProbeFinder()
sys.meta_path.insert(0, finder)
try:
    try:
        __import__("autojit_probe_missing_off")
    except ModuleNotFoundError:
        pass
finally:
    sys.meta_path.remove(finder)

assert observed_depths and all(depth == 0 for depth in observed_depths)
assert _cinderx._autojit_import_depth() == 0
)");
}

TEST_F(BehaviorClassifierRuntimeTest, BuiltinsImportProviderTracksDepth) {
  runStockCode(R"(
import os
import sys
os.environ["CINDERX_AUTOJIT_IMPORT_PROVIDER"] = "builtins"
import cinderx
import _cinderx
builtins = sys.modules["builtins"]

observed_depths = []

class ProbeFinder:
    def find_spec(self, fullname, path=None, target=None):
        if fullname == "autojit_probe_missing_builtins":
            observed_depths.append(_cinderx._autojit_import_depth())
        return None

finder = ProbeFinder()
sys.meta_path.insert(0, finder)
try:
    try:
        __import__("autojit_probe_missing_builtins")
    except ModuleNotFoundError:
        pass
finally:
    sys.meta_path.remove(finder)

assert getattr(
    builtins.__import__,
    "_cinderx_autojit_import_provider",
    None,
) == "builtins"
assert observed_depths, observed_depths
assert all(depth > 0 for depth in observed_depths), observed_depths
assert _cinderx._autojit_import_depth() == 0
)");
}

TEST_F(BehaviorClassifierRuntimeTest, FindAndLoadImportProviderTracksDepth) {
  runStockCode(R"(
import os
import sys
os.environ["CINDERX_AUTOJIT_IMPORT_PROVIDER"] = "find_and_load"
import cinderx
import _cinderx
bootstrap = sys.modules["importlib._bootstrap"]

observed_depths = []

class ProbeFinder:
    def find_spec(self, fullname, path=None, target=None):
        if fullname == "autojit_probe_missing_find_and_load":
            observed_depths.append(_cinderx._autojit_import_depth())
        return None

finder = ProbeFinder()
sys.meta_path.insert(0, finder)
try:
    try:
        __import__("autojit_probe_missing_find_and_load")
    except ModuleNotFoundError:
        pass
finally:
    sys.meta_path.remove(finder)

assert getattr(
    bootstrap._find_and_load,
    "_cinderx_autojit_import_provider",
    None,
) == "find_and_load"
assert observed_depths, observed_depths
assert all(depth > 0 for depth in observed_depths), observed_depths
assert _cinderx._autojit_import_depth() == 0
)");
}

TEST_F(
    BehaviorClassifierRuntimeTest,
    Lib2to3SetupProviderWrapsMainAndTracksDepth) {
  runStockCode(R"(
import os
import sys
import types
os.environ["CINDERX_AUTOJIT_SETUP_PROVIDER"] = "lib2to3_main"

import cinderx
import _cinderx

observed_depths = []

def main():
    observed_depths.append(_cinderx._autojit_import_depth())
    return 42

module = types.ModuleType("lib2to3.main")
module.main = main
sys.modules["lib2to3.main"] = module

cinderx._maybe_install_autojit_setup_provider_for_module("lib2to3.main")

assert getattr(
    module.main,
    "_cinderx_autojit_setup_provider",
    None,
) == "lib2to3_main"
assert module.main() == 42
assert observed_depths and observed_depths[0] > 0
assert _cinderx._autojit_import_depth() == 0
)");
}

TEST_F(
    BehaviorClassifierRuntimeTest,
    CompileAfterNCallsApiDisablesClassificationAndSchedulesExistingFunctions) {
  ScopedAutoJitConfig config_guard;
  getMutableConfig().compile_after_n_calls.reset();
  getMutableConfig().auto_classify = true;

  runStockCode(R"(
import cinderx.jit as jit

def target(x):
    return x + 1

jit.compile_after_n_calls(2)
assert jit.get_compile_after_n_calls() == 2
assert not jit.is_jit_compiled(target)
target(1)
target(2)
assert not jit.is_jit_compiled(target)
target(3)
assert jit.is_jit_compiled(target)
)");

  EXPECT_FALSE(getConfig().auto_classify);
}
