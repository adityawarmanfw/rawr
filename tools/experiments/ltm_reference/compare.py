from pathlib import Path
import numpy as np
from PIL import Image,ImageDraw
import json
p=Path('tmp/ltm-reference'); names=['global','quarter-ratio','full-ratio','full-inverse','full-guided-inverse','half-inverse','quarter-inverse']
def linear(x):return np.where(x<=.04045,x/12.92,((x+.055)/1.055)**2.4)
def srgb(x):return np.where(x<=.0031308,x*12.92,1.055*np.maximum(x,0)**(1/2.4)-.055)
def load(n):return linear(np.asarray(Image.open(p/n/'recon_on_h0.ppm').transpose(Image.Transpose.ROTATE_270),dtype=np.float32)/255)
def y(a):return a@np.array([.2126,.7152,.0722],np.float32)
box=(1000,1650,1700,2250); wall=(1160,1750,1500,2160)
def crop(a,b):x0,y0,x1,y1=b;return a[y0:y1,x0:x1]
med={};target=np.median(y(crop(load('global'),wall)))
for matched in [False,True]:
 sheet=Image.new('RGB',(700*4,640*2),'#202020');d=ImageDraw.Draw(sheet)
 for i,n in enumerate(names):
  a=load(n);m=float(np.median(y(crop(a,wall))));med[n]=m
  gain=target/m if matched else 1
  im=Image.fromarray(np.uint8(np.clip(srgb(crop(a,box)*gain),0,1)*255))
  x=(i%4)*700;z=(i//4)*640;sheet.paste(im,(x,z+40));d.text((x+10,z+10),n+(' %.2f EV'%np.log2(gain) if matched else ''),fill='white')
 sheet.save(p/('wall-matched.jpg' if matched else 'wall-native.jpg'),quality=95)
print(json.dumps(med,indent=2));(p/'metrics.json').write_text(json.dumps(med,indent=2))
# Validate full reference in chunks against shader-dumped desired lightness.
shape=(3064,4080,4)
a=np.memmap(p/'full-inverse/recon_on_h0.hdr.rgba16f',dtype=np.float16,mode='r',shape=shape)
b=np.memmap(p/'desired/recon_on_h0.hdr.rgba16f',dtype=np.float16,mode='r',shape=shape)
cam=np.array([.7862533613420349,-.0687295538350716,.054930436375498,.313093814128805,1.3260490215863179,-.530662861870815,-.09319000111702201,-.259987083993592,1.478577042066768]).reshape(3,3).T
ap=np.array([[1.70505,-.62179,-.08326],[-.13026,1.14080,-.01055],[-.024,-.12897,1.15297]])
inm=np.array([[.59719,.35458,.04823],[.076,.90834,.01566],[.0284,.13383,.83777]])
out=np.array([[1.60475,-.53108,-.07367],[-.10208,1.10813,-.00605],[-.00327,-.07276,1.07602]])
errors=[]
for row in range(0,3064,128):
 c=np.maximum(np.maximum(a[row:row+128,:,:3].astype(np.float32),0)@(ap@cam).T,0)/.6@inm.T
 c=np.clip(((c*(c+.0245786)-.000090537)/(c*(.983729*c+.432951)+.238081))@out.T,0,1)
 l=np.sqrt(c@np.array([.1,.7,.2]));errors.append(np.abs(l-b[row:row+128,:,0].astype(np.float32)).ravel())
e=np.concatenate(errors);print('lightness error quantiles',np.quantile(e,[.5,.95,.99,1]),'nonfinite',np.sum(~np.isfinite(e)))
des=np.asarray(b[:,:,0],dtype=np.float32).ravel(); valid=(des>=0)&(des<=1)
print('desired range',float(des.min()),float(des.max()),'outside fraction',float(np.mean(~valid)))
print('in-range errors',np.quantile(e[valid],[.5,.95,.99,1]),'over .001 fraction',float(np.mean(e[valid]>.001)))
