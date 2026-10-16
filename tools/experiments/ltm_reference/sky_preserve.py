from pathlib import Path
import json
import numpy as np
from PIL import Image, ImageDraw
p=Path('tmp/ltm-reference')
def read(n): return np.asarray(Image.open(p/n/'recon_on_h0.ppm').transpose(Image.Transpose.ROTATE_270),dtype=np.float32)/255
def lin(a):return np.where(a<=.04045,a/12.92,((a+.055)/1.055)**2.4)
def enc(a):return np.where(a<=.0031308,a*12.92,1.055*np.maximum(a,0)**(1/2.4)-.055)
def im(a):return Image.fromarray(np.rint(np.clip(enc(a),0,1)*255).astype(np.uint8))
g=lin(read('global-ev1'));l=lin(read('gentle-inverse'))
y=g@np.array([.2126,.7152,.0722],np.float32)
# Display-domain look prototype. Full local below Y=.04; full global above .18.
# A per-pixel guide avoids spatial mask bleeding across foliage/sky edges.
# Smooth log-luminance transition, no spatial blur or sharpening.
t=np.clip((np.log2(np.maximum(y,1e-8))-np.log2(.04))/np.log2(.18/.04),0,1)
w=1-t*t*(3-2*t)
r=g+w[...,None]*(l-g)
ri=im(r);ri.save(p/'sky-preserved-local-full-resolution.png');ri.save(p/'sky-preserved-local-full-resolution.jpg',quality=98,subsampling=0)
ims=[im(g),im(l),ri];labels=['Global +1 EV','Previous corrected local','Prototype: preserve global highlights']
for name,box in [('wall',(1000,1650,1700,2250)),('sky',(2100,250,2800,850)),('full',None)]:
 tiles=[i.crop(box) if box else i.resize((613,816),Image.Resampling.LANCZOS) for i in ims]
 a,b=tiles[0].size;out=Image.new('RGB',(a*3,b+48),'#202020');d=ImageDraw.Draw(out)
 for j,(tile,label) in enumerate(zip(tiles,labels)):
  out.paste(tile,(j*a,48));d.text((j*a+10,12),label,fill='white',font_size=18)
 out.save(p/('sky-preserved-comparison-'+name+'.jpg'),quality=98,subsampling=0)
a,b=ri.size;out=Image.new('RGB',(a*2,b+60),'#202020');out.paste(ims[0],(0,60));out.paste(ri,(a,60));d=ImageDraw.Draw(out)
d.text((20,15),labels[0],fill='white',font_size=24);d.text((a+20,15),labels[2],fill='white',font_size=24)
out.save(p/'global-vs-sky-preserved-full-resolution.jpg',quality=98,subsampling=0)
metrics={}
for name,box in [('clear_sky',(2150,260,2400,450)),('wall',(1160,1750,1500,2160))]:
 x0,y0,x1,y1=box;s=np.s_[y0:y1,x0:x1];metrics[name]={'mean_local_weight':float(w[s].mean()),'global_meanY':float(y[s].mean()),'prototype_meanY':float((r[s]@np.array([.2126,.7152,.0722])).mean()),'max_rgb_difference_from_global':float(np.abs(r[s]-g[s]).max())}
(p/'sky-preserved-metrics.json').write_text(json.dumps(metrics,indent=2));print(json.dumps(metrics,indent=2))
