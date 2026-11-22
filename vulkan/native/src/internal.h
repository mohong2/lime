#ifndef LIME_VULKAN_INTERNAL_H
#define LIME_VULKAN_INTERNAL_H

// No link against libvulkan: every entry point is resolved at runtime, so the header keeps types only.
#define VK_NO_PROTOTYPES

#if defined(_WIN32)
// vulkan.h includes <windows.h> under VK_USE_PLATFORM_WIN32_KHR, and the min/max
// macros from it break std::min / std::max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define VK_USE_PLATFORM_WIN32_KHR 1
#define LIME_VULKAN_WINDOWS 1
#elif defined(__ANDROID__)
#define VK_USE_PLATFORM_ANDROID_KHR 1
#define LIME_VULKAN_ANDROID 1
#else
#define VK_USE_PLATFORM_XLIB_KHR 1
#define LIME_VULKAN_LINUX 1
#endif

#include <vulkan/vulkan.h>

#include <lime_vulkan.h>

#include "glsl.h"

#if defined(LIME_VULKAN_ANDROID)
#include <android/log.h>
// On the phone there is no console: logcat is the only place a diagnostic can land,
// and it survives a fatal signal (unlike a crash report written after the fact).
// Silent unless the build asks for it (-DSEIUN_VULKAN_TRACE): a driver that crashes
// inside itself needs a trace, but a shipped game does not want one every frame.
#if defined(SEIUN_VULKAN_TRACE)
#define LIME_VK_LOGCAT(...) __android_log_print (ANDROID_LOG_INFO, "lime-vk", __VA_ARGS__)
#else
#define LIME_VK_LOGCAT(...) ((void)0)
#endif
#else
#define LIME_VK_LOGCAT(...) ((void)0)
#endif

#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace vulkan {

	/** How many frames of draw fences may be in flight. */
	const int MAX_FRAMES_IN_FLIGHT = 2;

	struct Api {

		PFN_vkCreateInstance CreateInstance;
		PFN_vkDestroyInstance DestroyInstance;
		PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties;
		PFN_vkEnumerateInstanceLayerProperties EnumerateInstanceLayerProperties;
		PFN_vkEnumerateInstanceVersion EnumerateInstanceVersion;

		PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices;
		PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties;
		PFN_vkGetPhysicalDeviceFeatures GetPhysicalDeviceFeatures;
		PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
		PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
		PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR;
		PFN_vkGetPhysicalDeviceSurfaceFormatsKHR GetPhysicalDeviceSurfaceFormatsKHR;
		PFN_vkGetPhysicalDeviceSurfacePresentModesKHR GetPhysicalDeviceSurfacePresentModesKHR;
		PFN_vkGetPhysicalDeviceSurfaceSupportKHR GetPhysicalDeviceSurfaceSupportKHR;
		PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties;
		PFN_vkDestroySurfaceKHR DestroySurfaceKHR;

		PFN_vkCreateDevice CreateDevice;
		PFN_vkGetDeviceProcAddr GetDeviceProcAddr;

		#if defined(LIME_VULKAN_WINDOWS)
		PFN_vkCreateWin32SurfaceKHR CreateWin32SurfaceKHR;
		#elif defined(LIME_VULKAN_ANDROID)
		PFN_vkCreateAndroidSurfaceKHR CreateAndroidSurfaceKHR;
		#else
		PFN_vkCreateXlibSurfaceKHR CreateXlibSurfaceKHR;
		#endif

		void Clear () { memset (this, 0, sizeof (*this)); }

	};

	struct DeviceApi {

		PFN_vkDestroyDevice DestroyDevice;
		PFN_vkDeviceWaitIdle DeviceWaitIdle;
		PFN_vkGetDeviceQueue GetDeviceQueue;
		PFN_vkQueueSubmit QueueSubmit;
		PFN_vkQueueWaitIdle QueueWaitIdle;
		PFN_vkQueuePresentKHR QueuePresentKHR;

		PFN_vkCreateSwapchainKHR CreateSwapchainKHR;
		PFN_vkDestroySwapchainKHR DestroySwapchainKHR;
		PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR;
		PFN_vkAcquireNextImageKHR AcquireNextImageKHR;

		PFN_vkCreateImageView CreateImageView;
		PFN_vkDestroyImageView DestroyImageView;
		PFN_vkCreateRenderPass CreateRenderPass;
		PFN_vkDestroyRenderPass DestroyRenderPass;
		PFN_vkCreateFramebuffer CreateFramebuffer;
		PFN_vkDestroyFramebuffer DestroyFramebuffer;

		PFN_vkCreateShaderModule CreateShaderModule;
		PFN_vkDestroyShaderModule DestroyShaderModule;
		PFN_vkCreatePipelineLayout CreatePipelineLayout;
		PFN_vkDestroyPipelineLayout DestroyPipelineLayout;
		PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines;
		PFN_vkDestroyPipeline DestroyPipeline;

		PFN_vkCreateBuffer CreateBuffer;
		PFN_vkDestroyBuffer DestroyBuffer;
		PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
		PFN_vkAllocateMemory AllocateMemory;
		PFN_vkFreeMemory FreeMemory;
		PFN_vkBindBufferMemory BindBufferMemory;
		PFN_vkMapMemory MapMemory;
		PFN_vkUnmapMemory UnmapMemory;

		PFN_vkCreateCommandPool CreateCommandPool;
		PFN_vkDestroyCommandPool DestroyCommandPool;
		PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
		PFN_vkBeginCommandBuffer BeginCommandBuffer;
		PFN_vkEndCommandBuffer EndCommandBuffer;
		PFN_vkResetCommandBuffer ResetCommandBuffer;
		PFN_vkCmdBeginRenderPass CmdBeginRenderPass;
		PFN_vkCmdEndRenderPass CmdEndRenderPass;
		PFN_vkCmdBindPipeline CmdBindPipeline;
		PFN_vkCmdBindVertexBuffers CmdBindVertexBuffers;
		PFN_vkCmdPushConstants CmdPushConstants;
		PFN_vkCmdDraw CmdDraw;
		PFN_vkCmdSetViewport CmdSetViewport;
		PFN_vkCmdSetScissor CmdSetScissor;

		PFN_vkCmdBindIndexBuffer CmdBindIndexBuffer;
		PFN_vkCmdDrawIndexed CmdDrawIndexed;
		PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
		PFN_vkCmdCopyBuffer CmdCopyBuffer;
		PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage;
		PFN_vkCmdBlitImage CmdBlitImage;

		PFN_vkCreateImage CreateImage;
		PFN_vkDestroyImage DestroyImage;
		PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
		PFN_vkBindImageMemory BindImageMemory;

		PFN_vkCreateSampler CreateSampler;
		PFN_vkDestroySampler DestroySampler;

		PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout;
		PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout;
		PFN_vkCreateDescriptorPool CreateDescriptorPool;
		PFN_vkDestroyDescriptorPool DestroyDescriptorPool;
		PFN_vkAllocateDescriptorSets AllocateDescriptorSets;
		PFN_vkUpdateDescriptorSets UpdateDescriptorSets;
		PFN_vkFreeDescriptorSets FreeDescriptorSets;
		PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets;
		PFN_vkResetDescriptorPool ResetDescriptorPool;

		PFN_vkCreateSemaphore CreateSemaphore;
		PFN_vkDestroySemaphore DestroySemaphore;
		PFN_vkCreateFence CreateFence;
		PFN_vkDestroyFence DestroyFence;
		PFN_vkWaitForFences WaitForFences;
		PFN_vkResetFences ResetFences;
		PFN_vkFreeCommandBuffers FreeCommandBuffers;

		PFN_vkCreatePipelineCache CreatePipelineCache;
		PFN_vkDestroyPipelineCache DestroyPipelineCache;
		PFN_vkGetPipelineCacheData GetPipelineCacheData;

		PFN_vkCreateQueryPool CreateQueryPool;
		PFN_vkDestroyQueryPool DestroyQueryPool;
		PFN_vkCmdResetQueryPool CmdResetQueryPool;
		PFN_vkCmdWriteTimestamp CmdWriteTimestamp;
		PFN_vkGetQueryPoolResults GetQueryPoolResults;

		void Clear () { memset (this, 0, sizeof (*this)); }

	};

	/** Quads per flush. Past this the batch flushes again, at the cost of one more draw call. */
	const uint32_t MAX_QUADS_PER_FLUSH = 16384;

	/** Filter index stored on the texture and on the batch. */
	const int FILTER_NEAREST = 0;
	const int FILTER_LINEAR = 1;

	/** One pipeline per blend mode: see lime_vulkan_blend in the public header. */
	const int BLEND_COUNT = 9;

	/** A texture that lives in device memory. The handle is the public 64 bit id; pointers stay out of the ABI. */
	/** A device memory block that several images share; defined in image.cpp. */
	struct MemoryBlock;

	struct Texture {

		VkImage image;
		/** The block the image lives in, NULL when the image owns its memory. */
		MemoryBlock* block;
		/** Offset inside that block. */
		VkDeviceSize offset;
		VkDeviceMemory memory;
		VkImageView view;
		/** Sampler per filter, indexed by FILTER_NEAREST / FILTER_LINEAR. */
		VkSampler samplers[2];
		/** Descriptor set per filter, bound by the draw flush. */
		VkDescriptorSet descriptors[2];
		int width;
		int height;
		/** The format the image and its view were created with. B8G8R8A8 exists so a
		 *  BGRA source can be copied straight in instead of being swizzled on the CPU. */
		VkFormat format;
		/** Bytes per texel of the uploaded image: 4 for RGBA, 1 for palette indices. */
		int bytesPerPixel;
		/** Device memory this texture holds, so the totals can drop it on release. */
		VkDeviceSize allocation;
		/** Bytes taken out of the block, which can exceed 'allocation': a remainder
		 *  smaller than the alignment is handed out with the image instead of being
		 *  left as a fragment nothing can ever use. */
		VkDeviceSize reserved;

		/** Non-null only for render targets: the framebuffer the pass draws into. */
		VkFramebuffer framebuffer;
		/** The layout the image is in right now. Plain textures never leave
		 *  SHADER_READ_ONLY_OPTIMAL; only render targets flip back and forth. */
		VkImageLayout layout;

		Texture ();

	};

	/** 2D batch state: a ring vertex buffer plus one pipeline per blend mode. */
	struct Batch {

		VkBuffer vertexBuffer[MAX_FRAMES_IN_FLIGHT];
		VkDeviceMemory vertexMemory[MAX_FRAMES_IN_FLIGHT];
		void* vertexMapped[MAX_FRAMES_IN_FLIGHT];
		uint32_t vertexCapacity; /**< vertices per slot */

		VkBuffer indexBuffer;
		VkDeviceMemory indexMemory;
		uint32_t indexCapacity; /**< quads the index buffer holds */

		VkDescriptorSetLayout descriptorLayout;
		VkDescriptorPool descriptorPool;
		VkPipelineLayout layout;
		VkPipeline pipelines[BLEND_COUNT];

		uint32_t verticesWritten; /**< vertices written this frame (decides the write offset) */
		uint32_t batchFirstVertex;/**< first vertex of the segment that is not submitted yet */
		uint32_t segmentQuads;	  /**< quads in that unfinished segment */
		bool segmentIndexed;	  /**< unfinished segment is quad indices or raw triangles */
		bool overflowed;		  /**< the last frame did not fit, grow before the next one */
		uint32_t droppedVertices; /**< vertices dropped last frame, which sizes the growth */
		uint32_t drawCalls;
		uint32_t frameVertices;
		/** Binding changes this frame: what a bindless table or a push constant rewrite would remove. */
		uint32_t bindTextures;
		uint32_t bindPipelines;
		uint32_t bindScissors;

		uint64_t currentTexture;
		int currentFilter; /**< FILTER_NEAREST / FILTER_LINEAR, see also currentTexture */
		int currentBlend;
		int scissorX, scissorY, scissorWidth, scissorHeight;

		float projection[4]; /**< (2/w, -2/h, -1, 1): pixel space to NDC, y down */

		/** 2D transform of the camera canvas (OpenFL Matrix order a,b,c,d,tx,ty).
		 *  flixel gives quads in canvas space and the canvas is transformed by
		 *  flashSprite before it reaches the screen; with hasView false the
		 *  vertices pass through, which the demo and the tests use. */
		float view[6];
		bool hasView;

		/** Colour offsets dropped because the vertex format has no additive part. A statistic, never silent. */
		uint32_t droppedColorOffsets;
		uint32_t droppedSegments;   /**< segments skipped because a texture or pipeline was missing: a silent hole has to be visible */
		uint32_t fallbackTextures;  /**< segments drawn with the white pixel because their texture handle was dead */
		uint32_t drawnVertices;     /**< vertices that reached vkCmdDraw*, a mismatch with verticesWritten means a segment was swallowed */

		/** Ring of recent draws for diagnostics: vertex start, quad count, scissor, texture. */
		struct DrawRecord {
			uint32_t firstVertex;
			uint32_t quads;
			int scissorX, scissorY, scissorW, scissorH;
			unsigned long long texture;
			float vx[4], vy[4], vu[4], vv[4];
			unsigned int vc[4];
		};
		DrawRecord draws[12];
		uint32_t drawRecordCount;

		/** Every quad written into the vertex buffer this frame, for diagnostics. */
		struct QuadNote { float x0, y0, x1, y1; unsigned int color; } notes[16];
		uint32_t noteCount;

		bool recording;

		uint64_t program; /**< program in use, 0 means the built-in quad pipeline */

		Batch ();

	};

	/** Ring of uniform buffers: every draw of a frame needs its own copy.
	 *  One slot per frame; the offset resets when a frame starts and each flush
	 *  takes the next chunk. */
	struct UniformRing {

		VkBuffer buffer[MAX_FRAMES_IN_FLIGHT];
		VkDeviceMemory memory[MAX_FRAMES_IN_FLIGHT];
		void* mapped[MAX_FRAMES_IN_FLIGHT];
		uint32_t capacity; /**< bytes per slot */
		uint32_t offset;   /**< bytes used this frame */

		UniformRing ();

	};

	/** A compiled shader program: six pipelines (one per blend mode) plus the reflection. */
	struct ShaderProgram {

		uint64_t handle;

		VkPipeline pipelines[BLEND_COUNT];
		VkPipelineLayout layout;
		VkDescriptorSetLayout descriptorLayout;

		bool openfl;
		bool premultiplied;
		uint32_t uniformSize;

		std::vector<unsigned char> shadow;              /**< CPU copy of the uniform data */
		std::vector<glsl::Variable> uniforms;
		std::vector<glsl::Variable> samplers;
		std::map<std::string, int> uniformIndex;        /**< name -> uniforms index */
		std::map<std::string, int> samplerIndex;        /**< name -> samplers index */
		uint64_t samplerTextures[16];                   /**< explicit textures, 0 uses the batch texture */

		std::string log;

		ShaderProgram ();

	};

	/** Render target: a native window plus its current size. */
	struct Target {

		void* window;
		int width;
		int height;

	};

	/**
	 * Frame timing history, one sample per submitted frame.
	 *
	 * The GPU number is read two frames late: the timestamps of a slot only become
	 * available once that slot's fence signals, which BeginFrame does anyway.
	 * Nothing waits for a number the current frame does not need.
	 */
	struct FramePerf {

		static const int HISTORY = 240;

		float cpu[HISTORY]; /**< milliseconds from the start of BeginFrame to the end of EndFrame */
		float gpu[HISTORY]; /**< GPU milliseconds for the same frame, negative when unknown */
		uint32_t next;
		uint32_t count;
		uint32_t frames;    /**< frames measured since the device came up */

		double frameStart;  /**< QPC seconds when the current frame opened */

		VkQueryPool queries;
		bool timestamps;    /**< the queue family can write timestamps at all */
		float period;       /**< nanoseconds per timestamp tick */

		/** CPU cost of the frame that last used a slot, waiting for its GPU half. */
		float pendingCpu[MAX_FRAMES_IN_FLIGHT];
		bool pendingCpuValid[MAX_FRAMES_IN_FLIGHT];

		FramePerf ();

	};

	struct State {

		Api api;
		DeviceApi dev;
		lime_vulkan_options options;

		VkInstance instance;
		VkPhysicalDevice physicalDevice;
		VkDevice device;
		VkQueue graphicsQueue;
		VkQueue presentQueue;
		uint32_t graphicsFamily;
		uint32_t presentFamily;

		VkSurfaceKHR surface;
		void* window;

		VkSwapchainKHR swapchain;
		VkFormat swapchainFormat;
		VkColorSpaceKHR swapchainColorSpace;
		VkExtent2D extent;
		std::vector<VkImage> images;
		std::vector<VkImageView> views;
		std::vector<VkFramebuffer> framebuffers;

		/** Shared VkPipelineCache, persisted in cache/ so a start does not recompile. */
		VkPipelineCache pipelineCache;

		VkRenderPass renderPass;
		/** Same attachment, but LOAD instead of CLEAR: reopens the window pass after
		 *  the batch went offscreen for a frame pass. */
		VkRenderPass surfacePassLoad;
		/** Offscreen pass: colour attachment in, SHADER_READ_ONLY image out. */
		VkRenderPass targetPass;
		VkPipelineLayout pipelineLayout;
		VkPipeline pipeline;
		VkBuffer vertexBuffer;
		VkDeviceMemory vertexMemory;

		VkCommandPool commandPool;
		std::vector<VkCommandBuffer> commandBuffers;
		VkCommandBuffer frameCommandBuffer; /**< command buffer being recorded (one per imageIndex) */
		uint32_t currentImageIndex;
		bool inFrame;
		std::vector<VkSemaphore> imageAvailable;
		std::vector<VkSemaphore> renderFinished;
		std::vector<VkFence> inFlight;
		uint32_t frameIndex;
		uint32_t imageCount;

		// Surface capabilities at the last swapchain build. Android reports the
		// swapchain in the panel's native orientation and asks the presentation
		// engine to rotate it (currentTransform), which is not the orientation the
		// engine renders in; these are kept so the status file can show both sides.
		VkSurfaceTransformFlagBitsKHR surfaceTransform;
		VkSurfaceTransformFlagBitsKHR surfacePreTransform;
		VkSurfaceTransformFlagsKHR surfaceTransforms;
		VkExtent2D surfaceExtent;
		VkExtent2D surfaceMinExtent;
		VkExtent2D surfaceMaxExtent;

		bool attached;
		bool resized;
		bool dialogEnabled;

		// Where the batch draws right now. Exactly one pass is open while a draw is
		// recorded, and it is ended before another one starts: the swapchain pass
		// cannot stay open while an offscreen pass runs.
		bool surfacePassOpen;
		bool surfacePassCleared;
		bool targetPassOpen;
		Texture* activeTarget;
		float clearColor[4];
		int frameWidth;
		int frameHeight;
		uint64_t framesPresented;
		int pattern;

		Batch batch;
		std::map<uint64_t, Texture*> textures;

		FramePerf perf;

		/** Upload channel totals: what the staging ring pushed, and how the waits went. */
		struct UploadTotals {
			uint64_t bytes;    /**< bytes copied into images */
			uint64_t copies;   /**< vkCmdCopyBufferToImage calls */
			uint64_t flushes;  /**< upload submits */
			uint64_t waits;    /**< host waits on the upload fence */
			uint64_t failures; /**< fence waits that did not succeed */

			UploadTotals () : bytes (0), copies (0), flushes (0), waits (0), failures (0) {}
		};

		UploadTotals upload;

		/** Device memory currently allocated, split by what it is for. */
		struct MemoryTotals {
			uint64_t textures;     /**< bytes actually handed to images */
			uint64_t textureCount;
			uint64_t staging;
			uint64_t other;
			/** Bytes reserved by the block allocator: what the process really holds. */
			uint64_t reserved;
			/** vkAllocateMemory calls made for image memory, the number the driver caps. */
			uint64_t allocations;

			MemoryTotals () : textures (0), textureCount (0), staging (0), other (0), reserved (0), allocations (0) {}
		};

		MemoryTotals memory;

		/** Retired textures waiting for release: {handle, submitted frame count at retire}.
		 *  Releasing right away reclaims a descriptor set that a submitted command
		 *  still references (validation vkFreeDescriptorSets-00309, device lost on
		 *  NVIDIA), so the real delete waits MAX_FRAMES_IN_FLIGHT+1 frames. */
		struct RetiredTexture {
			uint64_t handle;
			uint64_t frame;
		};

		std::vector<RetiredTexture> retiredTextures;

		uint64_t nextTextureHandle;
		uint64_t whiteTexture;

		std::map<uint64_t, ShaderProgram*> programs;
		uint64_t nextProgramHandle;
		UniformRing uniformRing;

		// One descriptor pool per frame slot: every draw takes a fresh set from it
		// and the whole pool resets when the frame starts. A shared set would let
		// the next draw overwrite the previous texture binding.
		VkDescriptorPool frameDescriptorPool[MAX_FRAMES_IN_FLIGHT];

		std::string deviceName;
		std::string lastError;
		std::string fallbackReason;

		/** Swapchain maintenance counters: how often the driver asked for a rebuild
		 *  and how often one was really done. */
		uint32_t presentSuboptimal;
		uint32_t swapchainRebuilds;

		/** Which wait the frame is actually blocked on, in milliseconds, reset at
		 *  the start of every BeginFrame.
		 *
		 *  Declared as the very last member on purpose: the Android build does not
		 *  track header dependencies, and a field inserted anywhere else moves the
		 *  offset of every member after it. An object nobody recompiled would then
		 *  read a different member - which is exactly how the batch crash of
		 *  November 22 was produced. Growing at the end keeps every existing offset. */
		struct FrameWaits {
			float fence;       /**< waiting for this slot's frame fence */
			float acquire;     /**< vkAcquireNextImageKHR */
			float submit;      /**< vkQueueSubmit of the frame */
			float present;     /**< vkQueuePresentKHR */
			float staging;     /**< waiting for the upload fence before writing staging */
			float uploadDrain; /**< waiting for the previous frame before an update copy */
			float uploadFlush; /**< waiting for the upload channel before recording */
			float uploadQueue; /**< recording and submitting the upload batch */
			float beginTotal;  /**< whole BeginFrame */
			float endTotal;    /**< whole EndFrame */
			float batchSubmit; /**< FlushBatch: bind state and record the draw */
			float beginPass;   /**< vkCmdBeginRenderPass */
			float endPass;     /**< vkCmdEndRenderPass */
			float endBuffer;   /**< vkEndCommandBuffer */
			float presentBarrier; /**< the layout transition to PRESENT_SRC */
			float timestamp;   /**< vkCmdWriteTimestamp of the frame end */

			FrameWaits () : fence (0.0f), acquire (0.0f), submit (0.0f), present (0.0f),
				staging (0.0f), uploadDrain (0.0f), uploadFlush (0.0f), uploadQueue (0.0f),
				beginTotal (0.0f), endTotal (0.0f), batchSubmit (0.0f), beginPass (0.0f),
				endPass (0.0f), endBuffer (0.0f), presentBarrier (0.0f), timestamp (0.0f) {}
		};

		FrameWaits waits;

		State ();

	};

	State& S ();

	void Log (const char* format, ...);
	void SetError (const char* message);

	bool LoadLoader (bool verbose);
	uint32_t LoaderVersion ();
	bool LoadInstanceApi (VkInstance instance);
	bool LoadDeviceApi (VkDevice device);

	std::string VersionText (uint32_t version);
	const char* DeviceTypeText (VkPhysicalDeviceType type);
	const char* PresentModeText (VkPresentModeKHR mode);
	const char* ResultText (VkResult result);

	bool CreateInstance (bool validation, bool verbose, VkInstance* instance);
	int DeviceScore (const VkPhysicalDeviceProperties& properties);

	// Platform layer: lime_vulkan_surface_win32.cpp / lime_vulkan_surface_x11.cpp / ...
	bool CreatePlatformSurface (VkInstance instance, void* window, VkSurfaceKHR* surface);
	bool CreateSurfaceExtensions (std::vector<const char*>& extensions);
	const char* PlatformName ();

	/** Hidden window used by the self test; returns the native handle, 0 on failure. */
	void* CreateHiddenWindow (int width, int height);
	void DestroyHiddenWindow (void* window);

	// Window discovery (lime_vulkan_window_win32.cpp); returns 0 / NULL on failure.
	unsigned int FindProcessIdByName (const char* exeName);
	void* FindProcessWindow (unsigned int pid);
	/** The main window of this process. */
	void* SelfWindow ();
	/** Android: the ANativeWindow the host handed over; a no-op elsewhere. */
	void SetAndroidWindow (void* window);

	#if defined(LIME_VULKAN_ANDROID)

	/**
	 * Android surface lifecycle.
	 *
	 * The activity's ANativeWindow is destroyed and recreated on every resume, rotation
	 * and split-screen change, and the call arrives on the Java/SDL thread. Swapping the
	 * swapchain there would tear down a frame the render thread is still submitting, so
	 * SetAndroidWindow() only records the handle: the render thread picks the change up
	 * in BeginFrame() and rebuilds the surface from it (RecreateAndroidSurface).
	 */
	void MarkAndroidWindowApplied (void* window);
	bool TakeChangedAndroidWindow (void** window);
	bool RecreateAndroidSurface (void* window);

	#endif
	bool WindowClientSize (void* window, int* width, int* height);
	int WindowTitle (void* window, char* out, int capacity);
	int SuspendProcess (unsigned int pid);
	int ResumeProcess (unsigned int pid);

	// With withSwapchain=false only the instance, surface and device are created,
	// the swapchain is never touched. That is the safe probe for 'can we switch':
	// CreateSwapchain hands the window to the DXGI compositor and GL cannot draw
	// there afterwards (measured), so the decision has to come before it.
	bool AttachDevice (void* window, int width, int height, bool withSwapchain = true);
	void DetachDevice ();
	bool CreateSwapchain (int width, int height);
	void DestroySwapchain ();
	bool RecreateSwapchain ();
	bool CreateRenderTargets ();
	void DestroyRenderTargets ();
	bool CreateTrianglePipeline ();
	void DestroyTrianglePipeline ();
	bool PresentFrame (const float clearColor[4], float timeSeconds);

	// A frame is three steps: BeginFrame (wait on the fence, acquire, open the
	// render pass), the draws, EndFrame (close, submit, present). PresentFrame and
	// the batch both use these, so there is one submission path, not two.
	bool BeginFrame (const float clearColor[4]);
	bool EndFrame ();

	// Pipeline cache (lime_vulkan_device.cpp): loaded at attach, written back at detach.
	bool CreatePipelineCache ();
	void SavePipelineCache ();
	void DestroyPipelineCache ();

	// Frame timings (lime_vulkan_perf.cpp): CPU frame cost, GPU time, percentiles.
	bool CreatePerfQueries ();
	void DestroyPerfQueries ();
	/** Writes the start timestamp and starts the CPU stopwatch of a frame. */
	void PerfBeginFrame (VkCommandBuffer commandBuffer, uint32_t slot);
	/** Writes the end timestamp, right before the command buffer is closed. */
	void PerfEndFrame (VkCommandBuffer commandBuffer, uint32_t slot);
	/** Stores the CPU cost of the frame that just presented; its GPU half follows later. */
	void PerfCloseFrame (uint32_t slot);
	/** Called once a slot fence signalled: pairs the CPU half with the GPU timestamps. */
	void PerfCollectFrame (uint32_t slot);
	/** Zeroes the per-wait split of the frame that is about to open. */
	void PerfResetFrame ();
	/** Appends the percentile block to the stats text. Returns the number of bytes written. */
	int PerfText (char* out, int capacity);
	double PerfNow ();

	// Render targets (lime_vulkan_image.cpp): a texture that is both a colour attachment and sampleable
	bool CreateRenderTargetPass ();
	void DestroyRenderTargetPass ();
	uint64_t CreateRenderTarget (int width, int height);

	// Render pass switching (lime_vulkan_batch.cpp): moving between the window and an offscreen target
	void ActiveExtent (int* width, int* height);
	bool EnsurePass ();
	bool BeginSurfacePass (bool clear);
	void EndSurfacePass ();
	bool BeginTargetPass (Texture* target, unsigned int argb);
	void EndTargetPass ();
	void EndActivePass ();

	// Batches (lime_vulkan_batch.cpp)
	bool CreateBatchResources ();
	void DestroyBatchResources ();
	bool EnsureBatchCapacity (uint32_t quads);
	void FlushBatch ();
	bool DrawBatchFrame ();

	// Textures (lime_vulkan_image.cpp)
	bool CreateTextureResources ();
	void DestroyTextureResources ();
	Texture* FindTexture (uint64_t handle);
	uint64_t CreateTexture (const void* rgba, int width, int height, int flags);
	int UpdateTexture (uint64_t handle, const void* rgba, int flags);

	/** Records every queued upload into the current frame slot's upload command
	 *  buffer and submits it on the graphics queue, ahead of the frame's own
	 *  commands. Nothing is waited for: the queue order plus the barrier at the end
	 *  of that buffer is what makes the pixels visible to this frame's draws.
	 *  Outside a frame the submit is waited for, so a loader handing textures over
	 *  never returns before the copy landed. */
	bool FlushTextureUploads (bool haveFrame);
	/** Marks the current frame slot's staging ring writable again. Called from
	 *  BeginFrame right after its fence wait, which is what proves the GPU is done
	 *  with the ring. */
	void BeginUploadFrame ();
	void DestroyTexture (uint64_t handle);
	uint64_t WhiteTexture ();

	/** Fills the pipeline state for a blend mode. premultiplied means the fragment output already is premultiplied. */
	/** Retires a texture: marks it for release at the next safe point, see the note in State. */
	void RetireTexture (uint64_t handle);
	/** Releases the retired textures whose frames have passed, called after the fence wait in BeginFrame. */
	void DrainRetiredTextures ();

	void FillBlendState (int blend, bool premultiplied, VkPipelineColorBlendAttachmentState* attachment);

	// Shader programs (lime_vulkan_program.cpp)
	bool CreateProgramResources ();
	void DestroyProgramResources ();
	bool AllocateUniformBlock (uint32_t size, uint32_t* offset);
	ShaderProgram* FindProgram (uint64_t handle);
	uint64_t CreateShaderProgram (const char* vertexSource, const char* fragmentSource,
		unsigned int flags, const char* cacheDirectory);
	void DestroyShaderProgram (uint64_t handle);
	void DestroyAllShaderPrograms ();
	bool PrepareProgramDraw (ShaderProgram* program, Texture* texture, int blend,
		VkCommandBuffer commandBuffer, uint32_t* dynamicOffset);
	void ResetProgramFrameState ();
	void* FindProgramUniform (ShaderProgram* program, const char* name, int expectedType);

	/** Why the last lime_vulkan_program_create failed (including the translated source).
	 *  lime_vulkan_program_log returns it when the handle is 0: a failure has to stay inspectable. */
	const char* LastProgramFailure ();

}
#endif
