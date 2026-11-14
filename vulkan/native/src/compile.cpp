#include "compile.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace vulkan {
namespace shader {

	Options::Options () :
		cacheDir (NULL),
		allowInProcess (true),
		allowProcess (true),
		verbose (false) {
	}

	Result::Result () :
		ok (false),
		backend ("none") {
	}

	namespace {

		bool gTrace = false;

	}

	void SetTrace (bool enabled) {

		gTrace = enabled;

	}

	static void Trace (const char* format, ...) {

		if (!gTrace) return;

		char buffer[512];
		va_list args;

		va_start (args, format);
		vsnprintf (buffer, sizeof (buffer), format, args);
		va_end (args);

		fputs (buffer, stdout);
		fflush (stdout);

	}

	uint64_t CacheKey (const char* vertexGlsl, const char* fragmentGlsl) {

		// FNV-1a 64: spread far enough, short enough, and no hash library needed for it.
		uint64_t hash = 1469598103934665603ULL;

		const unsigned char* cursor = (const unsigned char*)vertexGlsl;

		while (cursor && *cursor) {

			hash = (hash ^ *cursor) * 1099511628211ULL;
			++cursor;

		}

		hash = (hash ^ 0x01) * 1099511628211ULL;

		cursor = (const unsigned char*)fragmentGlsl;

		while (cursor && *cursor) {

			hash = (hash ^ *cursor) * 1099511628211ULL;
			++cursor;

		}

		return hash;

	}

	// ---------------------------------------------------------------------
	// Disk cache
	// ---------------------------------------------------------------------

	namespace {

		const uint32_t kCacheMagic = 0x534B564CU; // cache file magic
		const uint32_t kCacheVersion = 1;

#if defined(_WIN32)

		std::wstring ToWide (const char* text) {

			if (!text) return std::wstring();

			int length = MultiByteToWideChar (CP_UTF8, 0, text, -1, NULL, 0);

			if (length <= 0) return std::wstring();

			std::wstring wide ((size_t)length, L'\0');

			MultiByteToWideChar (CP_UTF8, 0, text, -1, &wide[0], length);
			wide.resize ((size_t)length - 1);

			return wide;

		}

		bool EnsureDirectory (const wchar_t* path) {

			if (!path || !*path) return false;

			std::wstring current;

			for (size_t i = 0; path[i]; ++i) {

				current += path[i];

				if (path[i] == L'\\' || path[i] == L'/') {

					if (current.size() > 1) CreateDirectoryW (current.c_str(), NULL);

				}

			}

			CreateDirectoryW (current.c_str(), NULL);

			DWORD attributes = GetFileAttributesW (current.c_str());

			return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);

		}

		bool ReadWholeFile (const std::wstring& path, std::vector<unsigned char>* data) {

			HANDLE file = CreateFileW (path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);

			if (file == INVALID_HANDLE_VALUE) return false;

			LARGE_INTEGER size;

			if (!GetFileSizeEx (file, &size) || size.QuadPart <= 0) {

				CloseHandle (file);
				return false;

			}

			data->resize ((size_t)size.QuadPart);

			DWORD read = 0;
			bool ok = ReadFile (file, data->data(), (DWORD)data->size(), &read, NULL) != 0 && read == data->size();

			CloseHandle (file);

			return ok;

		}

		bool WriteWholeFile (const std::wstring& path, const void* data, size_t size) {

			HANDLE file = CreateFileW (path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);

			if (file == INVALID_HANDLE_VALUE) return false;

			DWORD written = 0;
			bool ok = WriteFile (file, data, (DWORD)size, &written, NULL) != 0 && written == size;

			CloseHandle (file);

			return ok;

		}

#endif

		std::string CachePath (const char* directory, uint64_t key) {

			char name[64];
			snprintf (name, sizeof (name), "%016llx.spv", (unsigned long long)key);

			std::string path = directory;

			if (!path.empty() && path[path.size() - 1] != '\\' && path[path.size() - 1] != '/') path += '\\';

			return path + name;

		}

		bool LoadCache (const char* directory, uint64_t key, Result* out) {

#if defined(_WIN32)

			if (!directory || !*directory) return false;

			std::vector<unsigned char> data;
			std::wstring path = ToWide (CachePath (directory, key).c_str());

			if (!ReadWholeFile (path, &data)) return false;

			if (data.size() < 16) return false;

			uint32_t magic = 0;
			uint32_t version = 0;
			uint32_t vertexWords = 0;
			uint32_t fragmentWords = 0;

			memcpy (&magic, &data[0], 4);
			memcpy (&version, &data[4], 4);
			memcpy (&vertexWords, &data[8], 4);
			memcpy (&fragmentWords, &data[12], 4);

			if (magic != kCacheMagic || version != kCacheVersion) return false;

			size_t expected = 16 + (size_t)(vertexWords + fragmentWords) * 4;

			if (data.size() != expected) return false;

			out->vertex.assign (vertexWords, 0);
			out->fragment.assign (fragmentWords, 0);

			if (vertexWords) memcpy (out->vertex.data(), &data[16], (size_t)vertexWords * 4);
			if (fragmentWords) memcpy (out->fragment.data(), &data[16 + (size_t)vertexWords * 4], (size_t)fragmentWords * 4);

			return true;

#else

			(void)directory; (void)key; (void)out;
			return false;

#endif

		}

		void StoreCache (const char* directory, uint64_t key, const Result& result) {

#if defined(_WIN32)

			if (!directory || !*directory) return;
			if (result.vertex.empty() || result.fragment.empty()) return;

			std::wstring wide = ToWide (directory);

			if (!EnsureDirectory (wide.c_str())) return;

			std::vector<unsigned char> data;
			data.resize (16 + (result.vertex.size() + result.fragment.size()) * 4);

			uint32_t magic = kCacheMagic;
			uint32_t version = kCacheVersion;
			uint32_t vertexWords = (uint32_t)result.vertex.size();
			uint32_t fragmentWords = (uint32_t)result.fragment.size();

			memcpy (&data[0], &magic, 4);
			memcpy (&data[4], &version, 4);
			memcpy (&data[8], &vertexWords, 4);
			memcpy (&data[12], &fragmentWords, 4);
			memcpy (&data[16], result.vertex.data(), result.vertex.size() * 4);
			memcpy (&data[16 + result.vertex.size() * 4], result.fragment.data(), result.fragment.size() * 4);

			WriteWholeFile (ToWide (CachePath (directory, key).c_str()), data.data(), data.size());

#else

			(void)directory; (void)key; (void)result;

#endif

		}

	}

	// ---------------------------------------------------------------------
	// Backend 2: in-process glslang.dll
	//
	// The ABI is declared here instead of including the SDK headers, so the build
	// does not need VULKAN_SDK\Include\glslang and anyone with MSVC can compile
	// the source. Every enum in the structs is a plain C int and the three function
	// pointer callbacks are passed by value (24 bytes on x64), which matches the
	// official header layout.
	// ---------------------------------------------------------------------

	namespace {

		typedef void GlslangShader;
		typedef void GlslangProgram;

		struct GlslangInput {

			int language;
			int stage;
			int client;
			int clientVersion;
			int targetLanguage;
			int targetLanguageVersion;
			const char* code;
			int defaultVersion;
			int defaultProfile;
			int forceDefaultVersionAndProfile;
			int forwardCompatible;
			int messages;
			const void* resource;
			void* callbacks[3];
			void* callbacksCtx;

		};

		const int kGlslangStageVertex = 0;
		const int kGlslangStageFragment = 4;

		const int kGlslangSourceGlsl = 1;
		const int kGlslangClientVulkan = 1;
		const int kGlslangTargetSpv = 1;
		const int kGlslangTargetVulkan10 = (1 << 22);
		const int kGlslangTargetSpv10 = (1 << 16);
		const int kGlslangEsProfile = (1 << 3);
		const int kGlslangMsgRelaxedErrors = (1 << 0);

		struct GlslangApi {

			void* module;

			int (*initializeProcess)(void);
			void (*finalizeProcess)(void);

			GlslangShader* (*shaderCreate)(const GlslangInput*);
			void (*shaderDelete)(GlslangShader*);
			int (*shaderPreprocess)(GlslangShader*, const GlslangInput*);
			int (*shaderParse)(GlslangShader*, const GlslangInput*);
			const char* (*shaderInfoLog)(GlslangShader*);

			GlslangProgram* (*programCreate)(void);
			void (*programDelete)(GlslangProgram*);
			void (*programAddShader)(GlslangProgram*, GlslangShader*);
			int (*programLink)(GlslangProgram*, int);
			const char* (*programInfoLog)(GlslangProgram*);
			void (*programGenerate)(GlslangProgram*, int);
			size_t (*programSize)(GlslangProgram*);
			unsigned int* (*programPtr)(GlslangProgram*);

			// The resource limits (TBuiltInResource) come from
			// glslang-default-resource-limits.dll. The 100+ field struct is not
			// redefined here: versions add fields and a mismatch is a crash, so asking
			// the official library for the defaults is the safe option.
			void* limitsModule;
			const void* (*defaultResource)(void);
			const void* resource;

			bool loaded;
			bool initialized;
			std::string log;

			GlslangApi (); 

		};

		GlslangApi::GlslangApi () :
			module (0),
			initializeProcess (0),
			finalizeProcess (0),
			shaderCreate (0),
			shaderDelete (0),
			shaderPreprocess (0),
			shaderParse (0),
			shaderInfoLog (0),
			programCreate (0),
			programDelete (0),
			programAddShader (0),
			programLink (0),
			programInfoLog (0),
			programGenerate (0),
			programSize (0),
			programPtr (0),
			limitsModule (0),
			defaultResource (0),
			resource (0),
			loaded (false),
			initialized (false) {
		}

#if defined(_WIN32)

		void* TryLoadLibrary (const wchar_t* path, std::string* log) {

			HMODULE module = LoadLibraryW (path);

			if (!module && log) {

				log->append ("  tried ");
				log->append (path[0] ? "library" : "glslang.dll");
				log->append (" - not found\n");

			}

			return module;

		}

#endif

		GlslangApi& Api () {

			static GlslangApi api;
			static bool attempted = false;

			if (attempted) return api;

			attempted = true;

			Trace ("trace: probing for glslang.dll\n");

#if defined(_WIN32)

			const wchar_t* candidates[] = { L"glslang.dll", L"bin\\glslang.dll" };

			for (int i = 0; i < 2 && !api.module; ++i) {

				api.module = TryLoadLibrary (candidates[i], &api.log);

				Trace ("trace: LoadLibraryW candidate %d -> %p\n", i, api.module);

			}

			if (!api.module) {

				const char* sdk = getenv ("VULKAN_SDK");

				if (sdk && *sdk) {

					std::string path = sdk;
					path += "\\Bin\\glslang.dll";

					api.module = (void*)LoadLibraryW (ToWide (path.c_str()).c_str());

					Trace ("trace: LoadLibraryW(%s) -> %p\n", path.c_str(), api.module);

				}

				if (!api.module) api.log.append ("  tried VULKAN_SDK\\Bin\\glslang.dll\n");

			}

			if (api.module) {

				const wchar_t* limitsCandidates[] = { L"glslang-default-resource-limits.dll", L"bin\\glslang-default-resource-limits.dll" };

				for (int i = 0; i < 2 && !api.limitsModule; ++i) {

					api.limitsModule = (void*)LoadLibraryW (limitsCandidates[i]);

				}

				if (!api.limitsModule) {

					const char* sdk = getenv ("VULKAN_SDK");

					if (sdk && *sdk) {

						std::string path = sdk;
						path += "\\Bin\\glslang-default-resource-limits.dll";

						api.limitsModule = (void*)LoadLibraryW (ToWide (path.c_str()).c_str());

					}

				}

				Trace ("trace: resource limits module -> %p\n", api.limitsModule);

				if (api.limitsModule) {

					api.defaultResource = (const void* (*)(void))GetProcAddress ((HMODULE)api.limitsModule, "glslang_default_resource");

					if (api.defaultResource) api.resource = api.defaultResource();

				}

				Trace ("trace: default resource -> %p\n", api.resource);

				HMODULE module = (HMODULE)api.module;

#define LIME_VULKAN_BIND(field, name) api.field = (decltype (api.field))GetProcAddress (module, name); \
	if (!api.field) api.log.append ("  missing export " name "\n");

				LIME_VULKAN_BIND (initializeProcess, "glslang_initialize_process")
				LIME_VULKAN_BIND (finalizeProcess, "glslang_finalize_process")
				LIME_VULKAN_BIND (shaderCreate, "glslang_shader_create")
				LIME_VULKAN_BIND (shaderDelete, "glslang_shader_delete")
				LIME_VULKAN_BIND (shaderPreprocess, "glslang_shader_preprocess")
				LIME_VULKAN_BIND (shaderParse, "glslang_shader_parse")
				LIME_VULKAN_BIND (shaderInfoLog, "glslang_shader_get_info_log")
				LIME_VULKAN_BIND (programCreate, "glslang_program_create")
				LIME_VULKAN_BIND (programDelete, "glslang_program_delete")
				LIME_VULKAN_BIND (programAddShader, "glslang_program_add_shader")
				LIME_VULKAN_BIND (programLink, "glslang_program_link")
				LIME_VULKAN_BIND (programInfoLog, "glslang_program_get_info_log")
				LIME_VULKAN_BIND (programGenerate, "glslang_program_SPIRV_generate")
				LIME_VULKAN_BIND (programSize, "glslang_program_SPIRV_get_size")
				LIME_VULKAN_BIND (programPtr, "glslang_program_SPIRV_get_ptr")

#undef LIME_VULKAN_BIND

				Trace ("trace: exports bound, initializeProcess=%p programLink=%p programPtr=%p\n",
					(void*)api.initializeProcess, (void*)api.programLink, (void*)api.programPtr);

				if (api.initializeProcess && api.programLink && api.programPtr && api.resource) {

					Trace ("trace: glslang_initialize_process()...\n");

					int version = api.initializeProcess();

					Trace ("trace: glslang_initialize_process() -> %d\n", version);
					api.initialized = true;
					api.loaded = true;
					api.log.append ("  glslang.dll loaded, all exports resolved\n");
					Trace ("trace: in-process compiler ready\n");

				}

			}

#endif

			return api;

		}

		bool HasInProcessCompiler () {

			return Api().loaded;

		}

		std::string GlslangDiagnostics () {

			return Api().log;

		}

		bool CompileInProcess (const char* vertexGlsl, const char* fragmentGlsl, Result* out) {

			GlslangApi& api = Api();

			if (!api.loaded) return false;

			GlslangShader* vertexShader = NULL;
			GlslangShader* fragmentShader = NULL;
			GlslangProgram* program = NULL;

			bool ok = false;

			GlslangInput vertexInput;
			memset (&vertexInput, 0, sizeof (vertexInput));
			vertexInput.language = kGlslangSourceGlsl;
			vertexInput.stage = kGlslangStageVertex;
			vertexInput.client = kGlslangClientVulkan;
			vertexInput.clientVersion = kGlslangTargetVulkan10;
			vertexInput.targetLanguage = kGlslangTargetSpv;
			vertexInput.targetLanguageVersion = kGlslangTargetSpv10;
			vertexInput.code = vertexGlsl;
			vertexInput.defaultVersion = 310;
			vertexInput.defaultProfile = kGlslangEsProfile;

			// ES 1.00 sources lean on int/float conversions that ES 3.10 rejects.
			vertexInput.messages = kGlslangMsgRelaxedErrors;

			vertexInput.resource = api.resource;

			GlslangInput fragmentInput = vertexInput;
			fragmentInput.stage = kGlslangStageFragment;
			fragmentInput.code = fragmentGlsl;

			Trace ("trace: shaderCreate(vertex)...\n");
			vertexShader = api.shaderCreate (&vertexInput);
			Trace ("trace: shaderCreate(vertex) -> %p\n", (void*)vertexShader);

			if (!vertexShader) {

				out->log += "glslang: could not create the vertex shader object\n";
				goto cleanup;

			}

			Trace ("trace: shaderCreate(fragment)...\n");
			fragmentShader = api.shaderCreate (&fragmentInput);
			Trace ("trace: shaderCreate(fragment) -> %p\n", (void*)fragmentShader);

			if (!fragmentShader) {

				out->log += "glslang: could not create the fragment shader object\n";
				goto cleanup;

			}

			Trace ("trace: preprocess(vertex)...\n");

			if (!api.shaderPreprocess (vertexShader, &vertexInput) || !api.shaderParse (vertexShader, &vertexInput)) {

				out->log += "glslang(vertex): ";
				out->log += api.shaderInfoLog (vertexShader) ? api.shaderInfoLog (vertexShader) : "(no log)";
				out->log += "\n";
				goto cleanup;

			}

			Trace ("trace: preprocess(fragment)...\n");

			if (!api.shaderPreprocess (fragmentShader, &fragmentInput) || !api.shaderParse (fragmentShader, &fragmentInput)) {

				out->log += "glslang(fragment): ";
				out->log += api.shaderInfoLog (fragmentShader) ? api.shaderInfoLog (fragmentShader) : "(no log)";
				out->log += "\n";
				goto cleanup;

			}

			Trace ("trace: programCreate...\n");
			program = api.programCreate();
			Trace ("trace: programCreate -> %p\n", (void*)program);

			if (!program) {

				out->log += "glslang: could not create the program object\n";
				goto cleanup;

			}

			api.programAddShader (program, vertexShader);
			api.programAddShader (program, fragmentShader);

			if (!api.programLink (program, 0)) {

				const char* log = api.programInfoLog (program);

				out->log += "glslang(link): ";
				out->log += log ? log : "(no log)";
				out->log += "\n";
				goto cleanup;

			}

			api.programGenerate (program, kGlslangStageVertex);

			{
				size_t size = api.programSize (program);
				unsigned int* words = api.programPtr (program);

				if (!words || size == 0) {

					out->log += "glslang: the vertex stage produced no SPIR-V\n";
					goto cleanup;

				}

				out->vertex.assign (words, words + size);
			}

			api.programGenerate (program, kGlslangStageFragment);

			{
				size_t size = api.programSize (program);
				unsigned int* words = api.programPtr (program);

				if (!words || size == 0) {

					out->log += "glslang: the fragment stage produced no SPIR-V\n";
					goto cleanup;

				}

				out->fragment.assign (words, words + size);
			}

			ok = true;

		cleanup:

			if (program) api.programDelete (program);
			if (vertexShader) api.shaderDelete (vertexShader);
			if (fragmentShader) api.shaderDelete (fragmentShader);

			return ok;

		}

	}

	// ---------------------------------------------------------------------
	// Backend 3: the external glslangValidator.exe
	//
	// Still works on a machine without the SDK runtime but with the command line
	// tool installed. Slow (tens of milliseconds per call), and only reached on a
	// cache miss when the in-process dll is unavailable.
	// ---------------------------------------------------------------------

	namespace {

#if defined(_WIN32)

		std::string WideToUtf8 (const std::wstring& text) {

			if (text.empty()) return std::string();

			int length = WideCharToMultiByte (CP_UTF8, 0, text.c_str(), (int)text.size(), NULL, 0, NULL, NULL);

			if (length <= 0) return std::string();

			std::string result ((size_t)length, '\0');

			WideCharToMultiByte (CP_UTF8, 0, text.c_str(), (int)text.size(), &result[0], length, NULL, NULL);

			return result;

		}

		std::wstring ExecutableDirectory () {

			wchar_t buffer[MAX_PATH];
			DWORD length = GetModuleFileNameW (NULL, buffer, MAX_PATH);

			if (length == 0) return std::wstring();

			std::wstring path (buffer, length);
			size_t slash = path.find_last_of (L'\\');

			if (slash == std::wstring::npos) return std::wstring();

			return path.substr (0, slash);

		}

		bool RunAndCapture (const std::wstring& commandLine, std::string* output, DWORD* exitCode) {

			SECURITY_ATTRIBUTES attributes;
			memset (&attributes, 0, sizeof (attributes));
			attributes.nLength = sizeof (attributes);
			attributes.bInheritHandle = TRUE;

			HANDLE readPipe = NULL;
			HANDLE writePipe = NULL;

			if (!CreatePipe (&readPipe, &writePipe, &attributes, 0)) return false;

			SetHandleInformation (readPipe, HANDLE_FLAG_INHERIT, 0);

			STARTUPINFOW startup;
			memset (&startup, 0, sizeof (startup));
			startup.cb = sizeof (startup);
			startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
			startup.wShowWindow = SW_HIDE;
			startup.hStdOutput = writePipe;
			startup.hStdError = writePipe;
			startup.hStdInput = NULL;

			PROCESS_INFORMATION process;
			memset (&process, 0, sizeof (process));

			std::wstring mutableCommand = commandLine;

			BOOL started = CreateProcessW (NULL, &mutableCommand[0], NULL, NULL, TRUE,
				CREATE_NO_WINDOW, NULL, NULL, &startup, &process);

			CloseHandle (writePipe);

			if (!started) {

				CloseHandle (readPipe);
				return false;

			}

			char buffer[1024];

			for (;;) {

				DWORD available = 0;

				if (!PeekNamedPipe (readPipe, NULL, 0, NULL, &available, NULL)) break;
				if (available == 0) {

					DWORD wait = WaitForSingleObject (process.hProcess, 20);

					if (wait == WAIT_OBJECT_0) {

						if (!PeekNamedPipe (readPipe, NULL, 0, NULL, &available, NULL) || available == 0) break;

					}

					continue;

				}

				DWORD read = 0;

				if (!ReadFile (readPipe, buffer, (DWORD)((available < sizeof (buffer)) ? available : sizeof (buffer)), &read, NULL) || read == 0) break;

				output->append (buffer, read);

			}

			WaitForSingleObject (process.hProcess, INFINITE);

			if (exitCode) GetExitCodeProcess (process.hProcess, exitCode);

			CloseHandle (process.hThread);
			CloseHandle (process.hProcess);
			CloseHandle (readPipe);

			return true;

		}

		std::wstring FindGlslangValidator () {

			std::wstring local = ExecutableDirectory();

			if (!local.empty()) {

				std::wstring candidate = local + L"\\glslangValidator.exe";

				if (GetFileAttributesW (candidate.c_str()) != INVALID_FILE_ATTRIBUTES) return candidate;

			}

			const char* sdk = getenv ("VULKAN_SDK");

			if (sdk && *sdk) {

				std::wstring candidate = ToWide (sdk);
				candidate += L"\\Bin\\glslangValidator.exe";

				if (GetFileAttributesW (candidate.c_str()) != INVALID_FILE_ATTRIBUTES) return candidate;

			}

			return L"glslangValidator.exe";

		}

		bool CompileStageWithProcess (const std::wstring& tool, const char* source, const char* workDirectory,
			const char* inputName, const char* outputName, const char* stageName, std::string* log) {

			std::string inputPath = workDirectory;
			inputPath += '\\';
			inputPath += inputName;

			std::string outputPath = workDirectory;
			outputPath += '\\';
			outputPath += outputName;

			if (!WriteWholeFile (ToWide (inputPath.c_str()), source, strlen (source))) {

				log->append ("could not write the temporary shader file\n");
				return false;

			}

			std::wstring command = L"\x22";
			command += tool;
			command += L"\x22";
			command += L" -V -R --target-env vulkan1.0 -S ";
			command += ToWide (stageName);
			command += L" -o ";
			command += ToWide (outputPath.c_str());
			command += L" ";
			command += ToWide (inputPath.c_str());

			std::string output;
			DWORD exitCode = 1;

			if (!RunAndCapture (command, &output, &exitCode)) {

				log->append ("could not start glslangValidator.exe\n");
				return false;

			}

			if (exitCode != 0) {

				log->append ("glslangValidator failed: ");
				log->append (output);
				log->append ("\n");
				return false;

			}

			std::vector<unsigned char> data;

			if (!ReadWholeFile (ToWide (outputPath.c_str()), &data) || data.size() < 4) {

				log->append ("glslangValidator produced no output\n");
				return false;

			}

			return true;

		}

		bool CompileWithProcess (const char* vertexGlsl, const char* fragmentGlsl,
			std::vector<uint32_t>* vertex, std::vector<uint32_t>* fragment, std::string* log) {

			std::wstring tool = FindGlslangValidator ();

			wchar_t temporaryPath[MAX_PATH];
			DWORD length = GetTempPathW (MAX_PATH, temporaryPath);

			if (length == 0) {

				log->append ("GetTempPath failed\n");
				return false;

			}

			std::wstring directory = temporaryPath;
			directory += L"lime-vulkan";

			EnsureDirectory (directory.c_str());

			std::string narrow = WideToUtf8 (directory);

			char vertexOut[64];
			char fragmentOut[64];
			snprintf (vertexOut, sizeof (vertexOut), "lime-vulkan%u.vert.spv", GetCurrentProcessId());
			snprintf (fragmentOut, sizeof (fragmentOut), "lime-vulkan%u.frag.spv", GetCurrentProcessId());

			struct StageJob {

				const char* source;
				const char* inputName;
				const char* outputName;
				const char* stage;
				std::vector<uint32_t>* target;

			};

			StageJob jobs[2] = {
				{ vertexGlsl, "lime-vulkan.vert", vertexOut, "vert", vertex },
				{ fragmentGlsl, "lime-vulkan.frag", fragmentOut, "frag", fragment },
			};

			for (int i = 0; i < 2; ++i) {

				std::string stageLog;

				if (!CompileStageWithProcess (tool, jobs[i].source, narrow.c_str(),
					jobs[i].inputName, jobs[i].outputName, jobs[i].stage, &stageLog)) {

					log->append (stageLog);
					return false;

				}

				std::string outputPath = narrow;
				outputPath += '\\';
				outputPath += jobs[i].outputName;

				std::vector<unsigned char> data;

				if (!ReadWholeFile (ToWide (outputPath.c_str()), &data) || data.size() < 4) {

					log->append ("empty SPIR-V produced\n");
					return false;

				}

				size_t words = data.size() / 4;

				jobs[i].target->assign (words, 0);
				memcpy (jobs[i].target->data(), data.data(), words * 4);

			}

			return true;

		}

#else

		bool CompileWithProcess (const char* vertexGlsl, const char* fragmentGlsl,
			std::vector<uint32_t>* vertex, std::vector<uint32_t>* fragment, std::string* log) {

			(void)vertexGlsl; (void)fragmentGlsl; (void)vertex; (void)fragment;
			log->append ("no subprocess backend on this platform\n");
			return false;

		}

#endif

	}

	// ---------------------------------------------------------------------
	// Backend dispatch
	// ---------------------------------------------------------------------

	bool Compile (const char* vertexGlsl, const char* fragmentGlsl, const Options& options, Result* out) {

		if (!vertexGlsl || !*vertexGlsl || !fragmentGlsl || !*fragmentGlsl) {

			out->log = "empty shader source\n";
			out->backend = "none";
			return false;

		}

		uint64_t key = CacheKey (vertexGlsl, fragmentGlsl);

		if (options.cacheDir && *options.cacheDir) {

			if (LoadCache (options.cacheDir, key, out)) {

				out->ok = true;
				out->backend = "cache";

				if (options.verbose) {

					char line[160];
					snprintf (line, sizeof (line), "shaderc: cache hit %016llx (%u + %u words)\n",
						(unsigned long long)key, (unsigned)out->vertex.size(), (unsigned)out->fragment.size());
					out->log += line;

				}

				return true;

			}

			if (options.verbose) {

				char line[160];
				snprintf (line, sizeof (line), "shaderc: cache miss %016llx\n", (unsigned long long)key);
				out->log += line;

			}

		}

		if (options.allowInProcess) {

			if (HasInProcessCompiler()) {

				if (options.verbose) out->log += GlslangDiagnostics();

				Result attempt;

				if (CompileInProcess (vertexGlsl, fragmentGlsl, &attempt)) {

					out->vertex = attempt.vertex;
					out->fragment = attempt.fragment;
					out->ok = true;
					out->backend = "glslang.dll";

					StoreCache (options.cacheDir, key, *out);

					return true;

				}

				out->log += attempt.log;

			} else if (options.verbose) {

				out->log += "shaderc: in-process compiler unavailable\n";
				out->log += GlslangDiagnostics();

			}

		}

		if (options.allowProcess) {

			Result attempt;

			if (CompileWithProcess (vertexGlsl, fragmentGlsl, &attempt.vertex, &attempt.fragment, &attempt.log)) {

				out->vertex = attempt.vertex;
				out->fragment = attempt.fragment;
				out->ok = true;
				out->backend = "glslangValidator.exe";

				StoreCache (options.cacheDir, key, *out);

				return true;

			}

			out->log += attempt.log;

		}

		out->ok = false;
		out->backend = "none";

		if (out->log.empty()) {

			out->log = "no GLSL compiler available: set VULKAN_SDK or put glslangValidator.exe next to the executable\n";

		}

		return false;

	}

}
}
