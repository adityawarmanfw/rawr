"""Follow-up quality gates; timings are host crop timings, not phone estimates."""
import reference as r
import numpy as np,json,time,ctypes
from PIL import Image,ImageDraw
report=json.loads((r.OUT/'results.json').read_text())
lib=ctypes.CDLL(str(r.OUT/'baseline.dylib'))
ptr=np.ctypeslib.ndpointer(dtype=np.uint16,flags='C_CONTIGUOUS');fp=np.ctypeslib.ndpointer(dtype=np.float32,flags='C_CONTIGUOUS')
lib.remosaic.argtypes=[ptr,ctypes.c_int,ctypes.c_int,ctypes.c_int,ctypes.c_int];lib.reduce.argtypes=[ptr,ctypes.c_int,ctypes.c_int,fp,fp]
def baseline(raw,quad=False):
 h,w=raw.shape;a=np.uint16(np.rint(raw*959+64))
 if quad:
  a=np.uint16(np.rint(a.astype(float).reshape(h//2,2,w//2,2).mean((1,3))));lib.remosaic(a,w//2,h//2,2,2);out=(a.astype(np.float32)-64)/959
 else:
  lib.remosaic(a,w,h,4,2);out=np.empty((h//2,w//2),np.float32);lib.reduce(a,w,h,np.ones(4,np.float32),out)
 return r.demosaic(out,'BGGR')
y,x=np.indices((192,192));scenes={}
for name,v in [('edge',(x+.7*y>150).astype(float)),('grating',.5+.4*np.cos(2*np.pi*(.17*x+.09*y)))]:scenes[name]=np.repeat((.1+.7*v)[...,None],3,2)
scenes['colour_edge']=np.where((x+.7*y>150)[...,None],[.8,.15,.1],[.1,.3,.8])
for name,rgb in scenes.items():
 raw=np.take_along_axis(rgb,r.colors(192,192)[...,None],2)[...,0];target=r.lowpass(rgb)[12:-12,12:-12]
 for method,quad in [('existing_high',False),('quad_high',True)]:
  a=baseline(raw,quad)[12:-12,12:-12];report['synthetic'][name][method]={'psnr':float(-10*np.log10(np.mean((a-target)**2))),'max_error':float(abs(a-target).max())}
# Confirm sampling alignment with a smooth in-model achromatic sinusoid.
k=np.load(r.OUT/'kernels_c2.npy');rgb=np.repeat((.4+.1*np.cos(2*np.pi*x/16))[...,None],3,2)
err=float(abs(r.apply(rgb[...,0],k)[12:-12,12:-12]-r.lowpass(rgb)[12:-12,12:-12]).max());assert err<.001,err
report['in_model_sinusoid_max_error']=err
name='RAWR_20261112_165945_230.dng'
with r.rawpy.imread(str(r.Path.home()/'Downloads'/name)) as f:
 a=f.raw_image_visible[6016:6272,8032:8288].astype(float);wb=np.array(f.camera_whitebalance[:3]);wb/=wb[1]
raw=(a-64)/959;images={};sweep=[]
for n,lr,cr,ridge in [(16,6,2,.1),(16,6,2,1),(16,6,2,10),(24,9,3,1),(24,9,3,10),(24,9,2,10)]:
 label=f'n{n}_c{cr}_ridge{ridge}';t=time.perf_counter();k,stats,_=r.design(n,lr,cr,ridge)
 rgb=r.apply(raw,k);images[label]=rgb
 values={'label':label,'max_noise_gain':max(s['max_noise_gain'] for s in stats),'min_rank':min(s['rank'] for s in stats),'unknowns':stats[0]['unknowns'],'design_and_apply_seconds':time.perf_counter()-t,'synthetic':{}}
 for scene,targetrgb in scenes.items():
  mosaic=np.take_along_axis(targetrgb,r.colors(192,192)[...,None],2)[...,0]
  out=r.apply(mosaic,k)[12:-12,12:-12];target=r.lowpass(targetrgb)[12:-12,12:-12]
  values['synthetic'][scene]=float(-10*np.log10(np.mean((out-target)**2)))
 sweep.append(values);print(label,values,flush=True)
sheet=Image.new('RGB',(864,520))
for i,(label,rgb) in enumerate(images.items()):
 im=r.preview(rgb[16:-16,16:-16],wb, min(16.,.35/max(float(np.median(np.load(r.OUT/f'{name}.centre.existing_2.npy')[16:-16,16:-16,1])),.025))).resize((240,240),Image.Resampling.NEAREST)
 sheet.paste(im,((i%3)*288,(i//3)*260+20));ImageDraw.Draw(sheet).text(((i%3)*288,(i//3)*260),label,fill='white')
sheet.save(r.OUT/'regularization_sweep.png');report['sweep']=sweep
(r.OUT/'evaluation.json').write_text(json.dumps(report,indent=2))
print(json.dumps(report['synthetic'],indent=2))
