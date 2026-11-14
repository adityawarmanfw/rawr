"""Independent numeric checks for the augmented prototype, not production tests."""
import augmented as a
import numpy as np,json
from scipy.ndimage import correlate
r=a.r; k,stats=a.design(lr=4,cr=1)
rng=np.random.default_rng(20261114);raw=rng.normal(0,1,(256,256));out=r.apply(raw,k)
# Compare the optimized reference's phase indexing to explicit dot products.
error=0.
for oy in range(12,20):
 for ox in range(12,20):
  patch=raw[2*oy-7:2*oy+9,2*ox-7:2*ox+9]
  direct=np.sum(k[oy%4,ox%4]*patch,axis=(1,2))
  error=max(error,float(abs(direct-out[oy,ox]).max()))
assert error<1e-10,error
# Phase-specific measured variance should broadly agree with coefficient norms.
ratios=[]
for py in range(4):
 for px in range(4):
  samples=out[12+py:116:4,12+px:116:4]
  predicted=np.sum(k[py,px]**2,axis=(1,2))
  ratios.extend((np.var(samples,axis=(0,1))/predicted).tolist())
result={'explicit_dot_product_max_error':error,'noise_variance_ratio_range':[min(ratios),max(ratios)],'noise_variance_ratio_median':float(np.median(ratios)),'maximum_normal_equation_residual':max(s['normal_residual'] for s in stats),'maximum_dc_error':max(s['dc_error'] for s in stats)}
assert .6<min(ratios) and max(ratios)<1.4,result
(r.OUT/'augmented_checks.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))
