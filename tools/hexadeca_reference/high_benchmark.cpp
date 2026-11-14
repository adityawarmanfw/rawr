// Research wrapper: exact High + NaturalBayer reduction, bounded tile scratch.
#include "imaging/CfaRemosaic.h"
#include "renderer/NaturalBayerBin.h"
#include <atomic>
#include <cassert>
extern "C" void high_reduce(const uint16_t* raw,int w,int h,int side,int workers,const float* gains,float* output){
 assert(raw && gains && output && w>0 && h>0 && w%8==0 && h%8==0 && side>0 && side%4==0 && workers>0);
 const int ow=w/2,oh=h/2;
 const int nx=(ow+side-1)/side,ny=(oh+side-1)/side;
 std::atomic<int> next{0};
 auto work=[&]{
  rawrcam::renderer::NaturalBayerTile bin;
  std::vector<uint16_t> scratch;
  std::array<float,4> g{gains[0],gains[1],gains[2],gains[3]};
  for(int job;(job=next.fetch_add(1))<nx*ny;){
   int ox=(job%nx)*side,oy=(job/nx)*side,tw=std::min(side,ow-ox),th=std::min(side,oh-oy);
   // NaturalBayer halo20 + High's green/chroma dependency <=18; round to
   // an 8-pixel CFA period. Only actual image borders may truncate support.
   int x0=std::max(0,2*ox-40),y0=std::max(0,2*oy-40);
   int x1=std::min(w,2*(ox+tw)+40),y1=std::min(h,2*(oy+th)+40);
   int sw=x1-x0,sh=y1-y0;scratch.resize(size_t(sw)*sh);
   for(int y=0;y<sh;y++)std::copy_n(raw+size_t(y+y0)*w+x0,sw,scratch.data()+size_t(y)*sw);
   rawrcam::imaging::remosaicBlockCfa(scratch.data(),sw,sh,4,3,1,rawrcam::imaging::CfaRemosaicQuality::High);
   bin.run(ox-x0/2,oy-y0/2,tw,th,sw,sh,g,3,.25f/(959.f*959.f),
     [&](int x,int y){return (float(scratch[size_t(y)*sw+x])-64.f)/959.f;},[]{},output+size_t(oy)*ow+ox,ow);
  }
 };
 std::vector<std::thread> pool;for(int i=1;i<workers;i++)pool.emplace_back(work);work();for(auto& t:pool)t.join();
}
extern "C" void reduce_only(const uint16_t* raw,int w,int h,int side,int workers,const float* gains,float* output){
 const int ow=w/2,oh=h/2,nx=(ow+side-1)/side,ny=(oh+side-1)/side;
 std::atomic<int> next{0};
 auto work=[&]{rawrcam::renderer::NaturalBayerTile bin;std::array<float,4> g{gains[0],gains[1],gains[2],gains[3]};
  for(int job;(job=next.fetch_add(1))<nx*ny;){int ox=(job%nx)*side,oy=(job/nx)*side,tw=std::min(side,ow-ox),th=std::min(side,oh-oy);
   bin.run(ox,oy,tw,th,w,h,g,3,.25f/(959.f*959.f),[&](int x,int y){return (float(raw[size_t(y)*w+x])-64.f)/959.f;},[]{},output+size_t(oy)*ow+ox,ow);
  }
 };
 std::vector<std::thread> pool;for(int i=1;i<workers;i++)pool.emplace_back(work);work();for(auto& t:pool)t.join();
}

extern "C" void high_remosaic(uint16_t* p,unsigned w,unsigned h,unsigned n,unsigned order,unsigned threads,unsigned quality){
 rawrcam::imaging::remosaicBlockCfa(p,w,h,n,order,threads,static_cast<rawrcam::imaging::CfaRemosaicQuality>(quality));
}

// Quarter-DN input/output; callers retain the exact sum of four 10-bit sites.
extern "C" void quad_remosaic_precise(uint16_t* p,unsigned w,unsigned h,unsigned threads){
 rawrcam::imaging::remosaicBlockCfa(p,w,h,2,3,threads,rawrcam::imaging::CfaRemosaicQuality::High,4.0f);
}
