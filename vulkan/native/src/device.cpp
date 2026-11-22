#include "internal.h"

#include "triangle_spirv.h"

#include <algorithm>
#include <cstdio>

#if !defined(LIME_VULKAN_WINDOWS)
#include <sys/stat.h>
#endif

namespace vulkan {

	// ---------------------------------------------------------------------
	// Pipeline cache
	//
	// A driver compiles a pipeline the first time it is used, which is the ~20 ms
	// first frame the perf harness sees. vkPipelineCache lets it reuse that work
	// across runs; the data is validated by the driver against the device and
	// driver UUIDs, so a stale file is ignored rather than trusted.
	//
	// Kept in cache/ next to the executable (relative path, like the shader cache).
	// ---------------------------------------------------------------------

	static const char* kPipelineCachePath = "cache/pipelines.bin";
	/** Above this the file is not worth rewriting: a huge cache slows every start. */
	static const VkDeviceSize kPipelineCacheMax = 8u << 20;

	static void EnsureCacheDirectory () {

		// Cheap and non-fatal: without a directory the cache simply stays in memory.
		#if defined(LIME_VULKAN_WINDOWS)
		CreateDirectoryA ("cache", NULL);
		#else
		mkdir ("cache", 0777);
		#endif

	}

	bool CreatePipelineCache () {

		State& state = S ();
		DeviceApi& dev = state.dev;

		DestroyPipelineCache ();

		if (state.device == VK_NULL_HANDLE || !dev.CreatePipelineCache) return false;

		EnsureCacheDirectory ();

		std::vector<unsigned char> initial;

		if (FILE* file = fopen (kPipelineCachePath, "rb")) {

			fseek (file, 0, SEEK_END);
			long size = ftell (file);
			fseek (file, 0, SEEK_SET);

			if (size > 0 && (VkDeviceSize)size <= kPipelineCacheMax) {

				initial.resize ((size_t)size);

				if (fread (initial.data (), 1, (size_t)size, file) != (size_t)size) initial.clear ();

			}

			fclose (file);

		}

		VkPipelineCacheCreateInfo cacheInfo = {};
		cacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
		cacheInfo.pInitialData = initial.empty () ? NULL : initial.data ();
		cacheInfo.initialDataSize = initial.size ();

		VkResult result = dev.CreatePipelineCache (state.device, &cacheInfo, NULL, &state.pipelineCache);

		if (result != VK_SUCCESS && !initial.empty ()) {

			// The file did not match this driver. Start empty and overwrite it later
			// instead of failing: a stale cache is not an error worth a dialog.
			Log ("pipeline cache rejected (%s), starting empty", ResultText (result));

			remove (kPipelineCachePath);

			cacheInfo.pInitialData = NULL;
			cacheInfo.initialDataSize = 0;

			result = dev.CreatePipelineCache (state.device, &cacheInfo, NULL, &state.pipelineCache);

		}

		if (result != VK_SUCCESS) {

			state.pipelineCache = VK_NULL_HANDLE;
			Log ("no pipeline cache (%s); every start recompiles", ResultText (result));
			return false;

		}

		return true;

	}

	/** Writes the cache back. Called before the device goes away, never on a hot path. */
	void SavePipelineCache () {

		State& state = S ();
		DeviceApi& dev = state.dev;

		if (state.pipelineCache == VK_NULL_HANDLE || state.device == VK_NULL_HANDLE
			|| !dev.GetPipelineCacheData) return;

		// vkGetPipelineCacheData takes size_t, not VkDeviceSize: the Vulkan header
		// spells these out per platform and they are not the same type on Windows.
		size_t size = 0;

		if (dev.GetPipelineCacheData (state.device, state.pipelineCache, &size, NULL) != VK_SUCCESS) return;

		if (size == 0 || (VkDeviceSize)size > kPipelineCacheMax) return;

		std::vector<unsigned char> data (size);

		if (dev.GetPipelineCacheData (state.device, state.pipelineCache, &size, data.data ()) != VK_SUCCESS) return;

		EnsureCacheDirectory ();

		FILE* file = fopen (kPipelineCachePath, "wb");

		if (!file) return;

		fwrite (data.data (), 1, (size_t)size, file);
		fclose (file);

		Log ("pipeline cache written: %.1f KB", (double)size / 1024.0);

	}

	void DestroyPipelineCache () {

		State& state = S ();

		if (state.pipelineCache != VK_NULL_HANDLE && state.device != VK_NULL_HANDLE
			&& state.dev.DestroyPipelineCache) {

			state.dev.DestroyPipelineCache (state.device, state.pipelineCache, NULL);

		}

		state.pipelineCache = VK_NULL_HANDLE;

	}

	// ---------------------------------------------------------------------
	// Vertex format: position.xy + color.rgb (20 bytes, two shader locations)
	// ---------------------------------------------------------------------
	struct Vertex {

		float x, y;
		float r, g, b;

	};

	// Both patterns live in one vertex buffer and firstVertex selects between
	// them, so no buffer is rebuilt and nothing extra is allocated.
	//
	//   offset 0..2: spinning triangle
	//   offset 3..8: full screen gradient (two triangles covering NDC, used to
	//                fill the real game window)
	// The push constant layout has to match the PushConstants block in
	// native/shaders/triangle.vert|frag byte for byte (float, float).
	struct PatternConstants {

		float time;
		float rotate;

	};

	static const Vertex kPatternVertices[] = {

		{ 0.0f, -0.6f, 1.0f, 0.25f, 0.25f },
		{ 0.6f, 0.6f, 0.25f, 1.0f, 0.35f },
		{ -0.6f, 0.6f, 0.25f, 0.45f, 1.0f },

		{ -1.0f, -1.0f, 0.12f, 0.06f, 0.32f },
		{ 1.0f, -1.0f, 0.05f, 0.38f, 0.48f },
		{ -1.0f, 1.0f, 0.58f, 0.12f, 0.36f },

		{ 1.0f, -1.0f, 0.05f, 0.38f, 0.48f },
		{ 1.0f, 1.0f, 0.16f, 0.28f, 0.78f },
		{ -1.0f, 1.0f, 0.58f, 0.12f, 0.36f }

	};

	static const uint32_t kTriangleFirst = 0;
	static const uint32_t kGradientFirst = 3;
	static const uint32_t kGradientCount = 6;

	/** (firstVertex, vertexCount) of the current pattern. */
	static void PatternRange (int pattern, uint32_t* first, uint32_t* count) {

		if (pattern == LIME_VULKAN_PATTERN_GRADIENT) {

			*first = kGradientFirst;
			*count = kGradientCount;
			return;

		}

		*first = kTriangleFirst;
		*count = 3;

	}

	/** The gradient fills the window and must not rotate with time; the triangle does. */
	static float PatternRotation (int pattern) {

		return pattern == LIME_VULKAN_PATTERN_GRADIENT ? 0.0f : 1.0f;

	}

	static bool HasDeviceExtension (VkPhysicalDevice physicalDevice, const char* name) {

		Api& api = S ().api;

		if (!api.EnumerateDeviceExtensionProperties) return false;

		uint32_t count = 0;
		if (api.EnumerateDeviceExtensionProperties (physicalDevice, NULL, &count, NULL) != VK_SUCCESS || count == 0) return false;

		std::vector<VkExtensionProperties> extensions (count);
		if (api.EnumerateDeviceExtensionProperties (physicalDevice, NULL, &count, extensions.data ()) != VK_SUCCESS) return false;

		for (uint32_t i = 0; i < count; ++i) {
			if (strcmp (extensions[i].extensionName, name) == 0) return true;
		}

		return false;

	}

	static bool FindQueueFamilies (VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t* graphics, uint32_t* present) {

		Api& api = S ().api;

		uint32_t count = 0;
		api.GetPhysicalDeviceQueueFamilyProperties (physicalDevice, &count, NULL);
		if (count == 0) return false;

		std::vector<VkQueueFamilyProperties> families (count);
		api.GetPhysicalDeviceQueueFamilyProperties (physicalDevice, &count, families.data ());

		int graphicsIndex = -1;
		int presentIndex = -1;

		for (uint32_t i = 0; i < count; ++i) {

			if (graphicsIndex < 0 && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) graphicsIndex = (int)i;

			if (presentIndex < 0 && surface != VK_NULL_HANDLE) {

				VkBool32 supported = VK_FALSE;

				if (api.GetPhysicalDeviceSurfaceSupportKHR (physicalDevice, i, surface, &supported) == VK_SUCCESS && supported) presentIndex = (int)i;

			}

		}

		if (graphicsIndex < 0 || presentIndex < 0) return false;

		*graphics = (uint32_t)graphicsIndex;
		*present = (uint32_t)presentIndex;
		return true;

	}

	static bool SelectPhysicalDevice (int preferredIndex, uint32_t* graphicsFamily, uint32_t* presentFamily) {

		State& state = S ();
		Api& api = state.api;

		uint32_t count = 0;

		if (api.EnumeratePhysicalDevices (state.instance, &count, NULL) != VK_SUCCESS || count == 0) {

			SetError ("no Vulkan physical device");
			return false;

		}

		std::vector<VkPhysicalDevice> devices (count);
		if (api.EnumeratePhysicalDevices (state.instance, &count, devices.data ()) != VK_SUCCESS) {

			SetError ("vkEnumeratePhysicalDevices failed");
			return false;

		}

		int best = -1;
		int bestScore = -1;

		for (uint32_t i = 0; i < count; ++i) {

			if (preferredIndex >= 0 && (uint32_t)preferredIndex != i) continue;
			if (!HasDeviceExtension (devices[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;

			uint32_t graphics = 0;
			uint32_t present = 0;

			if (!FindQueueFamilies (devices[i], state.surface, &graphics, &present)) continue;

			VkPhysicalDeviceProperties properties;
			api.GetPhysicalDeviceProperties (devices[i], &properties);

			int score = DeviceScore (properties);

			if (score > bestScore) {

				bestScore = score;
				best = (int)i;

			}

		}

		if (best < 0) {

			SetError (preferredIndex >= 0 ? "requested device index cannot present to this surface" : "no device supports swapchain + present");
			return false;

		}

		state.physicalDevice = devices[best];

		VkPhysicalDeviceProperties properties;
		api.GetPhysicalDeviceProperties (state.physicalDevice, &properties);
		state.deviceName = properties.deviceName;

		if (!FindQueueFamilies (state.physicalDevice, state.surface, graphicsFamily, presentFamily)) {

			SetError ("could not resolve queue families");
			return false;

		}

		Log ("device %d: %s (%s, Vulkan %s)", best, properties.deviceName, DeviceTypeText (properties.deviceType),
			VersionText (properties.apiVersion).c_str ());

		return true;

	}

	// Explicitly **UNORM**, not SRGB.
	//
	// OpenGL's default framebuffer does no sRGB encoding: a shader writing 0.5
	// shows 0.5. With B8G8R8A8_SRGB here the driver would treat the shader output
	// as linear and encode it again, so the same colour comes out visibly brighter
	// on the Vulkan path than in GL, which fails the first per-pixel comparison.
	// So UNORM is preferred and SRGB is only the fallback.
	static VkSurfaceFormatKHR ChooseSurfaceFormat (const std::vector<VkSurfaceFormatKHR>& formats) {

		const VkFormat wanted[] = {
			VK_FORMAT_B8G8R8A8_UNORM,
			VK_FORMAT_R8G8B8A8_UNORM,
			VK_FORMAT_A8B8G8R8_UNORM_PACK32
		};

		for (size_t w = 0; w < sizeof (wanted) / sizeof (wanted[0]); ++w) {

			for (size_t i = 0; i < formats.size (); ++i) {

				if (formats[i].format == wanted[w] && formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) return formats[i];

			}

		}

		// Without UNORM, take the first 8 bit format; it at least has to run.
		for (size_t i = 0; i < formats.size (); ++i) {

			if (formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) return formats[i];

		}

		return formats[0];

	}

	static VkPresentModeKHR ChoosePresentMode (const std::vector<VkPresentModeKHR>& modes, int requested) {

		// requested: 0 = auto (uncapped), 1 = mailbox, 2 = immediate, 3 = vsync.
		// Auto prefers mailbox, then immediate, and only falls back to FIFO when the
		// driver offers neither. FIFO as a default caps the game to the refresh rate.
		if (requested == 0) {

			for (size_t i = 0; i < modes.size (); ++i) if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) return VK_PRESENT_MODE_MAILBOX_KHR;
			for (size_t i = 0; i < modes.size (); ++i) if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) return VK_PRESENT_MODE_IMMEDIATE_KHR;
			return VK_PRESENT_MODE_FIFO_KHR;

		}

		VkPresentModeKHR wanted = VK_PRESENT_MODE_FIFO_KHR;
		if (requested == 1) wanted = VK_PRESENT_MODE_MAILBOX_KHR;
		if (requested == 2) wanted = VK_PRESENT_MODE_IMMEDIATE_KHR;

		for (size_t i = 0; i < modes.size (); ++i) {
			if (modes[i] == wanted) return wanted;
		}

		return VK_PRESENT_MODE_FIFO_KHR;

	}

	// ---------------------------------------------------------------------
	// Render targets: image view / render pass / framebuffer
	// ---------------------------------------------------------------------

	void DestroyRenderTargets () {

		State& state = S ();
		DeviceApi& dev = state.dev;

		for (size_t i = 0; i < state.framebuffers.size (); ++i) {
			if (state.framebuffers[i]) dev.DestroyFramebuffer (state.device, state.framebuffers[i], NULL);
		}

		state.framebuffers.clear ();

		for (size_t i = 0; i < state.views.size (); ++i) {
			if (state.views[i]) dev.DestroyImageView (state.device, state.views[i], NULL);
		}

		state.views.clear ();

		if (state.renderPass != VK_NULL_HANDLE) {

			dev.DestroyRenderPass (state.device, state.renderPass, NULL);
			state.renderPass = VK_NULL_HANDLE;

		}

		if (state.surfacePassLoad != VK_NULL_HANDLE) {

			dev.DestroyRenderPass (state.device, state.surfacePassLoad, NULL);
			state.surfacePassLoad = VK_NULL_HANDLE;

		}

		if (state.targetPass != VK_NULL_HANDLE) {

			dev.DestroyRenderPass (state.device, state.targetPass, NULL);
			state.targetPass = VK_NULL_HANDLE;

		}

	}

	/**
	 * One colour attachment, one subpass.
	 *
	 * The three passes here differ only in load op and layouts, which is exactly
	 * what makes them compatible with each other and with the pipelines: Vulkan
	 * compares attachment format and sample count, not the layout transitions.
	 */
	static bool CreateSinglePass (VkFormat format, VkAttachmentLoadOp loadOp,
		VkImageLayout initialLayout, VkImageLayout finalLayout, VkRenderPass* renderPass) {

		State& state = S ();
		DeviceApi& dev = state.dev;

		VkAttachmentDescription colorAttachment = {};
		colorAttachment.format = format;
		colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
		colorAttachment.loadOp = loadOp;
		colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		colorAttachment.initialLayout = initialLayout;
		colorAttachment.finalLayout = finalLayout;

		VkAttachmentReference colorReference = {};
		colorReference.attachment = 0;
		colorReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		VkSubpassDescription subpass = {};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &colorReference;

		// EXTERNAL covers everything recorded before this pass, which includes an
		// earlier pass instance in the same command buffer: that is what makes the
		// LOAD variant actually see what the first window pass drew.
		VkSubpassDependency dependency = {};
		dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
		dependency.dstSubpass = 0;
		dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;

		VkRenderPassCreateInfo renderPassInfo = {};
		renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		renderPassInfo.attachmentCount = 1;
		renderPassInfo.pAttachments = &colorAttachment;
		renderPassInfo.subpassCount = 1;
		renderPassInfo.pSubpasses = &subpass;
		renderPassInfo.dependencyCount = 1;
		renderPassInfo.pDependencies = &dependency;

		if (dev.CreateRenderPass (state.device, &renderPassInfo, NULL, renderPass) != VK_SUCCESS) {

			SetError ("vkCreateRenderPass failed");
			return false;

		}

		return true;

	}

	bool CreateRenderTargets () {

		State& state = S ();
		DeviceApi& dev = state.dev;

		// The window pass leaves the image in COLOR_ATTACHMENT_OPTIMAL: a frame can
		// stop using the window at any draw and go offscreen, and only comes back
		// later. PRESENT_SRC is applied once, in EndFrame, by an explicit barrier.
		if (!CreateSinglePass (state.swapchainFormat, VK_ATTACHMENT_LOAD_OP_CLEAR,
			VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, &state.renderPass)) return false;

		if (!CreateSinglePass (state.swapchainFormat, VK_ATTACHMENT_LOAD_OP_LOAD,
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, &state.surfacePassLoad)) return false;

		if (!CreateSinglePass (state.swapchainFormat, VK_ATTACHMENT_LOAD_OP_CLEAR,
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &state.targetPass)) return false;

		state.views.resize (state.images.size ());
		state.framebuffers.resize (state.images.size ());

		for (size_t i = 0; i < state.images.size (); ++i) {

			VkImageViewCreateInfo viewInfo = {};
			viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			viewInfo.image = state.images[i];
			viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
			viewInfo.format = state.swapchainFormat;
			viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			viewInfo.subresourceRange.baseMipLevel = 0;
			viewInfo.subresourceRange.levelCount = 1;
			viewInfo.subresourceRange.baseArrayLayer = 0;
			viewInfo.subresourceRange.layerCount = 1;

			if (dev.CreateImageView (state.device, &viewInfo, NULL, &state.views[i]) != VK_SUCCESS) {

				SetError ("vkCreateImageView failed");
				return false;

			}

			VkFramebufferCreateInfo framebufferInfo = {};
			framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
			framebufferInfo.renderPass = state.renderPass;
			framebufferInfo.attachmentCount = 1;
			framebufferInfo.pAttachments = &state.views[i];
			framebufferInfo.width = state.extent.width;
			framebufferInfo.height = state.extent.height;
			framebufferInfo.layers = 1;

			if (dev.CreateFramebuffer (state.device, &framebufferInfo, NULL, &state.framebuffers[i]) != VK_SUCCESS) {

				SetError ("vkCreateFramebuffer failed");
				return false;

			}

		}

		return true;

	}

	void DestroySwapchain () {

		State& state = S ();
		DeviceApi& dev = state.dev;

		if (state.device == VK_NULL_HANDLE) return;

		for (size_t i = 0; i < state.inFlight.size (); ++i) {
			if (state.inFlight[i]) dev.DestroyFence (state.device, state.inFlight[i], NULL);
		}

		for (size_t i = 0; i < state.imageAvailable.size (); ++i) {
			if (state.imageAvailable[i]) dev.DestroySemaphore (state.device, state.imageAvailable[i], NULL);
		}

		for (size_t i = 0; i < state.renderFinished.size (); ++i) {
			if (state.renderFinished[i]) dev.DestroySemaphore (state.device, state.renderFinished[i], NULL);
		}

		state.inFlight.clear ();
		state.imageAvailable.clear ();
		state.renderFinished.clear ();

		DestroyRenderTargets ();

		if (state.swapchain != VK_NULL_HANDLE) {

			dev.DestroySwapchainKHR (state.device, state.swapchain, NULL);
			state.swapchain = VK_NULL_HANDLE;

		}

		if (state.commandPool != VK_NULL_HANDLE && !state.commandBuffers.empty ()) {

			// Returned to the pool instead of just clearing the vector: a plain clear
			// leaks one batch of command buffers per resize, and resizing repeatedly
			// would keep growing.
			dev.FreeCommandBuffers (state.device, state.commandPool,
				(uint32_t)state.commandBuffers.size (), state.commandBuffers.data ());

		}

		state.images.clear ();
		state.commandBuffers.clear ();
		state.imageCount = 0;

	}

	static bool CreateSyncObjects () {

		State& state = S ();
		DeviceApi& dev = state.dev;

		VkSemaphoreCreateInfo semaphoreInfo = {};
		semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

		VkFenceCreateInfo fenceInfo = {};
		fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

		state.imageAvailable.resize (MAX_FRAMES_IN_FLIGHT);
		state.inFlight.resize (MAX_FRAMES_IN_FLIGHT);
		state.renderFinished.resize (state.images.size ());

		for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {

			if (dev.CreateSemaphore (state.device, &semaphoreInfo, NULL, &state.imageAvailable[i]) != VK_SUCCESS ||
				dev.CreateFence (state.device, &fenceInfo, NULL, &state.inFlight[i]) != VK_SUCCESS) {

				SetError ("failed to create frame sync objects");
				return false;

			}

		}

		for (size_t i = 0; i < state.renderFinished.size (); ++i) {

			if (dev.CreateSemaphore (state.device, &semaphoreInfo, NULL, &state.renderFinished[i]) != VK_SUCCESS) {

				SetError ("failed to create present semaphores");
				return false;

			}

		}

		state.frameIndex = 0;
		return true;

	}

	// ---------------------------------------------------------------------
	// Triangle pipeline (the first real drawing code; the 2D batch reuses the same path)
	// ---------------------------------------------------------------------

	void DestroyTrianglePipeline () {

		State& state = S ();
		DeviceApi& dev = state.dev;

		if (state.device == VK_NULL_HANDLE) return;

		if (state.pipeline != VK_NULL_HANDLE) {
			dev.DestroyPipeline (state.device, state.pipeline, NULL);
			state.pipeline = VK_NULL_HANDLE;
		}

		if (state.pipelineLayout != VK_NULL_HANDLE) {
			dev.DestroyPipelineLayout (state.device, state.pipelineLayout, NULL);
			state.pipelineLayout = VK_NULL_HANDLE;
		}

		if (state.vertexBuffer != VK_NULL_HANDLE) {
			dev.DestroyBuffer (state.device, state.vertexBuffer, NULL);
			state.vertexBuffer = VK_NULL_HANDLE;
		}

		if (state.vertexMemory != VK_NULL_HANDLE) {
			dev.FreeMemory (state.device, state.vertexMemory, NULL);
			state.vertexMemory = VK_NULL_HANDLE;
		}

	}

	static VkShaderModule CreateShaderModule (const uint32_t* code, size_t byteLength) {

		State& state = S ();

		VkShaderModuleCreateInfo info = {};
		info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		info.codeSize = byteLength;
		info.pCode = code;

		VkShaderModule module = VK_NULL_HANDLE;

		if (state.dev.CreateShaderModule (state.device, &info, NULL, &module) != VK_SUCCESS) return VK_NULL_HANDLE;

		return module;

	}

	static bool CreateVertexBuffer () {

		State& state = S ();
		DeviceApi& dev = state.dev;

		VkBufferCreateInfo bufferInfo = {};
		bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bufferInfo.size = sizeof (kPatternVertices);
		bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
		bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		if (dev.CreateBuffer (state.device, &bufferInfo, NULL, &state.vertexBuffer) != VK_SUCCESS) {

			SetError ("vkCreateBuffer(vertex) failed");
			return false;

		}

		VkMemoryRequirements requirements;
		dev.GetBufferMemoryRequirements (state.device, state.vertexBuffer, &requirements);

		VkPhysicalDeviceMemoryProperties memory;
		state.api.GetPhysicalDeviceMemoryProperties (state.physicalDevice, &memory);

		uint32_t memoryType = 0;
		bool found = false;

		// The vertex data is tiny, so host visible and coherent avoids a staging copy.
		for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {

			bool usable = (requirements.memoryTypeBits & (1u << i)) != 0;
			bool visible = (memory.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
				(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

			if (usable && visible) { memoryType = i; found = true; break; }

		}

		if (!found) {

			SetError ("no host visible memory type for the vertex buffer");
			return false;

		}

		VkMemoryAllocateInfo allocateInfo = {};
		allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocateInfo.allocationSize = requirements.size;
		allocateInfo.memoryTypeIndex = memoryType;

		if (dev.AllocateMemory (state.device, &allocateInfo, NULL, &state.vertexMemory) != VK_SUCCESS) {

			SetError ("vkAllocateMemory(vertex) failed");
			return false;

		}

		if (dev.BindBufferMemory (state.device, state.vertexBuffer, state.vertexMemory, 0) != VK_SUCCESS) {

			SetError ("vkBindBufferMemory failed");
			return false;

		}

		void* data = 0;

		if (dev.MapMemory (state.device, state.vertexMemory, 0, sizeof (kPatternVertices), 0, &data) != VK_SUCCESS || !data) {

			SetError ("vkMapMemory(vertex) failed");
			return false;

		}

		memcpy (data, kPatternVertices, sizeof (kPatternVertices));
		dev.UnmapMemory (state.device, state.vertexMemory);

		return true;

	}

	bool CreateTrianglePipeline () {

		State& state = S ();
		DeviceApi& dev = state.dev;

		VkShaderModule vertexModule = CreateShaderModule (kTriangleVertexSpirv, sizeof (kTriangleVertexSpirv));
		VkShaderModule fragmentModule = CreateShaderModule (kTriangleFragmentSpirv, sizeof (kTriangleFragmentSpirv));

		if (vertexModule == VK_NULL_HANDLE || fragmentModule == VK_NULL_HANDLE) {

			SetError ("shader module creation failed (SPIR-V rejected)");
			return false;

		}

		VkPipelineShaderStageCreateInfo stages[2] = {};
		stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
		stages[0].module = vertexModule;
		stages[0].pName = "main";
		stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		stages[1].module = fragmentModule;
		stages[1].pName = "main";

		VkVertexInputBindingDescription binding = {};
		binding.binding = 0;
		binding.stride = sizeof (Vertex);
		binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

		VkVertexInputAttributeDescription attributes[2] = {};
		attributes[0].location = 0;
		attributes[0].binding = 0;
		attributes[0].format = VK_FORMAT_R32G32_SFLOAT;
		attributes[0].offset = 0;
		attributes[1].location = 1;
		attributes[1].binding = 0;
		attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
		attributes[1].offset = 8;

		VkPipelineVertexInputStateCreateInfo vertexInput = {};
		vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
		vertexInput.vertexBindingDescriptionCount = 1;
		vertexInput.pVertexBindingDescriptions = &binding;
		vertexInput.vertexAttributeDescriptionCount = 2;
		vertexInput.pVertexAttributeDescriptions = attributes;

		VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
		inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
		inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

		VkPipelineViewportStateCreateInfo viewportState = {};
		viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
		viewportState.viewportCount = 1;
		viewportState.scissorCount = 1;

		VkPipelineRasterizationStateCreateInfo rasterizer = {};
		rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
		rasterizer.cullMode = VK_CULL_MODE_NONE;
		rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		rasterizer.lineWidth = 1.0f;

		VkPipelineMultisampleStateCreateInfo multisampling = {};
		multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
		multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

		VkPipelineColorBlendAttachmentState blendAttachment = {};
		blendAttachment.blendEnable = VK_FALSE;
		blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

		VkPipelineColorBlendStateCreateInfo colorBlend = {};
		colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
		colorBlend.attachmentCount = 1;
		colorBlend.pAttachments = &blendAttachment;

		VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };

		VkPipelineDynamicStateCreateInfo dynamicState = {};
		dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
		dynamicState.dynamicStateCount = 2;
		dynamicState.pDynamicStates = dynamicStates;

		VkPushConstantRange pushRange = {};
		pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
		pushRange.offset = 0;
		pushRange.size = sizeof (PatternConstants);

		VkPipelineLayoutCreateInfo layoutInfo = {};
		layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		layoutInfo.pushConstantRangeCount = 1;
		layoutInfo.pPushConstantRanges = &pushRange;

		if (dev.CreatePipelineLayout (state.device, &layoutInfo, NULL, &state.pipelineLayout) != VK_SUCCESS) {

			dev.DestroyShaderModule (state.device, vertexModule, NULL);
			dev.DestroyShaderModule (state.device, fragmentModule, NULL);
			SetError ("vkCreatePipelineLayout failed");
			return false;

		}

		VkGraphicsPipelineCreateInfo pipelineInfo = {};
		pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		pipelineInfo.stageCount = 2;
		pipelineInfo.pStages = stages;
		pipelineInfo.pVertexInputState = &vertexInput;
		pipelineInfo.pInputAssemblyState = &inputAssembly;
		pipelineInfo.pViewportState = &viewportState;
		pipelineInfo.pRasterizationState = &rasterizer;
		pipelineInfo.pMultisampleState = &multisampling;
		pipelineInfo.pColorBlendState = &colorBlend;
		pipelineInfo.pDynamicState = &dynamicState;
		pipelineInfo.layout = state.pipelineLayout;
		pipelineInfo.renderPass = state.renderPass;
		pipelineInfo.subpass = 0;

		VkResult result = dev.CreateGraphicsPipelines (state.device, state.pipelineCache, 1, &pipelineInfo, NULL, &state.pipeline);

		dev.DestroyShaderModule (state.device, vertexModule, NULL);
		dev.DestroyShaderModule (state.device, fragmentModule, NULL);

		if (result != VK_SUCCESS) {

			SetError ("vkCreateGraphicsPipelines failed");
			return false;

		}

		if (!CreateVertexBuffer ()) return false;

		Log ("triangle pipeline ready (%s)", state.swapchainFormat == VK_FORMAT_B8G8R8A8_SRGB ? "bgra8-srgb" : "other format");
		return true;

	}

	// ---------------------------------------------------------------------

	bool CreateSwapchain (int width, int height) {

		State& state = S ();
		Api& api = state.api;
		DeviceApi& dev = state.dev;

		VkSurfaceCapabilitiesKHR capabilities;

		if (api.GetPhysicalDeviceSurfaceCapabilitiesKHR (state.physicalDevice, state.surface, &capabilities) != VK_SUCCESS) {

			SetError ("vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed");
			return false;

		}

		uint32_t formatCount = 0;
		api.GetPhysicalDeviceSurfaceFormatsKHR (state.physicalDevice, state.surface, &formatCount, NULL);

		uint32_t modeCount = 0;
		api.GetPhysicalDeviceSurfacePresentModesKHR (state.physicalDevice, state.surface, &modeCount, NULL);

		if (formatCount == 0 || modeCount == 0) {

			SetError ("surface reports no formats or present modes");
			return false;

		}

		std::vector<VkSurfaceFormatKHR> formats (formatCount);
		api.GetPhysicalDeviceSurfaceFormatsKHR (state.physicalDevice, state.surface, &formatCount, formats.data ());

		std::vector<VkPresentModeKHR> modes (modeCount);
		api.GetPhysicalDeviceSurfacePresentModesKHR (state.physicalDevice, state.surface, &modeCount, modes.data ());

		VkSurfaceFormatKHR surfaceFormat = ChooseSurfaceFormat (formats);
		VkPresentModeKHR presentMode = ChoosePresentMode (modes, state.options.presentMode);

		state.surfaceTransform = capabilities.currentTransform;
		state.surfaceTransforms = capabilities.supportedTransforms;
		state.surfaceExtent = capabilities.currentExtent;
		state.surfaceMinExtent = capabilities.minImageExtent;
		state.surfaceMaxExtent = capabilities.maxImageExtent;

		VkExtent2D extent = capabilities.currentExtent;

		// The engine always renders in the window's logical orientation: the Haxe
		// side builds its projection from the lime window size, and the viewport and
		// scissor come from this extent. A surface whose currentTransform is not
		// IDENTITY (Android does that on a rotated display: the panel is portrait,
		// the game is landscape) would have the compositor rotate that already
		// upright image by the same angle, so the game is drawn sideways and
		// squeezed and a 90 degree pre-rotation looks like a broken portrait mode.
		// Asking for IDENTITY keeps the image 1:1 with the window. Only fall back to
		// the surface transform when IDENTITY is not offered at all.
		VkSurfaceTransformFlagBitsKHR preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;

		if (!(capabilities.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)) {

			preTransform = capabilities.currentTransform;

		}

		state.surfacePreTransform = preTransform;

		{
			std::string fmts;

			for (uint32_t i = 0; i < formatCount; ++i) {

				fmts += std::to_string ((int)formats[i].format) + "/" + std::to_string ((int)formats[i].colorSpace) + " ";

			}

			LIME_VK_LOGCAT ("surface caps: %ux%u min=%u max=%u transform=%d pre=%d formats=[%s] modes=%u",
				capabilities.currentExtent.width, capabilities.currentExtent.height,
				capabilities.minImageCount, capabilities.maxImageCount,
				(int)capabilities.currentTransform, (int)preTransform,
				fmts.c_str(), (unsigned)modeCount);
		}

		if (extent.width == 0xFFFFFFFFu) {

			extent.width = (uint32_t)std::max (1, std::min ((int)capabilities.maxImageExtent.width, width));
			extent.height = (uint32_t)std::max (1, std::min ((int)capabilities.maxImageExtent.height, height));

		} else if (preTransform == VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR && width > 0 && height > 0
			&& (int)extent.width != width && (int)extent.height != height
			&& (int)extent.width == height && (int)extent.height == width) {

			// Some surfaces report the extent in the panel's orientation while the
			// window itself is rotated. With IDENTITY the image must be in window
			// space, so swap the axes when they are simply transposed.
			extent.width = (uint32_t)width;
			extent.height = (uint32_t)height;

		}

		// A minimised window reports 0x0. Never build a zero pixel swapchain: it
		// writes state.extent = 0, every later frame computes the projection and the
		// scissor from 0, and the window does not recover on its own (the player sees
		// a black window after restoring it).
		if (extent.width == 0 || extent.height == 0) {

			SetError ("surface reports no extent (window minimized?)");
			return false;

		}

		uint32_t imageCount = capabilities.minImageCount + 1;

		if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount) imageCount = capabilities.maxImageCount;

		VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;

		if (!(capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) {

			if (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;

		}

		VkSwapchainCreateInfoKHR createInfo = {};
		createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
		createInfo.surface = state.surface;
		createInfo.minImageCount = imageCount;
		createInfo.imageFormat = surfaceFormat.format;
		createInfo.imageColorSpace = surfaceFormat.colorSpace;
		createInfo.imageExtent = extent;
		createInfo.imageArrayLayers = 1;
		createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		createInfo.preTransform = preTransform;
		createInfo.compositeAlpha = compositeAlpha;
		createInfo.presentMode = presentMode;
		createInfo.clipped = VK_TRUE;

		uint32_t families[2] = { state.graphicsFamily, state.presentFamily };

		if (state.graphicsFamily != state.presentFamily) {

			createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
			createInfo.queueFamilyIndexCount = 2;
			createInfo.pQueueFamilyIndices = families;

		}

		LIME_VK_LOGCAT ("swapchain create: %ux%u images=%u fmt=%d cs=%d pre=%d cur=%d modes=%u alpha=0x%x clipped=%d sharing=%d",
			extent.width, extent.height, imageCount, (int)surfaceFormat.format, (int)surfaceFormat.colorSpace,
			(int)preTransform, (int)capabilities.currentTransform, (unsigned)modeCount,
			(unsigned)compositeAlpha, (int)createInfo.clipped, (int)createInfo.imageSharingMode);

		VkResult result = dev.CreateSwapchainKHR (state.device, &createInfo, NULL, &state.swapchain);

		if (result != VK_SUCCESS) {

			std::string message = "vkCreateSwapchainKHR failed: ";
			message += ResultText (result);
			SetError (message.c_str ());
			return false;

		}

		LIME_VK_LOGCAT ("swapchain created (%d)", (int)result);

		uint32_t swapchainImageCount = 0;
		dev.GetSwapchainImagesKHR (state.device, state.swapchain, &swapchainImageCount, NULL);

		state.images.resize (swapchainImageCount);

		if (dev.GetSwapchainImagesKHR (state.device, state.swapchain, &swapchainImageCount, state.images.data ()) != VK_SUCCESS) {

			SetError ("vkGetSwapchainImagesKHR failed");
			return false;

		}

		state.imageCount = swapchainImageCount;
		state.swapchainFormat = surfaceFormat.format;
		state.swapchainColorSpace = surfaceFormat.colorSpace;
		state.extent = extent;

		if (state.commandPool == VK_NULL_HANDLE) {

			VkCommandPoolCreateInfo poolInfo = {};
			poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
			poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
			poolInfo.queueFamilyIndex = state.graphicsFamily;

			if (dev.CreateCommandPool (state.device, &poolInfo, NULL, &state.commandPool) != VK_SUCCESS) {

				SetError ("vkCreateCommandPool failed");
				return false;

			}

		}

		// Command buffers are allocated per **frame slot**, not per swapchain image.
		// Per image is a trap: frame N can get image 0 and frame N+1 can get image 0
		// again (there are at least three), while the submission of frame N may still
		// be running, so ResetCommandBuffer hits 'still in use' and NVIDIA returns
		// VK_ERROR_DEVICE_LOST (measured the first time the game ran). The fence of a
		// frame slot has already been waited on in BeginFrame, so slots are safe.
		state.commandBuffers.resize (MAX_FRAMES_IN_FLIGHT);

		VkCommandBufferAllocateInfo allocateInfo = {};
		allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocateInfo.commandPool = state.commandPool;
		allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocateInfo.commandBufferCount = MAX_FRAMES_IN_FLIGHT;

		if (dev.AllocateCommandBuffers (state.device, &allocateInfo, state.commandBuffers.data ()) != VK_SUCCESS) {

			SetError ("vkAllocateCommandBuffers failed");
			return false;

		}

		LIME_VK_LOGCAT ("swapchain images acquired (%u)", state.imageCount);

		if (!CreateRenderTargets ()) return false;

		LIME_VK_LOGCAT ("swapchain render targets ready");

		if (!CreateSyncObjects ()) return false;

		LIME_VK_LOGCAT ("swapchain sync objects ready");

		LIME_VK_LOGCAT ("swapchain: creating the triangle pipeline");

		DestroyTrianglePipeline ();
		if (!CreateTrianglePipeline ()) return false;

		LIME_VK_LOGCAT ("swapchain: triangle pipeline ready, batch resources next");

		DestroyBatchResources ();
		if (!CreateBatchResources ()) return false;

		LIME_VK_LOGCAT ("swapchain: batch resources ready");

		Log ("swapchain %ux%u, %u image(s), %s (requested %dx%d, surface %ux%u, transform %d pre %d of 0x%x, extent range %ux%u..%ux%u)",
			extent.width, extent.height, state.imageCount, PresentModeText (presentMode),
			width, height, capabilities.currentExtent.width, capabilities.currentExtent.height,
			(int)capabilities.currentTransform, (int)preTransform, (unsigned)capabilities.supportedTransforms,
			capabilities.minImageExtent.width, capabilities.minImageExtent.height,
			capabilities.maxImageExtent.width, capabilities.maxImageExtent.height);
		return true;

	}

	bool AttachDevice (void* window, int width, int height, bool withSwapchain) {

		State& state = S ();

		if (state.attached) DetachDevice ();

		if (!window) {

			SetError ("null native window");
			return false;

		}

		state.window = window;

		LIME_VK_LOGCAT ("attach: creating the instance");

		if (!CreateInstance (state.options.enableValidation != 0, state.options.verbose != 0, &state.instance)) {

			DetachDevice ();
			return false;

		}

		LIME_VK_LOGCAT ("attach: instance created, loading the instance API");

		if (!LoadInstanceApi (state.instance)) {

			DetachDevice ();
			return false;

		}

		LIME_VK_LOGCAT ("attach: creating the platform surface");

		if (!CreatePlatformSurface (state.instance, window, &state.surface)) {

			DetachDevice ();
			return false;

		}

		uint32_t graphicsFamily = 0;
		uint32_t presentFamily = 0;

		LIME_VK_LOGCAT ("attach: platform surface done, selecting the device");

		if (!SelectPhysicalDevice (state.options.deviceIndex, &graphicsFamily, &presentFamily)) {

			DetachDevice ();
			return false;

		}

		state.graphicsFamily = graphicsFamily;
		state.presentFamily = presentFamily;

		float priority = 1.0f;
		uint32_t uniqueFamilies[2] = { graphicsFamily, presentFamily };
		uint32_t uniqueCount = (graphicsFamily == presentFamily) ? 1 : 2;

		VkDeviceQueueCreateInfo queueInfos[2] = {};
		queueInfos[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		queueInfos[0].queueFamilyIndex = uniqueFamilies[0];
		queueInfos[0].queueCount = 1;
		queueInfos[0].pQueuePriorities = &priority;

		if (uniqueCount == 2) {

			queueInfos[1] = queueInfos[0];
			queueInfos[1].queueFamilyIndex = uniqueFamilies[1];

		}

		const char* deviceExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

		VkDeviceCreateInfo deviceInfo = {};
		deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		deviceInfo.queueCreateInfoCount = uniqueCount;
		deviceInfo.pQueueCreateInfos = queueInfos;
		deviceInfo.enabledExtensionCount = 1;
		deviceInfo.ppEnabledExtensionNames = deviceExtensions;

		LIME_VK_LOGCAT ("attach: device selected, creating the device");

		VkResult result = state.api.CreateDevice (state.physicalDevice, &deviceInfo, NULL, &state.device);

		if (result != VK_SUCCESS) {

			std::string message = "vkCreateDevice failed: ";
			message += ResultText (result);
			SetError (message.c_str ());
			DetachDevice ();
			return false;

		}

		LIME_VK_LOGCAT ("attach: device created, loading the device API");

		if (!LoadDeviceApi (state.device)) {

			DetachDevice ();
			return false;

		}

		state.dev.GetDeviceQueue (state.device, graphicsFamily, 0, &state.graphicsQueue);
		state.dev.GetDeviceQueue (state.device, presentFamily, 0, &state.presentQueue);

		LIME_VK_LOGCAT ("attach: device API loaded, creating program resources");

		// GPU timestamps: optional, and the stats text says so when they are missing.
		CreatePerfQueries ();

		// Before any pipeline exists, so the first one already hits the cache.
		CreatePipelineCache ();

		// Descriptor layout, pool and the 1x1 white texture are independent of the swapchain and follow the device.
		LIME_VK_LOGCAT ("attach: perf queries and pipeline cache done");

		if (!CreateProgramResources ()) {

			return false;

		}

		LIME_VK_LOGCAT ("attach: program resources done, creating textures");

		if (!CreateTextureResources ()) {

			DetachDevice ();
			return false;

		}

		LIME_VK_LOGCAT ("attach: textures done, creating the swapchain");

		if (withSwapchain && !CreateSwapchain (width, height)) {

			DetachDevice ();
			return false;

		}

		#if defined(LIME_VULKAN_ANDROID)
		// The surface this swapchain belongs to is now the applied one: only a *newer*
		// handle from the activity counts as a change.
		MarkAndroidWindowApplied (window);
		#endif

		state.attached = withSwapchain;
		state.framesPresented = 0;
		state.lastError.clear ();
		return true;

	}

	void DetachDevice () {

		State& state = S ();

		if (state.device != VK_NULL_HANDLE) {

			if (state.dev.DeviceWaitIdle) state.dev.DeviceWaitIdle (state.device);

			// Written while the pipelines still exist: the cache only holds data about
			// the objects that were created with it.
			SavePipelineCache ();

			DestroyPerfQueries ();

			// Order matters: pipelines reference the render pass and the descriptor set
			// layouts, so the batch pipelines go first, then the swapchain and render
			// passes, then the descriptors. User programs as well, since their pipelines
			// reference the same render pass and layouts.
			DestroyProgramResources ();
			DestroyBatchResources ();
			DestroyTrianglePipeline ();
			DestroySwapchain ();
			DestroyTextureResources ();

			if (state.commandPool != VK_NULL_HANDLE && state.dev.DestroyCommandPool) {

				state.dev.DestroyCommandPool (state.device, state.commandPool, NULL);
				state.commandPool = VK_NULL_HANDLE;

			}

			DestroyPipelineCache ();

			if (state.dev.DestroyDevice) state.dev.DestroyDevice (state.device, NULL);

			state.device = VK_NULL_HANDLE;

		}

		if (state.surface != VK_NULL_HANDLE && state.api.DestroySurfaceKHR) {

			state.api.DestroySurfaceKHR (state.instance, state.surface, NULL);
			state.surface = VK_NULL_HANDLE;

		}

		if (state.instance != VK_NULL_HANDLE && state.api.DestroyInstance) {

			state.api.DestroyInstance (state.instance, NULL);
			state.instance = VK_NULL_HANDLE;

		}

		state.physicalDevice = VK_NULL_HANDLE;
		state.graphicsQueue = VK_NULL_HANDLE;
		state.presentQueue = VK_NULL_HANDLE;
		state.attached = false;

	}

	bool RecreateSwapchain () {

		State& state = S ();

		if (state.device == VK_NULL_HANDLE) return false;

		if (state.extent.width == 0 || state.extent.height == 0) return false;

		state.dev.DeviceWaitIdle (state.device);

		// DestroySwapchain destroys the old handle itself: clearing the swapchain
		// first would leak it, and the next vkCreateSwapchainKHR would fail with
		// VK_ERROR_NATIVE_WINDOW_IN_USE_KHR (the window stays white).
		DestroySwapchain ();

		if (!CreateSwapchain ((int)state.extent.width, (int)state.extent.height)) {

			state.swapchain = VK_NULL_HANDLE;
			return false;

		}

		state.resized = false;
		return true;

	}

	#if defined(LIME_VULKAN_ANDROID)

	// Provided by lime's SDL3 window: destroys the EGL surface that keeps the window away
	// from Vulkan. Weak, because the standalone build has no lime to ask.
	extern "C" int lime_vulkan_android_release_gl (void) __attribute__ ((weak));

	// A new ANativeWindow: the VkSurfaceKHR belongs to the surface the activity tore down,
	// so it goes first - a swapchain cannot be created for a surface that is gone.
	bool RecreateAndroidSurface (void* window) {

		State& state = S ();

		if (state.device == VK_NULL_HANDLE || !state.attached) return false;

		if (!window) {

			SetError ("the Android surface went away; nothing can be presented");
			return false;

		}

		// Before the size probe: the caller's "is this window minimised" check reads
		// state.window, and the old handle is exactly the one that no longer exists.
		state.window = window;

		int width = 0;
		int height = 0;

		if (!WindowClientSize (window, &width, &height) || width <= 0 || height <= 0) {

			// A resumed activity can report the surface before it is laid out. Not marked
			// as applied, so the next frame tries again instead of presenting into a
			// destroyed window.
			SetError ("the Android window has no client area yet; frame skipped");
			return false;

		}

		state.dev.DeviceWaitIdle (state.device);

		DestroySwapchain ();

		if (state.surface != VK_NULL_HANDLE && state.api.DestroySurfaceKHR) {

			state.api.DestroySurfaceKHR (state.instance, state.surface, NULL);
			state.surface = VK_NULL_HANDLE;

		}

		// A resume gives the activity a new Surface, and SDL immediately puts a new EGL
		// surface on it: the same producer that has to be gone before Vulkan may take the
		// window. Dropping it here keeps the order right (this runs on the render thread,
		// right before the surface is needed).
		if (lime_vulkan_android_release_gl) lime_vulkan_android_release_gl ();

		if (!CreatePlatformSurface (state.instance, window, &state.surface)) return false;

		state.extent.width = (uint32_t)width;
		state.extent.height = (uint32_t)height;

		if (!CreateSwapchain (width, height)) {

			state.swapchain = VK_NULL_HANDLE;
			return false;

		}

		state.resized = false;
		MarkAndroidWindowApplied (window);

		Log ("Android surface rebuilt for a new window (%dx%d)", width, height);
		return true;

	}

	#endif

	// ---------------------------------------------------------------------

	// A frame is three steps (BeginFrame, draws, EndFrame); PresentFrame is only the
	// 'draw a test pattern' use. Split so the batch inserts any number of draws in
	// the middle while the wait, acquire and submit sequence stays in one place.
	bool BeginFrame (const float clearColor[4]) {

		State& state = S ();
		DeviceApi& dev = state.dev;

		double beginTotalStart = PerfNow ();

		if (state.inFrame) {

			SetError ("BeginFrame called twice without EndFrame");
			return false;

		}

		if (!state.attached || state.swapchain == VK_NULL_HANDLE) {

			SetError ("present without an attached swapchain");
			return false;

		}

		#if defined(LIME_VULKAN_ANDROID)

		// The activity recreated its surface (resume, rotation, split screen, a permission
		// dialog). The swapchain of the old ANativeWindow cannot present any more, and the
		// new handle arrives from the Java/SDL thread, so it is taken here - at a frame
		// boundary, on the render thread - and the surface is rebuilt before any draw.
		void* changedWindow = NULL;

		if (TakeChangedAndroidWindow (&changedWindow)) {

			// RecreateAndroidSurface points state.window at the new handle even when the
			// rebuild has to wait for a client area, so the failure below is reported the
			// same way a minimised window is: the frame is skipped, the renderer stays
			// attached, and the next frame retries.
			if (!RecreateAndroidSurface (changedWindow)) return false;

		}

		#endif

		if (state.resized) {

			// When the rebuild failed the sync objects are already cleared, and continuing would index inFlight out of range.
			if (!RecreateSwapchain ()) {

				int windowWidth = 0;
				int windowHeight = 0;

				// No client area means minimised. attached must **not** be cleared here:
				// that puts the whole session into an unrecoverable 'no context' state, and
				// the player only minimised the window. Let the caller skip the frame.
				if (state.window != NULL
					&& WindowClientSize (state.window, &windowWidth, &windowHeight)
					&& (windowWidth <= 0 || windowHeight <= 0)) {

					return false;

				}

				state.attached = false;
				return false;

			}

		}

		if (state.inFlight.size () < (size_t)MAX_FRAMES_IN_FLIGHT || state.commandBuffers.empty ()) {

			SetError ("present before the swapchain finished recreating");
			state.attached = false;
			return false;

		}

		PerfResetFrame ();

		VkFence fence = state.inFlight[state.frameIndex];
		double waitStart = PerfNow ();
		VkResult fenceResult = dev.WaitForFences (state.device, 1, &fence, VK_TRUE, UINT64_MAX);
		state.waits.fence = (float)((PerfNow () - waitStart) * 1000.0);

		if (fenceResult != VK_SUCCESS) {

			// Always include the result code: device loss and timeout are completely
			// different things with identical symptoms (a frame that hangs). This line
			// is how the first game run was diagnosed.
			std::string message = "vkWaitForFences failed: ";
			message += ResultText (fenceResult);
			SetError (message.c_str ());
			return false;

		}

		// The same fence that made the slot safe also makes its GPU timestamps
		// readable: pair them with the CPU half that was stored when it presented.
		PerfCollectFrame (state.frameIndex);

		// The fence of this frame slot has passed, so the commands that used it last
		// have finished: only now is it safe to recycle its descriptor sets and the
		// uniform ring buffer.
		ResetProgramFrameState ();

		// Same reasoning for the upload channel: this slot's staging ring and command
		// buffer are only rewritable once the frame that last used them finished, and
		// this fence is that proof. Nothing waits for an upload inside a frame.
		BeginUploadFrame ();

		// Textures retired long enough are really freed here. Releasing them anywhere
		// else is unsafe, since a descriptor set may still be referenced by a
		// submitted command and that costs the device.
		DrainRetiredTextures ();

		uint32_t imageIndex = 0;
		double acquireStart = PerfNow ();
		VkResult acquire = dev.AcquireNextImageKHR (state.device, state.swapchain, UINT64_MAX,
			state.imageAvailable[state.frameIndex], VK_NULL_HANDLE, &imageIndex);
		state.waits.acquire = (float)((PerfNow () - acquireStart) * 1000.0);

		if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {

			state.swapchainRebuilds++;
			RecreateSwapchain ();
			return true;

		}

		if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {

			std::string message = "vkAcquireNextImageKHR failed: ";
			message += ResultText (acquire);
			SetError (message.c_str ());
			return false;

		}

		if (imageIndex >= state.framebuffers.size ()) {

			SetError ("unexpected swapchain image index");
			return false;

		}

		dev.ResetFences (state.device, 1, &fence);

		VkCommandBuffer commandBuffer = state.commandBuffers[state.frameIndex];
		dev.ResetCommandBuffer (commandBuffer, 0);

		VkCommandBufferBeginInfo beginInfo = {};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

		if (dev.BeginCommandBuffer (commandBuffer, &beginInfo) != VK_SUCCESS) {

			SetError ("vkBeginCommandBuffer failed");
			return false;

		}

		PerfBeginFrame (commandBuffer, state.frameIndex);

		// The render pass is opened by the first draw instead of here: a frame that
		// draws a filtered camera goes offscreen first, and the window pass must not
		// be open while an offscreen pass is being recorded.
		state.clearColor[0] = clearColor ? clearColor[0] : 0.05f;
		state.clearColor[1] = clearColor ? clearColor[1] : 0.05f;
		state.clearColor[2] = clearColor ? clearColor[2] : 0.08f;
		state.clearColor[3] = clearColor ? clearColor[3] : 1.0f;

		state.surfacePassOpen = false;
		state.surfacePassCleared = false;
		state.targetPassOpen = false;
		state.activeTarget = NULL;

		state.currentImageIndex = imageIndex;
		state.frameCommandBuffer = commandBuffer;
		state.inFrame = true;

		state.waits.beginTotal = (float)((PerfNow () - beginTotalStart) * 1000.0);

		return true;

	}

	// Closing: end the render pass, submit, present. Must pair with BeginFrame.
	bool EndFrame () {

		State& state = S ();
		DeviceApi& dev = state.dev;

		double endTotalStart = PerfNow ();

		if (!state.inFrame) {

			SetError ("EndFrame without BeginFrame");
			return false;

		}

		// Still "in frame" until the passes are closed: reopening the window pass
		// below goes through the same guard as a normal draw.
		uint32_t imageIndex = state.currentImageIndex;
		VkCommandBuffer commandBuffer = state.frameCommandBuffer;
		VkFence fence = state.inFlight[state.frameIndex];

		// Close whichever pass the batch left open. A frame that drew nothing still
		// has to clear the window, so the pass is opened once and closed again.
		double endPassStart = PerfNow ();
		EndActivePass ();
		state.waits.endPass = (float)((PerfNow () - endPassStart) * 1000.0);

		if (!state.surfacePassCleared) {

			BeginSurfacePass (true);
			EndSurfacePass ();

		}

		// The window pass leaves the image in COLOR_ATTACHMENT_OPTIMAL so it can be
		// reopened mid-frame; presenting requires PRESENT_SRC.
		VkImageMemoryBarrier presentBarrier = {};
		presentBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		presentBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		presentBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		presentBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		presentBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		presentBarrier.image = state.images[imageIndex];
		presentBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		presentBarrier.subresourceRange.baseMipLevel = 0;
		presentBarrier.subresourceRange.levelCount = 1;
		presentBarrier.subresourceRange.baseArrayLayer = 0;
		presentBarrier.subresourceRange.layerCount = 1;
		presentBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		presentBarrier.dstAccessMask = 0;

		double presentBarrierStart = PerfNow ();

		dev.CmdPipelineBarrier (commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &presentBarrier);

		state.waits.presentBarrier = (float)((PerfNow () - presentBarrierStart) * 1000.0);

		state.inFrame = false;

		double timestampStart = PerfNow ();
		PerfEndFrame (commandBuffer, state.frameIndex);
		state.waits.timestamp = (float)((PerfNow () - timestampStart) * 1000.0);

		double endBufferStart = PerfNow ();
		VkResult endBufferResult = dev.EndCommandBuffer (commandBuffer);
		state.waits.endBuffer = (float)((PerfNow () - endBufferStart) * 1000.0);

		if (endBufferResult != VK_SUCCESS) {

			SetError ("vkEndCommandBuffer failed");
			return false;

		}

		// Everything queued by texture creation or update goes out as one submit
		// ahead of this frame's commands: same queue, so the finished copies are
		// visible to the draws below through the barrier at the end of that buffer.
		// No semaphore and no fence: the frame's own fence already covers it, and
		// waiting for the copy here is what kept the CPU a whole frame behind.
		FlushTextureUploads (true);

		VkSemaphore waitSemaphores[1];
		VkPipelineStageFlags waitStages[1];
		uint32_t waitCount = 0;

		waitSemaphores[waitCount] = state.imageAvailable[state.frameIndex];
		waitStages[waitCount] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		waitCount++;

		VkSubmitInfo submitInfo = {};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.waitSemaphoreCount = waitCount;
		submitInfo.pWaitSemaphores = waitSemaphores;
		submitInfo.pWaitDstStageMask = waitStages;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &commandBuffer;
		submitInfo.signalSemaphoreCount = 1;
		submitInfo.pSignalSemaphores = &state.renderFinished[imageIndex];

		double submitStart = PerfNow ();
		VkResult submitResult = dev.QueueSubmit (state.graphicsQueue, 1, &submitInfo, fence);
		state.waits.submit = (float)((PerfNow () - submitStart) * 1000.0);

		if (submitResult != VK_SUCCESS) {

			std::string message = "vkQueueSubmit failed: ";
			message += ResultText (submitResult);
			SetError (message.c_str ());
			return false;

		}

		VkPresentInfoKHR presentInfo = {};
		presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		presentInfo.waitSemaphoreCount = 1;
		presentInfo.pWaitSemaphores = &state.renderFinished[imageIndex];
		presentInfo.swapchainCount = 1;
		presentInfo.pSwapchains = &state.swapchain;
		presentInfo.pImageIndices = &imageIndex;

		double presentStart = PerfNow ();
		VkResult present = dev.QueuePresentKHR (state.presentQueue, &presentInfo);
		state.waits.present = (float)((PerfNow () - presentStart) * 1000.0);

		if (present == VK_ERROR_OUT_OF_DATE_KHR) {

			state.swapchainRebuilds++;
			RecreateSwapchain ();

		} else if (present == VK_SUBOPTIMAL_KHR) {

			// Advisory, and on Android the driver reports it forever once the
			// swapchain is pre-rotated by us (that is what IDENTITY preTransform
			// means here: the display transform is applied at composition time).
			// Rebuilding on it was a full swapchain - nine images, nine
			// framebuffers, vkDeviceWaitIdle - on every single frame, which is
			// what made the game run at 33 fps on a Mali-G610 while OpenGL ran at
			// 120. A real change of window size still rebuilds.
			state.presentSuboptimal++;

			int windowWidth = 0;
			int windowHeight = 0;

			if (state.window != NULL
				&& WindowClientSize (state.window, &windowWidth, &windowHeight)
				&& windowWidth > 0 && windowHeight > 0
				&& ((uint32_t)windowWidth != state.extent.width || (uint32_t)windowHeight != state.extent.height)) {

				state.swapchainRebuilds++;
				RecreateSwapchain ();

			}

		} else if (present != VK_SUCCESS) {

			std::string message = "vkQueuePresentKHR failed: ";
			message += ResultText (present);
			SetError (message.c_str ());
			return false;

		}

		// The CPU half of this frame's sample; the GPU half arrives when the slot is
		// reused two frames from now.
		PerfCloseFrame (state.frameIndex);

		state.waits.endTotal = (float)((PerfNow () - endTotalStart) * 1000.0);

		state.frameIndex = (state.frameIndex + 1) % MAX_FRAMES_IN_FLIGHT;
		state.framesPresented++;
		return true;

	}

	// A frame drawing a test pattern (the triangle, or the full screen gradient). The batch uses DrawBatchFrame.
	bool PresentFrame (const float clearColor[4], float timeSeconds) {

		State& state = S ();
		DeviceApi& dev = state.dev;

		if (!BeginFrame (clearColor)) return false;

		VkCommandBuffer commandBuffer = state.frameCommandBuffer;

		// The pattern paths draw directly instead of going through the batch, so
		// they have to open the window pass themselves.
		if (!EnsurePass ()) return false;

		if (state.pipeline != VK_NULL_HANDLE) {

			dev.CmdBindPipeline (commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, state.pipeline);

			VkDeviceSize offset = 0;
			dev.CmdBindVertexBuffers (commandBuffer, 0, 1, &state.vertexBuffer, &offset);

			PatternConstants constants = {};
			constants.time = timeSeconds;
			constants.rotate = PatternRotation (state.pattern);

			dev.CmdPushConstants (commandBuffer, state.pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof (constants), &constants);

			uint32_t first = 0;
			uint32_t count = 0;
			PatternRange (state.pattern, &first, &count);

			dev.CmdDraw (commandBuffer, count, 1, first, 0);

		}

		return EndFrame ();

	}

}
