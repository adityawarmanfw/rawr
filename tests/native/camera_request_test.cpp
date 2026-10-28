#include <cassert>
#include <cstdint>
#include <iostream>

#include "camera/CameraRequest.h"

using namespace rawrcam::camera;

namespace {
ANativeWindow* window(uintptr_t id) { return reinterpret_cast<ANativeWindow*>(id); }

void testSetGetClear() {
    DataRequest r(2);
    assert(r.requestTemplate() == 2);
    const int32_t fps[2] = {12, 30};
    assert(r.setI32(0x10, 2, fps) == kRequestOk);
    RequestEntry e{};
    assert(r.get(0x10, &e) == kRequestOk);
    assert(e.type == RequestType::Int32 && e.count == 2 && e.data.i32[0] == 12 && e.data.i32[1] == 30);
    // Clearing reads like an absent tag, but the entry stays so the Java builder unsets the template default.
    assert(r.setI32(0x10, 0, nullptr) == kRequestOk);
    assert(r.get(0x10, &e) == kRequestNotFound);
    assert(r.entries().at(0x10).count == 0);
    assert(r.get(0x99, &e) == kRequestNotFound);
    assert(r.setU8(0x11, 1, nullptr) == kRequestInvalidParameter);
}

void testTypes() {
    DataRequest r;
    const uint8_t u8 = 3;
    const int64_t i64 = -5'000'000'000LL;
    const float f = 1.5f;
    const double d = 0.25;
    const RequestRational q[2] = {{1, 2}, {-3, 4}};
    r.setU8(1, 1, &u8);
    r.setI64(2, 1, &i64);
    r.setFloat(3, 1, &f);
    r.setDouble(4, 1, &d);
    r.setRational(5, 2, q);
    RequestEntry e{};
    assert(r.get(1, &e) == kRequestOk && e.type == RequestType::Byte && e.data.u8[0] == 3);
    assert(r.get(2, &e) == kRequestOk && e.type == RequestType::Int64 && e.data.i64[0] == i64);
    assert(r.get(3, &e) == kRequestOk && e.type == RequestType::Float && e.data.f[0] == 1.5f);
    assert(r.get(4, &e) == kRequestOk && e.type == RequestType::Double && e.data.d[0] == 0.25);
    assert(r.get(5, &e) == kRequestOk && e.count == 2 && e.data.r[1].numerator == -3 && e.data.r[1].denominator == 4);
    int32_t count = 0;
    const uint32_t* tags = nullptr;
    assert(r.tags(&count, &tags) == kRequestOk && count == 5 && tags[0] == 1 && tags[4] == 5);
    r.setU8(1, 0, nullptr);
    assert(r.tags(&count, &tags) == kRequestOk && count == 4 && tags[0] == 2);
}

void testTargetsContextCopy() {
    DataRequest r(1);
    assert(r.addTarget(window(1)) == kRequestOk);
    assert(r.addTarget(window(2)) == kRequestOk);
    assert(r.addTarget(window(1)) == kRequestOk);  // once per window
    assert(r.targets().size() == 2);
    assert(r.removeTarget(window(1)) == kRequestOk);
    assert(r.targets().size() == 1 && r.targets()[0] == window(2));
    assert(r.addTarget(nullptr) == kRequestInvalidParameter);
    int marker = 0;
    r.setUserContext(&marker);
    r.noteTagName(0x80000000u, "vendor.key");
    const int32_t v = 7;
    r.setI32(0x80000000u, 1, &v);
    const auto copied = r.copy();
    const auto* c = dynamic_cast<const DataRequest*>(copied.get());
    assert(c && c->requestTemplate() == 1 && c->targets().size() == 1);
    void* context = nullptr;
    assert(c->userContext(&context) == kRequestOk && context == &marker);
    assert(c->tagNames().at(0x80000000u) == "vendor.key");
    // The copy is independent.
    const int32_t w = 9;
    r.setI32(0x80000000u, 1, &w);
    RequestEntry e{};
    assert(c->get(0x80000000u, &e) == kRequestOk && e.data.i32[0] == 7);
}
}  // namespace

int main() {
    testSetGetClear();
    testTypes();
    testTargetsContextCopy();
    std::cout << "camera_request_test passed\n";
    return 0;
}
