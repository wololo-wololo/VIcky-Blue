#pragma once

#include "graphics/shader/recompiler/ir/Block.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

struct Program;

void ConstantPropagationPass(const BlockList& blocks, uint32_t wave_size = 64,
                             bool aggressive = true);
bool LowerDivergentExecRegions(Program& program, bool single_lane = true);

} // namespace Libs::Graphics::ShaderRecompiler::IR
