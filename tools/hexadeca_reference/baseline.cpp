#include "imaging/CfaRemosaic.h"
#include "renderer/NaturalBayerBin.h"
extern "C" void remosaic(unsigned short* p,int w,int h,int block,int quality) {
 rawrcam::imaging::remosaicBlockCfa(p,w,h,block,3,1,static_cast<rawrcam::imaging::CfaRemosaicQuality>(quality));
}
extern "C" void reduce(const unsigned short* p,int w,int h,const float* gain,float* out) {
 rawrcam::renderer::NaturalBayerTile tile;
 std::array<float,4> g{gain[0],gain[1],gain[2],gain[3]};
 tile.run(0,0,w/2,h/2,w,h,g,3,0.25f/(959.f*959.f),
   [&](int x,int y){return (float(p[y*w+x])-64.f)/959.f;}, []{},out,w/2);
}
