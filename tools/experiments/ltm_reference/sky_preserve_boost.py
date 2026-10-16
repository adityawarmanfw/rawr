exec(open('tmp/ltm-reference/run.py').read().split('variants=')[0])
with (p/'gentle-boost-inverse.log').open('w') as f:
 subprocess.run(base+['--output-dir',str(p/'gentle-boost-inverse'),'--adaptive-tone','0,-4,0','--fusion','1,2,0.5,5,7,1','--reference-base','0','--reference-transfer','1','--reference-direct'],stdout=f,stderr=subprocess.STDOUT,check=True)
# Use exactly the same blend; only change its local input and output names.
s=open(p/'sky_preserve.py').read().replace("read('gentle-inverse')","read('gentle-boost-inverse')").replace('sky-preserved','sky-preserved-boost')
exec(compile(s,str(p/'sky_preserve.py'),'exec'))
# Compare the previous prototype directly against the boost-on prototype.
a=Image.open(p/'sky-preserved-local-full-resolution.png').convert('RGB')
b=Image.open(p/'sky-preserved-boost-local-full-resolution.png').convert('RGB')
for name,box in [('wall',(1000,1650,1700,2250)),('sky',(2100,250,2800,850)),('overview',None),('full-resolution',None)]:
 tiles=[i.crop(box) if box else (i.resize((766,1020),Image.Resampling.LANCZOS) if name=='overview' else i) for i in [a,b]]
 width,height=tiles[0].size;out=Image.new('RGB',(width*2,height+60),'#202020');d=ImageDraw.Draw(out)
 for j,(tile,label) in enumerate(zip(tiles,['Local contrast OFF | global sky preserved','Local contrast ON | global sky preserved'])):
  out.paste(tile,(j*width,60));d.text((j*width+16,18),label,fill='white',font_size=22)
 out.save(p/('contrast-off-vs-on-'+name+'.jpg'),quality=98,subsampling=0)
