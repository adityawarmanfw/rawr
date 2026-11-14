"""Singh 2011 Eq. 7–9 local augmented least-squares reference.
Uses explicit experimental bandwidths and orthonormal YUV, not author parameters.
"""
import reference as r
import numpy as np,json,time
from scipy.ndimage import correlate1d
from PIL import Image,ImageDraw

def design(n=16,lr=6,cr=2,weight=1,angle=None,aspect=1):
 # B=[C; weight*S], where C picks measured RGB sites and S contains
 # out-of-band colour-transform DFT rows. B^+ [raw;0] solves the paper's
 # soft-constraint system. No hard-subspace parameterization or ridge.
 start=1-n//2;y,x=np.mgrid[start:start+n,start:start+n];N=n*n
 fy,fx=np.meshgrid(np.fft.fftfreq(n),np.fft.fftfreq(n),indexing='ij')
 f=np.stack([fy.ravel(),fx.ravel()],1)
 F=np.exp(-2j*np.pi*(f@np.stack([y.ravel(),x.ravel()],0)))/n
 U=np.array([[1,1,1],[1,0,-1],[1,-2,1]],float);U/=np.linalg.norm(U,axis=1)[:,None]
 constraints=[]
 for j,rad in enumerate([lr,cr,cr]):
  if angle is None:
   radius2=np.sum(f*f,axis=1)
  else:
   along=f[:,1]*np.cos(angle)+f[:,0]*np.sin(angle)
   across=-f[:,1]*np.sin(angle)+f[:,0]*np.cos(angle)
   radius2=along*along+(across/aspect)**2
  out=radius2>(rad/n)**2+1e-12
  constraints.append(np.hstack([U[j,c]*F[out] for c in range(3)]))
 S=np.vstack(constraints); P=(weight*weight*(S.conj().T@S)).real
 taps=np.array([1,3,3,1])/8;T=np.zeros((3,3*N))
 for c in range(3):
  for j in range(4):
   for i in range(4):T[c,c*N+(-1+j-start)*n+(-1+i-start)]=taps[j]*taps[i]
 kernels=np.empty((4,4,3,n,n));stats=[]
 for py in range(4):
  for px in range(4):
   col=r.PAT[((y+2*py)//4)%2,((x+2*px)//4)%2].ravel();idx=col*N+np.arange(N)
   H=P.copy();H[idx,idx]+=1
   # Symmetric eigensolver gives minimum-norm pseudoinverse, even if singular.
   eig,V=np.linalg.eigh(H);tol=eig[-1]*1e-10;active=eig>tol
   inv=(V[:,active]/eig[active])@V[:,active].T
   full=inv[:,idx]
   K=T@full
   stats.append({'phase':[2*py,2*px],'rank':int(active.sum()),'unknowns':3*N,'dc_error':float(abs(K@np.eye(3)[col]-np.eye(3)).max()),'noise_gain':float(np.linalg.norm(K,axis=1).max()),'normal_residual':float(np.max(abs(H@full-np.eye(3*N)[:,idx])))})
   kernels[py,px]=K.reshape(3,n,n)
 return kernels,stats

def main():
 y,x=np.indices((128,128));scenes={
 'flat':np.broadcast_to([.2,.4,.7],(128,128,3)),
 'edge':np.repeat((.1+.7*(x+.7*y>100))[...,None],3,2),
 'grating':np.repeat((.45+.28*np.cos(2*np.pi*(.17*x+.09*y)))[...,None],3,2),
 'colour_edge':np.where((x+.7*y>100)[...,None],[.8,.15,.1],[.1,.3,.8])}
 report={'formulation':'B=[C;weight*S], x=B^+[raw;0]; local 16x16, normalized DFT, orthonormal YUV','variants':[]}
 name='RAWR_20261112_165945_230.dng'
 with r.rawpy.imread(str(r.Path.home()/'Downloads'/name)) as f:
  a=(f.raw_image_visible[6016:6272,8032:8288].astype(float)-64)/959;wb=np.array(f.camera_whitebalance[:3]);wb/=wb[1]
 images={}
 for lr,cr,weight in [(6,2,1),(5,2,1),(5,2,3),(4,1,1)]:
  t=time.perf_counter();k,stats=design(lr=lr,cr=cr,weight=weight);label=f'aug_l{lr}_c{cr}_w{weight}'
  entry={'name':label,'seconds':time.perf_counter()-t,'phases':stats,'synthetic':{}}
  for name2,rgb in scenes.items():
   raw=np.take_along_axis(rgb,r.colors(128,128)[...,None],2)[...,0]
   out=r.apply(raw,k)[12:-12,12:-12];target=r.lowpass(rgb)[12:-12,12:-12]
   entry['synthetic'][name2]={'psnr':float(-10*np.log10(max(np.mean((out-target)**2),1e-30))),'max_error':float(abs(out-target).max())}
  entry['flat_gate_pass']=entry['synthetic']['flat']['max_error']<1e-7
  if lr<6: assert entry['flat_gate_pass']
  assert max(s['normal_residual'] for s in stats)<1e-7
  images[label]=r.apply(a,k);np.save(r.OUT/f'{label}.npy',images[label]);report['variants'].append(entry)
  print(label,entry['synthetic'],flush=True)
  (r.OUT/'augmented.json').write_text(json.dumps(report,indent=2))
 images['existing_high']=np.load(r.OUT/'RAWR_20261112_165945_230.dng.centre.existing_2.npy')
 images['previous_ridge']=np.load(r.OUT/'RAWR_20261112_165945_230.dng.centre.spectral_c2.npy')
 expo=min(16.,.35/max(float(np.median(images['existing_high'][16:-16,16:-16,1])),.025));sheet=Image.new('RGB',(864,520))
 for i,(label,rgb) in enumerate(images.items()):
  im=r.preview(rgb[16:-16,16:-16],wb,expo).resize((240,240),Image.Resampling.NEAREST);sheet.paste(im,((i%3)*288,(i//3)*260+20));ImageDraw.Draw(sheet).text(((i%3)*288,(i//3)*260),label,fill='white')
 sheet.save(r.OUT/'augmented_comparison.png')
if __name__=='__main__':main()
