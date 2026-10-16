exec(open('tmp/ltm-reference/run.py').read().split('variants=')[0])
for name,m,t,d in [('default-quarter',2,0,False),('default-full-inverse',0,1,True),('default-desired',0,2,True)]:
 with (p/(name+'.log')).open('w') as f:subprocess.run(base+['--output-dir',str(p/name),'--adaptive-tone','0,-4,0','--fusion','1,2,2,5,6,0','--reference-base',str(m),'--reference-transfer',str(t)]+(['--reference-direct'] if d else []),stdout=f,stderr=subprocess.STDOUT,check=True)
 print(name,flush=True)
