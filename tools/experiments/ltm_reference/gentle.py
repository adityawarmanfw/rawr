exec(open('tmp/ltm-reference/run.py').read().split('variants=')[0])
with (p/'gentle-inverse.log').open('w') as f:
 subprocess.run(base+['--output-dir',str(p/'gentle-inverse'),'--adaptive-tone','0,-4,0','--fusion','1,2,0.5,5,7,0','--reference-base','0','--reference-transfer','1','--reference-direct'],stdout=f,stderr=subprocess.STDOUT,check=True)
from PIL import Image,ImageDraw
names=['global-ev1','full-inverse','gentle-inverse']
labels=['Global | exposure +1 EV','Previous corrected | highlights 2 EV, boost on','Gentler corrected | highlights 0.5 EV, boost off']
images=[Image.open(p/n/'recon_on_h0.ppm').transpose(Image.Transpose.ROTATE_270) for n in names]
for name,box in [('wall',(1000,1650,1700,2250)),('sky',(2100,250,2800,850)),('full',None)]:
 tiles=[im.crop(box) if box else im.resize((613,816),Image.Resampling.LANCZOS) for im in images]
 w,h=tiles[0].size
 out=Image.new('RGB',(w*3,h+45),'#202020');d=ImageDraw.Draw(out)
 for i,(im,label) in enumerate(zip(tiles,labels)):
  out.paste(im,(i*w,45));d.text((i*w+10,15),label,fill='white')
 out.save(p/('gentle-comparison-'+name+'.jpg'),quality=97,subsampling=0)
