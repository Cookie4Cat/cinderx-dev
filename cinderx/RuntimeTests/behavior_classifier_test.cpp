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

TEST(BehaviorClassifierTest, ComputeThresholdDefersOnlyExplicitCandidates) {
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
  EXPECT_EQ(suspendable_trivial_decision.limit, 2);
  EXPECT_EQ(suspendable_trivial_decision.branch_reason, BranchReason::None);

  StructureKey risky_trivial{Family::Trivial};
  risky_trivial.risk_reason = kRiskHugeCode;
  auto risky_trivial_decision = computeThreshold(risky_trivial, ctx, 2);
  EXPECT_EQ(risky_trivial_decision.limit, 2);
  EXPECT_EQ(risky_trivial_decision.branch_reason, BranchReason::None);

  StructureKey risk_dispatch{Family::CallDispatcher};
  risk_dispatch.risk_reason = kRiskDynamic;
  auto risk_decision = computeThreshold(risk_dispatch, ctx, 2);
  EXPECT_EQ(risk_decision.limit, 2);
  EXPECT_EQ(risk_decision.branch_reason, BranchReason::None);

  StructureKey suspendable{Family::AsyncStateMachine};
  suspendable.is_suspendable = true;
  suspendable.risk_reason = kRiskSuspend;
  auto suspendable_decision = computeThreshold(suspendable, ctx, 2);
  EXPECT_EQ(suspendable_decision.limit, 2);
  EXPECT_EQ(suspendable_decision.branch_reason, BranchReason::None);

  StructureKey huge_code{Family::ObjectManipulator};
  huge_code.risk_reason = kRiskHugeCode;
  huge_code.code_size_bucket = 3;
  auto huge_code_decision = computeThreshold(huge_code, ctx, 2);
  EXPECT_EQ(huge_code_decision.limit, 2);
  EXPECT_EQ(huge_code_decision.branch_reason, BranchReason::None);

  StructureKey hot_loop{Family::NumericLoop};
  hot_loop.loop_score = 2;
  hot_loop.risk_reason = kRiskHugeCode;
  auto loop_decision = computeThreshold(hot_loop, ctx, 2);
  EXPECT_EQ(loop_decision.limit, 2);
  EXPECT_EQ(loop_decision.branch_reason, BranchReason::None);
}

TEST(BehaviorClassifierTest, StartupInitPolicyOnlyDefersStartupLikeWork) {
  ScopedAutoJitConfig config_guard;
  getMutableConfig().enable_startup_init_policy = true;

  GateContext startup{true};
  StructureKey dispatcher{Family::CallDispatcher};
  auto startup_decision = computeThreshold(dispatcher, startup, 2);
  EXPECT_EQ(startup_decision.limit, 16);
  EXPECT_EQ(startup_decision.branch_reason, BranchReason::StartupInit);

  GateContext steady_state{false};
  auto steady_state_decision = computeThreshold(dispatcher, steady_state, 2);
  EXPECT_EQ(steady_state_decision.limit, 2);
  EXPECT_EQ(steady_state_decision.branch_reason, BranchReason::None);

  StructureKey loop{Family::NumericLoop};
  auto loop_decision = computeThreshold(loop, startup, 2);
  EXPECT_EQ(loop_decision.limit, 2);
  EXPECT_EQ(loop_decision.branch_reason, BranchReason::None);

  StructureKey suspendable{Family::AsyncStateMachine};
  suspendable.is_suspendable = true;
  suspendable.risk_reason = kRiskSuspend;
  auto suspendable_decision = computeThreshold(suspendable, startup, 2);
  EXPECT_EQ(suspendable_decision.limit, 2);
  EXPECT_EQ(suspendable_decision.branch_reason, BranchReason::None);

  StructureKey static_dispatcher{Family::CallDispatcher};
  static_dispatcher.is_static = true;
  auto static_decision = computeThreshold(static_dispatcher, startup, 2);
  EXPECT_EQ(static_decision.limit, 2);
  EXPECT_EQ(static_decision.branch_reason, BranchReason::None);
}

class BehaviorClassifierRuntimeTest : public RuntimeTest {};

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

for value in range(5, 17):
    thin(value)
    assert jit.is_jit_compiled(thin)

assert jit.is_jit_compiled(thin)
)");
}

TEST_F(BehaviorClassifierRuntimeTest, AutoClassifyDoesNotDeferGenerators) {
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
gen()
assert jit.is_jit_compiled(gen)
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
