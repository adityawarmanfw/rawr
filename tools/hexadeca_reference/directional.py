"""Bounded directional Singh experiment, with explicit heuristic RAW selector.
Elliptical support follows sec.5.1; selector and parameters are our own.
"""
import augmented as a
import numpy as np,ctypes,json
from scipy.ndimage import gaussian_filter
from PIL import Image,ImageDraw
r=a.r

def blend(raw,candidates,fallback):
 # Compare sites 8 apart: identical colour and subpixel phase. Smooth tensor
 # over a full CFA neighbourhood before estimating gradient orientation.
 dx=(np.roll(raw,-8,1)-np.roll(raw,8,1))/16
 dy=(np.roll(raw,-8,0)-np.roll(raw,8,0))/16
 xx=gaussian_filter(dx*dx,4)[::2,::2];yy=gaussian_filter(dy*dy,4)[::2,::2];xy=gaussian_filter(dx*dy,4)[::2,::2]
 theta=.5*np.arctan2(2*xy,xx-yy)
 coherence=np.sqrt((xx-yy)**2+4*xy*xy)/(xx+yy+1e-7)
 weight=np.exp(4*np.cos(2*(theta[None]-np.arange(4)[:,None,None]*np.pi/4)));weight/=weight.sum(0)
 confidence=np.clip((coherence-.25)/.5,0,1)
 result=np.sum(candidates*weight[...,None],axis=0)
 return confidence[...,None]*result+(1-confidence[...,None])*fallback

def main():
 report={'selector':'same-phase stride-8 RAW derivatives, Gaussian sigma4 structure tensor, soft four-direction weights, coherence fallback','banks':[],'synthetic':{}}
 fallback,_=a.design(lr=4,cr=1);banks=[]
 for aspect in [.7,.5]:
  bank=[];record={'aspect':aspect,'directions':[]}
  for d in range(4):
   k,s=a.design(lr=5,cr=2,angle=d*np.pi/4,aspect=aspect)
   valid=all(v['rank']==v['unknowns'] and v['dc_error']<1e-7 for v in s)
   record['directions'].append({'direction':d,'valid':valid,'max_noise_gain':max(v['noise_gain'] for v in s),'min_rank':min(v['rank'] for v in s),'max_dc_error':max(v['dc_error'] for v in s)})
   assert valid,record
   bank.append(k)
  report['banks'].append(record);banks.append(bank);print('bank',record,flush=True)
 lib=ctypes.CDLL(str(r.OUT/'baseline.dylib'));ptr=np.ctypeslib.ndpointer(dtype=np.uint16,flags='C_CONTIGUOUS');fp=np.ctypeslib.ndpointer(dtype=np.float32,flags='C_CONTIGUOUS')
 lib.remosaic.argtypes=[ptr,ctypes.c_int,ctypes.c_int,ctypes.c_int,ctypes.c_int];lib.reduce.argtypes=[ptr,ctypes.c_int,ctypes.c_int,fp,fp]
 def existing(raw):
  h,w=raw.shape;b=np.uint16(np.rint(raw*959+64));lib.remosaic(b,w,h,4,2);out=np.empty((h//2,w//2),np.float32);lib.reduce(b,w,h,np.ones(4,np.float32),out);return r.demosaic(out,'BGGR')
 y,x=np.indices((128,128));scenes={'flat':np.broadcast_to([.2,.4,.7],(128,128,3)),'edge':np.repeat((.1+.7*(x+.7*y>100))[...,None],3,2),'grating':np.repeat((.45+.28*np.cos(2*np.pi*(.17*x+.09*y)))[...,None],3,2),'colour_edge':np.where((x+.7*y>100)[...,None],[.8,.15,.1],[.1,.3,.8])}
 def score(rgb,target):
  delta=rgb[16:-16,16:-16]-target[16:-16,16:-16];return float(-10*np.log10(max(np.mean(delta**2),1e-30)))
 for scene,rgb in scenes.items():
  raw=np.take_along_axis(rgb,r.colors(128,128)[...,None],2)[...,0];target=r.lowpass(rgb);base=r.apply(raw,fallback)
  result={'existing_high':score(existing(raw),target),'circular':score(base,target)}
  for j,bank in enumerate(banks):
   candidates=np.stack([r.apply(raw,k) for k in bank]);out=blend(raw,candidates,base)
   if scene=='flat':assert abs(out[16:-16,16:-16]-[.2,.4,.7]).max()<1e-7
   result[f'directional_{j}']=score(out,target)
   # Ground-truth selection is a diagnostic only, never used on real images.
   allc=np.concatenate([candidates,base[None]],0);err=np.mean((allc-target)**2,axis=-1);idx=err.argmin(0)
   oracle=np.take_along_axis(allc,idx[None,...,None],axis=0)[0];result[f'oracle_{j}']=score(oracle,target)
  report['synthetic'][scene]=result;print(scene,result,flush=True)
 for name in ['RAWR_20261112_165856_902.dng','RAWR_20261112_165945_230.dng','RAWR_20261112_170047_675.dng']:
  with r.rawpy.imread(str(r.Path.home()/'Downloads'/name)) as f:
   wb=np.array(f.camera_whitebalance[:3]);wb/=wb[1];h,w=f.raw_image_visible.shape
   for label,fy,fx in [('centre',.5,.5),('upper',.25,.5),('right',.5,.75)]:
    cy=int(h*fy)//8*8;cx=int(w*fx)//8*8;raw=(f.raw_image_visible[cy-128:cy+128,cx-128:cx+128].astype(float)-64)/959
    base=r.apply(raw,fallback);images={'existing_high':np.load(r.OUT/f'{name}.{label}.existing_2.npy'),'circular':base}
    for j,bank in enumerate(banks):images[f'directional_{j}']=blend(raw,np.stack([r.apply(raw,k) for k in bank]),base)
    expo=min(16.,.35/max(float(np.median(images['existing_high'][16:-16,16:-16,1])),.025));sheet=Image.new('RGB',(960,260))
    for i,(title,img) in enumerate(images.items()):
     np.save(r.OUT/f'{name}.{label}.{title}.direction.npy',img)
     sheet.paste(r.preview(img[16:-16,16:-16],wb,expo).resize((240,240),Image.Resampling.NEAREST),(i*240,20));ImageDraw.Draw(sheet).text((i*240,0),title,fill='white')
    sheet.save(r.OUT/f'{name}.{label}.directional.png')
  print(name,'done',flush=True)
 (r.OUT/'directional.json').write_text(json.dumps(report,indent=2))
if __name__=='__main__':main()
