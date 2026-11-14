"""Inspect input phase structure in sky crops; NEVER apply inferred correction."""
from pathlib import Path
import numpy as np,rawpy,json
out=[]
for name in ['RAWR_20261112_165856_902.dng','RAWR_20261112_165945_230.dng','RAWR_20261112_170047_675.dng']:
 with rawpy.imread(str(Path.home()/'Downloads'/name)) as r:
  h,w=r.raw_image_visible.shape;cy=(h//4)//8*8;cx=(w//2)//8*8
  v=r.raw_image_visible[cy-512:cy+512,cx-512:cx+512].astype(float)-64
 b=v.reshape(256,4,256,4).transpose(0,2,1,3);means=b.mean((2,3));rel=b/np.maximum(means[...,None,None],1)
 row={'file':name,'crop_origin':[cy-512,cx-512],'groups':{}}
 for label,py,px in [('B',0,0),('G1',0,1),('G2',1,0),('R',1,1)]:
  m=means[py::2,px::2];a=rel[py::2,px::2];dx=np.abs(np.gradient(m,axis=1));dy=np.abs(np.gradient(m,axis=0));valid=(m>20)&((dx+dy)/np.maximum(m,1)<.02)
  if valid.sum()>32:
   phase=np.median(a[valid],axis=0);row['groups'][label]={'selected_blocks':int(valid.sum()),'median_relative_phase':phase.tolist(),'peak_to_peak_fraction':float(np.ptp(phase))}
 out.append(row)
Path('tmp/high-remosaic/phase_diagnostic.json').write_text(json.dumps(out,indent=2))
for r in out:print(r['file'],{k:round(v['peak_to_peak_fraction']*100,2) for k,v in r['groups'].items()})
