from pathlib import Path
import subprocess
p=Path('tmp/ltm-reference')
base=[str(p/'build/rawr_offline_pipeline'),'--input','tmp/ltm-texture/source/input.rgba16f','--config','tmp/ltm-texture/source/input.rgba16f.txt','--single-recovery','1','--post-gain','1','--app-coloropp','--app-hlth','1','--app-compression','100','--dump-hdr']
variants={'quarter-ratio':(2,0,False),'full-ratio':(0,0,True),'full-inverse':(0,1,True),'full-guided-inverse':(0,1,False),'half-inverse':(1,1,False),'quarter-inverse':(2,1,False),'desired':(0,2,True)}
for name,(m,t,d) in variants.items():
 args=base+['--output-dir',str(p/name),'--adaptive-tone','0,-4,0','--fusion','1,2,2,5,7,1','--reference-base',str(m),'--reference-transfer',str(t)]+(['--reference-direct'] if d else [])
 with (p/(name+'.log')).open('w') as f: subprocess.run(args,stdout=f,stderr=subprocess.STDOUT,check=True)
 print(name,flush=True)
for name,ev,adaptive in [('global',0,'0.76,-4.49946928,-0.677802324'),('neutral',0,'0,-4,0'),('dark',-2,'0,-4,0'),('bright',2,'0,-4,0')]:
 with (p/(name+'.log')).open('w') as f:subprocess.run(base+['--output-dir',str(p/name),'--exposure-ev',str(ev),'--adaptive-tone',adaptive,'--local-tone','0'],stdout=f,stderr=subprocess.STDOUT,check=True)
 print(name,flush=True)
