// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/UpstreamBorrow/borrowed.h"

#define NEED_OPCODE_TABLES
#include "cinderx/Interpreter/cinder_opcode.h"

#include "internal/pycore_genobject.h"

#include <assert.h>
#include <limits.h>
#include <stdlib.h>

getattrofunc Ci_tp_getattr_hook;

#ifdef ENABLE_PEP523_HOOK
_PyFrameEvalFunction Ci_EvalFrameFunc;
#else
#define Ci_EvalFrameFunc NULL
#endif

#define CIX_DATA_STACK_CHUNK_SIZE (16 * 1024)
#define CIX_MINIMUM_OVERHEAD 1000

static _PyStackChunk* Cix_allocate_datastack_chunk(
    int size_in_bytes,
    _PyStackChunk* previous) {
  assert(size_in_bytes % sizeof(PyObject**) == 0);
  PyObjectArenaAllocator arena;
  PyObject_GetArenaAllocator(&arena);
  _PyStackChunk* res = arena.alloc(arena.ctx, size_in_bytes);
  if (res == NULL) {
    return NULL;
  }
  res->previous = previous;
  res->size = size_in_bytes;
  res->top = 0;
  return res;
}

static PyObject** Cix_datastack_chunk_start(_PyStackChunk* chunk) {
  return &chunk->data[chunk->previous == NULL];
}

static PyObject** Cix_datastack_chunk_limit(_PyStackChunk* chunk) {
  return (PyObject**)(((char*)chunk) + chunk->size);
}

static int Cix_datastack_top_in_chunk(
    PyObject** top,
    _PyStackChunk* chunk) {
  return top >= Cix_datastack_chunk_start(chunk) &&
      top <= Cix_datastack_chunk_limit(chunk);
}

static void Cix_free_datastack_chunk(_PyStackChunk* chunk) {
  PyObjectArenaAllocator arena;
  PyObject_GetArenaAllocator(&arena);
  arena.free(arena.ctx, chunk, chunk->size);
}

static void Cix_normalize_datastack(PyThreadState* tstate) {
  PyObject** top = tstate->datastack_top;
  _PyStackChunk* chunk = tstate->datastack_chunk;
  if (top == NULL || chunk == NULL) {
    return;
  }

  if (Cix_datastack_top_in_chunk(top, chunk)) {
    tstate->datastack_limit = Cix_datastack_chunk_limit(chunk);
    return;
  }

  _PyStackChunk* owner = chunk->previous;
  while (owner != NULL && !Cix_datastack_top_in_chunk(top, owner)) {
    owner = owner->previous;
  }
  if (owner == NULL) {
    return;
  }

  while (chunk != owner) {
    _PyStackChunk* previous = chunk->previous;
    Cix_free_datastack_chunk(chunk);
    chunk = previous;
  }
  tstate->datastack_chunk = owner;
  tstate->datastack_limit = Cix_datastack_chunk_limit(owner);
}

static PyObject** Cix_push_datastack_chunk(PyThreadState* tstate, int size) {
  int allocate_size = CIX_DATA_STACK_CHUNK_SIZE;
  while (allocate_size <
         (int)sizeof(PyObject*) * (size + CIX_MINIMUM_OVERHEAD)) {
    allocate_size *= 2;
  }
  _PyStackChunk* new_chunk =
      Cix_allocate_datastack_chunk(allocate_size, tstate->datastack_chunk);
  if (new_chunk == NULL) {
    return NULL;
  }
  if (tstate->datastack_chunk != NULL) {
    tstate->datastack_chunk->top =
        tstate->datastack_top - &tstate->datastack_chunk->data[0];
  }
  tstate->datastack_chunk = new_chunk;
  tstate->datastack_limit =
      (PyObject**)(((char*)new_chunk) + allocate_size);
  PyObject** res = &new_chunk->data[new_chunk->previous == NULL];
  tstate->datastack_top = res + size;
  return res;
}

PyObject* Cix_PyAsyncGenValueWrapperNew(PyObject* value) {
  (void)value;
  PyErr_SetString(
      PyExc_NotImplementedError,
      "async generator value wrapping is not supported by the stock Python "
      "3.11 CinderX fallback");
  return NULL;
}

PyObject* Cix_compute_cr_origin(
    int origin_depth,
    _PyInterpreterFrame* current_frame) {
  (void)origin_depth;
  (void)current_frame;
  Py_RETURN_NONE;
}

_PyInterpreterFrame* Cix_PyThreadState_PushFrame(
    PyThreadState* tstate,
    size_t size) {
  Cix_normalize_datastack(tstate);
  if (_PyThreadState_HasStackSpace(tstate, size)) {
    _PyInterpreterFrame* res = (_PyInterpreterFrame*)tstate->datastack_top;
    tstate->datastack_top += size;
    return res;
  }
  if (size > INT_MAX / 2) {
    PyErr_NoMemory();
    return NULL;
  }
  return (_PyInterpreterFrame*)Cix_push_datastack_chunk(tstate, (int)size);
}

void Cix_PyThreadState_PopFrame(
    PyThreadState* tstate,
    _PyInterpreterFrame* frame) {
  assert(tstate->datastack_chunk);
  PyObject** base = (PyObject**)frame;
  if (base == &tstate->datastack_chunk->data[0]) {
    _PyStackChunk* chunk = tstate->datastack_chunk;
    _PyStackChunk* previous = chunk->previous;
    // 根 chunk 保持分配（与 CPython pystate.c 语义一致）
    if (previous != NULL) {
      tstate->datastack_top = &previous->data[previous->top];
      tstate->datastack_chunk = previous;
      Cix_free_datastack_chunk(chunk);
      tstate->datastack_limit = Cix_datastack_chunk_limit(previous);
    }
  } else {
    assert(tstate->datastack_top >= base);
    tstate->datastack_top = base;
  }
}

PyObject* _PyGen_yf(PyGenObject* gen) {
  if (gen->gi_frame_state == FRAME_SUSPENDED) {
    _PyInterpreterFrame* frame = (_PyInterpreterFrame*)gen->gi_iframe;
    PyObject* yf = _PyFrame_StackPeek(frame);
    return yf == Py_None ? NULL : yf;
  }
  return NULL;
}

PyObject* _PyCoro_GetAwaitableIter(PyObject* o) {
  unaryfunc getter = NULL;

  if (PyCoro_CheckExact(o)) {
    return Py_NewRef(o);
  }
  if (PyGen_CheckExact(o) &&
      (((PyGenObject*)o)->gi_code->co_flags & CO_ITERABLE_COROUTINE)) {
    return Py_NewRef(o);
  }

  PyTypeObject* ot = Py_TYPE(o);
  if (ot->tp_as_async != NULL) {
    getter = ot->tp_as_async->am_await;
  }
  if (getter != NULL) {
    PyObject* res = (*getter)(o);
    if (res != NULL) {
      if (PyCoro_CheckExact(res) ||
          (PyGen_CheckExact(res) &&
           (((PyGenObject*)res)->gi_code->co_flags & CO_ITERABLE_COROUTINE))) {
        PyErr_SetString(PyExc_TypeError, "__await__() returned a coroutine");
        Py_CLEAR(res);
      } else if (!PyIter_Check(res)) {
        PyErr_Format(
            PyExc_TypeError,
            "__await__() returned non-iterator of type '%.100s'",
            Py_TYPE(res)->tp_name);
        Py_CLEAR(res);
      }
    }
    return res;
  }

  PyErr_Format(
      PyExc_TypeError,
      "object %.100s can't be used in 'await' expression",
      Py_TYPE(o)->tp_name);
  return NULL;
}

PyObject* _PyDict_LoadGlobal(
    PyDictObject* globals,
    PyDictObject* builtins,
    PyObject* key) {
  PyObject* value =
      PyDict_GetItemWithError((PyObject*)globals, key);
  if (value != NULL || PyErr_Occurred()) {
    return value;
  }
  return PyDict_GetItemWithError((PyObject*)builtins, key);
}

int _PyObjectDict_SetItem(
    PyTypeObject* tp,
    PyObject** dictptr,
    PyObject* key,
    PyObject* value) {
  (void)tp;
  if (*dictptr == NULL) {
    if (value == NULL) {
      PyErr_SetObject(PyExc_AttributeError, key);
      return -1;
    }
    *dictptr = PyDict_New();
    if (*dictptr == NULL) {
      return -1;
    }
  }
  if (value == NULL) {
    return PyDict_DelItem(*dictptr, key);
  }
  return PyDict_SetItem(*dictptr, key, value);
}

PyObject* _PyTuple_FromArray(PyObject* const* src, Py_ssize_t n) {
  PyObject* tuple = PyTuple_New(n);
  if (tuple == NULL) {
    return NULL;
  }
  for (Py_ssize_t i = 0; i < n; i++) {
    PyObject* item = Py_NewRef(src[i]);
    PyTuple_SET_ITEM(tuple, i, item);
  }
  return tuple;
}

// _PyFrame_MakeAndSetFrameObject now comes verbatim from the vendored
// Interpreter/3.11/ceval/frame.c (M2); the earlier NotImplementedError stub
// that lived here is gone.

// 3.11 has no _PyFrame_ClearExceptCode (that name is 3.13+); the verbatim
// 3.11 clear is _PyFrame_Clear in the vendored Interpreter/3.11/ceval/frame.c,
// which also handles the escaped-frame take_ownership transfer (data copy
// into the PyFrameObject, f_back linking, GC tracking) that a naive
// field-clearing mirror misses -- skipping it leaves escaped frame objects
// pointing at a dead interpreter frame.
//
// Callers own the code reference separately (cleanupFrameExecutable), so
// balance _PyFrame_Clear's Py_DECREF(f_code) with a pre-incref. On the
// take_ownership early-return path the copied frame keeps the extra code
// reference, which the caller's cleanup then consumes; both paths balance.
void _PyFrame_ClearExceptCode(_PyInterpreterFrame* frame) {
  Py_INCREF(frame->f_code);
  _PyFrame_Clear(frame);
}

#define ASSERT_VALID_BOUNDS(bounds)

void _PyLineTable_InitAddressRange(
    const char* linetable,
    Py_ssize_t length,
    int firstlineno,
    PyCodeAddressRange* range) {
  range->opaque.lo_next = (const uint8_t*)linetable;
  range->opaque.limit = range->opaque.lo_next + length;
  range->ar_start = -1;
  range->ar_end = 0;
  range->opaque.computed_line = firstlineno;
  range->ar_line = -1;
}

int _PyCode_InitAddressRange(PyCodeObject* co, PyCodeAddressRange* bounds) {
  assert(co->co_linetable != NULL);
  const char* linetable = PyBytes_AS_STRING(co->co_linetable);
  Py_ssize_t length = PyBytes_GET_SIZE(co->co_linetable);
  _PyLineTable_InitAddressRange(
      linetable, length, co->co_firstlineno, bounds);
  return bounds->ar_line;
}

static int scan_varint(const uint8_t* ptr) {
  unsigned int read = *ptr++;
  unsigned int val = read & 63;
  unsigned int shift = 0;
  while (read & 64) {
    read = *ptr++;
    shift += 6;
    val |= (read & 63) << shift;
  }
  return val;
}

static int scan_signed_varint(const uint8_t* ptr) {
  unsigned int uval = scan_varint(ptr);
  if (uval & 1) {
    return -(int)(uval >> 1);
  }
  return uval >> 1;
}

static int get_line_delta(const uint8_t* ptr) {
  int code = ((*ptr) >> 3) & 15;
  switch (code) {
    case PY_CODE_LOCATION_INFO_NONE:
      return 0;
    case PY_CODE_LOCATION_INFO_NO_COLUMNS:
    case PY_CODE_LOCATION_INFO_LONG:
      return scan_signed_varint(ptr + 1);
    case PY_CODE_LOCATION_INFO_ONE_LINE0:
      return 0;
    case PY_CODE_LOCATION_INFO_ONE_LINE1:
      return 1;
    case PY_CODE_LOCATION_INFO_ONE_LINE2:
      return 2;
    default:
      return 0;
  }
}

static int is_no_line_marker(uint8_t b) {
  return (b >> 3) == 0x1f;
}

static int next_code_delta(PyCodeAddressRange* bounds) {
  assert((*bounds->opaque.lo_next) & 128);
  return (((*bounds->opaque.lo_next) & 7) + 1) * sizeof(_Py_CODEUNIT);
}

static inline int at_end(PyCodeAddressRange* bounds) {
  return bounds->opaque.lo_next >= bounds->opaque.limit;
}

static void advance(PyCodeAddressRange* bounds) {
  ASSERT_VALID_BOUNDS(bounds);
  bounds->opaque.computed_line += get_line_delta(bounds->opaque.lo_next);
  if (is_no_line_marker(*bounds->opaque.lo_next)) {
    bounds->ar_line = -1;
  } else {
    bounds->ar_line = bounds->opaque.computed_line;
  }
  bounds->ar_start = bounds->ar_end;
  bounds->ar_end += next_code_delta(bounds);
  do {
    bounds->opaque.lo_next++;
  } while (bounds->opaque.lo_next < bounds->opaque.limit &&
           ((*bounds->opaque.lo_next) & 128) == 0);
  ASSERT_VALID_BOUNDS(bounds);
}

int _PyLineTable_NextAddressRange(PyCodeAddressRange* range) {
  if (at_end(range)) {
    return 0;
  }
  advance(range);
  assert(range->ar_end > range->ar_start);
  return 1;
}

int Cix_PyObjectDict_SetItem(
    PyTypeObject* tp,
    PyObject* obj,
    PyObject** dictptr,
    PyObject* key,
    PyObject* value) {
  (void)obj;
  return _PyObjectDict_SetItem(tp, dictptr, key, value);
}

void Cix_PyDict_SendEvent(
    int watcher_bits,
    PyDict_WatchEvent event,
    PyDictObject* mp,
    PyObject* key,
    PyObject* value) {
  (void)watcher_bits;
  (void)event;
  (void)mp;
  (void)key;
  (void)value;
}

int Cix_set_attribute_error_context(PyObject* v, PyObject* name) {
  (void)v;
  (void)name;
  return 0;
}

PyObject* Cix_match_class(
    PyThreadState* tstate,
    PyObject* subject,
    PyObject* type,
    Py_ssize_t nargs,
    PyObject* kwargs) {
  (void)tstate;
  (void)subject;
  (void)type;
  (void)nargs;
  (void)kwargs;
  PyErr_SetString(
      PyExc_RuntimeError,
      "MATCH_CLASS is not supported by the stock Python 3.11 CinderX fallback");
  return NULL;
}

PyObject* Cix_match_keys(PyThreadState* tstate, PyObject* map, PyObject* keys) {
  (void)tstate;
  (void)map;
  (void)keys;
  PyErr_SetString(
      PyExc_RuntimeError,
      "MATCH_KEYS is not supported by the stock Python 3.11 CinderX fallback");
  return NULL;
}

void Cix_format_kwargs_error(
    PyThreadState* tstate,
    PyObject* func,
    PyObject* kwargs) {
  (void)tstate;
  PyErr_Format(
      PyExc_TypeError,
      "%R got unexpected keyword arguments %R",
      func,
      kwargs);
}

void Cix_format_exc_check_arg(
    PyThreadState* tstate,
    PyObject* exc,
    const char* format_str,
    PyObject* obj) {
  (void)tstate;
  PyErr_Format(exc, format_str, obj);
}

PyObject* Ci_Builtin_Next_Core(PyObject* it, PyObject* def) {
  PyObject* item = (*Py_TYPE(it)->tp_iternext)(it);
  if (item != NULL) {
    return item;
  }
  if (def != NULL && PyErr_ExceptionMatches(PyExc_StopIteration)) {
    PyErr_Clear();
    return Py_NewRef(def);
  }
  return NULL;
}

PyObject* Ci_static_rand(PyObject* self) {
  (void)self;
  return PyLong_FromLong(rand());
}

int init_upstream_borrow(void) {
  const char* code_str =
      "class GetAttr:\n"
      "    def __getattr__(self, name): pass\n";

  PyObject* code = NULL;
  PyObject* globals = NULL;
  int result = -1;
  code = Py_CompileString(code_str, "cinderx_getattr_init.py", Py_file_input);
  if (code == NULL) {
    goto error;
  }
  globals = PyDict_New();
  if (globals == NULL) {
    goto error;
  }

  PyObject* eval_result = PyEval_EvalCode(code, globals, globals);
  if (eval_result == NULL) {
    goto error;
  }
  Py_DECREF(eval_result);

  PyObject* getattr = PyDict_GetItemString(globals, "GetAttr");
  if (getattr == NULL || Py_TYPE(getattr) != &PyType_Type) {
    PyErr_SetString(
        PyExc_RuntimeError, "failed to initialize GetAttr: class not defined");
    goto error;
  }

  Ci_tp_getattr_hook = ((PyTypeObject*)getattr)->tp_getattro;
  if (Ci_tp_getattr_hook == NULL) {
    PyErr_SetString(
        PyExc_RuntimeError,
        "failed to initialize GetAttr: got NULL tp_getattro");
    goto error;
  }

  result = 0;

error:
  Py_XDECREF(code);
  Py_XDECREF(globals);
  return result;
}
