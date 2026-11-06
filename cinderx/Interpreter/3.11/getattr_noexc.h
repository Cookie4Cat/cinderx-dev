// Copyright (c) Meta Platforms, Inc. and affiliates.
// CPython 3.11 interpreter attribute adaptation. No framework-specific lookup.
// Included once by ceval_wrapper.c, before its evaluator-local API mappings.

#include "cinderx/UpstreamBorrow/borrowed.h"
#include "internal/pycore_object.h"
#include "attr_type_guard.h"

static int ci_interp_getattr_enabled;
static int ci_interp_getattr_counting;
// Borrowed from the immutable, process-lifetime built-in object type.
static PyObject* ci_interp_default_getattribute;
static unsigned long long ci_interp_getattr_eligible;
static unsigned long long ci_interp_getattr_hits;
static unsigned long long ci_interp_getattr_misses;
static unsigned long long ci_interp_getattr_errors;
static unsigned long long ci_interp_getattr_fallbacks;
static unsigned long long ci_interp_getattr_methods;

int Ci_InterpGetattr311_Init(void) {
  Ci_AttrPathConfigSnapshot311();
  ci_interp_getattr_enabled = ci_attr_noexc_config_311;
  ci_interp_getattr_counting = ci_attr_diagnostics_config_311;
  if (ci_interp_getattr_enabled && Ci_tp_getattr_hook == NULL) {
    PyErr_SetString(PyExc_RuntimeError, "CPython getattr hook is not initialized");
    return -1;
  }
  if (ci_interp_getattr_enabled) {
    ci_interp_default_getattribute =
        _PyType_Lookup(&PyBaseObject_Type, &_Py_ID(__getattribute__));
    if (ci_interp_default_getattribute == NULL) {
      PyErr_SetString(PyExc_RuntimeError, "object.__getattribute__ is unavailable");
      return -1;
    }
  }
  return 0;
}

// Configuration is separate from optional diagnostic counters.
PyObject* Ci_AttrPathStatus311(void) {
  return Py_BuildValue("{s:i,s:i,s:i,s:i,s:i,s:i}",
      "no_exception", ci_interp_getattr_enabled,
      "specialization", Ci_SpecGaEnabled_311,
      "split_dict", Ci_SplitHintEnabled_311,
      "global_cache", ci_global_cache_enabled_311,
      "inline_call", ci_inline_enabled_311,
      "diagnostics", ci_interp_getattr_counting);
}

PyObject* Ci_AttrPathCounters311(void) {
  if (!ci_interp_getattr_counting) Py_RETURN_NONE;
  return Py_BuildValue("{s:K,s:K,s:K,s:K,s:K,s:K}",
      "attribute_hits", ci_interp_getattr_hits,
      "attribute_misses", ci_interp_getattr_misses,
      "attribute_fallbacks", ci_interp_getattr_fallbacks,
      "attribute_errors", ci_interp_getattr_errors,
      "global_hits", ci_global_cache_hits_311,
      "global_misses", ci_global_cache_misses_311);
}

static PyObject* Ci_InterpGetattrHook311(PyObject* obj, PyObject* name) {
  PyTypeObject* tp = Py_TYPE(obj);
  // Type lookup can execute Python equality on general-key namespaces.
  // Reject the whole request before doing any additional type lookup.
  if (!Ci_AttrHasUnicodeMro311(tp)) {
    if (ci_interp_getattr_counting) ci_interp_getattr_fallbacks++;
    return PyObject_GetAttr(obj, name);
  }
  PyObject* handler = _PyType_Lookup(tp, &_Py_ID(__getattr__));
  if (handler == NULL) {
    if (ci_interp_getattr_counting) ci_interp_getattr_fallbacks++;
    return PyObject_GetAttr(obj, name);
  }
  // Match the slot's lifetime ordering: own handler before the next lookup.
  Py_INCREF(handler);
  PyObject* access = _PyType_Lookup(tp, &_Py_ID(__getattribute__));
  if (access != ci_interp_default_getattribute) {
    if (ci_interp_getattr_counting) ci_interp_getattr_fallbacks++;
    Py_DECREF(handler);
    return PyObject_GetAttr(obj, name);
  }

  // Inspect storage without materializing a dict or invoking user code.
  // suppress=1 may continue to a class value after dict equality raises
  // AttributeError. The original slot instead calls __getattr__ immediately.
  if (tp->tp_flags & Py_TPFLAGS_MANAGED_DICT) {
    PyObject* dict = *_PyObject_ManagedDictPointer(obj);
    if (dict != NULL &&
        (!PyDict_CheckExact(dict) || !DK_IS_UNICODE(((PyDictObject*)dict)->ma_keys))) {
      if (ci_interp_getattr_counting) ci_interp_getattr_fallbacks++;
      Py_DECREF(handler);
      return PyObject_GetAttr(obj, name);
    }
  } else if (tp->tp_dictoffset != 0) {
    // Conservatively retain the original protocol for other dict layouts.
    if (ci_interp_getattr_counting) ci_interp_getattr_fallbacks++;
    Py_DECREF(handler);
    return PyObject_GetAttr(obj, name);
  }

  // The owned handler also survives mutations during the generic lookup.
  if (ci_interp_getattr_counting) {
    ci_interp_getattr_eligible++;
  }
  PyObject* result = _PyObject_GenericGetAttrWithDict(obj, name, NULL, 1);
  if (result != NULL) {
    if (ci_interp_getattr_counting) {
      ci_interp_getattr_hits++;
    }
  } else if (PyErr_Occurred()) {
    // suppress=1 only suppresses AttributeError. Other descriptor failures
    // must propagate without retrying a descriptor with side effects.
    if (ci_interp_getattr_counting) {
      ci_interp_getattr_errors++;
    }
  } else {
    if (ci_interp_getattr_counting) {
      ci_interp_getattr_misses++;
    }
    // Preserve CPython 3.11 typeobject.c:call_attribute descriptor binding
    // and invoke the original handler; do not implement its logic in C.
    descrgetfunc bind = Py_TYPE(handler)->tp_descr_get;
    PyObject* bound = bind == NULL
        ? NULL
        : bind(handler, obj, (PyObject*)Py_TYPE(obj));
    PyObject* callable = bind == NULL ? handler : bound;
    if (callable != NULL) {
      result = PyObject_CallOneArg(callable, name);
    }
    Py_XDECREF(bound);
  }
  Py_DECREF(handler);
  if (result == NULL) {
    // The outer PyObject_GetAttr normally adds this context. Reuse the
    // versioned borrowed implementation, including AttributeError subclasses.
    Cix_set_attribute_error_context(obj, name);
  }
  return result;
}

static inline PyObject* Ci_InterpGetAttr311(PyObject* obj, PyObject* name) {
  if (!ci_interp_getattr_enabled || !PyUnicode_CheckExact(name) ||
      Py_TYPE(obj)->tp_getattro != Ci_tp_getattr_hook) {
    return PyObject_GetAttr(obj, name);
  }
  return Ci_InterpGetattrHook311(obj, name);
}

static inline int Ci_InterpGetMethod311(
    PyObject* obj, PyObject* name, PyObject** method) {
  if (!ci_interp_getattr_enabled || !PyUnicode_CheckExact(name) ||
      Py_TYPE(obj)->tp_getattro != Ci_tp_getattr_hook) {
    return _PyObject_GetMethod(obj, name, method);
  }
  if (ci_interp_getattr_counting) {
    ci_interp_getattr_methods++;
  }
  // Stock _PyObject_GetMethod delegates to PyObject_GetAttr and returns 0
  // for these hook types too. The result is already bound, including errors.
  *method = Ci_InterpGetattrHook311(obj, name);
  return 0;
}
