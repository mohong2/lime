#include "internal.h"

#include "compile.h"

#include <cstring>

// Shader programs: turns GLSL ES 1.00 into a Vulkan pipeline at runtime.
//
// One program is six pipelines (one per blend mode) plus a descriptor layout and
// a uniform shadow. Six because blend state is pipeline state and Vulkan 1.0
// cannot change it dynamically; building them costs a few milliseconds once,
// which beats rebuilding a pipeline per frame whenever the blend mode changes.
//
// Uniform data goes through a ring buffer with a dynamic offset: the same program
// can be used by several draws in one frame with different values (flixel's
// palette is one copy per sprite), and a single block would let the next draw
// overwrite the previous one.

namespace vulkan {

	UniformRing::UniformRing () :
		capacity (0),
		offset (0) {

		for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {

			buffer[i] = VK_NULL_HANDLE;
			memory[i] = VK_NULL_HANDLE;
			mapped[i] = 0;

		}

	}

	ShaderProgram::ShaderProgram () :
		handle (0),
		layout (VK_NULL_HANDLE),
		descriptorLayout (VK_NULL_HANDLE),
		openfl (true),
		premultiplied (true),
		uniformSize (0) {

		for (int i = 0; i < BLEND_COUNT; ++i) pipelines[i] = VK_NULL_HANDLE;
		for (int i = 0; i < 16; ++i) samplerTextures[i] = 0;

	}

	// ---------------------------------------------------------------------
	// Infrastructure: the uniform ring buffer and the per-frame descriptor pool
	// ---------------------------------------------------------------------

	namespace {

		const uint32_t kUniformRingInitial = 64 * 1024;
		const uint32_t kUniformAlignment = 256; /**< upper bound of minUniformBufferOffsetAlignment */
		const uint32_t kDescriptorsPerFrame = 2048;
		const uint32_t kSamplersPerDescriptor = 8;

		void DestroyRing (UniformRing* ring) {

			State& state = S ();

			if (state.device == VK_NULL_HANDLE) {

				ring->capacity = 0;
				ring->offset = 0;
				return;

			}

			for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {

				if (ring->mapped[i]) {

					state.dev.UnmapMemory (state.device, ring->memory[i]);
					ring->mapped[i] = 0;

				}

				if (ring->buffer[i] != VK_NULL_HANDLE) {

					state.dev.DestroyBuffer (state.device, ring->buffer[i], NULL);
					ring->buffer[i] = VK_NULL_HANDLE;

				}

				if (ring->memory[i] != VK_NULL_HANDLE) {

					state.dev.FreeMemory (state.device, ring->memory[i], NULL);
					ring->memory[i] = VK_NULL_HANDLE;

				}

			}

			ring->capacity = 0;
			ring->offset = 0;

		}

		bool CreateRing (uint32_t capacity) {

			State& state = S ();
			DeviceApi& dev = state.dev;

			DestroyRing (&state.uniformRing);

			VkBufferCreateInfo bufferInfo = {};
			bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
			bufferInfo.size = capacity;
			bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
			bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

			for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {

				if (dev.CreateBuffer (state.device, &bufferInfo, NULL, &state.uniformRing.buffer[i]) != VK_SUCCESS) {

					SetError ("vkCreateBuffer(uniform ring) failed");
					DestroyRing (&state.uniformRing);
					return false;

				}

				VkMemoryRequirements requirements;
				dev.GetBufferMemoryRequirements (state.device, state.uniformRing.buffer[i], &requirements);

				VkPhysicalDeviceMemoryProperties memoryProperties;
				state.api.GetPhysicalDeviceMemoryProperties (state.physicalDevice, &memoryProperties);

				const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

				uint32_t type = 0;
				bool found = false;

				for (uint32_t t = 0; t < memoryProperties.memoryTypeCount; ++t) {

					if ((requirements.memoryTypeBits & (1u << t)) && (memoryProperties.memoryTypes[t].propertyFlags & wanted) == wanted) {

						type = t;
						found = true;
						break;

					}

				}

				if (!found) {

					SetError ("no host visible memory type for the uniform ring");
					DestroyRing (&state.uniformRing);
					return false;

				}

				VkMemoryAllocateInfo allocateInfo = {};
				allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
				allocateInfo.allocationSize = requirements.size;
				allocateInfo.memoryTypeIndex = type;

				if (dev.AllocateMemory (state.device, &allocateInfo, NULL, &state.uniformRing.memory[i]) != VK_SUCCESS) {

					SetError ("vkAllocateMemory(uniform ring) failed");
					DestroyRing (&state.uniformRing);
					return false;

				}

				if (dev.BindBufferMemory (state.device, state.uniformRing.buffer[i], state.uniformRing.memory[i], 0) != VK_SUCCESS) {

					SetError ("vkBindBufferMemory(uniform ring) failed");
					DestroyRing (&state.uniformRing);
					return false;

				}

				if (dev.MapMemory (state.device, state.uniformRing.memory[i], 0, VK_WHOLE_SIZE, 0, &state.uniformRing.mapped[i]) != VK_SUCCESS) {

					SetError ("vkMapMemory(uniform ring) failed");
					DestroyRing (&state.uniformRing);
					return false;

				}

			}

			state.uniformRing.capacity = capacity;
			state.uniformRing.offset = 0;

			return true;

		}

	}

	bool CreateProgramResources () {

		State& state = S ();

		if (!CreateRing (kUniformRingInitial)) return false;

		VkDescriptorPoolSize sizes[2];
		sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		sizes[0].descriptorCount = kDescriptorsPerFrame;
		sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		sizes[1].descriptorCount = kDescriptorsPerFrame * kSamplersPerDescriptor;

		VkDescriptorPoolCreateInfo poolInfo = {};
		poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		poolInfo.maxSets = kDescriptorsPerFrame;
		poolInfo.poolSizeCount = 2;
		poolInfo.pPoolSizes = sizes;

		for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {

			if (state.frameDescriptorPool[i] != VK_NULL_HANDLE) continue;

			if (state.dev.CreateDescriptorPool (state.device, &poolInfo, NULL, &state.frameDescriptorPool[i]) != VK_SUCCESS) {

				SetError ("vkCreateDescriptorPool(frames) failed");
				return false;

			}

		}

		return true;

	}

	void DestroyProgramResources () {

		State& state = S ();

		DestroyAllShaderPrograms ();

		if (state.device != VK_NULL_HANDLE) {

			for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {

				if (state.frameDescriptorPool[i] != VK_NULL_HANDLE) {

					state.dev.DestroyDescriptorPool (state.device, state.frameDescriptorPool[i], NULL);
					state.frameDescriptorPool[i] = VK_NULL_HANDLE;

				}

			}

		}

		DestroyRing (&state.uniformRing);

	}

	/** Carves size bytes out of this frame's ring buffer and returns the offset. */
	bool AllocateUniformBlock (uint32_t size, uint32_t* offset) {

		State& state = S ();

		uint32_t stride = (size + kUniformAlignment - 1) / kUniformAlignment * kUniformAlignment;

		if (stride < kUniformAlignment) stride = kUniformAlignment;

		if (state.uniformRing.capacity == 0) {

			if (!CreateRing (kUniformRingInitial)) return false;

		}

		if (state.uniformRing.offset + stride > state.uniformRing.capacity) {

			// More than 64 KB of uniforms in one frame: wait for the GPU to go idle
			// and double the buffer. Growing mid frame has to wait, otherwise it
			// steps on command buffers that are still executing.
			uint32_t capacity = state.uniformRing.capacity;

			while (capacity < state.uniformRing.offset + stride) capacity *= 2;

			if (state.dev.DeviceWaitIdle) state.dev.DeviceWaitIdle (state.device);

			Log ("uniform ring: growing to %u bytes", capacity);

			if (!CreateRing (capacity)) return false;

		}

		*offset = state.uniformRing.offset;
		state.uniformRing.offset += stride;

		return true;

	}

	// ---------------------------------------------------------------------
	// Pipeline creation
	// ---------------------------------------------------------------------

	std::string gLastProgramFailure;

	const char* LastProgramFailure () {

		return gLastProgramFailure.c_str();

	}

	namespace {

		ShaderProgram* Find (uint64_t handle) {

			State& state = S ();
			std::map<uint64_t, ShaderProgram*>::iterator found = state.programs.find (handle);

			if (found == state.programs.end()) return NULL;

			return found->second;

		}

		bool CreateProgramPipeline (ShaderProgram* program, int blend, VkShaderModule vertexModule, VkShaderModule fragmentModule) {

			State& state = S ();
			DeviceApi& dev = state.dev;

			VkPipelineShaderStageCreateInfo stages[2] = {};

			stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
			stages[0].module = vertexModule;
			stages[0].pName = "main";

			stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
			stages[1].module = fragmentModule;
			stages[1].pName = "main";

			// A single 24 byte vertex stream: pos / uv / color / colorOffset. Identical
			// to the built-in quad pipeline, so one vertex buffer feeds both.
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

			// Additive colour offset from colorTransform, signed bytes. Declared for
			// every program because the layout is shared; a shader that ignores it
			// simply does not read the attribute.
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
			FillBlendState (blend, program->premultiplied, &blendAttachment);

			VkPipelineColorBlendStateCreateInfo colorBlend = {};
			colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
			colorBlend.attachmentCount = 1;
			colorBlend.pAttachments = &blendAttachment;

			VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };

			VkPipelineDynamicStateCreateInfo dynamicState = {};
			dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
			dynamicState.dynamicStateCount = 2;
			dynamicState.pDynamicStates = dynamicStates;

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
			pipelineInfo.layout = program->layout;
			pipelineInfo.renderPass = state.renderPass;
			pipelineInfo.subpass = 0;

			if (dev.CreateGraphicsPipelines (state.device, state.pipelineCache, 1, &pipelineInfo, NULL, &program->pipelines[blend]) != VK_SUCCESS) {

				program->pipelines[blend] = VK_NULL_HANDLE;
				return false;

			}

			return true;

		}

		/** Default uniform values for the compatibility layer: they put flixel's
		 *  flixel_texture2D on the 'has a colour transform' path. With hasTransform=0
		 *  the texture colour goes through untouched and every vertex colour (fades,
		 *  tints) is lost. */
		void ApplyDefaultUniforms (ShaderProgram* program) {

			for (size_t i = 0; i < program->uniforms.size(); ++i) {

				const glsl::Variable& variable = program->uniforms[i];

				if (variable.type != glsl::TYPE_BOOL) continue;
				if (variable.size != 4) continue;

				int value = 0;

				if (variable.name == "hasTransform" || variable.name == "hasColorTransform") value = 1;

				memcpy (&program->shadow[variable.offset], &value, 4);

			}

		}

		bool ResolveSamplers (ShaderProgram* program, const glsl::Translated& translated) {

			for (size_t i = 0; i < translated.samplers.size(); ++i) {

				program->samplerIndex[translated.samplers[i].name] = (int)i;

			}

			return true;

		}

	}

	void* FindProgramUniform (ShaderProgram* program, const char* name, int expectedType) {

		if (!program || !name) return NULL;

		std::map<std::string, int>::iterator found = program->uniformIndex.find (name);

		if (found == program->uniformIndex.end()) return NULL;

		glsl::Variable& variable = program->uniforms[(size_t)found->second];

		if (expectedType >= 0 && variable.type != expectedType) return NULL;

		if ((size_t)variable.offset + variable.size > program->shadow.size()) return NULL;

		return &program->shadow[variable.offset];

	}

	ShaderProgram* FindProgram (uint64_t handle) {

		return Find (handle);

	}

	// ---------------------------------------------------------------------
	// Creation and destruction
	// ---------------------------------------------------------------------

	namespace {

		void DiscardProgram (ShaderProgram* program) {

			State& state = S ();
			DeviceApi& dev = state.dev;

			if (state.device != VK_NULL_HANDLE) {

				for (int i = 0; i < BLEND_COUNT; ++i) {

					if (program->pipelines[i] != VK_NULL_HANDLE) {

						dev.DestroyPipeline (state.device, program->pipelines[i], NULL);

					}

				}

				if (program->layout != VK_NULL_HANDLE) {

					dev.DestroyPipelineLayout (state.device, program->layout, NULL);

				}

				if (program->descriptorLayout != VK_NULL_HANDLE) {

					dev.DestroyDescriptorSetLayout (state.device, program->descriptorLayout, NULL);

				}

			}

			delete program;

		}

	}

	uint64_t CreateShaderProgram (const char* vertexSource, const char* fragmentSource,
		unsigned int flags, const char* cacheDirectory) {

		State& state = S ();
		DeviceApi& dev = state.dev;

		if (state.device == VK_NULL_HANDLE) {

			SetError ("lime_vulkan_program_create: no Vulkan device");
			return 0;

		}

		glsl::Options translateOptions;
		translateOptions.openfl = (flags & LIME_VULKAN_PROGRAM_PLAIN) == 0;

		glsl::Translated translated;

		if (!glsl::Translate (vertexSource, (int)strlen (vertexSource),
			fragmentSource, (int)strlen (fragmentSource), translateOptions, &translated)) {

			SetError ("lime_vulkan_program_create: the shader could not be translated");
			gLastProgramFailure = "translation failed:\n";
			gLastProgramFailure += translated.log;

			Log ("program: translation failed: %s", translated.log.c_str());
			return 0;

		}

		shader::Options compileOptions;
		compileOptions.cacheDir = cacheDirectory;
		compileOptions.allowInProcess = true;
		compileOptions.allowProcess = true;
		compileOptions.verbose = false;

		shader::Result compiled;

		if (!shader::Compile (translated.vertex.c_str(), translated.fragment.c_str(), compileOptions, &compiled)) {

			// Keep the translated GLSL when compilation fails: the line numbers in the
			// error refer to the translated source, and without it there is no way to
			// find the original line. Only the failure path pays for this.
			SetError ("lime_vulkan_program_create: the shader could not be compiled");
			gLastProgramFailure = "compile failed (backend=";
			gLastProgramFailure += compiled.backend;
			gLastProgramFailure += "):\n";
			gLastProgramFailure += compiled.log;
			gLastProgramFailure += "\n----- translated vertex -----\n";
			gLastProgramFailure += translated.vertex;
			gLastProgramFailure += "\n----- translated fragment -----\n";
			gLastProgramFailure += translated.fragment;

			Log ("program: compile failed: %s", compiled.log.c_str());
			return 0;

		}

		LARGE_INTEGER startCounter;
		QueryPerformanceCounter (&startCounter);

		ShaderProgram* program = new ShaderProgram ();

		program->openfl = translateOptions.openfl;
		program->premultiplied = translateOptions.openfl && (flags & LIME_VULKAN_PROGRAM_STRAIGHT_ALPHA) == 0;
		program->uniformSize = translated.uniformSize;
		program->uniforms = translated.uniforms;
		program->samplers = translated.samplers;
		program->shadow.assign (translated.uniformSize, 0);

		for (size_t i = 0; i < program->uniforms.size(); ++i) {

			program->uniformIndex[program->uniforms[i].name] = (int)i;

		}

		ApplyDefaultUniforms (program);
		ResolveSamplers (program, translated);

		program->log = translated.log;
		program->log += "backend: ";
		program->log += compiled.backend;
		program->log += "\n";
		program->log += glsl::Describe (translated);

		// ---- Descriptor layout: binding 0 is the dynamic uniform buffer, the rest are samplers ----

		VkDescriptorSetLayoutBinding bindings[17] = {};

		bindings[0].binding = 0;
		bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		bindings[0].descriptorCount = 1;
		bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

		uint32_t bindingCount = 1;

		for (size_t i = 0; i < program->samplers.size() && i < 16; ++i) {

			bindings[bindingCount].binding = (uint32_t)program->samplers[i].location;
			bindings[bindingCount].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			bindings[bindingCount].descriptorCount = 1;
			bindings[bindingCount].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
			++bindingCount;

		}

		VkDescriptorSetLayoutCreateInfo setLayoutInfo = {};
		setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		setLayoutInfo.bindingCount = bindingCount;
		setLayoutInfo.pBindings = bindings;

		if (dev.CreateDescriptorSetLayout (state.device, &setLayoutInfo, NULL, &program->descriptorLayout) != VK_SUCCESS) {

			SetError ("vkCreateDescriptorSetLayout(program) failed");
			DiscardProgram (program);
			return 0;

		}

		VkPipelineLayoutCreateInfo layoutInfo = {};
		layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		layoutInfo.setLayoutCount = 1;
		layoutInfo.pSetLayouts = &program->descriptorLayout;

		if (dev.CreatePipelineLayout (state.device, &layoutInfo, NULL, &program->layout) != VK_SUCCESS) {

			SetError ("vkCreatePipelineLayout(program) failed");
			DiscardProgram (program);
			return 0;

		}

		// ---- Shader modules ----

		VkShaderModuleCreateInfo moduleInfo = {};
		moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;

		VkShaderModule vertexModule = VK_NULL_HANDLE;
		VkShaderModule fragmentModule = VK_NULL_HANDLE;

		moduleInfo.codeSize = compiled.vertex.size() * 4;
		moduleInfo.pCode = compiled.vertex.data();

		if (dev.CreateShaderModule (state.device, &moduleInfo, NULL, &vertexModule) != VK_SUCCESS) {

			SetError ("vkCreateShaderModule(program.vert) failed");
			DiscardProgram (program);
			return 0;

		}

		moduleInfo.codeSize = compiled.fragment.size() * 4;
		moduleInfo.pCode = compiled.fragment.data();

		if (dev.CreateShaderModule (state.device, &moduleInfo, NULL, &fragmentModule) != VK_SUCCESS) {

			SetError ("vkCreateShaderModule(program.frag) failed");
			dev.DestroyShaderModule (state.device, vertexModule, NULL);
			DiscardProgram (program);
			return 0;

		}

		bool ok = true;

		for (int blend = 0; blend < BLEND_COUNT && ok; ++blend) {

			ok = CreateProgramPipeline (program, blend, vertexModule, fragmentModule);

		}

		dev.DestroyShaderModule (state.device, vertexModule, NULL);
		dev.DestroyShaderModule (state.device, fragmentModule, NULL);

		if (!ok) {

			SetError ("vkCreateGraphicsPipelines(program) failed");
			DiscardProgram (program);
			return 0;

		}

		uint64_t handle = ++state.nextProgramHandle;

		if (handle == 0) handle = ++state.nextProgramHandle;

		program->handle = handle;
		state.programs[handle] = program;

		LARGE_INTEGER frequency;
		LARGE_INTEGER end;
		QueryPerformanceFrequency (&frequency);
		QueryPerformanceCounter (&end);

		double milliseconds = (double)(end.QuadPart - startCounter.QuadPart) * 1000.0 / (double)frequency.QuadPart;

		Log ("program %llu: %u byte uniform block, %u sampler(s), %s, %.1f ms (backend=%s)",
			(unsigned long long)handle, program->uniformSize, (unsigned)program->samplers.size(),
			program->premultiplied ? "premultiplied" : "straight alpha", milliseconds, compiled.backend);

		return handle;

	}

	void DestroyShaderProgram (uint64_t handle) {

		State& state = S ();

		std::map<uint64_t, ShaderProgram*>::iterator found = state.programs.find (handle);

		if (found == state.programs.end()) return;

		ShaderProgram* program = found->second;
		state.programs.erase (found);

		// A command buffer may still reference this pipeline, so the GPU has to be idle before it is destroyed.
		if (state.device != VK_NULL_HANDLE && state.dev.DeviceWaitIdle) {

			state.dev.DeviceWaitIdle (state.device);

		}

		DiscardProgram (program);

	}

	void DestroyAllShaderPrograms () {

		State& state = S ();

		if (state.programs.empty()) return;

		if (state.device != VK_NULL_HANDLE && state.dev.DeviceWaitIdle) {

			state.dev.DeviceWaitIdle (state.device);

		}

		for (std::map<uint64_t, ShaderProgram*>::iterator it = state.programs.begin(); it != state.programs.end(); ++it) {

			DiscardProgram (it->second);

		}

		state.programs.clear();

	}

	// ---------------------------------------------------------------------
	// Submission
	// ---------------------------------------------------------------------

	namespace {

		bool gDescriptorPoolExhausted = false;

		void WriteVector2 (ShaderProgram* program, const char* name, float x, float y) {

			void* slot = FindProgramUniform (program, name, glsl::TYPE_VEC2);

			if (!slot) return;

			float values[2] = { x, y };

			memcpy (slot, values, sizeof (values));

		}

	}

	/** Prepares the pipeline, the descriptor and the uniform data for one draw.
	 *  commandBuffer is only used for binding; the draw itself is issued by the
	 *  caller, which owns the index buffer and the first vertex. */
	bool PrepareProgramDraw (ShaderProgram* program, Texture* texture, int blend,
		VkCommandBuffer commandBuffer, uint32_t* dynamicOffset) {

		State& state = S ();
		DeviceApi& dev = state.dev;

		if (!program || !texture || !commandBuffer) return false;

		if (blend < 0 || blend >= BLEND_COUNT) blend = LIME_VULKAN_BLEND_NORMAL;

		VkPipeline pipeline = program->pipelines[blend];

		// The descriptor pool for this frame is full: fall back to the built-in pipeline so the frame stays visible instead of going black.
		if (state.frameDescriptorPool[state.frameIndex] == VK_NULL_HANDLE) return false;

		// ---- Uniforms recomputed every frame: projection matrix, texture size ----

		if (program->shadow.size() >= 64) {

			// The projection follows the open pass: a camera drawn into a render
			// target is pixels of that target, not pixels of the window.
			int activeWidth = 0;
			int activeHeight = 0;
			ActiveExtent (&activeWidth, &activeHeight);

			float width = (float)(activeWidth > 0 ? activeWidth : 1);
			float height = (float)(activeHeight > 0 ? activeHeight : 1);

			// Pixel space to NDC. In Vulkan NDC y = -1 is the top of the screen and pixel y points down, so no flip.
			float projection[16] = {
				2.0f / width, 0.0f, 0.0f, 0.0f,
				0.0f, 2.0f / height, 0.0f, 0.0f,
				0.0f, 0.0f, 1.0f, 0.0f,
				-1.0f, -1.0f, 0.0f, 1.0f
			};

			memcpy (&program->shadow[0], projection, sizeof (projection));

		}

		WriteVector2 (program, "openfl_TextureSize", (float)texture->width, (float)texture->height);

		// The stock OpenFL filter shaders (blur, glow, drop shadow) read the size of
		// the image they sample under this name in their own vertex stage. Same
		// value, second spelling: nothing else writes it, and OpenFL's renderer
		// normally fills it through Shader.__update.
		WriteVector2 (program, "uTextureSize", (float)texture->width, (float)texture->height);

		// ---- Uniform data into the ring buffer ----

		uint32_t offset = 0;

		if (!AllocateUniformBlock (program->uniformSize, &offset)) return false;

		memcpy ((unsigned char*)state.uniformRing.mapped[state.frameIndex] + offset,
			program->shadow.data(), program->uniformSize);

		// ---- Take a descriptor set ----

		VkDescriptorSet descriptorSet = VK_NULL_HANDLE;

		VkDescriptorSetAllocateInfo allocateInfo = {};
		allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		allocateInfo.descriptorPool = state.frameDescriptorPool[state.frameIndex];
		allocateInfo.descriptorSetCount = 1;
		allocateInfo.pSetLayouts = &program->descriptorLayout;

		if (dev.AllocateDescriptorSets (state.device, &allocateInfo, &descriptorSet) != VK_SUCCESS) {

			if (!gDescriptorPoolExhausted) {

				Log ("program: descriptor pool exhausted (%u sets per frame), falling back to the built-in pipeline",
					(unsigned)2048);
				gDescriptorPoolExhausted = true;

			}

			return false;

		}

		VkDescriptorBufferInfo bufferInfo = {};
		bufferInfo.buffer = state.uniformRing.buffer[state.frameIndex];
		bufferInfo.offset = 0;
		bufferInfo.range = program->uniformSize;

		VkWriteDescriptorSet writes[17] = {};
		VkDescriptorImageInfo images[16];

		writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[0].dstSet = descriptorSet;
		writes[0].dstBinding = 0;
		writes[0].descriptorCount = 1;
		writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		writes[0].pBufferInfo = &bufferInfo;

		uint32_t writeCount = 1;

		for (size_t i = 0; i < program->samplers.size() && i < 16; ++i) {

			Texture* bound = texture;

			if (program->samplerTextures[i] != 0) {

				Texture* custom = FindTexture (program->samplerTextures[i]);

				if (custom) bound = custom;

			}

			if (!bound) continue;

			images[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			images[i].imageView = bound->view;
			images[i].sampler = bound->samplers[S ().batch.currentFilter];

			writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[writeCount].dstSet = descriptorSet;
			writes[writeCount].dstBinding = (uint32_t)program->samplers[i].location;
			writes[writeCount].descriptorCount = 1;
			writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			writes[writeCount].pImageInfo = &images[i];

			++writeCount;

		}

		dev.UpdateDescriptorSets (state.device, writeCount, writes, 0, NULL);

		dev.CmdBindPipeline (commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		dev.CmdBindDescriptorSets (commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, program->layout,
			0, 1, &descriptorSet, 1, &offset);

		if (dynamicOffset) *dynamicOffset = offset;

		return true;

	}

	void ResetProgramFrameState () {

		State& state = S ();

		state.uniformRing.offset = 0;
		gDescriptorPoolExhausted = false;

		if (state.device == VK_NULL_HANDLE) return;

		VkDescriptorPool pool = state.frameDescriptorPool[state.frameIndex];

		if (pool != VK_NULL_HANDLE && state.dev.ResetDescriptorPool) {

			state.dev.ResetDescriptorPool (state.device, pool, 0);

		}

	}

}
