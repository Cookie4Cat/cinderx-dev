"""Compare JSON output against stock; includes dict equality AttributeError."""
import json, os
from pathlib import Path
cases={}
for managed in (True,False):
 for method in (False,True):
  events=[]
  class Collision:
   def __hash__(self):return hash('x')
   def __eq__(self,other):events.append('eq');raise AttributeError('comparison failed')
  class Descriptor:
   def __get__(self,obj,tp):events.append('descriptor');return lambda:'class-method'
  ns={'__getattr__':lambda self,name:(lambda:'fallback:'+name) if method else 'fallback:'+name,'x':Descriptor() if method else 'class-value'}
  if not managed:ns['__slots__']=('__dict__',)
  C=type('C',(),ns);obj=C();obj.__dict__={Collision():0}
  def read(obj):return obj.x() if method else obj.x
  result=read(obj);assert result=='fallback:x',result;assert events==['eq'],events
  cases[f'{managed}-{method}']={'result':result,'events':events[:]}
Path(os.environ['REVIEW_RESULT']).write_text(json.dumps(cases,sort_keys=True))
print('DICT_ATTRIBUTEERROR_PASS',len(cases))