exec(open('tmp/ltm-reference/run.py').read().split('variants=')[0])
with (p/'global-ev1.log').open('w') as f:
 subprocess.run(base+['--output-dir',str(p/'global-ev1'),'--exposure-ev','1','--adaptive-tone','0.76,-4.49946928,-0.677802324','--local-tone','0'],stdout=f,stderr=subprocess.STDOUT,check=True)
from PIL import Image,ImageDraw
names=['global-ev1','quarter-ratio','full-inverse']
labels=['Global | exposure +1 EV','Current local | strength 2 EV','Corrected full-resolution local | strength 2 EV']
images=[Image.open(p/n/'recon_on_h0.ppm').transpose(Image.Transpose.ROTATE_270) for n in names]
for name,box in [('wall',(1000,1650,1700,2250)),('sky',(2100,250,2800,850)),('full',None)]:
 tiles=[im.crop(box) if box else im.resize((613,816),Image.Resampling.LANCZOS) for im in images]
 w,h=tiles[0].size
 out=Image.new('RGB',(w*3,h+45),'#202020');d=ImageDraw.Draw(out)
 for i,(im,label) in enumerate(zip(tiles,labels)):
  out.paste(im,(i*w,45));d.text((i*w+10,15),label,fill='white')
 out.save(p/('global-ev1-vs-local-'+name+'.jpg'),quality=97,subsampling=0)
# Direct comparison requested, with corrected local on the right.
a=Image.open(p/'global-ev1-vs-local-wall.jpg');o=Image.new('RGB',(1400,645));o.paste(a.crop((0,0,700,645)),(0,0));o.paste(a.crop((1400,0,2100,645)),(700,0));o.save(p/'global-ev1-vs-corrected-wall.jpg',quality=97,subsampling=0)
