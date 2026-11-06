import sys
import _hook_probe as probe
cx=None
if sys.argv[1]!='stock':
    import _cinderx as cx
    cx.install_frame_evaluator()
    assert cx._interp_getattr_stats311()['inline_entry_ready']
def child(x):return x+1
def parent():
    # The current CinderX frame must respect a subsequently installed hook.
    probe.install()
    try:result=child(2)
    finally:count=probe.restore()
    assert result==3 and count==1,(result,count)
for _ in range(100):parent()
if cx:
    proof=cx._get_trigger_stats()
    assert proof['compiled_function_creations']==proof['machine_code_entries']==0
print('HOOK_REPLACEMENT_PASS',sys.argv[1])
