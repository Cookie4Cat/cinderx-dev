// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/RuntimeTests/fixtures.h"
#include "cinderx/StaticPython/checked_dict.h"
#include "cinderx/StaticPython/checked_list.h"
#include "cinderx/StaticPython/thunks.h"
#include "cinderx/StaticPython/type.h"
#include "cinderx/StaticPython/type_code.h"
#include "cinderx/StaticPython/vtable_defs.h"

#include <cstdint>
#include <cstring>

extern "C" int Ci_CheckedDict_CheckConsistency(PyObject* op, int check_content);

class SanityTest : public RuntimeTest {};

class NoJitSanityTest : public RuntimeTest {
 public:
  NoJitSanityTest() : RuntimeTest(static_cast<Flags>(0)) {}
};

class StaticSanityTest : public RuntimeTest {
 public:
  StaticSanityTest() : RuntimeTest(kStaticCompiler) {}
};

namespace {

Ref<> importModule(const char* name) {
  auto mod = Ref<>::steal(PyImport_ImportModule(name));
  if (mod == nullptr) {
    PyErr_Print();
  }
  return mod;
}

Ref<> checkedListType(PyObject* elem_type) {
  auto mod = importModule("__static__");
  if (mod == nullptr) {
    return nullptr;
  }
  auto generic = Ref<>::steal(PyObject_GetAttrString(mod, "CheckedList"));
  if (generic == nullptr) {
    PyErr_Print();
    return nullptr;
  }
  auto type = Ref<>::steal(PyObject_GetItem(generic, elem_type));
  if (type == nullptr) {
    PyErr_Print();
  }
  return type;
}

Ref<> checkedDictType(PyObject* key_type, PyObject* value_type) {
  auto mod = importModule("__static__");
  if (mod == nullptr) {
    return nullptr;
  }
  auto generic = Ref<>::steal(PyObject_GetAttrString(mod, "CheckedDict"));
  if (generic == nullptr) {
    PyErr_Print();
    return nullptr;
  }
  auto args = Ref<>::steal(PyTuple_Pack(2, key_type, value_type));
  if (args == nullptr) {
    return nullptr;
  }
  auto type = Ref<>::steal(PyObject_GetItem(generic, args));
  if (type == nullptr) {
    PyErr_Print();
  }
  return type;
}

Ref<> callMethodNoArgs(PyObject* obj, const char* name) {
  auto name_obj = Ref<>::steal(PyUnicode_FromString(name));
  if (name_obj == nullptr) {
    return nullptr;
  }
  auto method = Ref<>::steal(PyObject_GetAttr(obj, name_obj));
  if (method == nullptr) {
    PyErr_Print();
    return nullptr;
  }
  auto result = Ref<>::steal(PyObject_CallNoArgs(method));
  if (result == nullptr) {
    PyErr_Print();
  }
  return result;
}

} // namespace

TEST_F(SanityTest, CanUsePrivateAPIs) {
  auto g = Ref<>::steal(PyLong_FromLong(100));
  ASSERT_NE(g.get(), nullptr);
  ASSERT_TRUE(PyLong_CheckExact(g.get()));
  ASSERT_EQ(PyLong_AsInt(g.get()), 100);
}

TEST_F(SanityTest, CanReinitRuntime) {
  TearDown();
  SetUp();
}

TEST_F(SanityTest, JitPythonShapeCoverage) {
  runStockCode(R"(
import cinderx.jit as jit

jit.compile_after_n_calls(0)

class SlotBox:
    __slots__ = ("x", "y")

    def __init__(self, x, y):
        self.x = x
        self.y = y

    def bump(self, scale):
        self.x += scale
        return self.x + self.y

class DictBox:
    def __init__(self, value):
        self.value = value

    def mix(self, other):
        self.value += other.value
        return self.value

def arithmetic(a, b):
    total = 0
    for i in range(20):
        if i & 1:
            total += (a + i) * b
        else:
            total -= (b - i)
    return total

def container_ops(seed):
    values = [seed, seed + 1, seed + 2]
    values.append(seed + 3)
    values[1] = values[1] + values[-1]
    seen = {"a": values[0], "b": values[1]}
    seen["c"] = seen.get("a", 0) + seen.pop("b")
    total = 0
    for key, value in seen.items():
        if key in seen:
            total += value
    return total + len(values)

def attr_and_calls(n):
    slot = SlotBox(n, n + 1)
    left = DictBox(n)
    right = DictBox(n + 2)
    total = slot.bump(3)
    total += left.mix(right)
    total += getattr(slot, "x")
    setattr(slot, "y", total)
    return slot.y

def exception_edges(n):
    total = 0
    for i in range(8):
        try:
            if i == n:
                raise ValueError(i)
            total += 10 // (i + 1)
        except ValueError as exc:
            total += exc.args[0]
        finally:
            total += 1
    return total

def nested_calls(n):
    def inner(x):
        return arithmetic(x, 3) + container_ops(x)
    total = 0
    for i in range(n):
        total += inner(i)
    return total

def bool_and_compare(a, b):
    if a is None:
        return b
    if isinstance(a, int) and a < b:
        return b - a
    return a == b

for i in range(80):
    assert arithmetic(i, 3) != 0
    assert container_ops(i) >= 0
    assert attr_and_calls(i) >= 0
    assert exception_edges(i % 8) >= 0
    assert nested_calls(4) != 0
    assert bool_and_compare(i, i + 1) == 1

for func in (
    arithmetic,
    container_ops,
    attr_and_calls,
    exception_edges,
    nested_calls,
    bool_and_compare,
):
    assert jit.is_jit_compiled(func), func
)");
}

TEST_F(SanityTest, AsyncLazyValueCoverage) {
  runStockCode(R"(
import _cinderx
import asyncio

async def compute(value, *, scale=1):
    await asyncio.sleep(0)
    return value * scale + 3

async def raises():
    await asyncio.sleep(0)
    raise RuntimeError("boom")

async def drive_success():
    value = _cinderx.AsyncLazyValue(compute, 7, scale=2)
    assert value.alv_state == "not_started"
    assert value._awaiting_tasks == 0
    assert await value == 17
    assert value.alv_state == "done"
    assert value._awaiting_tasks == 0
    assert await value == 17
    value._link()
    value._unlink()
    return 1

async def drive_error():
    value = _cinderx.AsyncLazyValue(raises)
    try:
        await value
    except RuntimeError as exc:
        assert exc.args == ("boom",)
    assert value.alv_state == "not_started"
    return 2

async def main():
    total = 0
    for _ in range(20):
        total += await drive_success()
    total += await drive_error()
    return total

assert asyncio.run(main()) == 22
)");
}

TEST_F(SanityTest, JitPublicApiCoverage) {
  runStockCode(R"(
import cinderx.jit as jit

def target(x):
    return x + 1

def suppressed(x):
    return x * 2

jit.compile_after_n_calls(0)
assert jit.get_compile_after_n_calls() == 0
assert jit.is_enabled()
assert jit.force_compile(target)
assert jit.is_jit_compiled(target)
assert target(41) == 42
assert isinstance(jit.get_compiled_functions(), list)
assert jit.get_compiled_size(target) >= 0
assert jit.get_compiled_stack_size(target) >= 0
assert jit.get_compiled_spill_stack_size(target) >= 0
assert jit.get_function_compilation_time(target) >= 0
assert isinstance(jit.get_function_hir_opcode_counts(target), dict)
assert isinstance(jit.get_num_inlined_functions(target), int)
assert isinstance(jit.get_inlined_functions_stats(target), dict)
assert isinstance(jit.get_compilation_time(), int)
assert isinstance(jit.get_allocator_stats(), dict)
assert isinstance(jit.page_in_profiler_dependencies(), list)
jit.mlock_profiler_dependencies()
assert isinstance(jit.get_and_clear_runtime_stats(), dict)
jit.clear_runtime_stats()
assert isinstance(jit.get_and_clear_inline_cache_stats(), dict)
assert isinstance(jit.is_inline_cache_stats_collection_enabled(), bool)
assert isinstance(jit.is_hir_inliner_enabled(), bool)
jit.disable_hir_inliner()
assert not jit.is_hir_inliner_enabled()
jit.enable_hir_inliner()
assert jit.is_hir_inliner_enabled()
jit.disable_emit_type_annotation_guards()
jit.enable_emit_type_annotation_guards()
jit.disable_specialized_opcodes()
jit.enable_specialized_opcodes()
jit.set_max_code_size(0)
assert jit.count_interpreted_calls(target) >= 0
assert jit.jit_suppress(suppressed) is suppressed
assert jit.jit_unsuppress(suppressed) is suppressed
assert jit.lazy_compile(suppressed) in (True, False)
suppressed(5)
jit.force_uncompile(target)
assert not jit.is_jit_compiled(target)
jit.append_jit_list("jittestmodule:target")
rules = jit.get_jit_list()
assert isinstance(rules, tuple)
jit.disable(False)
assert not jit.is_enabled()
jit.enable()
assert jit.is_enabled()
jit.auto()

def call_or_error(func, *args):
    try:
        func(*args)
    except (TypeError, ValueError, OverflowError, RuntimeError, OSError):
        return False
    return True

for bad in (None, target, -1, 10_000_000_000):
    call_or_error(jit.compile_after_n_calls, bad)
for api in (
    jit.force_compile,
    jit.force_uncompile,
    jit.lazy_compile,
    jit.is_jit_compiled,
    jit.count_interpreted_calls,
    jit.get_compiled_size,
    jit.get_compiled_stack_size,
    jit.get_compiled_spill_stack_size,
    jit.get_function_compilation_time,
    jit.get_function_hir_opcode_counts,
    jit.get_inlined_functions_stats,
    jit.get_num_inlined_functions,
):
    call_or_error(api, 42)
call_or_error(jit.append_jit_list, "")
call_or_error(jit.append_jit_list, "not a valid jit list line")
call_or_error(jit.read_jit_list, "/tmp/does-not-exist-cinderx-jit-list")
call_or_error(jit.set_max_code_size, -1)
call_or_error(jit.disassemble, target)
)");
}

TEST_F(SanityTest, JitAdvancedPythonShapeCoverage) {
  runStockCode(R"(
import cinderx.jit as jit

jit.compile_after_n_calls(0)

class Manager:
    def __init__(self, value):
        self.value = value

    def __enter__(self):
        return self.value

    def __exit__(self, exc_type, exc, tb):
        return False

class Parent:
    def calc(self, x):
        return x + 1

class Child(Parent):
    def calc(self, x):
        return super().calc(x) * 2

def comprehensions(n):
    xs = [i * 2 for i in range(n) if i % 2 == 0]
    ys = {str(i): i for i in xs}
    zs = {i for i in ys.values() if i > 2}
    return sum(xs) + sum(ys.values()) + sum(zs)

def closure_shape(base):
    total = base
    def inner(delta):
        nonlocal total
        total += delta
        return total
    return inner(3) + inner(4)

def with_shape(n):
    total = 0
    with Manager(n) as value:
        total += value
    return total

def unpack_shape(seq):
    first, *middle, last = seq
    return first + last + len(middle)

def kwargs_shape(a, *, b=1, c=2):
    return a + b + c

def call_kwargs_shape(n):
    data = {"b": n + 1, "c": n + 2}
    return kwargs_shape(n, **data)

def super_shape(n):
    obj = Child()
    return obj.calc(n)

def generator_shape(n):
    total = 0
    for value in (i * 3 for i in range(n)):
        total += value
    return total

def exception_group_shape(n):
    try:
        if n & 1:
            raise LookupError(n)
        raise KeyError(n)
    except LookupError as exc:
        return exc.args[0] + 10
    except KeyError as exc:
        return exc.args[0] + 20

def delete_shape(n):
    data = {"x": n, "y": n + 1}
    del data["x"]
    return data["y"]

funcs = (
    comprehensions,
    closure_shape,
    with_shape,
    unpack_shape,
    call_kwargs_shape,
    super_shape,
    generator_shape,
    exception_group_shape,
    delete_shape,
)

for func in funcs:
    jit.force_compile(func)

for i in range(100):
    assert comprehensions(8) > 0
    assert closure_shape(i) == (i + 3) + (i + 7)
    assert with_shape(i) == i
    assert unpack_shape([1, 2, 3, 4]) == 7
    assert call_kwargs_shape(i) == i * 3 + 3
    assert super_shape(i) == (i + 1) * 2
    assert generator_shape(6) == 45
    assert exception_group_shape(i) >= 10
    assert delete_shape(i) == i + 1
)");
}

TEST_F(SanityTest, JitRuntimeCallEdgeCoverage) {
  runStockCode(R"(
import asyncio
import cinderx.jit as jit

jit.compile_after_n_calls(0)

def callee(a, b=2, *args, c=3, **kw):
    total = a + b + c + sum(args)
    for value in kw.values():
        total += value
    return total

def kwonly(a, *, required, default=5):
    return a + required + default

def call_keywords(n):
    return callee(n, n + 1, n + 2, n + 3, c=n + 4, extra=n + 5)

def call_unpack(n):
    args = (n + 1, n + 2)
    kwargs = {"c": n + 3, "extra": n + 4}
    return callee(n, *args, **kwargs)

def call_kwonly(n):
    return kwonly(n, required=n + 1) + kwonly(n, required=n + 2, default=n + 3)

def call_error_edges(n):
    total = 0
    try:
        kwonly(n)
    except TypeError:
        total += 1
    try:
        kwonly(n, required=n, missing=n)
    except TypeError:
        total += 2
    try:
        callee(n, **{1: 2})
    except TypeError:
        total += 4
    return total

def make_closure(seed):
    value = seed
    def inner(delta=1):
        return value + delta
    return inner

def call_closure(n):
    fn = make_closure(n)
    return fn() + fn(3)

async def async_callee(n, *, step=1):
    await asyncio.sleep(0)
    return n + step

async def async_driver(n):
    total = 0
    for i in range(n):
        total += await async_callee(i, step=i + 1)
    return total

def gen_driver(n):
    def gen():
        for i in range(n):
            yield callee(i, c=i + 1)
    return sum(gen())

for func in (
    callee,
    kwonly,
    call_keywords,
    call_unpack,
    call_kwonly,
    call_error_edges,
    call_closure,
    async_callee,
    async_driver,
    gen_driver,
):
    jit.force_compile(func)

for i in range(80):
    assert call_keywords(i) == i * 6 + 15
    assert call_unpack(i) == i * 5 + 10
    assert call_kwonly(i) == i * 5 + 11
    assert call_error_edges(i) == 7
    assert call_closure(i) == i * 2 + 4
    assert gen_driver(5) == 35

assert asyncio.run(async_driver(5)) == 25
)");
}

TEST_F(SanityTest, JitBranchDensePythonCoverage) {
  runStockCode(R"(
import cinderx.jit as jit

jit.compile_after_n_calls(0)

class Descriptor:
    def __get__(self, obj, typ=None):
        if obj is None:
            return self
        return obj.value + 3

class Base:
    def method(self, x):
        return x + 5

class Child(Base):
    extra = Descriptor()

    def __init__(self, value):
        self.value = value

    def method(self, x):
        return super().method(x) + self.extra

class Rich:
    marker = 4

    def __init__(self, value):
        self.value = value

    @property
    def doubled(self):
        return self.value * 2

    @classmethod
    def make(cls, value):
        return cls(value + cls.marker)

    @staticmethod
    def adjust(value):
        return value + 7

    def __call__(self, extra):
        return self.value + extra

class Manager:
    def __init__(self, value, suppress=False):
        self.value = value
        self.suppress = suppress

    def __enter__(self):
        return self.value

    def __exit__(self, exc_type, exc, tb):
        return self.suppress

def numeric_mix(a, b):
    total = 0
    total += a + b
    total += a - b
    total += a * b
    total += (a + 20) // (b + 1)
    total += (a + 20) % (b + 1)
    total += (a | b) ^ (a & b)
    total += (a << 1) >> 1
    total += -a if a & 1 else +b
    return total

def compare_mix(a, b, c):
    total = 0
    if a < b <= c:
        total += 1
    if a != b and b in (a, b, c):
        total += 2
    if None is not c:
        total += 4
    return total

def container_mix(seed):
    xs = [seed, seed + 1, seed + 2]
    xs[0:2] = [xs[1], xs[0]]
    ys = {value: value * 2 for value in xs}
    zs = {value for value in ys.values() if value % 2 == 0}
    total = sum(xs) + sum(ys.values()) + sum(zs)
    total += len((*xs, seed + 9))
    left, *middle, right = xs + [seed + 7]
    return total + left + right + len(middle)

def loop_mix(n):
    total = 0
    for i in range(n):
        if i % 5 == 0:
            continue
        if i > 17:
            break
        total += i
    else:
        total -= 100
    while n > 0:
        total += n & 3
        n -= 3
    return total

def exception_mix(n):
    total = 0
    for i in range(4):
        try:
            if i == n:
                raise KeyError(i)
            if i + n == 5:
                raise RuntimeError(i)
            total += 10 // (i + 1)
        except KeyError as exc:
            total += exc.args[0] + 11
        except RuntimeError:
            total += 13
        finally:
            total += 1
    return total

def call_mix(n):
    child = Child(n)
    values = [numeric_mix(i + 3, (i % 4) + 1) for i in range(6)]
    return child.method(n) + sum(map(abs, values)) + max(values) - min(values)

def closure_mix(seed):
    total = seed
    def add(delta):
        nonlocal total
        total += delta
        return total
    return add(1) + add(2) + add(3)

def match_mix(value):
    match value:
        case 0:
            return 10
        case 1 | 2:
            return 20
        case [first, second]:
            return first + second
        case {"x": x}:
            return x
        case _:
            return 30

def bool_mix(a, b):
    return bool((a and b) or (not a and not b))

def format_mix(n):
    text = f"{n}:{n + 1!r}:{n / 2:.1f}"
    return len(text)

def import_global_mix(n):
    return len(str(n)) + globals().get("__name__").count("jittest")

def attribute_descriptor_mix(n):
    obj = Rich.make(n)
    setattr(obj, "dynamic", n + 9)
    total = obj.doubled + obj.dynamic + obj(3) + Rich.adjust(n)
    if hasattr(obj, "dynamic"):
        total += getattr(obj, "missing", 5)
    del obj.dynamic
    return total + (0 if hasattr(obj, "dynamic") else 11)

def slice_unpack_mix(n):
    data = list(range(n, n + 8))
    data[1:6:2] = [30, 31, 32]
    rev = data[::-1]
    head, second, *middle, tail = rev
    return head + second + tail + len(middle) + sum(data[2:5])

def dict_merge_mix(n):
    left = {"a": n, "b": n + 1}
    right = {"b": n + 2, "c": n + 3}
    merged = left | right
    left |= {"d": n + 4}
    built = dict(**{"x": n + 5}, y=n + 6)
    return merged["a"] + merged["b"] + merged["c"] + left["d"] + built["x"] + built["y"]

def nested_exception_mix(n):
    total = 0
    try:
        try:
            if n % 2:
                raise ValueError(n)
            raise TypeError(n)
        except ValueError as exc:
            total += exc.args[0] + 3
            raise
        except TypeError as exc:
            total += exc.args[0] + 5
    except ValueError:
        total += 7
    return total

def with_manager_mix(n):
    total = 0
    with Manager(n) as value:
        total += value
    try:
        with Manager(n + 1, suppress=True) as value:
            total += value
            raise LookupError(value)
    except LookupError:
        total -= 100
    return total

def bytes_unicode_mix(n):
    raw = bytes([65 + (n % 20), 66, 67])
    text = raw.decode("ascii")
    return raw[0] + len(text.lower()) + text.find("B")

funcs = (
    numeric_mix,
    compare_mix,
    container_mix,
    loop_mix,
    exception_mix,
    call_mix,
    closure_mix,
    match_mix,
    bool_mix,
    format_mix,
    import_global_mix,
    attribute_descriptor_mix,
    slice_unpack_mix,
    dict_merge_mix,
    nested_exception_mix,
    with_manager_mix,
    bytes_unicode_mix,
)

compiled = []
for func in funcs:
    try:
        if jit.force_compile(func):
            compiled.append(func)
    except Exception:
        pass

for i in range(120):
    assert isinstance(numeric_mix(i + 4, (i % 5) + 1), int)
    assert compare_mix(i, i + 1, i + 2) == 7
    assert container_mix(i) > 0
    assert isinstance(loop_mix(i % 25), int)
    assert exception_mix(i % 6) > 0
    assert call_mix(i) > 0
    assert closure_mix(i) == i * 3 + 10
    assert match_mix(i % 3) in (10, 20)
    assert match_mix([i, i + 1]) == i * 2 + 1
    assert match_mix({"x": i}) == i
    assert isinstance(bool_mix(i & 1, i & 2), bool)
    assert format_mix(i) > 0
    assert import_global_mix(i) > 0
    assert attribute_descriptor_mix(i) == i * 5 + 47
    assert slice_unpack_mix(i) > 0
    assert dict_merge_mix(i) == i * 6 + 20
    assert nested_exception_mix(i) > 0
    assert with_manager_mix(i) == i * 2 + 1
    assert bytes_unicode_mix(i) > 0

for func in compiled:
    jit.get_function_hir_opcode_counts(func)
    jit.get_num_inlined_functions(func)
    jit.get_inlined_functions_stats(func)
    try:
        text = jit.disassemble(func)
        assert isinstance(text, str)
    except Exception:
        pass
)");
}

TEST_F(SanityTest, StaticModuleApiCoverage) {
  runStockCode(R"(
import __static__ as st
import contextlib

class C:
    pass

class D:
    pass

assert st.is_type_static(C) is False
assert st.set_type_static(C) is C
assert st.is_type_static(C) is True
assert st.set_type_final(D) is D
assert st.set_type_static_final(type("E", (), {})).__name__ == "E"
assert st.is_static_callable(lambda: 1) in (True, False)
assert st.is_static_module(st) in (True, False)
assert st.rand() <= st.RAND_MAX
if hasattr(st, "_sizeof_dlopen_cache"):
    assert st._sizeof_dlopen_cache() >= 0
    assert st._sizeof_dlsym_cache() >= 0
    st._clear_dlopen_cache()
    st._clear_dlsym_cache()

for func in (st.set_type_static, st.set_type_final, st.set_type_static_final):
    try:
        func(42)
    except TypeError:
        pass

try:
    st.is_type_static(42)
except TypeError:
    pass

)");
}

TEST_F(NoJitSanityTest, StockInterpreterDataModelCoverage) {
  runStockCode(R"(
import contextlib
import functools
import operator

class Descriptor:
    def __init__(self):
        self.values = {}

    def __get__(self, obj, typ=None):
        if obj is None:
            return self
        return self.values.get(id(obj), 0) + 5

    def __set__(self, obj, value):
        self.values[id(obj)] = value

    def __delete__(self, obj):
        self.values.pop(id(obj), None)

class Base:
    marker = "base"

    def method(self):
        return 2

    @classmethod
    def cls_value(cls):
        return cls.marker

    @staticmethod
    def static_value(x):
        return x + 7

class Derived(Base):
    __slots__ = ("slot",)
    value = Descriptor()
    marker = "derived"

    def __init__(self, value):
        self.slot = value
        self.value = value * 2

    def method(self):
        return super().method() + self.slot

    @property
    def prop(self):
        return self.slot + self.value

    def __len__(self):
        return self.slot

    def __bool__(self):
        return self.slot > 0

    def __iter__(self):
        yield from range(self.slot)

obj = Derived(4)
assert obj.method() == 6
assert obj.prop == 17
assert len(obj) == 4
assert list(obj) == [0, 1, 2, 3]
assert Derived.cls_value() == "derived"
assert Derived.static_value(3) == 10
assert isinstance(obj, Base)
assert issubclass(Derived, Base)
setattr(obj, "extra", 11)
assert getattr(obj, "extra") == 11
delattr(obj, "extra")
try:
    getattr(obj, "extra")
except AttributeError:
    pass
else:
    raise AssertionError("expected missing attr")
del obj.value
assert obj.value == 5

class CM(contextlib.AbstractContextManager):
    def __init__(self):
        self.closed = False

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.closed = True
        return False

with CM() as cm:
    assert not cm.closed
assert cm.closed

@functools.total_ordering
class Ordered:
    def __init__(self, value):
        self.value = value

    def __eq__(self, other):
        return self.value == other.value

    def __lt__(self, other):
        return self.value < other.value

assert Ordered(1) < Ordered(2)
assert Ordered(2) >= Ordered(1)
assert operator.attrgetter("value")(Ordered(9)) == 9
)");
}

TEST_F(NoJitSanityTest, StockInterpreterControlFlowCoverage) {
  runStockCode(R"(
import itertools

def classify(value):
    match value:
        case {"kind": "point", "x": x, "y": y} if x == y:
            return ("diag", x)
        case {"kind": "point", "x": x, "y": y}:
            return ("point", x + y)
        case (name, value) if name in {"a", "b"}:
            return ("pair", name, value)
        case [first, *middle, last]:
            return ("seq", first, middle, last)
        case int() as n:
            return ("int", n)
        case _:
            return ("other", value)

assert classify({"kind": "point", "x": 2, "y": 2}) == ("diag", 2)
assert classify({"kind": "point", "x": 2, "y": 5}) == ("point", 7)
assert classify([1, 2, 3, 4]) == ("seq", 1, [2, 3], 4)
assert classify(("a", 10)) == ("pair", "a", 10)
assert classify(42) == ("int", 42)
assert classify(None) == ("other", None)

def exercise_loop(limit):
    total = 0
    seen = []
    for i in range(limit):
        if i % 2 == 0:
            total += i
            continue
        if i > 7:
            break
        seen.append(i)
    else:
        total = -1
    return total, seen

assert exercise_loop(5) == (-1, [1, 3])
assert exercise_loop(12) == (20, [1, 3, 5, 7])

def exception_paths(values):
    out = []
    for value in values:
        try:
            if value == 0:
                raise ZeroDivisionError("zero")
            if value < 0:
                raise ValueError("neg")
            out.append(20 // value)
        except ZeroDivisionError as exc:
            out.append(str(exc))
        except ValueError:
            out.append("value")
        finally:
            out.append("finally")
    return out

assert exception_paths([5, 0, -1]) == [4, "finally", "zero", "finally", "value", "finally"]

try:
    try:
        raise RuntimeError("inner")
    except RuntimeError as exc:
        raise ValueError("outer") from exc
except ValueError as exc:
    assert isinstance(exc.__cause__, RuntimeError)

values = [x * y for x in range(4) for y in range(3) if x != y]
assert values == [0, 0, 0, 2, 0, 2, 0, 3, 6]
assert {x: x * x for x in range(4)}[3] == 9
assert {x % 3 for x in range(8)} == {0, 1, 2}
assert tuple(i for i in itertools.islice(itertools.count(), 4)) == (0, 1, 2, 3)

def unpacking(seq):
    a, (b, c), *rest = seq
    return a, b, c, rest

assert unpacking([1, (2, 3), 4, 5]) == (1, 2, 3, [4, 5])
)");
}

TEST_F(NoJitSanityTest, StockInterpreterAsyncAndCallCoverage) {
  runStockCode(R"(
import asyncio

async def async_gen(limit):
    for i in range(limit):
        yield i

class AsyncCM:
    def __init__(self):
        self.entered = False

    async def __aenter__(self):
        self.entered = True
        return self

    async def __aexit__(self, exc_type, exc, tb):
        self.entered = False
        return False

async def worker(value, *, scale=1):
    await asyncio.sleep(0)
    return value * scale

async def drive():
    total = 0
    async with AsyncCM() as cm:
        assert cm.entered
        async for item in async_gen(5):
            total += await worker(item, scale=2)
    assert not cm.entered
    return total

assert asyncio.run(drive()) == 20

def call_shapes(a, b=2, *args, c=3, **kwargs):
    return a + b + c + sum(args) + sum(kwargs.values())

assert call_shapes(1) == 6
assert call_shapes(1, 4, 5, 6, c=7, d=8) == 31
args = (2, 3)
kwargs = {"c": 4, "d": 5}
assert call_shapes(*args, **kwargs) == 14

def closure(seed):
    current = seed
    def inner(delta):
        nonlocal current
        current += delta
        return current
    return inner

fn = closure(10)
assert fn(5) == 15
assert fn(-3) == 12

def annotated(x: int, y: str = "a") -> str:
    return y * x

assert annotated.__annotations__["x"] is int
assert annotated(3, "b") == "bbb"

namespace = {}
exec('result = sum(i for i in range(6))', namespace, namespace)
assert namespace["result"] == 15
assert eval('result + 5', namespace, namespace) == 20
)");
}

TEST_F(NoJitSanityTest, StockInterpreterOpcodeMatrixCoverage) {
  runStockCode(R"(
import math

def binary_matrix():
    out = []
    a = 37
    b = 5
    out.append(a + b)
    out.append(a - b)
    out.append(a * b)
    out.append(a // b)
    out.append(a / b)
    out.append(a % b)
    out.append(a ** 2)
    out.append(a << 2)
    out.append(a >> 1)
    out.append(a & b)
    out.append(a | b)
    out.append(a ^ b)
    out.append(-a)
    out.append(+b)
    out.append(~b)
    c = 3
    c += 4
    c -= 1
    c *= 5
    c //= 2
    c %= 7
    c <<= 2
    c >>= 1
    c |= 3
    c &= 14
    c ^= 9
    out.append(c)
    out.append("ab" + "cd")
    out.append("ha" * 3)
    out.append((1, 2) + (3, 4))
    out.append([1, 2] * 2)
    return out

bm = binary_matrix()
assert bm[:4] == [42, 32, 185, 7]
assert bm[-4:] == ["abcd", "hahaha", (1, 2, 3, 4), [1, 2, 1, 2]]

data = list(range(20))
assert data[2] == 2
assert data[-1] == 19
assert data[2:10:2] == [2, 4, 6, 8]
data[1:5] = [40, 41, 42, 43]
assert data[1:5] == [40, 41, 42, 43]
del data[1:3]
assert data[1] == 42

d = {str(i): i for i in range(20)}
assert d["7"] == 7
d["new"] = 99
assert d.get("missing", 11) == 11
assert "new" in d and "missing" not in d
del d["new"]

def comparisons(x, y):
    return (
        x < y,
        x <= y,
        x == y,
        x != y,
        x > y,
        x >= y,
        x is y,
        x is not y,
        x in {x, y},
        100 not in {x, y},
    )

assert comparisons(1, 2) == (True, True, False, True, False, False, False, True, True, True)

def fstring_and_format(value):
    return f"value={value!r}:{value:04d}", "{:.2f}".format(math.pi)

assert fstring_and_format(7) == ("value=7:0007", "3.14")

def gen():
    received = yield "start"
    try:
        yield received + 1
    except ValueError:
        yield "handled"
    return "done"

g = gen()
assert next(g) == "start"
assert g.send(4) == 5
try:
    g.throw(ValueError())
except StopIteration as exc:
    assert exc.value == "done"

try:
    raise ExceptionGroup("group", [ValueError("v"), TypeError("t")])
except* ValueError as group:
    assert len(group.exceptions) == 1
except* TypeError as group:
    assert len(group.exceptions) == 1

class MatchClass:
    __match_args__ = ("x", "y")
    def __init__(self, x, y):
        self.x = x
        self.y = y

match MatchClass(3, 4):
    case MatchClass(3, y):
        assert y == 4
    case _:
        raise AssertionError("bad class match")
)");
}

TEST_F(NoJitSanityTest, StockInterpreterMonitoringCoverage) {
  runStockCode(R"(
import sys

events = sys.monitoring.events
tool = 5
try:
    sys.monitoring.free_tool_id(tool)
except ValueError:
    pass
sys.monitoring.use_tool_id(tool, "runtime-test-monitor")
seen = []

def callback(*args):
    seen.append(len(args))
    return callback

for event in (
    events.LINE,
    events.INSTRUCTION,
    events.CALL,
    events.PY_START,
    events.PY_RETURN,
    events.PY_YIELD,
    events.PY_RESUME,
    events.JUMP,
    events.BRANCH,
    events.EXCEPTION_HANDLED,
    events.RAISE,
):
    sys.monitoring.register_callback(tool, event, callback)

mask = (
    events.LINE
    | events.INSTRUCTION
    | events.CALL
    | events.PY_START
    | events.PY_RETURN
    | events.PY_YIELD
    | events.PY_RESUME
    | events.JUMP
    | events.BRANCH
    | events.EXCEPTION_HANDLED
    | events.RAISE
)
sys.monitoring.set_events(tool, mask)

class Target:
    def method(self, left, *, right=3):
        return left + right

def branchy(value):
    total = 0
    for item in range(value):
        if item % 2:
            total += item
        else:
            total -= item
    try:
        if value > 3:
            raise RuntimeError("boom")
    except RuntimeError:
        total += 7
    return Target().method(total, right=5)

def yielding():
    received = yield "start"
    yield branchy(received)

assert branchy(6) == 15
gen = yielding()
assert next(gen) == "start"
assert gen.send(5) == 10
try:
    next(gen)
except StopIteration:
    pass
else:
    raise AssertionError("expected generator completion")

sys.monitoring.set_events(tool, events.NO_EVENTS)
sys.monitoring.free_tool_id(tool)
assert len(seen) > 20

trace_hits = []
def tracer(frame, event, arg):
    trace_hits.append(event)
    return tracer

sys.settrace(tracer)
assert branchy(4) == 14
sys.settrace(None)
assert "line" in trace_hits
assert "return" in trace_hits
)");
}

TEST_F(NoJitSanityTest, AsyncLazyValueRuntimeCoverage) {
  runStockCode(R"(
import asyncio
from _cinderx import AsyncLazyValue, AwaitableValue

hits = 0

async def slow(value):
    global hits
    hits += 1
    await asyncio.sleep(0)
    await asyncio.sleep(0)
    return value + 1

async def raises():
    await asyncio.sleep(0)
    raise ValueError("boom")

async def main():
    immediate = AwaitableValue(7)
    assert immediate.value == 7
    assert await immediate == 7

    value = AsyncLazyValue(slow, 40)
    assert value.alv_state == "not_started"
    assert value._awaiting_tasks == 0

    async def wait_value():
        return await value

    first = asyncio.create_task(wait_value())
    await asyncio.sleep(0)
    assert value.alv_state == "running"
    second = asyncio.create_task(wait_value())
    assert await first == 41
    assert await second == 41
    assert await value == 41
    assert hits == 1
    assert value.alv_state == "done"

    done_future = value.ensure_future(asyncio.get_running_loop())
    assert await done_future == 41

    close_value = AsyncLazyValue(slow, 50)
    close_iter = close_value.__await__()
    assert close_value.alv_state == "running"
    close_iter.close()

    thrown = AsyncLazyValue(slow, 60)
    thrown_iter = thrown.__await__()
    try:
        thrown_iter.throw(GeneratorExit)
    except GeneratorExit:
        pass
    else:
        raise AssertionError("expected GeneratorExit")

    linked = AsyncLazyValue(slow, 70)
    assert linked._link() is None
    assert linked._unlink() is None

    failed = AsyncLazyValue(raises)
    try:
        await failed
    except ValueError as exc:
        assert str(exc) == "boom"
    else:
        raise AssertionError("expected failure")

asyncio.run(main())
)");
}

TEST_F(NoJitSanityTest, GeneratedCasesOpcodeCoverage) {
  runStockCode(R"(
import contextlib
import operator

class Counter:
    __slots__ = ("value",)

    def __init__(self, value):
        self.value = value

    def bump(self, step=1):
        self.value += step
        return self.value

    @property
    def doubled(self):
        return self.value * 2

class Plain:
    pass

def takes_kwargs(a, b=0, *, c=0, d=0):
    return a + b + c + d

def varargs(*args, **kwargs):
    return sum(args) + sum(kwargs.values())

def index_ops(seq, mapping, text, i):
    seq[i] += 1
    seq[1:3] = [seq[1] + 1, seq[2] + 1]
    mapping["seen"] = mapping.get("seen", 0) + seq[i]
    return seq[i], seq[1:4], mapping["seen"], text[i], text[1:4]

def build_ops(a, b, c):
    values = [a, b, c]
    more = [*values, 9, 10]
    unique = {*values, *more}
    merged = {"a": a, **{"b": b}, "c": c}
    return more, unique, merged, f"{a}:{b}:{c}", slice(a, b, c)

def compare_and_branch(x, y):
    total = 0
    if x < y:
        total += 1
    if x <= y:
        total += 2
    if x != y:
        total += 4
    if x in {1, 2, 3, 4}:
        total += 8
    if "a" not in "xyz":
        total += 16
    return total

def exception_ops(value):
    try:
        if value & 1:
            raise KeyError(value)
        return value
    except KeyError as exc:
        return exc.args[0] + 10
    finally:
        value += 1

def unpack_and_match(obj):
    a, *middle, z = obj
    match {"kind": "point", "x": a, "y": z}:
        case {"kind": "point", "x": x, "y": y}:
            return x + y + len(middle)
        case _:
            return 0

def call_shapes(counter, plain):
    plain.attr = counter.bump(2)
    first = counter.bump()
    second = Counter.bump(counter, step=3)
    third = takes_kwargs(first, b=second, c=plain.attr, d=counter.doubled)
    fourth = varargs(1, 2, 3, x=4, y=5)
    fifth = operator.add(third, fourth)
    return first + second + third + fourth + fifth + isinstance(counter, Counter)

def run_once(seed):
    data = [0, 1, 2, 3, 4, 5]
    mapping = {"seen": seed}
    text = "abcdef"
    counter = Counter(seed)
    plain = Plain()
    a = seed + 3
    b = a * 2
    c = b - seed
    d = b / 2.0
    e = (a % 5) ** 2
    s = "x"
    s += str(seed)
    sub = index_ops(data, mapping, text, seed % 3)
    built = build_ops(seed, seed + 1, seed + 2)
    with contextlib.nullcontext(seed) as got:
        ctx = got
    return (
        a + b + c + int(d) + e + len(s)
        + sub[0] + len(sub[1]) + sub[2] + ord(sub[3])
        + len(built[0]) + len(built[1]) + len(built[2]) + len(built[3])
        + compare_and_branch(seed % 5, 4)
        + exception_ops(seed)
        + unpack_and_match((1, 2, 3, 4))
        + call_shapes(counter, plain)
        + ctx
    )

total = 0
for i in range(500):
    total += run_once(i % 7)
assert total > 0
)");
}

TEST_F(NoJitSanityTest, InstrumentedOpcodeMonitoringCoverage) {
  runStockCode(R"(
import sys

events_seen = []
monitoring = sys.monitoring
tool = monitoring.OPTIMIZER_ID
try:
    monitoring.use_tool_id(tool, "runtime-tests-monitoring")
except ValueError:
    monitoring.free_tool_id(tool)
    monitoring.use_tool_id(tool, "runtime-tests-monitoring")

def callback(*args):
    events_seen.append(args[1] if len(args) > 1 else None)
    return None

for event in (
    monitoring.events.PY_START,
    monitoring.events.PY_RESUME,
    monitoring.events.PY_RETURN,
    monitoring.events.PY_YIELD,
    monitoring.events.CALL,
    monitoring.events.LINE,
    monitoring.events.INSTRUCTION,
    monitoring.events.JUMP,
    monitoring.events.BRANCH_LEFT,
    monitoring.events.BRANCH_RIGHT,
    monitoring.events.STOP_ITERATION,
):
    monitoring.register_callback(tool, event, callback)

class Parent:
    def method(self, value):
        return value + 1

class Child(Parent):
    def method(self, value):
        return super().method(value) + len((value,))

def callee(a, b=0, *, c=0):
    return a + b + c

def gen(limit):
    for i in range(limit):
        yield i

def target(flag):
    child = Child()
    total = 0
    for value in gen(4):
        total += child.method(value)
    try:
        if flag:
            raise RuntimeError("flag")
    except RuntimeError:
        total += callee(*(1, 2), **{"c": 3})
    total += sum([1, 2, 3])
    total += list((4, 5))[0]
    return total

all_events = (
    monitoring.events.PY_START
    | monitoring.events.PY_RESUME
    | monitoring.events.PY_RETURN
    | monitoring.events.PY_YIELD
    | monitoring.events.CALL
    | monitoring.events.LINE
    | monitoring.events.INSTRUCTION
    | monitoring.events.JUMP
    | monitoring.events.BRANCH_LEFT
    | monitoring.events.BRANCH_RIGHT
    | monitoring.events.STOP_ITERATION
)
for func in (target, callee, gen, Parent.method, Child.method):
    monitoring.set_local_events(tool, func.__code__, all_events)

try:
    assert target(True) > 0
    assert target(False) > 0
    assert events_seen
finally:
    for func in (target, callee, gen, Parent.method, Child.method):
        monitoring.set_local_events(tool, func.__code__, monitoring.events.NO_EVENTS)
    for event in (
        monitoring.events.PY_START,
        monitoring.events.PY_RESUME,
        monitoring.events.PY_RETURN,
        monitoring.events.PY_YIELD,
        monitoring.events.CALL,
        monitoring.events.LINE,
        monitoring.events.INSTRUCTION,
        monitoring.events.JUMP,
        monitoring.events.BRANCH_LEFT,
        monitoring.events.BRANCH_RIGHT,
        monitoring.events.STOP_ITERATION,
    ):
        monitoring.register_callback(tool, event, None)
    monitoring.free_tool_id(tool)
)");
}

TEST_F(NoJitSanityTest, AsyncProtocolOpcodeCoverage) {
  runStockCode(R"(
import asyncio

class AsyncCounter:
    def __init__(self, limit):
        self.limit = limit
        self.value = 0

    def __aiter__(self):
        return self

    async def __anext__(self):
        await asyncio.sleep(0)
        if self.value >= self.limit:
            raise StopAsyncIteration
        self.value += 1
        return self.value

class AsyncContext:
    def __init__(self):
        self.entered = False

    async def __aenter__(self):
        await asyncio.sleep(0)
        self.entered = True
        return self

    async def __aexit__(self, exc_type, exc, tb):
        await asyncio.sleep(0)
        self.entered = False
        return False

async def agen(limit):
    async for value in AsyncCounter(limit):
        yield value * 2

async def drive():
    total = 0
    async with AsyncContext() as ctx:
        assert ctx.entered
        async for item in agen(4):
            total += item
    assert not ctx.entered
    try:
        async for item in AsyncCounter(2):
            total += item
            if item == 1:
                raise ValueError("stop")
    except ValueError:
        total += 10
    return total

assert asyncio.run(drive()) == 31
)");
}

TEST_F(NoJitSanityTest, SpecializedOpcodeEdgeCoverage) {
  runStockCode(R"(
import builtins
import collections
import functools
import operator

class AttrTrap:
    def __init__(self):
        self.value = 41

    def __getattribute__(self, name):
        if name == "value":
            return object.__getattribute__(self, name) + 1
        return object.__getattribute__(self, name)

class FastList(list):
    pass

def float_subtract(value):
    total = float(value)
    for _ in range(80):
        total = total - 1.25
    return total

def call_shapes(seq):
    total = 0
    total += int("11")
    total += len(seq)
    total += max(seq, key=lambda x: -x)
    total += dict.fromkeys(seq, 1).get(seq[0], 0)
    total += operator.index(5)
    total += functools.reduce(operator.add, seq, 0)
    total += list.__len__(seq)
    total += str.join("-", ("a", "b")) == "a-b"
    total += tuple.__getitem__((7, 8, 9), 1)
    total += isinstance(seq, list)
    return total

def kw_shapes():
    data = {"a": 1}
    data.update(b=2, c=3)
    merged = {**data, **{"d": 4}}
    try:
        {}.update([(1, 2, 3)])
    except ValueError:
        merged["bad"] = 5
    return sum(merged.values())

def list_extend_shapes():
    out = []
    out.extend((1, 2))
    out += [3, 4]
    try:
        out.extend(42)
    except TypeError:
        out.append(5)
    return out

def delete_deref_shape():
    value = "alive"
    def inner():
        nonlocal value
        del value
    inner()
    try:
        return value
    except NameError:
        return "deleted"

def extended_opcode_shape(index):
    values = tuple(range(320))
    return values[index]

def set_shape(values):
    s = {*values, 11, 12}
    t = {x for x in values if x % 2}
    return len(s ^ t)

def attr_shape():
    obj = AttrTrap()
    return obj.value + getattr(obj, "value")

for i in range(700):
    seq = FastList([1, 2, 3, 4])
    assert float_subtract(100.0) < 100.0
    assert call_shapes(seq) > 0
    assert kw_shapes() == 15
    assert list_extend_shapes() == [1, 2, 3, 4, 5]
    assert delete_deref_shape() == "deleted"
    assert extended_opcode_shape(i % 320) >= 0
    assert set_shape(range(8)) > 0
    assert attr_shape() == 84
)");
}

TEST_F(StaticSanityTest, StaticCheckedListOperations) {
  runStaticCode(R"(
from __static__ import CheckedList

def expect_type_error(fn):
    try:
        fn()
    except TypeError:
        return
    raise AssertionError("expected TypeError")

def make_iter():
    for value in (8, 9):
        yield value

class HintIter:
    def __init__(self, values, hint):
        self.values = list(values)
        self.hint = hint
        self.index = 0

    def __iter__(self):
        return self

    def __next__(self):
        if self.index >= len(self.values):
            raise StopIteration
        value = self.values[self.index]
        self.index += 1
        return value

    def __length_hint__(self):
        return self.hint

class BadIter:
    def __iter__(self):
        return self

    def __next__(self):
        raise RuntimeError("iter boom")

xs = CheckedList[int]([3, 1, 2])
assert list(xs) == [3, 1, 2]
assert len(xs) == 3
assert 1 in xs
assert xs.count(1) == 1
assert xs.index(2) == 2
assert xs.__sizeof__() > 0
assert list(reversed(xs)) == [2, 1, 3]

xs.append(4)
xs.insert(0, 0)
xs.extend([5, 6])
xs.extend((7,))
xs.extend(make_iter())
xs.extend([])
xs.extend(HintIter((10, 11), 8))
assert xs == CheckedList[int]([0, 3, 1, 2, 4, 5, 6, 7, 8, 9, 10, 11])
assert xs[-2:] == [10, 11]

ys = xs.copy()
assert type(ys) is type(xs)
ys[1] = 10
ys[2:4] = CheckedList[int]([11, 12])
assert ys[1:4] == [10, 11, 12]
expect_type_error(lambda: ys.__setitem__(1, "bad"))
expect_type_error(lambda: ys.__setitem__(slice(1, 2), [99]))

assert ys.pop() == 11
assert ys.pop(None) == 10
assert ys.pop(0) == 0
ys.remove(12)
try:
    ys.remove(123456)
except ValueError:
    pass
else:
    raise AssertionError("expected missing remove ValueError")
ys.reverse()
ys.sort()
assert ys == CheckedList[int]([4, 5, 6, 7, 8, 9, 10, 11])

a = CheckedList[int]([1, 2])
a.extend(a)
assert a == CheckedList[int]([1, 2, 1, 2])
assert a + CheckedList[int]([3]) == [1, 2, 1, 2, 3]
assert a * 2 == [1, 2, 1, 2, 1, 2, 1, 2]
assert a.pop(1) == 2
assert a.pop() == 2
assert a == CheckedList[int]([1, 1])
a.clear()
assert a == CheckedList[int]()

expect_type_error(lambda: CheckedList[int]([1, "bad"]))
expect_type_error(lambda: xs.append("bad"))
expect_type_error(lambda: xs.extend([10, "bad"]))
try:
    xs.pop("bad")
except TypeError:
    pass
else:
    raise AssertionError("expected pop TypeError")
try:
    CheckedList[int]().pop()
except IndexError:
    pass
else:
    raise AssertionError("expected empty pop IndexError")
try:
    xs.pop(100000)
except IndexError:
    pass
else:
    raise AssertionError("expected pop IndexError")
try:
    xs.extend(BadIter())
except RuntimeError:
    pass
else:
    raise AssertionError("expected iter RuntimeError")
try:
    xs.index(100)
except ValueError:
    pass
else:
    raise AssertionError("expected missing value")

big = CheckedList[int](range(200, 0, -1))
assert big[0] == 200
assert big[-1] == 1
assert big[10:15] == [190, 189, 188, 187, 186]
big.sort()
assert big[:5] == [1, 2, 3, 4, 5]
big.sort(reverse=True)
assert big[:5] == [200, 199, 198, 197, 196]
big.sort(key=lambda value: value % 17)
assert sorted(big) == list(range(1, 201))
big.reverse()
assert sorted(big) == list(range(1, 201))
repeat = CheckedList[int]([1, 2, 3])
assert repeat * 3 == [1, 2, 3, 1, 2, 3, 1, 2, 3]
assert 2 in repeat
assert repeat.count(2) == 1
assert repeat < CheckedList[int]([1, 2, 4])
assert repeat <= CheckedList[int]([1, 2, 3])
assert repeat > CheckedList[int]([1, 2])
assert repeat >= CheckedList[int]([1, 2, 3])
assert repeat != CheckedList[int]([1, 2])
)");
}

TEST_F(StaticSanityTest, StaticCheckedDictOperations) {
  runStaticCode(R"(
from __static__ import CheckedDict

def expect_type_error(fn):
    try:
        fn()
    except TypeError:
        return
    raise AssertionError("expected TypeError")

def pair_iter():
    yield ("d", 4)
    yield ("e", 5)

def dyn(value):
    return value

m = CheckedDict[str, int]({"a": 1, "b": 2})
assert len(m) == 2
assert m["a"] == 1
assert m.get("missing") is None
assert m.get("missing", 99) == 99
assert "a" in m
assert "z" not in m
m["c"] = 3
m.__setitem__("f", 6)
m.update({"g": 7})
m.update(pair_iter())
m.update(h=8)
assert m.setdefault("a", 10) == 1
assert m.setdefault("i", 9) == 9
assert sorted(m.keys()) == ["a", "b", "c", "d", "e", "f", "g", "h", "i"]
assert sorted(m.values()) == [1, 2, 3, 4, 5, 6, 7, 8, 9]

n = m.copy()
assert n == m

seen = []
for key in m:
    seen.append(key)
assert sorted(seen) == sorted(m.keys())
items = []
for key, value in m.items():
    items.append((key, value))
assert sorted(items) == sorted(list(m.items()))

m.clear()
assert m == CheckedDict[str, int]()

large = CheckedDict[str, int]()
for i in range(600):
    large[str(i)] = i
assert len(large) == 600
for i in range(0, 600, 37):
    assert large[str(i)] == i
assert large.get("599") == 599
assert large.get("missing", -1) == -1
large.update((str(i + 600), i + 600) for i in range(100))
large.update({str(i + 700): i + 700 for i in range(100)})
assert len(large) == 800
assert sorted(list(large.keys()))[0] == "0"
assert sum(large.values()) == sum(range(800))
mirror = large.copy()
assert mirror == large
)");
}

TEST_F(NoJitSanityTest, InstrumentedOpcodeCoverage) {
  runStockCode(R"(
import asyncio
import sys

events = []

def tracer(frame, event, arg):
    events.append(event)
    return tracer

def profiler(frame, event, arg):
    events.append(event)

def callee(a, b=1, *, c=2):
    return a + b + c

def loop_and_calls(n):
    total = 0
    for i in range(n):
        total += callee(i, b=i + 1, c=i + 2)
        if i % 2:
            total += len([i, total])
        else:
            total += sum((i, total))
    return total

async def agen(n):
    for i in range(n):
        await asyncio.sleep(0)
        yield i

async def async_driver():
    total = 0
    async for value in agen(4):
        total += callee(value, c=value + 3)
    return total

def exception_driver():
    total = 0
    try:
        raise ValueError("x")
    except ValueError:
        total += 5
    finally:
        total += 7
    return total

sys.settrace(tracer)
sys.setprofile(profiler)
try:
    assert loop_and_calls(8) > 0
    assert asyncio.run(async_driver()) > 0
    assert exception_driver() == 12
finally:
    sys.setprofile(None)
    sys.settrace(None)

assert events
)");
}

TEST_F(StaticSanityTest, CheckedContainersCApiCoverage) {
  auto list_type_obj = checkedListType(reinterpret_cast<PyObject*>(&PyLong_Type));
  ASSERT_NE(list_type_obj, nullptr);
  ASSERT_TRUE(PyType_Check(list_type_obj));
  auto* list_type = reinterpret_cast<PyTypeObject*>(list_type_obj.get());
  ASSERT_TRUE(Ci_CheckedList_TypeCheck(list_type));

  auto list = Ref<>::steal(Ci_CheckedList_New(list_type, 4));
  ASSERT_NE(list, nullptr);
  ASSERT_TRUE(Ci_CheckedList_Check(list));
  for (Py_ssize_t i = 0; i < 4; ++i) {
    auto value = Ref<>::steal(PyLong_FromLong(i + 1));
    ASSERT_NE(value, nullptr);
    Py_INCREF(value.get());
    Ci_CheckedList_SET_ITEM(list.get(), i, value.get());
  }
  EXPECT_EQ(Ci_CheckedList_GET_SIZE(list.get()), 4);
  ASSERT_EQ(Ci_ListOrCheckedList_Append(
                reinterpret_cast<PyListObject*>(list.get()),
                PyLong_FromLong(5)),
            0);

  auto repr = Ref<>::steal(PyObject_Repr(list));
  ASSERT_NE(repr, nullptr);
  auto contains = PySequence_Contains(list, PyLong_FromLong(3));
  EXPECT_EQ(contains, 1);
  auto item = Ref<>::steal(Ci_CheckedList_GetItem(list, 2));
  ASSERT_NE(item, nullptr);
  EXPECT_EQ(PyLong_AsLong(item), 3);
  auto repeat = Ref<>::steal(PySequence_Repeat(list, 2));
  ASSERT_NE(repeat, nullptr);
  auto concat = Ref<>::steal(PySequence_Concat(list, list));
  ASSERT_NE(concat, nullptr);
  auto slice = Ref<>::steal(PyObject_GetItem(list, PySlice_New(PyLong_FromLong(1), PyLong_FromLong(4), nullptr)));
  ASSERT_NE(slice, nullptr);
  auto insert_value = Ref<>::steal(PyLong_FromLong(99));
  ASSERT_NE(insert_value, nullptr);
  auto insert_result =
      Ref<>::steal(PyObject_CallMethod(list, "insert", "nO", -100, insert_value.get()));
  ASSERT_EQ(insert_result, Py_None);
  auto list_extend = Ref<>::steal(Ci_CheckedList_New(list_type, 2));
  ASSERT_NE(list_extend, nullptr);
  for (Py_ssize_t i = 0; i < 2; ++i) {
    auto value = Ref<>::steal(PyLong_FromLong(i + 6));
    ASSERT_NE(value, nullptr);
    Py_INCREF(value.get());
    Ci_CheckedList_SET_ITEM(list_extend.get(), i, value.get());
  }
  auto extend_result =
      Ref<>::steal(PyObject_CallMethod(list, "extend", "O", list_extend.get()));
  ASSERT_EQ(extend_result, Py_None);
  auto self_extend =
      Ref<>::steal(PyObject_CallMethod(list, "extend", "O", list.get()));
  ASSERT_EQ(self_extend, Py_None);
  auto pop_none =
      Ref<>::steal(PyObject_CallMethod(list, "pop", "O", Py_None));
  ASSERT_NE(pop_none, nullptr);

  auto reversed = callMethodNoArgs(list, "__reversed__");
  ASSERT_NE(reversed, nullptr);
  auto next = Ref<>::steal(PyIter_Next(reversed));
  ASSERT_NE(next, nullptr);
  EXPECT_GT(PyLong_AsLong(next), 0);
  auto sort_result = callMethodNoArgs(list, "sort");
  ASSERT_EQ(sort_result, Py_None);
  auto reverse_result = callMethodNoArgs(list, "reverse");
  ASSERT_EQ(reverse_result, Py_None);
  auto count = Ref<>::steal(PyObject_CallMethod(list, "count", "O", item.get()));
  ASSERT_NE(count, nullptr);
  auto index = Ref<>::steal(PyObject_CallMethod(list, "index", "O", item.get()));
  ASSERT_NE(index, nullptr);
  auto popped = Ref<>::steal(PyObject_CallMethod(list, "pop", nullptr));
  ASSERT_NE(popped, nullptr);
  auto clear_result = callMethodNoArgs(list, "clear");
  ASSERT_EQ(clear_result, Py_None);

  auto dict_type_obj = checkedDictType(
      reinterpret_cast<PyObject*>(&PyUnicode_Type),
      reinterpret_cast<PyObject*>(&PyLong_Type));
  ASSERT_NE(dict_type_obj, nullptr);
  ASSERT_TRUE(PyType_Check(dict_type_obj));
  auto* dict_type = reinterpret_cast<PyTypeObject*>(dict_type_obj.get());
  ASSERT_TRUE(Ci_CheckedDict_TypeCheck(dict_type));

  auto dict = Ref<>::steal(Ci_CheckedDict_NewPresized(dict_type, 16));
  ASSERT_NE(dict, nullptr);
  ASSERT_TRUE(Ci_CheckedDict_Check(dict));
  for (int i = 0; i < 32; ++i) {
    auto key = Ref<>::steal(PyUnicode_FromFormat("k%d", i));
    auto value = Ref<>::steal(PyLong_FromLong(i));
    ASSERT_NE(key, nullptr);
    ASSERT_NE(value, nullptr);
    ASSERT_EQ(Ci_CheckedDict_SetItem(dict, key, value), 0);
  }
  EXPECT_EQ(Ci_CheckedDict_CheckConsistency(dict, 1), 1);

  auto key0 = Ref<>::steal(PyUnicode_FromString("k0"));
  auto val0 = Ref<>::steal(PyObject_GetItem(dict, key0));
  ASSERT_NE(val0, nullptr);
  EXPECT_EQ(PyLong_AsLong(val0), 0);
  EXPECT_EQ(PyMapping_HasKey(dict, key0), 1);
  auto dict_repr = Ref<>::steal(PyObject_Repr(dict));
  ASSERT_NE(dict_repr, nullptr);
  auto copied = callMethodNoArgs(dict, "copy");
  ASSERT_NE(copied, nullptr);
  auto eq = Ref<>::steal(PyObject_RichCompare(dict, copied, Py_EQ));
  ASSERT_NE(eq, nullptr);
  EXPECT_TRUE(PyObject_IsTrue(eq));

  auto keys = callMethodNoArgs(dict, "keys");
  auto values = callMethodNoArgs(dict, "values");
  auto items = callMethodNoArgs(dict, "items");
  ASSERT_NE(keys, nullptr);
  ASSERT_NE(values, nullptr);
  ASSERT_NE(items, nullptr);
  auto keys_repr = Ref<>::steal(PyObject_Repr(keys));
  auto values_repr = Ref<>::steal(PyObject_Repr(values));
  auto items_repr = Ref<>::steal(PyObject_Repr(items));
  ASSERT_NE(keys_repr, nullptr);
  ASSERT_NE(values_repr, nullptr);
  ASSERT_NE(items_repr, nullptr);
  auto key_iter = Ref<>::steal(PyObject_GetIter(keys));
  auto value_iter = Ref<>::steal(PyObject_GetIter(values));
  auto item_iter = Ref<>::steal(PyObject_GetIter(items));
  ASSERT_NE(key_iter, nullptr);
  ASSERT_NE(value_iter, nullptr);
  ASSERT_NE(item_iter, nullptr);
  ASSERT_NE(Ref<>::steal(PyIter_Next(key_iter)), nullptr);
  ASSERT_NE(Ref<>::steal(PyIter_Next(value_iter)), nullptr);
  ASSERT_NE(Ref<>::steal(PyIter_Next(item_iter)), nullptr);

  auto rev = callMethodNoArgs(dict, "__reversed__");
  ASSERT_NE(rev, nullptr);
  ASSERT_NE(Ref<>::steal(PyIter_Next(rev)), nullptr);
  auto setdefault_key = Ref<>::steal(PyUnicode_FromString("missing"));
  auto setdefault_value = Ref<>::steal(PyLong_FromLong(1234));
  ASSERT_NE(setdefault_key, nullptr);
  ASSERT_NE(setdefault_value, nullptr);
  auto setdefault_result = Ref<>::steal(PyObject_CallMethod(
      dict, "setdefault", "OO", setdefault_key.get(), setdefault_value.get()));
  ASSERT_NE(setdefault_result, nullptr);
  EXPECT_EQ(PyLong_AsLong(setdefault_result), 1234);
  auto default_get = Ref<>::steal(PyObject_CallMethod(
      dict, "get", "OO", setdefault_key.get(), Py_None));
  ASSERT_NE(default_get, nullptr);
  EXPECT_EQ(PyLong_AsLong(default_get), 1234);
  auto update_pairs = Ref<>::steal(Py_BuildValue(
      "[(NO)(NO)]",
      PyUnicode_FromString("u1"),
      PyLong_FromLong(2001),
      PyUnicode_FromString("u2"),
      PyLong_FromLong(2002)));
  ASSERT_NE(update_pairs, nullptr);
  auto update_result =
      Ref<>::steal(PyObject_CallMethod(dict, "update", "O", update_pairs.get()));
  ASSERT_EQ(update_result, Py_None);
  auto fromkeys_keys = Ref<>::steal(Py_BuildValue(
      "[NN]", PyUnicode_FromString("fk1"), PyUnicode_FromString("fk2")));
  auto fromkeys_value = Ref<>::steal(PyLong_FromLong(88));
  ASSERT_NE(fromkeys_keys, nullptr);
  ASSERT_NE(fromkeys_value, nullptr);
  auto fromkeys = Ref<>::steal(PyObject_CallMethod(
      (PyObject*)dict_type, "fromkeys", "OO", fromkeys_keys.get(), fromkeys_value.get()));
  ASSERT_NE(fromkeys, nullptr);
  EXPECT_EQ(Ci_DictOrChecked_SetItem(dict, setdefault_key, setdefault_value), 0);
  auto plain = Ref<>::steal(PyDict_New());
  ASSERT_NE(plain, nullptr);
  EXPECT_EQ(Ci_DictOrChecked_SetItem(plain, setdefault_key, setdefault_value), 0);
  auto pop_key = Ref<>::steal(PyUnicode_FromString("k1"));
  auto popped_value =
      Ref<>::steal(PyObject_CallMethod(dict, "pop", "O", pop_key.get()));
  ASSERT_NE(popped_value, nullptr);
  auto popitem = callMethodNoArgs(dict, "popitem");
  ASSERT_NE(popitem, nullptr);
  auto sizeof_result = callMethodNoArgs(dict, "__sizeof__");
  ASSERT_NE(sizeof_result, nullptr);
  auto clear_dict = callMethodNoArgs(dict, "clear");
  ASSERT_EQ(clear_dict, Py_None);
}

TEST_F(StaticSanityTest, StaticPrimitiveAndThunkCApiCoverage) {
  const int primitive_types[] = {
      TYPED_INT8,
      TYPED_INT16,
      TYPED_INT32,
      TYPED_INT64,
      TYPED_UINT8,
      TYPED_UINT16,
      TYPED_UINT32,
      TYPED_UINT64,
      TYPED_BOOL,
      TYPED_DOUBLE,
      TYPED_SINGLE,
      TYPED_CHAR,
      TYPED_OBJECT,
  };
  for (int type : primitive_types) {
    EXPECT_GT(_PyClassLoader_PrimitiveTypeToSize(type), 0);
    EXPECT_GE(_PyClassLoader_PrimitiveTypeToStructMemberType(type), 0);
  }
  EXPECT_EQ(_PyClassLoader_PrimitiveTypeToSize(999), -1);
  PyErr_Clear();
  EXPECT_EQ(_PyClassLoader_PrimitiveTypeToStructMemberType(999), -1);
  PyErr_Clear();

  auto boxed_i8 = Ref<>::steal(_PyClassLoader_Box(0xff, TYPED_INT8));
  ASSERT_NE(boxed_i8, nullptr);
  EXPECT_EQ(PyLong_AsLong(boxed_i8), -1);
  EXPECT_EQ(static_cast<int8_t>(_PyClassLoader_Unbox(boxed_i8, TYPED_INT8)), -1);

  auto boxed_u16 = Ref<>::steal(_PyClassLoader_Box(65535, TYPED_UINT16));
  ASSERT_NE(boxed_u16, nullptr);
  EXPECT_EQ(PyLong_AsUnsignedLong(boxed_u16), 65535);
  EXPECT_EQ(_PyClassLoader_Unbox(boxed_u16, TYPED_UINT16), 65535);

  double dbl = 12.5;
  uint64_t dbl_bits;
  memcpy(&dbl_bits, &dbl, sizeof(double));
  auto boxed_double = Ref<>::steal(_PyClassLoader_Box(dbl_bits, TYPED_DOUBLE));
  ASSERT_NE(boxed_double, nullptr);
  EXPECT_EQ(PyFloat_AsDouble(boxed_double), dbl);
  EXPECT_EQ(_PyClassLoader_Unbox(boxed_double, TYPED_DOUBLE), dbl_bits);

  auto boxed_bool = Ref<>::steal(_PyClassLoader_Box(1, TYPED_BOOL));
  ASSERT_NE(boxed_bool, nullptr);
  EXPECT_EQ(boxed_bool, Py_True);
  EXPECT_EQ(_PyClassLoader_Unbox(Py_False, TYPED_BOOL), 0);

  auto sig = reinterpret_cast<_PyClassLoader_ThunkSignature*>(PyMem_Malloc(
      sizeof(_PyClassLoader_ThunkSignature) + sizeof(uint8_t) * 5));
  ASSERT_NE(sig, nullptr);
  sig->ta_argcount = 5;
  sig->ta_has_primitives = 1;
  sig->ta_allocated = 1;
  sig->ta_rettype = TYPED_OBJECT;
  sig->ta_argtype[0] = TYPED_OBJECT;
  sig->ta_argtype[1] = TYPED_INT8;
  sig->ta_argtype[2] = TYPED_UINT16;
  sig->ta_argtype[3] = TYPED_DOUBLE;
  sig->ta_argtype[4] = TYPED_BOOL;
  auto copied_sig = _PyClassLoader_CopyThunkSig(sig);
  ASSERT_NE(copied_sig, nullptr);
  EXPECT_EQ(copied_sig->ta_argcount, 5);
  EXPECT_EQ(copied_sig->ta_argtype[4], TYPED_BOOL);

  auto obj_arg0 = Ref<>::steal(PyUnicode_FromString("first"));
  ASSERT_NE(obj_arg0, nullptr);
  double native_double = 3.25;
  uint64_t native_double_bits;
  memcpy(&native_double_bits, &native_double, sizeof(double));
  void* native_args[] = {
      obj_arg0.get(),
      reinterpret_cast<void*>(static_cast<intptr_t>(-7)),
      reinterpret_cast<void*>(static_cast<uintptr_t>(65535)),
      reinterpret_cast<void*>(native_double_bits),
      reinterpret_cast<void*>(1),
  };
  PyObject* call_args[5] = {};
  PyObject* free_args[5] = {};
  ASSERT_EQ(
      _PyClassLoader_HydrateArgsFromSig(
          sig, 5, native_args, call_args, free_args),
      0);
  EXPECT_EQ(call_args[0], obj_arg0.get());
  EXPECT_EQ(PyLong_AsLong(call_args[1]), -7);
  EXPECT_EQ(PyLong_AsUnsignedLong(call_args[2]), 65535);
  EXPECT_EQ(PyFloat_AsDouble(call_args[3]), native_double);
  EXPECT_EQ(call_args[4], Py_True);
  _PyClassLoader_FreeHydratedArgs(free_args, 5);
  _PyClassLoader_FreeThunkSignature(copied_sig);
  _PyClassLoader_FreeThunkSignature(sig);

  runStaticCode(R"(
class PropertyHost:
    def __init__(self):
        self.value = 10

    @property
    def prop(self):
        return self.value

    @prop.setter
    def prop(self, value):
        self.value = value

    @prop.deleter
    def prop(self):
        self.value = -1
)");
  auto host_type = getGlobal("PropertyHost");
  auto prop = Ref<>::steal(PyObject_GetAttrString(host_type, "prop"));
  ASSERT_NE(prop, nullptr);
  auto host = Ref<>::steal(PyObject_CallNoArgs(host_type));
  ASSERT_NE(host, nullptr);

  auto getter = Ref<>::steal(_PyClassLoader_PropertyThunkGet_New(prop));
  auto setter = Ref<>::steal(_PyClassLoader_PropertyThunkSet_New(prop));
  auto deleter = Ref<>::steal(_PyClassLoader_PropertyThunkDel_New(prop));
  ASSERT_NE(getter, nullptr);
  ASSERT_NE(setter, nullptr);
  ASSERT_NE(deleter, nullptr);
  EXPECT_EQ(_PyClassLoader_PropertyThunk_GetProperty(getter), prop);
  EXPECT_EQ(_PyClassLoader_PropertyThunk_Kind(getter), THUNK_GETTER);
  EXPECT_EQ(_PyClassLoader_PropertyThunk_Kind(setter), THUNK_SETTER);
  EXPECT_EQ(_PyClassLoader_PropertyThunk_Kind(deleter), THUNK_DELETER);

  PyObject* get_args[] = {host};
  auto got = Ref<>::steal(PyObject_Vectorcall(getter, get_args, 1, nullptr));
  ASSERT_NE(got, nullptr);
  EXPECT_EQ(PyLong_AsLong(got), 10);

  auto value = Ref<>::steal(PyLong_FromLong(42));
  ASSERT_NE(value, nullptr);
  PyObject* set_args[] = {host, value};
  auto set_res = Ref<>::steal(PyObject_Vectorcall(setter, set_args, 2, nullptr));
  ASSERT_NE(set_res, nullptr);
  got = Ref<>::steal(PyObject_Vectorcall(getter, get_args, 1, nullptr));
  ASSERT_NE(got, nullptr);
  EXPECT_EQ(PyLong_AsLong(got), 42);

  auto del_res = Ref<>::steal(PyObject_Vectorcall(deleter, get_args, 1, nullptr));
  ASSERT_NE(del_res, nullptr);
  got = Ref<>::steal(PyObject_Vectorcall(getter, get_args, 1, nullptr));
  ASSERT_NE(got, nullptr);
  EXPECT_EQ(PyLong_AsLong(got), -1);
}

TEST_F(StaticSanityTest, StaticCachedPropertyDescriptors) {
  runStaticCode(R"(
import asyncio
from cinderx import (
    async_cached_classproperty,
    async_cached_property,
    cached_classproperty,
    cached_property,
)

class C:
    def __init__(self):
        self.hits = 0

    @cached_property
    def value(self):
        "value doc"
        self.hits += 1
        return 42

    @cached_property
    def other(self):
        return self.value + 1

c = C()
descr = C.__dict__["value"]
assert descr.__doc__ == "value doc"
assert descr.__name__ == "value"
assert descr.name == "value"
assert c.value == 42
assert c.value == 42
assert c.other == 43
assert c.hits == 1
assert descr.has_value(c)
descr.clear(c)
assert not descr.has_value(c)
assert c.value == 42
assert c.hits == 2
descr.__set__(c, 100)
assert c.value == 100
descr.clear(c)
assert c.value == 42

class D:
    calls = 0

    @cached_classproperty
    def answer(cls):
        "answer doc"
        cls.calls += 1
        return cls.calls + 10

class E(D):
    pass

cp = D.__dict__["answer"]
assert cp.__doc__ == "answer doc"
assert cp.__name__ == "answer"
assert cp.name == "answer"
first_answer = D.answer
assert first_answer == D.answer
assert E.answer >= first_answer

class AsyncC:
    def __init__(self):
        self.hits = 0

    @async_cached_property
    async def value(self):
        self.hits += 1
        return 55

async def main():
    ac = AsyncC()
    adescr = AsyncC.__dict__["value"]
    assert adescr is not None
    assert await ac.value == 55
    assert await ac.value == 55
    assert ac.hits == 1

    class AsyncD:
        calls = 0

        @async_cached_classproperty
        async def answer(cls):
            cls.calls += 1
            return cls.calls + 80

    assert await AsyncD.answer == 81
    assert await AsyncD.answer == 81
    assert AsyncD.calls == 1

asyncio.run(main())

)");
}

TEST_F(NoJitSanityTest, CachedPropertyDescriptorRuntimeCoverage) {
  runStockCode(R"(
from cinderx import cached_property

class Named:
    def get_value(self):
        return 99

named_descr = cached_property(Named.get_value)
named_descr.__set_name__(Named, "renamed")
assert named_descr.name == "renamed"

class WithDescr:
    __slots__ = ("base", "slot_value")

    def __init__(self):
        self.base = 10

    def impl(self):
        return self.base + 5

WithDescr.value = cached_property(WithDescr.impl, WithDescr.slot_value)

wd = WithDescr()
assert wd.value == 15
assert WithDescr.__dict__["value"].has_value(wd)
WithDescr.__dict__["value"].clear(wd)
assert wd.value == 15
)");
}

TEST_F(NoJitSanityTest, StrictModuleRuntimeCoverage) {
  runStockCode(R"(
import _cinderx

def expect_error(exc_type, fn):
    try:
        fn()
    except exc_type:
        return
    raise AssertionError("expected error")

def custom_dir():
    return ["custom"]

def module_getattr(name):
    if name == "dynamic":
        return 123
    raise AttributeError(name)

data = {
    "__name__": "mymod",
    "__dir__": custom_dir,
    "__getattr__": module_getattr,
    "x": 1,
    "y": 2,
    5: "five",
    "<imported-from>": [("z", "pkg")],
}
mod = _cinderx.StrictModule(data, False)
assert repr(mod) == "<module 'mymod'>"
assert mod.__name__ == "mymod"
assert mod.x == 1
assert mod.__dict__["y"] == 2
assert mod.__dict__[5] == "five"
assert dir(mod) == ["custom"]
assert mod.dynamic == 123
assert not hasattr(mod, "still_missing")
assert mod.__class__ is __import__("types").ModuleType
assert mod.__patch_enabled__ is False
assert not _cinderx.strict_module_patch_enabled(mod)
expect_error(AttributeError, lambda: mod.missing)
expect_error(AttributeError, lambda: setattr(mod, "x", 9))
expect_error(AttributeError, lambda: delattr(mod, "x"))
expect_error(AttributeError, lambda: _cinderx.strict_module_patch(mod, "x", 9))
expect_error(AttributeError, lambda: _cinderx.strict_module_patch_delete(mod, "x"))

patchable = _cinderx.StrictModule({"__name__": "patchmod", "x": 1}, True)
assert _cinderx.strict_module_patch_enabled(patchable)
_cinderx.strict_module_patch(patchable, "x", 42)
assert patchable.x == 42
_cinderx.strict_module_patch(patchable, "new_value", 99)
assert patchable.new_value == 99
patchable.patch("method_value", 77)
assert patchable.method_value == 77
patchable.patch_delete("method_value")
expect_error(AttributeError, lambda: patchable.method_value)
_cinderx.strict_module_patch_delete(patchable, "new_value")
expect_error(AttributeError, lambda: patchable.new_value)
assert "x" in dir(patchable)
assert patchable.__dict__["x"] == 42
assert patchable.__patch_enabled__ is True

import sys
origin = _cinderx.StrictModule({"__name__": "origin_mod", "value": 10}, True)
sys.modules["origin_mod"] = origin
child = _cinderx.StrictModule({
    "__name__": "child_mod",
    "value": origin.value,
    "<imported-from>": [("value", ("origin_mod", "value"))],
}, True)
assert child.value == 10
origin.patch("value", 20)
child.patch("value", 30)
assert child.value == 30
assert origin.value == 20

nameless = _cinderx.StrictModule({}, False)
expect_error(AttributeError, lambda: nameless.__name__)
assert repr(nameless)
expect_error(TypeError, lambda: _cinderx.StrictModule([], False))
expect_error(TypeError, lambda: _cinderx.StrictModule({}, "bad"))
)");
}

TEST_F(NoJitSanityTest, StockPatternExceptionOpcodeCoverage) {
  runStockCode(R"(
import asyncio

class Point:
    __match_args__ = ("x", "y")

    def __init__(self, x, y):
        self.x = x
        self.y = y

class Manager:
    def __init__(self, value, suppress=False):
        self.value = value
        self.suppress = suppress

    def __enter__(self):
        return self.value

    def __exit__(self, exc_type, exc, tb):
        return self.suppress

class AsyncManager:
    def __init__(self, value):
        self.value = value

    async def __aenter__(self):
        await asyncio.sleep(0)
        return self.value

    async def __aexit__(self, exc_type, exc, tb):
        await asyncio.sleep(0)
        return False

def match_shape(value):
    match value:
        case Point(0, y):
            return y + 1
        case Point(x, y) if x == y:
            return x * 2
        case {"kind": "pair", "items": [a, b]}:
            return a + b
        case (first, *middle, last):
            return first + last + len(middle)
        case int() as number:
            return number + 10
        case _:
            return -1

def except_star_shape(flag):
    total = 0
    try:
        if flag:
            raise ExceptionGroup("group", [ValueError(3), TypeError(5)])
        raise ExceptionGroup("other", [LookupError(7)])
    except* ValueError as group:
        total += group.exceptions[0].args[0]
    except* TypeError as group:
        total += group.exceptions[0].args[0]
    except* LookupError as group:
        total += group.exceptions[0].args[0]
    return total

def with_shape(n):
    total = 0
    with Manager(n) as value, Manager(n + 1, suppress=True) as other:
        total += value + other
        raise RuntimeError("suppressed")
    return total

async def async_with_shape(n):
    total = 0
    async with AsyncManager(n) as value:
        total += value
    return total

def comprehension_unpack_shape(n):
    data = [(i, i + 1) for i in range(n)]
    return sum(a + b for a, b in data if a % 2 == 0)

for i in range(80):
    assert match_shape(Point(0, i)) == i + 1
    assert match_shape(Point(i + 1, i + 1)) == (i + 1) * 2
    assert match_shape({"kind": "pair", "items": [i, i + 1]}) == i * 2 + 1
    assert match_shape((i, i + 1, i + 2, i + 3)) == i * 2 + 5
    assert match_shape(i) == i + 10
    assert except_star_shape(i & 1) in (7, 8)
    assert with_shape(i) == i * 2 + 1
    assert comprehension_unpack_shape(8) > 0

assert asyncio.run(async_with_shape(17)) == 17
)");
}

TEST_F(NoJitSanityTest, StockDescriptorClassOpcodeCoverage) {
  runStockCode(R"(
import functools
import math
import operator

class Descriptor:
    def __init__(self):
        self.values = {}

    def __get__(self, obj, typ=None):
        if obj is None:
            return self
        return self.values.get(id(obj), 0)

    def __set__(self, obj, value):
        self.values[id(obj)] = value

    def __delete__(self, obj):
        self.values.pop(id(obj), None)

class Meta(type):
    @classmethod
    def __prepare__(mcls, name, bases, **kw):
        ns = {"prepared": True}
        ns.update(kw)
        return ns

    def __new__(mcls, name, bases, ns, **kw):
        ns["created_by_meta"] = name.lower()
        return super().__new__(mcls, name, bases, dict(ns))

    def __call__(cls, *args, **kw):
        obj = super().__call__(*args, **kw)
        obj.called_by_meta = True
        return obj

def decorator(fn):
    @functools.wraps(fn)
    def wrapper(*args, **kw):
        return fn(*args, **kw) + 1
    return wrapper

class Host(metaclass=Meta, flag=7):
    field = Descriptor()

    def __init__(self, value):
        self.value = value
        self.field = value + 3

    @property
    def prop(self):
        return self.value * 2

    @prop.setter
    def prop(self, value):
        self.value = value // 2

    @decorator
    def method(self, a, b=2, *args, scale=1, **kw):
        total = self.value + a + b + sum(args) + sum(kw.values())
        return total * scale

def call_shapes(n):
    host = Host(n)
    before = host.field
    host.prop = (n + 4) * 2
    values = [1, 2, 3]
    mapping = {"extra": 4, "more": 5}
    total = host.method(n, *values, scale=2, **mapping)
    del host.field
    after = host.field
    return before + after + total + host.prop + int(host.called_by_meta)

def class_checks():
    assert Host.prepared
    assert Host.flag == 7
    assert Host.created_by_meta == "host"
    assert isinstance(Host.field, Descriptor)
    return len(Host.__mro__) + len(Host.__dict__)

def builtin_call_mix(n):
    values = list(range(n, n + 6))
    total = functools.reduce(operator.add, values, 0)
    total += math.isqrt(total * total)
    total += sum(map(abs, (-n, n + 1, -n - 2)))
    total += max(values) - min(values)
    return total

def namespace_mix(n):
    value = n
    local_name = "value"
    scope = locals()
    globals()["temporary_opcode_name"] = n
    try:
        return scope[local_name] + globals()["temporary_opcode_name"]
    finally:
        del globals()["temporary_opcode_name"]

for i in range(100):
    assert call_shapes(i) > 0
    assert class_checks() > 0
    assert builtin_call_mix(i) > 0
    assert namespace_mix(i) == i * 2
)");
}

TEST_F(StaticSanityTest, StaticClassVTableAndContextPaths) {
  runStaticCode(R"(
from __static__ import ContextDecorator
from typing import final, Literal

class Base:
    def __init__(self, value: int):
        self.value = value

    def f(self) -> int:
        return self.value + 1

    @classmethod
    def make(cls, value: int):
        return cls(value)

    @staticmethod
    def twice(value: int) -> int:
        return value * 2

    @property
    def prop(self) -> int:
        return self.value + 3

class Child(Base):
    def f(self) -> int:
        return self.value + 2

@final
class FinalChild(Child):
    pass

b = Base.make(10)
c = Child.make(10)
f = FinalChild.make(10)
assert b.f() == 11
assert c.f() == 12
assert f.f() == 12
assert Base.twice(4) == 8
assert c.prop == 13
assert isinstance(c, Base)

class Manager(ContextDecorator):
    def __init__(self):
        self.entered = 0
        self.exited = 0

    def __enter__(self):
        self.entered += 1
        return self

    def __exit__(self, exc_type, exc, tb) -> Literal[False]:
        self.exited += 1
        return False

mgr = Manager()
with mgr as token:
    assert token is mgr
assert mgr.entered == 1
assert mgr.exited == 1

@mgr
async def coro(value: int) -> int:
    return value + mgr.entered + mgr.exited

async def drive():
    return await coro(5)

import asyncio
assert asyncio.run(drive()) >= 5
)");
}

TEST_F(StaticSanityTest, StaticPrimitiveExtendedOpcodeCoverage) {
  runStaticCode(R"(
from __static__ import (
    box,
    double,
    int8,
    int16,
    int32,
    int64,
    uint8,
    uint16,
    uint32,
    uint64,
)

def signed_mix(value: int64) -> int64:
    a: int8 = int8(value)
    b: int16 = int16(a + int8(4))
    c: int32 = int32(b * int16(3))
    d: int64 = int64(c) << int64(2)
    d = d >> int64(1)
    d = d | int64(5)
    d = d & int64(127)
    d = d ^ int64(9)
    return d

def unsigned_mix(value: uint64) -> uint64:
    a: uint8 = uint8(value)
    b: uint16 = uint16(a + uint8(2))
    c: uint32 = uint32(b * uint16(5))
    d: uint64 = uint64(c) + uint64(11)
    d = d // uint64(3)
    d = d % uint64(19)
    d = d << uint64(1)
    return d

def float_mix(value: double) -> double:
    total: double = value
    step: double = 0.5
    for _ in range(6):
        total = (total + step) * 1.25 - 0.125
        step = step / 2.0
    return total

def primitive_loop(limit: int64) -> int64:
    i: int64 = 0
    total: int64 = 0
    while i < limit:
        if i & int64(1):
            total += signed_mix(i)
        else:
            total -= int64(unsigned_mix(uint64(i + int64(10))))
        i += 1
    return total

assert box(signed_mix(int64(9))) == (((9 + 4) * 3 << 2) >> 1 | 5) & 127 ^ 9
assert box(unsigned_mix(uint64(17))) == ((((17 + 2) * 5 + 11) // 3) % 19) << 1
assert float_mix(1.0) > 1.0
assert primitive_loop(int64(12)) != 0
)");
}

TEST_F(StaticSanityTest, StaticArrayAndFieldOpcodeCoverage) {
  runStaticCode(R"(
from __static__ import Array, box, clen, double, int64

class Point:
    x: int64
    y: int64
    scale: double

    def __init__(self, x: int64, y: int64):
        self.x = x
        self.y = y
        self.scale = 1.5

    def move(self, dx: int64, dy: int64) -> int64:
        self.x = self.x + dx
        self.y = self.y + dy
        return self.x + self.y

    def total(self) -> int64:
        return self.x + self.y

class Holder:
    peer: "Holder"
    value: int

    def __init__(self, value: int):
        self.value = value
        self.peer = self

    def get(self) -> int:
        return self.peer.value

def array_fill(limit: int) -> Array[int64]:
    arr: Array[int64] = Array[int64](limit)
    i: int64 = 0
    while i < clen(arr):
        arr[i] = i * int64(3)
        i += 1
    arr[int64(1)] = int64(11)
    arr[2] = int64(13)
    return arr

def array_sum(limit: int) -> int:
    arr = array_fill(limit)
    total: int64 = 0
    for value in arr:
        total += value
    total += arr[int64(1)]
    total += arr[2]
    total += arr[-1]
    try:
        total += arr[999]
    except IndexError:
        total += int64(17)
    return box(total)

def field_sum() -> int:
    p = Point(int64(3), int64(4))
    moved: int64 = p.move(int64(5), int64(6))
    p.x = p.x + int64(7)
    p.y = p.y + int64(8)
    h = Holder(23)
    return box(moved + p.total()) + h.get()

assert array_sum(6) > 0
assert field_sum() > 0
)");
}

TEST_F(StaticSanityTest, StaticSequenceOpcodeCoverage) {
  runStaticCode(R"(
from typing import List

def exact_list(index: int) -> int:
    values = [1, 2, 3, 4]
    values[index] = values[index] + 10
    return values[index]

class MyList(list):
    def __getitem__(self, idx):
        return super().__getitem__(idx) + 1

    def __setitem__(self, idx, value):
        super().__setitem__(idx, value + 1)

def list_subclass(values: List[int]) -> int:
    values[1] = 20
    return values[1]

def dynamic_index() -> int:
    return 2

def run() -> int:
    plain: List[int] = [5, 6, 7]
    plain[0] = 30
    sub = MyList([3, 4, 5])
    sub[1] = 10
    return exact_list(2) + list_subclass(plain) + sub[1] + plain[0] + exact_list(dynamic_index())

assert run() > 0
)");
}

TEST_F(StaticSanityTest, StaticPropertyAndThunkCoverage) {
  runStaticCode(R"(
import asyncio
from __static__ import int64, box
from cinderx import async_cached_property, cached_property
from typing import final

class Descriptor:
    def __get__(self, obj, typ=None):
        if obj is None:
            return self
        return lambda value=3: obj.base + value

class Base:
    base: int
    descr = Descriptor()

    def __init__(self, value: int):
        self.base = value

    @property
    def prop(self) -> int:
        return self.base + 1

    @prop.setter
    def prop(self, value: int) -> None:
        self.base = value - 1

    @prop.deleter
    def prop(self) -> None:
        self.base = 0

    @cached_property
    def cached(self) -> int:
        return self.base + 5

    @async_cached_property
    async def async_cached(self) -> int:
        await asyncio.sleep(0)
        return self.base + 7

    @classmethod
    def make(cls, value: int):
        return cls(value)

    @staticmethod
    def stat(value: int) -> int:
        return value + 9

    def primitive(self, value: int64) -> int64:
        return value + int64(self.base)

class Child(Base):
    def primitive(self, value: int64) -> int64:
        return super().primitive(value) + int64(2)

    @property
    def prop(self) -> int:
        return self.base + 2

@final
class Leaf(Child):
    pass

def drive(obj: Base) -> int:
    total = obj.prop
    obj.prop = total + 10
    total += obj.prop
    total += obj.cached
    total += obj.descr()
    total += obj.stat(5)
    total += box(obj.primitive(int64(4)))
    del obj.prop
    total += obj.prop
    return total

def drive_child(obj: Child) -> int:
    total = obj.prop
    total += obj.cached
    total += obj.descr()
    total += obj.stat(5)
    total += box(obj.primitive(int64(4)))
    return total

async def drive_async(obj: Base) -> int:
    return await obj.async_cached

b = Base.make(10)
c = Child.make(10)
l = Leaf.make(10)
assert drive(b) > 0
assert drive_child(c) > 0
assert drive_child(l) > 0
assert asyncio.run(drive_async(Base(20))) == 27
assert asyncio.run(drive_async(Child(20))) == 27
)");
}

TEST_F(StaticSanityTest, StaticExtendedOpcodeMatrixCoverage) {
  runStaticCode(R"(
from __static__ import Array, CheckedDict, CheckedList, box, clen, int64

class ExactList(list):
    pass

class Plain:
    pass

def primitive_locals(seed: int64) -> int:
    a: int64 = seed
    b: int64 = a + int64(5)
    c: int64 = b * int64(3)
    return box(c)

def checked_builders() -> int:
    xs: CheckedList[int] = CheckedList[int]([1, 2, 3, 4])
    ys: CheckedList[int] = CheckedList[int]([5, 6])
    d: CheckedDict[str, int] = CheckedDict[str, int]({"a": 1, "b": 2})
    assert xs[1] > 0
    assert ys[0] > 0
    d["c"] = 7
    total = len(xs) + len(ys) + len(d)
    assert xs[0] > 0
    xs[1] = 22
    assert xs[1] == 22
    assert total > 0
    assert d["c"] == 7
    return 1

def fast_len_shapes() -> int:
    plain_list = [1, 2, 3]
    plain_dict = {"x": 1, "y": 2}
    plain_set = {1, 2, 3, 4}
    plain_tuple = (1, 2, 3, 4, 5)
    plain_str = "abcdef"
    checked: CheckedList[int] = CheckedList[int]([7, 8, 9])
    arr: Array[int64] = Array[int64](4)
    assert len(plain_list) == 3
    assert len(plain_dict) == 2
    assert len(plain_set) == 4
    assert len(plain_tuple) == 5
    assert len(plain_str) == 6
    assert len(checked) == 3
    assert box(clen(arr)) == 4
    return 27

def sequence_shapes(index: int) -> int:
    values = [10, 20, 30, 40]
    values[index] = values[index] + 1
    del values[0]
    exact = ExactList([2, 4, 6, 8])
    exact[2] = exact[2] + 3
    arr: Array[int64] = Array[int64](3)
    arr[0] = int64(11)
    arr[1] = int64(13)
    arr[2] = int64(17)
    assert box(arr[0]) == 11
    assert box(arr[1]) == 13
    assert box(arr[2]) == 17
    assert values[0] > 0
    assert exact[2] > 0
    return 1

def type_shapes(obj: Plain) -> int:
    typ = type(obj)
    if typ is Plain:
        return 1
    return 0

def error_edges() -> int:
    total = 0
    values = [1, 2]
    try:
        del values[100]
    except IndexError:
        total += 3
    arr: Array[int64] = Array[int64](1)
    try:
        arr[5] = int64(1)
    except IndexError:
        total += 5
    return total

for i in range(300):
    assert primitive_locals(int64(i % 17)) >= 15
    assert checked_builders() > 0
    assert fast_len_shapes() == 27
    assert sequence_shapes(i % 3) > 0
    assert type_shapes(Plain()) == 1
    assert error_edges() == 8
)");
}

TEST_F(StaticSanityTest, StaticTargetedExtendedOpcodeCoverage) {
  runStaticCode(R"(
from __static__ import (
    Array,
    CheckedDict,
    CheckedList,
    box,
    cast,
    clen,
    int8,
    int64,
)
from typing import Optional

class MyList(list):
    def __getitem__(self, index):
        return list.__getitem__(self, index) + 1

class Base:
    def value(self) -> int:
        return 5

class Child(Base):
    def value(self) -> int:
        return 7

def typed_call_iter(a: int, b: int, c: str, d: float, e: float) -> int:
    if c == "hi" and d > 0.0 and e > 0.0:
        return a + b
    return -1

def typed_call_map(a: int, b: int, c: str, d: float = -0.1, e: float = 1.1, f: str = "x") -> int:
    if c == "hi" and f == "yo" and d > 0.0 and e > 0.0:
        return a + b
    return -1

def load_iterable_tuple() -> int:
    p = ("hi", 1.0, 2.0)
    return typed_call_iter(1, 3, *p)

def load_iterable_list() -> int:
    p = ["hi", 1.0, 2.0]
    return typed_call_iter(1, 3, *p)

def load_mapping() -> int:
    d = {"d": 1.0}
    return typed_call_map(1, 3, "hi", f="yo", **d)

def checked_map_list() -> int:
    d: CheckedDict[float, str] = {2.0: "hello", 2.3: "world"}
    xs: CheckedList[int] = [1, 2, 3, 4]
    xs.append(5)
    xs[1] = xs[1] + 10
    total = len(d) + len(xs)
    if d[2.0] == "hello":
        total += xs[1]
    return total

def checked_get_set_delete(xs: CheckedList[int]) -> int:
    i: int64 = 0
    j: int8 = 1
    xs[i] = xs[i] + xs[j]
    return xs[i] + len(xs)

def inexact_get(xs: list) -> int:
    i: int64 = 0
    got = xs[-1]
    return got + len(xs)

def array_shapes() -> int:
    arr: Array[int64] = Array[int64](4)
    idx: int64 = 0
    total: int64 = 0
    while idx < clen(arr):
        arr[idx] = idx + int64(10)
        total += arr[idx]
        idx += int64(1)
    return box(total)

def cast_shapes(obj: object, maybe: Optional[int]) -> int:
    child = cast(Child, obj)
    value = child.value()
    value += cast(int, 4)
    if cast(Optional[int], maybe) is None:
        value += 3
    return value

for i in range(120):
    assert load_iterable_tuple() == 4
    assert load_iterable_list() == 4
    assert load_mapping() == 4
    assert checked_map_list() == 19
    assert checked_get_set_delete(CheckedList[int]([10, 3, 2])) == 16
    assert inexact_get(MyList([1, 2, 3])) == 7
    assert array_shapes() == 46
    assert cast_shapes(Child(), None) == 14
)");
}
