"""Observable semantics, including warmed caches and hostile dictionary keys."""
import dis
import json
import os
import sys
from pathlib import Path

cases = {}

def capture(fn):
    try:
        return fn()
    except Exception as error:
        return {"error": type(error).__name__, "message": str(error)}

def warm(fn, obj, count=2000):
    for _ in range(count):
        fn(obj)

def attr(o): return o.x
def method(o): return o.f()

class Hook:
    def __getattr__(self, name):
        return "fallback:" + name
    def f(self):
        return "method"

obj = Hook()
obj.x = 11
warm(attr, obj)
cases["instance_value"] = attr(obj)
obj.__dict__
warm(attr, obj)
cases["materialized_split"] = attr(obj)
obj.x = 12
cases["value_reassignment"] = attr(obj)
del obj.x
cases["delete_to_fallback"] = attr(obj)
Hook.__getattr__ = lambda self, name: "changed:" + name
cases["replace_getattr"] = attr(obj)
obj.x = 13
warm(attr, obj)
Hook.__getattribute__ = lambda self, name: 99 if name == "x" else object.__getattribute__(self, name)
cases["replace_getattribute"] = attr(obj)
del Hook.__getattribute__
warm(attr, obj)
Hook.x = property(lambda self: 14)
cases["late_data_descriptor"] = attr(obj)
del Hook.x

class Alternate(Hook):
    @property
    def x(self):
        return 15
warm(attr, obj)
obj.__class__ = Alternate
cases["change_class"] = attr(obj)
obj.__class__ = Hook
obj.__dict__ = {"x": 16}
warm(attr, obj)
cases["replace_dict"] = attr(obj)
obj.__dict__ = {"x": 17, "f": lambda: "instance-method"}
cases["method_instance_shadow"] = method(obj)
del obj.__dict__["f"]
warm(method, obj)
cases["method_unshadow"] = method(obj)
obj.f = lambda: "late-shadow"
cases["method_late_shadow"] = method(obj)

class Raise(Hook):
    @property
    def x(self):
        raise AttributeError("inside-property")
cases["property_attribute_error"] = attr(Raise())
class RaiseOther(Hook):
    @property
    def x(self):
        raise ValueError("inside-property")
cases["property_other_error"] = capture(lambda: attr(RaiseOther()))

class Slots:
    __slots__ = ("x",)
    def __getattr__(self, name): return "unset:" + name
    def f(self): return 20
s = Slots()
s.x = 18
warm(attr, s)
cases["slot_set"] = attr(s)
del s.x
cases["slot_unset"] = attr(s)
warm(method, s)
cases["method_no_dict"] = method(s)

class BaseA:
    def f(self): return "A"
class BaseB:
    def f(self): return "B"
class Derived(BaseA, Hook): pass
d = Derived()
warm(method, d)
Derived.__bases__ = (BaseB, Hook)
cases["change_bases"] = method(d)

class Custom(Hook):
    def __getattribute__(self, name):
        return 123 if name == "x" else object.__getattribute__(self, name)
c = Custom()
c.__dict__["x"] = 999
warm(attr, c)
cases["custom_getattribute"] = attr(c)
poly = [Hook(), Custom(), Slots()]
poly[0].x = 21
poly[2].x = 22
cases["polymorphic"] = [attr(poly[i % 3]) for i in range(12)]

# Replace a warm hinted dictionary by one with a colliding non-string key
# inserted BEFORE the real attribute. Equality calls are observable even if
# the identity-matching attribute happens to occupy the old hint index.
for raises in (False, True):
    events = []
    class Collision:
        def __hash__(self): return hash("x")
        def __eq__(self, other):
            events.append(str(other))
            if raises: raise LookupError("collision")
            return False
    o = Hook()
    o.pad = 0
    o.x = 23
    o.__dict__
    warm(attr, o)
    # Build while equality is benign; turn error mode on only for reads.
    desired = raises
    raises = False
    dictionary = {Collision(): 0, "x": 24}
    raises = desired
    o.__dict__ = dictionary
    events.clear()
    first = capture(lambda: attr(o))
    # Attempt enough reads to trigger specialization retries.
    results = [capture(lambda: attr(o)) for _ in range(100)]
    cases["mixed_keys_error" if desired else "mixed_keys"] = {
        "first": first, "last": results[-1], "comparisons": len(events)}

# A string subclass can have equality side effects as well.
events = []
class StringKey(str):
    __hash__ = str.__hash__
    def __eq__(self, other):
        events.append(str(other))
        return super().__eq__(other)
o = Hook()
o.__dict__ = {StringKey("x"): 25}
values = [attr(o) for _ in range(100)]
cases["string_subclass_key"] = [values[-1], len(events)]

trace = []
o = Hook()
o.x = 26
warm(attr, o)
def tracer(frame, event, arg):
    if frame.f_code in (attr.__code__, method.__code__):
        trace.append((frame.f_code.co_name, event))
    return tracer
sys.settrace(tracer)
try:
    result = [attr(o), method(o)]
finally:
    sys.settrace(None)
cases["trace"] = [result, trace]

import torch
torch.set_num_threads(1)
class Module(torch.nn.Module):
    def __init__(self):
        super().__init__()
        self.x = 27
    def f(self):
        return self.x + 1
m = Module()
warm(attr, m)
warm(method, m)
cases["module_ordinary"] = [attr(m), method(m)]
del m.x
m.register_buffer("x", torch.tensor(28))
cases["module_buffer_transition"] = int(attr(m))
del m.x
m.register_parameter("x", torch.nn.Parameter(torch.tensor(29.)))
cases["module_parameter_transition"] = float(attr(m).detach())
del m.x
m.add_module("x", torch.nn.ReLU())
cases["module_child_transition"] = type(attr(m)).__name__
del m.x
cases["module_final_missing"] = capture(lambda: attr(m))

# New, monomorphic call sites for opcode proof; do not mix with the above.
def split_read(v): return v.x
def inline_read(v): return v.x
def slot_read(v): return v.x
def module_call(v): return v.f()
def nodict_call(v): return v.f()
def virtual_call(v): return v.f()
def opcodes(fn):
    return [{"opname": i.opname, "argrepr": i.argrepr}
            for i in dis.get_instructions(fn, adaptive=True, show_caches=True)]
fresh = Module()
class Virtual:
    def __getattr__(self, name): return "virtual:" + name
    def f(self): return self.x
virtual = Virtual()
virtual.x = 30
slot = Slots()
slot.x = 31
proof = {}
for fn, value, expected in [
    (split_read, fresh, "LOAD_ATTR_WITH_HINT"),
    (inline_read, virtual, "LOAD_ATTR_INSTANCE_VALUE"),
    (slot_read, slot, "LOAD_ATTR_SLOT"),
    (module_call, fresh, "LOAD_METHOD_WITH_DICT"),
    (nodict_call, slot, "LOAD_METHOD_NO_DICT"),
    (virtual_call, virtual, "LOAD_METHOD_WITH_VALUES"),
]:
    warm(fn, value)
    before = opcodes(fn)
    warm(fn, value, 16)
    after = opcodes(fn)
    proof[fn.__name__] = {"before": before, "after": after, "expected": expected}
    if os.environ.get("ATTR_PATH_MODE") == "cinderx" and __import__("_cinderx_auto")._verify()["attribute"]["specialization"]:
        if fn is split_read and os.environ.get("ATTR_PATH_SPLIT_HINT", "1") == "0":
            # Intentional ablation: a split dictionary cannot hit WITH_HINT
            # without the values-array fix and may already be adaptive again.
            continue
        assert expected in [i["opname"] for i in after], (fn.__name__, after)
        if os.environ.get("ATTR_PATH_SPLIT_HINT", "1") == "1":
            # Stable miss counters prove execution hits, not just a specialized
            # opcode that deoptimizes on every attempt.
            def attribute_caches(instructions):
                result = []
                active = False
                for ins in instructions:
                    if ins['opname'] != 'CACHE':
                        active = ins['opname'].startswith(('LOAD_ATTR', 'LOAD_METHOD'))
                    if active:
                        result.append(ins)
                return result
            assert attribute_caches(before) == attribute_caches(after), (fn.__name__, before, after)
runtime = {}
if "_cinderx" in sys.modules:
    import _cinderx_auto
    runtime = _cinderx_auto._verify()
    assert not any(runtime["jit"].values())
record = {"cases": cases, "proof": proof, "runtime": runtime, "python": sys.version}
Path(os.environ["SPECGA_OUTPUT"]).write_text(json.dumps(record, indent=2))
print("SPECGA_PASS", len(cases))
