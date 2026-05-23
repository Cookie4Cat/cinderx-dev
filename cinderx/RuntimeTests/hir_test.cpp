// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/python.h"

#include <gtest/gtest.h>
#include <string>

#include "cinderx/Common/ref.h"
#include "cinderx/Interpreter/cinder_opcode.h"
#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/hir/builder.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/parser.h"
#include "cinderx/Jit/hir/phi_elimination.h"
#include "cinderx/Jit/hir/printer.h"
#include "cinderx/Jit/hir/refcount_insertion.h"
#include "cinderx/Jit/hir/ssa.h"
#include "cinderx/RuntimeTests/fixtures.h"

extern "C" {
#if PY_VERSION_HEX >= 0x030C0000
#include "internal/pycore_intrinsics.h"
#endif
}

using namespace jit;
using namespace jit::hir;

HIRPrinter fullPrinter() {
  return HIRPrinter{}.setFullSnapshots(true);
}

size_t countSubstring(const std::string& haystack, const std::string& needle) {
  size_t count = 0;
  size_t pos = 0;
  while ((pos = haystack.find(needle, pos)) != std::string::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}

size_t countOpcode(const Function& func, Opcode opcode) {
  size_t count = 0;
  for (const auto& block : func.cfg.blocks) {
    for (const auto& instr : block) {
      if (instr.opcode() == opcode) {
        count++;
      }
    }
  }
  return count;
}

TEST(BasicBlockTest, CanAppendInstrs) {
  Environment env;
  BasicBlock block;
  auto v0 = env.AllocateRegister();
  block.append<LoadConst>(v0, TNoneType);
  block.append<Return>(v0);
  ASSERT_TRUE(block.GetTerminator()->IsReturn());
}

TEST(BasicBlockTest, CanIterateInstrs) {
  Environment env;
  BasicBlock block;
  auto v0 = env.AllocateRegister();
  block.append<LoadConst>(v0, TNoneType);
  block.append<Return>(v0);

  auto it = block.begin();
  ASSERT_TRUE(it->IsLoadConst());
  it++;
  ASSERT_TRUE(it->IsReturn());
  it++;
  ASSERT_TRUE(it == block.end());
}

TEST(BasicBlockTest, SplitAfterSplitsBlockAfterInstruction) {
  Environment env;
  CFG cfg;
  BasicBlock* head = cfg.AllocateBlock();
  auto v0 = env.AllocateRegister();
  head->append<LoadConst>(v0, TNoneType);
  Instr* load_const = head->GetTerminator();
  head->append<Return>(v0);
  BasicBlock* tail = cfg.splitAfter(*load_const);
  ASSERT_NE(nullptr, head->GetTerminator());
  EXPECT_TRUE(head->GetTerminator()->IsLoadConst());
  ASSERT_NE(nullptr, tail->GetTerminator());
  EXPECT_TRUE(tail->GetTerminator()->IsReturn());
}

TEST(CFGIterTest, IteratingEmptyCFGReturnsEmptyTraversal) {
  CFG cfg;
  std::vector<BasicBlock*> traversal = cfg.GetRPOTraversal();
  ASSERT_EQ(traversal.size(), 0);
}

TEST(CFGIterTest, IteratingSingleBlockCFGReturnsOneBlock) {
  Environment env;
  CFG cfg;
  BasicBlock* block = cfg.AllocateBlock();
  cfg.entry_block = block;

  // Add a single instuction to the block
  block->append<Return>(env.AllocateRegister());

  std::vector<BasicBlock*> traversal = cfg.GetRPOTraversal();
  ASSERT_EQ(traversal.size(), 1) << "Incorrect number of blocks returned";
  ASSERT_EQ(traversal[0], block) << "Incorrect block returned";
}

TEST(CFGIterTest, VisitsBlocksOnlyOnce) {
  CFG cfg;
  BasicBlock* block = cfg.AllocateBlock();
  cfg.entry_block = block;

  // The block loops on itself
  block->append<Branch>(block);

  std::vector<BasicBlock*> traversal = cfg.GetRPOTraversal();
  ASSERT_EQ(traversal.size(), 1) << "Incorrect number of blocks returned";
  ASSERT_EQ(traversal[0], block) << "Incorrect block returned";
}

TEST(CFGIterTest, VisitsAllBranches) {
  Environment env;
  CFG cfg;
  BasicBlock* cond = cfg.AllocateBlock();
  cfg.entry_block = cond;

  BasicBlock* true_block = cfg.AllocateBlock();
  true_block->append<Return>(env.AllocateRegister());

  BasicBlock* false_block = cfg.AllocateBlock();
  false_block->append<Return>(env.AllocateRegister());

  cond->append<CondBranch>(env.AllocateRegister(), true_block, false_block);

  std::vector<BasicBlock*> traversal = cfg.GetRPOTraversal();
  ASSERT_EQ(traversal.size(), 3) << "Incorrect number of blocks returned";
  ASSERT_EQ(traversal[0], cond) << "Should have visited cond block first";
  ASSERT_EQ(traversal[1], true_block)
      << "Should have visited true block second";
  ASSERT_EQ(traversal[2], false_block)
      << "Should have visited false block last";
}

TEST(CFGIterTest, VisitsLoops) {
  Environment env;
  CFG cfg;

  // Create the else block
  BasicBlock* outer_else = cfg.AllocateBlock();
  outer_else->append<Return>(env.AllocateRegister());

  // Create the inner loop
  BasicBlock* loop_cond = cfg.AllocateBlock();
  BasicBlock* loop_body = cfg.AllocateBlock();
  loop_body->append<Branch>(loop_cond);
  loop_cond->append<CondBranch>(env.AllocateRegister(), loop_body, outer_else);

  // Create the outer conditional
  BasicBlock* outer_cond = cfg.AllocateBlock();
  outer_cond->append<CondBranch>(env.AllocateRegister(), loop_cond, outer_else);
  cfg.entry_block = outer_cond;

  std::vector<BasicBlock*> traversal = cfg.GetRPOTraversal();
  ASSERT_EQ(traversal.size(), 4) << "Incorrect number of blocks returned";
  ASSERT_EQ(traversal[0], outer_cond) << "Should have visited outer cond first";
  ASSERT_EQ(traversal[1], loop_cond) << "Should have visited loop cond second";
  ASSERT_EQ(traversal[2], loop_body) << "Should have visited loop body third";
  ASSERT_EQ(traversal[3], outer_else) << "Should have visited else block last";
}

TEST(SplitCriticalEdgesTest, SplitsCriticalEdges) {
  auto hir_source = R"(
fun test {
  bb 0 {
    v0 = LoadConst<NoneType>
    CondBranch<1, 2> v0
  }
  bb 1 {
    v1 = LoadConst<NoneType>
    Branch<2>
  }
  bb 2 {
    v2 = Phi<0, 1> v0 v1
    CondBranch<3, 5> v2
  }
  bb 3 {
    Branch<5>
  }
  bb 5 {
    Return v2
  }
}
)";
  auto func = HIRParser{}.ParseHIR(hir_source);
  ASSERT_NE(func, nullptr);
  ASSERT_TRUE(checkFunc(*func, std::cout));

  func->cfg.splitCriticalEdges();
  const char* expected_hir = R"(fun test {
  bb 0 {
    v0 = LoadConst<NoneType>
    CondBranch<1, 5> v0
  }

  bb 1 (preds 0) {
    v1 = LoadConst<NoneType>
    Branch<2>
  }

  bb 5 (preds 0) {
    Branch<2>
  }

  bb 2 (preds 1, 5) {
    v2 = Phi<1, 5> v1 v0
    CondBranch<3, 6> v2
  }

  bb 3 (preds 2) {
    Branch<5>
  }

  bb 6 (preds 2) {
    Branch<5>
  }

  bb 5 (preds 3, 6) {
    Return v2
  }
}
)";
  EXPECT_EQ(HIRPrinter{}.ToString(*func), expected_hir);
}

TEST(RemoveTrampolineBlocksTest, DoesntModifySingleBlockLoops) {
  CFG cfg;
  Environment env;

  cfg.entry_block = cfg.AllocateBlock();
  cfg.entry_block->append<Branch>(cfg.entry_block);

  removeTrampolineBlocks(&cfg);

  auto s = HIRPrinter().ToString(cfg);
  const char* expected = R"(bb 0 (preds 0) {
  Branch<0>
}
)";
  ASSERT_EQ(s, expected);
}

TEST(RemoveTrampolineBlocksTest, ReducesSimpleLoops) {
  CFG cfg;
  Environment env;

  auto t1 = cfg.AllocateBlock();
  cfg.entry_block = cfg.AllocateBlock();
  cfg.entry_block->append<Branch>(t1);
  t1->append<Branch>(cfg.entry_block);

  removeTrampolineBlocks(&cfg);

  auto s = HIRPrinter().ToString(cfg);
  const char* expected = R"(bb 1 (preds 1) {
  Branch<1>
}
)";
  ASSERT_EQ(s, expected);
}

TEST(RemoveTrampolineBlocksTest, RemovesSimpleChain) {
  CFG cfg;
  Environment env;

  // This constructs a CFG that looks like
  //
  // entry -> t2 -> t1 -> exit
  //
  // after removing tramponline blocks we should be left
  // with only the exit block
  auto exit_block = cfg.AllocateBlock();
  exit_block->append<Return>(env.AllocateRegister());

  auto t1 = cfg.AllocateBlock();
  t1->append<Branch>(exit_block);

  auto t2 = cfg.AllocateBlock();
  t2->append<Branch>(t1);

  cfg.entry_block = cfg.AllocateBlock();
  cfg.entry_block->append<Branch>(t2);

  removeTrampolineBlocks(&cfg);

  auto s = HIRPrinter().ToString(cfg);
  auto expected = R"(bb 0 {
  Return v0
}
)";
  ASSERT_EQ(s, expected);
}

TEST(RemoveTrampolineBlocksTest, ReducesLoops) {
  CFG cfg;
  Environment env;

  // This constructs a CFG that look like
  //
  //              entry
  //                |
  //   +--- true ---+--- false ---+
  //   |                          |
  //  exit                        1->2->3->4-+
  //                                 ^       |
  //                                 |       |
  //                                 +-------+
  //
  // the loop of trampoline blocks on the right should be
  // reduced to a single block that loops back on itself:
  //
  //              entry
  //                |
  //   +--- true ---+--- false ---+
  //   |                          |
  //  exit                        4--+
  //                              ^  |
  //                              |  |
  //                              +--+
  Register* v0 = env.AllocateRegister();
  auto exit_block = cfg.AllocateBlock();
  exit_block->append<Return>(v0);

  auto t1 = cfg.AllocateBlock();
  auto t2 = cfg.AllocateBlock();
  auto t3 = cfg.AllocateBlock();
  auto t4 = cfg.AllocateBlock();
  t1->append<Branch>(t2);
  t2->append<Branch>(t3);
  t3->append<Branch>(t4);
  t4->append<Branch>(t2);

  cfg.entry_block = cfg.AllocateBlock();
  cfg.entry_block->append<CondBranch>(v0, exit_block, t1);

  removeTrampolineBlocks(&cfg);

  auto after = HIRPrinter().ToString(cfg);
  const char* expected = R"(bb 5 {
  CondBranch<0, 4> v0
}

bb 0 (preds 5) {
  Return v0
}

bb 4 (preds 4, 5) {
  Branch<4>
}
)";
  ASSERT_EQ(after, expected);
}

TEST(RemoveTrampolineBlocksTest, UpdatesAllPredecessors) {
  CFG cfg;
  Environment env;

  // This constructs a CFG that look like
  //
  //              entry
  //                |
  //   +--- true ---+--- false ---+
  //   |                          |
  //   4                          3
  //   |                          |
  //   +----------->2<------------+
  //                |
  //                v
  //                1
  //                |
  //                v
  //               exit
  //
  // After removing trampoline blocks this should look like
  //
  //              entry
  //                |
  //                v
  //               exit
  Register* v0 = env.AllocateRegister();
  auto exit_block = cfg.AllocateBlock();
  exit_block->append<Return>(v0);

  auto t1 = cfg.AllocateBlock();
  t1->append<Branch>(exit_block);

  auto t2 = cfg.AllocateBlock();
  t2->append<Branch>(t1);

  auto t3 = cfg.AllocateBlock();
  t3->append<Branch>(t2);

  auto t4 = cfg.AllocateBlock();
  t4->append<Branch>(t2);

  cfg.entry_block = cfg.AllocateBlock();
  cfg.entry_block->append<CondBranch>(v0, t4, t3);

  removeTrampolineBlocks(&cfg);

  auto after = HIRPrinter().ToString(cfg);
  const char* expected = R"(bb 5 {
  Branch<0>
}

bb 0 (preds 5) {
  Return v0
}
)";
  ASSERT_EQ(after, expected);
}

TEST(RemoveUnreachableBlocks, RemovesTransitivelyUnreachableBlocks) {
  const char* hir = R"(
fun foo {
  bb 0 {
    Branch<1>
  }

  bb 2 {
    Branch<2>
  }

  bb 3 {
    Branch<2>
  }

  bb 1 {
    v0 = LoadConst<NoneType>
    Return v0
  }

  bb 12 {
    Branch<11>
  }

  bb 11 {
    v1 = LoadConst<NoneType>
    Return v1
  }

  bb 4 {
    Branch<2>
  }

  bb 10 {
    Branch<1>
  }
}
)";

  std::unique_ptr<Function> func = HIRParser{}.ParseHIR(hir);
  ASSERT_NE(func, nullptr);

  removeUnreachableBlocks(*func);

  const char* expected = R"(fun foo {
  bb 0 {
    Branch<1>
  }

  bb 1 (preds 0) {
    v0 = LoadConst<NoneType>
    Return v0
  }
}
)";
  EXPECT_EQ(HIRPrinter{}.ToString(*func), expected);
}

TEST(RemoveUnreachableBlocks, FixesPhisOfReachableBlocks) {
  const char* hir = R"(
fun foo {
  bb 0 {
    v0 = LoadConst<NoneType>
    CondBranch<1, 3> v0
  }

  bb 1 {
    v1 = LoadConst<NoneType>
    Branch<3>
  }

  bb 2 {
    v2 = LoadConst<NoneType>
    Branch<3>
  }

  bb 3 {
    v3 = Phi<0, 1, 2> v0 v1 v2
    Return v3
  }
}
)";

  std::unique_ptr<Function> func = HIRParser{}.ParseHIR(hir);
  ASSERT_NE(func, nullptr);

  removeUnreachableBlocks(*func);

  const char* expected = R"(fun foo {
  bb 0 {
    v0 = LoadConst<NoneType>
    CondBranch<1, 3> v0
  }

  bb 1 (preds 0) {
    v1 = LoadConst<NoneType>
    Branch<3>
  }

  bb 3 (preds 0, 1) {
    v3 = Phi<0, 1> v0 v1
    Return v3
  }
}
)";
  EXPECT_EQ(HIRPrinter{}.ToString(*func), expected);
}

template <class T>
Ref<> toByteString(T&& data) {
  auto sp = std::span{data};
  return Ref<>::steal(PyBytes_FromStringAndSize(
      reinterpret_cast<const char*>(sp.data()), sp.size_bytes()));
}

class HIRBuildTest : public RuntimeTest {
 public:
  template <class T>
  std::unique_ptr<Function> build_test(
      T&& bc,
      const std::vector<PyObject*>& locals /* borrowed */) {
    Ref<> bytecode = toByteString(std::span{std::forward<T>(bc)});
    assert(bytecode.get());
    const int nlocals = locals.size();

    auto filename = Ref<>::steal(PyUnicode_FromString("filename"));
    auto funcname = Ref<>::steal(PyUnicode_FromString("funcname"));
    auto consts = Ref<>::steal(PyTuple_New(nlocals));
    auto varnames = Ref<>::steal(PyTuple_New(nlocals));
    for (int i = 0; i < nlocals; i++) {
      PyObject* local = locals.at(i);
      Py_INCREF(local);
      PyTuple_SET_ITEM(consts.get(), i, local);
      PyTuple_SET_ITEM(
          varnames.get(),
          i,
          PyUnicode_FromString(fmt::format("param{}", i).c_str()));
    }

    auto empty_tuple = Ref<>::steal(PyTuple_New(0));
    auto empty_bytes = Ref<>::steal(PyBytes_FromString(""));
    auto code = Ref<PyCodeObject>::steal(PyUnstable_Code_New(
        /*argcount=*/1,
        /*kwonlyargcount*/ 0,
        /*nlocals=*/nlocals,
        /*stacksize=*/0,
        /*flags=*/0,
        bytecode,
        consts,
        /*names=*/empty_tuple,
        varnames,
        /*freevars=*/empty_tuple,
        /*cellvars=*/empty_tuple,
        filename,
        funcname,
        /*_unused_qualname=*/funcname,
        /*firstlineno=*/0,
        /*linetable=*/empty_bytes,
        /*_unused_exceptiontable=*/empty_bytes));
    assert(code != nullptr);

    auto func =
        Ref<PyFunctionObject>::steal(PyFunction_New(code, MakeGlobals()));
    assert(func != nullptr);

    return buildHIR(func);
  }
};

TEST_F(HIRBuildTest, ExactIntGlobalLoadUsesGuardType) {
  const char* src = R"(
G = 257

def test():
    return G
)";
  std::unique_ptr<Function> irfunc;
  ASSERT_NO_FATAL_FAILURE(CompileToHIR(src, "test", irfunc));
  ASSERT_NE(irfunc, nullptr);

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countSubstring(hir, "GuardType<LongExact>"), 1) << hir;
  EXPECT_EQ(countSubstring(hir, "GuardIs<"), 0) << hir;
}

TEST_F(HIRBuildTest, ImmortalExactIntGlobalLoadUsesGuardType) {
  const char* src = R"(
G = 24

def test():
    return G
)";
  std::unique_ptr<Function> irfunc;
  ASSERT_NO_FATAL_FAILURE(CompileToHIR(src, "test", irfunc));
  ASSERT_NE(irfunc, nullptr);

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countSubstring(hir, "GuardType<LongExact>"), 1) << hir;
  EXPECT_EQ(countSubstring(hir, "GuardIs<"), 0) << hir;
}

TEST_F(HIRBuildTest, NonExactIntGlobalLoadKeepsGuardIs) {
  const char* src = R"(
G = True

def test():
    return G
)";
  std::unique_ptr<Function> irfunc;
  ASSERT_NO_FATAL_FAILURE(CompileToHIR(src, "test", irfunc));
  ASSERT_NE(irfunc, nullptr);

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countSubstring(hir, "GuardType<LongExact>"), 0) << hir;
  EXPECT_EQ(countSubstring(hir, "GuardIs<"), 1) << hir;
}

TEST_F(HIRBuildTest, IntSubclassGlobalLoadKeepsGuardIs) {
  const char* src = R"(
class MyInt(int):
    pass

G = MyInt(257)

def test():
    return G
)";
  std::unique_ptr<Function> irfunc;
  ASSERT_NO_FATAL_FAILURE(CompileToHIR(src, "test", irfunc));
  ASSERT_NE(irfunc, nullptr);

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countSubstring(hir, "GuardType<LongExact>"), 0) << hir;
  EXPECT_EQ(countSubstring(hir, "GuardIs<"), 1) << hir;
}

TEST_F(HIRBuildTest, FloatGlobalLoadKeepsGuardIs) {
  const char* src = R"(
G = 1.5

def test():
    return G
)";
  std::unique_ptr<Function> irfunc;
  ASSERT_NO_FATAL_FAILURE(CompileToHIR(src, "test", irfunc));
  ASSERT_NE(irfunc, nullptr);

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countSubstring(hir, "GuardType<LongExact>"), 0) << hir;
  EXPECT_EQ(countSubstring(hir, "GuardIs<"), 1) << hir;
}

TEST_F(HIRBuildTest, UnicodeGlobalLoadKeepsGuardIs) {
  const char* src = R"(
G = "foo"

def test():
    return G
)";
  std::unique_ptr<Function> irfunc;
  ASSERT_NO_FATAL_FAILURE(CompileToHIR(src, "test", irfunc));
  ASSERT_NE(irfunc, nullptr);

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countSubstring(hir, "GuardType<LongExact>"), 0) << hir;
  EXPECT_EQ(countSubstring(hir, "GuardIs<"), 1) << hir;
}

TEST_F(HIRBuildTest, BuiltinFunctionGlobalLoadKeepsGuardIs) {
  const char* src = R"(
def test():
    return len
)";
  std::unique_ptr<Function> irfunc;
  ASSERT_NO_FATAL_FAILURE(CompileToHIR(src, "test", irfunc));
  ASSERT_NE(irfunc, nullptr);

  std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_EQ(countSubstring(hir, "GuardType<LongExact>"), 0) << hir;
  EXPECT_EQ(countSubstring(hir, "GuardIs<"), 1) << hir;
}

TEST_F(HIRBuildTest, GetLength) {
  //  0 LOAD_FAST  0
  //  2 GET_LENGTH
  //  4 RETURN_VALUE
  uint8_t bc[] = {LOAD_FAST, 0, GET_LEN, 0, RETURN_VALUE, 0};
  std::unique_ptr<Function> irfunc = build_test(bc, {Py_None});

  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadArg<0; "param0">
    v1 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<1> v0
    }
    v2 = GetLength v0 {
      FrameState {
        CurInstrOffset 2
        Locals<1> v0
        Stack<1> v0
      }
    }
    Snapshot {
      CurInstrOffset 4
      Locals<1> v0
      Stack<2> v0 v2
    }
    v3 = Assign v2
    v2 = Assign v0
    Return v3
  }
}
)";
  EXPECT_EQ(fullPrinter().ToString(*(irfunc)), expected);
}

#ifndef Py_GIL_DISABLED
TEST_F(HIRBuildTest, TupleSpecializedUnpackKeepsListFastPath) {
  const char* src = R"(
T = (1, 2, 3)

def test(seq):
    a, b, c = seq
    return a + b + c
)";

  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func.get(), nullptr);
  Ref<> tuple(getGlobal("T"));
  ASSERT_NE(tuple.get(), nullptr);

  for (int i = 0; i < 100; i++) {
    auto result = Ref<>::steal(PyObject_CallFunctionObjArgs(
        reinterpret_cast<PyObject*>(func.get()), tuple.get(), nullptr));
    ASSERT_NE(result.get(), nullptr);
    ASSERT_TRUE(isIntEquals(result, 6));
  }

  std::unique_ptr<Function> irfunc(buildHIR(func));
  ASSERT_NE(irfunc, nullptr);

  EXPECT_EQ(countOpcode(*irfunc, Opcode::kUnpackSequence), 0);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kReserveStack), 0);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kCondBranchCheckType), 2);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kLoadFieldAddress), 1);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kLoadField), 1);
}

TEST_F(HIRBuildTest, ListSpecializedUnpackKeepsTupleFastPath) {
  const char* src = R"(
L = [1, 2, 3]

def test(seq):
    a, b, c = seq
    return a + b + c
)";

  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func.get(), nullptr);
  Ref<> list(getGlobal("L"));
  ASSERT_NE(list.get(), nullptr);

  for (int i = 0; i < 100; i++) {
    auto result = Ref<>::steal(PyObject_CallFunctionObjArgs(
        reinterpret_cast<PyObject*>(func.get()), list.get(), nullptr));
    ASSERT_NE(result.get(), nullptr);
    ASSERT_TRUE(isIntEquals(result, 6));
  }

  std::unique_ptr<Function> irfunc(buildHIR(func));
  ASSERT_NE(irfunc, nullptr);

  EXPECT_EQ(countOpcode(*irfunc, Opcode::kUnpackSequence), 0);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kReserveStack), 0);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kCondBranchCheckType), 2);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kLoadField), 1);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kLoadFieldAddress), 1);
}

TEST_F(HIRBuildTest, TwoTupleSpecializedUnpackKeepsListFastPath) {
  const char* src = R"(
T = (1, 2)

def test(seq):
    a, b = seq
    return a + b
)";

  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func.get(), nullptr);
  Ref<> tuple(getGlobal("T"));
  ASSERT_NE(tuple.get(), nullptr);

  for (int i = 0; i < 100; i++) {
    auto result = Ref<>::steal(PyObject_CallFunctionObjArgs(
        reinterpret_cast<PyObject*>(func.get()), tuple.get(), nullptr));
    ASSERT_NE(result.get(), nullptr);
    ASSERT_TRUE(isIntEquals(result, 3));
  }

  std::unique_ptr<Function> irfunc(buildHIR(func));
  ASSERT_NE(irfunc, nullptr);

  EXPECT_EQ(countOpcode(*irfunc, Opcode::kUnpackSequence), 0);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kReserveStack), 0);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kCondBranchCheckType), 2);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kLoadFieldAddress), 1);
  EXPECT_EQ(countOpcode(*irfunc, Opcode::kLoadField), 1);
}
#endif

#if PY_VERSION_HEX < 0x030E0000
TEST_F(HIRBuildTest, LoadAssertionError) {
  // No LOAD_ASSERTION_ERROR on 3.14 and later
  //  0 LOAD_ASSERTION_ERROR
  //  2 RETURN_VALUE
  uint8_t bc[] = {LOAD_ASSERTION_ERROR, 0, RETURN_VALUE, 0};
  Ref<> bytecode = toByteString(bc);
  ASSERT_NE(bytecode.get(), nullptr);
  auto filename = Ref<>::steal(PyUnicode_FromString("filename"));
  auto funcname = Ref<>::steal(PyUnicode_FromString("funcname"));
  auto empty_tuple = Ref<>::steal(PyTuple_New(0));
  auto empty_bytes = Ref<>::steal(PyBytes_FromString(""));
  auto code = Ref<PyCodeObject>::steal(PyUnstable_Code_New(
      /*argcount=*/0,
      /*kwonlyargcount=*/0,
      /*nlocals=*/0,
      /*stacksize=*/0,
      /*flags=*/0,
      bytecode,
      /*consts=*/empty_tuple,
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
  ASSERT_NE(code.get(), nullptr);

  auto func = Ref<PyFunctionObject>::steal(PyFunction_New(code, MakeGlobals()));
  ASSERT_NE(func.get(), nullptr);

  std::unique_ptr<Function> irfunc(buildHIR(func));

  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
    }
    v1 = LoadConst<ImmortalTypeExact[AssertionError:obj]>
    Return v1
  }
}
)";
  EXPECT_EQ(fullPrinter().ToString(*(irfunc)), expected);
}
#endif

TEST_F(HIRBuildTest, SetUpdate) {
  //  0 LOAD_FAST    0
  //  2 LOAD_FAST    1
  //  4 LOAD_FAST    2
  //  6 SET_UPDATE   1
  //  8 ROT_TWO
  //  10 POP_TOP
  //  12 RETURN_VALUE
  uint8_t bc[] = {
      LOAD_FAST,
      0,
      LOAD_FAST,
      1,
      LOAD_FAST,
      2,
      SET_UPDATE,
      1,

      SWAP,
      2,
      POP_TOP,
      0,
      RETURN_VALUE,
      0,
  };
  Ref<> bytecode = toByteString(bc);
  ASSERT_NE(bytecode.get(), nullptr);
  auto filename = Ref<>::steal(PyUnicode_FromString("filename"));
  auto funcname = Ref<>::steal(PyUnicode_FromString("funcname"));
  auto empty_tuple = Ref<>::steal(PyTuple_New(0));
  auto param0 = Ref<>::steal(PyUnicode_FromString("param0"));
  auto param1 = Ref<>::steal(PyUnicode_FromString("param1"));
  auto param2 = Ref<>::steal(PyUnicode_FromString("param2"));
  auto varnames =
      Ref<>::steal(PyTuple_Pack(3, param0.get(), param1.get(), param2.get()));
  auto empty_bytes = Ref<>::steal(PyBytes_FromString(""));
  auto code = Ref<PyCodeObject>::steal(PyUnstable_Code_New(
      /*argcount=*/3,
      /*kwonlyargcount=*/0,
      /*nlocals=*/3,
      /*stacksize=*/0,
      /*flags=*/0,
      bytecode,
      /*consts=*/empty_tuple,
      /*names=*/empty_tuple,
      varnames,
      /*freevars=*/empty_tuple,
      /*cellvars=*/empty_tuple,
      filename,
      funcname,
      /*_unused_qualname=*/funcname,
      /*firstlineno=*/0,
      /*linetable=*/empty_bytes,
      /*_unused_exceptiontable=*/empty_bytes));
  ASSERT_NE(code.get(), nullptr);

  auto func = Ref<PyFunctionObject>::steal(PyFunction_New(code, MakeGlobals()));
  ASSERT_NE(func.get(), nullptr);

  std::unique_ptr<Function> irfunc(buildHIR(func));

  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadArg<0; "param0">
    v1 = LoadArg<1; "param1">
    v2 = LoadArg<2; "param2">
    v3 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<3> v0 v1 v2
    }
    v4 = SetUpdate v1 v2 {
      FrameState {
        CurInstrOffset 6
        Locals<3> v0 v1 v2
        Stack<2> v0 v1
      }
    }
    Snapshot {
      CurInstrOffset 8
      Locals<3> v0 v1 v2
      Stack<2> v0 v1
    }
    Return v1
  }
}
)";
  EXPECT_EQ(fullPrinter().ToString(*(irfunc)), expected);
}

class EdgeCaseTest : public RuntimeTest {};

TEST_F(EdgeCaseTest, IgnoreUnreachableLoops) {
  //  0 LOAD_CONST    0
  //  2 RETURN_VALUE
  //
  //  4 LOAD_CONST    0
  //  6 RETURN_VALUE
  //  8 JUMP_ABSOLUTE 4
  uint8_t bc[] = {
      LOAD_CONST,
      0,
      RETURN_VALUE,
      0,
      LOAD_CONST,
      0,
      RETURN_VALUE,
      0,
      JUMP_BACKWARD,
      2,
#if PY_VERSION_HEX >= 0x030E0000
      // inline-cache slot for 3.14+
      0,
      0
#endif
  };
  Ref<> bytecode = toByteString(bc);
  ASSERT_NE(bytecode.get(), nullptr);
  auto filename = Ref<>::steal(PyUnicode_FromString("filename"));
  auto funcname = Ref<>::steal(PyUnicode_FromString("funcname"));
  auto consts = Ref<>::steal(PyTuple_New(1));
  Py_INCREF(Py_None);
  PyTuple_SET_ITEM(consts.get(), 0, Py_None);
  auto empty_tuple = Ref<>::steal(PyTuple_New(0));
  auto empty_bytes = Ref<>::steal(PyBytes_FromString(""));
  auto code = Ref<PyCodeObject>::steal(PyUnstable_Code_New(
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
  ASSERT_NE(code.get(), nullptr);

  auto func = Ref<PyFunctionObject>::steal(PyFunction_New(code, MakeGlobals()));
  ASSERT_NE(func.get(), nullptr);

  std::unique_ptr<Function> irfunc(buildHIR(func));
  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
    }
    v1 = LoadConst<ImmortalNoneType>
    Return v1
  }
}
)";
  EXPECT_EQ(fullPrinter().ToString(*(irfunc)), expected);
}

TEST_F(EdgeCaseTest, JumpBackwardNoInterrupt) {
  //  0 LOAD_CONST    0
  //  2 RETURN_VALUE
  //
  //  4 LOAD_CONST    0
  //  6 RETURN_VALUE
  //  8 JUMP_ABSOLUTE 4
  uint8_t bc[] = {
      LOAD_CONST,
      0,
      RETURN_VALUE,
      0,
      LOAD_CONST,
      0,
      RETURN_VALUE,
      0,
      JUMP_BACKWARD_NO_INTERRUPT,
      2,
  };
  Ref<> bytecode = toByteString(bc);
  ASSERT_NE(bytecode.get(), nullptr);
  auto filename = Ref<>::steal(PyUnicode_FromString("filename"));
  auto funcname = Ref<>::steal(PyUnicode_FromString("funcname"));
  auto consts = Ref<>::steal(PyTuple_New(1));
  Py_INCREF(Py_None);
  PyTuple_SET_ITEM(consts.get(), 0, Py_None);
  auto empty_tuple = Ref<>::steal(PyTuple_New(0));
  auto empty_bytes = Ref<>::steal(PyBytes_FromString(""));
  auto code = Ref<PyCodeObject>::steal(PyUnstable_Code_New(
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
  ASSERT_NE(code.get(), nullptr);

  auto func = Ref<PyFunctionObject>::steal(PyFunction_New(code, MakeGlobals()));
  ASSERT_NE(func.get(), nullptr);

  std::unique_ptr<Function> irfunc(buildHIR(func));
  ASSERT_NE(irfunc.get(), nullptr);
  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
    }
    v1 = LoadConst<ImmortalNoneType>
    Return v1
  }
}
)";
  EXPECT_EQ(fullPrinter().ToString(*(irfunc)), expected);
}

class CppInlinerTest : public RuntimeTest {};

TEST_F(CppInlinerTest, ChangingCalleeFunctionCodeCausesDeopt) {
  const char* pycode = R"(
def other():
  return 2

other_code = other.__code__

def g():
  return 1

def f():
  return g()
)";
  // Compile f
  Ref<PyObject> pyfunc(compileAndGet(pycode, "f"));
  ASSERT_NE(pyfunc, nullptr) << "Failed compiling func";
  // Call f
  auto empty_tuple = Ref<>::steal(PyTuple_New(0));
  auto call_result1 =
      Ref<>::steal(PyObject_Call(pyfunc, empty_tuple, /*kwargs=*/nullptr));
  EXPECT_TRUE(isIntEquals(call_result1, 1));
  // Set __code__
  Ref<PyObject> other_code(getGlobal("other_code"));
  ASSERT_NE(other_code, nullptr) << "Failed to get other_code global";
  int result = PyObject_SetAttrString(pyfunc, "__code__", other_code);
  ASSERT_NE(result, -1) << "Failed to set __code__";
  // Call f again
  auto call_result2 =
      Ref<>::steal(PyObject_Call(pyfunc, empty_tuple, /*kwargs=*/nullptr));
  EXPECT_TRUE(isIntEquals(call_result2, 2));
}

class HIRCloneTest : public RuntimeTest {};

TEST_F(HIRCloneTest, CanCloneInstrs) {
  Environment env;
  auto v0 = env.AllocateRegister();
  std::unique_ptr<Instr> load_const(
      LoadConst::create(v0, Type::fromObject(Py_False)));
  std::unique_ptr<Instr> new_load(load_const->clone());
  ASSERT_TRUE(new_load->IsLoadConst());
  EXPECT_TRUE(
      static_cast<LoadConst*>(new_load.get())->type() ==
      static_cast<LoadConst*>(load_const.get())->type());
  EXPECT_NE(load_const, new_load);
  EXPECT_EQ(load_const->output()->instr(), load_const.get());
  EXPECT_EQ(new_load->output()->instr(), load_const.get());
}

TEST_F(HIRCloneTest, CanCloneBranches) {
  Environment env;
  CFG cfg;
  BasicBlock* from = cfg.AllocateBlock();
  BasicBlock* to = cfg.AllocateBlock();
  cfg.entry_block = from;
  from->append<Branch>(to);
  Instr* branch = from->GetTerminator();
  std::unique_ptr<Instr> new_branch(branch->clone());
  ASSERT_TRUE(new_branch->IsBranch());
  EXPECT_EQ(branch->block(), from);
  EXPECT_EQ(new_branch->block(), nullptr);

  Edge* orig_edge = static_cast<Branch*>(branch)->edge(0);
  // Make sure that the two edges are different pointers with the same fields
  Edge* dup_edge = static_cast<Branch*>(new_branch.get())->edge(0);
  EXPECT_NE(orig_edge, dup_edge);

  EXPECT_EQ(orig_edge->from(), dup_edge->from());
  EXPECT_TRUE(from->out_edges().contains(orig_edge));
  EXPECT_TRUE(from->out_edges().contains(dup_edge));

  EXPECT_EQ(orig_edge->to(), dup_edge->to());
  EXPECT_TRUE(to->in_edges().contains(orig_edge));
  EXPECT_TRUE(to->in_edges().contains(dup_edge));
}

TEST_F(HIRCloneTest, CanCloneBorrwedRefFields) {
  Environment env;
  auto v0 = env.AllocateRegister();
  auto name = Ref<>::steal(PyUnicode_FromString("test"));
  std::unique_ptr<Instr> check(CheckVar::create(v0, v0, name));
  std::unique_ptr<Instr> new_check(check->clone());
  ASSERT_TRUE(new_check->IsCheckVar());
  BorrowedRef<> orig_name = static_cast<CheckVar*>(check.get())->name();
  BorrowedRef<> dup_name = static_cast<CheckVar*>(new_check.get())->name();
  EXPECT_EQ(orig_name, dup_name);
}

TEST_F(HIRCloneTest, CanCloneVariadicOpInstr) {
  Environment env;
  auto out = env.AllocateRegister();
  auto v0 = env.AllocateRegister();

  // Create a CallStatic with no arguments
  std::unique_ptr<Instr> call_static_no_args(
      CallStatic::create(0, out, nullptr, Type::fromObject(Py_None)));
  std::unique_ptr<Instr> new_call_static_no_args(call_static_no_args->clone());
  ASSERT_NE(call_static_no_args.get(), new_call_static_no_args.get());
  ASSERT_TRUE(new_call_static_no_args->IsCallStatic());

  CallStatic* orig_call = static_cast<CallStatic*>(call_static_no_args.get());
  CallStatic* dup_call =
      static_cast<CallStatic*>(new_call_static_no_args.get());
  EXPECT_EQ(orig_call->addr(), dup_call->addr());
  EXPECT_EQ(orig_call->ret_type(), dup_call->ret_type());

  // Create a CallStatic with one argument
  std::unique_ptr<Instr> call_static_one_arg(
      CallStatic::create(1, out, nullptr, Type::fromObject(Py_None), v0));
  std::unique_ptr<Instr> new_call_static_one_arg(call_static_one_arg->clone());
  ASSERT_NE(call_static_one_arg.get(), new_call_static_one_arg.get());
  ASSERT_TRUE(new_call_static_one_arg->IsCallStatic());

  orig_call = static_cast<CallStatic*>(call_static_one_arg.get());
  dup_call = static_cast<CallStatic*>(new_call_static_one_arg.get());
  EXPECT_EQ(orig_call->addr(), dup_call->addr());
  EXPECT_EQ(orig_call->ret_type(), dup_call->ret_type());
  EXPECT_EQ(orig_call->GetOperand(0), dup_call->GetOperand(0));

  // Create a CallStatic with two arguments
  std::unique_ptr<Instr> call_static_two_args(
      CallStatic::create(2, out, nullptr, Type::fromObject(Py_None), v0, v0));
  std::unique_ptr<Instr> new_call_static_two_args(
      call_static_two_args->clone());
  ASSERT_NE(call_static_two_args.get(), new_call_static_two_args.get());
  ASSERT_TRUE(new_call_static_two_args->IsCallStatic());

  orig_call = static_cast<CallStatic*>(call_static_two_args.get());
  dup_call = static_cast<CallStatic*>(new_call_static_two_args.get());
  EXPECT_EQ(orig_call->addr(), dup_call->addr());
  EXPECT_EQ(orig_call->ret_type(), dup_call->ret_type());
  EXPECT_EQ(orig_call->GetOperand(0), dup_call->GetOperand(0));
  EXPECT_EQ(orig_call->GetOperand(1), dup_call->GetOperand(1));
}

TEST_F(HIRCloneTest, CanCloneDeoptBase) {
  const char* hir = R"(fun jittestmodule:test {
  bb 0 {
    Snapshot {
      CurInstrOffset 0
      Locals<1> v0
    }
    v1 = LoadConst<ImmortalLongExact[1]>
    v0 = Assign v1
    v2 = LoadGlobal<0; "foo"> {
      FrameState {
        CurInstrOffset 6
        Locals<1> v0
      }
    }
    Snapshot {
      CurInstrOffset 6
      Locals<1> v0
      Stack<1> v2
    }
    Return v2
  }
}
)";
  auto irfunc = HIRParser().ParseHIR(hir);
  ASSERT_NE(irfunc, nullptr);
  ASSERT_TRUE(checkFunc(*irfunc, std::cout));
  reflowTypes(*irfunc);
  RefcountInsertion().Run(*irfunc);
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v1:ImmortalLongExact[1] = LoadConst<ImmortalLongExact[1]>
    v2:Object = LoadGlobal<0> {
      LiveValues<1> unc:v1
      FrameState {
        CurInstrOffset 6
        Locals<1> v1
      }
    }
    Return v2
  }
}
)";
  ASSERT_EQ(fullPrinter().ToString(*irfunc), expected);
  BasicBlock* bb0 = irfunc->cfg.entry_block;
  Instr& load_global = *(++(bb0->rbegin()));
  ASSERT_TRUE(load_global.IsLoadGlobal());

  std::unique_ptr<Instr> dup_load(load_global.clone());
  ASSERT_TRUE(dup_load->IsLoadGlobal());

  LoadGlobal* orig = static_cast<LoadGlobal*>(&load_global);
  LoadGlobal* dup = static_cast<LoadGlobal*>(dup_load.get());

  EXPECT_EQ(orig->output(), dup->output());
  EXPECT_EQ(orig->name_idx(), dup->name_idx());

  FrameState* orig_fs = orig->frameState();
  FrameState* dup_fs = dup->frameState();
  // Should not be pointer equal, but have equal contents
  EXPECT_NE(orig_fs, dup_fs);
  EXPECT_TRUE(*orig_fs == *dup_fs);

  // Should have equal contents
  EXPECT_TRUE(orig->live_regs() == dup->live_regs());
}

TEST_F(HIRBuildTest, MatchMapping) {
  uint8_t bc[] = {LOAD_FAST, 0, MATCH_MAPPING, 0, RETURN_VALUE, 0};
  std::unique_ptr<Function> irfunc = build_test(bc, {Py_None});

  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadArg<0; "param0">
    v1 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<1> v0
    }
    v2 = LoadField<ob_type@8, Type, borrowed> v0
    v3 = LoadField<tp_flags@168, CUInt64, borrowed> v2
    v4 = LoadConst<CUInt64[64]>
    v5 = IntBinaryOp<And> v3 v4
    CondBranch<1, 2> v5
  }

  bb 1 (preds 0) {
    v6 = LoadConst<ImmortalBool[True]>
    Branch<3>
  }

  bb 2 (preds 0) {
    v6 = LoadConst<ImmortalBool[False]>
    Branch<3>
  }

  bb 3 (preds 1, 2) {
    Snapshot {
      CurInstrOffset 4
      Locals<1> v0
      Stack<2> v0 v6
    }
    v2 = Assign v0
    Return v6
  }
}
)";
  EXPECT_EQ(fullPrinter().ToString(*(irfunc)), expected);
}

TEST_F(HIRBuildTest, MatchSequence) {
  uint8_t bc[] = {LOAD_FAST, 0, MATCH_SEQUENCE, 0, RETURN_VALUE, 0};
  std::unique_ptr<Function> irfunc = build_test(bc, {Py_None});

  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadArg<0; "param0">
    v1 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<1> v0
    }
    v2 = LoadField<ob_type@8, Type, borrowed> v0
    v3 = LoadField<tp_flags@168, CUInt64, borrowed> v2
    v4 = LoadConst<CUInt64[32]>
    v5 = IntBinaryOp<And> v3 v4
    CondBranch<1, 2> v5
  }

  bb 1 (preds 0) {
    v6 = LoadConst<ImmortalBool[True]>
    Branch<3>
  }

  bb 2 (preds 0) {
    v6 = LoadConst<ImmortalBool[False]>
    Branch<3>
  }

  bb 3 (preds 1, 2) {
    Snapshot {
      CurInstrOffset 4
      Locals<1> v0
      Stack<2> v0 v6
    }
    v2 = Assign v0
    Return v6
  }
}
)";
  EXPECT_EQ(fullPrinter().ToString(*(irfunc)), expected);
}

TEST_F(HIRBuildTest, MatchKeys) {
  uint8_t bc[] = {LOAD_FAST, 0, LOAD_FAST, 1, MATCH_KEYS, 0, RETURN_VALUE, 0};
  std::unique_ptr<Function> irfunc = build_test(bc, {Py_None, Py_None});

  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadArg<0; "param0">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = MatchKeys v0 v1 {
      FrameState {
        CurInstrOffset 4
        Locals<2> v0 v1
        Stack<2> v0 v1
      }
    }
    v4 = LoadConst<ImmortalNoneType>
    v5 = PrimitiveCompare<Equal> v3 v4
    CondBranch<1, 2> v5
  }

  bb 1 (preds 0) {
    v3 = RefineType<NoneType> v3
    Branch<3>
  }

  bb 2 (preds 0) {
    v3 = RefineType<TupleExact> v3
    Branch<3>
  }

  bb 3 (preds 1, 2) {
    Snapshot {
      CurInstrOffset 6
      Locals<2> v0 v1
      Stack<3> v0 v1 v3
    }
    v6 = Assign v3
    v3 = Assign v0
    v4 = Assign v1
    Return v6
  }
}
)";
  EXPECT_EQ(fullPrinter().ToString(*(irfunc)), expected);
}

TEST_F(HIRBuildTest, ListExtend) {
  uint8_t bc[] = {LOAD_FAST, 0, LOAD_FAST, 1, LIST_EXTEND, 1, RETURN_VALUE, 0};
  std::unique_ptr<Function> irfunc = build_test(bc, {Py_None, Py_None});

  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadArg<0; "param0">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = ListExtend v0 v1 {
      FrameState {
        CurInstrOffset 4
        Locals<2> v0 v1
        Stack<1> v0
      }
    }
    Snapshot {
      CurInstrOffset 6
      Locals<2> v0 v1
      Stack<1> v0
    }
    Return v0
  }
}
)";
  EXPECT_EQ(fullPrinter().ToString(*(irfunc)), expected);
}

TEST_F(HIRBuildTest, ListToTuple) {
  uint8_t bc[] = {
      LOAD_FAST, 0, CALL_INTRINSIC_1, INTRINSIC_LIST_TO_TUPLE, RETURN_VALUE, 0};
  std::unique_ptr<Function> irfunc = build_test(bc, {Py_None});

#if PY_VERSION_HEX >= 0x030E0000
  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadArg<0; "param0">
    v1 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<1> v0
    }
    v2 = CallIntrinsic<INTRINSIC_LIST_TO_TUPLE> v0
    Snapshot {
      CurInstrOffset 4
      Locals<1> v0
      Stack<1> v2
    }
    Return v2
  }
}
)";
#else
  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadArg<0; "param0">
    v1 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<1> v0
    }
    v2 = CallIntrinsic<6> v0
    Snapshot {
      CurInstrOffset 4
      Locals<1> v0
      Stack<1> v2
    }
    Return v2
  }
}
)";
#endif
  EXPECT_EQ(fullPrinter().ToString(*(irfunc)), expected);
}

#ifdef BINARY_OP_SUBSCR_DICT
TEST_F(HIRBuildTest, BinaryOpSubscrDictSpecializationGuards) {
  const char* src = R"(
def test(container, key):
    return container[key]

for _ in range(100):
    test({"a": "b"}, "a")
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func.get(), nullptr);

  std::unique_ptr<Function> irfunc(buildHIR(func));
  const std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_NE(hir.find("GuardType<DictExact>"), std::string::npos) << hir;
  EXPECT_NE(hir.find("BinaryOp<Subscript>"), std::string::npos) << hir;
}
#endif

#ifdef BINARY_OP_SUBSCR_LIST_INT
TEST_F(HIRBuildTest, BinaryOpSubscrListIntSpecializationGuards) {
  const char* src = R"(
def test(container, index):
    return container[index]

for _ in range(100):
    test(["a", "b"], 0)
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func.get(), nullptr);

  std::unique_ptr<Function> irfunc(buildHIR(func));
  const std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_NE(hir.find("GuardType<ListExact>"), std::string::npos) << hir;
  EXPECT_NE(hir.find("GuardType<LongExact>"), std::string::npos) << hir;
  EXPECT_NE(hir.find("BinaryOp<Subscript>"), std::string::npos) << hir;
}
#endif

#ifdef BINARY_OP_SUBSCR_TUPLE_INT
TEST_F(HIRBuildTest, BinaryOpSubscrTupleIntSpecializationGuards) {
  const char* src = R"(
def test(container, index):
    return container[index]

for _ in range(100):
    test(("a", "b"), 0)
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func.get(), nullptr);

  std::unique_ptr<Function> irfunc(buildHIR(func));
  const std::string hir = fullPrinter().ToString(*irfunc);
  EXPECT_NE(hir.find("GuardType<TupleExact>"), std::string::npos) << hir;
  EXPECT_NE(hir.find("GuardType<LongExact>"), std::string::npos) << hir;
  EXPECT_NE(hir.find("BinaryOp<Subscript>"), std::string::npos) << hir;
}
#endif

TEST_F(HIRBuildTest, LoadFastAndClear) {
  uint8_t bc[] = {
      LOAD_FAST_AND_CLEAR, 1, LOAD_FAST_CHECK, 0, POP_TOP, 0, RETURN_VALUE, 0};

  std::unique_ptr<Function> irfunc = build_test(bc, {Py_None, Py_None});

  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadArg<0; "param0">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = Assign v1
    v1 = LoadConst<Nullptr>
    v0 = CheckVar<"param0"> v0 {
      FrameState {
        CurInstrOffset 2
        Locals<2> v0 v1
        Stack<1> v3
      }
    }
    Return v3
  }
}
)";

  EXPECT_EQ(fullPrinter().ToString(*(irfunc)), expected);
}

TEST_F(HIRBuildTest, AtQuiescentStateInEvalBreakerCheck) {
  const char* src = R"(
def test():
    return 1
)";
  Ref<PyFunctionObject> funcobj(compileAndGet(src, "test"));
  ASSERT_NE(funcobj, nullptr);

  std::unique_ptr<Function> irfunc(buildHIR(funcobj));
  ASSERT_NE(irfunc, nullptr);

  bool found_at_quiescent_state = false;
  for (auto& block : irfunc->cfg.blocks) {
    for (auto& instr : block) {
      if (instr.IsAtQuiescentState()) {
        found_at_quiescent_state = true;
        break;
      }
    }
    if (found_at_quiescent_state) {
      break;
    }
  }

#ifdef Py_GIL_DISABLED
  EXPECT_TRUE(found_at_quiescent_state)
      << "AtQuiescentState should be present in free-threaded builds";
#else
  EXPECT_FALSE(found_at_quiescent_state)
      << "AtQuiescentState should not be present in non-free-threaded builds";
#endif
}

class HIRBuilderExtendedTest : public RuntimeTest {
 public:
  void SetUp() override {
    RuntimeTest::SetUp();
    jit_ctx_ = std::make_unique<jit::CompilerContext<jit::Compiler>>();
    ASSERT_NE(jit_ctx_, nullptr);
  }

  void TearDown() override {
    jit_ctx_.reset();
    RuntimeTest::TearDown();
  }

  std::unique_ptr<jit::CompilerContext<jit::Compiler>> jit_ctx_;
};

TEST_F(HIRBuilderExtendedTest, BuildSimpleFunc) {
  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);

  EXPECT_FALSE(irfunc->cfg.GetRPOTraversal().empty());
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithAdd) {
  const char* py_src = R"(
def add(a, b):
    return a + b
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "add"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithCompare) {
  const char* py_src = R"(
def cmp(a, b):
    return a > b
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "cmp"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithIf) {
  const char* py_src = R"(
def branch(x):
    if x > 0:
        return 1
    return 0
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "branch"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);

  auto traversal = irfunc->cfg.GetRPOTraversal();
  EXPECT_GE(traversal.size(), 2);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithLoop) {
  const char* py_src = R"(
def loop(n):
    total = 0
    for i in range(n):
        total += i
    return total
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "loop"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithAttrAccess) {
  const char* py_src = R"(
class MyClass:
    x = 10

def get_x():
    return MyClass.x
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "get_x"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithMethodCall) {
  const char* py_src = R"(
def call_method():
    return [1, 2, 3].append(4)
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "call_method"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithTuple) {
  const char* py_src = R"(
def make_tuple():
    return (1, 2, 3)
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "make_tuple"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithList) {
  const char* py_src = R"(
def make_list():
    return [1, 2, 3]
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "make_list"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithDict) {
  const char* py_src = R"(
def make_dict():
    return {"a": 1, "b": 2}
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "make_dict"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithSubscript) {
  const char* py_src = R"(
def get_item(lst, idx):
    return lst[idx]
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "get_item"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithUnaryOp) {
  const char* py_src = R"(
def negate(x):
    return -x
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "negate"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithNotOp) {
  const char* py_src = R"(
def not_op(x):
    return not x
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "not_op"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithBoolOp) {
  const char* py_src = R"(
def bool_and(a, b):
    return a and b
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "bool_and"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithTryExcept) {
  const char* py_src = R"(
def try_except():
    try:
        return 1
    except:
        return 0
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "try_except"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildGeneratorFunc) {
  const char* py_src = R"(
def gen():
    yield 1
    yield 2
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "gen"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithGlobalAccess) {
  const char* py_src = R"(
GLOBAL_VAR = 42

def read_global():
    return GLOBAL_VAR
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "read_global"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithClosure) {
  const char* py_src = R"(
def outer():
    x = 10
    def inner():
        return x
    return inner()
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "outer"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithIsOperator) {
  const char* py_src = R"(
def is_none(x):
    return x is None
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "is_none"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithInOperator) {
  const char* py_src = R"(
def contains(lst, val):
    return val in lst
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "contains"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithFString) {
  const char* py_src = R"(
def greet(name):
    return f"hello {name}"
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "greet"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithStarExpr) {
  const char* py_src = R"(
def spread():
    return [*range(3)]
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "spread"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithAugAssign) {
  const char* py_src = R"(
def aug_assign(x):
    x += 1
    x -= 1
    x *= 2
    return x
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "aug_assign"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithDelete) {
  const char* py_src = R"(
def delete_var():
    x = 1
    del x
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "delete_var"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithAssert) {
  const char* py_src = R"(
def assert_true():
    assert True
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "assert_true"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithRaise) {
  const char* py_src = R"(
def raise_error():
    raise ValueError("error")
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "raise_error"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithWhileLoop) {
  const char* py_src = R"(
def while_loop(n):
    i = 0
    while i < n:
        i += 1
    return i
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "while_loop"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithBreakContinue) {
  const char* py_src = R"(
def break_continue():
    for i in range(10):
        if i == 3:
            continue
        if i == 7:
            break
    return i
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "break_continue"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithSetLiteral) {
  const char* py_src = R"(
def make_set():
    return {1, 2, 3}
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "make_set"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithSlice) {
  const char* py_src = R"(
def slice_list(lst):
    return lst[1:3]
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "slice_list"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithWalrus) {
  const char* py_src = R"(
def walrus(lst):
    if (n := len(lst)) > 0:
        return n
    return 0
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "walrus"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}

TEST_F(HIRBuilderExtendedTest, BuildFuncWithMultipleReturns) {
  const char* py_src = R"(
def multi_ret(x):
    if x > 0:
        return 1
    elif x < 0:
        return -1
    else:
        return 0
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "multi_ret"));
  ASSERT_NE(func, nullptr);

  auto irfunc = buildHIR(func);
  ASSERT_NE(irfunc, nullptr);
}
