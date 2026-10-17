exec(open('tmp/ltm-reference/run.py').read().split('variants=')[0])
for name,m in [('gentle-half',1),('gentle-quarter',2)]:
 with (p/(name+'.log')).open('w') as f:
  subprocess.run(base+['--output-dir',str(p/name),'--adaptive-tone','0,-4,0','--fusion','1,2,0.5,5,7,0','--reference-base',str(m),'--reference-transfer','1'],stdout=f,stderr=subprocess.STDOUT,check=True)
 print(name,flush=True)
import numpy as np
from PIL import Image,ImageDraw
import json
# Same display-linear blend as the approved reference.
def load(name):return np.asarray(Image.open(p/name/'recon_on_h0.ppm').transpose(Image.Transpose.ROTATE_270),dtype=np.float32)/255
def lin(a):return np.where(a<=.04045,a/12.92,((a+.055)/1.055)**2.4)
def enc(a):return np.where(a<=.0031308,a*12.92,1.055*np.maximum(a,0)**(1/2.4)-.055)
g=lin(load('global-ev1'));y=g@np.array([.2126,.7152,.0722],np.float32)
t=np.clip((np.log2(np.maximum(y,1e-8))-np.log2(.04))/np.log2(.18/.04),0,1);w=1-t*t*(3-2*t)
names=['gentle-inverse','gentle-half','gentle-quarter'];images=[];metrics={};reference=None
for name in names:
 a=g+w[...,None]*(lin(load(name))-g)
 if reference is None:reference=a
 diff=np.abs(a-reference)
 metrics[name]={'linear_rgb_mae':float(diff.mean()),'linear_rgb_p99':float(np.quantile(diff,.99))}
 im=Image.fromarray(np.rint(np.clip(enc(a),0,1)*255).astype(np.uint8));images.append(im)
 im.save(p/(name+'-sky-preserved.png'));im.save(p/(name+'-sky-preserved.jpg'),quality=98,subsampling=0)
labels=['Full-resolution reference','Half resolution + guided reconstruction','Quarter resolution + guided reconstruction']
for cropname,box in [('wall',(1000,1650,1700,2250)),('sky',(2100,250,2800,850)),('roof',(600,1100,1300,1700)),('overview',None),('full-resolution',None)]:
 tiles=[im.crop(box) if box else (im.resize((613,816),Image.Resampling.LANCZOS) if cropname=='overview' else im) for im in images]
 a,b=tiles[0].size;out=Image.new('RGB',(a*3,b+60),'#202020');d=ImageDraw.Draw(out)
 for j,(tile,label) in enumerate(zip(tiles,labels)):
  out.paste(tile,(j*a,60));d.text((j*a+12,18),label,fill='white',font_size=20)
 out.save(p/('resolution-comparison-'+cropname+'.jpg'),quality=98,subsampling=0)
(p/'resolution-metrics.json').write_text(json.dumps(metrics,indent=2));print(json.dumps(metrics,indent=2))
