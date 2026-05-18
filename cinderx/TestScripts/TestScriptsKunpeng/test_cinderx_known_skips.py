from __future__ import annotations

import pytest


COMPILER_SBS_CORPUS_REASON = (
    "compiler SBS corpus bytecode mismatch in current CPython 3.14 baseline; "
    "tracked for compiler parity work"
)
STRICT_LOADER_REASON = (
    "strict loader lazy import cycle mismatch in current CPython 3.14 baseline"
)
OPCODE_STACK_EFFECT_REASON = (
    "CinderX opcode stack_effect expectation mismatch in current CPython 3.14 baseline"
)

KNOWN_SKIPS = {
    # cinderx/PythonLib/test_cinderx/test_compiler/test_corpus.py
    "cinderx/PythonLib/test_cinderx/test_compiler/test_corpus.py::SbsCorpusCompileTests::test_16_for_try_except": COMPILER_SBS_CORPUS_REASON,
    "cinderx/PythonLib/test_cinderx/test_compiler/test_corpus.py::SbsCorpusCompileTests::test_3_14_68_try_star_async_with": COMPILER_SBS_CORPUS_REASON,
    "cinderx/PythonLib/test_cinderx/test_compiler/test_corpus.py::SbsCorpusCompileTests::test_3_14_68_try_star_async_with_2": COMPILER_SBS_CORPUS_REASON,
    "cinderx/PythonLib/test_cinderx/test_compiler/test_corpus.py::SbsCorpusCompileTests::test_42_import_in_try_except": COMPILER_SBS_CORPUS_REASON,
    "cinderx/PythonLib/test_cinderx/test_compiler/test_corpus.py::SbsCorpusCompileTests::test_58_async_for_continue": COMPILER_SBS_CORPUS_REASON,
    "cinderx/PythonLib/test_cinderx/test_compiler/test_corpus.py::SbsCorpusCompileTests::test_58_async_for_in_if": COMPILER_SBS_CORPUS_REASON,
    "cinderx/PythonLib/test_cinderx/test_compiler/test_corpus.py::SbsCorpusCompileTests::test_59_async_with_nested_with": COMPILER_SBS_CORPUS_REASON,
    "cinderx/PythonLib/test_cinderx/test_compiler/test_corpus.py::SbsCorpusCompileTests::test_60_try_except": COMPILER_SBS_CORPUS_REASON,
    "cinderx/PythonLib/test_cinderx/test_compiler/test_corpus.py::SbsCorpusCompileTests::test_60_try_except2": COMPILER_SBS_CORPUS_REASON,
    "cinderx/PythonLib/test_cinderx/test_compiler/test_corpus.py::SbsCorpusCompileTests::test_60_try_finally_cond": COMPILER_SBS_CORPUS_REASON,
    "cinderx/PythonLib/test_cinderx/test_compiler/test_corpus.py::SbsCorpusCompileTests::test_61_try_except_finally": COMPILER_SBS_CORPUS_REASON,
    "cinderx/PythonLib/test_cinderx/test_compiler/test_corpus.py::SbsCorpusCompileTests::test_62_try_except_as": COMPILER_SBS_CORPUS_REASON,
    "cinderx/PythonLib/test_cinderx/test_compiler/test_corpus.py::SbsCorpusCompileTests::test_62_try_except_cond": COMPILER_SBS_CORPUS_REASON,
    "cinderx/PythonLib/test_cinderx/test_compiler/test_corpus.py::SbsCorpusCompileTests::test_62_try_except_else_borrowed": COMPILER_SBS_CORPUS_REASON,
    "cinderx/PythonLib/test_cinderx/test_compiler/test_corpus.py::SbsCorpusCompileTests::test_62_try_except_list_comp_return": COMPILER_SBS_CORPUS_REASON,
    "cinderx/PythonLib/test_cinderx/test_compiler/test_corpus.py::SbsCorpusCompileTests::test_62_try_except_return": COMPILER_SBS_CORPUS_REASON,
    "cinderx/PythonLib/test_cinderx/test_compiler/test_corpus.py::SbsCorpusCompileTests::test_62_try_except_return_nonconst": COMPILER_SBS_CORPUS_REASON,
    "cinderx/PythonLib/test_cinderx/test_compiler/test_corpus.py::SbsCorpusCompileTests::test_62_try_except_small_exit": COMPILER_SBS_CORPUS_REASON,
    "cinderx/PythonLib/test_cinderx/test_compiler/test_corpus.py::SbsCorpusCompileTests::test_62_try_except_try_finally_return": COMPILER_SBS_CORPUS_REASON,
    "cinderx/PythonLib/test_cinderx/test_compiler/test_corpus.py::SbsCorpusCompileTests::test_63_with_unassign_var": COMPILER_SBS_CORPUS_REASON,
    "cinderx/PythonLib/test_cinderx/test_compiler/test_corpus.py::SbsCorpusCompileTests::test_67_try_except_for_loop": COMPILER_SBS_CORPUS_REASON,
    "cinderx/PythonLib/test_cinderx/test_compiler/test_corpus.py::SbsCorpusCompileTests::test_67_try_except_inside_with": COMPILER_SBS_CORPUS_REASON,
    "cinderx/PythonLib/test_cinderx/test_compiler/test_corpus.py::SbsCorpusCompileTests::test_67_with": COMPILER_SBS_CORPUS_REASON,
    "cinderx/PythonLib/test_cinderx/test_compiler/test_corpus.py::SbsCorpusCompileTests::test_67_with_as": COMPILER_SBS_CORPUS_REASON,
    "cinderx/PythonLib/test_cinderx/test_compiler/test_corpus.py::SbsCorpusCompileTests::test_67_with_as_func": COMPILER_SBS_CORPUS_REASON,
    "cinderx/PythonLib/test_cinderx/test_compiler/test_corpus.py::SbsCorpusCompileTests::test_67_with_cond_return": COMPILER_SBS_CORPUS_REASON,
    "cinderx/PythonLib/test_cinderx/test_compiler/test_corpus.py::SbsCorpusCompileTests::test_67_with_inside_try_finally_multiple_terminal_elif": COMPILER_SBS_CORPUS_REASON,
    "cinderx/PythonLib/test_cinderx/test_compiler/test_corpus.py::SbsCorpusCompileTests::test_67_with_inside_try_finally_preceding_terminal_except": COMPILER_SBS_CORPUS_REASON,
    "cinderx/PythonLib/test_cinderx/test_compiler/test_corpus.py::SbsCorpusCompileTests::test_67_with_local": COMPILER_SBS_CORPUS_REASON,
    "cinderx/PythonLib/test_cinderx/test_compiler/test_corpus.py::SbsCorpusCompileTests::test_67_with_multi_exit": COMPILER_SBS_CORPUS_REASON,
    "cinderx/PythonLib/test_cinderx/test_compiler/test_corpus.py::SbsCorpusCompileTests::test_67_with_return": COMPILER_SBS_CORPUS_REASON,
    "cinderx/PythonLib/test_cinderx/test_compiler/test_corpus.py::SbsCorpusCompileTests::test_67_with_try_finally_if": COMPILER_SBS_CORPUS_REASON,
    "cinderx/PythonLib/test_cinderx/test_compiler/test_corpus.py::SbsCorpusCompileTests::test_68_with2": COMPILER_SBS_CORPUS_REASON,
    "cinderx/PythonLib/test_cinderx/test_compiler/test_corpus.py::SbsCorpusCompileTests::test_69_for_try_except_continue1": COMPILER_SBS_CORPUS_REASON,
    "cinderx/PythonLib/test_cinderx/test_compiler/test_corpus.py::SbsCorpusCompileTests::test_69_for_try_except_continue2": COMPILER_SBS_CORPUS_REASON,
    "cinderx/PythonLib/test_cinderx/test_compiler/test_corpus.py::SbsCorpusCompileTests::test_69_for_try_except_continue3": COMPILER_SBS_CORPUS_REASON,
    "cinderx/PythonLib/test_cinderx/test_compiler/test_corpus.py::SbsCorpusCompileTests::test_85_match_if_async": COMPILER_SBS_CORPUS_REASON,
    # cinderx/PythonLib/test_cinderx/test_compiler/test_strict/test_loader.py
    "cinderx/PythonLib/test_cinderx/test_compiler/test_strict/test_loader.py::StrictLoaderTest::test_strict_lazy_import_cycle": STRICT_LOADER_REASON,
    "cinderx/PythonLib/test_cinderx/test_compiler/test_strict/test_loader.py::StrictLoaderTest::test_strict_loader_lazy_imports_cycle": STRICT_LOADER_REASON,
    # cinderx/PythonLib/test_cinderx/test_cpython_overrides/test__opcode.py
    "cinderx/PythonLib/test_cinderx/test_cpython_overrides/test__opcode.py::CinderX_OpcodeTests::test_stack_effect": OPCODE_STACK_EFFECT_REASON,
    "cinderx/PythonLib/test_cinderx/test_cpython_overrides/test__opcode.py::CinderX_OpcodeTests::test_stack_effect_jump": OPCODE_STACK_EFFECT_REASON,
}


def pytest_collection_modifyitems(items: list[pytest.Item]) -> None:
    for item in items:
        reason = KNOWN_SKIPS.get(item.nodeid)
        if reason:
            item.add_marker(pytest.mark.skip(reason=f"Kunpeng known failure: {reason}"))
