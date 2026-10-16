exec(open('tmp/ltm-reference/compare.py').read().split('med={};')[0])
names=['global','quarter-ratio','full-inverse','display','display-boost','default-quarter','default-full-inverse']
labels=['Global','Current local','Full resolution + inverse','RAWR display fusion','RAWR fusion + contrast boost','Default local','Default full + inverse']
target=np.median(y(crop(load('global'),wall)))
metrics={}
for cropname,box2 in [('wall',box),('sky',(2100,250,2800,850))]:
 for matched in ([False,True] if cropname=='wall' else [False]):
  sheet=Image.new('RGB',(700*len(names),640),'#202020');d=ImageDraw.Draw(sheet)
  for i,(n,label) in enumerate(zip(names,labels)):
   a=load(n);m=float(np.median(y(crop(a,wall))));metrics[n]={'wallY':m,'wallEVvsGlobal':float(np.log2(m/target))};gain=target/m if matched else 1
   im=Image.fromarray(np.uint8(np.clip(srgb(crop(a,box2)*gain),0,1)*255))
   sheet.paste(im,(i*700,40));d.text((i*700+10,10),label+(' (%+.2f EV)'%np.log2(gain) if matched else ''),fill='white')
  sheet.save(p/(cropname+('-equal-brightness' if matched else '-comparison')+'.jpg'),quality=96)
(p/'final-metrics.json').write_text(json.dumps(metrics,indent=2))
# Concise three-column comparison for sharing.
for matched in [False,True]:
 src=Image.open(p/('wall-equal-brightness.jpg' if matched else 'wall-comparison.jpg'))
 out=Image.new('RGB',(2100,640))
 for j,i in enumerate([0,1,2]):out.paste(src.crop((i*700,0,(i+1)*700,640)),(j*700,0))
 out.save(p/('comparison-matched.jpg' if matched else 'comparison.jpg'),quality=96)
