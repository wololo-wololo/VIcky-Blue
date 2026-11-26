#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCETRACKING_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCETRACKING_H_

#include <stdexcept>
#include "graphics/shader/recompiler/ir/ShaderIR.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

// Thrown when a compute shader's resources cannot be tracked, so its dispatch can be skipped.
// Carries the tracking diagnostic as its message.
struct UnsupportedComputeShader: std::runtime_error {
	using std::runtime_error::runtime_error;
};

// The temporary default-sampler fallback for samplers whose state cannot be traced is on unless
// strict tracking is requested; the unit tests request it to keep checking the rejection paths.
void SetStrictSamplerTracking(bool strict);

// Resolves native descriptor sources, plans their scalar reads, and assigns dense resource bindings.
void TrackResources(Program& program, const Decoder::Program& decoded, const CFG::Graph& native_cfg,
                    ShaderStageInputInfo input);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCETRACKING_H_ */
