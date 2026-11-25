#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCETRACKING_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCETRACKING_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

// Thrown when a compute shader's resources cannot be tracked, so its dispatch can be skipped.
struct UnsupportedComputeShader {};

// Resolves native descriptor sources, plans their scalar reads, and assigns dense resource bindings.
// With gpu_table_descriptors, a buffer descriptor selected from a table is not enumerated on the
// host: the shader reads the V# and accesses memory through its device address.
void TrackResources(Program& program, const Decoder::Program& decoded, const CFG::Graph& native_cfg,
                    ShaderStageInputInfo input, bool gpu_table_descriptors = false);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCETRACKING_H_ */
