"""Patch-only changes must regenerate outputs on cmake --build (no configure)."""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

p = argparse.ArgumentParser()
p.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[2])
p.add_argument('--old-apply', type=Path)
a = p.parse_args()

def run(*cmd):
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)

def digest(build):
    return {name: hashlib.sha256((build / 'ci311-attr' / name).read_bytes()).hexdigest()
            for name in ('ceval.c', 'specialize.c')}

with tempfile.TemporaryDirectory(prefix='attr-patch-deps-') as tmp:
    root = Path(tmp)
    src = root / 'project'
    target = src / 'cinderx/Interpreter/3.11'
    target.mkdir(parents=True)
    upstream = a.repo / 'cinderx/Interpreter/3.11'
    (target / 'upstream').mkdir()
    for name in ('ceval.c', 'specialize.c'):
        shutil.copy2(upstream / 'upstream' / name, target / 'upstream' / name)
    shutil.copytree(upstream / 'attr-patches', target / 'attr-patches')
    if a.old_apply:
        shutil.copy2(a.old_apply, target / 'attr-patches/apply.cmake')
    (src / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.18)
project(attr_patch_dependencies NONE)
set(PROJECT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cinderx")
include("${PROJECT_SOURCE_DIR}/Interpreter/3.11/attr-patches/apply.cmake")
add_custom_target(patched ALL DEPENDS
 "${CMAKE_CURRENT_BINARY_DIR}/ci311-attr/ceval.c"
 "${CMAKE_CURRENT_BINARY_DIR}/ci311-attr/specialize.c")
''')
    build = root / 'incremental'
    run('cmake', '-G', 'Unix Makefiles', '-S', str(src), '-B', str(build))
    run('cmake', '--build', str(build))
    count = 0
    for patch in sorted((target / 'attr-patches').glob('*.patch')):
        before = digest(build)
        # Account for generators/filesystems with whole-second timestamps.
        time.sleep(1.1)
        lines = patch.read_text().splitlines(keepends=True)
        for i, line in enumerate(lines):
            text = line[1:].strip()
            if line.startswith('+ ') and not text.startswith(('/*', '*')):
                lines[i] = line.rstrip('\n') + ' /* dependency-test */\n'
                break
        else:
            raise AssertionError('no safe added line: ' + patch.name)
        patch.write_text(''.join(lines))
        # Deliberately do not invoke cmake -S/-B before the incremental build.
        run('cmake', '--build', str(build))
        after = digest(build)
        assert after != before, 'patch edit was ignored: ' + patch.name
        clean = root / ('clean-' + patch.stem)
        run('cmake', '-G', 'Unix Makefiles', '-S', str(src), '-B', str(clean))
        assert digest(clean) == after, 'incremental and clean outputs differ'
        count += 1
    print('PATCH_DEPENDENCY_PASS', count)