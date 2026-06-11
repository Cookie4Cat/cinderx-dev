// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/code_extra.h"
#include "cinderx/Common/ref.h"

#include <cstdint>
#include <optional>

namespace jit {

enum class Family : uint8_t {
  NumericLoop = 0,
  BranchFSM,
  ObjectManipulator,
  CallDispatcher,
  AsyncStateMachine,
  ReflectionMeta,
  Trivial,
  Mixed,
  kCount,
};

enum class WorkDim : uint8_t {
  Compute = 0,
  Control,
  Object,
  Dispatch,
  Suspend,
  Dynamic,
  kCount,
};

enum class OpcodeClass : uint8_t {
  Compute = 0,
  Control,
  Object,
  Dispatch,
  Suspend,
  Dynamic,
  Neutral,
  Ignored,
  Invalid,
};

constexpr bool isWorkDim(OpcodeClass cls) {
  return static_cast<uint8_t>(cls) <
      static_cast<uint8_t>(OpcodeClass::Neutral);
}

WorkDim toWorkDim(OpcodeClass cls);

using MixedShape = uint8_t;
constexpr MixedShape kMixedShapeNone = 0;

uint8_t activeDimMaskFor(WorkDim dim);

enum RiskReason : uint8_t {
  kRiskNone = 0,
  kRiskSuspend = 1u << 0,
  kRiskDynamic = 1u << 1,
  kRiskException = 1u << 2,
  kRiskHugeCode = 1u << 3,
};

struct StructureKey {
  Family family{Family::Trivial};
  MixedShape mixed_shape{kMixedShapeNone};
  uint8_t loop_score{0};
  bool is_suspendable{false};
  bool is_static{false};
  bool is_synthetic{false};
  uint8_t risk_reason{kRiskNone};
  uint8_t code_size_bucket{0};
  uint8_t active_dim_mask{0};

  bool highRisk() const {
    return risk_reason != kRiskNone;
  }

  bool hasActiveDim(WorkDim dim) const;
  bool computeHint() const;
  bool computeDominantHint() const;
  uint8_t activeDimCount() const;

  uint32_t pack() const;
  static StructureKey unpack(uint32_t payload);
};

constexpr uint32_t kSkeyValidBit = CI_CODE_EXTRA_SKEY_VALID_BIT;
constexpr uint32_t kSkeyDecidedColdBit =
    CI_CODE_EXTRA_SKEY_DECIDED_COLD_BIT;
constexpr uint32_t kSkeyPayloadMask = CI_CODE_EXTRA_SKEY_PAYLOAD_MASK;

struct GateContext {
  bool startup_phase{false};
  bool import_phase{false};
  bool setup_phase{false};
};

enum class BranchReason : uint8_t {
  None,
  LowRoi,
  StartupInit,
  RiskDefer,
  RoiBackoff,
  FallbackInvalid,
};

struct ThresholdDecision {
  uint32_t limit{0};
  BranchReason branch_reason{BranchReason::None};
};

MixedShape encodeMixedShape(WorkDim a, WorkDim b);
OpcodeClass opcodeClassOf(int canonical_opcode);
bool isExceptionControlOpcode(int canonical_opcode);

bool isAutoJitClassifiable(BorrowedRef<PyCodeObject> code);
bool shouldDeferSuspendableAutoJitWithoutStructureKey(
    BorrowedRef<PyCodeObject> code,
    const GateContext& context);
std::optional<StructureKey> deriveStructureKey(
    BorrowedRef<PyCodeObject> code);
std::optional<StructureKey> getOrComputeStructureKey(
    BorrowedRef<PyCodeObject> code,
    CodeExtra* extra);
ThresholdDecision computeThreshold(
    const StructureKey& key,
    const GateContext& context,
    uint32_t global);
ThresholdDecision computeThresholdForCode(
    BorrowedRef<PyCodeObject> code,
    const StructureKey& key,
    const GateContext& context,
    uint32_t global);

} // namespace jit
