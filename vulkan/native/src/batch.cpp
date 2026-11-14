#include "internal.h"

#include "quad_spirv.h"

#include <algorithm>

// 2D batches.
//
// Mirrors the four things the OpenFL renderer does: bindTexture, blendFunc,
// scissor and drawElements. The caller issues setters and quads in display list
// order, and a draw call is split only where the state really changes, so ten
// thousand sprites usually cost one to three submissions.
//
// There is one vertex buffer per frame slot (MAX_FRAMES_IN_FLIGHT of them): the
// data written for frame N may still be referenced by the commands of frame
// N-1, so a single shared buffer would mean waiting for the GPU every frame and
// giving up the overlap.

namespace vulkan {

	Batch::Batch () :
		vertexCapacity (0),
		indexBuffer (VK_NULL_HANDLE),
		indexMemory (VK_NULL_HANDLE),
		indexCapacity (0),
		descriptorLayout (VK_NULL_HANDLE),
		descriptorPool (VK_NULL_HANDLE),
		layout (VK_NULL_HANDLE),
		verticesWritten (0),
		batchFirstVertex (0),
		segmentQuads (0),
		segmentIndexed (true),
		overflowed (false),
		droppedVertices (0),
		drawCalls (0),
		frameVertices (0),
		currentTexture (0),
		currentFilter (FILTER_LINEAR),
		currentBlend (LIME_VULKAN_BLEND_NORMAL),
		scissorX (0),
		scissorY (0),
		scissorWidth (0),
		scissorHeight (0),
		hasView (false),
		droppedColorOffsets (0),
		recording (false),
		program (0) {

		for (int i = 0; i < 6; ++i) {

			view[i] = (i == 0 || i == 3) ? 1.0f : 0.0f;

		}


		for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {

			vertexBuffer[i] = VK_NULL_HANDLE;
			vertexMemory[i] = VK_NULL_HANDLE;
			vertexMapped[i] = 0;

		}

		for (int i = 0; i < BLEND_COUNT; ++i) pipelines[i] = VK_NULL_HANDLE;

		projection[0] = 0.0f;
		projection[1] = 0.0f;
		projection[2] = 0.0f;
		projection[3] = 0.0f;

	}

	// ---------------------------------------------------------------------
	// Buffers
	// ---------------------------------------------------------------------

	static bool CreateHostBuffer (VkDeviceSize size, VkBufferUsageFlags usage,
		VkBuffer* buffer, VkDeviceMemory* memory, void** mapped) {

		State& state = S ();
		DeviceApi& dev = state.dev;

		VkBufferCreateInfo bufferInfo = {};
		bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bufferInfo.size = size;
		bufferInfo.usage = usage;
		bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		if (dev.CreateBuffer (state.device, &bufferInfo, NULL, buffer) != VK_SUCCESS) {

			SetError ("vkCreateBuffer(batch) failed");
			return false;

		}

		VkMemoryRequirements requirements;
		dev.GetBufferMemoryRequirements (state.device, *buffer, &requirements);

		VkPhysicalDeviceMemoryProperties memoryProperties;
		state.api.GetPhysicalDeviceMemoryProperties (state.physicalDevice, &memoryProperties);

		uint32_t type = 0;
		bool found = false;
		const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

		for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {

			if ((requirements.memoryTypeBits & (1u << i)) && (memoryProperties.memoryTypes[i].propertyFlags & wanted) == wanted) {

				type = i;
				found = true;
				break;

			}

		}

		if (!found) {

			SetError ("no host visible memory type for the batch buffers");
			dev.DestroyBuffer (state.device, *buffer, NULL);
			*buffer = VK_NULL_HANDLE;
			return false;

		}

		VkMemoryAllocateInfo allocateInfo = {};
		allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocateInfo.allocationSize = requirements.size;
		allocateInfo.memoryTypeIndex = type;

		if (dev.AllocateMemory (state.device, &allocateInfo, NULL, memory) != VK_SUCCESS) {

			SetError ("vkAllocateMemory(batch) failed");
			dev.DestroyBuffer (state.device, *buffer, NULL);
			*buffer = VK_NULL_HANDLE;
			return false;

		}

		if (dev.BindBufferMemory (state.device, *buffer, *memory, 0) != VK_SUCCESS) {

			SetError ("vkBindBufferMemory(batch) failed");
			dev.FreeMemory (state.device, *memory, NULL);
			dev.DestroyBuffer (state.device, *buffer, NULL);
			*buffer = VK_NULL_HANDLE;
			*memory = VK_NULL_HANDLE;
			return false;

		}

		if (mapped) {

			if (dev.MapMemory (state.device, *memory, 0, VK_WHOLE_SIZE, 0, mapped) != VK_SUCCESS) {

				SetError ("vkMapMemory(batch) failed");
				return false;

			}

		}

		return true;

	}

	static void DestroyHostBuffer (VkBuffer* buffer, VkDeviceMemory* memory, void** mapped) {

		State& state = S ();
		DeviceApi& dev = state.dev;

		if (state.device == VK_NULL_HANDLE) {

			*buffer = VK_NULL_HANDLE;
			*memory = VK_NULL_HANDLE;
			if (mapped) *mapped = 0;
			return;

		}

		if (mapped && *mapped) {

			dev.UnmapMemory (state.device, *memory);
			*mapped = 0;

		}

		if (*buffer != VK_NULL_HANDLE) {

			dev.DestroyBuffer (state.device, *buffer, NULL);
			*buffer = VK_NULL_HANDLE;

		}

		if (*memory != VK_NULL_HANDLE) {

			dev.FreeMemory (state.device, *memory, NULL);
			*memory = VK_NULL_HANDLE;

		}

	}

	/** Grows the buffers on demand. Only valid at a frame boundary, when no recording command buffer references the old ones. */
	static bool AllocateBatchBuffers (uint32_t vertexCapacity) {

		State& state = S ();
		DeviceApi& dev = state.dev;
		Batch& batch = state.batch;

		DestroyHostBuffer (&batch.indexBuffer, &batch.indexMemory, 0);
		batch.indexCapacity = 0;

		for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {

			DestroyHostBuffer (&batch.vertexBuffer[i], &batch.vertexMemory[i], &batch.vertexMapped[i]);

		}

		batch.vertexCapacity = 0;

		VkDeviceSize vertexBytes = (VkDeviceSize)vertexCapacity * sizeof (lime_vulkan_vertex);

		for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {

			if (!CreateHostBuffer (vertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
				&batch.vertexBuffer[i], &batch.vertexMemory[i], &batch.vertexMapped[i])) {

				return false;

			}

		}

		// Index buffer: six indices per quad (0,1,2, 2,1,3), shared by every batch.
		uint32_t quadCapacity = vertexCapacity / 4;

		if (quadCapacity < 1) quadCapacity = 1;

		VkDeviceSize indexBytes = (VkDeviceSize)quadCapacity * 6 * sizeof (uint32_t);

		VkBuffer indexBuffer = VK_NULL_HANDLE;
		VkDeviceMemory indexMemory = VK_NULL_HANDLE;
		void* indexMapped = 0;

		if (!CreateHostBuffer (indexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &indexBuffer, &indexMemory, &indexMapped)) {

			return false;

		}

		uint32_t* indices = (uint32_t*)indexMapped;

		for (uint32_t quad = 0; quad < quadCapacity; ++quad) {

			uint32_t base = quad * 4;
			uint32_t* target = indices + quad * 6;

			target[0] = base + 0;
			target[1] = base + 1;
			target[2] = base + 2;
			target[3] = base + 2;
			target[4] = base + 1;
			target[5] = base + 3;

		}

		dev.UnmapMemory (state.device, indexMemory);

		batch.indexBuffer = indexBuffer;
		batch.indexMemory = indexMemory;
		batch.indexCapacity = quadCapacity;
		batch.vertexCapacity = vertexCapacity;

		return true;

	}

	// ---------------------------------------------------------------------
	// Pipelines: one per blend mode, built once and reused
	// ---------------------------------------------------------------------

	void FillBlendState (int blend, bool premultiplied, VkPipelineColorBlendAttachmentState* attachment) {

		attachment->colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
			VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		attachment->colorBlendOp = VK_BLEND_OP_ADD;
		attachment->alphaBlendOp = VK_BLEND_OP_ADD;

		switch (blend) {

			case LIME_VULKAN_BLEND_NONE:
				attachment->blendEnable = VK_FALSE;
				attachment->srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
				attachment->dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
				attachment->srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
				attachment->dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
				break;

			case LIME_VULKAN_BLEND_ADD:
				attachment->blendEnable = VK_TRUE;
				// A premultiplied fragment output must not be multiplied by SRC_ALPHA again.
				attachment->srcColorBlendFactor = premultiplied ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA;
				attachment->dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
				attachment->srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
				attachment->dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
				break;

			case LIME_VULKAN_BLEND_MULTIPLY:
				attachment->blendEnable = VK_TRUE;
				attachment->srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
				attachment->dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
				attachment->srcAlphaBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
				attachment->dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
				break;

			case LIME_VULKAN_BLEND_SCREEN:
				attachment->blendEnable = VK_TRUE;
				attachment->srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
				attachment->dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
				attachment->srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
				attachment->dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
				break;

			case LIME_VULKAN_BLEND_SUBTRACT:
				// dst - src, which is what Flash's subtract does. Clamped by the
				// hardware to 0 per channel.
				attachment->blendEnable = VK_TRUE;
				attachment->colorBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT;
				attachment->alphaBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT;
				attachment->srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
				attachment->dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
				attachment->srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
				attachment->dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
				break;

			case LIME_VULKAN_BLEND_DARKEN:
				// min(src, dst) per channel, straight from the blend unit: no shader
				// variant, no second draw.
				attachment->blendEnable = VK_TRUE;
				attachment->colorBlendOp = VK_BLEND_OP_MIN;
				attachment->alphaBlendOp = VK_BLEND_OP_MIN;
				attachment->srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
				attachment->dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
				attachment->srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
				attachment->dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
				break;

			case LIME_VULKAN_BLEND_LIGHTEN:
				attachment->blendEnable = VK_TRUE;
				attachment->colorBlendOp = VK_BLEND_OP_MAX;
				attachment->alphaBlendOp = VK_BLEND_OP_MAX;
				attachment->srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
				attachment->dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
				attachment->srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
				attachment->dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
				break;

			case LIME_VULKAN_BLEND_ERASE:
				attachment->blendEnable = VK_TRUE;
				attachment->srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
				attachment->dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
				attachment->srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
				attachment->dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
				break;

			default: // LIME_VULKAN_BLEND_NORMAL
				attachment->blendEnable = VK_TRUE;
				attachment->srcColorBlendFactor = premultiplied ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA;
				attachment->dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
				attachment->srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
				attachment->dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
				break;

		}

	}

	static bool CreateBatchPipeline (int blend, VkPipeline* pipeline) {

		State& state = S ();
		DeviceApi& dev = state.dev;
		Batch& batch = state.batch;

		VkShaderModuleCreateInfo moduleInfo = {};
		moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;

		VkShaderModule vertexModule = VK_NULL_HANDLE;
		VkShaderModule fragmentModule = VK_NULL_HANDLE;

		moduleInfo.codeSize = sizeof (kQuadVertexSpirv);
		moduleInfo.pCode = kQuadVertexSpirv;

		if (dev.CreateShaderModule (state.device, &moduleInfo, NULL, &vertexModule) != VK_SUCCESS) {

			SetError ("vkCreateShaderModule(quad.vert) failed");
			return false;

		}

		moduleInfo.codeSize = sizeof (kQuadFragmentSpirv);
		moduleInfo.pCode = kQuadFragmentSpirv;

		if (dev.CreateShaderModule (state.device, &moduleInfo, NULL, &fragmentModule) != VK_SUCCESS) {

			SetError ("vkCreateShaderModule(quad.frag) failed");
			dev.DestroyShaderModule (state.device, vertexModule, NULL);
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
		binding.stride = sizeof (lime_vulkan_vertex);
		binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

		VkVertexInputAttributeDescription attributes[4] = {};
		attributes[0].location = 0;
		attributes[0].binding = 0;
		attributes[0].format = VK_FORMAT_R32G32_SFLOAT;
		attributes[0].offset = 0;
		attributes[1].location = 1;
		attributes[1].binding = 0;
		attributes[1].format = VK_FORMAT_R32G32_SFLOAT;
		attributes[1].offset = 8;
		attributes[2].location = 2;
		attributes[2].binding = 0;
		attributes[2].format = VK_FORMAT_R8G8B8A8_UNORM;
		attributes[2].offset = 16;
		attributes[3].location = 3;
		attributes[3].binding = 0;
		attributes[3].format = VK_FORMAT_R8G8B8A8_SNORM;
		attributes[3].offset = 20;

		VkPipelineVertexInputStateCreateInfo vertexInput = {};
		vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
		vertexInput.vertexBindingDescriptionCount = 1;
		vertexInput.pVertexBindingDescriptions = &binding;
		vertexInput.vertexAttributeDescriptionCount = 4;
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
		// The fragment shader writes premultiplied alpha (see the note at the top of
		// quad.frag): NORMAL uses ONE / ONE_MINUS_SRC_ALPHA, and the other modes are
		// configured for premultiplied input as well.
		FillBlendState (blend, true, &blendAttachment);

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
		pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
		pushRange.offset = 0;
		pushRange.size = sizeof (float) * 4;

		VkPipelineLayoutCreateInfo layoutInfo = {};
		layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		layoutInfo.setLayoutCount = 1;
		layoutInfo.pSetLayouts = &batch.descriptorLayout;
		layoutInfo.pushConstantRangeCount = 1;
		layoutInfo.pPushConstantRanges = &pushRange;

		if (dev.CreatePipelineLayout (state.device, &layoutInfo, NULL, &batch.layout) != VK_SUCCESS) {

			SetError ("vkCreatePipelineLayout(batch) failed");
			dev.DestroyShaderModule (state.device, vertexModule, NULL);
			dev.DestroyShaderModule (state.device, fragmentModule, NULL);
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
		pipelineInfo.layout = batch.layout;
		pipelineInfo.renderPass = state.renderPass;
		pipelineInfo.subpass = 0;

		VkResult result = dev.CreateGraphicsPipelines (state.device, state.pipelineCache, 1, &pipelineInfo, NULL, pipeline);

		dev.DestroyShaderModule (state.device, vertexModule, NULL);
		dev.DestroyShaderModule (state.device, fragmentModule, NULL);

		if (result != VK_SUCCESS) {

			SetError ("vkCreateGraphicsPipelines(batch) failed");
			return false;

		}

		return true;

	}

	bool CreateBatchResources () {

		State& state = S ();
		DeviceApi& dev = state.dev;
		Batch& batch = state.batch;

		if (batch.vertexCapacity == 0) {

			uint32_t initial = MAX_QUADS_PER_FLUSH * 4;

			if (!AllocateBatchBuffers (initial)) return false;

		}

		if (batch.layout == VK_NULL_HANDLE) {

			for (int blend = 0; blend < BLEND_COUNT; ++blend) {

				// One pipeline per blend mode; the render pass layout is only built for the first.
				if (!CreateBatchPipeline (blend, &batch.pipelines[blend])) {

					for (int i = 0; i < BLEND_COUNT; ++i) {

						if (batch.pipelines[i] != VK_NULL_HANDLE) {

							dev.DestroyPipeline (state.device, batch.pipelines[i], NULL);
							batch.pipelines[i] = VK_NULL_HANDLE;

						}

					}

					if (batch.layout != VK_NULL_HANDLE) {

						dev.DestroyPipelineLayout (state.device, batch.layout, NULL);
						batch.layout = VK_NULL_HANDLE;

					}

					return false;

				}

			}

		}

		return true;

	}

	void DestroyBatchResources () {

		State& state = S ();
		DeviceApi& dev = state.dev;
		Batch& batch = state.batch;

		if (state.device != VK_NULL_HANDLE) {

			for (int i = 0; i < BLEND_COUNT; ++i) {

				if (batch.pipelines[i] != VK_NULL_HANDLE) {

					dev.DestroyPipeline (state.device, batch.pipelines[i], NULL);
					batch.pipelines[i] = VK_NULL_HANDLE;

				}

			}

			if (batch.layout != VK_NULL_HANDLE) {

				dev.DestroyPipelineLayout (state.device, batch.layout, NULL);
				batch.layout = VK_NULL_HANDLE;

			}

			for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {

				DestroyHostBuffer (&batch.vertexBuffer[i], &batch.vertexMemory[i], &batch.vertexMapped[i]);

			}

			DestroyHostBuffer (&batch.indexBuffer, &batch.indexMemory, 0);

		}

		batch.vertexCapacity = 0;
		batch.indexCapacity = 0;
		batch.verticesWritten = 0;
		batch.batchFirstVertex = 0;
		batch.segmentQuads = 0;
		batch.segmentIndexed = true;
		batch.recording = false;
		batch.overflowed = false;
		batch.droppedVertices = 0;

	}

	bool EnsureBatchCapacity (uint32_t vertexCount) {

		Batch& batch = S ().batch;

		if (vertexCount <= batch.vertexCapacity) return true;

		uint32_t target = batch.vertexCapacity ? batch.vertexCapacity : MAX_QUADS_PER_FLUSH * 4;

		while (target < vertexCount) target *= 2;

		State& state = S ();

		// Growing outside a frame boundary would leave a recording command buffer pointing at freed memory, so wait for the GPU here.
		if (state.device != VK_NULL_HANDLE && state.dev.DeviceWaitIdle) {

			state.dev.DeviceWaitIdle (state.device);

		}

		Log ("batch: growing vertex buffer to %u vertices (%u quads per flush)", target, target / 4);

		return AllocateBatchBuffers (target);

	}

	// ---------------------------------------------------------------------
	// Render passes: window <-> offscreen target
	// ---------------------------------------------------------------------

	void ActiveExtent (int* width, int* height) {

		State& state = S ();

		if (state.targetPassOpen && state.activeTarget) {

			if (width) *width = state.activeTarget->width;
			if (height) *height = state.activeTarget->height;
			return;

		}

		if (width) *width = state.frameWidth > 0 ? state.frameWidth : (int)state.extent.width;
		if (height) *height = state.frameHeight > 0 ? state.frameHeight : (int)state.extent.height;

	}

	/** Pixel space (origin top left, y down) to Vulkan NDC. The projection follows the current pass size. */
	static void SetProjection () {

		State& state = S ();

		int width = 0;
		int height = 0;

		ActiveExtent (&width, &height);

		if (width <= 0) width = 1;
		if (height <= 0) height = 1;

		state.batch.projection[0] = 2.0f / (float)width;
		state.batch.projection[1] = 2.0f / (float)height;
		state.batch.projection[2] = -1.0f;
		state.batch.projection[3] = -1.0f;

	}

	static void SetPassViewport (VkCommandBuffer commandBuffer, int width, int height) {

		State& state = S ();

		if (commandBuffer == VK_NULL_HANDLE) return;

		if (width <= 0) width = 1;
		if (height <= 0) height = 1;

		VkViewport viewport = {};
		viewport.width = (float)width;
		viewport.height = (float)height;
		viewport.minDepth = 0.0f;
		viewport.maxDepth = 1.0f;
		state.dev.CmdSetViewport (commandBuffer, 0, 1, &viewport);

		VkRect2D scissor = {};
		scissor.extent.width = (uint32_t)width;
		scissor.extent.height = (uint32_t)height;
		state.dev.CmdSetScissor (commandBuffer, 0, 1, &scissor);

	}

	bool BeginSurfacePass (bool clear) {

		State& state = S ();
		DeviceApi& dev = state.dev;

		if (!state.inFrame || state.surfacePassOpen) return false;

		if (state.currentImageIndex >= state.framebuffers.size ()) return false;

		VkCommandBuffer commandBuffer = state.frameCommandBuffer;

		VkClearValue clearValue = {};
		memcpy (clearValue.color.float32, state.clearColor, sizeof (state.clearColor));

		VkRenderPassBeginInfo info = {};
		info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		info.renderPass = clear ? state.renderPass : state.surfacePassLoad;
		info.framebuffer = state.framebuffers[state.currentImageIndex];
		info.renderArea.offset.x = 0;
		info.renderArea.offset.y = 0;
		info.renderArea.extent = state.extent;
		info.clearValueCount = 1;
		info.pClearValues = &clearValue;

		dev.CmdBeginRenderPass (commandBuffer, &info, VK_SUBPASS_CONTENTS_INLINE);

		SetPassViewport (commandBuffer, (int)state.extent.width, (int)state.extent.height);

		state.surfacePassOpen = true;
		state.surfacePassCleared = state.surfacePassCleared || clear;

		SetProjection ();

		return true;

	}

	void EndSurfacePass () {

		State& state = S ();

		if (!state.surfacePassOpen) return;

		state.dev.CmdEndRenderPass (state.frameCommandBuffer);
		state.surfacePassOpen = false;

	}

	bool BeginTargetPass (Texture* target, unsigned int argb) {

		State& state = S ();
		DeviceApi& dev = state.dev;

		if (!state.inFrame || target == NULL) return false;
		if (target->framebuffer == VK_NULL_HANDLE) return false;

		VkCommandBuffer commandBuffer = state.frameCommandBuffer;

		// The image was left as a shader read after the last filter pass; drawing
		// into it again needs the write layout back.
		VkImageMemoryBarrier barrier = {};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.oldLayout = target->layout;
		barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = target->image;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.baseMipLevel = 0;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.baseArrayLayer = 0;
		barrier.subresourceRange.layerCount = 1;
		barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

		dev.CmdPipelineBarrier (commandBuffer,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			0, 0, NULL, 0, NULL, 1, &barrier);

		// The layer's own background: flixel paints the camera bgColor over the
		// whole camera before anything else, so the filters see it too. Premultiplied
		// here, because that is the blend semantic the whole pipeline uses.
		VkClearValue clearValue = {};
		clearValue.color.float32[0] = (float)((argb >> 16) & 0xFF) / 255.0f;
		clearValue.color.float32[1] = (float)((argb >> 8) & 0xFF) / 255.0f;
		clearValue.color.float32[2] = (float)(argb & 0xFF) / 255.0f;
		clearValue.color.float32[3] = (float)((argb >> 24) & 0xFF) / 255.0f;

		VkRenderPassBeginInfo info = {};
		info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		info.renderPass = state.targetPass;
		info.framebuffer = target->framebuffer;
		info.renderArea.offset.x = 0;
		info.renderArea.offset.y = 0;
		info.renderArea.extent.width = (uint32_t)target->width;
		info.renderArea.extent.height = (uint32_t)target->height;
		info.clearValueCount = 1;
		info.pClearValues = &clearValue;

		dev.CmdBeginRenderPass (commandBuffer, &info, VK_SUBPASS_CONTENTS_INLINE);

		SetPassViewport (commandBuffer, target->width, target->height);

		state.targetPassOpen = true;
		state.activeTarget = target;

		SetProjection ();

		return true;

	}

	void EndTargetPass () {

		State& state = S ();

		if (!state.targetPassOpen) return;

		state.dev.CmdEndRenderPass (state.frameCommandBuffer);

		// The pass ended with SHADER_READ_ONLY as its final layout, which is what
		// the descriptor sets of every texture already declare.
		if (state.activeTarget) state.activeTarget->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		state.targetPassOpen = false;
		state.activeTarget = NULL;

	}

	void EndActivePass () {

		EndTargetPass ();
		EndSurfacePass ();

	}

	bool EnsurePass () {

		State& state = S ();

		if (state.surfacePassOpen || state.targetPassOpen) return true;

		// The window pass is reopened with LOAD so what was already drawn survives
		// the offscreen stretch; the very first one of the frame clears instead.
		return BeginSurfacePass (!state.surfacePassCleared);

	}

	// ---------------------------------------------------------------------
	// Submission
	// ---------------------------------------------------------------------

	static VkRect2D CurrentScissor () {

		State& state = S ();
		Batch& batch = state.batch;

		// The batch scissor is clamped to whatever pass is open, not to the window:
		// a filtered camera draws into a target that is usually smaller.
		int activeWidth = 0;
		int activeHeight = 0;
		ActiveExtent (&activeWidth, &activeHeight);

		if (activeWidth <= 0) activeWidth = 1;
		if (activeHeight <= 0) activeHeight = 1;

		VkRect2D scissor = {};
		scissor.extent.width = (uint32_t)activeWidth;
		scissor.extent.height = (uint32_t)activeHeight;

		if (batch.scissorWidth > 0 && batch.scissorHeight > 0) {

			scissor.offset.x = batch.scissorX > 0 ? batch.scissorX : 0;
			scissor.offset.y = batch.scissorY > 0 ? batch.scissorY : 0;
			scissor.extent.width = (uint32_t)batch.scissorWidth;
			scissor.extent.height = (uint32_t)batch.scissorHeight;

		}

		// Vulkan requires the scissor to lie inside the render area; anything else is a validation error.
		if (scissor.offset.x > (int32_t)activeWidth) scissor.offset.x = (int32_t)activeWidth;
		if (scissor.offset.y > (int32_t)activeHeight) scissor.offset.y = (int32_t)activeHeight;

		if ((uint32_t)scissor.offset.x + scissor.extent.width > (uint32_t)activeWidth) {

			scissor.extent.width = (uint32_t)activeWidth - (uint32_t)scissor.offset.x;

		}

		if ((uint32_t)scissor.offset.y + scissor.extent.height > (uint32_t)activeHeight) {

			scissor.extent.height = (uint32_t)activeHeight - (uint32_t)scissor.offset.y;

		}

		if (scissor.extent.width == 0) scissor.extent.width = 1;
		if (scissor.extent.height == 0) scissor.extent.height = 1;

		return scissor;

	}

	void FlushBatch () {

		State& state = S ();
		Batch& batch = state.batch;

		if (!batch.recording) return;

		uint32_t vertexCount = batch.verticesWritten - batch.batchFirstVertex;

		if (vertexCount == 0) return;

		// The pass is opened by the first draw of a stretch, not at frame start:
		// the batch may have gone offscreen since the previous draw.
		if (!EnsurePass ()) return;

		VkCommandBuffer commandBuffer = state.frameCommandBuffer;

		Texture* texture = FindTexture (batch.currentTexture);

		if (!texture) {

			// The handle the caller passed is not a live texture any more: a retired
			// one or a zero from an upload that failed. Drawn with the white pixel so
			// the segment does not vanish, but counted, because a white box on screen
			// has to be traceable to this line.
			batch.fallbackTextures++;

			texture = FindTexture (state.whiteTexture);

		}

		if (!texture) {

			batch.droppedSegments++;
			return;

		}

		int blend = batch.currentBlend;

		if (blend < 0 || blend >= BLEND_COUNT) blend = LIME_VULKAN_BLEND_NORMAL;

		DeviceApi& dev = state.dev;

		// Draw with the custom program when there is one, and fall back to the
		// built-in pipeline when it could not be built (descriptor pool full, zero
		// handle). The result may look wrong, but the batch does not vanish; the same
		// fallback rule as everywhere else applies here.
		bool useBuiltIn = true;
		ShaderProgram* program = FindProgram (batch.program);

		if (program) {

			VkRect2D programScissor = CurrentScissor ();
			dev.CmdSetScissor (commandBuffer, 0, 1, &programScissor);
			batch.bindScissors++;

			if (PrepareProgramDraw (program, texture, blend, commandBuffer, NULL)) {

				useBuiltIn = false;

			}

		}

		if (useBuiltIn) {

			if (batch.pipelines[blend] == VK_NULL_HANDLE) {

				batch.droppedSegments++;
				return;

			}

			dev.CmdBindPipeline (commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, batch.pipelines[blend]);
			batch.bindPipelines++;

			dev.CmdBindDescriptorSets (commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, batch.layout,
				0, 1, &texture->descriptors[batch.currentFilter], 0, NULL);
			batch.bindTextures++;

			VkRect2D scissor = CurrentScissor ();
			dev.CmdSetScissor (commandBuffer, 0, 1, &scissor);
			batch.bindScissors++;

			dev.CmdPushConstants (commandBuffer, batch.layout, VK_SHADER_STAGE_VERTEX_BIT,
				0, sizeof (float) * 4, batch.projection);

		}

		VkDeviceSize offset = 0;
		dev.CmdBindVertexBuffers (commandBuffer, 0, 1, &batch.vertexBuffer[state.frameIndex], &offset);

		if (batch.segmentIndexed && batch.segmentQuads > 0) {

			dev.CmdBindIndexBuffer (commandBuffer, batch.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
			dev.CmdDrawIndexed (commandBuffer, batch.segmentQuads * 6, 1, 0, batch.batchFirstVertex, 0);

		} else {

			dev.CmdDraw (commandBuffer, vertexCount, 1, batch.batchFirstVertex, 0);

		}

		if (batch.drawRecordCount < 12) {

			Batch::DrawRecord& record = batch.draws[batch.drawRecordCount++];
			record.firstVertex = batch.batchFirstVertex;
			record.quads = batch.segmentIndexed ? batch.segmentQuads : (vertexCount / 4u);
			record.scissorX = batch.scissorX;
			record.scissorY = batch.scissorY;
			record.scissorW = batch.scissorWidth;
			record.scissorH = batch.scissorHeight;
			record.texture = batch.currentTexture;

			const lime_vulkan_vertex* verts = (const lime_vulkan_vertex*)batch.vertexMapped[state.frameIndex] + batch.batchFirstVertex;

			for (int k = 0; k < 4 && k < (int)vertexCount; ++k) {

				record.vx[k] = verts[k].x;
				record.vy[k] = verts[k].y;
				record.vu[k] = verts[k].u;
				record.vv[k] = verts[k].v;
				record.vc[k] = verts[k].color;

			}

		}

		batch.drawCalls++;
		batch.drawnVertices += vertexCount;
		batch.batchFirstVertex = batch.verticesWritten;
		batch.segmentQuads = 0;

	}

	bool DrawBatchFrame () {

		State& state = S ();
		Batch& batch = state.batch;

		FlushBatch ();

		if (!EndFrame ()) {

			batch.recording = false;
			return false;

		}

		batch.recording = false;
		return true;

	}

}
