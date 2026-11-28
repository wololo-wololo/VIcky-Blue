#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVOUTLINER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVOUTLINER_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::Spirv {

// Outlines repeated single-entry single-exit (SESE) structured selection/switch
// regions in `main` into `DontInline` helper functions to reduce SPIR-V module
// size and host driver compile time.
void OutlineRepeatedRegions(std::vector<uint32_t>& words, size_t min_module_words = 32768u);

} // namespace Libs::Graphics::ShaderRecompiler::Spirv

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVOUTLINER_H_
