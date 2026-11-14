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
 parser.add_argument('--out',type=Path,required=True)
 parser.add_argument('--gpu-bayer',type=Path,help='Quarter-DN Bayer RAW16 from standalone Vulkan test')
 parser.add_argument('--quarter-dn',action='store_true')
 parser.add_argument('--baseline',type=Path,required=True)
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
 import time
 start=time.perf_counter()
 # Four 10-bit samples fit uint16. Their exact sum encodes quarter DN.
 # The legacy comparison mode rounds back to whole DN.
 quad=raw[0::2,0::2].copy()
 quad+=raw[0::2,1::2];quad+=raw[1::2,0::2];quad+=raw[1::2,1::2]
 if not args.quarter_dn:
  quad+=2
  quad//=4
 del raw
 bintime=time.perf_counter()-start
 lib.high_remosaic.argtypes=[ptr]+[ctypes.c_uint]*6
 start=time.perf_counter()
 if args.gpu_bayer:
  assert args.quarter_dn, 'GPU result uses quarter-DN codes'
  quad=np.fromfile(args.gpu_bayer,dtype=np.uint16).reshape(6144,8160)
 elif args.quarter_dn:
  lib.quad_remosaic_precise.argtypes=[ptr]+[ctypes.c_uint]*3
  lib.quad_remosaic_precise(quad,8160,6144,8)
 else: lib.high_remosaic(quad,8160,6144,2,3,8,2)
 remtime=time.perf_counter()-start
 bayer=(quad.astype(np.float32)/(4 if args.quarter_dn else 1)-64)/959;del quad
 if args.gpu_bayer:
  print('Loaded GPU Bayer; Python timings do not measure GPU conversion',flush=True)
 else:
  print('Quad averaging seconds',bintime,'50MP remosaic seconds',remtime,flush=True)
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
 # Match baseline exposure exactly, not variant quantile.
 baseline=json.loads((args.baseline/'diagnostic.json').read_text())
 assert Path(baseline['source']).resolve()==source.resolve()
 assert np.allclose(matrix,baseline['matrix'])
 exposure=baseline['exposure']
 result=np.empty((h,w,3),np.uint8)
 for y in range(0,h,256):
  v=np.clip((np.asarray(rgb[y:y+256])@matrix.T)*exposure,0,1)
  v=np.where(v<=.0031308,v*12.92,1.055*v**(1/2.4)-.055)
  result[y:y+256]=np.uint8(np.rint(v*255))
 im=Image.fromarray(result)
 display=im.transpose(Image.Transpose.ROTATE_270) if orientation==6 else im.copy()
 display.save(OUT/'quad-50mp.jpg',quality=97,subsampling=0)
 for region,box in [('foliage',(3760,2832,4400,3312)),('sky',(3800,1200,4440,1680))]:
  crop=im.crop(box);crop.save(OUT/f'quad-{region}.png')
  old=Image.open(args.baseline/f'before-{region}.png')
  panel=Image.new('RGB',(1280,510));panel.paste(old,(0,30));panel.paste(crop,(640,30))
  from PIL import ImageDraw
  d=ImageDraw.Draw(panel);d.text((10,8),'Current High + Natural + RCD',fill='white');d.text((650,8),'2x2 average + Quad High + RCD',fill='white')
  panel.save(OUT/f'compare-{region}.png')
 display.thumbnail((1800,1800),Image.Resampling.LANCZOS);display.save(OUT/'quad-preview.jpg',quality=95)
 (OUT/'timing.json').write_text(json.dumps(dict(quarter_dn=args.quarter_dn,gpu_bayer=str(args.gpu_bayer) if args.gpu_bayer else None,averaging_seconds=None if args.gpu_bayer else bintime,remosaic_seconds=None if args.gpu_bayer else remtime,exposure=exposure),indent=2))
 print('renders complete',flush=True)
if __name__=='__main__':main()
