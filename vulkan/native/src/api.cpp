#include "internal.h"

#if defined(LIME_VULKAN_WINDOWS)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <chrono>

namespace vulkan {

	static std::string BuildProbeReport () {

		std::string out;
		char buffer[512];

		#define APPEND(fmt, ...) \
			snprintf (buffer, sizeof (buffer), fmt, __VA_ARGS__); \
			out += buffer; \
			out += "\n";

		#define APPEND_LITERAL(text) out += text; out += "\n";

		APPEND_LITERAL ("compiled=1")
		APPEND ("platform=%s", PlatformName ())

		if (!LoadLoader (S ().options.verbose != 0)) {

			APPEND_LITERAL ("available=0")
			APPEND ("reason=%s", S ().lastError.c_str ())
			return out;

		}

		APPEND ("loaderVersion=%s", VersionText (LoaderVersion ()).c_str ())

		VkInstance instance = VK_NULL_HANDLE;

		if (!CreateInstance (false, S ().options.verbose != 0, &instance)) {

			APPEND_LITERAL ("available=0")
			APPEND ("reason=%s", S ().lastError.c_str ())
			return out;

		}

		S ().instance = instance;

		if (!LoadInstanceApi (instance)) {

			APPEND_LITERAL ("available=0")
			APPEND ("reason=%s", S ().lastError.c_str ())
			S ().api.DestroyInstance (instance, NULL);
			S ().instance = VK_NULL_HANDLE;
			return out;

		}

		APPEND_LITERAL ("available=1")

		uint32_t count = 0;
		S ().api.EnumeratePhysicalDevices (instance, &count, NULL);

		std::vector<VkPhysicalDevice> devices (count > 0 ? count : 1);

		if (count > 0) S ().api.EnumeratePhysicalDevices (instance, &count, devices.data ());

		APPEND ("deviceCount=%d", (int)count)

		int best = -1;
		int bestScore = -1;

		for (uint32_t i = 0; i < count; ++i) {

			VkPhysicalDeviceProperties properties;
			S ().api.GetPhysicalDeviceProperties (devices[i], &properties);

			VkPhysicalDeviceMemoryProperties memory;
			S ().api.GetPhysicalDeviceMemoryProperties (devices[i], &memory);

			uint64_t localMemory = 0;

			for (uint32_t h = 0; h < memory.memoryHeapCount; ++h) {
				if (memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) localMemory += memory.memoryHeaps[h].size;
			}

			APPEND ("device%d.name=%s", (int)i, properties.deviceName)
			APPEND ("device%d.type=%s", (int)i, DeviceTypeText (properties.deviceType))
			APPEND ("device%d.apiVersion=%s", (int)i, VersionText (properties.apiVersion).c_str ())
			APPEND ("device%d.localMemoryMb=%d", (int)i, (int)(localMemory / (1024ull * 1024ull)))

			int score = DeviceScore (properties);

			if (score > bestScore) { bestScore = score; best = (int)i; }

		}

		APPEND ("selected=%d", best)

		S ().api.DestroyInstance (instance, NULL);
		S ().instance = VK_NULL_HANDLE;

		#undef APPEND

		return out;

	}

	static int CopyOut (const std::string& text, char* out, int capacity) {

		int needed = (int)text.size ();

		if (out && capacity > 0) {

			int copied = needed < (capacity - 1) ? needed : (capacity - 1);
			memcpy (out, text.c_str (), (size_t)copied);
			out[copied] = 0;

		}

		return needed;

	}

}

// -------------------------------------------------------------------------
// Public C API
// -------------------------------------------------------------------------

extern "C" {

	void lime_vulkan_default_options (lime_vulkan_options* options) {

		if (!options) return;

		options->deviceIndex = -1;
		options->presentMode = 0;
		options->enableValidation = 0;
		options->verbose = 0;
		options->fallbackDialog = 1;

	}

	int lime_vulkan_available (void) {
		return vulkan::LoadLoader (false) ? 1 : 0;

	}

	int lime_vulkan_probe (char* out, int capacity) {

		return vulkan::CopyOut (vulkan::BuildProbeReport (), out, capacity);

	}

	const char* lime_vulkan_probe_text (void) {

		static char buffer[4096];
		lime_vulkan_probe (buffer, (int)sizeof (buffer));
		return buffer;

	}

	int lime_vulkan_context_create (void* nativeWindow, int width, int height, const lime_vulkan_options* options) {

		if (options) vulkan::S ().options = *options;

		vulkan::S ().dialogEnabled = vulkan::S ().options.fallbackDialog != 0;

		return vulkan::AttachDevice (nativeWindow, width, height) ? 1 : 0;

	}

	void lime_vulkan_context_destroy (void) {

		vulkan::DetachDevice ();

	}

	int lime_vulkan_context_attached (void) {

		return vulkan::S ().attached ? 1 : 0;

	}

	int lime_vulkan_probe_window (void* nativeWindow, int width, int height, const lime_vulkan_options* options) {


		if (options) vulkan::S ().options = *options;

		vulkan::S ().dialogEnabled = vulkan::S ().options.fallbackDialog != 0;

		if (!nativeWindow) {

			vulkan::SetError ("lime_vulkan_probe_window: no native window");
			return 0;

		}

		if (width <= 0 || height <= 0) {

			vulkan::WindowClientSize (nativeWindow, &width, &height);

		}

		if (width <= 0 || height <= 0) {

			vulkan::SetError ("lime_vulkan_probe_window: window has no client area");
			return 0;

		}




		// Torn down right after the probe: only deviceName and the capability verdict survive, no VRAM and no window.
		vulkan::S ().attached = false;
		vulkan::DetachDevice ();
		return 1;

	}

	int lime_vulkan_context_create_flat (void* nativeWindow, int width, int height,
		int deviceIndex, int presentMode, int enableValidation, int verbose, int fallbackDialog) {

		lime_vulkan_options options;
		lime_vulkan_default_options (&options);

		options.deviceIndex = deviceIndex;
		options.presentMode = presentMode;
		options.enableValidation = enableValidation;
		options.verbose = verbose;
		options.fallbackDialog = fallbackDialog;

		return lime_vulkan_context_create (nativeWindow, width, height, &options);

	}

	int lime_vulkan_probe_window_flat (void* nativeWindow, int width, int height,
		int deviceIndex, int presentMode, int enableValidation, int verbose, int fallbackDialog) {

		lime_vulkan_options options;
		lime_vulkan_default_options (&options);

		options.deviceIndex = deviceIndex;
		options.presentMode = presentMode;
		options.enableValidation = enableValidation;
		options.verbose = verbose;
		options.fallbackDialog = fallbackDialog;

		return lime_vulkan_probe_window (nativeWindow, width, height, &options);

	}

	int lime_vulkan_resize (int width, int height) {

		vulkan::State& state = vulkan::S ();

		if (!state.attached) return 0;

		// 0x0 means 'minimised', not a size. Treating it as 1x1 rebuilds the
		// swapchain at one pixel and every later frame draws with a wrong aspect;
		// the right move is to ignore it and let the size come back with the window.
		if (width <= 0 || height <= 0) return 0;

		state.extent.width = (uint32_t)width;
		state.extent.height = (uint32_t)height;
		state.resized = true;
		return 1;

	}

	int lime_vulkan_present (const float clearColor[4]) {

		static float last = 0.0f;
		last += 0.016f;

		return vulkan::PresentFrame (clearColor, last) ? 1 : 0;

	}

	int lime_vulkan_status (char* out, int capacity) {

		vulkan::State& state = vulkan::S ();
		std::string text;
		char buffer[256];

		snprintf (buffer, sizeof (buffer), "attached=%d\n", state.attached ? 1 : 0); text += buffer;
		snprintf (buffer, sizeof (buffer), "device=%s\n", state.deviceName.c_str ()); text += buffer;
		snprintf (buffer, sizeof (buffer), "width=%u\n", state.extent.width); text += buffer;
		snprintf (buffer, sizeof (buffer), "height=%u\n", state.extent.height); text += buffer;
		snprintf (buffer, sizeof (buffer), "images=%u\n", state.imageCount); text += buffer;
		snprintf (buffer, sizeof (buffer), "frames=%llu\n", (unsigned long long)state.framesPresented); text += buffer;
		snprintf (buffer, sizeof (buffer), "error=%s\n", state.lastError.c_str ()); text += buffer;
		snprintf (buffer, sizeof (buffer), "fallback=%s\n", state.fallbackReason.c_str ()); text += buffer;

		return vulkan::CopyOut (text, out, capacity);

	}

	const char* lime_vulkan_status_text (void) {

		static char buffer[2048];
		lime_vulkan_status (buffer, (int)sizeof (buffer));
		return buffer;

	}

	int lime_vulkan_self_test (int frames, int width, int height, char* out, int capacity) {

		if (frames <= 0) frames = 60;
		if (width <= 0) width = 640;
		if (height <= 0) height = 480;

		std::string report;
		char buffer[256];

		void* window = vulkan::CreateHiddenWindow (width, height);

		if (!window) {

			return vulkan::CopyOut ("status=fail\nstage=window\nreason=CreateWindow failed\n", out, capacity);

		}

		if (!vulkan::AttachDevice (window, width, height)) {

			snprintf (buffer, sizeof (buffer), "status=fail\nstage=attach\nreason=%s\n", vulkan::S ().lastError.c_str ());
			report = buffer;
			vulkan::DestroyHiddenWindow (window);
			return vulkan::CopyOut (report, out, capacity);

		}

		auto start = std::chrono::high_resolution_clock::now ();
		int presented = 0;
		bool ok = true;

		for (int i = 0; i < frames; ++i) {

			float t = (float)i * 0.016f;
			const float clear[4] = { 0.05f, 0.07f, 0.10f, 1.0f };

			if (!vulkan::PresentFrame (clear, t)) { ok = false; break; }

			presented++;

		}

		if (vulkan::S ().device != VK_NULL_HANDLE) vulkan::S ().dev.DeviceWaitIdle (vulkan::S ().device);

		auto end = std::chrono::high_resolution_clock::now ();
		double elapsedMs = std::chrono::duration<double, std::milli>(end - start).count ();

		snprintf (buffer, sizeof (buffer), "status=%s\n", ok ? "pass" : "fail"); report += buffer;
		snprintf (buffer, sizeof (buffer), "frames=%d\n", presented); report += buffer;
		snprintf (buffer, sizeof (buffer), "requested=%d\n", frames); report += buffer;
		snprintf (buffer, sizeof (buffer), "device=%s\n", vulkan::S ().deviceName.c_str ()); report += buffer;
		snprintf (buffer, sizeof (buffer), "totalMs=%.1f\n", elapsedMs); report += buffer;
		snprintf (buffer, sizeof (buffer), "avgMs=%.3f\n", presented > 0 ? elapsedMs / presented : 0.0); report += buffer;
		snprintf (buffer, sizeof (buffer), "fps=%.1f\n", elapsedMs > 0 ? (double)presented * 1000.0 / elapsedMs : 0.0); report += buffer;

		if (!ok) {

			snprintf (buffer, sizeof (buffer), "reason=%s\n", vulkan::S ().lastError.c_str ()); report += buffer;

		}

		vulkan::DetachDevice ();
		vulkan::DestroyHiddenWindow (window);

		return vulkan::CopyOut (report, out, capacity);

	}

	void lime_vulkan_set_dialog_enabled (int enabled) {

		vulkan::S ().dialogEnabled = enabled != 0;

	}

	void lime_vulkan_notify_fallback (const char* reason) {

		vulkan::S ().fallbackReason = reason ? reason : "";
		vulkan::Log ("fallback: %s", vulkan::S ().fallbackReason.c_str ());

		#if defined(LIME_VULKAN_WINDOWS)
		if (vulkan::S ().dialogEnabled) {
			std::string message = "Vulkan initialization failed, falling back to OpenGL.\n\nReason: ";
			message += vulkan::S ().fallbackReason;
			message += "\n\nThe game keeps running with OpenGL.";

			MessageBoxA (NULL, message.c_str (), "lime-vulkan: fell back to OpenGL", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);

		}
		#endif

	}

	// ---------------------------------------------------------------------
	// Textures and batches
	// ---------------------------------------------------------------------

	/**
	 * 0xAARRGGBB (the order OpenFL uses for tint) to the byte order of the vertex
	 * attribute.
	 *
	 * The attribute is VK_FORMAT_R8G8B8A8_UNORM, so on a little endian machine the
	 * first byte in memory is R and a uint32 has to be read as 0xAABBGGRR. Without
	 * this swap red and blue trade places (measured: red draws blue, yellow draws
	 * cyan). Using B8G8R8A8_UNORM for the attribute is not an option, that format is
	 * not in the list Vulkan requires vertex attributes to support.
	 */
	static inline unsigned int ToVertexColor (unsigned int argb) {

		return (argb & 0xFF00FF00u) | ((argb & 0x00FF0000u) >> 16) | ((argb & 0x000000FFu) << 16);

	}

	/** Canvas transform: x' = a*x + c*y + tx, y' = b*x + d*y + ty (OpenFL Matrix order). */
	static inline void ApplyView (const vulkan::Batch& batch, float* x, float* y) {

		if (!batch.hasView) return;

		const float px = *x;
		const float py = *y;

		*x = batch.view[0] * px + batch.view[2] * py + batch.view[4];
		*y = batch.view[1] * px + batch.view[3] * py + batch.view[5];

	}

	/** 0..1 to 0..255, rounded and clamped. */
	static inline unsigned int ToByte (double value) {

		const double scaled = value * 255.0 + 0.5;

		if (scaled <= 0.0) return 0;
		if (scaled >= 255.0) return 255;

		return (unsigned int)scaled;

	}

	static inline unsigned int PackColor (double alpha, double red, double green, double blue) {

		return (ToByte (alpha) << 24) | (ToByte (red) << 16) | (ToByte (green) << 8) | ToByte (blue);

	}

	/** One channel of an additive colour offset: 0..255 units -> signed byte (SNORM). */
	static inline unsigned int ToSignedByte (double units) {

		const double scaled = units * (127.0 / 255.0);

		if (scaled <= -127.0) return 0x81u;
		if (scaled >= 127.0) return 0x7Fu;

		return (unsigned int)((int)(scaled < 0.0 ? scaled - 0.5 : scaled + 0.5) & 0xFF);

	}

	/**
	 * OpenFL colour offset (redOffset / greenOffset / blueOffset / alphaOffset, each
	 * in -255..255) -> the vertex attribute bytes, in the same byte order as the
	 * packed colour so one memcpy layout serves both attributes.
	 */
	static inline unsigned int PackColorOffset (double red, double green, double blue, double alpha) {

		// On a little endian machine the lowest byte is the first one in memory, and
		// the attribute is R,G,B,A, so red goes last in the shift chain.
		const unsigned int r = ToSignedByte (red);
		const unsigned int g = ToSignedByte (green);
		const unsigned int b = ToSignedByte (blue);
		const unsigned int a = ToSignedByte (alpha);

		return (a << 24) | (b << 16) | (g << 8) | r;

	}

	lime_vulkan_texture_handle lime_vulkan_texture_create (const void* rows, int width, int height, int flags) {

		return vulkan::CreateTexture (rows, width, height, flags);

	}

	int lime_vulkan_texture_update (lime_vulkan_texture_handle texture, const void* rows, int flags) {

		return vulkan::UpdateTexture ((uint64_t)texture, rows, flags);

	}

	void lime_vulkan_texture_retire (lime_vulkan_texture_handle texture) {

		vulkan::RetireTexture ((uint64_t)texture);

	}

	void lime_vulkan_texture_destroy (lime_vulkan_texture_handle texture) {

		vulkan::DestroyTexture (texture);

	}

	int lime_vulkan_texture_size (lime_vulkan_texture_handle texture, int* width, int* height) {

		vulkan::Texture* found = vulkan::FindTexture (texture);

		if (!found) return 0;

		if (width) *width = found->width;
		if (height) *height = found->height;

		return 1;

	}

	int lime_vulkan_batch_begin (int screenWidth, int screenHeight, const float clearColor[4]) {

		vulkan::State& state = vulkan::S ();
		vulkan::Batch& batch = state.batch;

		if (state.inFrame) {

			vulkan::SetError ("lime_vulkan_batch_begin called while a frame is already open");
			return 0;

		}

		if (!state.attached) {

			vulkan::SetError ("lime_vulkan_batch_begin without an attached context");
			return 0;

		}

		// The client area is the only authoritative answer to 'is there anywhere to
		// draw right now'. A minimised window reports 0x0, and the right response is
		// to skip the frame, not to fail: the screen keeps the last image and the
		// window recovers by itself. This used to report an error that the caller
		// treated as fatal, so one minimise looked like a dead game.
		int clientWidth = 0;
		int clientHeight = 0;

		if (state.window != NULL
			&& vulkan::WindowClientSize (state.window, &clientWidth, &clientHeight)
			&& (clientWidth <= 0 || clientHeight <= 0)) {

			vulkan::SetError ("window has no client area (minimized); frame skipped");
			return LIME_VULKAN_FRAME_SKIPPED;

		}

		if (screenWidth <= 0) screenWidth = (int)state.extent.width;
		if (screenHeight <= 0) screenHeight = (int)state.extent.height;

		if (screenWidth <= 0 || screenHeight <= 0) {

			vulkan::SetError ("lime_vulkan_batch_begin: no usable screen size");
			return 0;

		}

		// The last frame did not fit, so grow before this one starts; a frame
		// boundary is the only safe moment. Growing by what was actually missing
		// converges faster than doubling and wastes less memory.
		if (batch.overflowed) {

			batch.overflowed = false;

			uint32_t target = batch.vertexCapacity * 2;

			if (batch.vertexCapacity + batch.droppedVertices > target) {

				target = batch.vertexCapacity + batch.droppedVertices;

			}

			if (!vulkan::EnsureBatchCapacity (target)) return 0;

		}

		batch.droppedVertices = 0;
		batch.droppedColorOffsets = 0;
		batch.droppedSegments = 0;
		batch.drawnVertices = 0;
		batch.bindTextures = 0;
		batch.bindPipelines = 0;
		batch.bindScissors = 0;
		batch.drawRecordCount = 0;
		batch.noteCount = 0;

		if (!vulkan::BeginFrame (clearColor)) {

			// Rebuild failed but the window has no client area: still 'minimised', not a failure.
			if (state.window != NULL
				&& vulkan::WindowClientSize (state.window, &clientWidth, &clientHeight)
				&& (clientWidth <= 0 || clientHeight <= 0)) {

				vulkan::SetError ("window has no client area (minimized); frame skipped");
				return LIME_VULKAN_FRAME_SKIPPED;

			}

			return 0;

		}

		batch.verticesWritten = 0;
		batch.batchFirstVertex = 0;
		batch.segmentQuads = 0;
		batch.segmentIndexed = true;
		batch.drawCalls = 0;
		batch.frameVertices = 0;
		batch.currentTexture = state.whiteTexture;
		batch.currentFilter = vulkan::FILTER_LINEAR;
		batch.currentBlend = LIME_VULKAN_BLEND_NORMAL;
		batch.scissorX = 0;
		batch.scissorY = 0;
		batch.scissorWidth = 0;
		batch.scissorHeight = 0;

		// Pixel space (origin top left, y down) to Vulkan NDC.
		// Vulkan's y axis is the opposite of OpenGL's: y = -1 is the top edge in NDC,
		// hence +2/H and -1 here. Writing -2/H would flip the whole image.
		batch.projection[0] = 2.0f / (float)screenWidth;
		batch.projection[1] = 2.0f / (float)screenHeight;
		batch.projection[2] = -1.0f;
		batch.projection[3] = -1.0f;

		// The window pass reopens with the same projection after an offscreen
		// stretch, so the size it was opened with is kept here.
		state.frameWidth = screenWidth;
		state.frameHeight = screenHeight;

		batch.recording = true;
		return 1;

	}

	int lime_vulkan_batch_begin_rgba (int screenWidth, int screenHeight, unsigned int argb) {

		float color[4];

		color[0] = (float)((argb >> 16) & 0xFF) / 255.0f;
		color[1] = (float)((argb >> 8) & 0xFF) / 255.0f;
		color[2] = (float)(argb & 0xFF) / 255.0f;
		color[3] = (float)((argb >> 24) & 0xFF) / 255.0f;

		return lime_vulkan_batch_begin (screenWidth, screenHeight, color);

	}

	void lime_vulkan_batch_set_texture (lime_vulkan_texture_handle texture) {

		vulkan::State& state = vulkan::S ();

		if (!state.batch.recording) return;
		if (state.batch.currentTexture == texture) return;

		vulkan::FlushBatch ();
		state.batch.currentTexture = texture;

	}

	void lime_vulkan_batch_set_filter (int nearest) {

		vulkan::State& state = vulkan::S ();
		vulkan::Batch& batch = state.batch;

		if (!batch.recording) return;

		int filter = nearest ? vulkan::FILTER_NEAREST : vulkan::FILTER_LINEAR;

		if (batch.currentFilter == filter) return;

		vulkan::FlushBatch ();
		batch.currentFilter = filter;

	}

	void lime_vulkan_batch_set_blend (int blend) {

		vulkan::State& state = vulkan::S ();

		if (!state.batch.recording) return;
		if (state.batch.currentBlend == blend) return;

		vulkan::FlushBatch ();
		state.batch.currentBlend = blend;

	}

	void lime_vulkan_batch_set_scissor (int x, int y, int width, int height) {

		vulkan::State& state = vulkan::S ();
		vulkan::Batch& batch = state.batch;

		if (!batch.recording) return;

		if (batch.scissorX == x && batch.scissorY == y && batch.scissorWidth == width && batch.scissorHeight == height) return;

		vulkan::FlushBatch ();

		batch.scissorX = x;
		batch.scissorY = y;
		batch.scissorWidth = width;
		batch.scissorHeight = height;

	}

	void lime_vulkan_batch_set_view (const double* matrix2x3) {

		vulkan::Batch& batch = vulkan::S ().batch;

		if (!matrix2x3) {

			batch.hasView = false;
			return;

		}

		for (int i = 0; i < 6; ++i) {

			batch.view[i] = (float)matrix2x3[i];

		}

		batch.hasView = true;

	}

	void lime_vulkan_batch_get_view (double* matrix2x3) {

		if (!matrix2x3) return;

		vulkan::Batch& batch = vulkan::S ().batch;

		for (int i = 0; i < 6; ++i) {

			matrix2x3[i] = batch.hasView ? (double)batch.view[i] : ((i == 0 || i == 3) ? 1.0 : 0.0);

		}

	}

	int lime_vulkan_batch_quad (float x, float y, float width, float height,
		float u0, float v0, float u1, float v1, unsigned int color) {

		vulkan::State& state = vulkan::S ();
		vulkan::Batch& batch = state.batch;

		if (!batch.recording) return 0;

		if (batch.verticesWritten + 4 > batch.vertexCapacity) {

			vulkan::FlushBatch ();

			// A flush does not free space, the vertex buffer accumulates for the whole
			// frame. When it is really full, mark it and grow at the start of the next
			// frame: this frame draws fewer quads, never crashes, never stalls.
			if (batch.verticesWritten + 4 > batch.vertexCapacity) {

				batch.overflowed = true;
				batch.droppedVertices += 4;

				// Report once per frame: logging every dropped quad floods the console
				// (measured: 50k sprites took the frame rate from 100 to 1).
				if (batch.droppedVertices == 4) {

					vulkan::SetError ("batch vertex buffer full; growing on the next frame");

				}

				return 0;

			}

		}

		lime_vulkan_vertex* target = ((lime_vulkan_vertex*)batch.vertexMapped[state.frameIndex]) + batch.verticesWritten;
		const unsigned int packed = ToVertexColor (color);

		float x0 = x,         y0 = y;
		float x1 = x + width, y1 = y;
		float x2 = x,         y2 = y + height;
		float x3 = x + width, y3 = y + height;

		ApplyView (batch, &x0, &y0);
		ApplyView (batch, &x1, &y1);
		ApplyView (batch, &x2, &y2);
		ApplyView (batch, &x3, &y3);

		// The single quad entry point carries no additive colour offset; SNORM zero is 0.0.
		target[0].x = x0; target[0].y = y0; target[0].u = u0; target[0].v = v0; target[0].color = packed; target[0].colorOffset = 0;
		target[1].x = x1; target[1].y = y1; target[1].u = u1; target[1].v = v0; target[1].color = packed; target[1].colorOffset = 0;
		target[2].x = x2; target[2].y = y2; target[2].u = u0; target[2].v = v1; target[2].color = packed; target[2].colorOffset = 0;
		target[3].x = x3; target[3].y = y3; target[3].u = u1; target[3].v = v1; target[3].color = packed; target[3].colorOffset = 0;

		batch.verticesWritten += 4;
		batch.segmentQuads++;
		batch.frameVertices += 4;
		return 1;

	}


	int lime_vulkan_batch_quads_d (const double* rects, const double* transforms, int quadCount,
		const double* alphas, float uniformAlpha,
		const double* colorMultipliers, const double* colorOffsets) {

		vulkan::State& state = vulkan::S ();
		vulkan::Batch& batch = state.batch;

		if (!batch.recording || !rects || !transforms || quadCount <= 0) return 0;

		const uint32_t needed = (uint32_t)quadCount * 4u;

		if (batch.verticesWritten + needed > batch.vertexCapacity) {

			vulkan::FlushBatch ();

			if (batch.verticesWritten + needed > batch.vertexCapacity) {

				batch.overflowed = true;
				batch.droppedVertices += needed;

				// Once per frame, see the note in lime_vulkan_batch_quad.
				if (batch.droppedVertices == needed) {

					vulkan::SetError ("batch vertex buffer full while uploading a quad batch; growing on the next frame");

				}

				return 0;

			}

		}

		// uv comes from the source rectangle divided by the texture size, looked up once per batch rather than per quad.
		double textureWidth = 1.0;
		double textureHeight = 1.0;

		if (batch.currentTexture) {

			int width = 0;
			int height = 0;

			if (lime_vulkan_texture_size (batch.currentTexture, &width, &height) && width > 0 && height > 0) {

				textureWidth = (double)width;
				textureHeight = (double)height;

			}

		}

		lime_vulkan_vertex* target = ((lime_vulkan_vertex*)batch.vertexMapped[state.frameIndex]) + batch.verticesWritten;
		uint32_t written = 0;

		for (int q = 0; q < quadCount; ++q) {

			const double* rect = rects + (size_t)q * 4u;
			const double* matrix = transforms + (size_t)q * 6u;

			const double sourceX = rect[0];
			const double sourceY = rect[1];
			const double sourceWidth = rect[2];
			const double sourceHeight = rect[3];

			// The corner order has to match lime_vulkan_batch_quad: the index buffer is hard
			// coded as (0,1,2,3), and any other order shifts every uv.
			//
			// The geometry starts at 0, only the source rectangle moves the uv. A rect
			// whose origin is not 0 is the normal case: the engine repacks small images
			// into an atlas (backend.gfx.GfxAtlasRepack), after which rect is a
			// sub-rectangle of the atlas. Adding the source origin as a position would
			// translate the whole quad by that rect, and sprites and text would appear
			// to run off screen.
			const double cornerX[4] = { 0.0, sourceWidth, 0.0, sourceWidth };
			const double cornerY[4] = { 0.0, 0.0, sourceHeight, sourceHeight };
			const double cornerU[4] = { sourceX / textureWidth, (sourceX + sourceWidth) / textureWidth,
				sourceX / textureWidth, (sourceX + sourceWidth) / textureWidth };
			const double cornerV[4] = { sourceY / textureHeight, sourceY / textureHeight,
				(sourceY + sourceHeight) / textureHeight, (sourceY + sourceHeight) / textureHeight };

			for (int k = 0; k < 4; ++k) {

				const double alpha = alphas ? alphas[(size_t)q * 4u + (size_t)k] : (double)uniformAlpha;
				double multiplierRed = 1.0;
				double multiplierGreen = 1.0;
				double multiplierBlue = 1.0;
				double multiplierAlpha = 1.0;
				double offsetRed = 0.0;
				double offsetGreen = 0.0;
				double offsetBlue = 0.0;
				double offsetAlpha = 0.0;

				if (colorMultipliers) {

					const double* color = colorMultipliers + ((size_t)q * 4u + (size_t)k) * 4u;

					multiplierRed = color[0];
					multiplierGreen = color[1];
					multiplierBlue = color[2];
					multiplierAlpha = color[3];

				}

				// OpenFL keeps 4 values per vertex here too, in -255..255 units.
				if (colorOffsets) {

					const double* color = colorOffsets + ((size_t)q * 4u + (size_t)k) * 4u;

					offsetRed = color[0];
					offsetGreen = color[1];
					offsetBlue = color[2];
					offsetAlpha = color[3];

				}

				float x = (float)(matrix[0] * cornerX[k] + matrix[2] * cornerY[k] + matrix[4]);
				float y = (float)(matrix[1] * cornerX[k] + matrix[3] * cornerY[k] + matrix[5]);

				ApplyView (batch, &x, &y);

				lime_vulkan_vertex& vertex = target[written++];

				vertex.x = x;
				vertex.y = y;
				vertex.u = (float)cornerU[k];
				vertex.v = (float)cornerV[k];
				vertex.color = ToVertexColor (PackColor (alpha * multiplierAlpha, multiplierRed, multiplierGreen, multiplierBlue));
				vertex.colorOffset = PackColorOffset (offsetRed, offsetGreen, offsetBlue, offsetAlpha);

			}

		}

		if (batch.noteCount < 16 && written >= 4) {

			vulkan::Batch::QuadNote& note = batch.notes[batch.noteCount++];
			note.x0 = target[0].x; note.y0 = target[0].y;
			note.x1 = target[3].x; note.y1 = target[3].y;
			note.color = target[0].color;

		}

		batch.verticesWritten += written;
		batch.segmentQuads += (uint32_t)quadCount;
		batch.frameVertices += written;
		return 1;

	}

	int lime_vulkan_batch_triangles (const lime_vulkan_vertex* vertices, int vertexCount, const unsigned int* indices, int indexCount) {

		vulkan::State& state = vulkan::S ();
		vulkan::Batch& batch = state.batch;

		if (!batch.recording || !vertices || vertexCount <= 0) return 0;

		// Raw triangles and quad indices are submitted two different ways, so close the current batch first.
		vulkan::FlushBatch ();

		if (indexCount > 0) {

			// With indices, expand into a triangle list: the batch only has to draw here, the index buffer path belongs to the Graphics work.
			if (batch.verticesWritten + indexCount > batch.vertexCapacity) {

				batch.overflowed = true;
				vulkan::SetError ("batch vertex buffer full while expanding indexed triangles");
				return 0;

			}

			lime_vulkan_vertex* target = ((lime_vulkan_vertex*)batch.vertexMapped[state.frameIndex]) + batch.verticesWritten;

			for (int i = 0; i < indexCount; ++i) {

				unsigned int index = indices ? indices[i] : (unsigned int)i;

				if ((int)index >= vertexCount) index = 0;

				target[i] = vertices[index];
				target[i].color = ToVertexColor (vertices[index].color);
				target[i].colorOffset = 0;
				ApplyView (batch, &target[i].x, &target[i].y);

			}

			batch.verticesWritten += (uint32_t)indexCount;
			batch.frameVertices += (uint32_t)indexCount;

		} else {

			if (batch.verticesWritten + (uint32_t)vertexCount > batch.vertexCapacity) {

				batch.overflowed = true;
				vulkan::SetError ("batch vertex buffer full while appending triangles");
				return 0;

			}

			lime_vulkan_vertex* target = ((lime_vulkan_vertex*)batch.vertexMapped[state.frameIndex]) + batch.verticesWritten;
			memcpy (target, vertices, sizeof (lime_vulkan_vertex) * (size_t)vertexCount);

			for (int i = 0; i < vertexCount; ++i) {

				target[i].color = ToVertexColor (target[i].color);
				target[i].colorOffset = 0;
				ApplyView (batch, &target[i].x, &target[i].y);

			}

			batch.verticesWritten += (uint32_t)vertexCount;
			batch.frameVertices += (uint32_t)vertexCount;

		}

		batch.segmentIndexed = false;
		vulkan::FlushBatch ();
		batch.segmentIndexed = true;

		return 1;

	}

	int lime_vulkan_batch_end (void) {

		vulkan::State& state = vulkan::S ();

		if (!state.batch.recording) {

			vulkan::SetError ("lime_vulkan_batch_end without lime_vulkan_batch_begin");
			return 0;

		}

		vulkan::FlushBatch ();
		return vulkan::DrawBatchFrame () ? 1 : 0;

	}

	// ---------------------------------------------------------------------
	// Offscreen render targets
	//
	// A frame can leave the window, draw into a texture and come back to paste the
	// result: that is what a camera filter does (camera content into a target, one
	// pass per filter flipping between two targets, composite back). A target is an
	// ordinary texture handle, sampling it needs no extra API.
	// ---------------------------------------------------------------------

	lime_vulkan_texture_handle lime_vulkan_target_create (int width, int height) {

		return vulkan::CreateRenderTarget (width, height);

	}

	int lime_vulkan_target_valid (lime_vulkan_texture_handle target) {

		vulkan::Texture* texture = vulkan::FindTexture (target);

		return (texture && texture->framebuffer != VK_NULL_HANDLE) ? 1 : 0;

	}

	int lime_vulkan_batch_begin_target (lime_vulkan_texture_handle target, unsigned int argb) {

		vulkan::State& state = vulkan::S ();

		if (!state.inFrame || !state.batch.recording) {

			vulkan::SetError ("lime_vulkan_batch_begin_target without an open frame");
			return 0;

		}

		vulkan::Texture* texture = vulkan::FindTexture (target);

		if (!texture || texture->framebuffer == VK_NULL_HANDLE) {

			vulkan::SetError ("lime_vulkan_batch_begin_target: handle is not a render target");
			return 0;

		}

		if (state.targetPassOpen && state.activeTarget == texture) return 1;

		// Whatever is still queued belongs to the pass that is open right now.
		vulkan::FlushBatch ();
		vulkan::EndActivePass ();

		if (!vulkan::BeginTargetPass (texture, argb)) {

			vulkan::SetError ("could not begin the render target pass");
			return 0;

		}

		return 1;

	}

	int lime_vulkan_batch_end_target (void) {

		vulkan::State& state = vulkan::S ();

		if (!state.inFrame) {

			vulkan::SetError ("lime_vulkan_batch_end_target without an open frame");
			return 0;

		}

		if (!state.targetPassOpen) return 1;

		vulkan::FlushBatch ();
		vulkan::EndTargetPass ();

		return 1;

	}

	int lime_vulkan_batch_stats (char* out, int capacity) {

		vulkan::Batch& batch = vulkan::S ().batch;

		vulkan::State::MemoryTotals& memory = vulkan::S ().memory;

		char text[3072];
		snprintf (text, sizeof (text), "memTex=%.1fMB/%llu memStage=%.1fMB\nmemReserved=%.1fMB\nmemAllocs=%llu\n"
			"drawCalls=%u\nquads=%u\nvertices=%u\ndrawn=%u\ncapacity=%u\ndropped=%u\ndroppedColorOffsets=%u\ndroppedSegments=%u\ntexFallback=%u\n",
			memory.textures / 1048576.0, (unsigned long long)memory.textureCount, memory.staging / 1048576.0,
			memory.reserved / 1048576.0, (unsigned long long)memory.allocations,
			batch.drawCalls, batch.frameVertices / 4, batch.frameVertices, batch.drawnVertices, batch.vertexCapacity,
			batch.droppedVertices, batch.droppedColorOffsets, batch.droppedSegments, batch.fallbackTextures);

		// Frame timings and the numbers a later optimization is judged by. Percentiles
		// are recomputed here, never per frame.
		{
			char perf[1024] = { 0 };

			if (vulkan::PerfText (perf, (int)sizeof (perf)) > 0) {

				size_t used = strlen (text);
				if (used + strlen (perf) + 1 < sizeof (text)) strcat (text, perf);

			}
		}

		// Record of every draw segment, to locate 'which one did not appear'; it lines up with the Haxe submit order.
		for (uint32_t i = 0; i < batch.drawRecordCount; ++i) {

			char line[160];

			snprintf (line, sizeof (line), "draw%u: first=%u quads=%u sc=%d,%d,%dx%d tex=%llu\n",
				i, batch.draws[i].firstVertex, batch.draws[i].quads,
				batch.draws[i].scissorX, batch.draws[i].scissorY,
				batch.draws[i].scissorW, batch.draws[i].scissorH,
				batch.draws[i].texture);

			char verts[200];

			snprintf (verts, sizeof (verts), " v0=(%.1f,%.1f,%.3f,%.3f,%08X) v3=(%.1f,%.1f,%.3f,%.3f,%08X)\n",
				batch.draws[i].vx[0], batch.draws[i].vy[0], batch.draws[i].vu[0], batch.draws[i].vv[0], batch.draws[i].vc[0],
				batch.draws[i].vx[3], batch.draws[i].vy[3], batch.draws[i].vu[3], batch.draws[i].vv[3], batch.draws[i].vc[3]);

			size_t used = strlen (text);
			if (used + strlen (line) + 1 < sizeof (text)) strcat (text, line);
			used = strlen (text);
			if (used + strlen (verts) + 1 < sizeof (text)) strcat (text, verts);

		}

		for (uint32_t i = 0; i < batch.noteCount; ++i) {

			char note[96];

			snprintf (note, sizeof (note), "q%u: (%.1f,%.1f)-(%.1f,%.1f) c=%08X\n",
				i, batch.notes[i].x0, batch.notes[i].y0, batch.notes[i].x1, batch.notes[i].y1, batch.notes[i].color);

			size_t used = strlen (text);
			if (used + strlen (note) + 1 < sizeof (text)) strcat (text, note);

		}

		return vulkan::CopyOut (text, out, capacity);

	}

	const char* lime_vulkan_batch_stats_text (void) {

		static char buffer[2048];
		lime_vulkan_batch_stats (buffer, (int)sizeof (buffer));
		return buffer;

	}

	void lime_vulkan_set_pattern (int pattern) {

		vulkan::S ().pattern = pattern;

	}

	int lime_vulkan_get_pattern (void) {

		return vulkan::S ().pattern;

	}

	unsigned int lime_vulkan_process_id (const char* exeName) {

		return vulkan::FindProcessIdByName (exeName);

	}

	void* lime_vulkan_process_window (unsigned int pid) {

		return vulkan::FindProcessWindow (pid);

	}

	void* lime_vulkan_self_window (void) {

		return vulkan::SelfWindow ();

	}

	int lime_vulkan_release_gl (void) {

#if defined(LIME_VULKAN_WINDOWS)

		// Resolved at run time: this library never links opengl32 itself.
		HMODULE opengl = GetModuleHandleA ("opengl32.dll");

		if (opengl == NULL) {

			vulkan::SetError ("opengl32.dll is not loaded in this process");
			return 2;

		}

		typedef void* (__stdcall * GetCurrentContextProc) (void);
		typedef int (__stdcall * MakeCurrentProc) (void*, void*);
		typedef int (__stdcall * DeleteContextProc) (void*);

		GetCurrentContextProc getCurrent = (GetCurrentContextProc)GetProcAddress (opengl, "wglGetCurrentContext");
		MakeCurrentProc makeCurrent = (MakeCurrentProc)GetProcAddress (opengl, "wglMakeCurrent");
		DeleteContextProc deleteContext = (DeleteContextProc)GetProcAddress (opengl, "wglDeleteContext");

		if (getCurrent == NULL || makeCurrent == NULL || deleteContext == NULL) {

			vulkan::SetError ("opengl32.dll does not export the wgl context entry points");
			return 2;

		}

		void* context = getCurrent ();

		if (context == NULL) return 1;

		makeCurrent (NULL, NULL);

		if (!deleteContext (context)) {

			vulkan::SetError ("wglDeleteContext failed");
			return 3;

		}

		return 0;

#else

		return 1;

#endif

	}

	int lime_vulkan_window_client_size (void* nativeWindow, int* width, int* height) {

		return vulkan::WindowClientSize (nativeWindow, width, height) ? 1 : 0;

	}

	int lime_vulkan_window_title (void* nativeWindow, char* out, int capacity) {

		return vulkan::WindowTitle (nativeWindow, out, capacity);

	}

	int lime_vulkan_process_suspend (unsigned int pid) {

		return vulkan::SuspendProcess (pid);

	}

	int lime_vulkan_process_resume (unsigned int pid) {

		return vulkan::ResumeProcess (pid);

	}

	// ---------------------------------------------------------------------
	// Shader programs
	// ---------------------------------------------------------------------

	lime_vulkan_program lime_vulkan_program_create (const lime_vulkan_program_desc* desc) {

		if (!desc || !desc->vertexSource || !desc->fragmentSource) {

			vulkan::SetError ("lime_vulkan_program_create: missing shader source");
			return 0;

		}

		return vulkan::CreateShaderProgram (desc->vertexSource, desc->fragmentSource, desc->flags, desc->cacheDirectory);

	}

	void lime_vulkan_program_destroy (lime_vulkan_program program) {

		vulkan::DestroyShaderProgram (program);

	}

	int lime_vulkan_program_valid (lime_vulkan_program program) {

		return vulkan::FindProgram (program) ? 1 : 0;

	}

	const char* lime_vulkan_program_log (lime_vulkan_program program) {

		vulkan::ShaderProgram* found = vulkan::FindProgram (program);

		// Handle 0 (creation failed) returns the details of the last failure, otherwise the caller sees nothing.
		if (!found) return vulkan::LastProgramFailure ();

		return found->log.c_str();

	}

	int lime_vulkan_program_uniform_count (lime_vulkan_program program) {

		vulkan::ShaderProgram* found = vulkan::FindProgram (program);

		return found ? (int)found->uniforms.size() : 0;

	}

	int lime_vulkan_program_uniform_info (lime_vulkan_program program, int index, lime_vulkan_uniform_info* info) {

		vulkan::ShaderProgram* found = vulkan::FindProgram (program);

		if (!found || !info) return 0;
		if (index < 0 || (size_t)index >= found->uniforms.size()) return 0;

		const vulkan::glsl::Variable& variable = found->uniforms[(size_t)index];

		info->name = variable.name.c_str();
		info->type = variable.type;
		info->offset = variable.offset;
		info->size = variable.size;
		info->count = variable.arraySize;

		return 1;

	}

	int lime_vulkan_program_set_float (lime_vulkan_program program, const char* name, float value) {

		void* slot = vulkan::FindProgramUniform (vulkan::FindProgram (program), name, vulkan::glsl::TYPE_FLOAT);

		if (!slot) return 0;

		memcpy (slot, &value, sizeof (value));
		return 1;

	}

	int lime_vulkan_program_set_int (lime_vulkan_program program, const char* name, int value) {

		vulkan::ShaderProgram* found = vulkan::FindProgram (program);

		// bool and int are both 32 bits: in GL a uniform bool is an integer anyway.
		void* slot = vulkan::FindProgramUniform (found, name, vulkan::glsl::TYPE_INT);

		if (!slot) slot = vulkan::FindProgramUniform (found, name, vulkan::glsl::TYPE_BOOL);
		if (!slot) return 0;

		memcpy (slot, &value, sizeof (value));
		return 1;

	}

	namespace {

		int SetVector (lime_vulkan_program program, const char* name, const float* value, int componentCount) {

			vulkan::ShaderProgram* found = vulkan::FindProgram (program);

			if (!found || !value) return 0;

			int type = vulkan::glsl::TYPE_VEC2 + (componentCount - 2);

			void* slot = vulkan::FindProgramUniform (found, name, type);

			if (!slot) return 0;

			memcpy (slot, value, sizeof (float) * (size_t)componentCount);
			return 1;

		}

	}

	int lime_vulkan_program_set_vec2 (lime_vulkan_program program, const char* name, const float* value) {

		return SetVector (program, name, value, 2);

	}

	int lime_vulkan_program_set_vec3 (lime_vulkan_program program, const char* name, const float* value) {

		return SetVector (program, name, value, 3);

	}

	int lime_vulkan_program_set_vec4 (lime_vulkan_program program, const char* name, const float* value) {

		return SetVector (program, name, value, 4);

	}

	int lime_vulkan_program_set_mat4 (lime_vulkan_program program, const char* name, const float* value) {

		void* slot = vulkan::FindProgramUniform (vulkan::FindProgram (program), name, vulkan::glsl::TYPE_MAT4);

		if (!slot || !value) return 0;

		memcpy (slot, value, sizeof (float) * 16);
		return 1;

	}

	int lime_vulkan_program_set_float_array (lime_vulkan_program program, const char* name, const float* values, int count) {

		vulkan::ShaderProgram* found = vulkan::FindProgram (program);

		if (!found || !name || !values || count <= 0) return 0;

		std::map<std::string, int>::iterator position = found->uniformIndex.find (name);

		if (position == found->uniformIndex.end()) return 0;

		const vulkan::glsl::Variable& variable = found->uniforms[(size_t)position->second];

		if (variable.arraySize <= 0 || variable.stride < 4) return 0;

		if (count > variable.arraySize) count = variable.arraySize;

		uint32_t components = vulkan::glsl::TypeSize (variable.type) / 4;

		if (components == 0) return 0;

		float* target = (float*)&found->shadow[variable.offset];
		uint32_t stride = variable.stride / 4;

		for (int i = 0; i < count; ++i) {

			for (uint32_t c = 0; c < components; ++c) {

				target[(size_t)i * stride + c] = values[(size_t)i * components + c];

			}

		}

		return 1;

	}

	int lime_vulkan_program_set_texture (lime_vulkan_program program, const char* name, lime_vulkan_texture_handle texture) {

		vulkan::ShaderProgram* found = vulkan::FindProgram (program);

		if (!found || !name) return 0;

		std::map<std::string, int>::iterator position = found->samplerIndex.find (name);

		if (position == found->samplerIndex.end()) return 0;

		found->samplerTextures[position->second] = texture;
		return 1;

	}

	int lime_vulkan_program_set_sampler_texture (lime_vulkan_program program, int index, lime_vulkan_texture_handle texture) {

		vulkan::ShaderProgram* found = vulkan::FindProgram (program);

		if (!found || index < 0 || (size_t)index >= found->samplers.size()) return 0;

		found->samplerTextures[index] = texture;
		return 1;

	}

	lime_vulkan_program lime_vulkan_program_create_flat (const char* vertexSource, const char* fragmentSource,
		unsigned int flags, const char* cacheDirectory) {

		lime_vulkan_program_desc desc;
		desc.vertexSource = vertexSource;
		desc.fragmentSource = fragmentSource;
		desc.flags = flags;
		desc.cacheDirectory = cacheDirectory;

		return lime_vulkan_program_create (&desc);

	}

	namespace {

		const vulkan::glsl::Variable* UniformAt (lime_vulkan_program program, int index) {

			vulkan::ShaderProgram* found = vulkan::FindProgram (program);

			if (!found || index < 0 || (size_t)index >= found->uniforms.size()) return NULL;

			return &found->uniforms[(size_t)index];

		}

	}

	const char* lime_vulkan_program_uniform_name (lime_vulkan_program program, int index) {

		const vulkan::glsl::Variable* variable = UniformAt (program, index);

		return variable ? variable->name.c_str() : NULL;

	}

	int lime_vulkan_program_uniform_type (lime_vulkan_program program, int index) {

		const vulkan::glsl::Variable* variable = UniformAt (program, index);

		return variable ? (int)variable->type : 0;

	}

	unsigned int lime_vulkan_program_uniform_offset (lime_vulkan_program program, int index) {

		const vulkan::glsl::Variable* variable = UniformAt (program, index);

		return variable ? variable->offset : 0;

	}

	unsigned int lime_vulkan_program_uniform_size (lime_vulkan_program program, int index) {

		const vulkan::glsl::Variable* variable = UniformAt (program, index);

		return variable ? variable->size : 0;

	}

	int lime_vulkan_program_uniform_length (lime_vulkan_program program, int index) {

		const vulkan::glsl::Variable* variable = UniformAt (program, index);

		return variable ? variable->arraySize : 0;

	}

	int lime_vulkan_program_sampler_count (lime_vulkan_program program) {

		vulkan::ShaderProgram* found = vulkan::FindProgram (program);

		return found ? (int)found->samplers.size() : 0;

	}

	const char* lime_vulkan_program_sampler_name (lime_vulkan_program program, int index) {

		vulkan::ShaderProgram* found = vulkan::FindProgram (program);

		if (!found || index < 0 || (size_t)index >= found->samplers.size()) return NULL;

		return found->samplers[(size_t)index].name.c_str();

	}

	int lime_vulkan_program_write_floats (lime_vulkan_program program, unsigned int offset, const double* values, int count) {

		vulkan::ShaderProgram* found = vulkan::FindProgram (program);

		if (!found || !values || count <= 0) return 0;

		const size_t bytes = (size_t)count * sizeof (float);

		if ((size_t)offset + bytes > found->shadow.size()) return 0;

		float* target = (float*)&found->shadow[offset];

		for (int i = 0; i < count; ++i) {

			target[i] = (float)values[i];

		}

		return 1;

	}

	int lime_vulkan_program_write_ints (lime_vulkan_program program, unsigned int offset, const int* values, int count) {

		vulkan::ShaderProgram* found = vulkan::FindProgram (program);

		if (!found || !values || count <= 0) return 0;

		const size_t bytes = (size_t)count * sizeof (int);

		if ((size_t)offset + bytes > found->shadow.size()) return 0;

		memcpy (&found->shadow[offset], values, bytes);
		return 1;

	}

	void lime_vulkan_batch_set_program (lime_vulkan_program program) {

		vulkan::State& state = vulkan::S ();

		if (state.batch.program == program) return;

		// Switching programs switches pipelines, so submit whatever has been accumulated first.
		vulkan::FlushBatch ();

		state.batch.program = program;

	}

	lime_vulkan_program lime_vulkan_batch_get_program (void) {

		return vulkan::S ().batch.program;

	}

	const char* lime_vulkan_version (void) {

		return "0.4.0";

	}

}
