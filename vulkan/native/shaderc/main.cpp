// lime-vulkan-shaderc: offline GLSL ES 1.00 to SPIR-V tool.
//
// It does exactly what lime_vulkan_program_create does at runtime (the same glsl and
// compile), except that every intermediate step is visible. When a shader
// comes out wrong under Vulkan, printing the translated GLSL here is much faster
// than breaking inside the engine.

#include "../src/compile.h"
#include "../src/glsl.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

	bool ReadTextFile (const char* path, std::string* out) {

#if defined(_WIN32)
		FILE* file = NULL;

		if (fopen_s (&file, path, "rb") != 0 || !file) return false;
#else
		FILE* file = fopen (path, "rb");
		if (!file) return false;
#endif

		fseek (file, 0, SEEK_END);
		long size = ftell (file);
		fseek (file, 0, SEEK_SET);

		if (size < 0) {

			fclose (file);
			return false;

		}

		out->resize ((size_t)size);

		size_t read = size > 0 ? fread (&(*out)[0], 1, (size_t)size, file) : 0;

		fclose (file);

		return read == (size_t)size;

	}

	bool WriteBinaryFile (const char* path, const void* data, size_t size) {

#if defined(_WIN32)
		FILE* file = NULL;

		if (fopen_s (&file, path, "wb") != 0 || !file) return false;
#else
		FILE* file = fopen (path, "wb");
		if (!file) return false;
#endif

		size_t written = fwrite (data, 1, size, file);

		fclose (file);

		return written == size;

	}

	void PrintUsage () {

		printf (
			"lime-vulkan-shaderc - GLSL ES 1.00 to SPIR-V for lime-vulkan\n"
			"\n"
			"  --vert <file>       vertex shader source (GLSL ES 1.00, OpenFL dialect)\n"
			"  --frag <file>       fragment shader source\n"
			"  --plain             disable the OpenFL / Flixel compatibility layer\n"
			"  --dump              print the translated GLSL ES 3.10\n"
			"  --reflect           print the reflected interface\n"
			"  --out <dir>         write <name>.vert.glsl, <name>.frag.glsl and the .spv files\n"
			"  --name <name>       base name for --out (default: shader)\n"
			"  --cache <dir>       enable the SPIR-V disk cache\n"
			"  --no-dll            do not use glslang.dll\n"
			"  --no-process        do not spawn glslangValidator.exe\n"
			"  --verbose           print what the backend is doing\n"
			"  --stats             print source and SPIR-V sizes\n");

	}

}

int main (int argc, char** argv) {

	// Flush so anything printed before a crash is still visible; most debugging depends on it.
	setvbuf (stdout, NULL, _IONBF, 0);
	setvbuf (stderr, NULL, _IONBF, 0);

	const char* vertexPath = NULL;
	const char* fragmentPath = NULL;
	const char* outputDirectory = NULL;
	const char* cacheDirectory = NULL;
	const char* name = "shader";

	bool openfl = true;
	bool dump = false;
	bool reflect = false;
	bool verbose = false;
	bool stats = false;
	bool allowDll = true;
	bool allowProcess = true;

	for (int i = 1; i < argc; ++i) {

		const char* argument = argv[i];

		if (strcmp (argument, "--vert") == 0 && i + 1 < argc) {

			vertexPath = argv[++i];

		} else if (strcmp (argument, "--frag") == 0 && i + 1 < argc) {

			fragmentPath = argv[++i];

		} else if (strcmp (argument, "--out") == 0 && i + 1 < argc) {

			outputDirectory = argv[++i];

		} else if (strcmp (argument, "--cache") == 0 && i + 1 < argc) {

			cacheDirectory = argv[++i];

		} else if (strcmp (argument, "--name") == 0 && i + 1 < argc) {

			name = argv[++i];

		} else if (strcmp (argument, "--plain") == 0) {

			openfl = false;

		} else if (strcmp (argument, "--dump") == 0) {

			dump = true;

		} else if (strcmp (argument, "--reflect") == 0) {

			reflect = true;

		} else if (strcmp (argument, "--verbose") == 0) {

			verbose = true;

		} else if (strcmp (argument, "--stats") == 0) {

			stats = true;

		} else if (strcmp (argument, "--no-dll") == 0) {

			allowDll = false;

		} else if (strcmp (argument, "--no-process") == 0) {

			allowProcess = false;

		} else if (strcmp (argument, "--help") == 0 || strcmp (argument, "-h") == 0) {

			PrintUsage ();
			return 0;

		} else {

			printf ("unknown argument: %s\n", argument);
			PrintUsage ();
			return 2;

		}

	}

	if (!vertexPath || !fragmentPath) {

		PrintUsage ();
		return 2;

	}

	std::string vertexSource, fragmentSource;

	if (!ReadTextFile (vertexPath, &vertexSource)) {

		printf ("could not read %s\n", vertexPath);
		return 1;

	}

	if (!ReadTextFile (fragmentPath, &fragmentSource)) {

		printf ("could not read %s\n", fragmentPath);
		return 1;

	}

	vulkan::glsl::Options options;
	options.openfl = openfl;

	vulkan::glsl::Translated translated;

	if (!vulkan::glsl::Translate (vertexSource.c_str(), (int)vertexSource.size(),
		fragmentSource.c_str(), (int)fragmentSource.size(), options, &translated)) {

		printf ("translation failed:\n%s", translated.log.c_str());
		return 1;

	}

	if (!translated.log.empty()) {

		printf ("translation notes:\n%s", translated.log.c_str());

	}

	if (dump) {

		printf ("\n===== vertex (GLSL ES 3.10) =====\n%s\n", translated.vertex.c_str());
		printf ("\n===== fragment (GLSL ES 3.10) =====\n%s\n", translated.fragment.c_str());

	}

	if (reflect) {

		printf ("\n===== reflection =====\n%s\n", vulkan::glsl::Describe (translated).c_str());

	}

	if (verbose) vulkan::shader::SetTrace (true);

	vulkan::shader::Options compileOptions;
	compileOptions.cacheDir = cacheDirectory;
	compileOptions.allowInProcess = allowDll;
	compileOptions.allowProcess = allowProcess;
	compileOptions.verbose = verbose;

	vulkan::shader::Result result;

	if (!vulkan::shader::Compile (translated.vertex.c_str(), translated.fragment.c_str(), compileOptions, &result)) {

		printf ("compile failed (backend=%s):\n%s", result.backend, result.log.c_str());
		return 1;

	}

	printf ("ok: backend=%s vertex=%u words fragment=%u words\n", result.backend,
		(unsigned)result.vertex.size(), (unsigned)result.fragment.size());

	if (verbose && !result.log.empty()) printf ("%s", result.log.c_str());

	if (stats) {

		printf ("source: vertex %u bytes, fragment %u bytes; uniform block %u bytes\n",
			(unsigned)translated.vertex.size(), (unsigned)translated.fragment.size(), translated.uniformSize);

	}

	if (outputDirectory) {

		std::string base = outputDirectory;
		if (!base.empty() && base[base.size() - 1] != '\\' && base[base.size() - 1] != '/') base += '\\';
		base += name;

		std::string vertexGlsl = base + ".vert.glsl";
		std::string fragmentGlsl = base + ".frag.glsl";
		std::string vertexSpv = base + ".vert.spv";
		std::string fragmentSpv = base + ".frag.spv";

		bool written = WriteBinaryFile (vertexGlsl.c_str(), translated.vertex.data(), translated.vertex.size())
			&& WriteBinaryFile (fragmentGlsl.c_str(), translated.fragment.data(), translated.fragment.size())
			&& WriteBinaryFile (vertexSpv.c_str(), result.vertex.data(), result.vertex.size() * 4)
			&& WriteBinaryFile (fragmentSpv.c_str(), result.fragment.data(), result.fragment.size() * 4);

		if (!written) {

			printf ("could not write the output files into %s\n", outputDirectory);
			return 1;

		}

		printf ("wrote %s.{vert,frag}.{glsl,spv}\n", base.c_str());

	}

	return 0;

}
