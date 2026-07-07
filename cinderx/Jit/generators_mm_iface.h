// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/ref.h"

namespace jit {

struct JitGenObject;

class IJitGenFreeList {
 public:
  IJitGenFreeList() = default;
  virtual ~IJitGenFreeList() = default;

  virtual std::pair<JitGenObject*, size_t> allocate(
      BorrowedRef<PyCodeObject> code,
      uint64_t jit_spill_words) = 0;
  virtual void free(PyObject* ptr) = 0;
  // 该指针的内存是否由本 free-list 的 arena 提供（deopt 后类型还原为
  // stock 生成器类型,但 arena 内存仍必须走自定义释放,不能交
  // PyObject_GC_Del）。
  virtual bool owns(PyObject* ptr) = 0;
};

} // namespace jit
