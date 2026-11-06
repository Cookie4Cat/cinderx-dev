#define Py_BUILD_CORE
#include <Python.h>
#include "internal/pycore_dict.h"
static PyObject* snapshot(PyObject* self, PyObject* obj) {
    if (!PyDict_CheckExact(obj)) {
        PyErr_SetString(PyExc_TypeError, "exact dict required"); return NULL;
    }
    PyDictObject* d = (PyDictObject*)obj;
    return Py_BuildValue("{s:k,s:K,s:K}", "version", (unsigned long)d->ma_keys->dk_version,
        "keys_address", (unsigned long long)(uintptr_t)d->ma_keys,
        "dict_version", (unsigned long long)d->ma_version_tag);
}
static PyObject* call_n(PyObject* self, PyObject* args) {
    PyObject* fn; PyObject* result = NULL; int count;
    if (!PyArg_ParseTuple(args, "Oi", &fn, &count)) return NULL;
    for (int i = 0; i < count; i++) {
        Py_XDECREF(result); result = PyObject_CallNoArgs(fn);
        if (result == NULL) return NULL;
    }
    if (result == NULL) Py_RETURN_NONE;
    return result;
}
static PyMethodDef methods[] = {
    {"snapshot", snapshot, METH_O, NULL}, {"call_n", call_n, METH_VARARGS, NULL}, {NULL}
};
static struct PyModuleDef module = {PyModuleDef_HEAD_INIT, "_key_probe", NULL, -1, methods};
PyMODINIT_FUNC PyInit__key_probe(void) { return PyModule_Create(&module); }
