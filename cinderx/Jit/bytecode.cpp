// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/bytecode.h"

#include "cinderx/Common/code.h"
#include "cinderx/Common/code_extra.h"
#include "cinderx/Jit/config.h"

extern "C" {
#include "internal/pycore_code.h"
}

#include <cstddef>

namespace jit {

namespace {

constexpr int kInlineCacheStartInstructionOffset = 1;

constexpr int attrCacheInstructionOffset(std::size_t field_offset) {
  return kInlineCacheStartInstructionOffset +
      field_offset / sizeof(_Py_CODEUNIT);
}

static_assert(
    offsetof(_PyAttrCache, version) % sizeof(_Py_CODEUNIT) == 0,
    "_PyAttrCache::version must be code-unit aligned");
static_assert(
    offsetof(_PyAttrCache, index) % sizeof(_Py_CODEUNIT) == 0,
    "_PyAttrCache::index must be code-unit aligned");

constexpr int kAttrCacheTypeVersionInstructionOffset =
    attrCacheInstructionOffset(offsetof(_PyAttrCache, version));
constexpr int kAttrCacheIndexInstructionOffset =
    attrCacheInstructionOffset(offsetof(_PyAttrCache, index));

static_assert(
    kAttrCacheTypeVersionInstructionOffset == 2,
    "_PyAttrCache::version moved; update attr cache readers");
static_assert(
    kAttrCacheIndexInstructionOffset == 4,
    "_PyAttrCache::index moved; update attr cache readers");

#if PY_VERSION_HEX >= 0x030E0000
// LOAD_ATTR specializations that carry a method descriptor (notably
// LOAD_ATTR_METHOD_WITH_VALUES) read their inline cache through the
// _PyLoadMethodCache view, NOT _PyAttrCache. The JIT bakes these fields as
// constants at compile time (see HIRBuilder::emitLoadAttr), so pin their
// code-unit offsets here: if CPython ever moves them, fail the build instead
// of silently reading the wrong cache slot.
static_assert(
    offsetof(_PyLoadMethodCache, type_version) % sizeof(_Py_CODEUNIT) == 0,
    "_PyLoadMethodCache::type_version must be code-unit aligned");
static_assert(
    offsetof(_PyLoadMethodCache, keys_version) % sizeof(_Py_CODEUNIT) == 0,
    "_PyLoadMethodCache::keys_version must be code-unit aligned");
static_assert(
    offsetof(_PyLoadMethodCache, descr) % sizeof(_Py_CODEUNIT) == 0,
    "_PyLoadMethodCache::descr must be code-unit aligned");

constexpr int kLoadMethodCacheTypeVersionInstructionOffset =
    attrCacheInstructionOffset(offsetof(_PyLoadMethodCache, type_version));
constexpr int kLoadMethodCacheKeysVersionInstructionOffset =
    attrCacheInstructionOffset(offsetof(_PyLoadMethodCache, keys_version));
constexpr int kLoadMethodCacheDescrInstructionOffset =
    attrCacheInstructionOffset(offsetof(_PyLoadMethodCache, descr));

static_assert(
    kLoadMethodCacheTypeVersionInstructionOffset == 2,
    "_PyLoadMethodCache::type_version moved; update LOAD_ATTR method readers "
    "in HIRBuilder::emitLoadAttr");
static_assert(
    kLoadMethodCacheKeysVersionInstructionOffset == 4,
    "_PyLoadMethodCache::keys_version moved; update LOAD_ATTR method readers "
    "in HIRBuilder::emitLoadAttr");
static_assert(
    kLoadMethodCacheDescrInstructionOffset == 6,
    "_PyLoadMethodCache::descr moved; update LOAD_ATTR method readers "
    "in HIRBuilder::emitLoadAttr");

// descr is read with a pointer-sized memcpy in HIRBuilder::emitLoadAttr; make
// sure the cache slot really is one pointer wide.
static_assert(
    sizeof(((_PyLoadMethodCache*)nullptr)->descr) == sizeof(PyObject*),
    "_PyLoadMethodCache::descr is not pointer-sized");
#endif

} // namespace

BCOffset BytecodeInstruction::baseOffset() const {
  return baseOffset_;
}

BCIndex BytecodeInstruction::baseIndex() const {
  return baseOffset();
}

BCOffset BytecodeInstruction::opcodeOffset() const {
  calcOpcodeOffsetAndOparg();
  return opcodeIndex_;
}

BCIndex BytecodeInstruction::opcodeIndex() const {
  return opcodeOffset();
}

int BytecodeInstruction::opcode() const {
  int op = _Py_OPCODE(word());
  if (extendedOpcode_) {
    return EXTENDED_OPCODE_FLAG | op;
  }
  return op;
}

void BytecodeInstruction::calcOpcodeOffsetAndOparg() const {
  if (opcodeIndex_.value() != std::numeric_limits<int>::min()) {
    return;
  }

  opcodeIndex_ = baseOffset_;
  BCIndex end_idx{countIndices(code_)};
  if (opcodeIndex_.value() >= end_idx) {
    return;
  }

  // Consume all EXTENDED_ARG opcodes until we get to something else.
  auto consume_extended_args = [&] {
    while (_Py_OPCODE(word()) == EXTENDED_ARG) {
      JIT_DCHECK(
          opcodeIndex_.value() < end_idx, "EXTENDED_ARG at end of bytecode");
      extendedOparg_ = (extendedOparg_ << 8) | _Py_OPARG(word());
      opcodeIndex_++;
    }
  };

  consume_extended_args();

  // Check for EXTENDED_OPCODE, bump forward one opcode if so.
  if (PY_VERSION_HEX >= 0x030E0000 && _Py_OPCODE(word()) == EXTENDED_OPCODE) {
    opcodeIndex_++;
    extendedOparg_ = 0;
    extendedOpcode_ = true;
  }

  // If we had an EXTENDED_OPCODE, then it might also be followed by more
  // EXTENDED_ARGS.
  consume_extended_args();

  extendedOparg_ = (extendedOparg_ << 8) | _Py_OPARG(word());
}

int BytecodeInstruction::uninstrumentedOpcode() const {
  return uninstrument(code_, opcodeIndex().value());
}

int BytecodeInstruction::specializedOpcode() const {
  int opcode = uninstrumentedOpcode();

#if PY_VERSION_HEX < 0x030C0000
  // 守卫自适应去特化：粘滞位置位后本 code 的一切特化形按生形翻译
  //（本函数是全部特化消费的单控制点，含 BINARY_OP/COMPARE 类型守卫
  // 与属性投机的识别）。
  if (getConfig().adaptive_despec) {
    CodeExtra* extra = codeExtraIfExists(code_);
    if (extra != nullptr && Ci_code_extra_load_despec_relaxed(extra) != 0) {
      return unspecialize(opcode);
    }
  }
#endif

  switch (opcode) {
    case BINARY_OP_ADD_FLOAT:
    case BINARY_OP_ADD_INT:
    case BINARY_OP_ADD_UNICODE:
    case BINARY_OP_MULTIPLY_FLOAT:
    case BINARY_OP_MULTIPLY_INT:
    case BINARY_OP_SUBTRACT_FLOAT:
    case BINARY_OP_SUBTRACT_INT:
    case BINARY_SUBSCR_DICT:
    case BINARY_SUBSCR_LIST_INT:
    case BINARY_SUBSCR_TUPLE_INT:
#ifdef BINARY_OP_SUBSCR_DICT
    case BINARY_OP_SUBSCR_DICT:
#endif
#ifdef BINARY_OP_SUBSCR_LIST_INT
    case BINARY_OP_SUBSCR_LIST_INT:
#endif
#ifdef BINARY_OP_SUBSCR_TUPLE_INT
    case BINARY_OP_SUBSCR_TUPLE_INT:
#endif
#if PY_VERSION_HEX >= 0x030C0000
    // COMPARE_OP 特化形在 3.11 下的 HIR 翻译显著劣于去特化生形
    // （提前 quickening 轮实测：raytrace 保留该族 191.8 ms vs 摘除
    // 124.6 ms，浮点比较密集负载全族受累），3.11 摘除、去特化走
    // 既有生形翻译；3.12+ 名单不变。
    case COMPARE_OP_FLOAT:
    case COMPARE_OP_INT:
    case COMPARE_OP_STR:
#endif
#if PY_VERSION_HEX < 0x030C0000
    // 3.11 的比较特化名带 _JUMP 后缀(COMPARE_OP 融合轮):不入白名单
    // 则被打回生形,builder 的类型守卫发射永不可达。前一版嵌套在
    // >=3.12 守卫内被预处理吃掉(定罪实录)。上方"提前 quickening 轮
    // raytrace 191.8 vs 124.6"判决成立于 despec 诞生同轮——本轮为
    // despec 武装态下的受控重测,若风暴复现即回摘。
    case COMPARE_OP_FLOAT_JUMP:
    case COMPARE_OP_INT_JUMP:
    // STR_JUMP 不放行:simplify 无 Unicode 比较降级,守卫纯付风险
    // 零回报——v26 实测字符串比较密集族回归(tomli_loads −14%/
    // django_template −5%),INT/FLOAT 数值族净正(float +3.8%)。
    // 下标写侧特化形(异常带轮):读侧 BINARY_SUBSCR_LIST_INT 早已
    // 放行且 simplify 有完整定型下沉,写侧此前缺席——list 元素写
    // 一律走泛型 helper。放行后 builder 发类型守卫,simplify 落成
    // 行内存储(收养旧值+锚定后置 decref,见 simplifyStoreSubscr)。
    case STORE_SUBSCR_LIST_INT:
#endif
#if PY_VERSION_HEX >= 0x030C0000
#endif
#if PY_VERSION_HEX >= 0x030E0000
    case TO_BOOL_BOOL:
    case TO_BOOL_INT:
    case TO_BOOL_LIST:
    case TO_BOOL_STR:
    case LOAD_ATTR_METHOD_WITH_VALUES:
#endif
    case LOAD_ATTR_SLOT:
#if PY_VERSION_HEX < 0x030C0000
    // 投机消费另由 specialized_attr_speculation 旗标（默认关）控制，
    // 此处保留识别。
    case LOAD_ATTR_INSTANCE_VALUE:
    case LOAD_METHOD_WITH_VALUES:
    case LOAD_GLOBAL_MODULE:
#endif
    case LOAD_ATTR_MODULE:
    case STORE_ATTR_SLOT:
    case STORE_SUBSCR_DICT:
    case UNPACK_SEQUENCE_LIST:
    case UNPACK_SEQUENCE_TUPLE:
    case UNPACK_SEQUENCE_TWO_TUPLE:
      return opcode;
    default:
      return unspecialize(opcode);
  }
}

bool BytecodeInstruction::isSubscrAdaptiveStuck() const {
#if PY_VERSION_HEX < 0x030C0000
  int raw = _Py_OPCODE(codeUnit(code_)[opcodeIndex().value()]);
  if (raw != BINARY_SUBSCR_ADAPTIVE && raw != STORE_SUBSCR_ADAPTIVE) {
    return false;
  }
  // 计数器低 4 位是 backoff 档。3.11 的 _PyCode_Quicken 将计数器
  // 清零("Make sure the adaptive counter is zero"),首次执行即尝试
  // 特化:成功则替换 opcode(不再是 ADAPTIVE 形),失败则
  // adaptive_counter_backoff 置 backoff≥1。故仍为 ADAPTIVE 形且
  // backoff 非零 ⇔ 至少尝试过一次且失败(或特化后因 miss 回退),
  // backoff 为零 ⇔ 自 quicken 起从未执行——排除冷位点假阳性。
  uint16_t counter = cacheU16(kInlineCacheStartInstructionOffset);
  return (counter & ((1 << ADAPTIVE_BACKOFF_BITS) - 1)) != 0;
#else
  return false;
#endif
}

int BytecodeInstruction::oparg() const {
  calcOpcodeOffsetAndOparg();
  return extendedOparg_;
}

uint16_t BytecodeInstruction::cacheU16(int instruction_offset) const {
  auto idx = opcodeIndex().value() + instruction_offset;
#if PY_VERSION_HEX >= 0x030C0000
  return codeUnit(code_)[idx].cache;
#else
  // 3.11: _Py_CODEUNIT 是裸 uint16，缓存槽即码元本身
  return codeUnit(code_)[idx];
#endif
}

uint32_t BytecodeInstruction::cacheU32(int instruction_offset) const {
  auto idx = opcodeIndex().value() + instruction_offset;
#if PY_VERSION_HEX >= 0x030C0000
  uint32_t lo = codeUnit(code_)[idx].cache;
  uint32_t hi = codeUnit(code_)[idx + 1].cache;
#else
  uint32_t lo = codeUnit(code_)[idx];
  uint32_t hi = codeUnit(code_)[idx + 1];
#endif
  return lo | (hi << 16);
}

uint32_t BytecodeInstruction::attrCacheTypeVersion() const {
  return cacheU32(kAttrCacheTypeVersionInstructionOffset);
}

uint16_t BytecodeInstruction::attrCacheIndex() const {
  return cacheU16(kAttrCacheIndexInstructionOffset);
}

bool BytecodeInstruction::isBranch() const {
  switch (opcode()) {
    case FOR_ITER:
    case JUMP_ABSOLUTE:
    case JUMP_BACKWARD:
    case JUMP_BACKWARD_NO_INTERRUPT:
#if PY_VERSION_HEX >= 0x030E0000
    case JUMP_BACKWARD_JIT:
    case JUMP_BACKWARD_NO_JIT:
#endif
    case JUMP_FORWARD:
    case JUMP_IF_FALSE_OR_POP:
    case JUMP_IF_NONZERO_OR_POP:
    case JUMP_IF_NOT_EXC_MATCH:
    case JUMP_IF_TRUE_OR_POP:
    case JUMP_IF_ZERO_OR_POP:
#if PY_VERSION_HEX < 0x030C0000
    case POP_JUMP_BACKWARD_IF_FALSE:
    case POP_JUMP_BACKWARD_IF_NONE:
    case POP_JUMP_BACKWARD_IF_NOT_NONE:
    case POP_JUMP_BACKWARD_IF_TRUE:
    case POP_JUMP_FORWARD_IF_FALSE:
    case POP_JUMP_FORWARD_IF_NONE:
    case POP_JUMP_FORWARD_IF_NOT_NONE:
    case POP_JUMP_FORWARD_IF_TRUE:
#endif
    case POP_JUMP_IF_FALSE:
    case POP_JUMP_IF_NONE:
    case POP_JUMP_IF_NONZERO:
    case POP_JUMP_IF_NOT_NONE:
    case POP_JUMP_IF_TRUE:
    case POP_JUMP_IF_ZERO:
    case SEND:
    case SETUP_FINALLY:
      return true;
    default:
      return false;
  }
}

bool BytecodeInstruction::isBackwardBranch() const {
  return isBranch() && getJumpTarget() <= baseOffset();
}

bool BytecodeInstruction::isReturn() const {
  switch (opcode()) {
    case RETURN_CONST:
    case RETURN_PRIMITIVE:
    case RETURN_VALUE:
      return true;
    default:
      return false;
  }
}

bool BytecodeInstruction::isTerminator() const {
  switch (opcode()) {
    case RAISE_VARARGS:
    case RERAISE:
      return true;
    default:
      return isBranch() || isReturn();
  }
}

BCOffset BytecodeInstruction::getJumpTarget() const {
  JIT_DCHECK(
      isBranch(), "Calling getJumpTarget() on a non-branch gives nonsense");

  if (isAbsoluteControlFlow()) {
    return BCIndex{oparg()};
  }

  int delta = oparg();
  if (opcode() == JUMP_BACKWARD || opcode() == JUMP_BACKWARD_NO_INTERRUPT
#if PY_VERSION_HEX >= 0x030E0000
      || opcode() == JUMP_BACKWARD_JIT || opcode() == JUMP_BACKWARD_NO_JIT
#endif
#if PY_VERSION_HEX < 0x030C0000
      || opcode() == POP_JUMP_BACKWARD_IF_FALSE ||
      opcode() == POP_JUMP_BACKWARD_IF_NONE ||
      opcode() == POP_JUMP_BACKWARD_IF_NOT_NONE ||
      opcode() == POP_JUMP_BACKWARD_IF_TRUE
#endif
  ) {
    delta = -delta;
  }
  BCIndex target = BCIndex{nextInstrOffset()} + delta;
  // In 3.11+ the FOR_ITER bytecode encodes a jump to an END_FOR instruction
  // then at runtime it usually dynamically jumps past this. The only time it
  // actually goes through the END_FOR is if the FOR_ITER is operating
  // on a generator and gets adaptively specialized. We always compile
  // unspecialized bytecode so we can always skip the END_FOR.
  //
  // We make this tweak here so it applies both when generating the branching
  // HIR operation, and when creating block boundaries for bytecode. The END_FOR
  // will end up on its own in an unreachable block.
  if (PY_VERSION_HEX >= 0x030C0000 && opcode() == FOR_ITER) {
    BytecodeInstruction target_bc{code_, target};
    JIT_CHECK(target_bc.opcode() == END_FOR, "Expected END_FOR");
    return target_bc.nextInstrOffset();
  }
  return target;
}

BCOffset BytecodeInstruction::nextInstrOffset() const {
  return BCOffset{
      opcodeIndex() + inlineCacheSize(code_, opcodeIndex().value()) + 1};
}

_Py_CODEUNIT BytecodeInstruction::word() const {
  int opcode = unspecialize(uninstrumentedOpcode());
  int oparg = _Py_OPARG(codeUnit(code_)[opcodeIndex().value()]);
  return _Py_MAKE_CODEUNIT(opcode, oparg);
}

bool BytecodeInstruction::isAbsoluteControlFlow() const {
#if PY_VERSION_HEX < 0x030C0000
  return false;
#else
  switch (opcode()) {
    case JUMP_ABSOLUTE:
    case JUMP_IF_FALSE_OR_POP:
    case JUMP_IF_NONZERO_OR_POP:
    case JUMP_IF_NOT_EXC_MATCH:
    case JUMP_IF_TRUE_OR_POP:
    case JUMP_IF_ZERO_OR_POP:
      return true;
    case POP_JUMP_IF_NONZERO:
    case POP_JUMP_IF_ZERO:
    case POP_JUMP_IF_FALSE:
    case POP_JUMP_IF_TRUE:
      return false;
    default:
      return false;
  }
#endif
}

BytecodeInstructionBlock::BytecodeInstructionBlock(
    BorrowedRef<PyCodeObject> code)
    : BytecodeInstructionBlock{code, BCIndex{0}, BCIndex{countIndices(code)}} {}

BytecodeInstructionBlock::BytecodeInstructionBlock(
    BorrowedRef<PyCodeObject> code,
    BCIndex start,
    BCIndex end)
    : code_{ThreadedRef<PyCodeObject>::create(code)},
      start_idx_{start},
      end_idx_{end} {}

BytecodeInstructionBlock::Iterator BytecodeInstructionBlock::begin() const {
  return Iterator{code_, start_idx_, end_idx_};
}

BytecodeInstructionBlock::Iterator BytecodeInstructionBlock::end() const {
  return Iterator{code_, end_idx_, end_idx_};
}

BCOffset BytecodeInstructionBlock::startOffset() const {
  return start_idx_;
}

BCOffset BytecodeInstructionBlock::endOffset() const {
  return end_idx_;
}

Py_ssize_t BytecodeInstructionBlock::size() const {
  return end_idx_ - start_idx_;
}

BytecodeInstruction BytecodeInstructionBlock::at(BCIndex idx) const {
  JIT_CHECK(
      idx >= start_idx_ && idx < end_idx_,
      "Invalid index {}, bytecode block is [{}, {})",
      idx,
      start_idx_,
      end_idx_);
  return BytecodeInstruction{code_, idx};
}

BorrowedRef<PyCodeObject> BytecodeInstructionBlock::code() const {
  return code_;
}

} // namespace jit
