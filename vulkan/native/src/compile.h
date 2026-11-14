#ifndef LIME_VULKAN_COMPILE_H
#define LIME_VULKAN_COMPILE_H

// GLSL(450) → SPIR-V。
//
// Three backends, tried in order:
//   1. disk cache (hash hit, zero cost)
//   2. in-process glslang.dll (LoadLibrary at runtime, no link dependency)
//   3. external glslangValidator.exe as a child process
// When all three fail the call returns failure plus a reason and the caller
// decides how to fall back.
//
// Why glslang is not linked statically: the glslang.lib in the SDK is 70 MB,
// which turns a few hundred KB dll into tens of MB, and it would make a 3D
// shader compiler a hard runtime dependency. With LoadLibrary a machine without
// the SDK only loses 'new shaders at runtime'; the cache and the built-in
// pipelines keep working.

#include <cstdint>
#include <string>
#include <vector>

namespace vulkan {
namespace shader {

	enum Stage {
		STAGE_VERTEX = 0,
		STAGE_FRAGMENT = 1
	};

	struct Options {

		/** Disk cache directory. NULL or empty disables caching. */
		const char* cacheDir;

		/** Allow the in-process glslang.dll. */
		bool allowInProcess;

		/** Allow the external glslangValidator.exe. */
		bool allowProcess;

		/** Print every step. */
		bool verbose;

		Options ();

	};

	struct Result {

		bool ok;

		std::vector<uint32_t> vertex;
		std::vector<uint32_t> fragment;

		std::string log;

		/** "cache" / "glslang.dll" / "glslangValidator.exe" / "none" */
		const char* backend;

		Result ();

	};

	/** Compiles both stages together: glslang only links them once it can check the varyings. */
	bool Compile (const char* vertexGlsl, const char* fragmentGlsl, const Options& options, Result* out);

	/** Cache key (FNV-1a 64), printed on the command line to make failures traceable. */
	uint64_t CacheKey (const char* vertexGlsl, const char* fragmentGlsl);

	/** Turns on per-step tracing of the compiler backends (the --verbose flag enables it).
	 *  The in-process backend is a LoadLibrary at runtime, so this is usually the only
	 *  way to see what went wrong. */
	void SetTrace (bool enabled);

}
}
#endif
