"""Full native-DNG host comparison; compile high_benchmark.cpp first (README)."""
import ctypes,numpy as np,rawpy,time,json
from pathlib import Path
ptr=np.ctypeslib.ndpointer(dtype=np.uint16,flags='C_CONTIGUOUS');fp=np.ctypeslib.ndpointer(dtype=np.float32,flags='C_CONTIGUOUS')
lib=ctypes.CDLL('tmp/high-remosaic/tiled.dylib');lib.high_remosaic.argtypes=[ptr]+[ctypes.c_uint]*6
tiled=ctypes.CDLL('tmp/high-remosaic/tiled.dylib')
for method in ['high_reduce','reduce_only']:getattr(tiled,method).argtypes=[ptr]+[ctypes.c_int]*4+[fp,fp]
with rawpy.imread(str(Path.home()/'Downloads/RAWR_20261112_165945_230.dng')) as r:
 source=r.raw_image_visible.copy();wb=np.array(r.camera_whitebalance[:3]);wb/=wb[1]
g=np.array([wb[2],1,1,wb[0]],np.float32);g/=g.max();h,w=source.shape
full=source.copy();t=time.perf_counter();lib.high_remosaic(full,w,h,4,3,8,2);remosaic=time.perf_counter()-t
ref=np.empty((h//2,w//2),np.float32);t=time.perf_counter();tiled.reduce_only(full,w,h,256,8,g,ref);reduce=time.perf_counter()-t;del full
print('reference',remosaic,reduce,flush=True);results={'host_reference_seconds':{'remosaic':remosaic,'reduce':reduce},'tiled':[]}
for side in [128,256,512]:
 out=np.empty_like(ref);t=time.perf_counter();tiled.high_reduce(source,w,h,side,8,g,out);seconds=time.perf_counter()-t
 diff=np.abs(out-ref);value={'side':side,'seconds':seconds,'max_difference':float(diff.max()),'different_values':int(np.count_nonzero(diff))};print(value,flush=True);results['tiled'].append(value);assert np.array_equal(out,ref),value
Path('tmp/high-remosaic/full_reduce.json').write_text(json.dumps(results,indent=2))
