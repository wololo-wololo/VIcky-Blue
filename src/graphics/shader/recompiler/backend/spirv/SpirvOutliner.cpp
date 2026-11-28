#include "graphics/shader/recompiler/backend/spirv/SpirvOutliner.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <queue>
#include <spirv/unified1/spirv.hpp>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::Spirv {
namespace {

constexpr uint32_t kSpirvMagic = 0x07230203u;
constexpr size_t   kMinRegionWords = 96u;
constexpr size_t   kMaxInputs      = 16u;
constexpr size_t   kMaxOutputs     = 8u;

struct InstSlice {
	spv::Op               op     = spv::OpNop;
	std::vector<uint32_t> words; // Full instruction including header word at [0]
};

struct ParsedInst {
	bool                  valid       = false;
	bool                  has_type    = false;
	bool                  has_result  = false;
	uint32_t              type_id     = 0u;
	uint32_t              result_id   = 0u;
	std::vector<size_t>   id_word_indices; // Word indices (in `words`) that are SSA IDs
};

bool IsNonResultOp(spv::Op op) {
	switch (op) {
		case spv::OpNop:
		case spv::OpSourceContinued:
		case spv::OpSource:
		case spv::OpSourceExtension:
		case spv::OpName:
		case spv::OpMemberName:
		case spv::OpLine:
		case spv::OpNoLine:
		case spv::OpModuleProcessed:
		case spv::OpDecorate:
		case spv::OpMemberDecorate:
		case spv::OpGroupDecorate:
		case spv::OpGroupMemberDecorate:
		case spv::OpDecorationGroup:
		case spv::OpCapability:
		case spv::OpExtension:
		case spv::OpMemoryModel:
		case spv::OpEntryPoint:
		case spv::OpExecutionMode:
		case spv::OpExecutionModeId:
		case spv::OpStore:
		case spv::OpCopyMemory:
		case spv::OpCopyMemorySized:
		case spv::OpBranch:
		case spv::OpBranchConditional:
		case spv::OpSwitch:
		case spv::OpReturn:
		case spv::OpReturnValue:
		case spv::OpKill:
		case spv::OpTerminateInvocation:
		case spv::OpDemoteToHelperInvocation:
		case spv::OpUnreachable:
		case spv::OpSelectionMerge:
		case spv::OpLoopMerge:
		case spv::OpLifetimeStart:
		case spv::OpLifetimeStop:
		case spv::OpAtomicStore:
		case spv::OpControlBarrier:
		case spv::OpMemoryBarrier:
		case spv::OpImageWrite:
		case spv::OpEmitVertex:
		case spv::OpEndPrimitive:
		case spv::OpEmitStreamVertex:
		case spv::OpEndStreamPrimitive:
		case spv::OpFunctionEnd:
		case spv::OpRayQueryInitializeKHR:
		case spv::OpRayQueryTerminateKHR:
		case spv::OpRayQueryGenerateIntersectionKHR:
		case spv::OpRayQueryConfirmIntersectionKHR: return true;
		default: return false;
	}
}

bool IsRecognizedFunctionBodyOp(spv::Op op) {
	if (IsNonResultOp(op)) {
		return op != spv::OpFunctionEnd && op != spv::OpCapability && op != spv::OpExtension &&
		       op != spv::OpMemoryModel && op != spv::OpEntryPoint && op != spv::OpExecutionMode &&
		       op != spv::OpExecutionModeId && op != spv::OpDecorate &&
		       op != spv::OpMemberDecorate && op != spv::OpGroupDecorate &&
		       op != spv::OpGroupMemberDecorate && op != spv::OpDecorationGroup;
	}
	switch (op) {
		case spv::OpLabel:
		case spv::OpUndef:
		case spv::OpVariable:
		case spv::OpLoad:
		case spv::OpAccessChain:
		case spv::OpInBoundsAccessChain:
		case spv::OpPtrAccessChain:
		case spv::OpArrayLength:
		case spv::OpFunctionCall:
		case spv::OpExtInst:
		case spv::OpPhi:
		case spv::OpSelect:
		case spv::OpCompositeConstruct:
		case spv::OpCompositeExtract:
		case spv::OpCompositeInsert:
		case spv::OpVectorShuffle:
		case spv::OpVectorExtractDynamic:
		case spv::OpVectorInsertDynamic:
		case spv::OpTranspose:
		case spv::OpConvertFToU:
		case spv::OpConvertFToS:
		case spv::OpConvertSToF:
		case spv::OpConvertUToF:
		case spv::OpUConvert:
		case spv::OpSConvert:
		case spv::OpFConvert:
		case spv::OpQuantizeToF16:
		case spv::OpConvertPtrToU:
		case spv::OpConvertUToPtr:
		case spv::OpBitcast:
		case spv::OpSNegate:
		case spv::OpFNegate:
		case spv::OpIAdd:
		case spv::OpFAdd:
		case spv::OpISub:
		case spv::OpFSub:
		case spv::OpIMul:
		case spv::OpFMul:
		case spv::OpUDiv:
		case spv::OpSDiv:
		case spv::OpFDiv:
		case spv::OpUMod:
		case spv::OpSRem:
		case spv::OpSMod:
		case spv::OpFRem:
		case spv::OpFMod:
		case spv::OpVectorTimesScalar:
		case spv::OpMatrixTimesScalar:
		case spv::OpVectorTimesMatrix:
		case spv::OpMatrixTimesVector:
		case spv::OpMatrixTimesMatrix:
		case spv::OpOuterProduct:
		case spv::OpDot:
		case spv::OpIAddCarry:
		case spv::OpISubBorrow:
		case spv::OpUMulExtended:
		case spv::OpSMulExtended:
		case spv::OpShiftRightLogical:
		case spv::OpShiftRightArithmetic:
		case spv::OpShiftLeftLogical:
		case spv::OpBitwiseOr:
		case spv::OpBitwiseXor:
		case spv::OpBitwiseAnd:
		case spv::OpNot:
		case spv::OpBitFieldInsert:
		case spv::OpBitFieldSExtract:
		case spv::OpBitFieldUExtract:
		case spv::OpBitReverse:
		case spv::OpBitCount:
		case spv::OpAny:
		case spv::OpAll:
		case spv::OpIsNan:
		case spv::OpIsInf:
		case spv::OpLogicalEqual:
		case spv::OpLogicalNotEqual:
		case spv::OpLogicalOr:
		case spv::OpLogicalAnd:
		case spv::OpLogicalNot:
		case spv::OpIEqual:
		case spv::OpINotEqual:
		case spv::OpUGreaterThan:
		case spv::OpSGreaterThan:
		case spv::OpUGreaterThanEqual:
		case spv::OpSGreaterThanEqual:
		case spv::OpULessThan:
		case spv::OpSLessThan:
		case spv::OpULessThanEqual:
		case spv::OpSLessThanEqual:
		case spv::OpFOrdEqual:
		case spv::OpFUnordEqual:
		case spv::OpFOrdNotEqual:
		case spv::OpFUnordNotEqual:
		case spv::OpFOrdLessThan:
		case spv::OpFUnordLessThan:
		case spv::OpFOrdGreaterThan:
		case spv::OpFUnordGreaterThan:
		case spv::OpFOrdLessThanEqual:
		case spv::OpFUnordLessThanEqual:
		case spv::OpFOrdGreaterThanEqual:
		case spv::OpFUnordGreaterThanEqual:
		case spv::OpDPdx:
		case spv::OpDPdy:
		case spv::OpFwidth:
		case spv::OpDPdxFine:
		case spv::OpDPdyFine:
		case spv::OpFwidthFine:
		case spv::OpDPdxCoarse:
		case spv::OpDPdyCoarse:
		case spv::OpFwidthCoarse:
		case spv::OpSampledImage:
		case spv::OpImage:
		case spv::OpImageSampleImplicitLod:
		case spv::OpImageSampleExplicitLod:
		case spv::OpImageSampleDrefImplicitLod:
		case spv::OpImageSampleDrefExplicitLod:
		case spv::OpImageFetch:
		case spv::OpImageGather:
		case spv::OpImageDrefGather:
		case spv::OpImageRead:
		case spv::OpImageTexelPointer:
		case spv::OpImageQuerySizeLod:
		case spv::OpImageQuerySize:
		case spv::OpImageQueryLod:
		case spv::OpImageQueryLevels:
		case spv::OpImageQuerySamples:
		case spv::OpAtomicLoad:
		case spv::OpAtomicExchange:
		case spv::OpAtomicCompareExchange:
		case spv::OpAtomicIIncrement:
		case spv::OpAtomicIDecrement:
		case spv::OpAtomicIAdd:
		case spv::OpAtomicISub:
		case spv::OpAtomicSMin:
		case spv::OpAtomicUMin:
		case spv::OpAtomicSMax:
		case spv::OpAtomicUMax:
		case spv::OpAtomicAnd:
		case spv::OpAtomicOr:
		case spv::OpAtomicXor:
		case spv::OpGroupNonUniformElect:
		case spv::OpGroupNonUniformAll:
		case spv::OpGroupNonUniformAny:
		case spv::OpGroupNonUniformAllEqual:
		case spv::OpGroupNonUniformBroadcast:
		case spv::OpGroupNonUniformBroadcastFirst:
		case spv::OpGroupNonUniformBallot:
		case spv::OpGroupNonUniformInverseBallot:
		case spv::OpGroupNonUniformBallotBitExtract:
		case spv::OpGroupNonUniformBallotBitCount:
		case spv::OpGroupNonUniformBallotFindLSB:
		case spv::OpGroupNonUniformBallotFindMSB:
		case spv::OpGroupNonUniformShuffle:
		case spv::OpGroupNonUniformShuffleXor:
		case spv::OpGroupNonUniformShuffleUp:
		case spv::OpGroupNonUniformShuffleDown:
		case spv::OpGroupNonUniformIAdd:
		case spv::OpGroupNonUniformFAdd:
		case spv::OpGroupNonUniformIMul:
		case spv::OpGroupNonUniformFMul:
		case spv::OpGroupNonUniformSMin:
		case spv::OpGroupNonUniformUMin:
		case spv::OpGroupNonUniformFMin:
		case spv::OpGroupNonUniformSMax:
		case spv::OpGroupNonUniformUMax:
		case spv::OpGroupNonUniformFMax:
		case spv::OpGroupNonUniformBitwiseAnd:
		case spv::OpGroupNonUniformBitwiseOr:
		case spv::OpGroupNonUniformBitwiseXor:
		case spv::OpRayQueryProceedKHR:
		case spv::OpRayQueryGetIntersectionTypeKHR:
		case spv::OpRayQueryGetIntersectionTKHR:
		case spv::OpRayQueryGetIntersectionInstanceCustomIndexKHR:
		case spv::OpRayQueryGetIntersectionInstanceIdKHR:
		case spv::OpRayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetKHR:
		case spv::OpRayQueryGetIntersectionGeometryIndexKHR:
		case spv::OpRayQueryGetIntersectionPrimitiveIndexKHR:
		case spv::OpRayQueryGetIntersectionBarycentricsKHR:
		case spv::OpRayQueryGetIntersectionFrontFaceKHR:
		case spv::OpRayQueryGetIntersectionCandidateAABBOpaqueKHR:
		case spv::OpRayQueryGetIntersectionObjectRayDirectionKHR:
		case spv::OpRayQueryGetIntersectionObjectRayOriginKHR:
		case spv::OpRayQueryGetWorldRayDirectionKHR:
		case spv::OpRayQueryGetWorldRayOriginKHR:
		case spv::OpRayQueryGetRayTMinKHR:
		case spv::OpRayQueryGetRayFlagsKHR:
		case spv::OpRayQueryGetIntersectionWorldToObjectKHR:
		case spv::OpRayQueryGetIntersectionObjectToWorldKHR:
		case spv::OpRayQueryGetIntersectionTriangleVertexPositionsKHR: return true;
		default: return false;
	}
}

ParsedInst ParseFunctionInst(const uint32_t* w, size_t n,
                             const std::unordered_map<uint32_t, uint32_t>& scalar_type_bits) {
	ParsedInst info;
	if (n == 0u) return info;
	const auto op = static_cast<spv::Op>(w[0] & spv::OpCodeMask);
	if (!IsRecognizedFunctionBodyOp(op)) return info;
	info.valid = true;

	size_t operand_start = 1u;
	if (op == spv::OpLabel) {
		info.has_result = true;
		info.result_id  = w[1];
		return info;
	}
	if (!IsNonResultOp(op)) {
		if (n < 3u) {
			info.valid = false;
			return info;
		}
		info.has_type   = true;
		info.has_result = true;
		info.type_id    = w[1];
		info.result_id  = w[2];
		operand_start   = 3u;
	}

	auto push_range = [&](size_t begin, size_t end) {
		for (size_t i = begin; i < end && i < n; ++i) info.id_word_indices.push_back(i);
	};

	switch (op) {
		case spv::OpCompositeExtract:
		case spv::OpImageTexelPointer:
			if (op == spv::OpCompositeExtract) {
				if (n > 3u) info.id_word_indices.push_back(3u);
			} else {
				push_range(operand_start, n);
			}
			break;
		case spv::OpCompositeInsert:
		case spv::OpVectorShuffle:
			if (n > 3u) info.id_word_indices.push_back(3u);
			if (n > 4u) info.id_word_indices.push_back(4u);
			break;
		case spv::OpExtInst:
			if (n > 3u) info.id_word_indices.push_back(3u);
			push_range(5u, n);
			break;
		case spv::OpSwitch: {
			if (n < 3u) {
				info.valid = false;
				return info;
			}
			info.id_word_indices.push_back(1u);
			info.id_word_indices.push_back(2u);
			// Only 32-bit selector switches have 2 words per (literal, label) arm.
			if (((n - 3u) % 2u) != 0u) {
				info.valid = false;
				return info;
			}
			for (size_t i = 3u; i + 1u < n; i += 2u) {
				info.id_word_indices.push_back(i + 1u);
			}
			break;
		}
		case spv::OpBranchConditional:
			if (n < 4u) {
				info.valid = false;
				return info;
			}
			info.id_word_indices.push_back(1u);
			info.id_word_indices.push_back(2u);
			info.id_word_indices.push_back(3u);
			break;
		case spv::OpSelectionMerge:
			if (n < 3u) {
				info.valid = false;
				return info;
			}
			info.id_word_indices.push_back(1u);
			break;
		case spv::OpLoopMerge:
			if (n < 4u) {
				info.valid = false;
				return info;
			}
			info.id_word_indices.push_back(1u);
			info.id_word_indices.push_back(2u);
			break;
		case spv::OpLoad:
			if (n > 3u) info.id_word_indices.push_back(3u);
			break;
		case spv::OpStore:
			if (n > 1u) info.id_word_indices.push_back(1u);
			if (n > 2u) info.id_word_indices.push_back(2u);
			break;
		case spv::OpCopyMemory:
			if (n > 1u) info.id_word_indices.push_back(1u);
			if (n > 2u) info.id_word_indices.push_back(2u);
			break;
		case spv::OpCopyMemorySized:
			if (n > 1u) info.id_word_indices.push_back(1u);
			if (n > 2u) info.id_word_indices.push_back(2u);
			if (n > 3u) info.id_word_indices.push_back(3u);
			break;
		case spv::OpLifetimeStart:
		case spv::OpLifetimeStop:
			if (n > 1u) info.id_word_indices.push_back(1u);
			break;
		case spv::OpImageRead:
		case spv::OpImageFetch:
		case spv::OpImageSampleImplicitLod:
		case spv::OpImageSampleExplicitLod:
		case spv::OpImageQuerySizeLod:
			if (n > 3u) info.id_word_indices.push_back(3u);
			if (n > 4u) info.id_word_indices.push_back(4u);
			if (op == spv::OpImageQuerySizeLod) {
				break;
			}
			if (n > 5u) push_range(6u, n);
			break;
		case spv::OpImageSampleDrefImplicitLod:
		case spv::OpImageSampleDrefExplicitLod:
		case spv::OpImageGather:
		case spv::OpImageDrefGather:
			if (n > 3u) info.id_word_indices.push_back(3u);
			if (n > 4u) info.id_word_indices.push_back(4u);
			if (n > 5u) info.id_word_indices.push_back(5u);
			if (n > 6u) push_range(7u, n);
			break;
		case spv::OpImageWrite:
			if (n > 1u) info.id_word_indices.push_back(1u);
			if (n > 2u) info.id_word_indices.push_back(2u);
			if (n > 3u) info.id_word_indices.push_back(3u);
			if (n > 4u) push_range(5u, n);
			break;
		case spv::OpGroupNonUniformBallotBitCount:
		case spv::OpGroupNonUniformIAdd:
		case spv::OpGroupNonUniformFAdd:
		case spv::OpGroupNonUniformIMul:
		case spv::OpGroupNonUniformFMul:
		case spv::OpGroupNonUniformSMin:
		case spv::OpGroupNonUniformUMin:
		case spv::OpGroupNonUniformFMin:
		case spv::OpGroupNonUniformSMax:
		case spv::OpGroupNonUniformUMax:
		case spv::OpGroupNonUniformFMax:
		case spv::OpGroupNonUniformBitwiseAnd:
		case spv::OpGroupNonUniformBitwiseOr:
		case spv::OpGroupNonUniformBitwiseXor:
			if (n > 3u) info.id_word_indices.push_back(3u);
			// word 4 is GroupOperation literal
			push_range(5u, n);
			break;
		case spv::OpRayQueryGetIntersectionTypeKHR:
		case spv::OpRayQueryGetIntersectionTKHR:
		case spv::OpRayQueryGetIntersectionInstanceCustomIndexKHR:
		case spv::OpRayQueryGetIntersectionInstanceIdKHR:
		case spv::OpRayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetKHR:
		case spv::OpRayQueryGetIntersectionGeometryIndexKHR:
		case spv::OpRayQueryGetIntersectionPrimitiveIndexKHR:
		case spv::OpRayQueryGetIntersectionBarycentricsKHR:
		case spv::OpRayQueryGetIntersectionFrontFaceKHR:
		case spv::OpRayQueryGetIntersectionCandidateAABBOpaqueKHR:
		case spv::OpRayQueryGetIntersectionObjectRayDirectionKHR:
		case spv::OpRayQueryGetIntersectionObjectRayOriginKHR:
		case spv::OpRayQueryGetIntersectionWorldToObjectKHR:
		case spv::OpRayQueryGetIntersectionObjectToWorldKHR:
		case spv::OpRayQueryGetIntersectionTriangleVertexPositionsKHR:
			if (n > 3u) info.id_word_indices.push_back(3u);
			if (n > 4u) info.id_word_indices.push_back(4u);
			break;
		case spv::OpArrayLength:
			// word 4 is the member index literal
			if (n > 3u) info.id_word_indices.push_back(3u);
			break;
		case spv::OpVariable:
		case spv::OpUndef: info.valid = false; break;
		default: push_range(operand_start, n); break;
	}
	(void)scalar_type_bits;
	return info;
}

uint32_t MakeHeaderWord(spv::Op op, size_t word_count) {
	return (static_cast<uint32_t>(word_count) << spv::WordCountShift) |
	       static_cast<uint32_t>(op);
}

void AppendInst(std::vector<uint32_t>& out, spv::Op op, std::initializer_list<uint32_t> operands) {
	out.push_back(MakeHeaderWord(op, 1u + operands.size()));
	out.insert(out.end(), operands.begin(), operands.end());
}

void AppendInstVec(std::vector<uint32_t>& out, spv::Op op, const std::vector<uint32_t>& operands) {
	out.push_back(MakeHeaderWord(op, 1u + operands.size()));
	out.insert(out.end(), operands.begin(), operands.end());
}

uint64_t HashRegionKey(const std::vector<uint32_t>& key) {
	uint64_t h = 14695981039346656037ull;
	for (const uint32_t w: key) {
		h ^= w;
		h *= 1099511628211ull;
	}
	return h;
}

} // namespace

void OutlineRepeatedRegions(std::vector<uint32_t>& words, size_t min_module_words) {
	if (words.size() < 5u || words[0] != kSpirvMagic || words.size() < min_module_words) {
		return;
	}

	uint32_t next_id = words[3];

	for (int pass = 0; pass < 2; ++pass) {
		// 1. Parse preamble & functions
		size_t functions_start = words.size();
		size_t main_start      = words.size();
		size_t main_end        = words.size();

		std::unordered_set<uint32_t>                        module_ids;
		std::unordered_set<uint32_t>                        func_storage_ptrs;
		std::unordered_set<uint32_t>                        all_ptr_types;
		std::unordered_map<uint32_t, std::vector<uint32_t>> func_types_by_key;
		std::unordered_map<uint32_t, uint32_t>              scalar_type_bits;
		// Annotations section offsets [ann_start, ann_end)
		size_t ann_start = 5u;
		size_t ann_end   = 5u;
		bool   saw_ann   = false;
		bool   saw_decl  = false;
		size_t decl_end  = 5u;

		struct DecorEntry {
			std::vector<uint32_t> extra_words; // decoration + literal args
		};
		std::unordered_map<uint32_t, std::vector<DecorEntry>> decorations;

		uint32_t entry_func_id = 0u;
		size_t   off           = 5u;
		while (off < words.size()) {
			const uint32_t wc = words[off] >> spv::WordCountShift;
			const auto     op = static_cast<spv::Op>(words[off] & spv::OpCodeMask);
			if (wc == 0u || off + wc > words.size()) return;

			if (functions_start == words.size() && op == spv::OpFunction) {
				functions_start = off;
				decl_end        = off;
				if (!saw_ann) {
					ann_start = off;
					ann_end   = off;
				}
			}
			if (functions_start == words.size()) {
				if (op == spv::OpEntryPoint && wc >= 3u) {
					entry_func_id = words[off + 2u];
				} else if (op == spv::OpDecorate || op == spv::OpMemberDecorate ||
				           op == spv::OpDecorationGroup || op == spv::OpGroupDecorate ||
				           op == spv::OpGroupMemberDecorate) {
					if (!saw_ann) {
						ann_start = off;
						saw_ann   = true;
					}
					ann_end = off + wc;
					if (op == spv::OpDecorate && wc >= 3u) {
						DecorEntry e;
						e.extra_words.assign(words.begin() + static_cast<ptrdiff_t>(off + 2u),
						                     words.begin() + static_cast<ptrdiff_t>(off + wc));
						decorations[words[off + 1u]].push_back(std::move(e));
					}
				} else if (op == spv::OpExtInstImport || op == spv::OpString) {
					module_ids.insert(words[off + 1u]);
				} else if (op == spv::OpTypeVoid || op == spv::OpTypeBool || op == spv::OpTypeInt ||
				           op == spv::OpTypeFloat || op == spv::OpTypeVector ||
				           op == spv::OpTypeMatrix || op == spv::OpTypeImage ||
				           op == spv::OpTypeSampler || op == spv::OpTypeSampledImage ||
				           op == spv::OpTypeArray || op == spv::OpTypeRuntimeArray ||
				           op == spv::OpTypeStruct || op == spv::OpTypeOpaque ||
				           op == spv::OpTypePointer || op == spv::OpTypeFunction ||
				           op == spv::OpTypeForwardPointer || op == spv::OpConstantTrue ||
				           op == spv::OpConstantFalse || op == spv::OpConstant ||
				           op == spv::OpConstantComposite || op == spv::OpConstantNull ||
				           op == spv::OpSpecConstantTrue || op == spv::OpSpecConstantFalse ||
				           op == spv::OpSpecConstant || op == spv::OpSpecConstantComposite ||
				           op == spv::OpSpecConstantOp || op == spv::OpVariable ||
				           op == spv::OpUndef) {
					if (!saw_decl) {
						if (!saw_ann) {
							ann_start = off;
							ann_end   = off;
						}
						saw_decl = true;
					}
					const uint32_t res_id =
					    (op == spv::OpTypeVoid || op == spv::OpTypeBool || op == spv::OpTypeInt ||
					     op == spv::OpTypeFloat || op == spv::OpTypeVector ||
					     op == spv::OpTypeMatrix || op == spv::OpTypeImage ||
					     op == spv::OpTypeSampler || op == spv::OpTypeSampledImage ||
					     op == spv::OpTypeArray || op == spv::OpTypeRuntimeArray ||
					     op == spv::OpTypeStruct || op == spv::OpTypeOpaque ||
					     op == spv::OpTypePointer || op == spv::OpTypeFunction)
					        ? words[off + 1u]
					        : (op == spv::OpTypeForwardPointer ? words[off + 1u] : words[off + 2u]);
					module_ids.insert(res_id);
					if (op == spv::OpTypePointer && wc >= 4u &&
					    words[off + 2u] == static_cast<uint32_t>(spv::StorageClassFunction)) {
						func_storage_ptrs.insert(res_id);
					}
					if (op == spv::OpTypePointer) all_ptr_types.insert(res_id);
				}
			} else {
				if (op == spv::OpFunction && wc >= 5u) {
					module_ids.insert(words[off + 2u]);
					if (words[off + 2u] == entry_func_id) {
						main_start = off;
					}
				} else if (op == spv::OpFunctionEnd && main_start < words.size() &&
				           main_end == words.size()) {
					main_end = off + wc;
				}
			}
			off += wc;
		}
		if (main_start >= main_end || entry_func_id == 0u) break;

		// 2. Parse `main` into header + basic blocks
		struct Block {
			uint32_t                  label_id = 0u;
			std::vector<InstSlice>    insts; // insts[0] is OpLabel
			std::vector<ParsedInst>   parsed;
			std::vector<size_t>       succs;
			std::vector<size_t>       preds;
			size_t                    word_count = 0u;
		};
		std::vector<InstSlice> main_header;
		std::vector<Block>     blocks;
		std::unordered_map<uint32_t, size_t> label_to_block;

		off = main_start;
		bool in_header = true;
		while (off < main_end) {
			const uint32_t wc = words[off] >> spv::WordCountShift;
			const auto     op = static_cast<spv::Op>(words[off] & spv::OpCodeMask);
			InstSlice      slice;
			slice.op = op;
			slice.words.assign(words.begin() + static_cast<ptrdiff_t>(off),
			                   words.begin() + static_cast<ptrdiff_t>(off + wc));
			if (op == spv::OpLabel) {
				in_header = false;
				Block b;
				b.label_id = slice.words[1];
				label_to_block.emplace(b.label_id, blocks.size());
				b.parsed.push_back(ParseFunctionInst(slice.words.data(), slice.words.size(),
				                                     scalar_type_bits));
				b.word_count += slice.words.size();
				b.insts.push_back(std::move(slice));
				blocks.push_back(std::move(b));
			} else if (op == spv::OpFunctionEnd) {
				break;
			} else if (in_header) {
				if (op == spv::OpFunctionParameter && wc >= 3u) {
					module_ids.insert(slice.words[2]);
				}
				main_header.push_back(std::move(slice));
			} else {
				auto& b = blocks.back();
				b.parsed.push_back(ParseFunctionInst(slice.words.data(), slice.words.size(),
				                                     scalar_type_bits));
				b.word_count += slice.words.size();
				if (op == spv::OpVariable && wc >= 4u &&
				    words[off + 3u] == static_cast<uint32_t>(spv::StorageClassFunction)) {
					func_storage_ptrs.insert(words[off + 2u]);
				}
				b.insts.push_back(std::move(slice));
			}
			off += wc;
		}
		if (blocks.size() < 4u) break;

		// Build CFG edges and definition/use maps across `main`
		struct DefSite {
			size_t block_idx = 0u;
			size_t inst_idx  = 0u;
			uint32_t type_id = 0u;
		};
		std::unordered_map<uint32_t, DefSite>            def_site;
		std::unordered_map<uint32_t, std::vector<size_t>> use_blocks;

		for (size_t bi = 0u; bi < blocks.size(); ++bi) {
			auto& b = blocks[bi];
			for (size_t ii = 0u; ii < b.insts.size(); ++ii) {
				const auto& p = b.parsed[ii];
				if (p.has_result) {
					def_site[p.result_id] = {bi, ii, p.type_id};
				}
				for (const size_t widx: p.id_word_indices) {
					const uint32_t id = b.insts[ii].words[widx];
					if (module_ids.count(id) == 0u && label_to_block.count(id) == 0u) {
						use_blocks[id].push_back(bi);
					}
				}
			}
			if (!b.insts.empty()) {
				const auto& term = b.insts.back();
				auto        add_edge = [&](uint32_t lbl) {
                    const auto it = label_to_block.find(lbl);
                    if (it != label_to_block.end()) {
                        b.succs.push_back(it->second);
                        blocks[it->second].preds.push_back(bi);
                    }
                };
				if (term.op == spv::OpBranch && term.words.size() >= 2u) {
					add_edge(term.words[1]);
				} else if (term.op == spv::OpBranchConditional && term.words.size() >= 4u) {
					add_edge(term.words[2]);
					add_edge(term.words[3]);
				} else if (term.op == spv::OpSwitch && term.words.size() >= 3u) {
					add_edge(term.words[2]);
					for (size_t i = 3u; i + 1u < term.words.size(); i += 2u) {
						add_edge(term.words[i + 1u]);
					}
				}
			}
		}

		// 3. Discover SESE selection/switch regions
		struct Candidate {
			size_t                header_block = 0u;
			size_t                merge_block  = 0u;
			std::vector<size_t>   interior_blocks; // BFS order
			std::vector<size_t>   merge_phi_indices;
			std::vector<uint32_t> inputs;
			std::vector<uint32_t> outputs;      // SSA IDs defined in region or merge Phis
			std::vector<uint32_t> output_types; // Type IDs for outputs
			size_t                region_words = 0u;
			std::vector<uint32_t> canonical_key;
			uint64_t              hash = 0u;
		};

		std::vector<Candidate> candidates;
		for (size_t hi = 0u; hi < blocks.size(); ++hi) {
			const auto& H = blocks[hi];
			if (H.insts.size() < 3u) continue;
			const auto& merge_inst = H.insts[H.insts.size() - 2u];
			const auto& term_inst  = H.insts.back();
			if (merge_inst.op != spv::OpSelectionMerge) continue;
			if (term_inst.op != spv::OpSwitch && term_inst.op != spv::OpBranchConditional) continue;
			if (!H.parsed[H.insts.size() - 2u].valid || !H.parsed.back().valid) continue;

			const auto mit = label_to_block.find(merge_inst.words[1]);
			if (mit == label_to_block.end()) continue;
			const size_t mi = mit->second;
			if (mi == hi) continue;

			// Collect interior blocks between H and M
			std::unordered_set<size_t> interior_set;
			std::vector<size_t>        interior;
			std::queue<size_t>         q;
			for (const size_t s: H.succs) {
				if (s != mi && interior_set.insert(s).second) {
					interior.push_back(s);
					q.push(s);
				}
			}
			if (interior.empty()) continue;

			bool sese_ok = true;
			while (!q.empty() && sese_ok) {
				const size_t b = q.front();
				q.pop();
				if (b == hi) {
					sese_ok = false;
					break;
				}
				const auto& blk = blocks[b];
				if (blk.succs.empty()) {
					// OpReturn / OpKill / OpUnreachable inside region -> cannot outline
					sese_ok = false;
					break;
				}
				for (const size_t s: blk.succs) {
					if (s == mi) continue;
					if (s == hi) {
						sese_ok = false;
						break;
					}
					if (interior_set.insert(s).second) {
						interior.push_back(s);
						q.push(s);
					}
				}
			}
			if (!sese_ok) continue;

			// Verify every interior block only has predecessors in {H} U interior
			for (const size_t b: interior) {
				for (const size_t p: blocks[b].preds) {
					if (p != hi && interior_set.count(p) == 0u) {
						sese_ok = false;
						break;
					}
				}
				if (!sese_ok) break;
			}
			if (!sese_ok) continue;

			// Verify merge block M only has predecessors in {H} U interior
			for (const size_t p: blocks[mi].preds) {
				if (p != hi && interior_set.count(p) == 0u) {
					sese_ok = false;
					break;
				}
			}
			if (!sese_ok) continue;

			// Verify all instructions in interior + M's Phis are recognized and use no Function ptrs
			size_t total_words = merge_inst.words.size() + term_inst.words.size();
			std::unordered_set<uint32_t> region_defs;
			for (const size_t b: interior) {
				total_words += blocks[b].word_count;
				for (size_t ii = 0u; ii < blocks[b].insts.size(); ++ii) {
					const auto& p = blocks[b].parsed[ii];
					if (!p.valid) {
						sese_ok = false;
						break;
					}
					if (p.has_result) {
						if (func_storage_ptrs.count(p.type_id) != 0u) {
							sese_ok = false;
							break;
						}
						region_defs.insert(p.result_id);
					}
					for (const size_t widx: p.id_word_indices) {
						const uint32_t id = blocks[b].insts[ii].words[widx];
						if (func_storage_ptrs.count(id) != 0u) {
							sese_ok = false;
							break;
						}
					}
					if (!sese_ok) break;
				}
				if (!sese_ok) break;
			}
			if (!sese_ok || total_words < kMinRegionWords) continue;

			std::vector<size_t> merge_phis;
			for (size_t ii = 1u; ii < blocks[mi].insts.size(); ++ii) {
				if (blocks[mi].insts[ii].op != spv::OpPhi) break;
				if (!blocks[mi].parsed[ii].valid) {
					sese_ok = false;
					break;
				}
				merge_phis.push_back(ii);
				region_defs.insert(blocks[mi].parsed[ii].result_id);
			}
			if (!sese_ok) continue;

			// Compute inputs & outputs
			std::vector<uint32_t>        inputs;
			std::unordered_set<uint32_t> input_set;
			auto record_use = [&](uint32_t id) {
				if (id == 0u) {
					sese_ok = false;
					return;
				}
				if (module_ids.count(id) != 0u || region_defs.count(id) != 0u) return;
				if (id == H.label_id || id == blocks[mi].label_id) return;
				if (label_to_block.count(id) != 0u) return;
				if (func_storage_ptrs.count(id) != 0u) {
					sese_ok = false;
					return;
				}
				// Passing a pointer needs the variable-pointers capability: keep those inline.
				if (const auto dsit = def_site.find(id);
				    dsit != def_site.end() && all_ptr_types.count(dsit->second.type_id) != 0u) {
					sese_ok = false;
					return;
				}
				if (input_set.insert(id).second) inputs.push_back(id);
			};
			for (const size_t widx: H.parsed.back().id_word_indices) {
				record_use(term_inst.words[widx]);
			}
			for (const size_t b: interior) {
				for (size_t ii = 1u; ii < blocks[b].insts.size(); ++ii) {
					for (const size_t widx: blocks[b].parsed[ii].id_word_indices) {
						record_use(blocks[b].insts[ii].words[widx]);
					}
				}
			}
			for (const size_t ii: merge_phis) {
				for (const size_t widx: blocks[mi].parsed[ii].id_word_indices) {
					record_use(blocks[mi].insts[ii].words[widx]);
				}
			}
			if (!sese_ok || inputs.size() > kMaxInputs) continue;

			std::vector<uint32_t> outputs;
			std::vector<uint32_t> output_types;
			auto                  is_used_outside = [&](uint32_t id) {
                const auto it = use_blocks.find(id);
                if (it == use_blocks.end()) return false;
                for (const size_t ub: it->second) {
                    if (interior_set.count(ub) != 0u) continue;
                    if (ub == mi) {
                        // Used in M: check if used ONLY in merge_phis or also after merge_phis
                        bool used_after_phis = false;
                        for (size_t ii = 1u + merge_phis.size(); ii < blocks[mi].insts.size(); ++ii) {
                            for (const size_t widx: blocks[mi].parsed[ii].id_word_indices) {
                                if (blocks[mi].insts[ii].words[widx] == id) {
                                    used_after_phis = true;
                                    break;
                                }
                            }
                            if (used_after_phis) break;
                        }
                        if (!used_after_phis) continue;
                    }
                    return true;
                }
                return false;
            };
			// Preserve deterministic order of outputs: interior blocks then merge phis
			for (const size_t b: interior) {
				for (size_t ii = 1u; ii < blocks[b].insts.size(); ++ii) {
					const auto& p = blocks[b].parsed[ii];
					if (p.has_result && is_used_outside(p.result_id)) {
						outputs.push_back(p.result_id);
						output_types.push_back(p.type_id);
					}
				}
			}
			for (const size_t ii: merge_phis) {
				const auto& p = blocks[mi].parsed[ii];
				if (is_used_outside(p.result_id)) {
					outputs.push_back(p.result_id);
					output_types.push_back(p.type_id);
				}
			}
			if (outputs.size() > kMaxOutputs) continue;
			if (std::ranges::any_of(output_types, [&](uint32_t t) { return all_ptr_types.count(t) != 0u; })) {
				continue;
			}

			// Build canonical key
			std::unordered_map<uint32_t, uint32_t> norm_id;
			uint32_t next_norm = 0x80000000u;
			auto     get_norm  = [&](uint32_t id) -> uint32_t {
                if (module_ids.count(id) != 0u) return id;
                auto [it, ins] = norm_id.emplace(id, next_norm);
                if (ins) ++next_norm;
                return it->second;
            };
			for (const uint32_t in_id: inputs) (void)get_norm(in_id);
			(void)get_norm(H.label_id);
			(void)get_norm(blocks[mi].label_id);

			std::vector<uint32_t> key;
			key.push_back(static_cast<uint32_t>(inputs.size()));
			for (const uint32_t in_id: inputs) {
				const auto dit = def_site.find(in_id);
				key.push_back(dit != def_site.end() ? dit->second.type_id : 0u);
			}
			key.push_back(static_cast<uint32_t>(outputs.size()));
			for (const uint32_t ot: output_types) key.push_back(ot);

			auto append_norm_inst = [&](const InstSlice& s, const ParsedInst& p) {
				std::vector<uint32_t> nw = s.words;
				if (p.has_result) nw[p.has_type ? 2u : 1u] = get_norm(p.result_id);
				for (const size_t widx: p.id_word_indices) nw[widx] = get_norm(nw[widx]);
				key.insert(key.end(), nw.begin(), nw.end());
				if (p.has_result) {
					if (const auto dit = decorations.find(p.result_id); dit != decorations.end()) {
						key.push_back(0xDEADBEEFu);
						key.push_back(static_cast<uint32_t>(dit->second.size()));
						for (const auto& d: dit->second) {
							key.push_back(static_cast<uint32_t>(d.extra_words.size()));
							key.insert(key.end(), d.extra_words.begin(), d.extra_words.end());
						}
					}
				}
			};
			append_norm_inst(merge_inst, H.parsed[H.insts.size() - 2u]);
			append_norm_inst(term_inst, H.parsed.back());
			for (const size_t b: interior) {
				for (size_t ii = 0u; ii < blocks[b].insts.size(); ++ii) {
					append_norm_inst(blocks[b].insts[ii], blocks[b].parsed[ii]);
				}
			}
			for (const size_t ii: merge_phis) {
				append_norm_inst(blocks[mi].insts[ii], blocks[mi].parsed[ii]);
			}
			for (const uint32_t out_id: outputs) key.push_back(get_norm(out_id));

			Candidate cand;
			cand.header_block      = hi;
			cand.merge_block       = mi;
			cand.interior_blocks   = std::move(interior);
			cand.merge_phi_indices = std::move(merge_phis);
			cand.inputs            = std::move(inputs);
			cand.outputs           = std::move(outputs);
			cand.output_types      = std::move(output_types);
			cand.region_words      = total_words;
			cand.hash              = HashRegionKey(key);
			cand.canonical_key     = std::move(key);
			candidates.push_back(std::move(cand));
		}
		if (candidates.size() < 2u) break;

		// 4. Group identical candidates by canonical key
		std::unordered_map<uint64_t, std::vector<size_t>> buckets;
		for (size_t i = 0u; i < candidates.size(); ++i) {
			buckets[candidates[i].hash].push_back(i);
		}
		std::vector<std::vector<size_t>> groups;
		for (auto& [h, idxs]: buckets) {
			std::vector<bool> used(idxs.size(), false);
			for (size_t a = 0u; a < idxs.size(); ++a) {
				if (used[a]) continue;
				std::vector<size_t> grp {idxs[a]};
				used[a] = true;
				for (size_t b = a + 1u; b < idxs.size(); ++b) {
					if (!used[b] &&
					    candidates[idxs[a]].canonical_key == candidates[idxs[b]].canonical_key) {
						grp.push_back(idxs[b]);
						used[b] = true;
					}
				}
				if (grp.size() >= 2u) groups.push_back(std::move(grp));
			}
		}
		if (groups.empty()) break;

		std::sort(groups.begin(), groups.end(), [&](const auto& a, const auto& b) {
			const size_t sa = (a.size() - 1u) * candidates[a[0]].region_words;
			const size_t sb = (b.size() - 1u) * candidates[b[0]].region_words;
			return sa > sb;
		});

		// Select non-overlapping instances
		std::vector<bool>                       block_taken(blocks.size(), false);
		std::vector<std::vector<size_t>>        chosen_groups;
		for (const auto& grp: groups) {
			std::vector<size_t> valid_instances;
			for (const size_t ci: grp) {
				const auto& c = candidates[ci];
				bool        conflict = block_taken[c.header_block] || block_taken[c.merge_block];
				for (const size_t bi: c.interior_blocks) {
					if (block_taken[bi]) {
						conflict = true;
						break;
					}
				}
				if (!conflict) {
					valid_instances.push_back(ci);
				}
			}
			if (valid_instances.size() >= 2u) {
				for (const size_t ci: valid_instances) {
					const auto& c = candidates[ci];
					for (const size_t bi: c.interior_blocks) block_taken[bi] = true;
				}
				chosen_groups.push_back(std::move(valid_instances));
			}
		}
		if (chosen_groups.empty()) break;

		// 5. Emit outlined functions, new types/annotations, and rewrite `main`
		uint32_t void_type_id = 0u;
		std::map<std::vector<uint32_t>, uint32_t> function_types;
		for (size_t o = 5u; o < functions_start;) {
			const uint32_t wc = words[o] >> spv::WordCountShift;
			const auto     op = static_cast<spv::Op>(words[o] & spv::OpCodeMask);
			if (op == spv::OpTypeVoid && wc >= 2u) void_type_id = words[o + 1u];
			if (op == spv::OpTypeFunction && wc >= 3u) {
				function_types.emplace(
				    std::vector<uint32_t>(words.begin() + static_cast<ptrdiff_t>(o + 2u),
				                          words.begin() + static_cast<ptrdiff_t>(o + wc)),
				    words[o + 1u]);
			}
			o += wc;
		}

		std::vector<uint32_t> new_types;
		std::vector<uint32_t> new_annots;
		std::vector<uint32_t> new_funcs;
		std::unordered_set<uint32_t>                removed_ids;
		std::unordered_map<uint32_t, uint32_t>      id_replacements;
		std::vector<bool>                           drop_block(blocks.size(), false);

		auto ensure_void_type = [&]() {
			if (void_type_id == 0u) {
				void_type_id = next_id++;
				AppendInst(new_types, spv::OpTypeVoid, {void_type_id});
			}
			return void_type_id;
		};

		for (const auto& grp: chosen_groups) {
			const auto& rep = candidates[grp[0]];
			uint32_t    ret_type = 0u;
			if (rep.outputs.empty()) {
				ret_type = ensure_void_type();
			} else if (rep.outputs.size() == 1u) {
				ret_type = rep.output_types[0];
			} else {
				ret_type = next_id++;
				std::vector<uint32_t> ops {ret_type};
				ops.insert(ops.end(), rep.output_types.begin(), rep.output_types.end());
				AppendInstVec(new_types, spv::OpTypeStruct, ops);
			}

			// Function types must be unique in a module: reuse an existing identical one.
			std::vector<uint32_t> fn_sig {ret_type};
			for (const uint32_t in_id: rep.inputs) {
				const auto dit = def_site.find(in_id);
				fn_sig.push_back(dit != def_site.end() ? dit->second.type_id : 0u);
			}
			uint32_t fn_type = 0u;
			if (const auto known = function_types.find(fn_sig); known != function_types.end()) {
				fn_type = known->second;
			} else {
				fn_type = next_id++;
				std::vector<uint32_t> ops {fn_type};
				ops.insert(ops.end(), fn_sig.begin(), fn_sig.end());
				AppendInstVec(new_types, spv::OpTypeFunction, ops);
				function_types.emplace(std::move(fn_sig), fn_type);
			}

			const uint32_t fn_id = next_id++;
			AppendInst(new_funcs, spv::OpFunction,
			           {ret_type, fn_id, static_cast<uint32_t>(spv::FunctionControlDontInlineMask),
			            fn_type});
			std::unordered_map<uint32_t, uint32_t> clone_map;
			for (const uint32_t in_id: rep.inputs) {
				const uint32_t pid = next_id++;
				const auto     dit = def_site.find(in_id);
				AppendInst(new_funcs, spv::OpFunctionParameter,
				           {dit != def_site.end() ? dit->second.type_id : 0u, pid});
				clone_map[in_id] = pid;
			}
			const uint32_t fn_entry = next_id++;
			const uint32_t fn_merge = next_id++;
			clone_map[blocks[rep.header_block].label_id] = fn_entry;
			clone_map[blocks[rep.merge_block].label_id]  = fn_merge;

			auto map_clone = [&](uint32_t id) {
				if (module_ids.count(id) != 0u) return id;
				auto [it, ins] = clone_map.emplace(id, 0u);
				if (ins) it->second = next_id++;
				return it->second;
			};
			auto emit_cloned_inst = [&](const InstSlice& s, const ParsedInst& p) {
				std::vector<uint32_t> cw = s.words;
				if (p.has_result) {
					const uint32_t new_res = map_clone(p.result_id);
					cw[p.has_type ? 2u : 1u] = new_res;
					if (const auto dit = decorations.find(p.result_id); dit != decorations.end()) {
						for (const auto& d: dit->second) {
							std::vector<uint32_t> ops {new_res};
							ops.insert(ops.end(), d.extra_words.begin(), d.extra_words.end());
							AppendInstVec(new_annots, spv::OpDecorate, ops);
						}
					}
				}
				for (const size_t widx: p.id_word_indices) cw[widx] = map_clone(cw[widx]);
				new_funcs.insert(new_funcs.end(), cw.begin(), cw.end());
			};

			AppendInst(new_funcs, spv::OpLabel, {fn_entry});
			const auto& rep_H = blocks[rep.header_block];
			emit_cloned_inst(rep_H.insts[rep_H.insts.size() - 2u],
			                 rep_H.parsed[rep_H.insts.size() - 2u]);
			emit_cloned_inst(rep_H.insts.back(), rep_H.parsed.back());
			for (const size_t bi: rep.interior_blocks) {
				for (size_t ii = 0u; ii < blocks[bi].insts.size(); ++ii) {
					emit_cloned_inst(blocks[bi].insts[ii], blocks[bi].parsed[ii]);
				}
			}
			AppendInst(new_funcs, spv::OpLabel, {fn_merge});
			for (const size_t ii: rep.merge_phi_indices) {
				emit_cloned_inst(blocks[rep.merge_block].insts[ii],
				                 blocks[rep.merge_block].parsed[ii]);
			}
			if (rep.outputs.empty()) {
				AppendInst(new_funcs, spv::OpReturn, {});
			} else if (rep.outputs.size() == 1u) {
				AppendInst(new_funcs, spv::OpReturnValue, {map_clone(rep.outputs[0])});
			} else {
				const uint32_t agg = next_id++;
				std::vector<uint32_t> ops {ret_type, agg};
				for (const uint32_t oid: rep.outputs) ops.push_back(map_clone(oid));
				AppendInstVec(new_funcs, spv::OpCompositeConstruct, ops);
				AppendInst(new_funcs, spv::OpReturnValue, {agg});
			}
			AppendInst(new_funcs, spv::OpFunctionEnd, {});

			// Rewrite each call site in `main`
			for (const size_t ci: grp) {
				const auto& c = candidates[ci];
				auto&       H = blocks[c.header_block];
				auto&       M = blocks[c.merge_block];

				// Pop OpSelectionMerge and terminator from H
				H.insts.pop_back();
				H.parsed.pop_back();
				H.insts.pop_back();
				H.parsed.pop_back();

				const uint32_t call_res = next_id++;
				{
					InstSlice call;
					call.op = spv::OpFunctionCall;
					std::vector<uint32_t> ops {ret_type, call_res, fn_id};
					ops.insert(ops.end(), c.inputs.begin(), c.inputs.end());
					AppendInstVec(call.words, spv::OpFunctionCall, ops);
					ParsedInst call_parsed;
					call_parsed.valid      = true;
					call_parsed.has_type   = true;
					call_parsed.has_result = true;
					call_parsed.type_id    = ret_type;
					call_parsed.result_id  = call_res;
					for (size_t ii = 0u; ii < c.inputs.size(); ++ii) {
						call_parsed.id_word_indices.push_back(4u + ii);
					}
					H.insts.push_back(std::move(call));
					H.parsed.push_back(std::move(call_parsed));
				}
				if (c.outputs.size() == 1u) {
					id_replacements[c.outputs[0]] = call_res;
				} else if (c.outputs.size() > 1u) {
					for (size_t oi = 0u; oi < c.outputs.size(); ++oi) {
						const uint32_t ext = next_id++;
						InstSlice      ex;
						ex.op = spv::OpCompositeExtract;
						AppendInst(ex.words, spv::OpCompositeExtract,
						           {c.output_types[oi], ext, call_res, static_cast<uint32_t>(oi)});
						H.insts.push_back(std::move(ex));
						H.parsed.push_back(ParsedInst{});
						id_replacements[c.outputs[oi]] = ext;
					}
				}
				{
					InstSlice br;
					br.op = spv::OpBranch;
					AppendInst(br.words, spv::OpBranch, {M.label_id});
					H.insts.push_back(std::move(br));
					H.parsed.push_back(ParsedInst{});
				}

				for (const size_t bi: c.interior_blocks) {
					drop_block[bi] = true;
					for (const auto& p: blocks[bi].parsed) {
						if (p.has_result) removed_ids.insert(p.result_id);
					}
				}
				for (const size_t ii: c.merge_phi_indices) {
					if (M.parsed[ii].has_result) removed_ids.insert(M.parsed[ii].result_id);
				}
				if (!c.merge_phi_indices.empty()) {
					const size_t phi_cnt = c.merge_phi_indices.size();
					M.insts.erase(M.insts.begin() + 1,
					              M.insts.begin() + static_cast<ptrdiff_t>(1u + phi_cnt));
					M.parsed.erase(M.parsed.begin() + 1,
					               M.parsed.begin() + static_cast<ptrdiff_t>(1u + phi_cnt));
				}
			}
		}

		// Apply id_replacements to remaining instructions in `main`
		if (!id_replacements.empty()) {
			auto resolve_rep = [&](uint32_t id) {
				auto it = id_replacements.find(id);
				while (it != id_replacements.end()) {
					id = it->second;
					it = id_replacements.find(id);
				}
				return id;
			};
			for (size_t bi = 0u; bi < blocks.size(); ++bi) {
				if (drop_block[bi]) continue;
				for (size_t ii = 0u; ii < blocks[bi].insts.size(); ++ii) {
					for (const size_t widx: blocks[bi].parsed[ii].id_word_indices) {
						blocks[bi].insts[ii].words[widx] =
						    resolve_rep(blocks[bi].insts[ii].words[widx]);
					}
				}
			}
		}

		// Rebuild module `words`
		std::vector<uint32_t> rebuilt;
		rebuilt.reserve(words.size());
		rebuilt.insert(rebuilt.end(), words.begin(), words.begin() + 5);
		rebuilt[3] = next_id;

		// Preamble up to ann_start
		rebuilt.insert(rebuilt.end(), words.begin() + 5,
		               words.begin() + static_cast<ptrdiff_t>(ann_start));
		// Annotations (filtering out removed_ids)
		for (size_t o = ann_start; o < ann_end;) {
			const uint32_t wc = words[o] >> spv::WordCountShift;
			const auto     op = static_cast<spv::Op>(words[o] & spv::OpCodeMask);
			if (op == spv::OpDecorate && wc >= 2u && removed_ids.count(words[o + 1u]) != 0u) {
				o += wc;
				continue;
			}
			rebuilt.insert(rebuilt.end(), words.begin() + static_cast<ptrdiff_t>(o),
			               words.begin() + static_cast<ptrdiff_t>(o + wc));
			o += wc;
		}
		rebuilt.insert(rebuilt.end(), new_annots.begin(), new_annots.end());
		// Types / constants / globals
		rebuilt.insert(rebuilt.end(), words.begin() + static_cast<ptrdiff_t>(ann_end),
		               words.begin() + static_cast<ptrdiff_t>(decl_end));
		rebuilt.insert(rebuilt.end(), new_types.begin(), new_types.end());
		// Existing functions before main
		rebuilt.insert(rebuilt.end(), words.begin() + static_cast<ptrdiff_t>(functions_start),
		               words.begin() + static_cast<ptrdiff_t>(main_start));
		// Outlined helper functions
		rebuilt.insert(rebuilt.end(), new_funcs.begin(), new_funcs.end());
		// Rewritten main
		for (const auto& s: main_header) {
			rebuilt.insert(rebuilt.end(), s.words.begin(), s.words.end());
		}
		for (size_t bi = 0u; bi < blocks.size(); ++bi) {
			if (drop_block[bi]) continue;
			for (const auto& s: blocks[bi].insts) {
				rebuilt.insert(rebuilt.end(), s.words.begin(), s.words.end());
			}
		}
		AppendInst(rebuilt, spv::OpFunctionEnd, {});
		// Any functions after main
		rebuilt.insert(rebuilt.end(), words.begin() + static_cast<ptrdiff_t>(main_end),
		               words.end());

		words = std::move(rebuilt);
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv
