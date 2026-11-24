#pragma once
// Android Dynamic Performance Framework (ADPF) hint session for latency-critical native work (the multiframe merge).
// A session tells the system the calling thread's work is time-critical and how long each unit of it really took
// (and, from Android 15, how much of that was GPU), so the platform can raise CPU/GPU clocks ahead of the load.
// Off unless the system property debug.rawr.adpf is 1: whether the vendor acts on it must be measured per device.
#include <android/api-level.h>
#include <android/log.h>
#include <android/performance_hint.h>
#include <dlfcn.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <cstdint>
#include <ctime>

namespace rawrcam::support {

class PerformanceHintSession {
   public:
    static bool enabledByProperty() {
        char value[PROP_VALUE_MAX] = {};
        return __system_property_get("debug.rawr.adpf", value) > 0 && value[0] == '1';
    }

    // Creates a session for the calling thread. targetNs is the duration the system should consider "on time": a small
    // value keeps the work reading as late, which asks for the highest clocks.
    explicit PerformanceHintSession(std::int64_t targetNs) {
        APerformanceHintManager* manager = APerformanceHint_getManager();
        if (!manager) return;
        const int32_t tid = static_cast<int32_t>(gettid());
        session_ = APerformanceHint_createSession(manager, &tid, 1, targetNs);
        if (session_ && api().setPreferPowerEfficiency) api().setPreferPowerEfficiency(session_, false);
    }
    ~PerformanceHintSession() {
        if (session_) APerformanceHint_closeSession(session_);
    }
    PerformanceHintSession(const PerformanceHintSession&) = delete;
    PerformanceHintSession& operator=(const PerformanceHintSession&) = delete;

    bool active() const { return session_ != nullptr; }

    // One unit of work finished: wallNs total, of which gpuNs ran on the GPU (reported separately where supported).
    void report(std::int64_t wallNs, std::int64_t gpuNs) {
        if (!session_ || wallNs <= 0) return;
        const Api& a = api();
        if (a.create && a.report2) {
            void* d = a.create();
            if (d) {
                a.setStart(d, nowNs() - wallNs);
                a.setTotal(d, wallNs);
                a.setCpu(d, wallNs > gpuNs ? wallNs - gpuNs : 1);
                a.setGpu(d, gpuNs > 0 ? gpuNs : 1);
                a.report2(session_, d);
                a.release(d);
                return;
            }
        }
        APerformanceHint_reportActualWorkDuration(session_, wallNs);
    }

   private:
    // Android 15 (API 35) entry points, resolved at run time: the app targets minSdk 33, where these are not declared usable.
    struct Api {
        void* (*create)() = nullptr;
        void (*release)(void*) = nullptr;
        int (*setStart)(void*, std::int64_t) = nullptr;
        int (*setTotal)(void*, std::int64_t) = nullptr;
        int (*setCpu)(void*, std::int64_t) = nullptr;
        int (*setGpu)(void*, std::int64_t) = nullptr;
        int (*report2)(APerformanceHintSession*, void*) = nullptr;
        int (*setPreferPowerEfficiency)(APerformanceHintSession*, bool) = nullptr;
    };
    static const Api& api() {
        static const Api table = [] {
            Api a;
            void* lib = dlopen("libandroid.so", RTLD_NOW);
            if (!lib) return a;
            auto sym = [&](const char* name) { return dlsym(lib, name); };
            a.create = reinterpret_cast<decltype(a.create)>(sym("AWorkDuration_create"));
            a.release = reinterpret_cast<decltype(a.release)>(sym("AWorkDuration_release"));
            a.setStart = reinterpret_cast<decltype(a.setStart)>(sym("AWorkDuration_setWorkPeriodStartTimestampNanos"));
            a.setTotal = reinterpret_cast<decltype(a.setTotal)>(sym("AWorkDuration_setActualTotalDurationNanos"));
            a.setCpu = reinterpret_cast<decltype(a.setCpu)>(sym("AWorkDuration_setActualCpuDurationNanos"));
            a.setGpu = reinterpret_cast<decltype(a.setGpu)>(sym("AWorkDuration_setActualGpuDurationNanos"));
            a.report2 = reinterpret_cast<decltype(a.report2)>(sym("APerformanceHint_reportActualWorkDuration2"));
            a.setPreferPowerEfficiency =
                reinterpret_cast<decltype(a.setPreferPowerEfficiency)>(sym("APerformanceHint_setPreferPowerEfficiency"));
            if (!(a.create && a.release && a.setStart && a.setTotal && a.setCpu && a.setGpu)) a.report2 = nullptr;
            return a;
        }();
        return table;
    }
    static std::int64_t nowNs() {
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return std::int64_t(ts.tv_sec) * 1'000'000'000ll + ts.tv_nsec;
    }
    APerformanceHintSession* session_ = nullptr;
};

}  // namespace rawrcam::support
