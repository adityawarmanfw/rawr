exec(open('tmp/ltm-reference/compare.py').read().split('med={};')[0])
from scipy.ndimage import gaussian_filter

def resize(a,shape):return np.asarray(Image.fromarray(a).resize((shape[1],shape[0]),Image.Resampling.BILINEAR))
def pyr(a):
 r=[a]
 for i in range(7):r.append(gaussian_filter(r[-1],1,mode='reflect')[::2,::2])
 return r
ls=[];ws=[]
for n in ['dark','neutral','bright']:
 l=np.sqrt(np.maximum(y(load(n)),0));ls.append(pyr(l));ws.append(pyr(np.exp(-.5*(l-.5)**2*25)))
for boost in [False,True]:
 result=None
 for level in range(7,-1,-1):
  vals=[l[level] if level==7 else l[level]-resize(l[level+1],l[level].shape) for l in ls]
  weights=[w[level]*(np.abs(v)+1e-5 if boost and level<7 else 1) for w,v in zip(ws,vals)]
  fused=sum(w*v for w,v in zip(weights,vals))/np.maximum(sum(weights),1e-8)
  result=fused if result is None else resize(result,fused.shape)+fused
 a=load('neutral');a*= (np.maximum(result,0)**2/np.maximum(y(a),1e-8))[...,None]
 name='display-boost' if boost else 'display';(p/name).mkdir(exist_ok=True)
 Image.fromarray(np.uint8(np.clip(srgb(a),0,1)*255)).transpose(Image.Transpose.ROTATE_90).save(p/name/'recon_on_h0.ppm')
 print(name,flush=True)
