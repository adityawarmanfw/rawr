"""Full-frame viewing render of the exact tiled High + NaturalBayer result.
Uses LibRaw AHD for final Bayer-to-RGB visualization, camera WB/matrices, sRGB.
Research DNG omits source GainMap opcodes pending native-phase interpretation.
"""
from pathlib import Path
import ctypes,json,time
import numpy as np,rawpy,tifffile
from PIL import Image
root=Path(__file__).resolve().parents[2];out=root/'tmp/hexadeca-full';out.mkdir(parents=True,exist_ok=True)
source=Path.home()/'Downloads/RAWR_20261112_165945_230.dng'
with rawpy.imread(str(source)) as f:
 raw=f.raw_image_visible.copy();wb=np.array(f.camera_whitebalance[:3]);wb/=wb[1]
h,w=raw.shape;g=np.array([wb[2],1,1,wb[0]],np.float32);g/=g.max()
lib=ctypes.CDLL(str(root/'tmp/high-remosaic/tiled.dylib'));lib.high_reduce.argtypes=[np.ctypeslib.ndpointer(dtype=np.uint16,flags='C_CONTIGUOUS')]+[ctypes.c_int]*4+[np.ctypeslib.ndpointer(dtype=np.float32,flags='C_CONTIGUOUS')]*2
bayer=np.empty((h//2,w//2),np.float32);lib.high_reduce(raw,w,h,256,8,g,bayer);del raw
codes=np.uint16(np.rint(np.clip(bayer,0,1)*65535));del bayer
extra=[(50706,'B',4,(1,4,0,0),False),(50707,'B',4,(1,1,0,0),False),(50708,'s',0,'RAWR Hexadeca 50MP research render',False),(33421,'H',2,(2,2),False),(33422,'B',4,(2,1,1,0),False),(50710,'B',3,(0,1,2),False),(50711,'H',1,1,False),(50714,'H',1,0,False),(50717,'I',1,65535,False),(50829,'I',4,(0,0,h//2,w//2),False),(50719,'I',2,(0,0),False),(50720,'I',2,(w//2,h//2),False)]
with tifffile.TiffFile(source) as tf:
 for code in [50721,50722,50728,50778,50779,50964,50965]:
  tag=tf.pages[0].tags.get(code)
  if tag:extra.append((code,int(tag.dtype),tag.count,tag.value,False))
dng=out/'hexadeca-50mp-research.dng'
tifffile.imwrite(dng,codes,photometric=32803,metadata=None,extratags=extra,rowsperstrip=128);del codes
with rawpy.imread(str(dng)) as f:
 assert f.raw_image_visible.shape==(6144,8160)
 print('rendering',f.raw_image_visible.shape,flush=True)
 rgb=f.postprocess(use_camera_wb=True,demosaic_algorithm=rawpy.DemosaicAlgorithm.AHD,output_color=rawpy.ColorSpace.sRGB,output_bps=8,no_auto_bright=False,bright=1.,user_flip=0)
im=Image.fromarray(rgb);im.save(out/'hexadeca-50mp.jpg',quality=97,subsampling=0)
im.thumbnail((1800,1800),Image.Resampling.LANCZOS);im.save(out/'hexadeca-preview.jpg',quality=95,subsampling=0)
(out/'render.json').write_text(json.dumps({'source':str(source),'dimensions':[8160,6144],'pipeline':['native Hexadeca High remosaic','NaturalBayer half-width/height reduction','LibRaw AHD demosaic','camera WB and DNG colour matrices','LibRaw auto-bright and sRGB'],'limitations':['not production RCD rendering','source lens gain maps omitted','no denoise or sharpening','research DNG is a viewing intermediate, not a validated converter export']},indent=2))
print('saved',out,flush=True)
