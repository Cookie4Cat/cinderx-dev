// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/tree_iter_state_machine_pass.h"

#include "cinderx/Common/log.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/hir/clean_cfg.h"
#include "cinderx/Jit/hir/function.h"
#include "cinderx/Jit/hir/pass.h"

#include <Python.h>

#include <unordered_map>

namespace jit::hir {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Return true if `names` (a tuple) contains the UTF-8 string `name`.
static bool tupleContainsName(PyObject* names, const char* name) {
  if (names == nullptr || !PyTuple_Check(names)) {
    return false;
  }
  Py_ssize_t n = PyTuple_GET_SIZE(names);
  for (Py_ssize_t i = 0; i < n; i++) {
    PyObject* item = PyTuple_GET_ITEM(names, i);
    if (PyUnicode_CheckExact(item) &&
        PyUnicode_CompareWithASCIIString(item, name) == 0) {
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// TreeIterStateMachinePass::traceYieldFromIterable
// ---------------------------------------------------------------------------

const LoadField* TreeIterStateMachinePass::traceYieldFromIterable(
    const Register* iter_reg) const {
  if (iter_reg == nullptr) {
    return nullptr;
  }

  // Walk through transparent wrappers to reach the original field producer.
  // Limit iterations to avoid infinite loops on ill-formed HIR.
  constexpr int kMaxDepth = 16;
  const Register* cur = iter_reg;
  for (int depth = 0; depth < kMaxDepth; depth++) {
    const Instr* def = cur->instr();
    if (def == nullptr) {
      return nullptr;
    }
    switch (def->opcode()) {
      case Opcode::kLoadField:
        return static_cast<const LoadField*>(def);

      case Opcode::kCheckField:
        // Slot/member-descriptor field loads are commonly guarded by
        // CheckField to preserve AttributeError semantics for unset slots.
        // The field identity and offset are still carried by the operand.
        cur = def->GetOperand(0);
        break;

      case Opcode::kAssign:
        // Assign is transparent: continue tracing through the source operand.
        cur = def->GetOperand(0);
        break;

      case Opcode::kGetIter: {
        // GetIter wraps the iterable; trace into it.
        const auto* gi = static_cast<const GetIter*>(def);
        cur = gi->iterable();
        break;
      }

      case Opcode::kPhi: {
        // A Phi is acceptable only if it is trivial (all arms produce the same
        // source), which is the emitGetYieldFromIter pattern where exact
        // generator/coroutine objects are Assigned directly while the slow
        // path goes through GetIter.  Accept a non-trivial Phi only if every
        // arm traces to the same LoadField (same instruction pointer).
        const auto* phi = static_cast<const Phi*>(def);
        const LoadField* result = nullptr;
        for (std::size_t i = 0; i < phi->NumOperands(); i++) {
          const Register* arm = phi->GetOperand(i);
          const LoadField* arm_lf = traceYieldFromIterable(arm);
          if (arm_lf == nullptr) {
            return nullptr;
          }
          if (result == nullptr) {
            result = arm_lf;
          } else if (result != arm_lf) {
            return nullptr;
          }
        }
        return result;
      }

      default:
        return nullptr;
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// TreeIterStateMachinePass::matchTreeIter
// ---------------------------------------------------------------------------

std::optional<TreeIterMatch> TreeIterStateMachinePass::matchTreeIter(
    const Function& func) const {
  // -----------------------------------------------------------------------
  // 1. Validate code object and field name membership.
  // -----------------------------------------------------------------------
  if (!func.code) {
    return std::nullopt;
  }
  if (func.code->co_argcount != 1 || func.code->co_kwonlyargcount != 0 ||
      (func.code->co_flags & (CO_VARARGS | CO_VARKEYWORDS)) ||
      !PyUnicode_CheckExact(func.code->co_name) ||
      PyUnicode_CompareWithASCIIString(func.code->co_name, "__iter__") != 0) {
    JIT_DLOG(
        "TreeIter matcher: code object is not an exact __iter__(self): {}",
        func.fullname);
    return std::nullopt;
  }

  PyObject* co_names = func.code->co_names;
  if (!tupleContainsName(co_names, "left") ||
      !tupleContainsName(co_names, "right") ||
      !tupleContainsName(co_names, "value")) {
    return std::nullopt;
  }

  // -----------------------------------------------------------------------
  // 2. Scan CFG to collect candidate instructions.
  // -----------------------------------------------------------------------
  Instr* initial_yield = nullptr;
  Register* self_reg = nullptr;

  // There should be exactly one plain YieldValue (the value yield) and
  // exactly two yield-from YieldValues (left and right).
  const YieldValue* plain_yv = nullptr;
  const YieldValue* yf_first = nullptr;
  const YieldValue* yf_second = nullptr;
  std::unordered_map<const Instr*, std::size_t> instr_order;
  std::size_t next_instr_order = 0;

  for (const BasicBlock& block : func.cfg.blocks) {
    for (const Instr& instr : block) {
      instr_order.emplace(&instr, next_instr_order++);
      switch (instr.opcode()) {
        case Opcode::kInitialYield:
          if (initial_yield != nullptr) {
            // Multiple InitialYield instructions — not the expected pattern.
            JIT_DLOG(
                "TreeIter matcher: multiple InitialYield in {}", func.fullname);
            return std::nullopt;
          }
          initial_yield = const_cast<Instr*>(&instr);
          break;

        case Opcode::kLoadArg: {
          const auto* la = static_cast<const LoadArg*>(&instr);
          if (la->arg_idx() == 0) {
            // Accept the first LoadArg(0) as self.  If there are multiple,
            // SSAify will have produced separate registers for each use, but
            // they all trace to the same argument.  We only need one.
            if (self_reg == nullptr) {
              self_reg = la->output();
            }
          }
          break;
        }

        case Opcode::kYieldValue: {
          const auto* yv = static_cast<const YieldValue*>(&instr);
          if (yv->isYieldFrom()) {
            if (yf_first == nullptr) {
              yf_first = yv;
            } else if (yf_second == nullptr) {
              yf_second = yv;
            } else {
              // More than two yield-from — not the canonical pattern.
              JIT_DLOG(
                  "TreeIter matcher: >2 yield-from in {}", func.fullname);
              return std::nullopt;
            }
          } else {
            if (plain_yv != nullptr) {
              // More than one plain yield — not the canonical pattern.
              JIT_DLOG(
                  "TreeIter matcher: >1 plain yield in {}", func.fullname);
              return std::nullopt;
            }
            plain_yv = yv;
          }
          break;
        }

        default:
          break;
      }
    }
  }

  // -----------------------------------------------------------------------
  // 3. Verify we found all required instructions.
  // -----------------------------------------------------------------------
  if (initial_yield == nullptr) {
    JIT_DLOG("TreeIter matcher: no InitialYield in {}", func.fullname);
    return std::nullopt;
  }
  if (self_reg == nullptr) {
    JIT_DLOG("TreeIter matcher: no LoadArg(0) in {}", func.fullname);
    return std::nullopt;
  }
  if (plain_yv == nullptr) {
    JIT_DLOG("TreeIter matcher: no plain YieldValue in {}", func.fullname);
    return std::nullopt;
  }
  if (yf_first == nullptr || yf_second == nullptr) {
    JIT_DLOG(
        "TreeIter matcher: need exactly 2 yield-from, got {} in {}",
        (yf_first ? 1 : 0) + (yf_second ? 1 : 0),
        func.fullname);
    return std::nullopt;
  }

  // -----------------------------------------------------------------------
  // 4. Trace yield-from iterables to LoadField instructions.
  // -----------------------------------------------------------------------
  const LoadField* lf_first = traceYieldFromIterable(yf_first->yieldFromIter());
  const LoadField* lf_second =
      traceYieldFromIterable(yf_second->yieldFromIter());

  if (lf_first == nullptr || lf_second == nullptr) {
    JIT_DLOG(
        "TreeIter matcher: cannot trace yield-from to LoadField in {}",
        func.fullname);
    return std::nullopt;
  }

  // -----------------------------------------------------------------------
  // 5. Identify left vs. right by field name and exact self receiver.
  //
  //    The receiver of each LoadField must trace back to LoadArg(0) through
  //    transparent HIR wrappers such as Assign/GuardType/RefineType.  The
  //    accepted field loads must then share the same exact receiver register,
  //    which is the owner proof used by the generated state machine.
  // -----------------------------------------------------------------------
  auto isFromSelf = [&](const Register* recv) -> bool {
    if (recv == self_reg) {
      return true;
    }
    constexpr int kMaxSelfTraceDepth = 8;
    const Register* cur = recv;
    for (int depth = 0; depth < kMaxSelfTraceDepth; depth++) {
      const Instr* def = cur->instr();
      if (def == nullptr) {
        return false;
      }
      if (def->opcode() == Opcode::kLoadArg) {
        return static_cast<const LoadArg*>(def)->arg_idx() == 0;
      }
      if (def->opcode() == Opcode::kAssign ||
          def->opcode() == Opcode::kGuardType ||
          def->opcode() == Opcode::kRefineType) {
        cur = def->GetOperand(0);
        if (cur == self_reg) {
          return true;
        }
        continue;
      }
      return false;
    }
    return false;
  };

  auto isSameReceiver = [](const LoadField* lhs, const LoadField* rhs) {
    return lhs->receiver() == rhs->receiver();
  };

  auto isSameField = [&](const LoadField* lhs, const LoadField* rhs) {
    return lhs->name() == rhs->name() && lhs->offset() == rhs->offset() &&
        isSameReceiver(lhs, rhs);
  };

  if (!isFromSelf(lf_first->receiver()) ||
      !isFromSelf(lf_second->receiver())) {
    JIT_DLOG(
        "TreeIter matcher: yield-from LoadField receiver is not self in {}",
        func.fullname);
    return std::nullopt;
  }

  // Map each LoadField to left or right by name.
  const LoadField* lf_left = nullptr;
  const LoadField* lf_right = nullptr;
  const YieldValue* left_yf = nullptr;
  const YieldValue* right_yf = nullptr;

  if (lf_first->name() == "left" && lf_second->name() == "right") {
    lf_left = lf_first;
    lf_right = lf_second;
    left_yf = yf_first;
    right_yf = yf_second;
  } else if (lf_first->name() == "right" && lf_second->name() == "left") {
    lf_left = lf_second;
    lf_right = lf_first;
    left_yf = yf_second;
    right_yf = yf_first;
  } else {
    JIT_DLOG(
        "TreeIter matcher: yield-from fields are not left+right in {}",
        func.fullname);
    return std::nullopt;
  }

  if (!isSameReceiver(lf_left, lf_right)) {
    JIT_DLOG(
        "TreeIter matcher: left/right receivers differ in {}", func.fullname);
    return std::nullopt;
  }

  Register* exact_self_reg = lf_left->receiver();
  Type self_type = exact_self_reg->type();
  if (!self_type.isExact() || self_type.runtimePyType() == nullptr) {
    JIT_DLOG(
        "TreeIter matcher: self type is not exact in {}", func.fullname);
    return std::nullopt;
  }

  auto orderOf = [&](const Instr* instr) -> std::size_t {
    auto it = instr_order.find(instr);
    JIT_CHECK(it != instr_order.end(), "TreeIter matcher lost instruction");
    return it->second;
  };

  if (!(orderOf(left_yf) < orderOf(plain_yv) &&
        orderOf(plain_yv) < orderOf(right_yf))) {
    JIT_DLOG(
        "TreeIter matcher: yields are not ordered left/value/right in {}",
        func.fullname);
    return std::nullopt;
  }

  // -----------------------------------------------------------------------
  // 6. Verify the plain YieldValue comes from LoadField(self, "value").
  // -----------------------------------------------------------------------
  const Register* value_reg = plain_yv->reg();
  if (value_reg == nullptr) {
    return std::nullopt;
  }
  const LoadField* lf_value = traceYieldFromIterable(value_reg);
  if (lf_value == nullptr) {
    JIT_DLOG(
        "TreeIter matcher: plain yield value is not from LoadField in {}",
        func.fullname);
    return std::nullopt;
  }
  if (lf_value->name() != "value" || !isFromSelf(lf_value->receiver())) {
    JIT_DLOG(
        "TreeIter matcher: plain yield value LoadField not self.value in {}",
        func.fullname);
    return std::nullopt;
  }
  if (!isSameReceiver(lf_left, lf_value)) {
    JIT_DLOG(
        "TreeIter matcher: value receiver differs in {}", func.fullname);
    return std::nullopt;
  }

  // -----------------------------------------------------------------------
  // 7. Require an explicit None guard before each yield-from.
  //
  //    The canonical HIR for `if self.left is not None: yield from self.left`
  //    contains a PrimitiveCompare(kEqual/kNotEqual, field_val, none_const)
  //    whose first operand traces to the same LoadField that feeds the
  //    yield-from.  A GuardIs(field_val, Py_None) is the post-Simplify form.
  //
  //    We scan for any PrimitiveCompare or GuardIs instruction that has at
  //    least one operand whose producer-chain ends at the same field identity.
  //    A full dominance check is deferred to M3; here we just verify that
  //    the guard code exists somewhere in the function.
  // -----------------------------------------------------------------------
  bool left_guard_found = false;
  bool right_guard_found = false;

  for (const BasicBlock& block : func.cfg.blocks) {
    for (const Instr& instr : block) {
      if (instr.opcode() != Opcode::kPrimitiveCompare &&
          instr.opcode() != Opcode::kGuardIs) {
        continue;
      }
      for (std::size_t i = 0; i < instr.NumOperands(); i++) {
        const Register* operand = instr.GetOperand(i);
        if (operand == nullptr) {
          continue;
        }
        const LoadField* lf = traceYieldFromIterable(operand);
        if (lf != nullptr && isSameField(lf, lf_left)) {
          left_guard_found = true;
        }
        if (lf != nullptr && isSameField(lf, lf_right)) {
          right_guard_found = true;
        }
      }
      if (left_guard_found && right_guard_found) {
        break;
      }
    }
  }

  if (!left_guard_found || !right_guard_found) {
    JIT_DLOG(
        "TreeIter matcher: missing None guard (left={}, right={}) in {}",
        left_guard_found,
        right_guard_found,
        func.fullname);
    return std::nullopt;
  }

  // -----------------------------------------------------------------------
  // 8. Collect the FrameState from the plain YieldValue.
  // -----------------------------------------------------------------------
  const FrameState* frame_state = plain_yv->frameState();
  if (frame_state == nullptr) {
    JIT_DLOG(
        "TreeIter matcher: plain YieldValue has no FrameState in {}",
        func.fullname);
    return std::nullopt;
  }

  // -----------------------------------------------------------------------
  // 9. Build and return the match result.
  // -----------------------------------------------------------------------
  TreeIterMatch match;
  match.self_reg = exact_self_reg;
  match.initial_yield = initial_yield;
  match.yield_frame_state = const_cast<FrameState*>(frame_state);
  match.exact_node_type = self_type;
  match.left_offset = lf_left->offset();
  match.right_offset = lf_right->offset();
  match.value_offset = lf_value->offset();
  match.left_yield_from = left_yf;
  match.value_yield = plain_yv;
  match.right_yield_from = right_yf;
  return match;
}

// ---------------------------------------------------------------------------
// TreeIterStateMachinePass::buildTreeIterStateMachine
// ---------------------------------------------------------------------------

// Helper: allocate a register holding an int32 phase constant.
static Register* emitPhaseConst(
    BasicBlock* block,
    Environment& env,
    TreeIterPhase phase) {
  Register* r = env.AllocateRegister();
  block->append<LoadConst>(
      r, Type::fromCInt(static_cast<int32_t>(phase), TCInt32));
  return r;
}

void TreeIterStateMachinePass::buildTreeIterStateMachine(
    Function& func,
    const TreeIterMatch& match) {
  // -----------------------------------------------------------------------
  // Locate the block that contains InitialYield; we need to insert state
  // machine setup instructions before it.
  // -----------------------------------------------------------------------
  Instr* init_yield = match.initial_yield;
  BasicBlock* init_block = init_yield->block();
  JIT_CHECK(init_block != nullptr, "InitialYield has no block");

  const FrameState& fs = *match.yield_frame_state;

  // -----------------------------------------------------------------------
  // 1. Split the CFG after InitialYield so init_block ends with it.
  //    The tail is the original continuation of the init block.  We do not
  //    use the tail in the state machine; it will become unreachable and
  //    removed by CleanCFG.
  // -----------------------------------------------------------------------
  /* tail = */ func.cfg.splitAfter(*init_yield);

  // -----------------------------------------------------------------------
  // 2. Allocate state machine basic blocks.
  // -----------------------------------------------------------------------
  BasicBlock* bb_loop            = func.cfg.AllocateBlock();
  BasicBlock* bb_left            = func.cfg.AllocateBlock();
  BasicBlock* bb_check_null_left = func.cfg.AllocateBlock();
  BasicBlock* bb_has_left        = func.cfg.AllocateBlock();
  BasicBlock* bb_no_left         = func.cfg.AllocateBlock();
  BasicBlock* bb_yield           = func.cfg.AllocateBlock();
  BasicBlock* bb_right           = func.cfg.AllocateBlock();
  BasicBlock* bb_check_null_right= func.cfg.AllocateBlock();
  BasicBlock* bb_has_right       = func.cfg.AllocateBlock();
  BasicBlock* bb_no_right        = func.cfg.AllocateBlock();
  BasicBlock* bb_backtrack       = func.cfg.AllocateBlock();
  BasicBlock* bb_pop             = func.cfg.AllocateBlock();
  BasicBlock* bb_exit            = func.cfg.AllocateBlock();
  BasicBlock* bb_done            = func.cfg.AllocateBlock();

  Environment& env = func.env;

  // -----------------------------------------------------------------------
  // 3. init_block resume path setup → Branch(bb_loop).
  //
  //    TreeIter helpers take GenDataFooter* as their first argument.  The JIT
  //    frame pointer is not switched to the footer until InitialYield runs, so
  //    setup must be emitted after InitialYield.  LIR generation places HIR
  //    instructions following InitialYield in the resume block.
  // -----------------------------------------------------------------------
  {
    Register* ensure_status = env.AllocateRegister();
    init_block->append<EnsureTreeIterState>(ensure_status, fs);
    init_block->append<SaveCurrentNode>(match.self_reg);
    Register* kLeft_r = emitPhaseConst(init_block, env, TreeIterPhase::kLeft);
    init_block->append<SavePhase>(kLeft_r);
  }
  init_block->append<Branch>(bb_loop);

  // -----------------------------------------------------------------------
  // 4. bb_loop: LoadPhase, then dispatch to one of five phase blocks.
  //    We chain CondBranch comparisons: kLeft → bb_left, kYield → bb_yield,
  //    kRight → bb_right, kExit → bb_exit, default → bb_backtrack.
  // -----------------------------------------------------------------------
  {
    Register* phase = env.AllocateRegister();
    bb_loop->append<LoadPhase>(phase);

    // Dispatch: kLeft (0)
    Register* kLeft_v = emitPhaseConst(bb_loop, env, TreeIterPhase::kLeft);
    Register* is_left = env.AllocateRegister();
    bb_loop->append<PrimitiveCompare>(
        is_left, PrimitiveCompareOp::kEqual, phase, kLeft_v);

    // CondBranch is_left → bb_left, else continue to check kYield
    BasicBlock* bb_not_left = func.cfg.AllocateBlock();
    bb_loop->append<CondBranch>(is_left, bb_left, bb_not_left);

    // kYield (1)
    Register* kYield_v = emitPhaseConst(bb_not_left, env, TreeIterPhase::kYield);
    Register* is_yield = env.AllocateRegister();
    bb_not_left->append<PrimitiveCompare>(
        is_yield, PrimitiveCompareOp::kEqual, phase, kYield_v);
    BasicBlock* bb_not_yield = func.cfg.AllocateBlock();
    bb_not_left->append<CondBranch>(is_yield, bb_yield, bb_not_yield);

    // kRight (2)
    Register* kRight_v = emitPhaseConst(bb_not_yield, env, TreeIterPhase::kRight);
    Register* is_right = env.AllocateRegister();
    bb_not_yield->append<PrimitiveCompare>(
        is_right, PrimitiveCompareOp::kEqual, phase, kRight_v);
    BasicBlock* bb_not_right = func.cfg.AllocateBlock();
    bb_not_yield->append<CondBranch>(is_right, bb_right, bb_not_right);

    // kExit (4) vs kBacktrack (3): anything != kBacktrack goes to bb_exit
    Register* kBack_v = emitPhaseConst(bb_not_right, env, TreeIterPhase::kBacktrack);
    Register* is_back = env.AllocateRegister();
    bb_not_right->append<PrimitiveCompare>(
        is_back, PrimitiveCompareOp::kEqual, phase, kBack_v);
    bb_not_right->append<CondBranch>(is_back, bb_backtrack, bb_exit);
  }

  // -----------------------------------------------------------------------
  // 5. bb_left (LEFT phase): load left child, branch on None.
  // -----------------------------------------------------------------------
  {
    Register* current = env.AllocateRegister();
    bb_left->append<LoadCurrentNode>(current);

    Register* left_child = env.AllocateRegister();
    bb_left->append<LoadField>(
        left_child, current, "left", match.left_offset, TOptObject);

    // Check left_child != None (None is represented as nullptr or TNoneType).
    // We use a PrimitiveCompare(kEqual, left_child, none_const) to detect None.
    Register* none_const = env.AllocateRegister();
    bb_left->append<LoadConst>(none_const, Type::fromObject(Py_None));
    Register* is_none = env.AllocateRegister();
    bb_left->append<PrimitiveCompare>(
        is_none, PrimitiveCompareOp::kEqual, left_child, none_const);
    bb_left->append<CondBranch>(is_none, bb_no_left, bb_check_null_left);
  }

  // bb_check_null_left: check for nullptr (also means no child)
  {
    Register* kZero = env.AllocateRegister();
    bb_check_null_left->append<LoadConst>(kZero, Type::fromCInt(0, TCInt64));
    // Reuse left_child — but we don't have it here. Instead rely on the fact
    // that if left_child != None it might still be nullptr (null ptr).
    // We re-load current and left_child to check for null.
    Register* current2 = env.AllocateRegister();
    bb_check_null_left->append<LoadCurrentNode>(current2);
    Register* left_child2 = env.AllocateRegister();
    bb_check_null_left->append<LoadField>(
        left_child2, current2, "left", match.left_offset, TOptObject);
    // If left_child is null (0), go to no_left
    bb_check_null_left->append<CondBranch>(left_child2, bb_has_left, bb_no_left);
  }

  // bb_no_left: set phase to kYield and loop
  {
    Register* kYield_r = emitPhaseConst(bb_no_left, env, TreeIterPhase::kYield);
    bb_no_left->append<SavePhase>(kYield_r);
    bb_no_left->append<Branch>(bb_loop);
  }

  // bb_has_left: push (current, kYield) and enter left child
  {
    // Re-load current and left_child for the state transition.
    Register* current = env.AllocateRegister();
    bb_has_left->append<LoadCurrentNode>(current);
    Register* left_child = env.AllocateRegister();
    bb_has_left->append<LoadField>(
        left_child, current, "left", match.left_offset, TOptObject);

    Register* kYield_r = emitPhaseConst(bb_has_left, env, TreeIterPhase::kYield);
    Register* check_status = env.AllocateRegister();
    bb_has_left->append<CheckTreeIterChildEntry>(check_status, left_child, fs);
    Register* push_status = env.AllocateRegister();
    bb_has_left->append<StateStackPush>(push_status, current, kYield_r, fs);
    bb_has_left->append<TreeIterEnterChild>(left_child);
    bb_has_left->append<SaveCurrentNode>(left_child);
    Register* kLeft_r = emitPhaseConst(bb_has_left, env, TreeIterPhase::kLeft);
    bb_has_left->append<SavePhase>(kLeft_r);
    bb_has_left->append<Branch>(bb_loop);
  }

  // -----------------------------------------------------------------------
  // 6. bb_yield (YIELD phase): load value, yield, set phase to kRight.
  // -----------------------------------------------------------------------
  {
    Register* current = env.AllocateRegister();
    bb_yield->append<LoadCurrentNode>(current);
    Register* value = env.AllocateRegister();
    bb_yield->append<LoadField>(
        value, current, "value", match.value_offset, TObject);

    Register* yield_dst = env.AllocateRegister();
    bb_yield->append<YieldValue>(yield_dst, value, fs);

    Register* kRight_r = emitPhaseConst(bb_yield, env, TreeIterPhase::kRight);
    bb_yield->append<SavePhase>(kRight_r);
    bb_yield->append<Branch>(bb_loop);
  }

  // -----------------------------------------------------------------------
  // 7. bb_right (RIGHT phase): load right child, branch on None.
  // -----------------------------------------------------------------------
  {
    Register* current = env.AllocateRegister();
    bb_right->append<LoadCurrentNode>(current);
    Register* right_child = env.AllocateRegister();
    bb_right->append<LoadField>(
        right_child, current, "right", match.right_offset, TOptObject);

    Register* none_const = env.AllocateRegister();
    bb_right->append<LoadConst>(none_const, Type::fromObject(Py_None));
    Register* is_none = env.AllocateRegister();
    bb_right->append<PrimitiveCompare>(
        is_none, PrimitiveCompareOp::kEqual, right_child, none_const);
    bb_right->append<CondBranch>(is_none, bb_no_right, bb_check_null_right);
  }

  // bb_check_null_right
  {
    Register* current2 = env.AllocateRegister();
    bb_check_null_right->append<LoadCurrentNode>(current2);
    Register* right_child2 = env.AllocateRegister();
    bb_check_null_right->append<LoadField>(
        right_child2, current2, "right", match.right_offset, TOptObject);
    bb_check_null_right->append<CondBranch>(
        right_child2, bb_has_right, bb_no_right);
  }

  // bb_no_right: phase = kBacktrack, loop
  {
    Register* kBack_r = emitPhaseConst(bb_no_right, env, TreeIterPhase::kBacktrack);
    bb_no_right->append<SavePhase>(kBack_r);
    bb_no_right->append<Branch>(bb_loop);
  }

  // bb_has_right: push (current, kExit) and enter right child
  {
    Register* current = env.AllocateRegister();
    bb_has_right->append<LoadCurrentNode>(current);
    Register* right_child = env.AllocateRegister();
    bb_has_right->append<LoadField>(
        right_child, current, "right", match.right_offset, TOptObject);

    Register* kExit_r = emitPhaseConst(bb_has_right, env, TreeIterPhase::kExit);
    Register* check_status = env.AllocateRegister();
    bb_has_right->append<CheckTreeIterChildEntry>(check_status, right_child, fs);
    Register* push_status = env.AllocateRegister();
    bb_has_right->append<StateStackPush>(push_status, current, kExit_r, fs);
    bb_has_right->append<TreeIterEnterChild>(right_child);
    bb_has_right->append<SaveCurrentNode>(right_child);
    Register* kLeft_r = emitPhaseConst(bb_has_right, env, TreeIterPhase::kLeft);
    bb_has_right->append<SavePhase>(kLeft_r);
    bb_has_right->append<Branch>(bb_loop);
  }

  // -----------------------------------------------------------------------
  // 8. bb_backtrack (BACKTRACK phase): empty stack → done, else pop.
  // -----------------------------------------------------------------------
  {
    Register* stack_top = env.AllocateRegister();
    bb_backtrack->append<LoadStackTop>(stack_top);
    bb_backtrack->append<CondBranch>(stack_top, bb_pop, bb_done);
  }

  // bb_pop: leave current, pop, restore, loop
  {
    bb_pop->append<TreeIterLeaveCurrentNode>();
    Register* pop_node = env.AllocateRegister();
    bb_pop->append<StateStackPop>(pop_node);
    Register* pop_phase = env.AllocateRegister();
    bb_pop->append<LoadPoppedPhase>(pop_phase);
    bb_pop->append<SaveCurrentNode>(pop_node);
    bb_pop->append<SavePhase>(pop_phase);
    bb_pop->append<Branch>(bb_loop);
  }

  // -----------------------------------------------------------------------
  // 9. bb_exit (EXIT phase): leave current node, phase=kBacktrack, loop.
  // -----------------------------------------------------------------------
  {
    bb_exit->append<TreeIterLeaveCurrentNode>();
    Register* kBack_r = emitPhaseConst(bb_exit, env, TreeIterPhase::kBacktrack);
    bb_exit->append<SavePhase>(kBack_r);
    bb_exit->append<Branch>(bb_loop);
  }

  // -----------------------------------------------------------------------
  // 10. bb_done: clear state, return None.
  // -----------------------------------------------------------------------
  {
    bb_done->append<TreeIterLeaveCurrentNode>();
    bb_done->append<ClearTreeIterState>();
    Register* none_r = env.AllocateRegister();
    bb_done->append<LoadConst>(none_r, Type::fromObject(Py_None));
    bb_done->append<Return>(none_r);
  }

  // -----------------------------------------------------------------------
  // 11. Clean up unreachable blocks (the original recursive body) and
  //     re-derive register types.
  // -----------------------------------------------------------------------
  CleanCFG{}.Run(func);
  reflowTypes(func);
}

// ---------------------------------------------------------------------------
// TreeIterStateMachinePass::Run
// ---------------------------------------------------------------------------

void TreeIterStateMachinePass::Run(Function& func) {
  if (!getConfig().hir_opts.tree_iter_state_machine) {
    return;
  }

  auto match = matchTreeIter(func);
  if (!match.has_value()) {
    return;
  }

  JIT_DLOG(
      "TreeIter matcher: matched {} (left_offset={}, right_offset={}, "
      "value_offset={})",
      func.fullname,
      match->left_offset,
      match->right_offset,
      match->value_offset);

  buildTreeIterStateMachine(func, *match);
}

} // namespace jit::hir
