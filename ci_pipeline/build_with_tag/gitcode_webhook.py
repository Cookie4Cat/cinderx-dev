#!/home/pybin/bin/python3.14
import argparse
import hashlib
import json
import os
import re
import shlex
import shutil
import socket
import subprocess
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path


ZERO_SHA = "0" * 40


def env(name, default=""):
    return os.environ.get(name, default)


def env_bool(name, default=False):
    value = os.environ.get(name)
    if value is None:
        return default
    return value.lower() in {"1", "true", "yes", "on"}


OWNER = env("GITCODE_OWNER", "openeuler")
REPO = env("GITCODE_REPO", "cinderx")
REPO_URL = env("GITCODE_REPO_URL", f"https://gitcode.com/{OWNER}/{REPO}.git")
API_BASE = env("GITCODE_API_BASE", "https://api.gitcode.com/api/v5").rstrip("/")
ACCESS_TOKEN = env("GITCODE_ACCESS_TOKEN", "")
GIT_USERNAME = env("GITCODE_GIT_USERNAME", "oauth2")
USE_TOKEN_FOR_GIT = env_bool("GITCODE_USE_TOKEN_FOR_GIT", True)

WORK_BASE = Path(env("CINDERX_WEBHOOK_WORK_BASE", "/var/lib/cinderx-webhook"))
REPO_DIR = Path(env("CINDERX_REPO_DIR", str(WORK_BASE / "repo")))
LOG_DIR = WORK_BASE / "logs"
STATE_DIR = WORK_BASE / "state"
RUN_DIR = WORK_BASE / "run"

BUILD_COMMAND = env(
    "CINDERX_BUILD_COMMAND",
    "/home/pybin/bin/python3.14 -m cibuildwheel --output-dir wheelhouse --only cp314-manylinux_aarch64",
)
RELEASE_STATUS = env("GITCODE_RELEASE_STATUS", "latest")
STRICT_RELEASE_CREATE = env_bool("GITCODE_STRICT_RELEASE_CREATE", False)
HTTP_TIMEOUT = int(env("CINDERX_WEBHOOK_HTTP_TIMEOUT", "120"))
RELEASE_TAG_RE = re.compile(
    r"^v(?P<major>0|[1-9]\d*)\.(?P<minor>0|[1-9]\d*)\.(?P<patch>0|[1-9]\d*)"
    r"(?:-(?P<prerelease>alpha|beta|rc)\.(?P<pre_num>0|[1-9]\d*))?$"
)
PRERELEASE_ORDER = {"alpha": 0, "beta": 1, "rc": 2}


def log_line(fp, msg):
    stamp = time.strftime("%Y-%m-%d %H:%M:%S %z")
    line = f"[{stamp}] {msg}"
    print(line, flush=True)
    fp.write(line + "\n")
    fp.flush()


def run_cmd(args, cwd, log, env_overrides=None, timeout=None):
    log_line(log, "$ " + " ".join(shlex.quote(a) for a in args))
    child_env = os.environ.copy()
    if env_overrides:
        child_env.update(env_overrides)
    proc = subprocess.Popen(
        args,
        cwd=str(cwd),
        env=child_env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    try:
        assert proc.stdout is not None
        for line in proc.stdout:
            log.write(line)
            log.flush()
            print(line, end="", flush=True)
        code = proc.wait(timeout=timeout)
    except Exception:
        proc.kill()
        proc.wait()
        raise
    if code != 0:
        raise RuntimeError(f"command failed with exit code {code}: {' '.join(args)}")


def check_output(args, cwd):
    return subprocess.check_output(
        args,
        cwd=str(cwd),
        text=True,
        encoding="utf-8",
        errors="replace",
    ).strip()


def validate_release_tag(tag):
    match = RELEASE_TAG_RE.fullmatch(tag)
    if not match:
        raise RuntimeError(
            "invalid release tag: "
            f"{tag}; expected v<MAJOR>.<MINOR>.<PATCH> or "
            "v<MAJOR>.<MINOR>.<PATCH>-<alpha|beta|rc>.<N>"
        )
    return match


def release_tag_key(tag):
    match = validate_release_tag(tag)
    prerelease = match.group("prerelease")
    pre_num = match.group("pre_num")
    if prerelease:
        release_rank = 0
        pre_key = (PRERELEASE_ORDER[prerelease], int(pre_num))
    else:
        release_rank = 1
        pre_key = (len(PRERELEASE_ORDER), 0)
    return (
        int(match.group("major")),
        int(match.group("minor")),
        int(match.group("patch")),
        release_rank,
        *pre_key,
    )


def make_git_env():
    git_env = {"GIT_TERMINAL_PROMPT": "0"}
    if ACCESS_TOKEN and USE_TOKEN_FOR_GIT:
        RUN_DIR.mkdir(parents=True, exist_ok=True)
        askpass = RUN_DIR / "git-askpass.sh"
        askpass.write_text(
            "#!/usr/bin/env bash\n"
            "case \"$1\" in\n"
            "  *Username*) printf '%s\\n' \"${GITCODE_GIT_USERNAME:-oauth2}\" ;;\n"
            "  *Password*) printf '%s\\n' \"${GITCODE_ACCESS_TOKEN:-}\" ;;\n"
            "  *) printf '%s\\n' \"${GITCODE_ACCESS_TOKEN:-}\" ;;\n"
            "esac\n",
            encoding="utf-8",
        )
        askpass.chmod(0o700)
        git_env.update(
            {
                "GIT_ASKPASS": str(askpass),
                "GITCODE_GIT_USERNAME": GIT_USERNAME,
                "GITCODE_ACCESS_TOKEN": ACCESS_TOKEN,
            }
        )
    return git_env


def ensure_existing_repo(log):
    if not (REPO_DIR / ".git").exists():
        raise RuntimeError(
            f"{REPO_DIR} is not a git repository; clone {REPO_URL} before running this script"
        )
    git_env = make_git_env()
    run_cmd(["git", "remote", "set-url", "origin", REPO_URL], REPO_DIR, log, git_env)
    return git_env


def checkout_tag(tag, sha, log):
    git_env = ensure_existing_repo(log)
    refspec = f"+refs/tags/{tag}:refs/tags/{tag}"
    run_cmd(["git", "fetch", "--force", "--tags", "origin", refspec], REPO_DIR, log, git_env)
    run_cmd(["git", "checkout", "--force", f"refs/tags/{tag}"], REPO_DIR, log, git_env)
    run_cmd(["git", "clean", "-ffdx"], REPO_DIR, log, git_env)
    checked_sha = check_output(["git", "rev-parse", "HEAD"], REPO_DIR)
    log_line(log, f"checked out {tag} at {checked_sha}")
    if sha and sha != ZERO_SHA and checked_sha != sha:
        log_line(log, f"warning: webhook sha {sha} differs from checked out sha {checked_sha}")
    return checked_sha


def api_url(path, params):
    query = dict(params or {})
    if ACCESS_TOKEN:
        query["access_token"] = ACCESS_TOKEN
    return f"{API_BASE}{path}?{urllib.parse.urlencode(query)}"


def request_json(method, path, params=None, body=None, expected=(200, 201)):
    data = None
    headers = {"Accept": "application/json"}
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(api_url(path, params), data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as resp:
            payload = resp.read()
            text = payload.decode("utf-8", "replace") if payload else ""
            parsed = json.loads(text) if text else {}
            if resp.status not in expected:
                raise RuntimeError(f"{method} {path} returned {resp.status}: {text[:1000]}")
            return resp.status, parsed
    except urllib.error.HTTPError as exc:
        text = exc.read().decode("utf-8", "replace")
        raise RuntimeError(f"{method} {path} returned {exc.code}: {text[:1000]}") from exc


def release_exists(tag):
    try:
        request_json("GET", f"/repos/{OWNER}/{REPO}/releases/{urllib.parse.quote(tag, safe='')}")
        return True
    except Exception:
        return False


def previous_release_tag(current_tag, log):
    current_key = release_tag_key(current_tag)
    try:
        tags = check_output(
            ["git", "tag", "--merged", "HEAD"],
            REPO_DIR,
        ).splitlines()
    except subprocess.CalledProcessError as exc:
        log_line(log, f"warning: failed to list previous tags: {exc}")
        return ""
    candidates = []
    for tag in tags:
        tag = tag.strip()
        if not tag or tag == current_tag or not RELEASE_TAG_RE.fullmatch(tag):
            continue
        tag_key = release_tag_key(tag)
        if tag_key < current_key:
            candidates.append((tag_key, tag))
    if not candidates:
        return ""
    return max(candidates)[1]


def git_log_entries(previous_tag, log):
    if not previous_tag:
        output = check_output(
            ["git", "log", "--pretty=format:%h %s", "--no-merges", "-20"],
            REPO_DIR,
        )
        return [line for line in output.splitlines() if line.strip()]

    rev_range = f"{previous_tag}..HEAD"
    try:
        output = check_output(
            ["git", "log", "--pretty=format:%h %s", "--no-merges", rev_range],
            REPO_DIR,
        )
    except subprocess.CalledProcessError as exc:
        log_line(log, f"warning: failed to collect changelog from {rev_range}: {exc}")
        return []
    return [line for line in output.splitlines() if line.strip()]


def categorize_change(line):
    text = line.lower()
    if any(token in text for token in ("特性", "feature", "feat")):
        return "features"
    if any(token in text for token in ("修复", "fix", "bug")):
        return "fixes"
    if any(token in text for token in ("性能", "perf")):
        return "performance"
    return "chores"


def format_section(title, entries):
    lines = [f"## {title}"]
    if entries:
        lines.extend(f"- {entry}" for entry in entries)
    else:
        lines.append("- 暂无")
    return "\n".join(lines)


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_sha256_sidecar(wheel_path, digest):
    sha_path = wheel_path.with_name(f"{wheel_path.name}.sha256")
    sha_path.write_text(f"{digest}  {wheel_path.name}\n", encoding="utf-8")
    return sha_path


def collect_wheel_assets(wheels):
    assets = []
    for wheel in wheels:
        digest = sha256_file(wheel)
        sidecar = write_sha256_sidecar(wheel, digest)
        assets.append(
            {
                "path": wheel,
                "name": wheel.name,
                "sha256": digest,
                "sha256_path": sidecar,
                "sha256_name": sidecar.name,
            }
        )
    return assets


def format_integrity_section(wheel_assets):
    lines = [
        "## 完整性校验",
        "请从本 Release 附件下载 `.whl` 和对应 `.sha256` 文件，安装前先校验：",
        "",
        "```bash",
        "sha256sum -c <wheel-file>.sha256",
        "```",
        "",
        "安装时可使用 pip hash-checking mode，让 pip 在安装过程中再次校验 wheel：",
        "",
        "```bash",
        "python3.14 -m pip install --no-deps --require-hashes -r cinderx-release-requirements.txt",
        "```",
        "",
        "`cinderx-release-requirements.txt` 内容示例：",
        "",
        "```text",
    ]
    if wheel_assets:
        lines.extend(
            f"./{asset['name']} --hash=sha256:{asset['sha256']}"
            for asset in wheel_assets
        )
    else:
        lines.append("./<wheel-file>.whl --hash=sha256:<sha256>")
    lines.extend(
        [
            "```",
            "",
            "Release 附件 SHA256：",
        ]
    )
    if wheel_assets:
        for asset in wheel_assets:
            lines.append(
                f"- `{asset['name']}`: `{asset['sha256']}` "
                f"(sidecar: `{asset['sha256_name']}`)"
            )
    else:
        lines.append("- 暂无")
    return "\n".join(lines)


def build_release_body(tag, sha, log, wheel_assets=None):
    wheel_assets = wheel_assets or []
    match = validate_release_tag(tag)
    previous_tag = previous_release_tag(tag, log)
    entries = git_log_entries(previous_tag, log)
    grouped = {
        "features": [],
        "fixes": [],
        "performance": [],
        "chores": [],
    }
    for entry in entries:
        grouped[categorize_change(entry)].append(entry)

    today = time.strftime("%Y-%m-%d")
    compare_from = previous_tag or "recent commits"
    body = [
        f"# {tag} ({today})",
        "",
        format_section("新功能 (Features)", grouped["features"]),
        "",
        format_section("修复 (Bug Fixes)", grouped["fixes"]),
        "",
        format_section("性能优化 (Performance)", grouped["performance"]),
        "",
        format_section("其他变更 (Chores)", grouped["chores"]),
        "",
        "## 安装与部署",
        "- Python: CPython 3.14",
        "- Wheel: 请在本 Release 附件中下载对应 `.whl`",
        f"- 构建命令: `{BUILD_COMMAND}`",
        "",
        format_integrity_section(wheel_assets),
        "",
        "## 完整变更日志",
        f"Changes since `{compare_from}`:",
    ]
    body.extend(f"- {entry}" for entry in entries)
    if not entries:
        body.append("- 暂无")
    if match.group("prerelease"):
        body.extend(
            [
                "",
                "## 预发布说明",
                "This is a pre-release version.",
                "",
                "## 已知问题",
                "- 暂无",
            ]
        )
    body.extend(
        [
            "",
            "## 构建信息",
            f"- Commit: `{sha}`",
            f"- Builder: `{socket.gethostname()}`",
        ]
    )
    return "\n".join(body) + "\n"


def create_release(tag, sha, log, wheel_assets=None):
    validate_release_tag(tag)
    body = {
        "tag_name": tag,
        "name": f"CinderX {tag}",
        "body": build_release_body(tag, sha, log, wheel_assets),
        "target_commitish": sha,
        "release_status": RELEASE_STATUS,
    }
    path = f"/repos/{OWNER}/{REPO}/releases"
    try:
        status, _ = request_json("POST", path, body=body)
        log_line(log, f"created release {tag}, status={status}")
    except Exception as exc:
        log_line(log, f"release create failed: {exc}")
        if STRICT_RELEASE_CREATE or not release_exists(tag):
            raise
        log_line(log, f"release {tag} already exists or is readable; continuing to upload assets")


def upload_asset(tag, file_path, log):
    name = file_path.name
    quoted_tag = urllib.parse.quote(tag, safe="")
    status, upload_info = request_json(
        "GET",
        f"/repos/{OWNER}/{REPO}/releases/{quoted_tag}/upload_url",
        params={"file_name": name},
    )
    log_line(log, f"got upload_url for {name}, status={status}")
    upload_url = upload_info["url"]
    headers = upload_info.get("headers") or {}
    data = file_path.read_bytes()
    req = urllib.request.Request(upload_url, data=data, headers=headers, method="PUT")
    try:
        with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as resp:
            text = resp.read().decode("utf-8", "replace")
            if resp.status < 200 or resp.status >= 300:
                raise RuntimeError(f"PUT returned {resp.status}: {text[:1000]}")
            log_line(log, f"uploaded {name}, status={resp.status}, bytes={len(data)}")
    except urllib.error.HTTPError as exc:
        text = exc.read().decode("utf-8", "replace")
        raise RuntimeError(f"PUT upload for {name} returned {exc.code}: {text[:1000]}") from exc


def build_and_publish(tag, sha):
    validate_release_tag(tag)
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    RUN_DIR.mkdir(parents=True, exist_ok=True)
    safe_tag = tag.replace("/", "_")
    log_path = LOG_DIR / f"{safe_tag}-{int(time.time())}.log"
    success_marker = STATE_DIR / f"{safe_tag}.success"
    running_marker = STATE_DIR / f"{safe_tag}.running"
    with log_path.open("a", encoding="utf-8") as log:
        log_line(log, f"job start tag={tag} sha={sha}")
        if not ACCESS_TOKEN:
            raise RuntimeError("GITCODE_ACCESS_TOKEN is empty; fill /etc/cinderx-webhook.env before publishing")
        if success_marker.exists():
            log_line(log, f"success marker exists for {tag}; skipping duplicate build")
            return str(log_path)
        running_marker.write_text(str(os.getpid()), encoding="utf-8")
        try:
            checked_sha = checkout_tag(tag, sha, log)
            wheelhouse = REPO_DIR / "wheelhouse"
            if wheelhouse.exists():
                shutil.rmtree(wheelhouse)
            run_cmd(shlex.split(BUILD_COMMAND), REPO_DIR, log)
            wheels = sorted(wheelhouse.glob("*.whl"))
            if not wheels:
                raise RuntimeError(f"no wheels found in {wheelhouse}")
            log_line(log, "built wheels: " + ", ".join(w.name for w in wheels))
            wheel_assets = collect_wheel_assets(wheels)
            for asset in wheel_assets:
                log_line(log, f"sha256 {asset['name']} {asset['sha256']}")
            create_release(tag, checked_sha, log, wheel_assets)
            for asset in wheel_assets:
                upload_asset(tag, asset["path"], log)
                upload_asset(tag, asset["sha256_path"], log)
            success_marker.write_text(time.strftime("%Y-%m-%d %H:%M:%S %z"), encoding="utf-8")
            log_line(log, f"job success tag={tag}")
            return str(log_path)
        finally:
            running_marker.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description="Build CinderX wheels for a GitCode tag and publish a release.")
    parser.add_argument("--build-tag", required=True, help="tag name to build and publish")
    parser.add_argument("--sha", default="", help="expected commit sha from GitCode webhook")
    parser.add_argument("--repo-dir", help="existing local cinderx git repository")
    args = parser.parse_args()

    global REPO_DIR
    if args.repo_dir:
        REPO_DIR = Path(args.repo_dir)

    build_and_publish(args.build_tag, args.sha)


if __name__ == "__main__":
    main()
