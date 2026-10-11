#pragma once
#include <amaze/Amaze.hpp>
#include <dual/ShaderProvider.hpp>
#include <quadfix/ShaderProvider.hpp>
#include <rcd/ShaderProvider.hpp>
#include <vng4/ShaderProvider.hpp>

// Shader providers backed by SPIR-V compiled into the binary with the
// production workgroup sizes (target raw_demosaic::embedded_shaders).
namespace raw_demosaic {
::amaze::ShaderProvider embeddedAmazeShaders();
::rcd::ShaderProvider embeddedRcdShaders();
::vng4::ShaderProvider embeddedVng4Shaders();
// Also resolves the vng4_* and rcd_* shaders the Dual pipeline instantiates.
::dual::ShaderProvider embeddedDualShaders();
::quadfix::ShaderProvider embeddedQuadfixShaders();
}  // namespace raw_demosaic
