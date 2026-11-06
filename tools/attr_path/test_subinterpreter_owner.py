"""A refused subinterpreter import must not change the main interpreter cache."""
import os
import _cinderx as cx
import _xxsubinterpreters as sub
before=cx._attr_path_status();assert before['global_cache']==1
assert cx.is_frame_evaluator_installed()
for _ in range(3):
 mode=os.environ.pop('ATTR_PATH_MODE',None)
 try:
  i=sub.create()
  try:
   try:sub.run_string(i,'import _cinderx')
   except sub.RunFailedError as e:assert 'interpreter' in str(e).lower(),str(e)
   else:raise AssertionError('subinterpreter import unexpectedly accepted')
  finally:sub.destroy(i)
 finally:
  if mode is not None:os.environ['ATTR_PATH_MODE']=mode
 assert cx._attr_path_status()==before
 assert cx.is_frame_evaluator_installed()
 cx.install_frame_evaluator();assert cx._attr_path_status()==before
print('SUBINTERPRETER_OWNER_PASS')