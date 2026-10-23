#pragma once
#include <media/NdkImage.h>
#include <android/native_window.h>
#include <deque>
struct AImageReader_ImageListener {
    void* context = nullptr;
    void (*onImageAvailable)(void*, AImageReader*) = nullptr;
};
struct AImageReader {
    AImageReader_ImageListener listener;
    int held = 0, limit = 0;
    std::deque<int64_t> pending;
    ANativeWindow window;
};
int AImageReader_newWithUsage(int32_t, int32_t, int32_t, uint64_t, int32_t, AImageReader**);
int AImageReader_setImageListener(AImageReader*, AImageReader_ImageListener*);
void AImageReader_delete(AImageReader*);
int AImageReader_getWindow(AImageReader*, ANativeWindow**);
int AImageReader_acquireNextImageAsync(AImageReader*, AImage**, int*);
