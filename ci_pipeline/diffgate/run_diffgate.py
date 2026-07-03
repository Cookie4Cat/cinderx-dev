#!/usr/bin/env python3
"""diffgate orchestrator: JIT on/off differential gate (M0).

Runs every corpus module under interp / jit / jit_deopt via _bootstrap.py in
subprocesses of the *current* interpreter, diffs per-case output lines against
the interp oracle after applying allowlist normalizers, and writes a JSON
report. Gate passes when the failure set is a subset of the checked-in
baseline (interpreter-as-oracle with an explicitly frozen known-bad list).

Usage (typically inside the target container):
  python3.11 run_diffgate.py --out /out/report.json \
      [--baseline baselines/<target>.json] [--update-baseline PATH] \
      [--modes jit,jit_deopt] [--corpus CORPUS_DIR]

Exit codes: 0 pass, 1 new failures vs baseline, 2 infrastructure error.
"""

import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_CORPUS = os.path.join(HERE, "corpus")
BOOTSTRAP = os.path.join(HERE, "_bootstrap.py")
ALLOWLIST = os.path.join(HERE, "allowlist.toml")


def load_allowlist(path):
    """Parse [[rule]] entries: id, rationale, pattern, replacement."""
    try:
        import tomllib

        with open(path, "rb") as f:
            data = tomllib.load(f)
    except ImportError:  # 3.10 fallback, minimal line parser
        data = {"rule": _parse_toml_rules(path)}
    rules = []
    for rule in data.get("rule", []):
        rules.append(
            (rule["id"], re.compile(rule["pattern"]), rule["replacement"])
        )
    return rules


def _parse_toml_rules(path):
    rules, cur = [], None
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line == "[[rule]]":
                cur = {}
                rules.append(cur)
            elif cur is not None and "=" in line and not line.startswith("#"):
                key, _, val = line.partition("=")
                cur[key.strip()] = json.loads(val.strip())
    return rules


def normalize(line, rules):
    for _rid, pattern, repl in rules:
        line = pattern.sub(repl, line)
    return line


def discover_modules(corpus_dir):
    mods = []
    for name in sorted(os.listdir(corpus_dir)):
        if name.startswith("corpus_") and name.endswith(".py"):
            mods.append(name[:-3])
    return mods


def _run_once(mode, corpus_dir, module, timeout, skip):
    env = dict(os.environ)
    env["PYTHONHASHSEED"] = "0"
    proc = subprocess.run(
        [sys.executable, BOOTSTRAP, mode, corpus_dir, module, str(skip)],
        capture_output=True,
        text=True,
        timeout=timeout,
        env=env,
    )
    cases = {}
    order = []
    for line in proc.stdout.splitlines():
        if line.startswith("CASE "):
            name = line.split(" ", 2)[1]
            cases[name] = line
            order.append(name)
    return cases, order, proc.returncode, proc.stderr[-2000:]


def run_module(mode, corpus_dir, module, timeout, oracle_order=None):
    """Run one module; on a crash (SEGV etc.) attribute it to the next
    pending case and resume after it, so one crash doesn't hide the rest
    of the matrix."""
    all_cases = {}
    skip = 0
    stderr_tail = ""
    returncode = 0
    while True:
        cases, _order, returncode, stderr_tail = _run_once(
            mode, corpus_dir, module, timeout, skip
        )
        all_cases.update(cases)
        if returncode == 0 or oracle_order is None:
            break
        done = skip + len(cases)
        if done >= len(oracle_order):
            break
        crashed = oracle_order[done]
        all_cases[crashed] = "CASE {} CRASH exit={}".format(crashed, returncode)
        skip = done + 1
        if skip >= len(oracle_order):
            break
    return {
        "cases": all_cases,
        "returncode": returncode,
        "stderr_tail": stderr_tail,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", default=DEFAULT_CORPUS)
    ap.add_argument("--out", required=True)
    ap.add_argument("--baseline")
    ap.add_argument("--update-baseline")
    ap.add_argument("--modes", default="jit,jit_deopt")
    ap.add_argument("--timeout", type=int, default=600)
    args = ap.parse_args()

    rules = load_allowlist(ALLOWLIST)
    modes = [m.strip() for m in args.modes.split(",") if m.strip()]
    modules = discover_modules(args.corpus)
    if not modules:
        print("diffgate: no corpus modules found", file=sys.stderr)
        return 2

    failures = []  # sorted "mode:case" ids
    detail = {}
    infra_errors = []
    total_cases = 0

    for module in modules:
        oracle = run_module("interp", args.corpus, module, args.timeout)
        if oracle["returncode"] != 0:
            infra_errors.append(
                {"module": module, "mode": "interp", "stderr": oracle["stderr_tail"]}
            )
            continue
        total_cases += len(oracle["cases"])
        oracle_order = list(oracle["cases"].keys())
        for mode in modes:
            run = run_module(
                mode, args.corpus, module, args.timeout, oracle_order=oracle_order
            )
            if run["returncode"] != 0 and not run["cases"]:
                infra_errors.append(
                    {"module": module, "mode": mode, "stderr": run["stderr_tail"]}
                )
                continue
            for case, oracle_line in oracle["cases"].items():
                fid = "{}:{}".format(mode, case)
                got_line = run["cases"].get(case)
                if got_line is None:
                    failures.append(fid)
                    detail[fid] = {
                        "kind": "missing (crash?)",
                        "interp": oracle_line,
                        "stderr_tail": run["stderr_tail"][-400:],
                    }
                    continue
                if normalize(got_line, rules) != normalize(oracle_line, rules):
                    failures.append(fid)
                    detail[fid] = {
                        "kind": "diff",
                        "interp": oracle_line,
                        mode: got_line,
                    }

    failures.sort()
    baseline = []
    if args.baseline and os.path.exists(args.baseline):
        with open(args.baseline, encoding="utf-8") as f:
            baseline = json.load(f)["failures"]
    new = sorted(set(failures) - set(baseline))
    fixed = sorted(set(baseline) - set(failures))

    report = {
        "python": sys.version,
        "modules": modules,
        "total_cases": total_cases,
        "modes": modes,
        "failures": failures,
        "new_failures": new,
        "fixed_vs_baseline": fixed,
        "infra_errors": infra_errors,
        "detail": detail,
    }
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2, ensure_ascii=False)

    if args.update_baseline:
        with open(args.update_baseline, "w", encoding="utf-8") as f:
            json.dump({"failures": failures}, f, indent=2)

    print(
        "diffgate: {} cases, {} failures ({} new, {} fixed vs baseline), "
        "{} infra errors".format(
            total_cases, len(failures), len(new), len(fixed), len(infra_errors)
        )
    )
    for fid in new[:20]:
        print("  NEW {} :: {}".format(fid, detail.get(fid, {})))
    if infra_errors:
        return 2
    return 1 if new else 0


if __name__ == "__main__":
    sys.exit(main())
