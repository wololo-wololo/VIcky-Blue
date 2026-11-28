#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATION_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATION_H_

#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

// Canonical module-affecting resource state. Runtime addresses and descriptor payloads remain in
// ResourceSnapshot and therefore do not create shader permutations.
struct ResourceSpecialization {
	struct Buffer {
		uint32_t               packed_stride                   = 0;
		Prospero::BufferFormat descriptor_format               = Prospero::BufferFormat::kInvalid;
		uint32_t               descriptor_swizzle              = DstSel(4, 5, 6, 7);
		bool                   zero_stride_oob                 = false;
		bool                   scalar_empty                    = false;
		uint32_t               indirect_root                   = BufferResource::NoIndirectBuffer;
		uint32_t               indirect_mapping_offset         = 0;
		uint32_t               indirect_search_iterations      = 0;
		// A large table's candidates take their layout from runtime words, not from these
		// fields, so the module does not change with the table's rows.
		bool                   runtime_layout                  = false;
		bool                   operator==(const Buffer&) const = default;
	};

	// A layout that the candidates of a runtime-layout table can select at run time. The list
	// only grows while a program lives, so a layout coming and going does not recompile.
	struct BufferLayoutClass {
		uint32_t               root    = 0;
		Prospero::BufferFormat format  = Prospero::BufferFormat::kInvalid;
		uint32_t               swizzle = DstSel(4, 5, 6, 7);
		// Stride 0: a null (or all-out-of-bounds formatted) candidate. NormalizeIndirectBuffer
		// rejects mapped indexed candidates with stride 0, so no addressable buffer lands here.
		bool                   empty   = false;
		auto                   operator<=>(const BufferLayoutClass&) const = default;
	};

	struct Image {
		Prospero::TextureNumericClass numeric_class = Prospero::TextureNumericClass::Unsupported;
		Decoder::ImageDimension       dimension     = Decoder::ImageDimension::Unknown;
		uint32_t                      mip_count     = 1;
		Prospero::BufferFormat        conversion_format          = Prospero::BufferFormat::kInvalid;
		uint32_t                      shader_swizzle             = ShaderImageIdentitySwizzle;
		Prospero::BufferFormat        srgb_decode                = Prospero::BufferFormat::kInvalid;
		uint32_t                      indirect_root              = ImageResource::NoIndirectImage;
		uint32_t                      indirect_mapping_offset    = 0;
		uint32_t                      indirect_search_iterations = 0;
		bool                          cube                       = false;
		bool                          skip_degamma               = false;
		bool                          fmask                      = false;
		bool                          heap                       = false;
		uint8_t                       atomic_bits                = 0;
		bool                          atomic_signed              = false;
		bool                          operator==(const Image&) const = default;
	};

	struct Sampler {
		uint32_t indirect_root = SamplerResource::NoIndirectSampler;
		uint32_t indirect_mapping_offset = 0;
		uint32_t indirect_search_iterations = 0;
		bool operator==(const Sampler&) const = default;
	};

	std::vector<Buffer> buffers;
	std::vector<Image>  images;
	std::vector<Sampler> samplers;
	// Sorted by root, then layout; a candidate's runtime word holds its index among its root's.
	std::vector<BufferLayoutClass> buffer_layout_classes;

	bool operator==(const ResourceSpecialization&) const = default;
};

// A runtime-layout table has at least this many candidate slots (null descriptors pad it).
inline constexpr uint32_t RuntimeLayoutMinSlots = 64u;

// GPU-selected buffer tables with at least this many distinct candidates take their candidates'
// layouts from runtime words (ResourceSpecialization::Buffer::runtime_layout).
uint32_t RuntimeLayoutMinCandidates();
void     SetRuntimeLayoutMinCandidatesForTests(uint32_t candidates);
// The threshold for plan: its own (ResourcePlan::runtime_layout_min_candidates) or the global one.
uint32_t RuntimeLayoutMinCandidates(const ResourcePlan& plan);

// Extracts the descriptor/SRT value graph before resource specialization. The returned plan owns
// its values and is independent of the translated shader CFG.
ResourcePlan ExtractResourcePlan(const Program& program);

// Shader cache record of a plan; loading it into a default-constructed plan rebuilds the walker
// and scratch state like extraction. A failed load leaves a plan that must not be used.
std::vector<uint8_t> SaveResourcePlan(const ResourcePlan& plan);
bool                 LoadResourcePlan(std::span<const uint8_t> bytes, ResourcePlan& plan);
// Takes the lowered threshold and the larger slots of another record of the same program, so a
// program loaded from several store records keeps what it learned in the last run.
void                 MergeRuntimeLayoutState(ResourcePlan& plan, const ResourcePlan& other);

// Refreshes cached resources and specialization in place. A failed refresh must not be used.
bool MaterializeResources(const ResourcePlan& program, const SrtRuntime& runtime,
                          ResourceSnapshot& snapshot, ResourceSpecialization& specialization);

// Applies an already-derived specialization to native IR before layout and emission.
void ApplyResourceSpecialization(Program& program, const ResourceSpecialization& specialization);
// Rewrites any indirect root mapping offsets to their fixed flattened-SRT directory slots.
void CanonicalizeIndirectMappingOffsets(const Program&          program,
                                        ResourceSpecialization& specialization);

// Kind of an image heap entry as root sees it, or 0 (and a zeroed value) when root reads it as
// null. Entries of one kind specialize identically.
uint32_t ImageHeapKind(const ImageResource& root, DescriptorValue& value);
// The sampled resource that root resolves entries of kind as; kind 0 gives the identity of
// root's view of a heap.
ImageResource ImageHeapResource(const ImageResource& root, uint32_t kind);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATION_H_ */
