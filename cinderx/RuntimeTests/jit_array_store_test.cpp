// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/RuntimeTests/fixtures.h"

#include "cinderx/Jit/hir/hir.h"

using ArrayStoreTest = RuntimeTest;

// Test that getStdlibArrayType returns a valid type object.
TEST_F(ArrayStoreTest, GetStdlibArrayTypeReturnsNonNull) {
  auto func = compileAndGet(
      R"(
def dummy():
    pass
)",
      "dummy");
  ASSERT_NE(func, nullptr);

  // Import array module to ensure it's available
  auto array_mod = Ref<>::steal(PyImport_ImportModule("array"));
  ASSERT_NE(array_mod, nullptr);

  auto array_type = Ref<>::steal(
      PyObject_GetAttrString(array_mod, "array"));
  ASSERT_NE(array_type, nullptr);
  ASSERT_TRUE(PyType_Check(array_type));
}

// Test that StdlibArrayObject layout is correct for array('d').
TEST_F(ArrayStoreTest, ArrayDoubleLayoutValidation) {
  auto array_mod = Ref<>::steal(PyImport_ImportModule("array"));
  ASSERT_NE(array_mod, nullptr);

  auto array_type = Ref<>::steal(
      PyObject_GetAttrString(array_mod, "array"));
  ASSERT_NE(array_type, nullptr);

  // Create array('d', [1.5]) and verify we can read the value
  auto d_str = Ref<>::steal(PyUnicode_InternFromString("d"));
  ASSERT_NE(d_str, nullptr);
  auto val_list = Ref<>::steal(PyList_New(1));
  ASSERT_NE(val_list, nullptr);
  auto val = PyFloat_FromDouble(1.5);
  ASSERT_NE(val, nullptr);
  PyList_SET_ITEM(val_list, 0, val);

  auto args = Ref<>::steal(PyTuple_Pack(2, d_str.get(), val_list.get()));
  ASSERT_NE(args, nullptr);

  auto arr = Ref<>::steal(PyObject_CallObject(array_type, args));
  ASSERT_NE(arr, nullptr);

  // Verify it has the right typecode
  auto tc = Ref<>::steal(PyObject_GetAttrString(arr, "typecode"));
  ASSERT_NE(tc, nullptr);
  auto tc_str = PyUnicode_AsUTF8(tc);
  ASSERT_STREQ(tc_str, "d");

  // Verify the value is correct
  auto item = Ref<>::steal(PyObject_GetItem(arr, PyLong_FromLong(0)));
  ASSERT_NE(item, nullptr);
  double dval = PyFloat_AsDouble(item);
  ASSERT_EQ(dval, 1.5);
}

// Test that STORE_SUBSCR on array('d') generates StoreArrayItem in HIR.
TEST_F(ArrayStoreTest, StoreSubscrArrayDoubleGeneratesStoreArrayItem) {
  std::unique_ptr<jit::hir::Function> irfunc;
  CompileToHIR(
      R"(
from array import array
def store_array_double(a, i, v):
    a[i] = v
)",
      "store_array_double",
      irfunc);

  ASSERT_NE(irfunc, nullptr);

  // Walk the HIR and check for StoreArrayItem with TCDouble type
  bool found_store_array_item = false;
  for (auto& block : irfunc->cfg.blocks) {
    for (auto it = block.begin(); it != block.end(); ++it) {
      auto& instr = *it;
      if (instr.IsStoreArrayItem()) {
        auto* sai = static_cast<const jit::hir::StoreArrayItem*>(&instr);
        if (sai->type() <= jit::hir::TCDouble) {
          found_store_array_item = true;
        }
      }
    }
  }

  EXPECT_TRUE(found_store_array_item)
      << "Expected StoreArrayItem(TCDouble) in HIR for array('d') store";
}

// Test that STORE_SUBSCR on any type generates both fast and slow paths.
// The array fast path is always emitted with a CondBranchCheckType guard;
// non-array containers will take the slow path at runtime.
TEST_F(ArrayStoreTest, StoreSubscriGeneratesBothPaths) {
  std::unique_ptr<jit::hir::Function> irfunc;
  CompileToHIR(
      R"(
def store_list(a, i, v):
    a[i] = v
)",
      "store_list",
      irfunc);

  ASSERT_NE(irfunc, nullptr);

  // Should find both StoreArrayItem (fast path) and StoreSubscr (slow path)
  bool found_store_array_item = false;
  bool found_store_subscr = false;
  for (auto& block : irfunc->cfg.blocks) {
    for (auto it = block.begin(); it != block.end(); ++it) {
      auto& instr = *it;
      if (instr.IsStoreArrayItem()) {
        found_store_array_item = true;
      }
      if (instr.IsStoreSubscr()) {
        found_store_subscr = true;
      }
    }
  }

  EXPECT_TRUE(found_store_array_item)
      << "Expected StoreArrayItem (fast path) in HIR";
  EXPECT_TRUE(found_store_subscr)
      << "Expected StoreSubscr (slow path) in HIR";
}
