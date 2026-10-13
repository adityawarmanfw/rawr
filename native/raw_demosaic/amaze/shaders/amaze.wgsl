// SPDX-License-Identifier: GPL-3.0-or-later
// AMaZE demosaic for rawr, faithful to librtprocess amaze.cc (verified to float
// rounding against librtprocess on host). Provenance:
//   AMaZE: Emil Martinec, 2008-2010. Optimizations: Ingo Weyrich.
//   librtprocess / RawTherapee amaze.cc; darktable src/iop/demosaicing/amaze.cc
//   (Copyright (C) 2011-2024 darktable developers).
//   GPU pass structure adapted from a privately shared GPL-3.0 WGSL implementation
//   (2026; copyright remains with its author); made faithful for rawr
//   (in-place hcd/vcd, hvwt and pmwt chains, post-blend clips, chroma neighbours,
//   per-tile stage ranges, no extra envelope clamp).
// Compiled offline to SPIR-V by tools/build_spv.sh (naga). Edit this file, then
// regenerate shaders/spv/*.spv; the app embeds the committed SPIR-V.
// The first section is the shared tile ABI (Params, bindings, CFA sampling).
// Per-tile ABI shared with amaze_import.comp / amaze_export.comp and src/Pipeline.cpp
// (TileParams). In rawr `source` always holds f32 bits of the black-subtracted,
// white-normalized CFA (_pad0 = 1); wb_* are applied here and removed on export.
// _pad1 = output scale, _pad2 = output alpha, _pad4 = BayerPattern (GLSL passes only).
struct Params {
  src_origin_x:u32, src_origin_y:u32, src_width:u32, src_height:u32,
  output_origin_x:u32, output_origin_y:u32, output_width:u32, output_height:u32,
  cfa_00:u32, cfa_10:u32, cfa_01:u32, cfa_11:u32,
  black_r:f32, black_g0:f32, black_g1:f32, black_b:f32,
  white:f32, wb_r:f32, wb_g:f32, wb_b:f32,
  color_00:f32, color_01:f32, color_02:f32, color_10:f32,
  color_11:f32, color_12:f32, color_20:f32, color_21:f32,
  color_22:f32, _pad0:f32, _pad1:f32, _pad2:f32,
  highlight_reconstruction:f32, _pad3:f32, _pad4:f32, _pad5:f32, // _pad3: calibrated display white
};
// Every dispatch covers one batch of tiles: workgroup_id.z selects the tile.
// `params` holds the batch's tiles (bound at the batch offset); each tile owns
// a fixed slot of the source, output and scratch buffers.
@group(0) @binding(0) var<storage,read> source: array<u32>;
@group(0) @binding(1) var<storage,read> params: array<Params>;
@group(0) @binding(2) var<storage,read_write> output: array<vec4<f32>>;
@group(0) @binding(3) var<storage,read_write> scratch: array<f32>;
// Slot sizes: librtprocess tiles are at most 160x160 with 128x128 output.
const SRC_PIX:u32=25600u;
const OUT_PIX:u32=16384u;
// Scalar scratch channels, SRC_PIX floats each (librtprocess array in brackets).
const C_GV:u32=0u;     // vertical gradient [dirwts0]
const C_GH:u32=1u;     // horizontal gradient [dirwts1]
const C_DELSQ:u32=2u;  // delhvsqsum
const C_CFA:u32=3u;    // normalized, white-balanced CFA [cfa]
const C_CD:u32=4u;     // 4: hcd 5: vcd 6: hcdalt 7: vcdalt candidates
const C_DIFF:u32=8u;   // 8: dgintv 9: dginth
const C_HVCD:u32=10u;  // 10: hcd 11: vcd 12: cddiffsq
const C_HVWT0:u32=13u; // first hvwt estimate
const C_NYQ:u32=14u;   // nyquist test
const C_NYQ2:u32=15u;  // nyquist2 flags
const C_HVWT1:u32=16u; // area-corrected hvwt
const C_G:u32=17u;     // 17: green 18: hvwt 19: Dgrbh2 20: Dgrbv2
const C_GREF:u32=21u;  // Nyquist-refined green
const C_PM:u32=22u;    // 22: rbm 23: rbp 24: pmwt
const C_RB:u32=25u;    // 25: rbint 26: pmwt (refined)
const C_GC:u32=27u;    // 27: corrected green 28: Dgrb
const C_HVWT:u32=29u;  // refined hvwt
const C_VCD:u32=30u;   // vcd chain, then refined pmwt
const C_HCD:u32=31u;   // hcd chain
const C_DGRB:u32=32u;  // 32: Dgrb[0] 33: Dgrb[1]
const CHANNELS:u32=34u;
var<private> p:Params;
var<private> slot:u32;
fn begin_tile(z:u32){ p=params[z]; slot=z; }
// Reflect by two pixels, keeping the Bayer parity valid at image edges. A plain clamp
// changes the sample colour and causes coloured borders.
fn reflect_same_cfa(v:i32, n:i32)->i32 { if(v>=0&&v<n){return v;}if(n<=1){return 0;}let period=u32(2*(n-1));let q=u32(abs(v))%period;return i32(select(q,period-q,q>=u32(n))); }
fn source_i(x:i32,y:i32)->u32 { let xx:u32=u32(reflect_same_cfa(x,i32(p.src_width))); let yy:u32=u32(reflect_same_cfa(y,i32(p.src_height))); return yy*p.src_width+xx; }
// `_pad0` is the source encoding flag. GainMap correction writes normalized
// float bits into the same storage ABI; uncorrected tiles remain native u16.
fn raw(x:i32,y:i32)->f32 {
  let value=source[slot*SRC_PIX+source_i(x,y)];
  return select(f32(value & 65535u),bitcast<f32>(value),p._pad0>0.5);
}
fn cfa_global(x:u32,y:u32)->u32 { let xx=(p.src_origin_x+x)&1u; let yy=(p.src_origin_y+y)&1u; if(yy==0u){if(xx==0u){return p.cfa_00;}return p.cfa_10;}if(xx==0u){return p.cfa_01;}return p.cfa_11; }
fn normalize(v:f32,c:u32)->f32 { var b=p.black_r;var gain=p.wb_r;if(c==1u){b=0.5*(p.black_g0+p.black_g1);gain=p.wb_g;}if(c==2u){b=p.black_b;gain=p.wb_b;}return max(0.0,(v-b)/max(1.0,p.white-b)*gain); }
fn normalized_sample(x:i32,y:i32)->f32 {
  let xx=u32(reflect_same_cfa(x,i32(p.src_width)));let yy=u32(reflect_same_cfa(y,i32(p.src_height)));
  let c=cfa_global(xx,yy);let v=raw(i32(xx),i32(yy));
  if(p._pad0>0.5){
    var gain=p.wb_r;
    if(c==1u){gain=p.wb_g;}if(c==2u){gain=p.wb_b;}
    return max(0.0,v)*gain;
  }
  if(c!=1u){return normalize(v,c);}
  let black=select(p.black_g0,p.black_g1,((p.src_origin_y+yy)&1u)==1u);
  return max(0.0,(v-black)/max(1.0,p.white-black)*p.wb_g);
}
fn out_index(x:u32,y:u32)->u32{return slot*OUT_PIX+y*p.output_width+x;}

const AE:f32=0.00001;
const AE2:f32=0.0000000001;
fn asq(x:f32)->f32{return x*x;}
fn si(x:i32,y:i32,ch:u32)->u32{return (slot*CHANNELS+ch)*SRC_PIX+source_i(x,y);}
fn sg(x:i32,y:i32,ch:u32)->f32{return scratch[si(x,y,ch)];}
fn ss(x:i32,y:i32,ch:u32,v:f32){scratch[si(x,y,ch)]=v;}
// The normalized CFA, staged once by amaze_gradients (every later pass).
fn ac(x:i32,y:i32)->f32 { return sg(x,y,C_CFA); }
fn acol(x:i32,y:i32)->u32{return cfa_global(u32(reflect_same_cfa(x,i32(p.src_width))),u32(reflect_same_cfa(y,i32(p.src_height))));}
fn alim(v:f32,a:f32,b:f32)->f32{return clamp(v,min(a,b),max(a,b));}
fn ain(x:i32,y:i32,m:i32)->bool{return x>=m&&y>=m&&x<i32(p.src_width)-m&&y<i32(p.src_height)-m;}
fn arefine(w:f32,n:f32)->f32{return select(w,n,abs(0.5-w)<abs(0.5-n));}
// Gradients read the CFA within +-2: stage the 20x20 window.
var<workgroup> gtile:array<f32,400>;
@compute @workgroup_size(16,16)
fn amaze_gradients(@builtin(global_invocation_id) id:vec3<u32>,@builtin(workgroup_id) wg:vec3<u32>,@builtin(local_invocation_index) li:u32){
  begin_tile(wg.z);
  let o=vec2<i32>(wg.xy)*16;
  for(var i=i32(li);i<400;i+=256){gtile[i]=normalized_sample(o.x+i%20-2,o.y+i/20-2);}
  workgroupBarrier();
  if(id.x>=p.src_width||id.y>=p.src_height){return;}
  let x=i32(id.x);let y=i32(id.y);let b=(y-o.y+2)*20+(x-o.x+2);
  let c=gtile[b];
  let gv=AE+abs(gtile[b+40]-c)+abs(c-gtile[b-40])+abs(gtile[b+20]-gtile[b-20]);
  let gh=AE+abs(gtile[b+2]-c)+abs(c-gtile[b-2])+abs(gtile[b+1]-gtile[b-1]);
  ss(x,y,C_GV,gv);ss(x,y,C_GH,gh);ss(x,y,C_DELSQ,asq(gtile[b+1]-gtile[b-1])+asq(gtile[b+20]-gtile[b-20]));ss(x,y,C_CFA,c);
}
// candidates reads the CFA up to +-4 along the axes (~63 reads per pixel), so each
// 16x16 workgroup stages its 24x24 window; cc() returns exactly ac() for it.
const CR:i32=4;const CW:i32=24;
var<workgroup> ctile:array<f32,576>;
var<private> corigin:vec2<i32>;
fn cc(x:i32,y:i32)->f32{return ctile[(y-corigin.y+CR)*CW+(x-corigin.x+CR)];}
fn cgrad(x:i32,y:i32,dx:i32,dy:i32)->f32 {
  return AE+abs(cc(x+2*dx,y+2*dy)-cc(x,y))+abs(cc(x,y)-cc(x-2*dx,y-2*dy))+abs(cc(x+dx,y+dy)-cc(x-dx,y-dy));
}
// One directional estimate: adaptive ratio, Hamilton-Adams, local weight.
fn cest(x:i32,y:i32,dx:i32,dy:i32)->vec2<f32>{
  let v=cc(x,y);let n=cc(x+dx,y+dy);let n2=cc(x+2*dx,y+2*dy);
  let g=cgrad(x,y,dx,dy);let g2=cgrad(x+2*dx,y+2*dy,dx,dy);
  let ratio=n*(g2+g)/(g2*(AE+v)+g*(AE+n2));let ha=n+0.5*(v-n2);
  return vec2<f32>(select(ha,v*ratio,abs(1.0-ratio)<0.75),ha);
}
@compute @workgroup_size(16,16)
fn amaze_candidates(@builtin(global_invocation_id) id:vec3<u32>,@builtin(workgroup_id) wg:vec3<u32>,@builtin(local_invocation_index) li:u32){
  begin_tile(wg.z);
  corigin=vec2<i32>(wg.xy)*16;
  for(var i=i32(li);i<CW*CW;i+=256){ctile[i]=ac(corigin.x+i%CW-CR,corigin.y+i/CW-CR);}
  workgroupBarrier();
  if(id.x>=p.src_width||id.y>=p.src_height){return;}
  let x=i32(id.x);let y=i32(id.y);let v=cc(x,y);
  let n=cest(x,y,0,-1);let s=cest(x,y,0,1);let w=cest(x,y,-1,0);let e=cest(x,y,1,0);
  let hw=sg(x-1,y,C_GH)/(sg(x-1,y,C_GH)+sg(x+1,y,C_GH));
  let vw=sg(x,y-1,C_GV)/(sg(x,y-1,C_GV)+sg(x,y+1,C_GV));
  let hv=mix(w,e,hw);let vv=mix(n,s,vw);let sign=select(1.0,-1.0,acol(x,y)==1u);
  var cd=vec4<f32>(sign*(hv.x-v),sign*(vv.x-v),sign*(hv.y-v),sign*(vv.y-v));
  var diff=vec2<f32>(min(asq(w.y-e.y),asq(w.x-e.x)),min(asq(n.y-s.y),asq(n.x-s.x)));
  if(v>0.8||hv.y>0.8||vv.y>0.8){cd.x=cd.z;cd.y=cd.w;diff=vec2<f32>(asq(w.y-e.y),asq(n.y-s.y));}
  ss(x,y,C_CD,cd.x);ss(x,y,C_CD+1u,cd.y);ss(x,y,C_CD+2u,cd.z);ss(x,y,C_CD+3u,cd.w);
  ss(x,y,C_DIFF,diff.x);ss(x,y,C_DIFF+1u,diff.y);
}
fn avar3(a:f32,b:f32,c:f32)->f32{return 3.0*(asq(a)+asq(b)+asq(c))-asq(a+b+c);}
fn abound(cd:f32,v:f32,a:f32,b:f32,isgreen:bool)->f32{
  let sign=select(1.0,-1.0,isgreen);let estimate=v+sign*cd;let bounded=sign*(alim(estimate,a,b)-v);
  if(estimate>1.0){return bounded;}
  if(sign*cd<0.0){
    if(3.0*sign*cd < -(estimate+v)){return bounded;}
    let w=1.0+3.0*sign*cd/(AE+estimate+v);return mix(bounded,cd,clamp(w,0.0,1.0));
  }return cd;
}
// librtprocess updates hcd/vcd in place while scanning: the variance test reads the
// already-final left (x-2) hcd and upper (y-2) vcd. hcd chains run along rows and vcd
// chains down columns; the x-2 / y-2 dependency splits each into independent even and
// odd chains, so one invocation walks each (row|column, parity) and carries its previous
// result in a register. Values outside the candidate range [4, size-4) are zero, as in
// librtprocess. Dispatch: ceil(2*(width+height)/64) workgroups per tile.
fn achoice(cur:f32,alt:f32,pf:f32,pa:f32,nf:f32,na:f32,x:i32,y:i32,a:f32,b:f32)->f32{
  let c=select(cur,alt,avar3(pa,alt,na)<avar3(pf,cur,nf));
  return abound(c,ac(x,y),a,b,acol(x,y)==1u);
}
@compute @workgroup_size(64)
fn amaze_choose(@builtin(global_invocation_id) id:vec3<u32>,@builtin(workgroup_id) wg:vec3<u32>){
  begin_tile(wg.z);
  let w=i32(p.src_width);let h=i32(p.src_height);let t=i32(id.x);
  if(t<2*h){
    let y=t>>1u;let x0=t&1;var prev=0.0;
    // Candidate (cd, alt) pairs at x-2, x and x+2 slide along the chain (each loaded once).
    var back=vec2<f32>(0.0);var cur=vec2<f32>(sg(x0,y,C_CD),sg(x0,y,C_CD+2u));var left=ac(x0-1,y);
    for(var x=x0;x<w;x+=2){
      let next=vec2<f32>(sg(x+2,y,C_CD),sg(x+2,y,C_CD+2u));let right=ac(x+1,y);
      var r=0.0;
      if(ain(x,y,4)){
        var pf=0.0;var pa=0.0;var nf=0.0;var na=0.0;
        if(x-2>=4){pf=prev;pa=back.y;}
        if(x+2<w-4){nf=next.x;na=next.y;}
        r=achoice(cur.x,cur.y,pf,pa,nf,na,x,y,left,right);
      }
      ss(x,y,C_HCD,r);prev=r;back=cur;cur=next;left=right;
    }
  }else if(t<2*(h+w)){
    let x=(t-2*h)>>1u;let y0=(t-2*h)&1;var prev=0.0;
    var back=vec2<f32>(0.0);var cur=vec2<f32>(sg(x,y0,C_CD+1u),sg(x,y0,C_CD+3u));var up=ac(x,y0-1);
    for(var y=y0;y<h;y+=2){
      let next=vec2<f32>(sg(x,y+2,C_CD+1u),sg(x,y+2,C_CD+3u));let down=ac(x,y+1);
      var r=0.0;
      if(ain(x,y,4)){
        var pf=0.0;var pa=0.0;var nf=0.0;var na=0.0;
        if(y-2>=4){pf=prev;pa=back.y;}
        if(y+2<h-4){nf=next.x;na=next.y;}
        r=achoice(cur.x,cur.y,pf,pa,nf,na,x,y,up,down);
      }
      ss(x,y,C_VCD,r);prev=r;back=cur;cur=next;up=down;
    }
  }
}
@compute @workgroup_size(16,16)
fn amaze_cddiff(@builtin(global_invocation_id) id:vec3<u32>,@builtin(workgroup_id) wg:vec3<u32>){
  begin_tile(wg.z);
  if(id.x>=p.src_width||id.y>=p.src_height){return;}
  let x=i32(id.x);let y=i32(id.y);let hh=sg(x,y,C_HCD);let vv=sg(x,y,C_VCD);
  ss(x,y,C_HVCD,hh);ss(x,y,C_HVCD+1u,vv);ss(x,y,C_HVCD+2u,asq(vv-hh));
}
// Explicit taps, same summation order as the loop form: a dynamically indexed
// local array spills to memory on Adreno (measured 6x slower pass).
fn acdvar(x:i32,y:i32,dx:i32,dy:i32,ch:u32)->f32{
  let c0=sg(x,y,ch);let c1=sg(x+dx,y+dy,ch);let c2=sg(x+2*dx,y+2*dy,ch);let c3=sg(x+3*dx,y+3*dy,ch);
  let sum=((c0+c1)+c2)+c3;
  return ((asq(c0-sum)+asq(c1-sum))+asq(c2-sum))+asq(c3-sum);
}
fn adiffsum(x:i32,y:i32,dx:i32,dy:i32,ch:u32)->f32{
  return sg(x,y,ch)+sg(x+dx,y+dy,ch)+sg(x+2*dx,y+2*dy,ch);
}
@compute @workgroup_size(16,16)
fn amaze_weights(@builtin(global_invocation_id) id:vec3<u32>,@builtin(workgroup_id) wg:vec3<u32>){
  begin_tile(wg.z);
  if(id.x>=p.src_width||id.y>=p.src_height){return;}
  let x=i32(id.x);let y=i32(id.y);
  let hw=sg(x-1,y,C_GH)/(sg(x-1,y,C_GH)+sg(x+1,y,C_GH));let vw=sg(x,y-1,C_GV)/(sg(x,y-1,C_GV)+sg(x,y+1,C_GV));
  let vvar=AE2+mix(acdvar(x,y,0,-1,C_HVCD+1u),acdvar(x,y,0,1,C_HVCD+1u),vw);
  let hvar=AE2+mix(acdvar(x,y,-1,0,C_HVCD),acdvar(x,y,1,0,C_HVCD),hw);
  let vd=AE2+mix(adiffsum(x,y,0,-1,C_DIFF+1u),adiffsum(x,y,0,1,C_DIFF+1u),vw);
  let hd=AE2+mix(adiffsum(x,y,-1,0,C_DIFF),adiffsum(x,y,1,0,C_DIFF),hw);
  let a=hvar/(vvar+hvar);let b=hd/(vd+hd);
  let weight=select(b,a,(0.5-a)*(0.5-b)>0.0&&abs(0.5-b)<abs(0.5-a));
  ss(x,y,C_HVWT0,weight);
}
fn quinc(x:i32,y:i32,ch:u32,w:vec4<f32>)->f32{
  var sum=w.x*sg(x,y,ch);
  for(var dy=-2;dy<=2;dy++){for(var dx=-2;dx<=2;dx++){
    let ax=abs(dx);let ay=abs(dy);if((ax+ay)%2!=0||(ax+ay)==0){continue;}
    var k=w.y;if(ax==2||ay==2){k=select(w.z,w.w,ax==2&&ay==2);}sum+=k*sg(x+dx,y+dy,ch);
  }}return sum;
}
// nyquist reads cddiffsq and delhvsqsum within +-2; each workgroup stages both 20x20
// windows (one float each instead of a load per tap).
const NR:i32=2;const NW:i32=20;
var<workgroup> ncd:array<f32,400>;
var<workgroup> ngr:array<f32,400>;
@compute @workgroup_size(16,16)
fn amaze_nyquist(@builtin(global_invocation_id) id:vec3<u32>,@builtin(workgroup_id) wg:vec3<u32>,@builtin(local_invocation_index) li:u32){
  begin_tile(wg.z);
  let o=vec2<i32>(wg.xy)*16;
  for(var i=i32(li);i<NW*NW;i+=256){
    let qx=o.x+i%NW-NR;let qy=o.y+i/NW-NR;ncd[i]=sg(qx,qy,C_HVCD+2u);ngr[i]=sg(qx,qy,C_DELSQ);
  }
  workgroupBarrier();
  if(id.x>=p.src_width||id.y>=p.src_height){return;}
  let x=i32(id.x);let y=i32(id.y);let base=(y-o.y+NR)*NW+(x-o.x+NR);
  let wq=vec4<f32>(0.1465972771,0.1035927134,0.0732036125,0.0365543548);
  var cd=wq.x*ncd[base];
  for(var dy=-2;dy<=2;dy++){for(var dx=-2;dx<=2;dx++){
    let ax=abs(dx);let ay=abs(dy);if((ax+ay)%2!=0||(ax+ay)==0){continue;}
    var k=wq.y;if(ax==2||ay==2){k=select(wq.z,wq.w,ax==2&&ay==2);}cd+=k*ncd[base+dy*NW+dx];
  }}
  var g=0.0;
  for(var dy=-2;dy<=2;dy++){for(var dx=-2;dx<=2;dx++){
    let ax=abs(dx);let ay=abs(dy);let total=ax+ay;var w=0.0738441189;
    if(total==1){w=0.0620751197;}else if(ax==1&&ay==1){w=0.0521818195;}else if(total==2){w=0.0368741929;}else if(total==3){w=0.0309973220;}else if(total==4){w=0.0184131942;}
    g+=0.5*w*ngr[base+dy*NW+dx];
  }}
  ss(x,y,C_NYQ,select(0.0,1.0,cd>g&&acol(x,y)!=1u));
}
@compute @workgroup_size(16,16)
fn amaze_nyquist_vote(@builtin(global_invocation_id) id:vec3<u32>,@builtin(workgroup_id) wg:vec3<u32>){
  begin_tile(wg.z);
  if(id.x>=p.src_width||id.y>=p.src_height){return;}
  let x=i32(id.x);let y=i32(id.y);var count=0.0;
  for(var dy=-2;dy<=2;dy++){for(var dx=-2;dx<=2;dx++){if(abs(dx)+abs(dy)==2){count+=sg(x+dx,y+dy,C_NYQ);}}}
  var flag=sg(x,y,C_NYQ);if(count>4.0){flag=1.0;}else if(count<4.0){flag=0.0;}if(!ain(x,y,8)){flag=0.0;}
  ss(x,y,C_NYQ2,flag);
}
@compute @workgroup_size(16,16)
fn amaze_area(@builtin(global_invocation_id) id:vec3<u32>,@builtin(workgroup_id) wg:vec3<u32>){
  begin_tile(wg.z);
  if(id.x>=p.src_width||id.y>=p.src_height){return;}
  let x=i32(id.x);let y=i32(id.y);var weight=sg(x,y,C_HVWT0);
  if(sg(x,y,C_NYQ2)>0.5){
    var c=0.0;var h=0.0;var v=0.0;var sh=0.0;var sv=0.0;var n=0.0;
    for(var dy=-6;dy<=6;dy+=2){for(var dx=-6;dx<=6;dx+=2){if(sg(x+dx,y+dy,C_NYQ2)>0.5){
      let a=ac(x+dx,y+dy);let l=ac(x+dx-1,y+dy);let r=ac(x+dx+1,y+dy);let u=ac(x+dx,y+dy-1);let d=ac(x+dx,y+dy+1);
      c+=a;h+=l+r;v+=u+d;sh+=asq(a-l)+asq(a-r);sv+=asq(a-u)+asq(a-d);n+=1.0;
    }}}
    let hv=AE2+abs(0.5*n*sh-asq(c-0.5*h));let vv=AE2+abs(0.5*n*sv-asq(c-0.5*v));weight=hv/(hv+vv);
  }
  ss(x,y,C_HVWT1,weight);
}
// librtprocess refines hvwt in place while walking rows downward, so the two upper
// diagonal neighbours are already refined. One workgroup walks the rows in order.
var<workgroup> arow:array<array<f32,256>,2>;
// Input rows y and y+1 (row parity selects the slot).
var<workgroup> ain_row:array<array<f32,256>,2>;
// Tile source width is at most 160, so one lane per column; the refined row y-1 is
// read from workgroup memory. Dispatch: one workgroup per tile.
@compute @workgroup_size(256)
fn amaze_hvwt_seq(@builtin(local_invocation_index) li:u32,@builtin(workgroup_id) wg:vec3<u32>){
  begin_tile(wg.z);
  let w=i32(p.src_width);let h=i32(p.src_height);let x=i32(li);
  // ain(.,y) excludes y+1 >= h, so rows past the tile are never read.
  if(x<w){ain_row[0][x]=sg(x,0,C_HVWT1);if(h>1){ain_row[1][x]=sg(x,1,C_HVWT1);}}
  workgroupBarrier();
  for(var y=0;y<h;y++){
    let cur=u32(y&1);let up=1u-cur;let down=up;
    if(x<w){
      var v=ain_row[cur][x];
      if(ain(x,y,8)&&acol(x,y)!=1u){
        let alt=0.25*(arow[up][x-1]+arow[up][x+1]+ain_row[down][x-1]+ain_row[down][x+1]);
        v=arefine(v,alt);
      }
      ss(x,y,C_HVWT,v);arow[cur][x]=v;
    }
    workgroupBarrier();
    // Row y is consumed; its slot takes row y+2 for the next iteration.
    if(x<w&&y+2<h){ain_row[cur][x]=sg(x,y+2,C_HVWT1);}
    workgroupBarrier();
  }
}
@compute @workgroup_size(16,16)
fn amaze_green(@builtin(global_invocation_id) id:vec3<u32>,@builtin(workgroup_id) wg:vec3<u32>){
  begin_tile(wg.z);
  if(id.x>=p.src_width||id.y>=p.src_height){return;}
  let x=i32(id.x);let y=i32(id.y);
  let w=sg(x,y,C_HVWT);var g=ac(x,y);var ch=0.0;var cv=0.0;
  if(acol(x,y)!=1u){g+=mix(sg(x,y,C_HVCD),sg(x,y,C_HVCD+1u),w);if(sg(x,y,C_NYQ2)>0.5){ch=asq(g-0.5*(ac(x-1,y)+ac(x+1,y)));cv=asq(g-0.5*(ac(x,y-1)+ac(x,y+1)));}}
  ss(x,y,C_G,g);ss(x,y,C_G+1u,w);ss(x,y,C_G+2u,ch);ss(x,y,C_G+3u,cv);
}
@compute @workgroup_size(16,16)
fn amaze_refine(@builtin(global_invocation_id) id:vec3<u32>,@builtin(workgroup_id) wg:vec3<u32>){
  begin_tile(wg.z);
  if(id.x>=p.src_width||id.y>=p.src_height){return;}
  let x=i32(id.x);let y=i32(id.y);var g=sg(x,y,C_G);
  if(acol(x,y)!=1u&&sg(x,y,C_NYQ2)>0.5){
    let weights=vec4<f32>(0.169917,0.108947,0.069855,0.0287182);
    let h=AE2+quinc(x,y,C_G+2u,weights);let v=AE2+quinc(x,y,C_G+3u,weights);g=ac(x,y)+(sg(x,y,C_HVCD)*v+sg(x,y,C_HVCD+1u)*h)/(v+h);
  }ss(x,y,C_GREF,g);
}
// The diagonal pass reads the CFA only within +-3 of each pixel (~110 reads per
// pixel), so each 16x16 workgroup stages its 22x22 window once; dc() returns
// exactly ac() for that window.
const DR:i32=3;const DW:i32=22;
var<workgroup> dtile:array<f32,484>;
var<private> dorigin:vec2<i32>;
fn dc(x:i32,y:i32)->f32{return dtile[(y-dorigin.y+DR)*DW+(x-dorigin.x+DR)];}
fn adiag_est(x:i32,y:i32,dx:i32,dy:i32)->f32{
  let v=dc(x,y);let n=dc(x+dx,y+dy);let n2=dc(x+2*dx,y+2*dy);let ratio=2.0*n/(AE+v+n2);
  return select(n+0.5*(v-n2),v*ratio,abs(1.0-ratio)<0.75);
}
fn adiag_delta(x:i32,y:i32,dx:i32,dy:i32)->f32{return abs(dc(x+dx,y+dy)-dc(x-dx,y-dy));}
fn adiag_gradient(x:i32,y:i32,dx:i32,dy:i32)->f32 {
  return AE+adiag_delta(x,y,dx,dy)+adiag_delta(x+dx,y+dy,dx,dy)+adiag_delta(x+2*dx,y+2*dy,dx,dy);
}
fn adiag_sq(x:i32,y:i32,dx:i32,dy:i32)->f32{return asq(dc(x,y)-dc(x-dx,y-dy))+asq(dc(x,y)-dc(x+dx,y+dy));}
fn adiag_var(x:i32,y:i32,dx:i32,dy:i32)->f32{
  var sum=AE2;for(var yy=-2;yy<=2;yy++){for(var xx=-2;xx<=2;xx++){
    let total=abs(xx)+abs(yy);if(total==1||total==3){sum+=select(0.0564025278,0.1371949444,total==1)*adiag_sq(x+xx,y+yy,dx,dy);}
  }}return sum;
}
fn adiag_bound(estimate:f32,v:f32,a:f32,b:f32)->f32{
  var r=estimate;
  if(estimate<v){
    if(2.0*estimate<v){r=alim(estimate,a,b);}
    else{let w=2.0*(v-estimate)/(AE+estimate+v);r=w*estimate+(1.0-w)*alim(estimate,a,b);}
  }
  if(r>1.0){r=alim(r,a,b);}
  return r;
}
@compute @workgroup_size(16,16)
fn amaze_diagonal(@builtin(global_invocation_id) id:vec3<u32>,@builtin(workgroup_id) wg:vec3<u32>,@builtin(local_invocation_index) li:u32){
  begin_tile(wg.z);
  dorigin=vec2<i32>(wg.xy)*16;
  for(var i=i32(li);i<DW*DW;i+=256){dtile[i]=ac(dorigin.x+i%DW-DR,dorigin.y+i/DW-DR);}
  workgroupBarrier();
  if(id.x>=p.src_width||id.y>=p.src_height){return;}
  let x=i32(id.x);let y=i32(id.y);let nw=adiag_gradient(x,y,-1,-1);let se=adiag_gradient(x,y,1,1);let ne=adiag_gradient(x,y,1,-1);let sw=adiag_gradient(x,y,-1,1);
  let m=(se*adiag_est(x,y,-1,-1)+nw*adiag_est(x,y,1,1))/(nw+se);
  let q=(ne*adiag_est(x,y,-1,1)+sw*adiag_est(x,y,1,-1))/(ne+sw);
  let vm=adiag_var(x,y,1,1);let vp=adiag_var(x,y,1,-1);
  ss(x,y,C_PM,adiag_bound(m,dc(x,y),dc(x-1,y-1),dc(x+1,y+1)));ss(x,y,C_PM+1u,adiag_bound(q,dc(x,y),dc(x+1,y-1),dc(x-1,y+1)));ss(x,y,C_PM+2u,vm/(vm+vp));
}
@compute @workgroup_size(256)
fn amaze_pmwt_seq(@builtin(local_invocation_index) li:u32,@builtin(workgroup_id) wg:vec3<u32>){
  begin_tile(wg.z);
  let w=i32(p.src_width);let h=i32(p.src_height);let x=i32(li);
  // ain(.,y) excludes y+1 >= h, so rows past the tile are never read.
  if(x<w){ain_row[0][x]=sg(x,0,C_PM+2u);if(h>1){ain_row[1][x]=sg(x,1,C_PM+2u);}}
  workgroupBarrier();
  for(var y=0;y<h;y++){
    let cur=u32(y&1);let up=1u-cur;let down=up;
    if(x<w){
      var v=ain_row[cur][x];
      if(ain(x,y,10)&&acol(x,y)!=1u){
        let alt=0.25*(arow[up][x-1]+arow[up][x+1]+ain_row[down][x-1]+ain_row[down][x+1]);
        v=arefine(v,alt);
      }
      ss(x,y,C_VCD,v);arow[cur][x]=v;
    }
    workgroupBarrier();
    // Row y is consumed; its slot takes row y+2 for the next iteration.
    if(x<w&&y+2<h){ain_row[cur][x]=sg(x,y+2,C_PM+2u);}
    workgroupBarrier();
  }
}
@compute @workgroup_size(16,16)
fn amaze_rbint(@builtin(global_invocation_id) id:vec3<u32>,@builtin(workgroup_id) wg:vec3<u32>){
  begin_tile(wg.z);
  if(id.x>=p.src_width||id.y>=p.src_height){return;}
  let x=i32(id.x);let y=i32(id.y);
  let w=sg(x,y,C_VCD);
  ss(x,y,C_RB,0.5*(ac(x,y)+mix(sg(x,y,C_PM),sg(x,y,C_PM+1u),w)));ss(x,y,C_RB+1u,w);
}
// correct reads rbint and the CFA within +-2: stage both 20x20 windows.
var<workgroup> rgm:array<f32,400>;
var<workgroup> rcf:array<f32,400>;
fn rest(b:i32,step:i32)->f32{
  let v=rgm[b];let n=rcf[b+step];let n2=rgm[b+2*step];let ratio=2.0*n/(AE+v+n2);
  return select(n+0.5*(v-n2),v*ratio,abs(1.0-ratio)<0.75);
}
@compute @workgroup_size(16,16)
fn amaze_correct(@builtin(global_invocation_id) id:vec3<u32>,@builtin(workgroup_id) wg:vec3<u32>,@builtin(local_invocation_index) li:u32){
  begin_tile(wg.z);
  let o=vec2<i32>(wg.xy)*16;
  for(var i=i32(li);i<400;i+=256){
    let qx=o.x+i%20-2;let qy=o.y+i/20-2;rgm[i]=sg(qx,qy,C_RB);rcf[i]=ac(qx,qy);
  }
  workgroupBarrier();
  if(id.x>=p.src_width||id.y>=p.src_height){return;}
  let x=i32(id.x);let y=i32(id.y);let b=(y-o.y+2)*20+(x-o.x+2);let hv=sg(x,y,C_G+1u);var g=sg(x,y,C_GREF);
  if(acol(x,y)!=1u&&abs(0.5-sg(x,y,C_RB+1u))>=abs(0.5-hv)){
    let n=sg(x,y-1,C_GV);let s=sg(x,y+1,C_GV);let w=sg(x-1,y,C_GH);let e=sg(x+1,y,C_GH);
    let gv=(n*rest(b,20)+s*rest(b,-20))/(s+n);
    let gh=(w*rest(b,1)+e*rest(b,-1))/(w+e);let v=rgm[b];
    g=mix(adiag_bound(gh,v,rcf[b-1],rcf[b+1]),adiag_bound(gv,v,rcf[b-20],rcf[b+20]),hv);
  }ss(x,y,C_GC,g);ss(x,y,C_GC+1u,g-rcf[b]);
}
// chroma reads Dgrb within +-3: stage the 22x22 window.
var<workgroup> htile:array<f32,484>;
var<private> horigin:vec2<i32>;
fn hd(x:i32,y:i32)->f32{return htile[(y-horigin.y+3)*22+(x-horigin.x+3)];}
@compute @workgroup_size(16,16)
fn amaze_chroma(@builtin(global_invocation_id) id:vec3<u32>,@builtin(workgroup_id) wg:vec3<u32>,@builtin(local_invocation_index) li:u32){
  begin_tile(wg.z);
  horigin=vec2<i32>(wg.xy)*16;
  for(var i=i32(li);i<484;i+=256){htile[i]=sg(horigin.x+i%22-3,horigin.y+i/22-3,C_GC+1u);}
  workgroupBarrier();
  if(id.x>=p.src_width||id.y>=p.src_height){return;}
  let x=i32(id.x);let y=i32(id.y);let c=acol(x,y);var result=vec2<f32>(0.0);
  if(c!=1u){
    let nw=hd(x-1,y-1);let ne=hd(x+1,y-1);let sw=hd(x-1,y+1);let se=hd(x+1,y+1);
    let wnw=1.0/(AE+abs(nw-se)+abs(nw-hd(x-3,y-3))+abs(se-hd(x-3,y-3)));
    let wne=1.0/(AE+abs(ne-sw)+abs(ne-hd(x+3,y-3))+abs(sw-hd(x+3,y-3)));
    let wsw=1.0/(AE+abs(sw-ne)+abs(sw-hd(x+3,y+3))+abs(ne-hd(x-3,y+3)));
    let wse=1.0/(AE+abs(se-nw)+abs(se-hd(x-3,y+3))+abs(nw-hd(x+3,y+3)));
    let enw=1.325*nw-0.175*hd(x-3,y-3)-0.075*hd(x-3,y-1)-0.075*hd(x-1,y-3);
    let ene=1.325*ne-0.175*hd(x+3,y-3)-0.075*hd(x+3,y-1)-0.075*hd(x+1,y+1);
    let esw=1.325*sw-0.175*hd(x-3,y+3)-0.075*hd(x-3,y+1)-0.075*hd(x-1,y-1);
    let ese=1.325*se-0.175*hd(x+3,y+3)-0.075*hd(x+3,y+1)-0.075*hd(x+1,y+3);
    let weighted=wnw*enw+wne*ene+wsw*esw+wse*ese;let total=wnw+wne+wsw+wse;
    result[c/2u]=hd(x,y);result[1u-c/2u]=weighted/max(AE,total);
  }ss(x,y,C_DGRB,result.x);ss(x,y,C_DGRB+1u,result.y);
}
fn adgrb(x:i32,y:i32)->vec2<f32>{return vec2<f32>(sg(x,y,C_DGRB),sg(x,y,C_DGRB+1u));}
@compute @workgroup_size(16,16)
fn amaze_finish(@builtin(global_invocation_id) id:vec3<u32>,@builtin(workgroup_id) wg:vec3<u32>){
  begin_tile(wg.z);
  if(id.x>=p.output_width||id.y>=p.output_height){return;}
  let x=i32(id.x+p.output_origin_x-p.src_origin_x);let y=i32(id.y+p.output_origin_y-p.src_origin_y);let g=sg(x,y,C_GC);
  var diff=adgrb(x,y);
  if(acol(x,y)==1u){
    let n=sg(x,y-1,C_G+1u);let s=sg(x,y+1,C_G+1u);let w=1.0-sg(x-1,y,C_G+1u);let e=1.0-sg(x+1,y,C_G+1u);
    diff=(n*adgrb(x,y-1)+s*adgrb(x,y+1)+w*adgrb(x-1,y)+e*adgrb(x+1,y))/max(AE,n+s+w+e);
  }
  // librtprocess clamps every reconstructed channel at zero (max(0, 65535 * v)).
  output[out_index(id.x,id.y)]=vec4<f32>(max(vec3<f32>(g-diff.x,g,g-diff.y),vec3<f32>(0.0)),1.0);
}
