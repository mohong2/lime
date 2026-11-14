#ifndef LIME_VULKAN_H
#define LIME_VULKAN_H

/**
 * lime-vulkan - private Vulkan backend (experimental)
 *
 * Design rules, each one has a reason:
 *
 *  1. Never link libvulkan. Entry points are resolved at runtime from
 *     vulkan-1.dll / libvulkan.so.1, so a machine without a driver reports
 *     'unavailable' instead of failing to load the process.
 *  2. No dependency on SDL and none on lime. The caller passes the window
 *     handle (HWND on Win32), so the same dll / lib serves the game and the
 *     standalone demo.
 *  3. No failure aborts: return 0, write the reason into lime_vulkan_status(), and
 *     let the caller decide how to fall back.
 *  4. A fallback must be visible: lime_vulkan_notify_fallback() shows a dialog.
 *
 * This file is the public ABI. Any change here must be mirrored in the Haxe
 * side, haxe/seiun/vulkan/VkExtern.hx.
 */

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#define LIME_VULKAN_API __declspec(dllexport)
#else
#define LIME_VULKAN_API __attribute__((visibility("default")))
#endif

	/** Whether the backend is usable: a loader and at least one device. No window is created. */
	LIME_VULKAN_API int lime_vulkan_available (void);

	/**
	 * Text probe report, one key=value per line.
	 * @return bytes written, excluding the trailing 0; when the buffer is too
	 *         small the required length is returned instead.
	 */
	LIME_VULKAN_API int lime_vulkan_probe (char* out, int capacity);

	typedef struct lime_vulkan_options {

		int deviceIndex;		/**< -1 = automatic (discrete GPU first) */
		int presentMode;		/**< 0 = fifo, 1 = mailbox, 2 = immediate */
		int enableValidation; /**< needs VK_LAYER_KHRONOS_validation installed */
		int verbose;		  /**< detailed log */
		int fallbackDialog;   /**< show a dialog on fallback (default 1) */

	} lime_vulkan_options;

	LIME_VULKAN_API void lime_vulkan_default_options (lime_vulkan_options* options);

	/**
	 * Creates the instance, device and swapchain on the given native window.
	 *
	 * @param nativeWindow HWND on Windows; other platforms use lime_vulkan_platform.h
	 * @return 1 on success, 0 on failure (reason in lime_vulkan_status)
	 */
	LIME_VULKAN_API int lime_vulkan_context_create (void* nativeWindow, int width, int height, const lime_vulkan_options* options);
	LIME_VULKAN_API void lime_vulkan_context_destroy (void);
	LIME_VULKAN_API int lime_vulkan_context_attached (void);

	/**
	 * Releases the OpenGL context that the window backend made current, so the
	 * driver can take its memory back. Only meaningful after lime_vulkan_context_create
	 * succeeded: the presenting swapchain owns the window from that point.
	 *
	 * @return 0 released; 1 no OpenGL context was current; 2 opengl32.dll is
	 *         missing; 3 the driver refused to delete the context
	 */
	LIME_VULKAN_API int lime_vulkan_release_gl (void);

	/**
	 * Safe probe: creates the instance, surface and device on this window but
	 * never creates a swapchain.
	 *
	 * This is the decision point for 'switch to Vulkan or not'.
	 *
	 * The distinction matters: vkCreateSwapchainKHR hands the window to the DXGI
	 * compositor, and from that moment OpenGL's GDI presentation is gone
	 * (measured, and irreversible for that window). So the order has to be
	 * probe (may fail) -> decide -> create the swapchain (no way back), instead
	 * of creating first and asking later whether to fall back.
	 *
	 * @return 1 when the switch is safe; 0 when it is not (see the error= line
	 *         of lime_vulkan_status)
	 */
	LIME_VULKAN_API int lime_vulkan_probe_window (void* nativeWindow, int width, int height, const lime_vulkan_options* options);

	/** The swapchain size changed (call after a resize, rebuilt on the next frame). */
	LIME_VULKAN_API int lime_vulkan_resize (int width, int height);

	/** Submits one frame: optional clear colour plus the 2D batch the caller built. */
	LIME_VULKAN_API int lime_vulkan_present (const float clearColor[4]);

	// ---------------------------------------------------------------------
	// 2D batches
	//
	// This group mirrors the four things the OpenFL renderer does: bindTexture,
	// blendFunc, scissor and drawElements. The caller issues them in display list
	// order and the batch splits draw calls on state changes, so ten thousand
	// sprites usually cost one to three submissions.
	//
	// Coordinates match OpenFL: pixels, origin top left, y pointing down.
	// ---------------------------------------------------------------------

	typedef unsigned long long lime_vulkan_texture_handle; /**< 0 means invalid */

	typedef enum lime_vulkan_texture_flags {

		LIME_VULKAN_TEXTURE_NONE = 0,
		LIME_VULKAN_TEXTURE_MIPMAPS = 1, /**< build a mip chain (off by default, like OpenFL bitmaps) */
		LIME_VULKAN_TEXTURE_REPEAT = 2,  /**< clamp to edge by default */
		LIME_VULKAN_TEXTURE_NEAREST = 4, /**< linear filtering by default */

		/**
		 * Byte order is BGRA, which is what lime decodes to on Windows. The upload
		 * swaps R and B once instead of making callers guess the format per frame.
		 */
		LIME_VULKAN_TEXTURE_BGRA = 8,

		/**
		 * The data is premultiplied. The upload converts it back to straight alpha
		 * so the pipeline has a single blend meaning (SRC_ALPHA over
		 * ONE_MINUS_SRC_ALPHA) instead of per-texture state.
		 */
		LIME_VULKAN_TEXTURE_UNPREMULTIPLY = 16,

		/**
		 * One byte per pixel: a palette index, uploaded as R8_UNORM. The shader
		 * expands it through a 256x1 palette texture, so the image costs a
		 * quarter of the RGBA footprint.
		 */
		LIME_VULKAN_TEXTURE_INDEX8 = 32

	} lime_vulkan_texture_flags;

	/**
	 * Vertex layout: 24 bytes (pos.xy + uv.xy + 0xAARRGGBB + additive colour offset).
	 *
	 * colorOffset is the offset half of the OpenFL / flixel colorTransform
	 * (redOffset and friends, 0..255 units, either sign). It cannot be folded
	 * into color: the vertex colour multiplies, the offset adds, and the colour
	 * transform contract is color * multiplier + offset. Dropping this component
	 * made every 'brighten or darken the whole layer' effect (the BnW style
	 * events are the usual case) work in GL and do nothing here.
	 */
	typedef struct lime_vulkan_vertex {

		float x, y;
		float u, v;
		unsigned int color;

		/** One signed byte per channel, memory order R,G,B,A; 127 == +1.0. */
		unsigned int colorOffset;

	} lime_vulkan_vertex;

	typedef enum lime_vulkan_blend {

		LIME_VULKAN_BLEND_NORMAL = 0,  /**< srcAlpha / oneMinusSrcAlpha */
		LIME_VULKAN_BLEND_ADD = 1,     /**< srcAlpha / one */
		LIME_VULKAN_BLEND_MULTIPLY = 2,/**< dstColor / oneMinusSrcAlpha */
		LIME_VULKAN_BLEND_SCREEN = 3,  /**< one / oneMinusSrcColor */
		LIME_VULKAN_BLEND_ERASE = 4,   /**< zero / oneMinusSrcAlpha */
		LIME_VULKAN_BLEND_NONE = 5     /**< one / zero (plain overwrite) */

	} lime_vulkan_blend;

	/**
	 * Uploads an RGBA8 texture (tightly packed, no row padding).
	 * @param rows pixel data; NULL allocates an empty texture of that size
	 * @return handle, or 0 on failure (reason in lime_vulkan_status)
	 */
	LIME_VULKAN_API lime_vulkan_texture_handle lime_vulkan_texture_create (const void* rows, int width, int height, int flags);
	/**
	 * Retire: hands the texture to the native side, which frees it a few frames later.
	 *
	 * Dropping a texture has to go through this and never through
	 * lime_vulkan_texture_destroy: a descriptor set of that texture may still be
	 * referenced by a submitted command buffer, and freeing it right away is
	 * undefined behaviour (validation vkFreeDescriptorSets-00309, device lost on
	 * NVIDIA). A retired handle must not be bound again.
	 */
	LIME_VULKAN_API void lime_vulkan_texture_retire (lime_vulkan_texture_handle texture);

	/**
	 * Overwrites the pixels of an existing texture (same size).
	 *
	 * Creating a texture costs an allocation, an image view and a descriptor, and
	 * text and overlay bitmaps are repainted every frame; this path skips the
	 * allocation.
	 *
	 * @return 1 on success; 0 when the handle is invalid or the size differs
	 *         (the caller should recreate the texture)
	 */
	LIME_VULKAN_API int lime_vulkan_texture_update (lime_vulkan_texture_handle texture, const void* rows, int flags);

	LIME_VULKAN_API void lime_vulkan_texture_destroy (lime_vulkan_texture_handle texture);
	LIME_VULKAN_API int lime_vulkan_texture_size (lime_vulkan_texture_handle texture, int* width, int* height);

	/** The three outcomes of lime_vulkan_batch_begin. */
	#define LIME_VULKAN_FRAME_FAILED 0  /**< real failure: see lime_vulkan_status, the caller should fall back */
	#define LIME_VULKAN_FRAME_OPEN 1    /**< the frame is open and can draw */
	#define LIME_VULKAN_FRAME_SKIPPED 2 /**< nowhere to draw (0x0 client area, usually minimised): not an
	                                 error, draw nothing and do not end the frame, the screen
	                                 keeps the last image */

	/**
	 * Starts a frame: clear colour plus screen size (which sets the projection).
	 * @return LIME_VULKAN_FRAME_OPEN / LIME_VULKAN_FRAME_SKIPPED / LIME_VULKAN_FRAME_FAILED
	 *
	 * SKIPPED and FAILED must stay separate: minimising is normal and players do
	 * it all the time. Treating it as a failure once is enough to make
	 * 'minimise' look like 'crash'.
	 */
	LIME_VULKAN_API int lime_vulkan_batch_begin (int screenWidth, int screenHeight, const float clearColor[4]);

	/** The five setters below only split a draw call when the state really changes; repeats are free. */
	LIME_VULKAN_API void lime_vulkan_batch_set_texture (lime_vulkan_texture_handle texture);

	/**
	 * Sampling filter: nearest when nonzero, linear otherwise.
	 * Pixel art needs nearest, since linear turns magnified pixels into mush.
	 * Linear is the default, matching texture creation.
	 */
	LIME_VULKAN_API void lime_vulkan_batch_set_filter (int nearest);

	LIME_VULKAN_API void lime_vulkan_batch_set_blend (int blend);
	LIME_VULKAN_API void lime_vulkan_batch_set_scissor (int x, int y, int width, int height);

	/** Appends one textured quad. Returns 1 on success, 0 when the buffer cannot grow. */
	LIME_VULKAN_API int lime_vulkan_batch_quad (float x, float y, float width, float height,
		float u0, float v0, float u1, float v1, unsigned int color);

	/** Appends a triangle list with its own uv and colour. Indices are relative to this batch. */
	LIME_VULKAN_API int lime_vulkan_batch_triangles (const lime_vulkan_vertex* vertices, int vertexCount,
		const unsigned int* indices, int indexCount);

	/** Ends a frame: flush, submit and present. Returns 1 when the frame really reached the screen. */
	LIME_VULKAN_API int lime_vulkan_batch_end (void);

	// ---------------------------------------------------------------------
	// Offscreen render targets (camera filters)
	//
	// A target is a texture usable as a colour attachment: the handle still works
	// with lime_vulkan_batch_set_texture / lime_vulkan_program_set_texture for sampling, and with
	// lime_vulkan_texture_size / lime_vulkan_texture_retire. The format follows the swapchain, so
	// existing pipelines work in both paths without a second set of pipelines.
	// ---------------------------------------------------------------------

	/** @return texture handle, or 0 on failure (reason in lime_vulkan_status). */
	LIME_VULKAN_API lime_vulkan_texture_handle lime_vulkan_target_create (int width, int height);

	/** Whether this is a render target (a plain texture returns 0). */
	LIME_VULKAN_API int lime_vulkan_target_valid (lime_vulkan_texture_handle target);

	/**
	 * Later batches draw into this target (cleared to argb, projection from the
	 * target size).
	 *
	 * argb is the layer background: a flixel camera fills its canvas with bgColor
	 * before drawing, and the filters see 'background + sprites', so the caller
	 * supplies it; a fixed transparent clear would be wrong.
	 *
	 * The window pass is closed first, the two cannot be open at the same time.
	 * @return 1 on success, 0 when the handle is not a target or no frame is open
	 */
	LIME_VULKAN_API int lime_vulkan_batch_begin_target (lime_vulkan_texture_handle target, unsigned int argb);

	/** Ends the target pass. Later batches go back to the window, resumed with LOAD. */
	LIME_VULKAN_API int lime_vulkan_batch_end_target (void);

	/** Stats of the last frame: drawCalls / quads / vertices, one key=value per line. */
	LIME_VULKAN_API int lime_vulkan_batch_stats (char* out, int capacity);

	/** Status text: attached / device / swapchain / frame count / last error. */
	LIME_VULKAN_API int lime_vulkan_status (char* out, int capacity);

	/** Self test: renders N frames on a hidden window (demo and CI only, never the game window). */
	LIME_VULKAN_API int lime_vulkan_self_test (int frames, int width, int height, char* out, int capacity);

	/**
	 * Fallback notice: writes one line to the terminal or log and, by default,
	 * shows a modal dialog. Call it only when the user explicitly asked for
	 * Vulkan, so an automatic fallback never interrupts the player.
	 */
	LIME_VULKAN_API void lime_vulkan_notify_fallback (const char* reason);
	LIME_VULKAN_API void lime_vulkan_set_dialog_enabled (int enabled);

	// ---------------------------------------------------------------------
	// Test patterns
	//
	// Used to prove that a frame really comes from Vulkan. Once the batch API took
	// over, these are diagnostics only and never affect the game image.
	// ---------------------------------------------------------------------

	typedef enum lime_vulkan_pattern {

		LIME_VULKAN_PATTERN_TRIANGLE = 0, /**< spinning triangle */
		LIME_VULKAN_PATTERN_GRADIENT = 1  /**< full screen gradient, easiest to confirm by eye */

	} lime_vulkan_pattern;

	LIME_VULKAN_API void lime_vulkan_set_pattern (int pattern);
	LIME_VULKAN_API int lime_vulkan_get_pattern (void);

	// ---------------------------------------------------------------------
	// Window discovery
	//
	// Plain Win32, never touches Vulkan, so the attach tool and the engine share
	// it. Nothing found returns 0 / NULL: no abort, no dialog.
	// ---------------------------------------------------------------------

	/** Finds a pid by process name (with extension, case insensitive); 0 when not found. */
	LIME_VULKAN_API unsigned int lime_vulkan_process_id (const char* exeName);

	/** The largest visible top level window of that process; NULL when not found. */
	LIME_VULKAN_API void* lime_vulkan_process_window (unsigned int pid);

	/** Client area size (1 on success, 0 when the handle is invalid). */
	LIME_VULKAN_API int lime_vulkan_window_client_size (void* nativeWindow, int* width, int* height);

	/** Window title; returns the number of bytes written. */
	LIME_VULKAN_API int lime_vulkan_window_title (void* nativeWindow, char* out, int capacity);

	/**
	 * Freezes or resumes every thread of the target process.
	 *
	 * Only for the forensics case where GL and Vulkan fight over one window:
	 * freeze the other render loop first, then let our frames stay on screen.
	 * Returns the number of threads touched. Normal takeover does not need it,
	 * the two renderers are mutually exclusive there.
	 */
	LIME_VULKAN_API int lime_vulkan_process_suspend (unsigned int pid);
	LIME_VULKAN_API int lime_vulkan_process_resume (unsigned int pid);

	// ---------------------------------------------------------------------
	// Shader programs
	//
	// The input is GLSL ES 1.00 as OpenFL and flixel write it; the output is a
	// Vulkan pipeline. Translation and compilation happen at runtime, and the
	// SPIR-V result can be cached on disk.
	// ---------------------------------------------------------------------

	typedef unsigned long long lime_vulkan_program;

	enum lime_vulkan_program_flag {

		LIME_VULKAN_PROGRAM_DEFAULT = 0,

		/** Disables the OpenFL / flixel compatibility layer: attribute, varying and
		 *  uniform are translated verbatim, vertex attributes take locations in
		 *  declaration order, and the caller keeps them in sync with the layout. */
		LIME_VULKAN_PROGRAM_PLAIN = 1 << 0,

		/** The fragment output is not premultiplied. With the compatibility layer
		 *  premultiplied is the default (that is what flixel writes); a hand written
		 *  shader without a colour transform needs this flag. */
		LIME_VULKAN_PROGRAM_STRAIGHT_ALPHA = 1 << 1

	};

	typedef struct lime_vulkan_program_desc {

		const char* vertexSource;	/**< GLSL ES 1.00 vertex shader */
		const char* fragmentSource; /**< GLSL ES 1.00 fragment shader */
		unsigned int flags;		 /**< combination of lime_vulkan_program_flag */
		const char* cacheDirectory; /**< SPIR-V cache directory, NULL disables caching */

	} lime_vulkan_program_desc;

	/** Reflected uniform types, numbered like the GLSL types. */
	enum lime_vulkan_uniform_type {

		LIME_VULKAN_UNIFORM_FLOAT = 1,
		LIME_VULKAN_UNIFORM_INT = 2,
		LIME_VULKAN_UNIFORM_UINT = 3,
		LIME_VULKAN_UNIFORM_BOOL = 4,
		LIME_VULKAN_UNIFORM_VEC2 = 5,
		LIME_VULKAN_UNIFORM_VEC3 = 6,
		LIME_VULKAN_UNIFORM_VEC4 = 7,
		LIME_VULKAN_UNIFORM_IVEC2 = 8,
		LIME_VULKAN_UNIFORM_IVEC3 = 9,
		LIME_VULKAN_UNIFORM_IVEC4 = 10,
		LIME_VULKAN_UNIFORM_UVEC2 = 11,
		LIME_VULKAN_UNIFORM_UVEC3 = 12,
		LIME_VULKAN_UNIFORM_UVEC4 = 13,
		LIME_VULKAN_UNIFORM_MAT2 = 17,
		LIME_VULKAN_UNIFORM_MAT3 = 18,
		LIME_VULKAN_UNIFORM_MAT4 = 19

	};

	typedef struct lime_vulkan_uniform_info {

		const char* name;
		int type;            /**< lime_vulkan_uniform_type */
		unsigned int offset; /**< byte offset inside the uniform block */
		unsigned int size;	/**< bytes used (the whole array for an array) */
		int count;		   /**< array length, 0 when not an array */

	} lime_vulkan_uniform_info;

	/** Returns 0 on failure, reason in lime_vulkan_status; the log holds the translation and compile output. */
	LIME_VULKAN_API lime_vulkan_program lime_vulkan_program_create (const lime_vulkan_program_desc* desc);
	LIME_VULKAN_API void lime_vulkan_program_destroy (lime_vulkan_program program);
	LIME_VULKAN_API int lime_vulkan_program_valid (lime_vulkan_program program);

	/** Human readable log of translation, compilation and reflection. Valid until the program is destroyed. */
	LIME_VULKAN_API const char* lime_vulkan_program_log (lime_vulkan_program program);

	LIME_VULKAN_API int lime_vulkan_program_uniform_count (lime_vulkan_program program);

	/** Describes uniform index. name is an internal string, valid until the program is destroyed. */
	LIME_VULKAN_API int lime_vulkan_program_uniform_info (lime_vulkan_program program, int index, lime_vulkan_uniform_info* info);

	/** Writes a uniform by name. Returns 0 when the name is unknown or the type differs, which is not an error: optional uniforms are common. */
	LIME_VULKAN_API int lime_vulkan_program_set_float (lime_vulkan_program program, const char* name, float value);
	LIME_VULKAN_API int lime_vulkan_program_set_vec2 (lime_vulkan_program program, const char* name, const float* value);
	LIME_VULKAN_API int lime_vulkan_program_set_vec3 (lime_vulkan_program program, const char* name, const float* value);
	LIME_VULKAN_API int lime_vulkan_program_set_vec4 (lime_vulkan_program program, const char* name, const float* value);
	LIME_VULKAN_API int lime_vulkan_program_set_int (lime_vulkan_program program, const char* name, int value);
	LIME_VULKAN_API int lime_vulkan_program_set_mat4 (lime_vulkan_program program, const char* name, const float* value);

	/** Array uniform: writes count elements with the std140 stride. */
	LIME_VULKAN_API int lime_vulkan_program_set_float_array (lime_vulkan_program program, const char* name, const float* values, int count);

	/** Binds a texture to one sampler. Unbound samplers use the current batch texture. */
	LIME_VULKAN_API int lime_vulkan_program_set_texture (lime_vulkan_program program, const char* name, lime_vulkan_texture_handle texture);

	/** The same binding addressed by sampler index, for callers that already hold it. */
	LIME_VULKAN_API int lime_vulkan_program_set_sampler_texture (lime_vulkan_program program, int index, lime_vulkan_texture_handle texture);

	/**
	 * lime_vulkan_program_desc as plain arguments: bindings that cannot pass a struct
	 * (Haxe, C#) use this. Same semantics as lime_vulkan_program_create.
	 */
	LIME_VULKAN_API lime_vulkan_program lime_vulkan_program_create_flat (const char* vertexSource, const char* fragmentSource,
		unsigned int flags, const char* cacheDirectory);

	/**
	 * Reflection lookups by index instead of lime_vulkan_uniform_info: returns 0 / NULL when
	 * the index is out of range. Names stay valid until the program is destroyed.
	 */
	LIME_VULKAN_API const char* lime_vulkan_program_uniform_name (lime_vulkan_program program, int index);
	LIME_VULKAN_API int lime_vulkan_program_uniform_type (lime_vulkan_program program, int index);
	LIME_VULKAN_API unsigned int lime_vulkan_program_uniform_offset (lime_vulkan_program program, int index);
	LIME_VULKAN_API unsigned int lime_vulkan_program_uniform_size (lime_vulkan_program program, int index);
	LIME_VULKAN_API int lime_vulkan_program_uniform_length (lime_vulkan_program program, int index);

	LIME_VULKAN_API int lime_vulkan_program_sampler_count (lime_vulkan_program program);
	LIME_VULKAN_API const char* lime_vulkan_program_sampler_name (lime_vulkan_program program, int index);

	/**
	 * Writes uniform data straight into the program's shadow block at a reflected
	 * offset. The float variant converts from double, so a Haxe Array<Float> can be
	 * handed over without a copy. Returns 0 when the range falls outside the block.
	 */
	LIME_VULKAN_API int lime_vulkan_program_write_floats (lime_vulkan_program program, unsigned int offset, const double* values, int count);
	LIME_VULKAN_API int lime_vulkan_program_write_ints (lime_vulkan_program program, unsigned int offset, const int* values, int count);

	/** Later batches draw with this program; 0 restores the built-in quad pipeline. */
	LIME_VULKAN_API void lime_vulkan_batch_set_program (lime_vulkan_program program);
	LIME_VULKAN_API lime_vulkan_program lime_vulkan_batch_get_program (void);

	// ---------------------------------------------------------------------
	// Engine integration
	//
	// For callers that cannot pass structs or open buffers: Haxe, C# and other
	// FFI bindings have no struct arguments and should not allocate a char[] per
	// call. Purely additive, no existing entry point changed.
	// ---------------------------------------------------------------------

	/** The main window of this process (largest visible top level window); NULL when not found. */
	LIME_VULKAN_API void* lime_vulkan_self_window (void);

	/**
	 * Flat argument versions of lime_vulkan_context_create / lime_vulkan_probe_window (options are
	 * not passed in a struct). Same semantics as the originals, including
	 * 'a probe never creates a swapchain'.
	 */
	LIME_VULKAN_API int lime_vulkan_context_create_flat (void* nativeWindow, int width, int height,
		int deviceIndex, int presentMode, int enableValidation, int verbose, int fallbackDialog);
	LIME_VULKAN_API int lime_vulkan_probe_window_flat (void* nativeWindow, int width, int height,
		int deviceIndex, int presentMode, int enableValidation, int verbose, int fallbackDialog);

	/**
	 * Buffer free versions of lime_vulkan_probe / lime_vulkan_status / lime_vulkan_batch_stats: they return
	 * an internal static string, valid until the next call with the same name.
	 * Single threaded use only, do not hold the pointer across threads.
	 */
	LIME_VULKAN_API const char* lime_vulkan_probe_text (void);
	LIME_VULKAN_API const char* lime_vulkan_status_text (void);
	LIME_VULKAN_API const char* lime_vulkan_batch_stats_text (void);

	/** Buffer free lime_vulkan_batch_begin: clearColor is 0xAARRGGBB. */
	LIME_VULKAN_API int lime_vulkan_batch_begin_rgba (int screenWidth, int screenHeight, unsigned int argb);

	/**
	 * 2D transform of the camera canvas (OpenFL Matrix order: a,b,c,d,tx,ty).
	 *
	 * flixel hands out quad coordinates in camera canvas space, and the canvas is
	 * transformed by flashSprite's position, scale and rotation before it reaches
	 * the screen. NULL or the identity matrix resets it.
	 *
	 * Why double: flixel's Array<Float> is a double[] under hxcpp, so the caller
	 * passes the array pointer straight through instead of copying it per frame.
	 */
	LIME_VULKAN_API void lime_vulkan_batch_set_view (const double* matrix2x3);
	LIME_VULKAN_API void lime_vulkan_batch_get_view (double* matrix2x3);

	/**
	 * Uploads a whole batch of quads at once, which is the shape of flixel's
	 * FlxDrawQuadsItem.
	 *
	 * @param rects       4 per quad: source rectangle x,y,w,h in pixels
	 * @param transforms  6 per quad: a,b,c,d,tx,ty (2D matrix in camera canvas space)
	 * @param quadCount   number of quads
	 * @param alphas      NULL shares uniformAlpha for the whole batch, otherwise
	 *                    one value per vertex (4 per quad)
	 * @param uniformAlpha alpha used when alphas is NULL
	 * @param colorMultipliers NULL means all ones, otherwise RGBA per vertex
	 * @param colorOffsets     NULL means all zeros, otherwise RGBA per vertex in
	 *                    0..255 units
	 *
	 * colorOffsets is the additive half of colorTransform (redOffset and friends)
	 * and is written per vertex into lime_vulkan_vertex::colorOffset, four signed bytes.
	 * It stays separate from colorMultipliers on purpose: multiply and offset
	 * cannot share a channel, or every 'brighten or darken the layer' effect
	 * silently stops working.
	 *
	 * @return 1 on success; 0 when the frame buffer is full (the next frame grows it)
	 */
	LIME_VULKAN_API int lime_vulkan_batch_quads_d (const double* rects, const double* transforms, int quadCount,
		const double* alphas, float uniformAlpha,
		const double* colorMultipliers, const double* colorOffsets);

	/** Version string, for example '0.4.0'. See the cacheDirectory note in lime_vulkan_program_desc. */
	LIME_VULKAN_API const char* lime_vulkan_version (void);

#ifdef __cplusplus
}
#endif

#endif
