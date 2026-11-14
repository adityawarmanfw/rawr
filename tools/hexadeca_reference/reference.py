"""Experimental local spectral reconstruction, inspired by Singh 2011 sec. 4.1.
Not the authors' implementation. Explicit BGGR 4x4 blocks; no auto-detection.
Run from repo root using uv run --with numpy --with scipy --with rawpy --with pillow.
"""
from pathlib import Path
import ctypes,json,time,sys
import numpy as np
from scipy.ndimage import correlate
from PIL import Image,ImageDraw
import rawpy
ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'tmp/hexadeca-research'; OUT.mkdir(parents=True,exist_ok=True)
sys.path.insert(0,str(ROOT/'native/raw_demosaic/rcd/tools'))
from rcd_reference import demosaic
PAT=np.array([[2,1],[1,0]])
# RGB = Y + two orthogonal chromatic coordinates. Equal-channel Y is an
# explicit experimental assumption, not the paper's canonical colour basis.
BASIS=np.array([[1,1,1],[1,0,-1],[1,-2,1]],float).T

def colors(h,w,block=4,oy=0,ox=0):
 y,x=np.indices((h,w)); return PAT[((y+oy)//block)%2,((x+ox)//block)%2]

def design(n=16,lr=6,cr=2,ridge=.01):
 """Fit bandlimited Y/chroma by ridge least squares; compose target 4-tap LP.
 Output centre (0.5,0.5). Input support [-7,8] for n=16. Generate 16 even phases.
 """
 start=1-n//2
 y,x=np.mgrid[start:start+n,start:start+n]; coords=np.stack([y.ravel(),x.ravel()],1)
 freqs=[]; comps=[]
 for c,r in enumerate([lr,cr,cr]):
  for fy in range(-r,r+1):
   for fx in range(-r,r+1):
    if fy*fy+fx*fx<=r*r: freqs.append((fy/n,fx/n)); comps.append(c)
 freqs=np.array(freqs); comps=np.array(comps)
 E=np.exp(2j*np.pi*(coords@freqs.T))
 taps=np.array([1,3,3,1])/8; positions=np.arange(-1,3)
 response=np.prod(np.array([np.sum(taps[:,None]*np.exp(2j*np.pi*positions[:,None]*freqs[:,d]),axis=0) for d in range(2)]),axis=0)
 target=BASIS[:,comps]*response
 kernels=np.empty((4,4,3,n,n)); stats=[]
 for py in range(4):
  for px in range(4):
   c=PAT[((y+py*2)//4)%2,((x+px*2)//4)%2].ravel()
   A=E*BASIS[c[:,None],comps[None,:]]
   u,s,vh=np.linalg.svd(A,full_matrices=False)
   inv=(vh.conj().T*(s/(s*s+ridge)))@u.conj().T
   K=(target@inv).real
   # Exact flat-colour reproduction, avoiding ridge-induced colour bias.
   masks=np.eye(3)[c]
   K+=(np.eye(3)-K@masks)@np.linalg.pinv(masks)
   kernels[py,px]=K.reshape(3,n,n)
   stats.append({'phase':[py*2,px*2],'rank':int(np.sum(s>s[0]*1e-8)), 'unknowns':len(s),'condition':float(s[0]/s[-1]),'max_noise_gain':float(np.linalg.norm(K,axis=1).max()),'dc_error':float(abs(K@masks-np.eye(3)).max())})
 return kernels,stats,start

def apply(raw,kernels):
 # Correlation origin=0 on an even kernel addresses [-n/2,n/2-1].
 # Shift input by one sample to get design support [1-n/2,n/2].
 h,w=raw.shape; rgb=np.empty((h//2,w//2,3))
 for py in range(4):
  for px in range(4):
   for c in range(3):
    a=correlate(raw,kernels[py,px,c],mode='mirror',origin=-1)
    rgb[py::4,px::4,c]=a[py*2::8,px*2::8]
 return rgb

def lowpass(rgb):
 taps=np.array([1,3,3,1])/8
 from scipy.ndimage import correlate1d
 return correlate1d(correlate1d(rgb,taps,axis=0,origin=-1,mode='mirror'),taps,axis=1,origin=-1,mode='mirror')[::2,::2]

def preview(rgb,gains,exposure):
 # Shared WB and sRGB transfer only, NO camera colour matrix/tone/sharpening.
 a=np.clip(rgb*np.array(gains)*exposure,0,1)
 a=np.where(a<=.0031308,12.92*a,1.055*a**(1/2.4)-.055)
 return Image.fromarray(np.uint8(np.clip(a*255,0,255)))

def main():
 lib=ctypes.CDLL(str(OUT/'baseline.dylib'))
 ptr=np.ctypeslib.ndpointer(dtype=np.uint16,flags='C_CONTIGUOUS')
 fp=np.ctypeslib.ndpointer(dtype=np.float32,flags='C_CONTIGUOUS')
 lib.remosaic.argtypes=[ptr,ctypes.c_int,ctypes.c_int,ctypes.c_int,ctypes.c_int]
 lib.reduce.argtypes=[ptr,ctypes.c_int,ctypes.c_int,fp,fp]
 report={'method':'experimental 2011-inspired local spectral fit, not 2024 reproduction','variants':{},'files':[]}
 designs={}
 for cr in [1,2,3]:
  t=time.perf_counter(); k,stats,start=design(cr=cr)
  designs[cr]=k; np.save(OUT/f'kernels_c{cr}.npy',k)
  report['variants'][str(cr)]={'design_seconds':time.perf_counter()-t,'phases':stats}
  # Exact DC and phase alignment sanity on colour fields; exclude boundaries.
  rgb=np.broadcast_to([.2,.4,.7],(96,96,3)); c=colors(96,96)
  raw=np.take_along_axis(rgb,c[...,None],2)[...,0]
  error=float(abs(apply(raw,k)[12:-12,12:-12]-[.2,.4,.7]).max())
  assert error<1e-8,error
  report['variants'][str(cr)]['flat_rgb_error']=error
 # Synthetic tests share exact downsample target and crop exclusion.
 y,x=np.indices((192,192)); scenes={}
 for name,v in [('edge',(x+.7*y>150).astype(float)),('grating',.5+.4*np.cos(2*np.pi*(.17*x+.09*y)))]:
  scenes[name]=np.repeat((.1+.7*v)[...,None],3,2)
 scenes['colour_edge']=np.where((x+.7*y>150)[...,None],[.8,.15,.1],[.1,.3,.8])
 report['synthetic']={}
 for name,rgb in scenes.items():
  c=colors(*rgb.shape[:2]); raw=np.take_along_axis(rgb,c[...,None],2)[...,0]
  target=lowpass(rgb)[12:-12,12:-12]
  report['synthetic'][name]={}
  for cr,k in designs.items():
   a=apply(raw,k)[12:-12,12:-12]; mse=float(np.mean((a-target)**2))
   report['synthetic'][name][str(cr)]={'psnr':float(-10*np.log10(max(mse,1e-30))), 'max_error':float(abs(a-target).max())}
 names=['RAWR_20261112_165856_902.dng','RAWR_20261112_165945_230.dng','RAWR_20261112_170047_675.dng']
 for name in names:
  t=time.perf_counter()
  with rawpy.imread(str(Path.home()/'Downloads'/name)) as r:
   raw=r.raw_image_visible.copy(); wb=np.array(r.camera_whitebalance[:3]); wb/=wb[1]
   entry={'file':name,'shape':list(raw.shape),'decode_seconds':time.perf_counter()-t,'wb_rgb':wb.tolist(),'black':r.black_level_per_channel,'white':r.white_level,'crops':[]}
  h,w=raw.shape
  # Whole-image coarse block-mean preview for locating scene content.
  blocks=raw.reshape(h//4,4,w//4,4).mean((1,3))
  small=np.stack([blocks[1::2,1::2],(blocks[::2,1::2]+blocks[1::2,::2])/2,blocks[::2,::2]],2)
  preview((small-64)/959,wb,1).resize((816,614)).save(OUT/(name+'.overview.png'))
  for label,fy,fx in [('centre',.5,.5),('upper',.25,.5),('right',.5,.75)]:
   yy=(int(h*fy)//8)*8; xx=(int(w*fx)//8)*8
   a=raw[yy-128:yy+128,xx-128:xx+128].copy(); linear=(a.astype(float)-64)/959
   phase=[[float(a[j::8,i::8].mean()) for i in range(8)] for j in range(8)]
   crop={'name':label,'origin':[yy-128,xx-128],'phase_means':phase,'timings':{}}
   images={}
   gains=np.array([wb[2],1,1,wb[0]],np.float32); gains/=gains.max()
   for quality in [1,2]:
    b=a.copy(); t=time.perf_counter();lib.remosaic(b,256,256,4,quality);crop['timings'][f'remosaic_{quality}']=time.perf_counter()-t
    out=np.empty((128,128),np.float32);t=time.perf_counter();lib.reduce(b,256,256,gains,out);crop['timings'][f'natural_bin_{quality}']=time.perf_counter()-t
    images[f'existing_{quality}']=demosaic(out,'BGGR')
   b=np.uint16(np.rint(a.astype(float).reshape(128,2,128,2).mean((1,3))))
   t=time.perf_counter();lib.remosaic(b,128,128,2,2);crop['timings']['quad_high']=time.perf_counter()-t
   images['quad_first']=demosaic((b.astype(np.float32)-64)/959,'BGGR')
   for cr,k in designs.items():
    t=time.perf_counter();rgb=apply(linear,k);crop['timings'][f'spectral_c{cr}_python']=time.perf_counter()-t
    images[f'spectral_c{cr}']=rgb
   expo= min(16.,.35/max(float(np.median(images['existing_2'][16:-16,16:-16,1])),.025))
   sheet=Image.new('RGB',(3*288,2*260))
   for i,(method,rgb) in enumerate(images.items()):
    np.save(OUT/f'{name}.{label}.{method}.npy',rgb)
    im=preview(rgb[16:-16,16:-16],wb,expo).resize((240,240),Image.Resampling.NEAREST)
    sheet.paste(im,((i%3)*288,(i//3)*260+20));ImageDraw.Draw(sheet).text(((i%3)*288,(i//3)*260),method,fill='white')
   sheet.save(OUT/f'{name}.{label}.comparison.png');entry['crops'].append(crop)
  report['files'].append(entry)
  (OUT/'results.json').write_text(json.dumps(report,indent=2));print(name,'done',flush=True)
 print(json.dumps(report['synthetic'],indent=2))
if __name__=='__main__':main()
