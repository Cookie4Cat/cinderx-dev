import os
os.environ.update(ATTR_PATH_NOEXC='1',ATTR_PATH_SPECIALIZE='1',ATTR_PATH_GLOBAL_CACHE='1',ATTR_PATH_GLOBAL_ROUTE='compact',ATTR_PATH_INLINE_CALL='1',CINDERX_JIT_MODE='off',CINDERX_JIT_DISABLE='1',PYTHONJITDISABLE='1')
import _cinderx,json,pickle,copy
_cinderx.install_frame_evaluator()
class M:
 def __init__(self):self.x=2;self.registry={'weight':3}
 def __getattr__(self,n):return self.registry[n]
 def f(self,x):return x+self.x+self.weight
m=M();m.__dict__
def f():
 total=0
 for i in range(100000):total+=m.f(i)
 return total
for _ in range(12):f()
x=[{'a':list(range(20))} for _ in range(40)]
for _ in range(200):json.loads(json.dumps(x));pickle.loads(pickle.dumps(x));copy.deepcopy(x)
assert _cinderx._attr_path_status()['global_cache']==1
assert not any(_cinderx._get_trigger_stats().values())
print('ATTR_SUPPORT_PGO_TRAIN_PASS')
