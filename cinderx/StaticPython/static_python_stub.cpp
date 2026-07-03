// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/StaticPython/_static.h"
#include "cinderx/StaticPython/awaitable.h"
#include "cinderx/StaticPython/checked_dict.h"
#include "cinderx/StaticPython/checked_list.h"
#include "cinderx/StaticPython/classloader.h"
#include "cinderx/StaticPython/descrs.h"
#include "cinderx/StaticPython/objectkey.h"
#include "cinderx/StaticPython/static_array.h"
#include "cinderx/StaticPython/strictmoduleobject.h"
#include "cinderx/StaticPython/thunks.h"
#include "cinderx/StaticPython/type.h"
#include "cinderx/StaticPython/typed-args-info.h"
#include "cinderx/StaticPython/typed_method_def.h"
#include "cinderx/StaticPython/vtable_defs.h"

namespace {

PyTypeObject disabledType(const char* name, Py_ssize_t basicsize) {
  PyTypeObject type = {PyVarObject_HEAD_INIT(nullptr, 0)};
  type.tp_name = name;
  type.tp_basicsize = basicsize;
  type.tp_flags = Py_TPFLAGS_DEFAULT;
  return type;
}

} // namespace

PyTypeObject _PyTypedArgsInfo_Type =
    disabledType("cinderx._TypedArgsInfo", sizeof(_PyTypedArgsInfo));
static PyTypeObject PyStaticArray_DisabledType =
    disabledType("cinderx.staticarray", sizeof(PyObject));

extern "C" {

PyTypeObject Ci_StrictModule_Type =
    disabledType("cinderx.StrictModule", sizeof(Ci_StrictModuleObject));

PyTypeObject* Ci_CheckedDict_Type = &PyDict_Type;
PyTypeObject* Ci_CheckedList_Type = &PyList_Type;
_PyGenericTypeDef Ci_CheckedDict_GenericType = {};
_PyGenericTypeDef Ci_CheckedList_GenericType = {};

PyTypeObject _PyTypedDescriptor_Type =
    disabledType("cinderx._TypedDescriptor", sizeof(_PyTypedDescriptor));
PyTypeObject _PyTypedDescriptorWithDefaultValue_Type = disabledType(
    "cinderx._TypedDescriptorWithDefaultValue",
    sizeof(_PyTypedDescriptorWithDefaultValue));
PyTypeObject _Ci_ObjectKeyType =
    disabledType("cinderx.object_key", sizeof(_Ci_ObjectKey));
PyType_Spec PyStaticArray_Spec = {};
PyTypeObject* PyStaticArray_Type = &PyStaticArray_DisabledType;
PyTypeObject _PyType_VTableType =
    disabledType("cinderx._VTable", sizeof(_PyType_VTable));
PyTypeObject _PyType_MethodThunk = {PyVarObject_HEAD_INIT(nullptr, 0)};
PyTypeObject _PyType_StaticThunk = {PyVarObject_HEAD_INIT(nullptr, 0)};
PyTypeObject _PyType_PropertyThunk = {PyVarObject_HEAD_INIT(nullptr, 0)};
PyTypeObject _PyClassLoader_VTableInitThunk_Type = {
    PyVarObject_HEAD_INIT(nullptr, 0)};
PyTypeObject _PyType_TypeCheckThunk = {PyVarObject_HEAD_INIT(nullptr, 0)};
PyTypeObject _PyClassLoader_LazyFuncJitThunk_Type = {
    PyVarObject_HEAD_INIT(nullptr, 0)};
PyTypeObject _PyClassLoader_StaticMethodThunk_Type = {
    PyVarObject_HEAD_INIT(nullptr, 0)};
PyTypeObject _PyClassLoader_ClassMethodThunk_Type = {
    PyVarObject_HEAD_INIT(nullptr, 0)};

const Ci_Py_SigElement Ci_Py_Sig_T0 = {};
const Ci_Py_SigElement Ci_Py_Sig_T1 = {};
Ci_Py_SigElement Ci_Py_Sig_T0_Opt = {};
Ci_Py_SigElement Ci_Py_Sig_T1_Opt = {};
const Ci_Py_SigElement Ci_Py_Sig_Object = {};
Ci_Py_SigElement Ci_Py_Sig_Object_Opt = {};
const Ci_Py_SigElement Ci_Py_Sig_String = {};
Ci_Py_SigElement Ci_Py_Sig_String_Opt = {};
const Ci_Py_SigElement Ci_Py_Sig_SSIZET = {};
const Ci_Py_SigElement Ci_Py_Sig_SIZET = {};
const Ci_Py_SigElement Ci_Py_Sig_INT8 = {};
const Ci_Py_SigElement Ci_Py_Sig_INT16 = {};
const Ci_Py_SigElement Ci_Py_Sig_INT32 = {};
const Ci_Py_SigElement Ci_Py_Sig_INT64 = {};
const Ci_Py_SigElement Ci_Py_Sig_UINT8 = {};
const Ci_Py_SigElement Ci_Py_Sig_UINT16 = {};
const Ci_Py_SigElement Ci_Py_Sig_UINT32 = {};
const Ci_Py_SigElement Ci_Py_Sig_UINT64 = {};

int _Ci_CreateStaticModule() {
  return 0;
}

void _PyCheckedDict_ClearCaches() {}
void _PyCheckedList_ClearCaches() {}
int Ci_CheckedDict_Check(PyObject*) {
  return 0;
}
int Ci_CheckedDict_TypeCheck(PyTypeObject*) {
  return 0;
}
PyObject* Ci_CheckedDict_New(PyTypeObject*) {
  return PyDict_New();
}
PyObject* Ci_CheckedDict_NewPresized(PyTypeObject*, Py_ssize_t minused) {
  return _PyDict_NewPresized(minused);
}
int Ci_CheckedDict_SetItem(PyObject* op, PyObject* key, PyObject* value) {
  return PyDict_SetItem(op, key, value);
}
int Ci_DictOrChecked_SetItem(PyObject* op, PyObject* key, PyObject* value) {
  return PyDict_SetItem(op, key, value);
}
int Ci_CheckedList_Check(PyObject*) {
  return 0;
}
int Ci_CheckedList_TypeCheck(PyTypeObject*) {
  return 0;
}
PyObject* Ci_CheckedList_New(PyTypeObject*, Py_ssize_t size) {
  return PyList_New(size);
}
PyObject* Ci_CheckedList_GetItem(PyObject* self, Py_ssize_t index) {
  return Py_NewRef(PyList_GET_ITEM(self, index));
}
int Ci_ListOrCheckedList_Append(PyListObject* self, PyObject* value) {
  return PyList_Append(reinterpret_cast<PyObject*>(self), value);
}

PyObject* Ci_StrictModule_New(PyTypeObject*, PyObject*, PyObject*) {
  PyErr_SetString(PyExc_NotImplementedError, "StrictModule is disabled on stock 3.11");
  return nullptr;
}
PyObject* _Ci_ObjectKey_New(PyObject*) {
  PyErr_SetString(
      PyExc_NotImplementedError,
      "Static Python object keys are disabled on stock 3.11");
  return nullptr;
}
int _Ci_StaticArray_Set(PyObject*, Py_ssize_t, PyObject*) {
  PyErr_SetString(
      PyExc_NotImplementedError,
      "Static arrays are disabled on stock 3.11");
  return -1;
}
PyObject* _Ci_StaticArray_Get(PyObject*, Py_ssize_t) {
  PyErr_SetString(
      PyExc_NotImplementedError,
      "Static arrays are disabled on stock 3.11");
  return nullptr;
}
PyObject* Ci_StrictModule_GetOriginal(PyObject*, PyObject*) {
  Py_RETURN_NONE;
}
int Ci_do_strictmodule_patch(PyObject*, PyObject*, PyObject*) {
  PyErr_SetString(PyExc_NotImplementedError, "StrictModule is disabled on stock 3.11");
  return -1;
}
PyObject* Ci_StrictModule_GetDictSetter(PyObject*) {
  return nullptr;
}
PyObject* Ci_StrictModule_GetDict(PyObject* mod) {
  return PyModule_GetDict(mod);
}
int _PyClassLoader_IsImmutable(PyObject* container) {
  return PyType_Check(container) &&
      PyType_HasFeature(reinterpret_cast<PyTypeObject*>(container), Py_TPFLAGS_IMMUTABLETYPE);
}

Py_ssize_t _PyClassLoader_ResolveMethod(PyObject*) {
  return -1;
}
Py_ssize_t _PyClassLoader_ResolveFieldOffset(PyObject*, int*) {
  return -1;
}
int _PyClassLoader_GetTypeCode(PyTypeObject*) {
  return TYPED_OBJECT;
}
int _PyClassLoader_AddSubclass(PyTypeObject*, PyTypeObject*) {
  return 0;
}
_PyType_VTable* _PyClassLoader_EnsureVtable(PyTypeObject*, int) {
  return nullptr;
}
int _PyClassLoader_ClearVtables() {
  return 0;
}
void _PyClassLoader_ClearGenericTypes() {}
int _PyClassLoader_IsPatchedThunk(PyObject*) {
  return 0;
}
PyObject** _PyClassLoader_ResolveIndirectPtr(PyObject*) {
  return nullptr;
}
PyObject* _PyClassLoader_ResolveFunction(PyObject*, PyObject**) {
  Py_RETURN_NONE;
}
PyMethodDescrObject* _PyClassLoader_ResolveMethodDef(PyObject*) {
  return nullptr;
}
int _PyClassLoader_IsFinalMethodOverridden(PyTypeObject*, PyObject*) {
  return 0;
}
_PyTypedArgsInfo* _PyClassLoader_GetTypedArgsInfoFromThunk(PyObject*, PyObject*, int) {
  return nullptr;
}
PyObject* _PyClassLoader_ResolveReturnType(
    PyObject*,
    int* optional,
    int* exact,
    int* func_flags) {
  if (optional != nullptr) {
    *optional = 0;
  }
  if (exact != nullptr) {
    *exact = 0;
  }
  if (func_flags != nullptr) {
    *func_flags = 0;
  }
  return Py_NewRef(&PyBaseObject_Type);
}
PyObject* _PyClassloader_SizeOf_DlSym_Cache() {
  return PyLong_FromLong(0);
}
PyObject* _PyClassloader_SizeOf_DlOpen_Cache() {
  return PyLong_FromLong(0);
}
void _PyClassloader_Clear_DlSym_Cache() {}
void _PyClassloader_Clear_DlOpen_Cache() {}
void* _PyClassloader_LookupSymbol(PyObject*, PyObject*) {
  return nullptr;
}
int _PyClassLoader_HasPrimitiveArgs(PyCodeObject*) {
  return 0;
}
int _PyClassLoader_NotifyDictChange(PyDictObject*, PyDict_WatchEvent, PyObject*, PyObject*) {
  return 0;
}
PyObject* _PyClassloader_InvokeNativeFunction(PyObject*, PyObject*, PyObject*, PyObject**, Py_ssize_t) {
  PyErr_SetString(PyExc_NotImplementedError, "native Static Python calls are disabled on stock 3.11");
  return nullptr;
}
PyObject* _PyClassLoader_InvokeMethod(_PyType_VTable*, Py_ssize_t, PyObject**, Py_ssize_t) {
  PyErr_SetString(PyExc_NotImplementedError, "Static Python vtables are disabled on stock 3.11");
  return nullptr;
}
int32_t _PyClassLoader_CacheValue(PyObject*) {
  return -1;
}
PyObject* _PyClassLoader_GetCachedValue(int32_t) {
  return nullptr;
}
void _PyClassLoader_ClearValueCache() {}
PyObject* _PyClassLoader_GetModuleAttr(PyObject* module, PyObject* name) {
  return PyObject_GetAttr(module, name);
}
PyObject* _PyClassLoader_ResolveContainer(PyObject*) {
  return nullptr;
}
int _PyClassLoader_VerifyType(PyObject*, PyObject*) {
  return 0;
}
PyTypeObject* _PyClassLoader_ResolveType(PyObject*, int* optional, int* exact) {
  if (optional != nullptr) {
    *optional = 0;
  }
  if (exact != nullptr) {
    *exact = 0;
  }
  return &PyBaseObject_Type;
}
int _PyClassLoader_CheckModuleChange(PyDictObject*, PyObject*) {
  return 0;
}
void _PyClassLoader_ClearCache() {}
PyObject* _PyClassLoader_GetCache() {
  return PyDict_New();
}
int _PyClassLoader_ResolvePrimitiveType(PyObject*) {
  return TYPED_OBJECT;
}
int is_static_type(PyTypeObject*) {
  return 0;
}
Py_ssize_t _PyClassLoader_PrimitiveTypeToSize(int) {
  return sizeof(PyObject*);
}
int _PyClassLoader_PrimitiveTypeToStructMemberType(int) {
  return T_OBJECT;
}
PyObject* _PyClassLoader_Box(uint64_t value, int) {
  return PyLong_FromUnsignedLongLong(value);
}
uint64_t _PyClassLoader_Unbox(PyObject* value, int) {
  return PyLong_AsUnsignedLongLong(value);
}
_PyTypedArgsInfo* _PyClassLoader_GetTypedArgsInfo(PyCodeObject*, int) {
  return nullptr;
}
PyObject* _PyClassLoader_GetReturnTypeDescr(PyFunctionObject*) {
  return nullptr;
}
PyObject* _PyClassLoader_GetCodeReturnTypeDescr(PyCodeObject*) {
  return nullptr;
}
PyObject* _PyClassLoader_GetCodeArgumentTypeDescrs(PyCodeObject*) {
  return nullptr;
}
PyObject* _PyClassLoader_CheckReturnType(PyTypeObject*, PyObject* value, _PyClassLoader_RetTypeInfo*) {
  return Py_NewRef(value);
}
PyObject* _PyClassLoader_CheckReturnCallback(_PyClassLoader_Awaitable*, PyObject*) {
  Py_RETURN_NONE;
}
int _PyClassLoader_IsPropertyName(PyTupleObject*) {
  return 0;
}
PyObject* _PyClassLoader_GetFunctionName(PyObject* name) {
  return Py_NewRef(name);
}
PyObject* _PyClassLoader_MaybeUnwrapCallable(PyObject* func) {
  return Py_NewRef(func);
}
PyObject* _PyClassLoader_CallCoroutine(
    _PyClassLoader_TypeCheckThunk*,
    PyObject* const*,
    size_t) {
  PyErr_SetString(PyExc_NotImplementedError, "Static Python coroutine wrappers are disabled on stock 3.11");
  return nullptr;
}
PyObject* _PyClassLoader_CallCoroutineOverridden(
    _PyClassLoader_TypeCheckThunk*,
    PyObject*,
    PyObject* const*,
    size_t) {
  PyErr_SetString(PyExc_NotImplementedError, "Static Python coroutine wrappers are disabled on stock 3.11");
  return nullptr;
}

PyObject* _PyClassLoader_NewAwaitableWrapper(
    PyObject*,
    int,
    PyObject*,
    awaitable_cb,
    awaitable_presend) {
  PyErr_SetString(PyExc_NotImplementedError, "Static Python awaitable wrappers are disabled on stock 3.11");
  return nullptr;
}
PyObject* _PyTypedDescriptor_New(PyObject*, PyObject*, Py_ssize_t) {
  Py_RETURN_NONE;
}
PyObject* _PyTypedDescriptorWithDefaultValue_New(PyObject*, PyObject*, Py_ssize_t, PyObject*) {
  Py_RETURN_NONE;
}
PyObject* _PyClassLoader_GtdGetItem(_PyGenericTypeDef*, PyObject*) {
  Py_RETURN_NONE;
}
PyTypeObject* _PyClassLoader_MakeGenericHeapType(_PyGenericTypeDef*) {
  return &PyBaseObject_Type;
}
int _PyClassLoader_TypeDealloc(PyTypeObject*) {
  return 0;
}
int _PyClassLoader_TypeTraverse(PyTypeObject*, visitproc, void*) {
  return 0;
}
void _PyClassLoader_TypeClear(PyTypeObject*) {}
PyObject* _PyClassLoader_GetGenericInst(PyObject*, PyObject**, Py_ssize_t) {
  Py_RETURN_NONE;
}

int _PyClassLoader_CheckOneArg(
    PyObject*,
    PyObject*,
    char*,
    int,
    const Ci_Py_SigElement*) {
  return 1;
}
void _PyClassLoader_ArgError(
    PyObject*,
    int,
    int,
    const Ci_Py_SigElement*,
    PyObject*) {}
void _PyClassLoader_ArgErrorStr(
    const char*,
    int,
    const Ci_Py_SigElement*,
    PyObject*) {}
void _PyClassLoader_FreeHydratedArgs(PyObject**, Py_ssize_t) {}

}
