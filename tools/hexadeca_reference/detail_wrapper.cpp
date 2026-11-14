#include "imaging/CfaRemosaic.h"
#include "imaging/PhaseFit.h"
extern "C" void detail_quad(uint16_t* p,unsigned w,unsigned h,unsigned threads){
 rawrcam::imaging::remosaicBlockCfa(p,w,h,2,3,threads,rawrcam::imaging::CfaRemosaicQuality::Detail,4.0f);
}
extern "C" void fit_sum(const uint16_t* raw,unsigned w,unsigned h,unsigned stride,uint16_t* out,unsigned threads){
 rawrcam::imaging::hexadecaSumWithFit(raw,w,h,64.0f,stride,out,threads);
}
