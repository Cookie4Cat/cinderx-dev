import argparse
import ast
import asyncio
from dataclasses import dataclass
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

if "--osr-case" in sys.argv:
    class _PytestStub:
        class mark:
            @staticmethod
            def parametrize(*args, **kwargs):
                def decorator(func):
                    return func

                return decorator

    pytest = _PytestStub()
else:
    import pytest


HELPER = Path(__file__).resolve()
DEFAULT_TIMEOUT = 120


@dataclass(frozen=True)
class OSRScenario:
    name: str
    comment: str
    case_name: str
    env: dict[str, str]
    results: tuple[str, ...] = ()
    perf_mode: str | None = None
    osr_entries: tuple[str, ...] = ()
    no_osr_entries: tuple[str, ...] = ()
    not_compiled: tuple[str, ...] = ()
    deopt_reasons: tuple[str, ...] = ()
    timeout: int = DEFAULT_TIMEOUT


class OSRDeoptError(Exception):
    pass


def _expect(name, actual, expected):
    assert actual == expected, f"{name}: {actual!r} != {expected!r}"
    print(f"CASE_RESULT {name} OK {actual}")


def _clean_env():
    env = os.environ.copy()
    for key in (
        "CINDERX_DISABLE",
        "CINDERX_JIT_DISABLE",
        "CINDERX_OSR_BACKEDGE_THRESHOLD",
        "CINDERX_OSR_COMPILE_BUDGET",
        "CINDERX_OSR_ENABLED",
        "CINDERX_PLUGIN_ENABLE",
        "OSR_PERF_CHUNK",
        "OSR_PERF_MODE",
        "OSR_PERF_N",
        "PYTHONJITALL",
        "PYTHONJITAUTO",
        "PYTHONJITDEBUG",
        "PYTHONJITDISABLE",
        "PYTHONJITDUMPHIR",
        "PYTHONJITDUMPSTATS",
        "PYTHONJITLIGHTWEIGHTFRAME",
        "PYTHONJITLISTFILE",
        "PYTHONPATH",
    ):
        env.pop(key, None)
    return env


def _run_scenario(scenario):
    env = _clean_env()
    env.update(scenario.env)
    env.setdefault("PYTHONJITLIGHTWEIGHTFRAME", "0")
    env["PYTHONUNBUFFERED"] = "1"

    with tempfile.TemporaryDirectory(prefix=f"kunpeng-osr-{scenario.name}-") as cwd:
        completed = subprocess.run(
            [sys.executable, str(HELPER), "--osr-case", scenario.case_name],
            cwd=cwd,
            env=env,
            capture_output=True,
            text=True,
            timeout=scenario.timeout,
        )

    output = completed.stdout + completed.stderr
    assert completed.returncode == 0, (
        f"{scenario.name}: subprocess failed with {completed.returncode}\n"
        f"# {scenario.comment}\n"
        f"stdout:\n{completed.stdout}\n"
        f"stderr:\n{completed.stderr}"
    )
    assert "Traceback" not in output, output
    return output


def _function_block(output, fullname):
    pattern = re.compile(
        rf"Initial HIR for {re.escape(fullname)}:\n(?P<body>.*?)(?:\nJIT: .* -- Finished compiling {re.escape(fullname)}|\Z)",
        re.DOTALL,
    )
    match = pattern.search(output)
    return match.group("body") if match else ""


def _compile_count(output, fullname):
    return len(re.findall(rf"-- Compiling {re.escape(fullname)}(?:\s|$)", output))


def _has_osr_entry(output, fullname):
    return "OSREntry" in _function_block(output, fullname)


def _runtime_stats(output):
    match = re.search(r"JIT runtime stats:\n(?P<stats>\{.*?\})(?:\n|$)", output, re.DOTALL)
    if not match:
        return {}
    return ast.literal_eval(match.group("stats"))


def _assert_scenario_output(scenario, output):
    for result_name in scenario.results:
        assert f"CASE_RESULT {result_name} OK" in output

    if scenario.perf_mode is not None:
        assert f"PERF_RESULT {scenario.perf_mode} " in output

    for fullname in scenario.osr_entries:
        assert _has_osr_entry(output, fullname), (
            f"{scenario.name}: expected OSREntry in {fullname}\n{output}"
        )

    for fullname in scenario.no_osr_entries:
        assert not _has_osr_entry(output, fullname), (
            f"{scenario.name}: forbidden OSREntry appeared in {fullname}\n{output}"
        )

    for fullname in scenario.not_compiled:
        count = _compile_count(output, fullname)
        assert count == 0, (
            f"{scenario.name}: {fullname} compiled {count} time(s), expected 0\n"
            f"{output}"
        )

    deopts = _runtime_stats(output).get("deopt", [])
    reasons = {
        event.get("normal", {}).get("reason")
        for event in deopts
        if isinstance(event, dict)
    }
    for reason in scenario.deopt_reasons:
        assert reason in reasons, (
            f"{scenario.name}: missing deopt reason {reason!r}; "
            f"saw {sorted(reasons)!r}\n{output}"
        )


def add(left, right):
    return left + right


def hot_simple_while(limit):
    index = 0
    total = 0
    while index < limit:
        index += 1
        total += index
    return total


def hot_nested_while(outer_limit, inner_limit):
    outer = 0
    total = 0
    while outer < outer_limit:
        inner = 0
        while inner < inner_limit:
            inner += 1
            total += inner
        outer += 1
    return total


def hot_multiple_while(limit):
    total = 0
    left = 0
    while left < limit:
        left += 1
        total += left

    right = 0
    while right < limit:
        right += 1
        total += right * 2
    return total


def hot_with_call(limit):
    index = 0
    total = 0
    while index < limit:
        index += 1
        total = add(total, index)
    return total


def case_supported_while():
    simple_limit = 5000
    _expect(
        "hot_simple_while",
        hot_simple_while(simple_limit),
        simple_limit * (simple_limit + 1) // 2,
    )

    outer_limit = 20
    inner_limit = 250
    _expect(
        "hot_nested_while",
        hot_nested_while(outer_limit, inner_limit),
        outer_limit * inner_limit * (inner_limit + 1) // 2,
    )

    multi_limit = 2500
    _expect(
        "hot_multiple_while",
        hot_multiple_while(multi_limit),
        3 * multi_limit * (multi_limit + 1) // 2,
    )

    call_limit = 5000
    _expect(
        "hot_with_call",
        hot_with_call(call_limit),
        call_limit * (call_limit + 1) // 2,
    )


def osr_sum_while(limit):
    i = 0
    total = 0
    while i < limit:
        i += 1
        total += i
    return total


def osr_multi_locals(limit):
    i = 0
    total = 0
    scale = 3
    bias = 7
    value = 0
    while i < limit:
        i += 1
        value = i * scale + bias
        total += value
    return total + scale + bias + value


def osr_list_append(limit):
    i = 0
    values = []
    while i < limit:
        values.append(i & 3)
        i += 1
    return len(values), sum(values), values[:4], values[-4:]


def osr_alias_state(limit):
    i = 0
    box = []
    alias = box
    while i < limit:
        alias.append(i & 7)
        i += 1
    return box is alias, len(box), box[-1], sum(box)


def osr_float_accumulate(limit):
    i = 0
    total = 0.0
    while i < limit:
        total += 0.5
        i += 1
    return total


def osr_string_build(limit):
    i = 0
    total = 0
    text = "kunpeng-osr"
    while i < limit:
        total += len(text)
        i += 1
    return total


def _repeating_mask_sum(limit, mask):
    cycle = mask + 1
    full_cycles, remainder = divmod(limit, cycle)
    return full_cycles * (mask * cycle // 2) + remainder * (remainder - 1) // 2


def case_b017_state_mapping():
    limit = 5000
    _expect("osr_sum_while", osr_sum_while(limit), limit * (limit + 1) // 2)

    scale = 3
    bias = 7
    last_value = limit * scale + bias
    _expect(
        "osr_multi_locals",
        osr_multi_locals(limit),
        scale * limit * (limit + 1) // 2 + bias * limit + scale + bias + last_value,
    )

    _expect(
        "osr_list_append",
        osr_list_append(limit),
        (limit, _repeating_mask_sum(limit, 3), [0, 1, 2, 3], [0, 1, 2, 3]),
    )

    _expect(
        "osr_alias_state",
        osr_alias_state(limit),
        (True, limit, (limit - 1) & 7, _repeating_mask_sum(limit, 7)),
    )

    _expect("osr_float_accumulate", osr_float_accumulate(limit), limit * 0.5)
    _expect("osr_string_build", osr_string_build(limit), limit * len("kunpeng-osr"))


def case_b017_low_heat_sum_while():
    limit = 1000
    _expect("osr_sum_while_low_heat", osr_sum_while(limit), limit * (limit + 1) // 2)


def hot_threshold_loop(limit):
    index = 0
    total = 0
    while index < limit:
        index += 1
        total += index
    return total


def case_threshold_loop():
    limit = 5000
    _expect("hot_threshold_loop", hot_threshold_loop(limit), limit * (limit + 1) // 2)


def for_loop_unsupported(limit):
    total = 0
    for value in range(limit):
        total += value
    return total


def generator_unsupported(limit):
    index = 0
    while index < limit:
        index += 1
        yield index


def protected_loop(limit):
    index = 0
    total = 0
    try:
        while index < limit:
            index += 1
            total += index
    except Exception:
        total = -1
    return total


def budget_loop(limit):
    index = 0
    total = 0
    salt = 1
    while index < limit:
        index += 1
        total += index
        salt = (salt * 3 + index) % 97
    return total + salt


TOP_LEVEL_WHILE_TOTAL = None
if "--osr-case" in sys.argv and "unsupported_shapes" in sys.argv:
    TOP_LEVEL_WHILE_TOTAL = 0
    top_index = 0
    while top_index < 300:
        top_index += 1
        TOP_LEVEL_WHILE_TOTAL += top_index


def case_unsupported_shapes():
    _expect("top_level_while", TOP_LEVEL_WHILE_TOTAL, 300 * 301 // 2)

    for_limit = 1000
    _expect("for_loop_unsupported", for_loop_unsupported(for_limit), sum(range(for_limit)))

    gen_limit = 400
    _expect(
        "generator_unsupported",
        sum(generator_unsupported(gen_limit)),
        gen_limit * (gen_limit + 1) // 2,
    )

    protected_limit = 1000
    _expect(
        "protected_loop",
        protected_loop(protected_limit),
        protected_limit * (protected_limit + 1) // 2,
    )

    budget_limit = 1000
    expected = budget_limit * (budget_limit + 1) // 2
    salt = 1
    for index in range(1, budget_limit + 1):
        salt = (salt * 3 + index) % 97
    _expect("budget_loop", budget_loop(budget_limit), expected + salt)


def escaped_frame_loop(limit):
    frame = sys._getframe()
    assert frame.f_code.co_name == "escaped_frame_loop"

    index = 0
    total = 0
    while index < limit:
        index += 1
        total += index
    return total


def case_escaped_frame():
    limit = 1000
    _expect("escaped_frame_loop", escaped_frame_loop(limit), limit * (limit + 1) // 2)


async def coroutine_unsupported(limit):
    index = 0
    total = 0
    while index < limit:
        index += 1
        total += index
    return total


async def async_generator_unsupported(limit):
    index = 0
    while index < limit:
        index += 1
        yield index


async def _run_coroutine_cases():
    coro_limit = 1000
    coro_actual = await coroutine_unsupported(coro_limit)
    _expect("coroutine_unsupported", coro_actual, coro_limit * (coro_limit + 1) // 2)

    agen_limit = 400
    agen_actual = 0
    async for value in async_generator_unsupported(agen_limit):
        agen_actual += value
    _expect(
        "async_generator_unsupported",
        agen_actual,
        agen_limit * (agen_limit + 1) // 2,
    )


def case_coroutine_async_generator():
    asyncio.run(_run_coroutine_cases())


def two_cold_backedges(limit):
    total = 0

    left = 0
    while left < limit:
        left += 1
        total += left

    right = 0
    while right < limit:
        right += 1
        total += right * 2

    return total


def case_per_backedge_threshold():
    limit = 60
    _expect("two_cold_backedges", two_cold_backedges(limit), 3 * limit * (limit + 1) // 2)


def budget_rejected_loop(limit):
    index = 0
    total = 0
    salt = 11
    while index < limit:
        index += 1
        total += index
        salt = (salt * 5 + index) % 131
    return total + salt


def case_persistent_budget_failure():
    limit = 8000
    expected = limit * (limit + 1) // 2
    salt = 11
    for index in range(1, limit + 1):
        salt = (salt * 5 + index) % 131
    _expect("budget_rejected_loop", budget_rejected_loop(limit), expected + salt)


def step_for_deopt(index, switch_at):
    if index >= switch_at:
        return index + 1.0
    return index + 1


def hot_type_deopt(limit, switch_at):
    index = 0
    total = 0
    while index < limit:
        index = step_for_deopt(index, switch_at)
        total += index
    return total


def case_deopt_after_osr():
    limit = 5000
    _expect("hot_type_deopt", hot_type_deopt(limit, 2000), float(limit * (limit + 1) // 2))


def step_for_exception(index, switch_at):
    if index >= switch_at:
        return index + 1.0
    return index + 1


def hot_deopt_then_raise(limit, switch_at, raise_at):
    index = 0
    total = 0
    while index < limit:
        index = step_for_exception(index, switch_at)
        total += index
        if index >= raise_at:
            raise OSRDeoptError(f"osr exception at {int(index)}")
    return total


def case_osr_exception_propagation():
    try:
        hot_deopt_then_raise(5000, 1500, 1600)
    except OSRDeoptError as exc:
        assert str(exc) == "osr exception at 1600", str(exc)
        print("CASE_RESULT hot_deopt_then_raise OK osr exception at 1600")
    else:
        raise AssertionError("expected OSRDeoptError")


def hot_break_continue(limit, break_at):
    index = 0
    total = 0
    while index < limit:
        index += 1
        if index % 7 == 0:
            continue
        if index == break_at:
            break
        total += index
    return total


def hot_early_return(limit, return_at):
    index = 0
    total = 0
    while index < limit:
        index += 1
        total += index
        if index == return_at:
            return total
    return -1


def case_osr_control_flow():
    break_limit = 5000
    break_at = 3000
    _expect(
        "hot_break_continue",
        hot_break_continue(break_limit, break_at),
        sum(value for value in range(1, break_at) if value % 7 != 0),
    )

    return_at = 3000
    _expect(
        "hot_early_return",
        hot_early_return(5000, return_at),
        return_at * (return_at + 1) // 2,
    )


def bump(value):
    return value + 1


def hot_loop(limit):
    index = 0
    total = 0
    while index < limit:
        index = bump(index)
        total += index
    return total


def arithmetic_hot_loop(limit):
    index = 0
    total = 0
    while index < limit:
        index += 1
        total += index * 3 + index % 11
    return total


def chunked_hot_loop(limit, chunk_size):
    index = 0
    total = 0
    chunk_index = 0
    chunk_start = time.perf_counter()
    while index < limit:
        index = bump(index)
        total += index
        if index % chunk_size == 0:
            now = time.perf_counter()
            print(f"PERF_CHUNK {chunk_index} {now - chunk_start:.9f}")
            chunk_start = now
            chunk_index += 1
    return total


def case_perf_hot_loop():
    mode = os.environ.get("OSR_PERF_MODE", "long")
    limit = int(os.environ.get("OSR_PERF_N", "20000"))
    chunk_size = int(os.environ.get("OSR_PERF_CHUNK", "5000"))

    start = time.perf_counter()
    if mode == "chunks":
        result = chunked_hot_loop(limit, chunk_size)
        expected = limit * (limit + 1) // 2
    elif mode == "arith":
        result = arithmetic_hot_loop(limit)
        full_cycles, remainder = divmod(limit, 11)
        expected = (
            3 * limit * (limit + 1) // 2
            + full_cycles * 55
            + remainder * (remainder + 1) // 2
        )
    else:
        result = hot_loop(limit)
        expected = limit * (limit + 1) // 2
    elapsed = time.perf_counter() - start

    assert result == expected, f"{result!r} != {expected!r}"
    print(f"PERF_RESULT {mode} {limit} {elapsed:.9f} {result}")


CASES = {
    "b017_low_heat_sum_while": case_b017_low_heat_sum_while,
    "b017_state_mapping": case_b017_state_mapping,
    "coroutine_async_generator": case_coroutine_async_generator,
    "deopt_after_osr": case_deopt_after_osr,
    "escaped_frame": case_escaped_frame,
    "osr_control_flow": case_osr_control_flow,
    "osr_exception_propagation": case_osr_exception_propagation,
    "per_backedge_threshold": case_per_backedge_threshold,
    "perf_hot_loop": case_perf_hot_loop,
    "persistent_budget_failure": case_persistent_budget_failure,
    "supported_while": case_supported_while,
    "threshold_loop": case_threshold_loop,
    "unsupported_shapes": case_unsupported_shapes,
}


def _osr_env(**overrides):
    env = {
        "CINDERX_PLUGIN_ENABLE": "1",
        "CINDERX_OSR_ENABLED": "1",
        "CINDERX_OSR_BACKEDGE_THRESHOLD": "100",
        "PYTHONJITAUTO": "10",
        "PYTHONJITDEBUG": "1",
        "PYTHONJITDUMPHIR": "1",
        "PYTHONJITDUMPSTATS": "1",
    }
    env.update({key: str(value) for key, value in overrides.items()})
    return env


SCENARIOS = (
    # 用例 1：OSR 开启后，普通函数帧里的 while 热循环应生成 OSREntry。
    OSRScenario(
        name="osr_enabled_supported_while",
        comment="支持普通 while、嵌套 while、同一函数多个 while 回边和循环内函数调用。",
        case_name="supported_while",
        env=_osr_env(),
        results=("hot_simple_while", "hot_nested_while", "hot_multiple_while", "hot_with_call"),
        osr_entries=(
            "__main__:hot_simple_while",
            "__main__:hot_nested_while",
            "__main__:hot_multiple_while",
            "__main__:hot_with_call",
        ),
    ),
    # B017 S1-S6：OSR 入口要能迁移常见 Python 端到端循环状态。
    OSRScenario(
        name="b017_osr_python_state_mapping",
        comment="覆盖计数 while、多局部变量、list append、对象 alias、float accumulate 和 string read。",
        case_name="b017_state_mapping",
        env=_osr_env(),
        results=(
            "osr_sum_while",
            "osr_multi_locals",
            "osr_list_append",
            "osr_alias_state",
            "osr_float_accumulate",
            "osr_string_build",
        ),
        osr_entries=(
            "__main__:osr_sum_while",
            "__main__:osr_multi_locals",
            "__main__:osr_list_append",
            "__main__:osr_alias_state",
            "__main__:osr_float_accumulate",
            "__main__:osr_string_build",
        ),
    ),
    # B017 S7：同一计数 while 在低热度时应保持解释执行。
    OSRScenario(
        name="b017_low_heat_sum_while_stays_interpreted",
        comment="osr_sum_while(1000) 低于阈值 2000，dump 中不应出现目标函数编译记录。",
        case_name="b017_low_heat_sum_while",
        env=_osr_env(CINDERX_OSR_BACKEDGE_THRESHOLD="2000"),
        results=("osr_sum_while_low_heat",),
        not_compiled=("__main__:osr_sum_while",),
    ),
    # 用例 2：只关闭 OSR 时，单次函数调用里的热循环不应触发 OSR 编译。
    OSRScenario(
        name="osr_disabled_no_hot_loop_compile",
        comment="PYTHONJITAUTO=10 时目标函数只调用一次，关闭 OSR 后不应出现编译日志。",
        case_name="threshold_loop",
        env=_osr_env(CINDERX_OSR_ENABLED="0"),
        results=("hot_threshold_loop",),
        not_compiled=("__main__:hot_threshold_loop",),
    ),
    # 用例 3：插件关闭时，即使设置 JIT/OSR 环境变量也不能编译目标函数。
    OSRScenario(
        name="plugin_disabled_no_osr",
        comment="CINDERX_PLUGIN_ENABLE=0 覆盖未启用 CinderX 插件的 baseline 路径。",
        case_name="threshold_loop",
        env=_osr_env(CINDERX_PLUGIN_ENABLE="0"),
        results=("hot_threshold_loop",),
        not_compiled=("__main__:hot_threshold_loop",),
    ),
    # 用例 4：JIT 总开关关闭时，OSR 不能单独触发目标函数编译。
    OSRScenario(
        name="jit_disabled_no_osr",
        comment="PYTHONJITDISABLE=1 是更强的 JIT 关闭开关，OSR 应随之关闭。",
        case_name="threshold_loop",
        env=_osr_env(PYTHONJITDISABLE="1"),
        results=("hot_threshold_loop",),
        not_compiled=("__main__:hot_threshold_loop",),
    ),
    # 用例 5：低 OSR 回边阈值应触发热循环 OSR。
    OSRScenario(
        name="low_threshold_triggers_osr",
        comment="阈值 50 低于循环次数，目标函数 HIR 中应包含 OSREntry。",
        case_name="threshold_loop",
        env=_osr_env(CINDERX_OSR_BACKEDGE_THRESHOLD="50"),
        results=("hot_threshold_loop",),
        osr_entries=("__main__:hot_threshold_loop",),
    ),
    # 用例 6：高 OSR 回边阈值不能误触发热循环 OSR。
    OSRScenario(
        name="high_threshold_does_not_trigger_osr",
        comment="阈值 100000 高于循环次数，目标函数不应被编译。",
        case_name="threshold_loop",
        env=_osr_env(CINDERX_OSR_BACKEDGE_THRESHOLD="100000"),
        results=("hot_threshold_loop",),
        not_compiled=("__main__:hot_threshold_loop",),
    ),
    # 用例 7：不支持的形态应安全降级，并保持 Python 语义。
    OSRScenario(
        name="unsupported_shapes_degrade",
        comment="覆盖顶层 while、for、generator、异常保护区 while 和预算相关循环。",
        case_name="unsupported_shapes",
        env=_osr_env(),
        results=(
            "top_level_while",
            "for_loop_unsupported",
            "generator_unsupported",
            "protected_loop",
            "budget_loop",
        ),
        no_osr_entries=(
            "__main__:for_loop_unsupported",
            "__main__:generator_unsupported",
            "__main__:protected_loop",
        ),
    ),
    # 用例 8：当前 Python frame 逃逸后不应触发 OSR。
    OSRScenario(
        name="escaped_frame_unsupported",
        comment="sys._getframe() 会 materialize 当前帧，OSR 应保持解释执行。",
        case_name="escaped_frame",
        env=_osr_env(),
        results=("escaped_frame_loop",),
        not_compiled=("__main__:escaped_frame_loop",),
    ),
    # 用例 9：coroutine 和 async generator 帧内热循环不应触发 OSR。
    OSRScenario(
        name="coroutine_async_generator_unsupported",
        comment="特殊帧类型的 await/async for 结果要正确，目标函数不应被 OSR 编译。",
        case_name="coroutine_async_generator",
        env=_osr_env(),
        results=("coroutine_unsupported", "async_generator_unsupported"),
        not_compiled=("__main__:coroutine_unsupported", "__main__:async_generator_unsupported"),
    ),
    # 用例 10：OSR 阈值按单条回边独立计数。
    OSRScenario(
        name="per_backedge_independent_threshold",
        comment="两个各跑 60 次的冷回边不能被错误聚合成一个超过阈值 100 的热回边。",
        case_name="per_backedge_threshold",
        env=_osr_env(),
        results=("two_cold_backedges",),
        not_compiled=("__main__:two_cold_backedges",),
    ),
    # 用例 11：编译预算不足时，OSR 应安全拒绝并继续解释执行。
    OSRScenario(
        name="compile_budget_failure_is_not_retried",
        comment="CINDERX_OSR_COMPILE_BUDGET=4 故意过低，目标函数不能成功编译。",
        case_name="persistent_budget_failure",
        env=_osr_env(CINDERX_OSR_BACKEDGE_THRESHOLD="20", CINDERX_OSR_COMPILE_BUDGET="4"),
        results=("budget_rejected_loop",),
        not_compiled=("__main__:budget_rejected_loop",),
    ),
    # 用例 12：OSR 后类型变化应触发 GuardFailure deopt，结果仍保持正确。
    OSRScenario(
        name="osr_then_guard_deopt",
        comment="先进入 OSR，再让 loop-carried value 发生 int 到 float 的类型变化。",
        case_name="deopt_after_osr",
        env=_osr_env(),
        results=("hot_type_deopt",),
        osr_entries=("__main__:hot_type_deopt",),
        deopt_reasons=("GuardFailure",),
    ),
    # 用例 13：OSR/deopt 后抛出的异常仍应按 Python 语义传播到调用方。
    OSRScenario(
        name="osr_exception_propagation",
        comment="热循环中类型变化 deopt 后继续执行，并抛出可被外层捕获的自定义异常。",
        case_name="osr_exception_propagation",
        env=_osr_env(),
        results=("hot_deopt_then_raise",),
        osr_entries=("__main__:hot_deopt_then_raise",),
        deopt_reasons=("GuardFailure",),
    ),
    # 用例 14：OSR 后 break、continue 和循环内提前 return 的控制流结果应正确。
    OSRScenario(
        name="osr_break_continue_return",
        comment="覆盖 continue 跳过、break 退出和 loop body 内提前 return。",
        case_name="osr_control_flow",
        env=_osr_env(),
        results=("hot_break_continue", "hot_early_return"),
        osr_entries=("__main__:hot_break_continue", "__main__:hot_early_return"),
    ),
    # 用例 15：性能脚本 long 模式保留为轻量正确性测试，不再做耗时性能对比。
    OSRScenario(
        name="perf_hot_loop_long_result",
        comment="普通 while 循环含 bump() 调用，验证迁移后的 perf case 能独立执行并产生 PERF_RESULT。",
        case_name="perf_hot_loop",
        env=_osr_env(OSR_PERF_MODE="long", OSR_PERF_N="20000", OSR_PERF_CHUNK="5000"),
        perf_mode="long",
    ),
    # 用例 16：性能脚本 arith 模式保留为轻量正确性测试。
    OSRScenario(
        name="perf_hot_loop_arith_result",
        comment="纯算术 while 循环减少函数调用干扰，只验证结果和 PERF_RESULT 输出。",
        case_name="perf_hot_loop",
        env=_osr_env(OSR_PERF_MODE="arith", OSR_PERF_N="20000", OSR_PERF_CHUNK="5000"),
        perf_mode="arith",
    ),
    # 用例 17：性能脚本 chunks 模式保留为轻量正确性测试。
    OSRScenario(
        name="perf_hot_loop_chunks_result",
        comment="分窗口 while 循环继续打印 PERF_CHUNK，用较小循环次数避免 pytest 变成 benchmark。",
        case_name="perf_hot_loop",
        env=_osr_env(OSR_PERF_MODE="chunks", OSR_PERF_N="20000", OSR_PERF_CHUNK="5000"),
        perf_mode="chunks",
    ),
)


@pytest.mark.parametrize("scenario", SCENARIOS, ids=lambda scenario: scenario.name)
def test_osr_scenarios(scenario):
    output = _run_scenario(scenario)
    _assert_scenario_output(scenario, output)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--osr-case", choices=sorted(CASES))
    args = parser.parse_args()
    CASES[args.osr_case]()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
