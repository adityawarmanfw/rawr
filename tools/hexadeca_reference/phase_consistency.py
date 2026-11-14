"""Diagnostic only: assess whether a global phase calibration is defensible."""
from pathlib import Path
import numpy as np,rawpy,json
with rawpy.imread(str(Path.home()/'Downloads/RAWR_20261112_165945_230.dng')) as f:raw=f.raw_image_visible.copy()
result=[]
for cy,cx in [(2048,6144),(3072,8160),(2048,12288),(4096,10240),(6144,8160)]:
 v=raw[cy-512:cy+512,cx-512:cx+512].astype(float)-64;b=v.reshape(256,4,256,4).transpose(0,2,1,3);m=b.mean((2,3));row={'center':[cy,cx],'groups':{}}
 for name,py,px in [('B',0,0),('G1',0,1),('G2',1,0),('R',1,1)]:
  means=m[py::2,px::2];blocks=b[py::2,px::2];dy,dx=np.gradient(means);valid=(means>40)&((abs(dx)+abs(dy))/np.maximum(means,1)<.02)
  if valid.sum()>100:
   phases=np.median(blocks[valid]/means[valid,None,None],axis=0);row['groups'][name]={'count':int(valid.sum()),'phase':phases.tolist()}
 result.append(row)
print(json.dumps(result,indent=2));Path('tmp/hexadeca-artifacts/phase_consistency.json').write_text(json.dumps(result,indent=2))
