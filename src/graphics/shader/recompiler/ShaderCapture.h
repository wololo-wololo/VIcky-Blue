#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERCAPTURE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERCAPTURE_H_

#include "graphics/shader/recompiler/ShaderRecompiler.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <type_traits>
#include <vector>

// Debug aid. With KYTY_SHADER_CAPTURE=<folder> set, the pipeline cache saves every guest shader
// that fails to translate or specialize, together with the inputs translation depends on. The
// recompiler tests replay such a folder offline (shader_recompiler_compute_tests --replay), so a
// failure seen in a title can be reproduced without running the title again.
namespace Libs::Graphics::ShaderRecompiler::Capture {

inline constexpr uint32_t Magic   = 0x4348534bu; // "KSHC"
inline constexpr uint32_t Version = 1;

struct Header {
	uint32_t magic           = Magic;
	uint32_t version         = Version;
	uint32_t stage           = 0;
	uint32_t wave_size       = 0;
	uint32_t user_data_base  = 0;
	uint32_t code_words      = 0;
	uint32_t user_data_words = 0;
	uint32_t back_code_words = 0;
	uint32_t compute_bytes   = 0; // sizeof(ShaderComputeInputInfo) for compute, else 0
	uint32_t reserved        = 0;
	uint64_t hash            = 0;
};

struct Shader {
	Header                 header;
	std::vector<uint32_t>  code;
	std::vector<uint32_t>  user_data;
	std::vector<uint32_t>  back_code;
	ShaderComputeInputInfo compute;
	bool                   has_compute = false;
};

static_assert(std::is_trivially_copyable_v<ShaderComputeInputInfo>);

[[nodiscard]] inline const std::filesystem::path& Folder() {
	static const std::filesystem::path folder = [] {
		const char* text = std::getenv("KYTY_SHADER_CAPTURE");
		return text == nullptr ? std::filesystem::path {} : std::filesystem::path {text};
	}();
	return folder;
}

// reason names the failing phase; it becomes a subfolder so captures sort by failure class.
inline void Save(const char* reason, std::span<const uint32_t> code, const CompileOptions& options) {
	const auto& folder = Folder();
	if (folder.empty()) return;
	std::error_code error;
	const auto      dir = folder / reason;
	std::filesystem::create_directories(dir, error);
	const auto path =
	    dir / fmt::format("{}_{:016x}.kshc", StageName(options.stage), options.shader_hash);
	if (std::filesystem::exists(path, error)) return;
	const bool compute = options.stage == ShaderType::Compute && options.input_info.compute != nullptr;
	Header     header;
	header.stage           = static_cast<uint32_t>(options.stage);
	header.wave_size       = options.wave_size;
	header.user_data_base  = options.user_data_base;
	header.code_words      = static_cast<uint32_t>(code.size());
	header.user_data_words = static_cast<uint32_t>(options.user_data.size());
	header.back_code_words = static_cast<uint32_t>(options.back_code.size());
	header.compute_bytes   = compute ? static_cast<uint32_t>(sizeof(ShaderComputeInputInfo)) : 0u;
	header.hash            = options.shader_hash;
	std::ofstream file(path, std::ios::binary);
	const auto    write = [&](const void* data, size_t size) {
        file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
	};
	write(&header, sizeof(header));
	write(code.data(), code.size_bytes());
	write(options.user_data.data(), options.user_data.size_bytes());
	write(options.back_code.data(), options.back_code.size_bytes());
	if (compute) {
		// The runtime pointers are meaningless outside this process; Load clears them.
		write(options.input_info.compute, sizeof(ShaderComputeInputInfo));
	}
}

[[nodiscard]] inline bool Load(const std::filesystem::path& path, Shader& shader) {
	std::ifstream file(path, std::ios::binary);
	const auto    read = [&](void* data, size_t size) {
        file.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
        return static_cast<size_t>(file.gcount()) == size;
	};
	auto& header = shader.header;
	if (!read(&header, sizeof(header)) || header.magic != Magic || header.version != Version ||
	    (header.compute_bytes != 0 && header.compute_bytes != sizeof(ShaderComputeInputInfo))) {
		return false;
	}
	shader.code.resize(header.code_words);
	shader.user_data.resize(header.user_data_words);
	shader.back_code.resize(header.back_code_words);
	if (!read(shader.code.data(), shader.code.size() * sizeof(uint32_t)) ||
	    !read(shader.user_data.data(), shader.user_data.size() * sizeof(uint32_t)) ||
	    !read(shader.back_code.data(), shader.back_code.size() * sizeof(uint32_t))) {
		return false;
	}
	shader.has_compute = header.compute_bytes != 0;
	if (shader.has_compute) {
		if (!read(&shader.compute, sizeof(shader.compute))) return false;
		shader.compute.stage = {};
	}
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::Capture

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERCAPTURE_H_ */
