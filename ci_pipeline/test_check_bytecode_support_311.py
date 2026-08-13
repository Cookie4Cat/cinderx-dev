"""Self-test for the CPython 3.11 bytecode support list checker.

Runs the checker against the committed list (which must be consistent) and
then against targeted mutations of it, each of which must be rejected.  A
checker that cannot see these faults would wave a stale or corrupted list
through the gate, so this file runs before the checker in CI.
"""

import sys
from pathlib import Path

import pytest

if sys.version_info[:3] != (3, 11, 6):
    pytest.skip(
        "the 3.11 support list is validated under the anchored CPython "
        "3.11.6 only",
        allow_module_level=True,
    )

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_bytecode_support_311 as checker


@pytest.fixture(scope="module")
def truth():
    return checker.ground_truth(checker.REPO_ROOT)


@pytest.fixture(scope="module")
def committed_rows():
    return checker.load_rows(checker.REPO_ROOT / checker.SUPPORT_LIST)


def mutate(rows):
    return {name: dict(row) for name, row in rows.items()}


def test_committed_list_is_consistent(committed_rows, truth):
    assert checker.validate(committed_rows, truth) == []


def test_missing_row_is_detected(committed_rows, truth):
    rows = mutate(committed_rows)
    del rows["LOAD_FAST"]
    errors = checker.validate(rows, truth)
    assert any("missing row" in e and "LOAD_FAST" in e for e in errors)


def test_unknown_opcode_row_is_detected(committed_rows, truth):
    rows = mutate(committed_rows)
    rows["NOT_A_REAL_OPCODE"] = {"num": 1, "state": "translate", "cache": 0}
    errors = checker.validate(rows, truth)
    assert any("unknown opcode NOT_A_REAL_OPCODE" in e for e in errors)


def test_wrong_cache_width_is_detected(committed_rows, truth):
    rows = mutate(committed_rows)
    rows["LOAD_ATTR"]["cache"] += 1
    errors = checker.validate(rows, truth)
    assert any("LOAD_ATTR" in e and "cache" in e for e in errors)


def test_wrong_number_is_detected(committed_rows, truth):
    rows = mutate(committed_rows)
    rows["LOAD_FAST"]["num"] += 1
    errors = checker.validate(rows, truth)
    assert any("LOAD_FAST" in e and "num" in e for e in errors)


def test_unregistered_state_is_detected(committed_rows, truth):
    rows = mutate(committed_rows)
    rows["LOAD_FAST"]["state"] = "maybe"
    errors = checker.validate(rows, truth)
    assert any("LOAD_FAST" in e and "'maybe'" in e for e in errors)


def test_normalization_must_follow_the_interpreter(committed_rows, truth):
    rows = mutate(committed_rows)
    rows["LOAD_ATTR_INSTANCE_VALUE"]["to"] = "LOAD_GLOBAL"
    errors = checker.validate(rows, truth)
    assert any(
        "LOAD_ATTR_INSTANCE_VALUE" in e and "specialization family" in e
        for e in errors
    )


def test_normalization_target_must_be_translated(committed_rows, truth):
    rows = mutate(committed_rows)
    rows["LOAD_ATTR"]["state"] = "refuse"
    rows["LOAD_ATTR"]["reason"] = "REFUSE_UNPORTED"
    errors = checker.validate(rows, truth)
    assert any("instead of 'translate'" in e for e in errors)


def test_refusal_requires_registered_reason(committed_rows, truth):
    rows = mutate(committed_rows)
    del rows["MATCH_CLASS"]["reason"]
    errors = checker.validate(rows, truth)
    assert any("MATCH_CLASS" in e and "reason registered for" in e for e in errors)


def test_variant_flipped_to_translate_is_detected(committed_rows, truth):
    # A one-directional check would let a specialization variant dodge the
    # normalize cross-check entirely by claiming state "translate".
    rows = mutate(committed_rows)
    rows["LOAD_ATTR_INSTANCE_VALUE"]["state"] = "translate"
    del rows["LOAD_ATTR_INSTANCE_VALUE"]["to"]
    errors = checker.validate(rows, truth)
    assert any(
        "LOAD_ATTR_INSTANCE_VALUE" in e and "must be 'normalize'" in e
        for e in errors
    )


def test_meta_anchor_is_validated():
    doc = checker.load_doc(checker.REPO_ROOT / checker.SUPPORT_LIST)
    assert checker.validate_meta(doc) == []

    drifted = {"meta": dict(doc["meta"]), "opcodes": doc["opcodes"]}
    drifted["meta"]["python"] = "3.11.5"
    assert any("anchored" in e for e in checker.validate_meta(drifted))

    shrunk = {"meta": dict(doc["meta"]), "opcodes": doc["opcodes"]}
    shrunk["meta"]["slots"] = 1
    assert any("slots" in e for e in checker.validate_meta(shrunk))


def test_cross_state_reason_is_detected(committed_rows, truth):
    # A refusal reason and an interpreter-only reason are not interchangeable;
    # both swap directions must be rejected.
    rows = mutate(committed_rows)
    rows["MATCH_CLASS"]["reason"] = "INTERP_ONLY_PSEUDO_SLOT"
    errors = checker.validate(rows, truth)
    assert any("MATCH_CLASS" in e and "registered for that state" in e for e in errors)

    rows = mutate(committed_rows)
    rows["CACHE"]["reason"] = "REFUSE_UNPORTED"
    errors = checker.validate(rows, truth)
    assert any("CACHE" in e and "registered for that state" in e for e in errors)


def test_numeric_impostors_are_detected(committed_rows, truth):
    # TOML floats and booleans satisfy Python == against ints; the schema
    # requires exact ints for num and cache.
    rows = mutate(committed_rows)
    rows["POP_TOP"]["num"] = True
    errors = checker.validate(rows, truth)
    assert any("POP_TOP" in e and "num must be an int" in e for e in errors)

    rows = mutate(committed_rows)
    rows["POP_TOP"]["cache"] = False
    errors = checker.validate(rows, truth)
    assert any("POP_TOP" in e and "cache must be an int" in e for e in errors)

    doc = checker.load_doc(checker.REPO_ROOT / checker.SUPPORT_LIST)
    floaty = {"meta": dict(doc["meta"]), "opcodes": doc["opcodes"]}
    floaty["meta"]["slots"] = 256.0
    assert any("int 256" in e for e in checker.validate_meta(floaty))


# ---------------------------------------------------------------------------
# CLI-path mutations: the tests above prove validate() sees faults; these
# prove the public entry point (main / the gate command) actually consults
# it.  A neutered aggregation in main() must fail here with exit code 1.

import re
import shutil
import subprocess


def _make_repo_copy(tmp_path, mutate_toml=None):
    repo = tmp_path / "repo"
    for rel in (checker.SUPPORT_LIST, checker.OPCODE_TARGETS):
        src = checker.REPO_ROOT / rel
        dst = repo / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(src, dst)
    if mutate_toml is not None:
        toml_path = repo / checker.SUPPORT_LIST
        text = toml_path.read_text()
        new_text, count = mutate_toml(text)
        assert count == 1, "mutation did not apply exactly once"
        toml_path.write_text(new_text)
    return repo


def _run_cli(repo):
    return subprocess.run(
        [
            sys.executable,
            str(checker.REPO_ROOT / "ci_pipeline" / "check_bytecode_support_311.py"),
            "--repo",
            str(repo),
        ],
        capture_output=True,
        text=True,
        timeout=60,
    )


def test_cli_accepts_the_pristine_list(tmp_path):
    # Positive control: the end-to-end command exits 0 on an unmutated copy,
    # so the red assertions below cannot pass merely because the CLI is
    # unconditionally broken.
    proc = _run_cli(_make_repo_copy(tmp_path))
    assert proc.returncode == 0, proc.stderr[-400:]
    assert "opcodes consistent" in proc.stdout


def test_cli_rejects_an_opcode_mutation(tmp_path):
    repo = _make_repo_copy(
        tmp_path,
        mutate_toml=lambda text: re.subn(
            r'LOAD_ATTR = \{ num = 106, state = "translate", cache = 4 \}',
            'LOAD_ATTR = { num = 106, state = "translate", cache = 9 }',
            text,
        ),
    )
    proc = _run_cli(repo)
    assert proc.returncode == 1, (proc.returncode, proc.stderr[-400:])
    assert "LOAD_ATTR" in proc.stderr and "cache" in proc.stderr


def test_cli_rejects_a_meta_mutation(tmp_path):
    repo = _make_repo_copy(
        tmp_path,
        mutate_toml=lambda text: re.subn(
            r'python = "3\.11\.6"', 'python = "3.11.5"', text
        ),
    )
    proc = _run_cli(repo)
    assert proc.returncode == 1, (proc.returncode, proc.stderr[-400:])
    assert "anchored" in proc.stderr


def test_reason_is_refusal_only(committed_rows, truth):
    rows = mutate(committed_rows)
    rows["LOAD_FAST"]["reason"] = "REFUSE_UNPORTED"
    errors = checker.validate(rows, truth)
    assert any("LOAD_FAST" in e and "refusal rows" in e for e in errors)
