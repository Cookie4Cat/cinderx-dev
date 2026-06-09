// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/behavior_classifier.h"

#include "cinderx/Common/code.h"
#include "cinderx/Common/extra-py-flags.h"
#include "cinderx/Common/py-portability.h"
#include "cinderx/Jit/bytecode.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/osr.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace jit {

namespace {

constexpr uint32_t kDimCountFloor = 2;
constexpr uint32_t kLowCutoffPct = 10;
constexpr uint32_t kMidCutoffPct = 25;
constexpr uint32_t kHighCutoffPct = 50;
constexpr uint8_t kMixedMinBucket = 2;
constexpr uint8_t kMixedBucketDelta = 1;
constexpr uint8_t kRiskSuspendBucket = 2;
constexpr uint8_t kRiskDynamicBucket = 2;
constexpr uint32_t kRiskExceptionFloor = 2;
constexpr uint32_t kRiskEffectiveInstructionFloor = 200;
constexpr uint32_t kLowRoiThresholdFactor = 2;
constexpr uint32_t kStartupDeferThresholdFactor = 1u << 20;
constexpr uint32_t kSteadyNonnumericWarmupThreshold = 1000;
constexpr uint8_t kWorkDimCount = static_cast<uint8_t>(WorkDim::kCount);

struct Signature {
  std::array<uint32_t, static_cast<size_t>(WorkDim::kCount)> counts{};
  uint32_t exception_control_count{0};
  uint32_t n_eff{0};
  std::array<std::pair<int, int>, CI_OSR_MAX_BACKEDGES> backedges{};
  size_t stored_backedge_count{0};
  uint32_t seen_backedge_count{0};
  uint8_t loop_score{0};
};

struct RankedDim {
  WorkDim dim;
  uint8_t bucket;
  uint32_t count;
};

uint8_t dimIndex(WorkDim dim) {
  return static_cast<uint8_t>(dim);
}

bool hasRequiredFlags(BorrowedRef<PyCodeObject> code) {
  constexpr int kRequiredCodeFlags = CO_OPTIMIZED | CO_NEWLOCALS;
  return (code->co_flags & kRequiredCodeFlags) == kRequiredCodeFlags;
}

bool nameEquals(BorrowedRef<PyObject> name, const char* expected) {
  if (name == nullptr || !PyUnicode_Check(name)) {
    return false;
  }
  const char* utf8 = PyUnicode_AsUTF8(name);
  if (utf8 == nullptr) {
    PyErr_Clear();
    return false;
  }
  return std::strcmp(utf8, expected) == 0;
}

std::string lowerAscii(std::string_view value) {
  std::string lowered;
  lowered.reserve(value.size());
  for (unsigned char ch : value) {
    lowered.push_back(static_cast<char>(std::tolower(ch)));
  }
  return lowered;
}

bool contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool isSyntheticFilename(BorrowedRef<PyCodeObject> code) {
  BorrowedRef<> filename{code->co_filename};
  if (filename == nullptr || !PyUnicode_Check(filename)) {
    return false;
  }
  const char* utf8 = PyUnicode_AsUTF8(filename);
  if (utf8 == nullptr) {
    PyErr_Clear();
    return false;
  }
  std::string_view filename_view{utf8};
  if (!filename_view.empty() && filename_view.front() == '<') {
    return true;
  }
  std::string lowered = lowerAscii(filename_view);
  return contains(lowered, "generated") ||
      contains(lowered, "/_generated") || contains(lowered, "/genshi/") ||
      contains(lowered, "/mako/") || contains(lowered, "/jinja") ||
      contains(lowered, "/django/template/");
}

uint8_t bucketDim(uint32_t count, uint32_t n_eff) {
  if (n_eff == 0 || count < kDimCountFloor) {
    return 0;
  }
  uint64_t scaled_count = static_cast<uint64_t>(count) * 100;
  if (scaled_count >= static_cast<uint64_t>(kHighCutoffPct) * n_eff) {
    return 3;
  }
  if (scaled_count >= static_cast<uint64_t>(kMidCutoffPct) * n_eff) {
    return 2;
  }
  if (scaled_count >= static_cast<uint64_t>(kLowCutoffPct) * n_eff) {
    return 1;
  }
  return 0;
}

std::array<uint8_t, static_cast<size_t>(WorkDim::kCount)> bucketize(
    const Signature& sig) {
  std::array<uint8_t, static_cast<size_t>(WorkDim::kCount)> buckets{};
  for (size_t i = 0; i < buckets.size(); ++i) {
    buckets[i] = bucketDim(sig.counts[i], sig.n_eff);
  }
  return buckets;
}

bool allBucketsZero(
    const std::array<uint8_t, static_cast<size_t>(WorkDim::kCount)>& buckets) {
  return std::all_of(buckets.begin(), buckets.end(), [](uint8_t bucket) {
    return bucket == 0;
  });
}

uint8_t activeDimMask(
    const std::array<uint8_t, static_cast<size_t>(WorkDim::kCount)>& buckets) {
  uint8_t mask = 0;
  for (WorkDim dim :
       {WorkDim::Compute,
        WorkDim::Control,
        WorkDim::Object,
        WorkDim::Dispatch,
        WorkDim::Dynamic}) {
    if (buckets[dimIndex(dim)] > 0) {
      mask |= activeDimMaskFor(dim);
    }
  }
  return mask;
}

uint8_t tieRank(WorkDim dim) {
  switch (dim) {
    case WorkDim::Compute:
      return 0;
    case WorkDim::Dispatch:
      return 1;
    case WorkDim::Object:
      return 2;
    case WorkDim::Control:
      return 3;
    case WorkDim::Dynamic:
      return 4;
    case WorkDim::Suspend:
      return 5;
    case WorkDim::kCount:
      break;
  }
  return 255;
}

std::array<RankedDim, static_cast<size_t>(WorkDim::kCount)> rankDims(
    const Signature& sig,
    const std::array<uint8_t, static_cast<size_t>(WorkDim::kCount)>& buckets) {
  std::array<RankedDim, static_cast<size_t>(WorkDim::kCount)> ranked{{
      {WorkDim::Compute, buckets[dimIndex(WorkDim::Compute)],
       sig.counts[dimIndex(WorkDim::Compute)]},
      {WorkDim::Control, buckets[dimIndex(WorkDim::Control)],
       sig.counts[dimIndex(WorkDim::Control)]},
      {WorkDim::Object, buckets[dimIndex(WorkDim::Object)],
       sig.counts[dimIndex(WorkDim::Object)]},
      {WorkDim::Dispatch, buckets[dimIndex(WorkDim::Dispatch)],
       sig.counts[dimIndex(WorkDim::Dispatch)]},
      {WorkDim::Suspend, buckets[dimIndex(WorkDim::Suspend)],
       sig.counts[dimIndex(WorkDim::Suspend)]},
      {WorkDim::Dynamic, buckets[dimIndex(WorkDim::Dynamic)],
       sig.counts[dimIndex(WorkDim::Dynamic)]},
  }};

  std::sort(
      ranked.begin(),
      ranked.end(),
      [](const RankedDim& a, const RankedDim& b) {
        if (a.bucket != b.bucket) {
          return a.bucket > b.bucket;
        }
        if (a.count != b.count) {
          return a.count > b.count;
        }
        return tieRank(a.dim) < tieRank(b.dim);
      });
  return ranked;
}

Family familyForFirstDim(WorkDim dim) {
  switch (dim) {
    case WorkDim::Compute:
      return Family::NumericLoop;
    case WorkDim::Control:
      return Family::BranchFSM;
    case WorkDim::Object:
      return Family::ObjectManipulator;
    case WorkDim::Dispatch:
      return Family::CallDispatcher;
    case WorkDim::Suspend:
      return Family::AsyncStateMachine;
    case WorkDim::Dynamic:
      return Family::ReflectionMeta;
    case WorkDim::kCount:
      break;
  }
  return Family::Trivial;
}

uint8_t codeSizeBucket(uint32_t n_eff) {
  if (n_eff >= 500) {
    return 3;
  }
  if (n_eff >= 100) {
    return 2;
  }
  if (n_eff >= 50) {
    return 1;
  }
  return 0;
}

uint8_t deriveRiskReason(
    const Signature& sig,
    const std::array<uint8_t, static_cast<size_t>(WorkDim::kCount)>& buckets) {
  uint8_t reason = kRiskNone;
  if (buckets[dimIndex(WorkDim::Suspend)] >= kRiskSuspendBucket) {
    reason |= kRiskSuspend;
  }
  if (buckets[dimIndex(WorkDim::Dynamic)] >= kRiskDynamicBucket) {
    reason |= kRiskDynamic;
  }
  if (sig.exception_control_count >= kRiskExceptionFloor) {
    reason |= kRiskException;
  }
  if (sig.n_eff >= kRiskEffectiveInstructionFloor) {
    reason |= kRiskHugeCode;
  }
  return reason;
}

uint8_t loopScore(
    const std::array<std::pair<int, int>, CI_OSR_MAX_BACKEDGES>& edges,
    size_t stored_edge_count,
    uint32_t seen_edge_count) {
  if (seen_edge_count == 0) {
    return 0;
  }
  if (seen_edge_count >= CI_OSR_MAX_BACKEDGES) {
    return 3;
  }

  std::array<std::pair<int, int>, CI_OSR_MAX_BACKEDGES * 2> events{};
  size_t event_count = 0;
  for (size_t i = 0; i < stored_edge_count; ++i) {
    auto [src, tgt] = edges[i];
    events[event_count++] = {tgt, 1};
    events[event_count++] = {src + 1, -1};
  }
  std::sort(events.begin(), events.begin() + event_count);

  int cur = 0;
  int depth = 0;
  for (size_t i = 0; i < event_count; ++i) {
    cur += events[i].second;
    depth = std::max(depth, cur);
  }

  uint8_t nesting_score = static_cast<uint8_t>(std::min(depth, 3));
  uint8_t count_score = seen_edge_count >= 4
      ? 3
      : (seen_edge_count >= 2 ? 2 : 1);
  return std::min<uint8_t>(3, std::max(nesting_score, count_score));
}

std::optional<Signature> scanCode(BorrowedRef<PyCodeObject> code) {
  Signature sig;
  BytecodeInstructionBlock block{code};
  for (auto it = block.begin(); it != block.end(); ++it) {
    BytecodeInstruction instr = *it;
    int opcode = instr.opcode();
    OpcodeClass cls = opcodeClassOf(opcode);
    if (cls == OpcodeClass::Invalid) {
      return std::nullopt;
    }
    if (cls == OpcodeClass::Ignored) {
      continue;
    }

    sig.n_eff++;
    if (isWorkDim(cls)) {
      sig.counts[dimIndex(toWorkDim(cls))]++;
    }
    if (isExceptionControlOpcode(opcode)) {
      sig.exception_control_count++;
    }

    if (instr.isBranch()) {
      BCIndex src = instr.opcodeIndex();
      BCIndex tgt = instr.getJumpTarget().asIndex();
      if (tgt < src) {
        if (sig.stored_backedge_count < sig.backedges.size()) {
          sig.backedges[sig.stored_backedge_count++] = {
              src.value(), tgt.value()};
        }
        sig.seen_backedge_count++;
      }
    }
  }

  sig.loop_score = loopScore(
      sig.backedges, sig.stored_backedge_count, sig.seen_backedge_count);
  return sig;
}

uint32_t saturatingMul(uint32_t value, uint32_t factor) {
  uint64_t product = static_cast<uint64_t>(value) * factor;
  return product > std::numeric_limits<uint32_t>::max()
      ? std::numeric_limits<uint32_t>::max()
      : static_cast<uint32_t>(product);
}

std::optional<std::pair<WorkDim, WorkDim>> decodeMixedShape(MixedShape shape) {
  if (shape == kMixedShapeNone) {
    return std::nullopt;
  }
  MixedShape current = 1;
  for (uint8_t i = 0; i < kWorkDimCount; ++i) {
    for (uint8_t j = i + 1; j < kWorkDimCount; ++j) {
      if (current == shape) {
        return std::make_pair(static_cast<WorkDim>(i), static_cast<WorkDim>(j));
      }
      current++;
    }
  }
  return std::nullopt;
}

bool dimInSet(WorkDim dim, std::initializer_list<WorkDim> dims) {
  return std::find(dims.begin(), dims.end(), dim) != dims.end();
}

bool mixedShapeAllIn(MixedShape shape, std::initializer_list<WorkDim> dims) {
  auto decoded = decodeMixedShape(shape);
  if (!decoded.has_value()) {
    return false;
  }
  return dimInSet(decoded->first, dims) && dimInSet(decoded->second, dims);
}

bool mixedShapeContains(MixedShape shape, WorkDim dim) {
  auto decoded = decodeMixedShape(shape);
  if (!decoded.has_value()) {
    return false;
  }
  return decoded->first == dim || decoded->second == dim;
}

bool isPureControlExceptionRisk(const StructureKey& key) {
  return key.risk_reason == kRiskException &&
      key.active_dim_mask == activeDimMaskFor(WorkDim::Control);
}

} // namespace

WorkDim toWorkDim(OpcodeClass cls) {
  switch (cls) {
    case OpcodeClass::Compute:
      return WorkDim::Compute;
    case OpcodeClass::Control:
      return WorkDim::Control;
    case OpcodeClass::Object:
      return WorkDim::Object;
    case OpcodeClass::Dispatch:
      return WorkDim::Dispatch;
    case OpcodeClass::Suspend:
      return WorkDim::Suspend;
    case OpcodeClass::Dynamic:
      return WorkDim::Dynamic;
    case OpcodeClass::Neutral:
    case OpcodeClass::Ignored:
    case OpcodeClass::Invalid:
      break;
  }
  return WorkDim::Compute;
}

uint8_t activeDimMaskFor(WorkDim dim) {
  switch (dim) {
    case WorkDim::Compute:
      return 1u << 0;
    case WorkDim::Control:
      return 1u << 1;
    case WorkDim::Object:
      return 1u << 2;
    case WorkDim::Dispatch:
      return 1u << 3;
    case WorkDim::Dynamic:
      return 1u << 4;
    case WorkDim::Suspend:
    case WorkDim::kCount:
      break;
  }
  return 0;
}

bool StructureKey::hasActiveDim(WorkDim dim) const {
  return (active_dim_mask & activeDimMaskFor(dim)) != 0;
}

bool StructureKey::computeHint() const {
  return hasActiveDim(WorkDim::Compute);
}

bool StructureKey::computeDominantHint() const {
  return family == Family::NumericLoop ||
      (family == Family::Mixed &&
       mixedShapeContains(mixed_shape, WorkDim::Compute));
}

uint8_t StructureKey::activeDimCount() const {
  uint8_t count = 0;
  uint8_t mask = active_dim_mask;
  while (mask != 0) {
    count += mask & 1u;
    mask >>= 1;
  }
  return count;
}

uint32_t StructureKey::pack() const {
  uint32_t payload = 0;
  payload |= (static_cast<uint32_t>(mixed_shape) & 0xFu) << 20;
  payload |= (static_cast<uint32_t>(family) & 0xFu) << 16;
  payload |= (static_cast<uint32_t>(loop_score) & 0x3u) << 14;
  payload |= (is_suspendable ? 1u : 0u) << 13;
  payload |= (is_static ? 1u : 0u) << 12;
  payload |= (is_synthetic ? 1u : 0u) << 11;
  payload |= (static_cast<uint32_t>(risk_reason) & 0xFu) << 7;
  payload |= (static_cast<uint32_t>(code_size_bucket) & 0x3u) << 5;
  payload |= static_cast<uint32_t>(active_dim_mask) & 0x1Fu;
  return payload & kSkeyPayloadMask;
}

StructureKey StructureKey::unpack(uint32_t payload) {
  payload &= kSkeyPayloadMask;
  StructureKey key;
  key.mixed_shape = static_cast<MixedShape>((payload >> 20) & 0xFu);
  key.family = static_cast<Family>((payload >> 16) & 0xFu);
  key.loop_score = static_cast<uint8_t>((payload >> 14) & 0x3u);
  key.is_suspendable = ((payload >> 13) & 0x1u) != 0;
  key.is_static = ((payload >> 12) & 0x1u) != 0;
  key.is_synthetic = ((payload >> 11) & 0x1u) != 0;
  key.risk_reason = static_cast<uint8_t>((payload >> 7) & 0xFu);
  key.code_size_bucket = static_cast<uint8_t>((payload >> 5) & 0x3u);
  key.active_dim_mask = static_cast<uint8_t>(payload & 0x1Fu);
  if (key.family != Family::Mixed) {
    key.mixed_shape = kMixedShapeNone;
  }
  return key;
}

MixedShape encodeMixedShape(WorkDim a, WorkDim b) {
  uint8_t ai = dimIndex(a);
  uint8_t bi = dimIndex(b);
  if (ai >= kWorkDimCount || bi >= kWorkDimCount || ai == bi) {
    return kMixedShapeNone;
  }
  if (ai > bi) {
    std::swap(ai, bi);
  }

  MixedShape shape = 1;
  for (uint8_t i = 0; i < kWorkDimCount; ++i) {
    for (uint8_t j = i + 1; j < kWorkDimCount; ++j) {
      if (i == ai && j == bi) {
        return shape;
      }
      shape++;
    }
  }
  return kMixedShapeNone;
}

OpcodeClass opcodeClassOf(int canonical_opcode) {
  switch (canonical_opcode) {
    case BINARY_OP:
    case BINARY_OP_ADD_FLOAT:
    case BINARY_OP_ADD_INT:
    case BINARY_OP_ADD_UNICODE:
    case BINARY_OP_EXTEND:
    case BINARY_OP_INPLACE_ADD_UNICODE:
    case BINARY_OP_MULTIPLY_FLOAT:
    case BINARY_OP_MULTIPLY_INT:
    case BINARY_OP_SUBTRACT_FLOAT:
    case BINARY_OP_SUBTRACT_INT:
    case CAST:
    case CAST_CACHED:
    case COMPARE_OP:
    case COMPARE_OP_FLOAT:
    case COMPARE_OP_INT:
    case COMPARE_OP_STR:
    case CONTAINS_OP:
    case CONTAINS_OP_DICT:
    case CONTAINS_OP_SET:
    case CONVERT_PRIMITIVE:
    case IS_OP:
    case LOAD_TYPE:
    case PRIMITIVE_BINARY_OP:
    case PRIMITIVE_BOX:
    case PRIMITIVE_COMPARE_OP:
    case PRIMITIVE_UNARY_OP:
    case PRIMITIVE_UNBOX:
    case REFINE_TYPE:
    case UNARY_INVERT:
    case UNARY_NEGATIVE:
    case UNARY_NOT:
      return OpcodeClass::Compute;

    case CHECK_EG_MATCH:
    case CHECK_EXC_MATCH:
    case CLEANUP_THROW:
    case END_FOR:
    case EXIT_INIT_CHECK:
    case FOR_ITER:
    case FOR_ITER_GEN:
    case FOR_ITER_LIST:
    case FOR_ITER_RANGE:
    case FOR_ITER_TUPLE:
    case INSTRUMENTED_END_FOR:
    case INSTRUMENTED_FOR_ITER:
    case INSTRUMENTED_JUMP_BACKWARD:
    case INSTRUMENTED_JUMP_FORWARD:
    case INSTRUMENTED_NOT_TAKEN:
    case INSTRUMENTED_POP_JUMP_IF_FALSE:
    case INSTRUMENTED_POP_JUMP_IF_NONE:
    case INSTRUMENTED_POP_JUMP_IF_NOT_NONE:
    case INSTRUMENTED_POP_JUMP_IF_TRUE:
    case INSTRUMENTED_RETURN_VALUE:
    case JUMP:
    case JUMP_BACKWARD:
    case JUMP_BACKWARD_JIT:
    case JUMP_BACKWARD_NO_INTERRUPT:
    case JUMP_BACKWARD_NO_JIT:
    case JUMP_FORWARD:
    case JUMP_IF_FALSE:
    case JUMP_IF_TRUE:
    case JUMP_NO_INTERRUPT:
    case NOT_TAKEN:
    case POP_BLOCK:
    case POP_EXCEPT:
    case POP_JUMP_IF_FALSE:
    case POP_JUMP_IF_NONE:
    case POP_JUMP_IF_NONZERO:
    case POP_JUMP_IF_NOT_NONE:
    case POP_JUMP_IF_TRUE:
    case POP_JUMP_IF_ZERO:
    case PUSH_EXC_INFO:
    case RAISE_VARARGS:
    case RERAISE:
    case RETURN_PRIMITIVE:
    case RETURN_VALUE:
    case SETUP_CLEANUP:
    case SETUP_FINALLY:
    case SETUP_WITH:
    case TO_BOOL:
    case TO_BOOL_ALWAYS_TRUE:
    case TO_BOOL_BOOL:
    case TO_BOOL_INT:
    case TO_BOOL_LIST:
    case TO_BOOL_NONE:
    case TO_BOOL_STR:
    case WITH_EXCEPT_START:
      return OpcodeClass::Control;

    case BINARY_OP_SUBSCR_DICT:
    case BINARY_OP_SUBSCR_GETITEM:
    case BINARY_OP_SUBSCR_LIST_INT:
    case BINARY_OP_SUBSCR_LIST_SLICE:
    case BINARY_OP_SUBSCR_STR_INT:
    case BINARY_OP_SUBSCR_TUPLE_INT:
    case BINARY_SLICE:
    case BUILD_CHECKED_LIST:
    case BUILD_CHECKED_LIST_CACHED:
    case BUILD_CHECKED_MAP:
    case BUILD_CHECKED_MAP_CACHED:
    case BUILD_LIST:
    case BUILD_MAP:
    case BUILD_SET:
    case BUILD_SLICE:
    case BUILD_TUPLE:
    case DELETE_ATTR:
    case DELETE_SUBSCR:
    case DICT_MERGE:
    case DICT_UPDATE:
    case FAST_LEN:
    case GET_ITER:
    case GET_LEN:
    case LIST_APPEND:
    case LIST_DEL:
    case LIST_EXTEND:
    case LOAD_ATTR:
    case LOAD_ATTR_CLASS:
    case LOAD_ATTR_CLASS_WITH_METACLASS_CHECK:
    case LOAD_ATTR_GETATTRIBUTE_OVERRIDDEN:
    case LOAD_ATTR_INSTANCE_VALUE:
    case LOAD_ATTR_METHOD_LAZY_DICT:
    case LOAD_ATTR_METHOD_NO_DICT:
    case LOAD_ATTR_METHOD_WITH_VALUES:
    case LOAD_ATTR_MODULE:
    case LOAD_ATTR_NONDESCRIPTOR_NO_DICT:
    case LOAD_ATTR_NONDESCRIPTOR_WITH_VALUES:
    case LOAD_ATTR_PROPERTY:
    case LOAD_ATTR_SLOT:
    case LOAD_ATTR_WITH_HINT:
    case LOAD_FIELD:
    case LOAD_ITERABLE_ARG:
    case LOAD_MAPPING_ARG:
    case LOAD_OBJ_FIELD:
    case LOAD_PRIMITIVE_FIELD:
    case MAP_ADD:
    case MATCH_CLASS:
    case MATCH_KEYS:
    case MATCH_MAPPING:
    case MATCH_SEQUENCE:
    case SEQUENCE_GET:
    case SEQUENCE_SET:
    case SET_ADD:
    case SET_UPDATE:
    case STORE_ATTR:
    case STORE_ATTR_INSTANCE_VALUE:
    case STORE_ATTR_SLOT:
    case STORE_ATTR_WITH_HINT:
    case STORE_FIELD:
    case STORE_OBJ_FIELD:
    case STORE_PRIMITIVE_FIELD:
    case STORE_SLICE:
    case STORE_SUBSCR:
    case STORE_SUBSCR_DICT:
    case STORE_SUBSCR_LIST_INT:
    case TP_ALLOC:
    case TP_ALLOC_CACHED:
    case UNPACK_EX:
    case UNPACK_SEQUENCE:
    case UNPACK_SEQUENCE_LIST:
    case UNPACK_SEQUENCE_TUPLE:
    case UNPACK_SEQUENCE_TWO_TUPLE:
      return OpcodeClass::Object;

    case CALL:
    case CALL_ALLOC_AND_ENTER_INIT:
    case CALL_BOUND_METHOD_EXACT_ARGS:
    case CALL_BOUND_METHOD_GENERAL:
    case CALL_BUILTIN_CLASS:
    case CALL_BUILTIN_FAST:
    case CALL_BUILTIN_FAST_WITH_KEYWORDS:
    case CALL_BUILTIN_O:
    case CALL_FUNCTION_EX:
    case CALL_INTRINSIC_1:
    case CALL_INTRINSIC_2:
    case CALL_ISINSTANCE:
    case CALL_KW:
    case CALL_KW_BOUND_METHOD:
    case CALL_KW_NON_PY:
    case CALL_KW_PY:
    case CALL_LEN:
    case CALL_LIST_APPEND:
    case CALL_METHOD_DESCRIPTOR_FAST:
    case CALL_METHOD_DESCRIPTOR_FAST_WITH_KEYWORDS:
    case CALL_METHOD_DESCRIPTOR_NOARGS:
    case CALL_METHOD_DESCRIPTOR_O:
    case CALL_NON_PY_GENERAL:
    case CALL_PY_EXACT_ARGS:
    case CALL_PY_GENERAL:
    case CALL_STR_1:
    case CALL_TUPLE_1:
    case CALL_TYPE_1:
    case INSTRUMENTED_CALL:
    case INSTRUMENTED_CALL_FUNCTION_EX:
    case INSTRUMENTED_CALL_KW:
    case INSTRUMENTED_LOAD_SUPER_ATTR:
    case INVOKE_FUNCTION:
    case INVOKE_FUNCTION_CACHED:
    case INVOKE_INDIRECT_CACHED:
    case INVOKE_METHOD:
    case INVOKE_NATIVE:
    case LOAD_METHOD_STATIC:
    case LOAD_METHOD_STATIC_CACHED:
    case LOAD_SPECIAL:
    case LOAD_SUPER_ATTR:
    case LOAD_SUPER_ATTR_ATTR:
    case LOAD_SUPER_ATTR_METHOD:
    case PUSH_NULL:
      return OpcodeClass::Dispatch;

    case END_ASYNC_FOR:
    case END_SEND:
    case GET_AITER:
    case GET_ANEXT:
    case GET_AWAITABLE:
    case GET_YIELD_FROM_ITER:
    case INSTRUMENTED_END_ASYNC_FOR:
    case INSTRUMENTED_END_SEND:
    case INSTRUMENTED_YIELD_VALUE:
    case RETURN_GENERATOR:
    case SEND:
    case SEND_GEN:
    case YIELD_VALUE:
      return OpcodeClass::Suspend;

    case ANNOTATIONS_PLACEHOLDER:
    case BUILD_INTERPOLATION:
    case BUILD_STRING:
    case BUILD_TEMPLATE:
    case CONVERT_VALUE:
    case COPY_FREE_VARS:
    case DELETE_DEREF:
    case DELETE_GLOBAL:
    case DELETE_NAME:
    case EAGER_IMPORT_NAME:
    case FORMAT_SIMPLE:
    case FORMAT_WITH_SPEC:
    case IMPORT_FROM:
    case IMPORT_NAME:
    case LOAD_BUILD_CLASS:
    case LOAD_CLASS:
    case LOAD_CLOSURE:
    case LOAD_DEREF:
    case LOAD_FROM_DICT_OR_DEREF:
    case LOAD_FROM_DICT_OR_GLOBALS:
    case LOAD_GLOBAL:
    case LOAD_GLOBAL_BUILTIN:
    case LOAD_GLOBAL_MODULE:
    case LOAD_LOCALS:
    case LOAD_NAME:
    case MAKE_CELL:
    case MAKE_FUNCTION:
    case SETUP_ANNOTATIONS:
    case SET_FUNCTION_ATTRIBUTE:
    case STORE_DEREF:
    case STORE_GLOBAL:
    case STORE_NAME:
      return OpcodeClass::Dynamic;

    case COPY:
    case DELETE_FAST:
    case INSTRUMENTED_POP_ITER:
    case INTERPRETER_EXIT:
    case LOAD_COMMON_CONSTANT:
    case LOAD_CONST:
    case LOAD_CONST_IMMORTAL:
    case LOAD_CONST_MORTAL:
    case LOAD_FAST:
    case LOAD_FAST_AND_CLEAR:
    case LOAD_FAST_BORROW:
    case LOAD_FAST_BORROW_LOAD_FAST_BORROW:
    case LOAD_FAST_CHECK:
    case LOAD_FAST_LOAD_FAST:
    case LOAD_LOCAL:
    case LOAD_SMALL_INT:
    case POP_ITER:
    case POP_TOP:
    case PRIMITIVE_LOAD_CONST:
    case STORE_FAST:
    case STORE_FAST_LOAD_FAST:
    case STORE_FAST_MAYBE_NULL:
    case STORE_FAST_STORE_FAST:
    case STORE_LOCAL:
    case STORE_LOCAL_CACHED:
    case SWAP:
      return OpcodeClass::Neutral;

    case CACHE:
    case ENTER_EXECUTOR:
    case EXTENDED_ARG:
    case EXTENDED_OPCODE:
    case INSTRUMENTED_INSTRUCTION:
    case INSTRUMENTED_LINE:
    case INSTRUMENTED_RESUME:
    case NOP:
    case RESERVED:
    case RESUME:
    case RESUME_CHECK:
      return OpcodeClass::Ignored;
  }
  return OpcodeClass::Invalid;
}

bool isExceptionControlOpcode(int canonical_opcode) {
  switch (canonical_opcode) {
    case CHECK_EG_MATCH:
    case CHECK_EXC_MATCH:
    case CLEANUP_THROW:
    case POP_EXCEPT:
    case PUSH_EXC_INFO:
    case RERAISE:
    case WITH_EXCEPT_START:
      return true;
    default:
      return false;
  }
}

bool isAutoJitClassifiable(BorrowedRef<PyCodeObject> code) {
  if (code == nullptr) {
    return false;
  }
  if (!hasRequiredFlags(code)) {
    return false;
  }
  if (nameEquals(code->co_name, "<module>")) {
    return false;
  }
  if (code->co_flags & CO_ASYNC_GENERATOR) {
    return false;
  }
  if (code->co_flags & CI_CO_SUPPRESS_JIT) {
    return false;
  }
  return true;
}

bool shouldDeferSuspendableAutoJitWithoutStructureKey(
    BorrowedRef<PyCodeObject> code,
    const GateContext& context) {
  if (!getConfig().enable_startup_init_policy || !context.startup_phase) {
    return false;
  }
  if (!isAutoJitClassifiable(code)) {
    return false;
  }
  if (code->co_flags & CI_CO_STATICALLY_COMPILED) {
    return false;
  }
  return (code->co_flags & (CO_GENERATOR | CO_COROUTINE | CO_ASYNC_GENERATOR)) !=
      0;
}

std::optional<StructureKey> deriveStructureKey(
    BorrowedRef<PyCodeObject> code) {
  if (!isAutoJitClassifiable(code)) {
    return std::nullopt;
  }

  auto sig = scanCode(code);
  if (!sig.has_value()) {
    return std::nullopt;
  }

  auto buckets = bucketize(*sig);
  StructureKey key;
  key.loop_score = sig->loop_score;
  key.is_static = (code->co_flags & CI_CO_STATICALLY_COMPILED) != 0;
  key.is_suspendable =
      (code->co_flags & (CO_GENERATOR | CO_COROUTINE | CO_ASYNC_GENERATOR)) !=
          0 ||
      sig->counts[dimIndex(WorkDim::Suspend)] > 0;
  key.is_synthetic = isSyntheticFilename(code);
  key.risk_reason = deriveRiskReason(*sig, buckets);
  key.code_size_bucket = codeSizeBucket(sig->n_eff);
  key.active_dim_mask = activeDimMask(buckets);

  if (allBucketsZero(buckets)) {
    key.family = Family::Trivial;
    key.mixed_shape = kMixedShapeNone;
    return key;
  }

  auto ranked = rankDims(*sig, buckets);
  const RankedDim& first = ranked[0];
  const RankedDim& second = ranked[1];
  if (first.bucket >= kMixedMinBucket &&
      second.bucket >= kMixedMinBucket &&
      first.bucket - second.bucket <= kMixedBucketDelta) {
    key.family = Family::Mixed;
    key.mixed_shape = encodeMixedShape(first.dim, second.dim);
    return key;
  }

  key.family = familyForFirstDim(first.dim);
  key.mixed_shape = kMixedShapeNone;
  return key;
}

std::optional<StructureKey> getOrComputeStructureKey(
    BorrowedRef<PyCodeObject> code,
    CodeExtra* extra) {
  if (extra == nullptr) {
    return std::nullopt;
  }
  uint32_t word = Ci_code_extra_load_skey_acquire(extra);
  if (word & kSkeyValidBit) {
    return StructureKey::unpack(word & kSkeyPayloadMask);
  }

  auto key = deriveStructureKey(code);
  if (!key.has_value()) {
    return std::nullopt;
  }
  Ci_code_extra_store_skey_release(extra, key->pack() | kSkeyValidBit);
  return key;
}

ThresholdDecision computeThreshold(
    const StructureKey& key,
    const GateContext& context,
    uint32_t global) {
  bool startup_like_family = key.family == Family::CallDispatcher ||
      key.family == Family::ReflectionMeta ||
      key.family == Family::ObjectManipulator ||
      key.family == Family::BranchFSM;
  bool startup_like_mixed = key.family == Family::Mixed &&
      mixedShapeAllIn(
          key.mixed_shape,
          {WorkDim::Dynamic,
           WorkDim::Dispatch,
           WorkDim::Object,
           WorkDim::Control});
  bool high_cost_nonnumeric_import_candidate =
      getConfig().enable_startup_init_policy && context.startup_phase &&
      !key.is_static && key.family != Family::NumericLoop &&
      !key.computeDominantHint() &&
      (key.highRisk() || key.code_size_bucket > 0 ||
       key.family == Family::CallDispatcher ||
       key.family == Family::ReflectionMeta || key.family == Family::BranchFSM ||
       startup_like_mixed);
  bool startup_low_roi_nonnumeric_candidate =
      getConfig().enable_startup_init_policy && context.startup_phase &&
      !key.is_static &&
      (key.family == Family::Trivial ||
       (key.family == Family::ObjectManipulator && key.loop_score <= 1 &&
        !key.computeHint()));
  if (high_cost_nonnumeric_import_candidate ||
      startup_low_roi_nonnumeric_candidate) {
    return {
        saturatingMul(global, kStartupDeferThresholdFactor),
        key.highRisk() ? BranchReason::RiskDefer : BranchReason::StartupInit};
  }

  bool steady_exception_high_cost_framework_candidate =
      !context.startup_phase && !key.is_static && !key.is_suspendable &&
      !key.computeHint() && key.code_size_bucket >= 2 &&
      (key.risk_reason & kRiskException) != 0 &&
      (startup_like_family || startup_like_mixed);
  if (steady_exception_high_cost_framework_candidate) {
    return {
        saturatingMul(global, kStartupDeferThresholdFactor),
        BranchReason::RiskDefer};
  }

  bool expected_exception_loop_candidate = !context.startup_phase &&
      !key.is_static && !key.is_suspendable &&
      key.family == Family::BranchFSM && key.loop_score >= 2 &&
      key.code_size_bucket == 1 && isPureControlExceptionRisk(key);
  if (expected_exception_loop_candidate) {
    return {
        saturatingMul(global, kStartupDeferThresholdFactor),
        BranchReason::RiskDefer};
  }

  bool steady_nonnumeric_warmup_candidate = !key.is_static &&
      key.loop_score == 0 &&
      (key.is_suspendable || startup_like_family || startup_like_mixed);
  if (steady_nonnumeric_warmup_candidate) {
    if (key.highRisk() || key.code_size_bucket > 0) {
      return {
          saturatingMul(global, kStartupDeferThresholdFactor),
          key.highRisk() ? BranchReason::RiskDefer : BranchReason::LowRoi};
    }
    if (key.family == Family::ObjectManipulator) {
      return {global, BranchReason::None};
    }
    return {std::max(global, kSteadyNonnumericWarmupThreshold),
            BranchReason::LowRoi};
  }

  bool large_branch_warmup_candidate = !key.is_static &&
      key.loop_score > 0 && key.family == Family::BranchFSM &&
      (key.highRisk() || key.code_size_bucket >= 2);
  if (large_branch_warmup_candidate) {
    return {std::max(global, kSteadyNonnumericWarmupThreshold),
            BranchReason::LowRoi};
  }

  bool low_roi_base = key.loop_score == 0 && !key.is_static &&
      !key.is_suspendable && !key.highRisk();
  bool trivial_low_roi_candidate =
      low_roi_base && key.family == Family::Trivial;
  bool synthetic_low_roi_candidate = key.is_synthetic && low_roi_base &&
      (key.family == Family::ReflectionMeta || key.family == Family::Trivial);
  if (trivial_low_roi_candidate || synthetic_low_roi_candidate) {
    return {
        saturatingMul(global, kLowRoiThresholdFactor), BranchReason::LowRoi};
  }
  return {global, BranchReason::None};
}

} // namespace jit
