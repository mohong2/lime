#include "internal.h"

#include "triangle_spirv.h"

#include <algorithm>

namespace vulkan {

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

		VkResult result = dev.CreateGraphicsPipelines (state.device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &state.pipeline);

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

		VkExtent2D extent = capabilities.currentExtent;

		if (extent.width == 0xFFFFFFFFu) {

			extent.width = (uint32_t)std::max (1, std::min ((int)capabilities.maxImageExtent.width, width));
			extent.height = (uint32_t)std::max (1, std::min ((int)capabilities.maxImageExtent.height, height));

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
		createInfo.preTransform = capabilities.currentTransform;
		createInfo.compositeAlpha = compositeAlpha;
		createInfo.presentMode = presentMode;
		createInfo.clipped = VK_TRUE;

		uint32_t families[2] = { state.graphicsFamily, state.presentFamily };

		if (state.graphicsFamily != state.presentFamily) {

			createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
			createInfo.queueFamilyIndexCount = 2;
			createInfo.pQueueFamilyIndices = families;

		}

		VkResult result = dev.CreateSwapchainKHR (state.device, &createInfo, NULL, &state.swapchain);

		if (result != VK_SUCCESS) {

			std::string message = "vkCreateSwapchainKHR failed: ";
			message += ResultText (result);
			SetError (message.c_str ());
			return false;

		}

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

		if (!CreateRenderTargets ()) return false;
		if (!CreateSyncObjects ()) return false;

		DestroyTrianglePipeline ();
		if (!CreateTrianglePipeline ()) return false;

		DestroyBatchResources ();
		if (!CreateBatchResources ()) return false;

		Log ("swapchain %ux%u, %u image(s), %s", extent.width, extent.height, state.imageCount, PresentModeText (presentMode));
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

		if (!CreateInstance (state.options.enableValidation != 0, state.options.verbose != 0, &state.instance)) {

			DetachDevice ();
			return false;

		}

		if (!LoadInstanceApi (state.instance)) {

			DetachDevice ();
			return false;

		}

		if (!CreatePlatformSurface (state.instance, window, &state.surface)) {

			DetachDevice ();
			return false;

		}

		uint32_t graphicsFamily = 0;
		uint32_t presentFamily = 0;

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

		VkResult result = state.api.CreateDevice (state.physicalDevice, &deviceInfo, NULL, &state.device);

		if (result != VK_SUCCESS) {

			std::string message = "vkCreateDevice failed: ";
			message += ResultText (result);
			SetError (message.c_str ());
			DetachDevice ();
			return false;

		}

		if (!LoadDeviceApi (state.device)) {

			DetachDevice ();
			return false;

		}

		state.dev.GetDeviceQueue (state.device, graphicsFamily, 0, &state.graphicsQueue);
		state.dev.GetDeviceQueue (state.device, presentFamily, 0, &state.presentQueue);

		// Descriptor layout, pool and the 1x1 white texture are independent of the swapchain and follow the device.
		if (!CreateProgramResources ()) {

			return false;

		}

		if (!CreateTextureResources ()) {

			DetachDevice ();
			return false;

		}

		if (withSwapchain && !CreateSwapchain (width, height)) {

			DetachDevice ();
			return false;

		}

		state.attached = withSwapchain;
		state.framesPresented = 0;
		state.lastError.clear ();
		return true;

	}

	void DetachDevice () {

		State& state = S ();

		if (state.device != VK_NULL_HANDLE) {

			if (state.dev.DeviceWaitIdle) state.dev.DeviceWaitIdle (state.device);

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

	// ---------------------------------------------------------------------

	// A frame is three steps (BeginFrame, draws, EndFrame); PresentFrame is only the
	// 'draw a test pattern' use. Split so the batch inserts any number of draws in
	// the middle while the wait, acquire and submit sequence stays in one place.
	bool BeginFrame (const float clearColor[4]) {

		State& state = S ();
		DeviceApi& dev = state.dev;

		if (state.inFrame) {

			SetError ("BeginFrame called twice without EndFrame");
			return false;

		}

		if (!state.attached || state.swapchain == VK_NULL_HANDLE) {

			SetError ("present without an attached swapchain");
			return false;

		}

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

		VkFence fence = state.inFlight[state.frameIndex];
		VkResult fenceResult = dev.WaitForFences (state.device, 1, &fence, VK_TRUE, UINT64_MAX);

		if (fenceResult != VK_SUCCESS) {

			// Always include the result code: device loss and timeout are completely
			// different things with identical symptoms (a frame that hangs). This line
			// is how the first game run was diagnosed.
			std::string message = "vkWaitForFences failed: ";
			message += ResultText (fenceResult);
			SetError (message.c_str ());
			return false;

		}

		// The fence of this frame slot has passed, so the commands that used it last
		// have finished: only now is it safe to recycle its descriptor sets and the
		// uniform ring buffer.
		ResetProgramFrameState ();

		// Textures retired long enough are really freed here. Releasing them anywhere
		// else is unsafe, since a descriptor set may still be referenced by a
		// submitted command and that costs the device.
		DrainRetiredTextures ();

		uint32_t imageIndex = 0;
		VkResult acquire = dev.AcquireNextImageKHR (state.device, state.swapchain, UINT64_MAX,
			state.imageAvailable[state.frameIndex], VK_NULL_HANDLE, &imageIndex);

		if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {

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

		return true;

	}

	// Closing: end the render pass, submit, present. Must pair with BeginFrame.
	bool EndFrame () {

		State& state = S ();
		DeviceApi& dev = state.dev;

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
		EndActivePass ();

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

		dev.CmdPipelineBarrier (commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &presentBarrier);

		state.inFrame = false;

		if (dev.EndCommandBuffer (commandBuffer) != VK_SUCCESS) {

			SetError ("vkEndCommandBuffer failed");
			return false;

		}

		// Everything queued by texture creation or update is recorded into one
		// command buffer here. The frame waits for it on the GPU instead of the CPU
		// waiting for the copy, so a frame that uploads still submits immediately.
		VkSemaphore uploadSemaphore = FlushTextureUploads ();

		VkSemaphore waitSemaphores[2];
		VkPipelineStageFlags waitStages[2];
		uint32_t waitCount = 0;

		waitSemaphores[waitCount] = state.imageAvailable[state.frameIndex];
		waitStages[waitCount] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		waitCount++;

		if (uploadSemaphore != VK_NULL_HANDLE) {

			waitSemaphores[waitCount] = uploadSemaphore;
			waitStages[waitCount] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
			waitCount++;

		}

		VkSubmitInfo submitInfo = {};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.waitSemaphoreCount = waitCount;
		submitInfo.pWaitSemaphores = waitSemaphores;
		submitInfo.pWaitDstStageMask = waitStages;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &commandBuffer;
		submitInfo.signalSemaphoreCount = 1;
		submitInfo.pSignalSemaphores = &state.renderFinished[imageIndex];

		VkResult submitResult = dev.QueueSubmit (state.graphicsQueue, 1, &submitInfo, fence);

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

		VkResult present = dev.QueuePresentKHR (state.presentQueue, &presentInfo);

		if (present == VK_ERROR_OUT_OF_DATE_KHR || present == VK_SUBOPTIMAL_KHR) {

			RecreateSwapchain ();

		} else if (present != VK_SUCCESS) {

			std::string message = "vkQueuePresentKHR failed: ";
			message += ResultText (present);
			SetError (message.c_str ());
			return false;

		}

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
