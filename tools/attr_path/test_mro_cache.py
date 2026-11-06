"""Positive qualification cache: invalidation, raw dict writes and lifetime."""
import gc,json,os,weakref
from pathlib import Path
import _cinderx as cx
import _key_probe as probe
checks=[]
def stats(): return cx._attr_mro_stats()
def read(o):return o.x
def warm(o):
 for _ in range(100):read(o)
class Base:
 def __getattr__(self,n):return 'old'
class C(Base):pass
o=C();warm(o)
b=stats();warm(o);a=stats()
assert a['hits']>b['hits'] if a['cache_enabled'] else a['hits']==0
checks.append('positive hits' if a['cache_enabled'] else 'scan control')
C.marker=1;b=stats();assert read(o)=='old';warm(o)
Base.__getattr__=lambda self,n:'new';assert read(o)=='new';warm(o)
C.__getattribute__=lambda self,n:77 if n=='x' else object.__getattribute__(self,n)
assert read(o)==77
del C.__getattribute__;assert read(o)=='new';warm(o)
checks.append('leaf ancestor getter mutation')
events=[];armed=False
class Key:
 def __hash__(self):return hash('__getattribute__')
 def __eq__(self,other):
  if armed and other=='__getattribute__':events.append('eq');raise AttributeError('compare')
  return False
Unsafe=type('Unsafe',(),{Key():None,'__getattr__':lambda self,n:'unsafe'})
C.__bases__=(Unsafe,);events.clear();armed=True
assert read(o)=='unsafe' and events==['eq'],events
armed=False;C.__bases__=(Base,);assert read(o)=='new';warm(o)
checks.append('bases replacement to general namespace')
# Direct dictionary mutation, then clear only the host lookup cache. The
# type version remains valid: the host-cache shortcut must miss and run the original MRO guard.
namespace=gc.get_referents(C.__dict__)[0];assert isinstance(namespace,dict)
v=probe.type_info(C);key=Key();namespace[key]=None
probe.clear_type_cache();assert probe.type_info(C)==v
armed=True;events.clear();assert read(o)=='new' and events==['eq'],events
armed=False;del namespace[key]
checks.append('raw namespace edit with unchanged type version')
D=type('D',(Base,),{});d=D();warm(d)
base_namespace=gc.get_referents(Base.__dict__)[0]
v=probe.type_info(D);base_key=Key();base_namespace[base_key]=None
probe.clear_type_cache();assert probe.type_info(D)==v
armed=True;events.clear();assert read(d)=='new' and events==['eq'],events
armed=False;del base_namespace[base_key]
checks.append('raw ancestor namespace edit after host cache clear')
class Meta(type):
 def mro(cls):return super().mro()
class Custom(metaclass=Meta):
 def __getattr__(self,n):return 9
z=Custom();warm(z);b=stats();warm(z);a=stats()
assert read(z)==9
checks.append('custom MRO parity')
def temporary():
 T=type('Temp',(),{'__getattr__':lambda self,n:1});x=T();warm(x);return weakref.ref(T)
refs=[temporary() for _ in range(300)];gc.collect();assert all(ref() is None for ref in refs)
checks.append('no type retention after cache fill/collisions')
class Oracle:
 def __getattr__(self,n):return 'oracle'
q=Oracle();raw=gc.get_referents(Oracle.__dict__)[0];raw[Key()]=None
observed=[]
for getter in (read,lambda x:getattr(x,'x')):
 armed=False
 for _ in range(100):getattr(q,'x')
 events.clear();armed=True
 value=getter(q);observed.append((value,events[:]))
armed=False
assert observed[0]==observed[1],observed
checks.append('cached general namespace matches builtin getattr')
cx.remove_frame_evaluator();cx.install_frame_evaluator();assert read(o)=='new' 
checks.append('remove reinstall')
assert not any(cx._get_trigger_stats().values())
Path(os.environ['REVIEW_RESULT']).write_text(json.dumps({'checks':checks,'stats':stats()},indent=2))
print('MRO_CACHE_PASS',len(checks))