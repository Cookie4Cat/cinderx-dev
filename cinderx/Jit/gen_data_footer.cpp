// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/gen_data_footer.h"

#include "cinderx/Common/py-portability.h"
#include "cinderx/module_state.h"

namespace jit {
GenDataFooter** jitGenDataFooterPtr(PyGenObject* gen, PyCodeObject* gen_code) {
  // TASK(T209501671): This has way too much going on. If we made PyGenObject
  // use PyObject_VAR_HEAD like it probably should this would get simpler. If
  // we expanded the allocation to include the GenDataFooter it'd get simpler
  // still.
  BorrowedRef<PyTypeObject> gen_type = cinderx::getModuleState()->gen_type;

  size_t python_frame_data_bytes =
      _PyFrame_NumSlotsForCodeObject(gen_code) * gen_type->tp_itemsize;
  // A *pointer* to JIT data comes after all the other data in the default
  // generator object.
  return reinterpret_cast<GenDataFooter**>(
      reinterpret_cast<uintptr_t>(gen) + gen_type->tp_basicsize +
      python_frame_data_bytes);
}

GenDataFooter** jitGenDataFooterPtr(PyGenObject* gen) {
  _PyInterpreterFrame* gen_frame = generatorFrame(gen);
  return jitGenDataFooterPtr(gen_frame);
}

GenDataFooter** jitGenDataFooterPtr(_PyInterpreterFrame* gen_frame) {
#if PY_VERSION_HEX >= 0x030E0000
  constexpr Py_ssize_t kJitGenDataOffsetFromFrameEnd = sizeof(PyGenObject) +
      sizeof(GenDataFooter*) - offsetof(PyGenObject, gi_iframe) -
      FRAME_SPECIALS_SIZE * sizeof(PyObject*);
  static_assert(
      kJitGenDataOffsetFromFrameEnd % sizeof(PyObject*) == 0,
      "JIT generator data offset should be pointer-aligned");
  return reinterpret_cast<GenDataFooter**>(
      reinterpret_cast<char*>(gen_frame) +
      _PyFrame_GetCode(gen_frame)->co_framesize * sizeof(PyObject*) +
      kJitGenDataOffsetFromFrameEnd);
#else
  PyGenObject* gen = _PyGen_GetGeneratorFromFrame(gen_frame);
  return jitGenDataFooterPtr(gen, _PyFrame_GetCode(gen_frame));
#endif
}

GenDataFooter* jitGenDataFooter(PyGenObject* gen) {
  return *jitGenDataFooterPtr(gen);
}

GenDataFooter* jitGenDataFooter(_PyInterpreterFrame* gen_frame) {
  return *jitGenDataFooterPtr(gen_frame);
}
} // namespace jit
