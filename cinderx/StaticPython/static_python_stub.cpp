// Copyright (c) Meta Platforms, Inc. and affiliates.

// Static Python surface for builds that do not ship it (CPython 3.11).
//
// The runtime module links against a small set of Static Python symbols for
// module bookkeeping: cache-clearing hooks it calls on shutdown and type
// invalidation, the StrictModule type it registers, and the accessors that
// go with it.  Everything else in StaticPython belongs to compiled Static
// Python code, which cannot exist here, so it is not part of this build at
// all rather than being present as dead definitions.
//
// The symbol set below is exactly the one the 3.11 link requires; add to it
// only when the linker asks.

#include "cinderx/StaticPython/classloader.h"
#include "cinderx/StaticPython/objectkey.h"
#include "cinderx/StaticPython/strictmoduleobject.h"

namespace {

PyTypeObject disabledType(const char* name, Py_ssize_t basicsize) {
  PyTypeObject type = {PyVarObject_HEAD_INIT(nullptr, 0)};
  type.tp_name = name;
  type.tp_basicsize = basicsize;
  type.tp_flags = Py_TPFLAGS_DEFAULT;
  return type;
}

} // namespace

extern "C" {

// Types the module registers with PyType_Ready(). They are named and sized
// like the real ones so registration behaves normally; no instance is ever
// created because nothing constructs them.
PyTypeObject Ci_StrictModule_Type =
    disabledType("cinderx.StrictModule", sizeof(Ci_StrictModuleObject));
PyTypeObject _Ci_ObjectKeyType =
    disabledType("cinderx.object_key", sizeof(_Ci_ObjectKey));

int _Ci_CreateStaticModule() {
  return 0;
}

// Cache-clearing hooks. No Static Python cache exists, so these are complete
// implementations rather than stubs.
void _PyCheckedDict_ClearCaches() {}
void _PyCheckedList_ClearCaches() {}
void _PyClassLoader_ClearCache() {}
void _PyClassLoader_ClearGenericTypes() {}
void _PyClassLoader_ClearValueCache() {}

int _PyClassLoader_ClearVtables() {
  return 0;
}

int _PyClassLoader_NotifyDictChange(
    PyDictObject*,
    PyDict_WatchEvent,
    PyObject*,
    PyObject*) {
  return 0;
}

// StrictModule accessors. Reading the dict is the plain module behaviour;
// patching requires the strict-module machinery this build does not have.
PyObject* Ci_StrictModule_GetDict(PyObject* mod) {
  return PyModule_GetDict(mod);
}

PyObject* Ci_StrictModule_GetDictSetter(PyObject*) {
  return nullptr;
}

int Ci_do_strictmodule_patch(PyObject*, PyObject*, PyObject*) {
  PyErr_SetString(
      PyExc_NotImplementedError,
      "strict module patching requires Static Python, which is not built on "
      "CPython 3.11");
  return -1;
}

} // extern "C"
