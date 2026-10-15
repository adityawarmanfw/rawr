#pragma once
struct AImage;
extern "C" void AImage_delete(AImage* image);
extern "C" void AImage_deleteAsync(AImage* image, int releaseFenceFd);
