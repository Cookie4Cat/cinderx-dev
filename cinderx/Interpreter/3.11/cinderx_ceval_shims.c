// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// Hand-written semantic shims for private CPython 3.11 helpers referenced
// by the vendored eval loop, where a verbatim extraction would drag in deep
// file-static machinery (dictobject insertdict, sysmodule audit plumbing,
// unicode/thread internals). Each shim documents why it is behaviorally
// equivalent to the upstream v3.11.6 implementation; equivalence is gated
// by the config-② libtest diff, not by review alone.

#define Py_BUILD_CORE

#include "Python.h"
#include "pycore_frame.h"
#include "pycore_pystate.h"

#include <pthread.h>
#include <stdarg.h>

// ---------------------------------------------------------------------------
// Objects/abstract.c: _PyNumber_PowerNoMod / _PyNumber_InPlacePowerNoMod
//
// Upstream bodies are one-line calls to the file-static ternary_op with
// Py_None as the modulus. The public PyNumber_Power/PyNumber_InPlacePower
// are the exact same ternary_op calls, so forwarding is equivalent.

PyObject* _PyNumber_PowerNoMod(PyObject* lhs, PyObject* rhs) {
  return PyNumber_Power(lhs, rhs, Py_None);
}

PyObject* _PyNumber_InPlacePowerNoMod(PyObject* lhs, PyObject* rhs) {
  return PyNumber_InPlacePower(lhs, rhs, Py_None);
}

// ---------------------------------------------------------------------------
// Objects/dictobject.c: _PyDict_FromItems / _PyDict_SetItem_Take2
//
// Upstream _PyDict_FromItems presizes the dict and uses insertdict directly;
// the observable behavior (resulting dict contents, error propagation) is
// identical to building via the public API, only slower. BUILD_MAP /
// BUILD_CONST_KEY_MAP are the only vendored-loop callers.

PyObject* _PyDict_FromItems(
    PyObject* const* keys,
    Py_ssize_t keys_offset,
    PyObject* const* values,
    Py_ssize_t values_offset,
    Py_ssize_t length) {
  PyObject* dict = PyDict_New();
  if (dict == NULL) {
    return NULL;
  }
  for (Py_ssize_t i = 0; i < length; i++) {
    PyObject* key = keys[i * keys_offset];
    PyObject* value = values[i * values_offset];
    if (PyDict_SetItem(dict, key, value) < 0) {
      Py_DECREF(dict);
      return NULL;
    }
  }
  return dict;
}

// Upstream steals both references (setitem_take2_lock_held path); equivalent
// to a plain SetItem followed by dropping our two references.
int _PyDict_SetItem_Take2(PyDictObject* mp, PyObject* key, PyObject* value) {
  int result = PyDict_SetItem((PyObject*)mp, key, value);
  Py_DECREF(key);
  Py_DECREF(value);
  return result;
}

// ---------------------------------------------------------------------------
// Objects/longobject.c: _PyLong_Add / _PyLong_Subtract / _PyLong_Multiply
//
// Upstream versions fast-path medium ints and otherwise fall through to the
// same bignum kernel the number-protocol slots use. Calling the runtime's
// own nb_ slots executes libpython's long_add/long_sub/long_mul directly:
// bit-identical results from the very same code, minus the medium-int
// shortcut (a performance delta only; perf is out of M2 scope). The
// vendored loop's BINARY_OP_*_INT specializations guard exactness before
// calling these.

PyObject* _PyLong_Add(PyLongObject* left, PyLongObject* right) {
  return PyLong_Type.tp_as_number->nb_add((PyObject*)left, (PyObject*)right);
}

PyObject* _PyLong_Subtract(PyLongObject* left, PyLongObject* right) {
  return PyLong_Type.tp_as_number->nb_subtract(
      (PyObject*)left, (PyObject*)right);
}

PyObject* _PyLong_Multiply(PyLongObject* left, PyLongObject* right) {
  return PyLong_Type.tp_as_number->nb_multiply(
      (PyObject*)left, (PyObject*)right);
}

// ---------------------------------------------------------------------------
// Objects/floatobject.c: _PyFloat_ExactDealloc
//
// Called by Py_DECREF_SPECIALIZED fast paths when the refcount hit zero for
// an exact float. Upstream inlines float_dealloc's exact-type branch;
// dispatching through tp_dealloc runs libpython's float_dealloc, which
// performs the same freelist push on the runtime's own freelist state.

void _PyFloat_ExactDealloc(PyObject* op) {
  assert(PyFloat_CheckExact(op));
  Py_TYPE(op)->tp_dealloc(op);
}

// ---------------------------------------------------------------------------
// Objects/tupleobject.c: _PyTuple_FromArraySteal
//
// Upstream allocates from the tuple freelist and steals the item
// references. PyTuple_New uses the same freelist inside libpython and
// PyTuple_SET_ITEM steals identically; on allocation failure the references
// are dropped exactly like upstream's error path.

PyObject* _PyTuple_FromArraySteal(PyObject* const* src, Py_ssize_t n) {
  PyObject* tuple = PyTuple_New(n);
  if (tuple == NULL) {
    for (Py_ssize_t i = 0; i < n; i++) {
      Py_DECREF(src[i]);
    }
    return NULL;
  }
  for (Py_ssize_t i = 0; i < n; i++) {
    PyTuple_SET_ITEM(tuple, i, src[i]);
  }
  return tuple;
}

// ---------------------------------------------------------------------------
// Objects/unicodeobject.c: _PyUnicode_ExactDealloc
//
// Called by Py_DECREF_SPECIALIZED fast paths when the refcount already hit
// zero for an exact str. Upstream asserts exactness and calls the type's
// dealloc; dispatching through the type slot is the same call.

void _PyUnicode_ExactDealloc(PyObject* op) {
  assert(PyUnicode_CheckExact(op));
  Py_TYPE(op)->tp_dealloc(op);
}

// ---------------------------------------------------------------------------
// Python/sysmodule.c: _PySys_Audit
//
// Upstream builds the event args with Py_VaBuildValue and raises through
// sys_audit_tstate. PySys_Audit does exactly that for the current thread
// state; a single "O" format returns the built tuple unmodified, so the
// argument tuple observed by hooks is identical. The vendored loop only
// audits on its own (current) tstate.

int _PySys_Audit(
    PyThreadState* tstate,
    const char* event,
    const char* argFormat,
    ...) {
  assert(tstate == _PyThreadState_GET());
  if (argFormat == NULL) {
    return PySys_Audit(event, NULL);
  }
  va_list vargs;
  va_start(vargs, argFormat);
  PyObject* args = Py_VaBuildValue(argFormat, vargs);
  va_end(vargs);
  if (args == NULL) {
    return -1;
  }
  int result = PySys_Audit(event, "O", args);
  Py_DECREF(args);
  return result;
}

// ---------------------------------------------------------------------------
// Python/thread_pthread.h: _PyThread_cond_init / _PyThread_cond_after
//
// Mirrors upstream's monotonic-clock condvar helpers. Upstream keys the
// attr off CONDATTR_MONOTONIC probing done at thread-init; we perform the
// same probe lazily. Timeouts computed against the same clock upstream
// selects, so GIL switch-interval behavior matches.

#if defined(HAVE_PTHREAD_CONDATTR_SETCLOCK) && defined(HAVE_CLOCK_GETTIME) && \
    defined(CLOCK_MONOTONIC)
#define CI_CONDATTR_MONOTONIC 1
#else
#define CI_CONDATTR_MONOTONIC 0
#endif

static pthread_condattr_t* ci_condattr(void) {
#if CI_CONDATTR_MONOTONIC
  static pthread_condattr_t ca;
  static int initialized = 0;
  if (!initialized) {
    pthread_condattr_init(&ca);
    if (pthread_condattr_setclock(&ca, CLOCK_MONOTONIC) != 0) {
      initialized = -1;
    } else {
      initialized = 1;
    }
  }
  return initialized == 1 ? &ca : NULL;
#else
  return NULL;
#endif
}

int _PyThread_cond_init(PyCOND_T* cond) {
  return pthread_cond_init(cond, ci_condattr());
}

void _PyThread_cond_after(long long us, struct timespec* abs) {
#if CI_CONDATTR_MONOTONIC
  if (ci_condattr() != NULL) {
    clock_gettime(CLOCK_MONOTONIC, abs);
    abs->tv_sec += us / 1000000;
    abs->tv_nsec += (us % 1000000) * 1000;
    if (abs->tv_nsec >= 1000000000) {
      abs->tv_sec += 1;
      abs->tv_nsec -= 1000000000;
    }
    return;
  }
#endif
  struct timeval tv;
  gettimeofday(&tv, NULL);
  abs->tv_sec = tv.tv_sec + (tv.tv_usec + us) / 1000000;
  abs->tv_nsec = ((tv.tv_usec + us) % 1000000) * 1000;
}

// ---------------------------------------------------------------------------
// Aliases for helpers already mirrored in UpstreamBorrow's 3.11 fallback.
// (_PyAsyncGenValueWrapperNew comes verbatim via the extras generator; the
// fallback's NotImplementedError stub is no longer referenced from here.)

extern void Cix_PyThreadState_PopFrame(
    PyThreadState* tstate,
    _PyInterpreterFrame* frame);

void _PyThreadState_PopFrame(PyThreadState* tstate, _PyInterpreterFrame* frame) {
  Cix_PyThreadState_PopFrame(tstate, frame);
}

// ---------------------------------------------------------------------------
// Objects/obmalloc.c: _PyObject_VirtualAlloc
//
// Upstream forwards to the arena allocator, whose default Linux backend is
// exactly this anonymous mmap. Used by the vendored datastack push path;
// the matching frees (upstream _PyObject_VirtualFree / the fallback's
// datastack pop) are munmap, so allocator pairing is preserved.

#include <sys/mman.h>

void* _PyObject_VirtualAlloc(size_t size) {
  void* ptr = mmap(
      NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (ptr == MAP_FAILED) {
    return NULL;
  }
  return ptr;
}

// ---------------------------------------------------------------------------
// Seed for the [P2] dict-version shadow counter in cinderx_ceval.c: read the
// runtime allocator's current position off a probe dict and park our shadow
// a 2^40 gap above it. Called from Ci_InitOpcodes at module exec.

uint64_t ci_pydict_global_version_shadow;

int Ci_SeedDictVersionShadow(void) {
  PyObject* probe = PyDict_New();
  if (probe == NULL) {
    return -1;
  }
  PyObject* key = PyUnicode_FromString("ci_version_probe");
  if (key == NULL || PyDict_SetItem(probe, key, Py_None) < 0) {
    Py_XDECREF(key);
    Py_DECREF(probe);
    return -1;
  }
  ci_pydict_global_version_shadow =
      ((PyDictObject*)probe)->ma_version_tag + ((uint64_t)1 << 40);
  Py_DECREF(key);
  Py_DECREF(probe);
  return 0;
}
