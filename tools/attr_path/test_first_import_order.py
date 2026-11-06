"""Run each main mode in a fresh process, before native CinderX is imported."""
import json
import os
import sys
import _xxsubinterpreters as sub

assert '_cinderx' not in sys.modules
requested = sys.argv[1]
assert requested in ('execute', 'cinderx_control')
for key in list(os.environ):
    if key.startswith(('ATTR_PATH_', 'CINDERX_', 'PYTHONJIT', 'AUTO_JIT')):
        os.environ.pop(key)
# Create the child before setting its attempted native-import configuration.
child = sub.create()
os.environ.update(ATTR_PATH_MODE='cinderx', ATTR_PATH_DIAGNOSTICS='1')
try:
    try:
        sub.run_string(child, 'import _cinderx')
    except sub.RunFailedError as error:
        assert 'subinterpreter' in str(error).lower(), str(error)
    else:
        raise AssertionError('child native import unexpectedly succeeded')
finally:
    sub.destroy(child)
for key in list(os.environ):
    if key.startswith(('ATTR_PATH_', 'CINDERX_', 'PYTHONJIT', 'AUTO_JIT')):
        os.environ.pop(key)
if requested == 'execute':
    os.environ.update(CINDERX_JIT_MODE='execute', PYTHONJITAUTO='0')
else:
    os.environ['ATTR_PATH_MODE'] = 'cinderx_control'
import _cinderx as cx
cx.install_frame_evaluator()
mode = cx._get_observe_stats()['mode']
status = cx._attr_path_status()
print('OBSERVED', json.dumps({'mode': mode, 'attribute': status}), flush=True)
assert mode == ('execute' if requested == 'execute' else 'off'), mode
assert status['no_exception'] == status['diagnostics'] == status['specialization'] == 0, status
if requested == 'execute':
    import cinderjit
    def add_one(x):
        return x + 1
    cinderjit.force_compile(add_one)
    assert add_one(6) == 7
    stats = cx._get_trigger_stats()
    assert stats['compiled_function_creations'] > 0 and stats['machine_code_entries'] > 0, stats
else:
    assert not any(cx._get_trigger_stats().values())
print('FIRST_IMPORT_ORDER_PASS', requested)