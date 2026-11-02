#include <jni.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>
#include "tonemap/Agx.h"

#include "raw_preview.h"
#include "raw_preview_cfa_state.h"
#include "highlight_guide_seed.h"
#include "highlight_guide_propagate.h"
#include "highlight_guide_smooth.h"
#include "highlight_apply_inplace.h"
#include "coloropp.h"
#include "coloropp_tone.h"
#include "tonemap.h"
#include "tonemap_reference.h"

int rawr_rgb_validation(int, char**);
int rawr_cfa_validation(int, char**);
int rawr_preview_benchmark(int, char**);
int rawr_tonemap_validation(int, char**);
int rawr_readback_validation();

extern "C" JNIEXPORT jint JNICALL Java_com_rawr_gputest_HeadlessGpuTest_agxPresetCount(JNIEnv*, jclass) {
    return static_cast<jint>(tonemap::kAgxPresetCount);
}

namespace {
std::mutex lane;
std::string text(JNIEnv* env, jstring s) {
    const char* chars = env->GetStringUTFChars(s, nullptr);
    if (!chars) throw std::runtime_error("JNI string allocation failed");
    std::string result(chars);
    env->ReleaseStringUTFChars(s, chars);
    return result;
}
void writeShader(const std::filesystem::path& dir, const char* name, const unsigned char* data, size_t size) {
    std::ofstream out(dir / name, std::ios::binary);
    if (!out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size)))
        throw std::runtime_error("cannot write shader");
}
}

extern "C" JNIEXPORT jint JNICALL Java_com_rawr_gputest_HeadlessGpuTest_runNative(
    JNIEnv* env, jclass, jstring testCase, jobjectArray options, jstring outputPath) {
    std::lock_guard<std::mutex> lock(lane);
    const auto originalCwd = std::filesystem::current_path();
    const int savedOut = dup(STDOUT_FILENO), savedErr = dup(STDERR_FILENO);
    int result = -1;
    try {
        const std::filesystem::path dir(text(env, outputPath));
        std::filesystem::create_directories(dir);
#define SHADER(symbol, file) writeShader(dir, file, symbol##_bytes, symbol##_bytes_size)
        SHADER(raw_preview, "raw_preview.comp.spv");
        SHADER(raw_preview_cfa_state, "raw_preview_cfa_state.comp.spv");
        SHADER(highlight_guide_seed, "raw_preview_highlight_guide_seed.comp.spv");
        SHADER(highlight_guide_propagate, "raw_preview_highlight_guide_propagate.comp.spv");
        SHADER(highlight_guide_smooth, "raw_preview_highlight_guide_smooth.comp.spv");
        SHADER(highlight_apply_inplace, "raw_preview_highlight_apply.comp.spv");
        SHADER(coloropp, "raw_preview_coloropp.comp.spv");
        SHADER(coloropp_tone, "raw_preview_coloropp_tone.comp.spv");
        SHADER(tonemap, "tonemap.comp.spv");
        SHADER(tonemap_reference, "tonemap-reference.comp.spv");
#undef SHADER
        const int log = open((dir / "native.txt").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (log < 0 || savedOut < 0 || savedErr < 0) throw std::runtime_error("cannot create test log");
        std::cout.flush(); std::cerr.flush(); std::fflush(nullptr);
        dup2(log, STDOUT_FILENO); dup2(log, STDERR_FILENO); close(log);
        std::filesystem::current_path(dir);
        const auto name = text(env, testCase);
        std::vector<std::string> values{name};
        for (jsize i = 0; i < env->GetArrayLength(options); ++i) {
            const auto option = static_cast<jstring>(env->GetObjectArrayElement(options, i));
            values.push_back(text(env, option));
            env->DeleteLocalRef(option);
        }
        std::vector<char*> argv;
        for (auto& value : values) argv.push_back(value.data());
        argv.push_back(nullptr);
        const int argc = static_cast<int>(values.size());
        if (name == "rgb") result = rawr_rgb_validation(argc, argv.data());
        else if (name == "cfa") result = rawr_cfa_validation(argc, argv.data());
        else if (name == "preview") result = rawr_preview_benchmark(argc, argv.data());
        else if (name == "tonemap") result = rawr_tonemap_validation(argc, argv.data());
        else if (name == "readback") result = rawr_readback_validation();
        else throw std::runtime_error("unknown test case");
    } catch (const std::exception& error) { std::cerr << "HARNESS_FAIL " << error.what() << '\n'; }
    std::cout.flush(); std::cerr.flush(); std::fflush(nullptr);
    std::filesystem::current_path(originalCwd);
    if (savedOut >= 0) { dup2(savedOut, STDOUT_FILENO); close(savedOut); }
    if (savedErr >= 0) { dup2(savedErr, STDERR_FILENO); close(savedErr); }
    return result;
}
