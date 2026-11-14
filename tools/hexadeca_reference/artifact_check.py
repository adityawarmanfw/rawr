"""Consistent RCD before/after diagnostic, not a production calibration change."""
from pathlib import Path
import sys,json,ctypes,argparse
import numpy as np,rawpy,tifffile
from scipy.ndimage import median_filter,gaussian_filter
from PIL import Image
ROOT=Path(__file__).resolve().parents[2];OUT=ROOT/'tmp/hexadeca-artifacts';OUT.mkdir(exist_ok=True)
sys.path.insert(0,str(ROOT/'native/raw_demosaic/rcd/tools'))
from rcd_numba_core import demosaic_rg

def rcd(b):
 # Rotate even-size BGGR into RGGB, demosaic, rotate RGB back.
 return demosaic_rg(np.ascontiguousarray(b[::-1,::-1]))[::-1,::-1].copy()

def cleanup(rgb):
 # Experimental edge-aware median chroma filter. Exact YIQ inverse preserves
 # Y; unlike full RGB smoothing, it leaves brightness detail unchanged.
 M=np.array([[.299,.587,.114],[.596,-.275,-.321],[.212,-.523,.311]],np.float32)
 yiq=rgb@M.T;Y=yiq[...,0];iq=median_filter(yiq[...,1:],size=(3,3,1),mode='reflect')
 acc=np.zeros_like(iq);weights=np.zeros_like(Y)
 padY=np.pad(Y,1,mode='reflect');padIQ=np.pad(iq,((1,1),(1,1),(0,0)),mode='reflect');h,w=Y.shape
 for dy in range(3):
  for dx in range(3):
   wt=np.exp(-((padY[dy:dy+h,dx:dx+w]-Y)/.02)**2)
   acc+=padIQ[dy:dy+h,dx:dx+w]*wt[...,None];weights+=wt
 yiq[...,1:]=acc/weights[...,None]
 return yiq@np.linalg.inv(M).T

def phase(rgb,period):
 if rgb.ndim==2:rgb=rgb[...,None]
 out=[]
 for c in range(rgb.shape[2]):
  v=rgb[...,c];smooth=gaussian_filter(v,12);rel=(v-smooth)/np.maximum(abs(smooth),.02)
  med=np.array([[np.median(rel[32+y:-32:period,32+x:-32:period]) for x in range(period)] for y in range(period)])
  out.append({'relative_phase_peak_to_peak':float(np.ptp(med)),'phase_residual':med.tolist()})
 return out

def main():
 global OUT
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('source',nargs='?',type=Path,default=Path.home()/'Downloads/RAWR_20261112_165945_230.dng')
 parser.add_argument('--out',type=Path,default=OUT)
 args=parser.parse_args();source=args.source;OUT=args.out;OUT.mkdir(parents=True,exist_ok=True)
 with tifffile.TiffFile(source) as f:
  tags=f.pages[0].tags
  orientation=int(tags[274].value) if 274 in tags else 1
  assert orientation in (1,6), "Unsupported display orientation"
  def rational(code):v=np.array(tags[code].value,float);return v[::2]/v[1::2]
  wb=1/rational(50728);wb/=wb[1];fm=rational(50964).reshape(3,3)
  assert np.allclose(fm,rational(50965).reshape(3,3)), "Different illuminant matrices need interpolation"
 # DNG forward matrix maps white-balanced camera RGB to XYZ D50.
 brad=np.array([[.8951,.2664,-.1614],[-.7502,1.7135,.0367],[.0389,-.0685,1.0296]])
 adapt=np.linalg.inv(brad)@np.diag((brad@np.array([.95047,1,1.08883]))/(brad@np.array([.96422,1,.82521])))@brad
 xyzrgb=np.array([[3.2404542,-1.5371385,-.4985314],[-.969266,1.8760108,.041556],[.0556434,-.2040259,1.0572252]])
 matrix=(xyzrgb@adapt@fm@np.diag(wb)).astype(np.float32)
 with rawpy.imread(str(source)) as f:
  assert f.raw_image_visible.shape==(12288,16320) and f.white_level==1023 and all(v==64 for v in f.black_level_per_channel), 'Unsupported prototype calibration'
  raw=f.raw_image_visible.copy()
 lib=ctypes.CDLL(str(ROOT/'tmp/high-remosaic/tiled.dylib'))
 ptr=np.ctypeslib.ndpointer(dtype=np.uint16,flags='C_CONTIGUOUS');fp=np.ctypeslib.ndpointer(dtype=np.float32,flags='C_CONTIGUOUS')
 lib.high_reduce.argtypes=[ptr]+[ctypes.c_int]*4+[fp,fp]
 bayer=np.empty((6144,8160),np.float32);g=np.array([wb[2],1,1,wb[0]],np.float32);g/=g.max()
 lib.high_reduce(raw,16320,12288,256,8,g,bayer);del raw
 # Match the prior research-DNG baseline's 16-bit quantization exactly.
 np.clip(bayer,0,1,out=bayer);bayer*=65535;np.rint(bayer,out=bayer);bayer/=65535
 h,w=bayer.shape;rgb=np.lib.format.open_memmap(OUT/'rcd-camera.npy',mode='w+',dtype=np.float32,shape=(h,w,3))
 demosaic_rg(np.zeros((32,32),np.float32))
 for y in range(0,h,256):
  y0=max(0,y-32);y1=min(h,y+256+32);tile=rcd(bayer[y0:y1]);rgb[y:y+256]=tile[y-y0:min(y+256,h)-y0]
  if y%1024==0:print('RCD rows',y,flush=True)
 del bayer;rgb.flush()
 # Fixed exposure from baseline; never retune it for after image.
 sample=np.maximum(np.asarray(rgb[::16,::16])@matrix.T,0);exposure=.9/max(float(np.quantile(sample,.995)),.01)
 before=np.empty((h,w,3),np.uint8);after=np.empty_like(before);Yerror=0
 M=np.array([.299,.587,.114],np.float32)
 for y in range(0,h,256):
  y0=max(0,y-4);y1=min(h,y+256+4);tile=np.array(rgb[y0:y1]);clean=cleanup(tile)
  s=slice(y-y0,min(y+256,h)-y0);Yerror=max(Yerror,float(np.max(abs(clean[s]@M-tile[s]@M))))
  for destination,values in [(before,tile[s]),(after,clean[s])]:
   v=np.clip((values@matrix.T)*exposure,0,1);v=np.where(v<=.0031308,v*12.92,1.055*v**(1/2.4)-.055);destination[y:min(y+256,h)]=np.uint8(np.rint(v*255))
 for label,data in [('before',before),('after',after)]:
  im=Image.fromarray(data)
  display=im.transpose(Image.Transpose.ROTATE_270) if orientation==6 else im
  display.save(OUT/f'{label}-50mp.jpg',quality=97,subsampling=0)
  for region,box in [('sky',(3800,1200,4440,1680)),('foliage',(3760,2832,4400,3312))]:im.crop(box).save(OUT/f'{label}-{region}.png')
  display.thumbnail((1800,1800),Image.Resampling.LANCZOS);display.save(OUT/f'{label}-preview.jpg',quality=95,subsampling=0)
 print('renders complete',flush=True)
 # Stage inspection on a matching native sky ROI, aligned to CFA period.
 with rawpy.imread(str(source)) as f:raw=f.raw_image_visible[2560:3584,7648:8672].copy()
 norm=(raw.astype(float)-64)/959;blocks=norm.reshape(256,4,256,4).transpose(0,2,1,3)
 native={}
 for label,py,px in [('B',0,0),('G1',0,1),('G2',1,0),('R',1,1)]:
  z=blocks[py::2,px::2];m=z.mean((2,3));valid=m>.02;count=int(valid.sum())
  pattern=np.median((z/np.maximum(m[...,None,None],.02))[valid],axis=0) if count else None
  native[label]={'selected_blocks':count,'within_block_span':float(np.ptp(pattern)) if count else None}
 lib=ctypes.CDLL(str(ROOT/'tmp/high-remosaic/tiled.dylib'));ptr=np.ctypeslib.ndpointer(dtype=np.uint16,flags='C_CONTIGUOUS');fp=np.ctypeslib.ndpointer(dtype=np.float32,flags='C_CONTIGUOUS')
 lib.high_remosaic.argtypes=[ptr]+[ctypes.c_uint]*6;lib.reduce_only.argtypes=[ptr]+[ctypes.c_int]*4+[fp,fp]
 lib.high_remosaic(raw,1024,1024,4,3,1,2)
 remosaic={str((py,px)):phase((raw[py::2,px::2].astype(float)-64)/959,4) for py in range(2) for px in range(2)}
 b=np.empty((512,512),np.float32);g=np.array([wb[2],1,1,wb[0]],np.float32);g/=g.max();lib.reduce_only(raw,1024,1024,256,2,g,b)
 reduced={str((py,px)):phase(b[py::2,px::2],2) for py in range(2) for px in range(2)}
 rr=rcd(b);cc=cleanup(rr)
 metrics={'source':str(source),'display_orientation':orientation,'native':native,'remosaic':remosaic,'reduced':reduced,'rcd':phase(rr,4),'cleanup':phase(cc,4),'Y_max_change':Yerror,'exposure':exposure,'matrix':matrix.tolist(),'limitations':['No gain-map correction or inferred phase calibration','RCD BGGR by even-image rotation','Experimental one-pass YIQ chroma filtering; no sharpening or denoise','Forward matrices identical in source; D50 to D65 Bradford adaptation','Stage phase metrics differ in sampling support and are not directly comparable noise estimates']}
 (OUT/'diagnostic.json').write_text(json.dumps(metrics,indent=2));print('done',flush=True)
if __name__=='__main__':main()
