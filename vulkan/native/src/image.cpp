#include "internal.h"

#include <algorithm>

// Textures: RGBA8 upload, samplers, descriptor sets.
//
// Design notes:
//   * pixel data goes into one mapped staging buffer and the copy is recorded
//     once per frame, right before the frame is submitted (see
//     FlushPendingUploads). The CPU never waits for the copy: the frame submit
//     waits on the upload semaphore instead;
//   * an update reuses the existing image when the size did not change, so a
//     redrawn text or overlay texture does not allocate anything;
//   * no mipmaps by default: OpenFL bitmaps are single level with linear
//     filtering, and generating mips would not match the GL path;
//   * one descriptor set per texture (tens to hundreds of textures), so a flush
//     never has to allocate one;
//   * the handle is a 64-bit id; pointers stay out of the ABI.

namespace vulkan {

	Texture::Texture () :
		image (VK_NULL_HANDLE),
		memory (VK_NULL_HANDLE),
		view (VK_NULL_HANDLE),
		width (0),
		height (0),
		bytesPerPixel (4),
		allocation (0),
		framebuffer (VK_NULL_HANDLE),
		layout (VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {

		samplers[FILTER_NEAREST] = VK_NULL_HANDLE;
		samplers[FILTER_LINEAR] = VK_NULL_HANDLE;

		descriptors[FILTER_NEAREST] = VK_NULL_HANDLE;
		descriptors[FILTER_LINEAR] = VK_NULL_HANDLE;

	}

	// ---------------------------------------------------------------------
	// Memory types
	// ---------------------------------------------------------------------

	static bool FindMemoryType (uint32_t typeBits, VkMemoryPropertyFlags required, uint32_t* index) {

		VkPhysicalDeviceMemoryProperties memory;
		S ().api.GetPhysicalDeviceMemoryProperties (S ().physicalDevice, &memory);

		for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {

			if ((typeBits & (1u << i)) == 0) continue;

			if ((memory.memoryTypes[i].propertyFlags & required) == required) {

				*index = i;
				return true;

			}

		}

		return false;

	}

	// ---------------------------------------------------------------------
	// Upload queue
	// ---------------------------------------------------------------------

	struct PendingUpload {

		VkImage image;
		bool initialized;   /**< the image already has a defined layout */
		bool pixels;        /**< false: layout transition only */
		VkDeviceSize offset;
		int width;
		int height;
		uint32_t mipLevels;

	};

	static std::vector<PendingUpload> s_pending;

	static VkCommandPool s_uploadPool = VK_NULL_HANDLE;
	static VkCommandBuffer s_uploadBuffer = VK_NULL_HANDLE;
	static VkFence s_uploadFence = VK_NULL_HANDLE;
	static VkSemaphore s_uploadSemaphore = VK_NULL_HANDLE;
	static bool s_uploadInFlight = false;

	static VkBuffer s_staging = VK_NULL_HANDLE;
	static VkDeviceMemory s_stagingMemory = VK_NULL_HANDLE;
	static void* s_stagingMapped = NULL;
	static VkDeviceSize s_stagingSize = 0;
	static VkDeviceSize s_stagingUsed = 0;
	static bool s_burstClosed = true;

	// Uploads copy through a ring buffer that lives in host visible memory, which
	// is plain RAM. It is kept small on purpose: a ring sized for a whole burst
	// parks hundreds of megabytes there for the rest of the session.
	static const VkDeviceSize kStagingRing = 8u << 20;

	// Granularity when a single image does not fit into the ring.
	static const VkDeviceSize kStagingStep = 4u << 20;

	static VkSemaphore FlushPendingUploads (bool signal);

	static std::map<uint64_t, VkSampler> s_samplers;

	static bool EnsureUploadObjects () {

		State& state = S ();
		DeviceApi& dev = state.dev;

		if (s_uploadPool != VK_NULL_HANDLE) return true;

		VkCommandPoolCreateInfo poolInfo = {};
		poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		// RESET_COMMAND_BUFFER is required: the same buffer is recorded again for
		// every flush. TRANSIENT alone would make the reset undefined.
		poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		poolInfo.queueFamilyIndex = state.graphicsFamily;

		if (dev.CreateCommandPool (state.device, &poolInfo, NULL, &s_uploadPool) != VK_SUCCESS) {

			SetError ("vkCreateCommandPool(upload) failed");
			return false;

		}

		VkCommandBufferAllocateInfo allocateInfo = {};
		allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocateInfo.commandPool = s_uploadPool;
		allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocateInfo.commandBufferCount = 1;

		if (dev.AllocateCommandBuffers (state.device, &allocateInfo, &s_uploadBuffer) != VK_SUCCESS) {

			SetError ("vkAllocateCommandBuffers(upload) failed");
			return false;

		}

		VkFenceCreateInfo fenceInfo = {};
		fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

		if (dev.CreateFence (state.device, &fenceInfo, NULL, &s_uploadFence) != VK_SUCCESS) {

			SetError ("vkCreateFence(upload) failed");
			return false;

		}

		VkSemaphoreCreateInfo semaphoreInfo = {};
		semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

		if (dev.CreateSemaphore (state.device, &semaphoreInfo, NULL, &s_uploadSemaphore) != VK_SUCCESS) {

			SetError ("vkCreateSemaphore(upload) failed");
			return false;

		}

		return true;

	}

	/** Waits for the previously submitted upload, if there is one in flight. */
	static bool WaitUploads () {

		State& state = S ();

		if (!s_uploadInFlight) return true;

		s_uploadInFlight = false;

		if (state.device == VK_NULL_HANDLE || s_uploadFence == VK_NULL_HANDLE) return true;

		if (state.dev.WaitForFences (state.device, 1, &s_uploadFence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {

			SetError ("vkWaitForFences(upload) failed");
			return false;

		}

		state.dev.ResetFences (state.device, 1, &s_uploadFence);

		return true;

	}

	/** Gives the ring back to the driver. The next upload creates it again. */
	static void ReleaseStaging () {

		State& state = S ();

		if (state.device != VK_NULL_HANDLE) {

			if (s_stagingMapped != NULL) state.dev.UnmapMemory (state.device, s_stagingMemory);
			if (s_stagingMemory != VK_NULL_HANDLE) state.dev.FreeMemory (state.device, s_stagingMemory, NULL);
			if (s_staging != VK_NULL_HANDLE) state.dev.DestroyBuffer (state.device, s_staging, NULL);

		}

		s_staging = VK_NULL_HANDLE;
		s_stagingMemory = VK_NULL_HANDLE;
		s_stagingMapped = NULL;
		s_stagingSize = 0;
		s_stagingUsed = 0;
		state.memory.staging = 0;

	}

	/**
	 * Reserves staging space that is safe to write now.
	 *
	 * Writing into a region the previous upload is still reading would corrupt the
	 * copy, so the previous batch is waited for here. That happens once per burst
	 * (the fence is normally already signalled), not once per texture.
	 */
	static bool StagingAlloc (VkDeviceSize bytes, void** data, VkDeviceSize* offset) {

		State& state = S ();
		DeviceApi& dev = state.dev;

		// The previous burst is still being read by the GPU. Rewinding the buffer
		// is only safe once that finished, and only once per burst: doing it for
		// every texture made all uploads of a frame share the same bytes.
		if (s_burstClosed) {

			if (!WaitUploads ()) return false;

			s_stagingUsed = 0;

			// A ring grown for one large image goes back to the small size once the
			// GPU stopped reading it, instead of staying resident for the session.
			if (s_stagingSize > kStagingRing) ReleaseStaging ();

			s_burstClosed = false;

		}

		// A full ring is finished off and reused from the start. Growing it for the
		// whole burst is what turned a song load into a 256 MB ring.
		if (s_stagingUsed + bytes > s_stagingSize && s_stagingUsed > 0) {

			FlushPendingUploads (false);

			if (!WaitUploads ()) return false;

			s_stagingUsed = 0;
			s_burstClosed = false;

		}

		// Grow only for an image that is larger than the ring itself. Everything
		// else was already handled by reusing the ring above.
		if (s_stagingUsed + bytes > s_stagingSize) {

			VkDeviceSize size = s_stagingSize > 0 ? s_stagingSize : kStagingRing;

			while (size < s_stagingUsed + bytes) size <<= 1;

			VkBuffer previousBuffer = s_staging;
			VkDeviceMemory previousMemory = s_stagingMemory;
			void* previousMapped = s_stagingMapped;
			VkDeviceSize carried = s_stagingUsed;

			s_staging = VK_NULL_HANDLE;
			s_stagingMemory = VK_NULL_HANDLE;
			s_stagingMapped = NULL;
			s_stagingSize = 0;

			VkBufferCreateInfo bufferInfo = {};
			bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
			bufferInfo.size = size;
			bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
			bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

			if (dev.CreateBuffer (state.device, &bufferInfo, NULL, &s_staging) != VK_SUCCESS) {

				SetError ("vkCreateBuffer(staging) failed");
				return false;

			}

			VkMemoryRequirements requirements;
			dev.GetBufferMemoryRequirements (state.device, s_staging, &requirements);

			uint32_t type = 0;

			if (!FindMemoryType (requirements.memoryTypeBits,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &type)) {

				SetError ("no host visible memory for the staging buffer");
				dev.DestroyBuffer (state.device, s_staging, NULL);
				s_staging = VK_NULL_HANDLE;
				return false;

			}

			VkMemoryAllocateInfo allocateInfo = {};
			allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
			allocateInfo.allocationSize = requirements.size;
			allocateInfo.memoryTypeIndex = type;

			if (dev.AllocateMemory (state.device, &allocateInfo, NULL, &s_stagingMemory) != VK_SUCCESS
				|| dev.BindBufferMemory (state.device, s_staging, s_stagingMemory, 0) != VK_SUCCESS) {

				SetError ("staging buffer allocation failed");

				if (s_stagingMemory) dev.FreeMemory (state.device, s_stagingMemory, NULL);

				dev.DestroyBuffer (state.device, s_staging, NULL);

				s_staging = VK_NULL_HANDLE;
				s_stagingMemory = VK_NULL_HANDLE;

				return false;

			}

			state.memory.staging = requirements.size;

			if (dev.MapMemory (state.device, s_stagingMemory, 0, size, 0, &s_stagingMapped) != VK_SUCCESS) {

				SetError ("vkMapMemory(staging) failed");

				dev.FreeMemory (state.device, s_stagingMemory, NULL);
				dev.DestroyBuffer (state.device, s_staging, NULL);

				s_staging = VK_NULL_HANDLE;
				s_stagingMemory = VK_NULL_HANDLE;
				s_stagingMapped = NULL;

				return false;

			}

			// Queued uploads reference the staging buffer by offset, so the pixels
			// already written have to move with it.
			if (carried > 0 && previousMapped != NULL) {

				memcpy (s_stagingMapped, previousMapped, (size_t)carried);

			}

			if (previousBuffer != VK_NULL_HANDLE) {

				if (previousMapped != NULL) dev.UnmapMemory (state.device, previousMemory);
				dev.FreeMemory (state.device, previousMemory, NULL);
				dev.DestroyBuffer (state.device, previousBuffer, NULL);

			}

			s_stagingSize = size;

		}

		*offset = s_stagingUsed;
		*data = (unsigned char*)s_stagingMapped + s_stagingUsed;

		s_stagingUsed += (bytes + 15) & ~(VkDeviceSize)15;

		return true;

	}

	/**
	 * Queues one upload. The pixels are copied into staging now; the GPU copy is
	 * recorded by FlushPendingUploads.
	 *
	 * @param rows NULL queues only the layout transition of a fresh image.
	 */
	static bool QueueUpload (VkImage image, bool initialized, const void* rows, int width, int height,
		uint32_t mipLevels, bool bgra, bool unpremultiply, int bytesPerPixel) {

		VkDeviceSize bytes = rows ? (VkDeviceSize)width * (VkDeviceSize)height * (VkDeviceSize)bytesPerPixel : 0;
		VkDeviceSize offset = 0;

		if (bytes > 0) {

			void* data = NULL;

			if (!StagingAlloc (bytes, &data, &offset) || data == NULL) return false;

			if (bytesPerPixel == 4 && (bgra || unpremultiply)) {

				// Format normalisation happens here, once, so the renderer does not
				// have to know about byte order.
				unsigned char* target = (unsigned char*)data;
				const unsigned char* source = (const unsigned char*)rows;
				const uint32_t pixels = (uint32_t)width * (uint32_t)height;

				for (uint32_t i = 0; i < pixels; ++i) {

					const unsigned char* from = source + (size_t)i * 4u;
					unsigned char* to = target + (size_t)i * 4u;

					unsigned char red = bgra ? from[2] : from[0];
					const unsigned char green = from[1];
					unsigned char blue = bgra ? from[0] : from[2];
					const unsigned char alpha = from[3];

					if (unpremultiply && alpha > 0 && alpha < 255) {

						const int scale = 255 * 255 / (int)alpha;

						red = (unsigned char)std::min (255, (int)red * scale / 255);
						blue = (unsigned char)std::min (255, (int)blue * scale / 255);

						to[1] = (unsigned char)std::min (255, (int)green * scale / 255);

					} else {

						to[1] = green;

					}

					to[0] = red;
					to[2] = blue;
					to[3] = alpha;

				}

			} else {

				memcpy (data, rows, (size_t)bytes);

			}

		}

		PendingUpload entry;
		entry.image = image;
		entry.initialized = initialized;
		entry.pixels = bytes > 0;
		entry.offset = offset;
		entry.width = width;
		entry.height = height;
		entry.mipLevels = mipLevels;

		s_pending.push_back (entry);

		return true;

	}

	/**
	 * Records every queued copy into one command buffer and submits it.
	 *
	 * @return semaphore the next frame submit has to wait on, or VK_NULL_HANDLE
	 *         when there was nothing to do.
	 */
	static VkSemaphore FlushPendingUploads (bool signal) {

		State& state = S ();
		DeviceApi& dev = state.dev;

		if (s_pending.empty ()) return VK_NULL_HANDLE;

		// Overwriting an image that a frame still in flight is sampling is a race the
		// driver cannot see. A freshly created image has no such reader, but an
		// update does: wait for the most recently submitted frame to finish before
		// the copy is queued. That fence is normally already signalled, so this
		// costs a status query and nothing else.
		bool updates = false;

		for (size_t i = 0; i < s_pending.size (); ++i) {

			if (s_pending[i].initialized) { updates = true; break; }

		}

		if (updates) {

			State& frameState = S ();
			uint32_t previous = (frameState.frameIndex + 1) % MAX_FRAMES_IN_FLIGHT;

			if (previous < frameState.inFlight.size () && frameState.inFlight[previous] != VK_NULL_HANDLE) {

				if (dev.WaitForFences (state.device, 1, &frameState.inFlight[previous], VK_TRUE, UINT64_MAX) != VK_SUCCESS) {

					SetError ("vkWaitForFences(update) failed");
					s_pending.clear ();
					return VK_NULL_HANDLE;

				}

			}

		}

		if (!EnsureUploadObjects () || !WaitUploads ()) {

			s_pending.clear ();
			return VK_NULL_HANDLE;

		}

		dev.ResetCommandBuffer (s_uploadBuffer, 0);

		VkCommandBufferBeginInfo beginInfo = {};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

		if (dev.BeginCommandBuffer (s_uploadBuffer, &beginInfo) != VK_SUCCESS) {

			SetError ("vkBeginCommandBuffer(upload) failed");
			s_pending.clear ();
			return VK_NULL_HANDLE;

		}

		for (size_t i = 0; i < s_pending.size (); ++i) {

			PendingUpload& entry = s_pending[i];

			VkImageMemoryBarrier barrier = {};
			barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			barrier.oldLayout = entry.initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
			barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = entry.image;
			barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			barrier.subresourceRange.baseMipLevel = 0;
			barrier.subresourceRange.levelCount = entry.mipLevels;
			barrier.subresourceRange.baseArrayLayer = 0;
			barrier.subresourceRange.layerCount = 1;
			barrier.srcAccessMask = entry.initialized ? VK_ACCESS_SHADER_READ_BIT : 0;
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

			dev.CmdPipelineBarrier (s_uploadBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
				0, 0, NULL, 0, NULL, 1, &barrier);

			if (entry.pixels) {

				VkBufferImageCopy region = {};
				region.bufferOffset = entry.offset;
				region.bufferRowLength = 0;
				region.bufferImageHeight = 0;
				region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				region.imageSubresource.mipLevel = 0;
				region.imageSubresource.baseArrayLayer = 0;
				region.imageSubresource.layerCount = 1;
				region.imageExtent.width = (uint32_t)entry.width;
				region.imageExtent.height = (uint32_t)entry.height;
				region.imageExtent.depth = 1;

				dev.CmdCopyBufferToImage (s_uploadBuffer, s_staging, entry.image,
					VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

			}

			if (entry.mipLevels > 1) {

				VkImageMemoryBarrier toSource = barrier;
				toSource.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				toSource.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
				toSource.subresourceRange.levelCount = 1;
				toSource.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				toSource.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

				dev.CmdPipelineBarrier (s_uploadBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
					0, 0, NULL, 0, NULL, 1, &toSource);

				int sourceWidth = entry.width;
				int sourceHeight = entry.height;

				for (uint32_t level = 1; level < entry.mipLevels; ++level) {

					VkImageMemoryBarrier toDestination = barrier;
					toDestination.subresourceRange.baseMipLevel = level;
					toDestination.subresourceRange.levelCount = 1;

					dev.CmdPipelineBarrier (s_uploadBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
						0, 0, NULL, 0, NULL, 1, &toDestination);

					VkImageBlit blit = {};
					blit.srcOffsets[0].x = 0;
					blit.srcOffsets[0].y = 0;
					blit.srcOffsets[0].z = 0;
					blit.srcOffsets[1].x = sourceWidth;
					blit.srcOffsets[1].y = sourceHeight;
					blit.srcOffsets[1].z = 1;
					blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
					blit.srcSubresource.mipLevel = level - 1;
					blit.srcSubresource.baseArrayLayer = 0;
					blit.srcSubresource.layerCount = 1;
					blit.dstOffsets[0].x = 0;
					blit.dstOffsets[0].y = 0;
					blit.dstOffsets[0].z = 0;
					blit.dstOffsets[1].x = sourceWidth > 1 ? sourceWidth / 2 : 1;
					blit.dstOffsets[1].y = sourceHeight > 1 ? sourceHeight / 2 : 1;
					blit.dstOffsets[1].z = 1;
					blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
					blit.dstSubresource.mipLevel = level;
					blit.dstSubresource.baseArrayLayer = 0;
					blit.dstSubresource.layerCount = 1;

					dev.CmdBlitImage (s_uploadBuffer, entry.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
						entry.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

					VkImageMemoryBarrier sourceToDestination = toSource;
					sourceToDestination.subresourceRange.baseMipLevel = level;
					sourceToDestination.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
					sourceToDestination.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

					dev.CmdPipelineBarrier (s_uploadBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
						0, 0, NULL, 0, NULL, 1, &sourceToDestination);

					if (sourceWidth > 1) sourceWidth /= 2;
					if (sourceHeight > 1) sourceHeight /= 2;

				}

				VkImageMemoryBarrier toRead = barrier;
				toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
				toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				toRead.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
				toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

				dev.CmdPipelineBarrier (s_uploadBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
					0, 0, NULL, 0, NULL, 1, &toRead);

			} else {

				VkImageMemoryBarrier toRead = barrier;
				toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

				dev.CmdPipelineBarrier (s_uploadBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
					0, 0, NULL, 0, NULL, 1, &toRead);

			}

		}

		s_pending.clear ();

		if (dev.EndCommandBuffer (s_uploadBuffer) != VK_SUCCESS) {

			SetError ("vkEndCommandBuffer(upload) failed");
			return VK_NULL_HANDLE;

		}

		VkSubmitInfo submitInfo = {};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &s_uploadBuffer;
		// An upload that is waited for here needs no semaphore: the copy is done
		// before anything else is submitted, and the caller just reads the fence.
		submitInfo.signalSemaphoreCount = signal ? 1 : 0;
		submitInfo.pSignalSemaphores = signal ? &s_uploadSemaphore : NULL;

		if (dev.QueueSubmit (state.graphicsQueue, 1, &submitInfo, s_uploadFence) != VK_SUCCESS) {

			SetError ("vkQueueSubmit(upload) failed");
			return VK_NULL_HANDLE;

		}

		s_uploadInFlight = true;
		s_burstClosed = true;

		return signal ? s_uploadSemaphore : VK_NULL_HANDLE;

	}

	VkSemaphore FlushTextureUploads () {

		return FlushPendingUploads (true);

	}

	// ---------------------------------------------------------------------
	// Resources: descriptor layout / pool / samplers
	// ---------------------------------------------------------------------

	static VkSampler SamplerFor (bool nearest, bool repeat, uint32_t mipLevels) {

		uint64_t key = (nearest ? 1ull : 0ull) | (repeat ? 2ull : 0ull) | ((uint64_t)mipLevels << 8);

		std::map<uint64_t, VkSampler>::iterator found = s_samplers.find (key);

		if (found != s_samplers.end ()) return found->second;

		State& state = S ();

		VkSamplerCreateInfo samplerInfo = {};
		samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		samplerInfo.magFilter = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
		samplerInfo.minFilter = samplerInfo.magFilter;
		samplerInfo.mipmapMode = mipLevels > 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
		samplerInfo.addressModeU = repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.addressModeV = samplerInfo.addressModeU;
		samplerInfo.addressModeW = samplerInfo.addressModeU;
		samplerInfo.maxLod = mipLevels > 1 ? (float)mipLevels : 0.0f;

		VkSampler sampler = VK_NULL_HANDLE;

		if (state.dev.CreateSampler (state.device, &samplerInfo, NULL, &sampler) != VK_SUCCESS) {

			SetError ("vkCreateSampler failed");
			return VK_NULL_HANDLE;

		}

		s_samplers[key] = sampler;

		return sampler;

	}

	bool CreateTextureResources () {

		State& state = S ();
		DeviceApi& dev = state.dev;

		VkDescriptorSetLayoutBinding binding = {};
		binding.binding = 0;
		binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		binding.descriptorCount = 1;
		binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

		VkDescriptorSetLayoutCreateInfo layoutInfo = {};
		layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		layoutInfo.bindingCount = 1;
		layoutInfo.pBindings = &binding;

		if (dev.CreateDescriptorSetLayout (state.device, &layoutInfo, NULL, &state.batch.descriptorLayout) != VK_SUCCESS) {

			SetError ("vkCreateDescriptorSetLayout failed");
			return false;

		}

		VkDescriptorPoolSize poolSize = {};
		poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		poolSize.descriptorCount = 4096;

		VkDescriptorPoolCreateInfo poolInfo = {};
		poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
		poolInfo.maxSets = 4096;
		poolInfo.poolSizeCount = 1;
		poolInfo.pPoolSizes = &poolSize;

		if (dev.CreateDescriptorPool (state.device, &poolInfo, NULL, &state.batch.descriptorPool) != VK_SUCCESS) {

			SetError ("vkCreateDescriptorPool failed");
			return false;

		}

		// 1x1 white texture: solid rectangles (UI borders, health bars) do not need
		// a texture of their own.
		const unsigned char white[4] = { 255, 255, 255, 255 };

		state.whiteTexture = CreateTexture (white, 1, 1, LIME_VULKAN_TEXTURE_NEAREST);

		if (state.whiteTexture == 0) {

			SetError ("could not create the 1x1 white texture");
			return false;

		}

		return true;

	}

	void DestroyTextureResources () {

		State& state = S ();
		DeviceApi& dev = state.dev;

		std::vector<uint64_t> handles;

		for (std::map<uint64_t, Texture*>::iterator it = state.textures.begin (); it != state.textures.end (); ++it) {

			handles.push_back (it->first);

		}

		for (size_t i = 0; i < handles.size (); ++i) {

			DestroyTexture (handles[i]);

		}

		state.whiteTexture = 0;
		s_pending.clear ();

		if (state.device != VK_NULL_HANDLE) WaitUploads ();

		for (std::map<uint64_t, VkSampler>::iterator it = s_samplers.begin (); it != s_samplers.end (); ++it) {

			if (state.device != VK_NULL_HANDLE) dev.DestroySampler (state.device, it->second, NULL);

		}

		s_samplers.clear ();

		ReleaseStaging ();
		s_burstClosed = true;

		if (s_uploadSemaphore != VK_NULL_HANDLE) {

			if (state.device != VK_NULL_HANDLE) dev.DestroySemaphore (state.device, s_uploadSemaphore, NULL);
			s_uploadSemaphore = VK_NULL_HANDLE;

		}

		if (s_uploadFence != VK_NULL_HANDLE) {

			if (state.device != VK_NULL_HANDLE) dev.DestroyFence (state.device, s_uploadFence, NULL);
			s_uploadFence = VK_NULL_HANDLE;

		}

		if (s_uploadPool != VK_NULL_HANDLE) {

			if (state.device != VK_NULL_HANDLE) dev.DestroyCommandPool (state.device, s_uploadPool, NULL);

			s_uploadPool = VK_NULL_HANDLE;
			s_uploadBuffer = VK_NULL_HANDLE;

		}

		if (state.batch.descriptorPool != VK_NULL_HANDLE) {

			dev.DestroyDescriptorPool (state.device, state.batch.descriptorPool, NULL);
			state.batch.descriptorPool = VK_NULL_HANDLE;

		}

		if (state.batch.descriptorLayout != VK_NULL_HANDLE) {

			dev.DestroyDescriptorSetLayout (state.device, state.batch.descriptorLayout, NULL);
			state.batch.descriptorLayout = VK_NULL_HANDLE;

		}

	}

	// ---------------------------------------------------------------------

	Texture* FindTexture (uint64_t handle) {

		if (handle == 0) return NULL;

		std::map<uint64_t, Texture*>& table = S ().textures;
		std::map<uint64_t, Texture*>::iterator it = table.find (handle);

		if (it == table.end ()) return NULL;

		return it->second;

	}

	uint64_t WhiteTexture () {

		return S ().whiteTexture;

	}

	// ---------------------------------------------------------------------
	// Create / destroy
	// ---------------------------------------------------------------------

	/** Returns both descriptor sets to the pool, ignoring the ones never made. */
	static void FreeTextureDescriptors (Texture* texture) {

		State& state = S ();
		DeviceApi& dev = state.dev;

		if (!texture || !dev.FreeDescriptorSets || state.batch.descriptorPool == VK_NULL_HANDLE) return;

		VkDescriptorSet sets[2];
		uint32_t count = 0;

		for (int i = 0; i < 2; ++i) {

			if (texture->descriptors[i] == VK_NULL_HANDLE) continue;

			sets[count++] = texture->descriptors[i];
			texture->descriptors[i] = VK_NULL_HANDLE;

		}

		if (count > 0) dev.FreeDescriptorSets (state.device, state.batch.descriptorPool, count, sets);

	}

	static void DiscardTexture (Texture* texture) {

		if (!texture) return;

		State& state = S ();
		DeviceApi& dev = state.dev;

		if (state.device != VK_NULL_HANDLE) {

			FreeTextureDescriptors (texture);

			// The samplers come from the shared cache and stay alive.
			if (texture->framebuffer != VK_NULL_HANDLE) dev.DestroyFramebuffer (state.device, texture->framebuffer, NULL);
			if (texture->view != VK_NULL_HANDLE) dev.DestroyImageView (state.device, texture->view, NULL);
			if (texture->memory != VK_NULL_HANDLE) dev.FreeMemory (state.device, texture->memory, NULL);
			if (texture->image != VK_NULL_HANDLE) dev.DestroyImage (state.device, texture->image, NULL);

			state.memory.textures -= texture->allocation;
			if (texture->allocation > 0) state.memory.textureCount--;

		}

		delete texture;

	}

	uint64_t CreateTexture (const void* rgba, int width, int height, int flags) {

		State& state = S ();
		Api& api = state.api;
		DeviceApi& dev = state.dev;

		if (state.device == VK_NULL_HANDLE) {

			SetError ("lime_vulkan_texture_create before a device exists");
			return 0;

		}

		if (width <= 0 || height <= 0) {

			SetError ("lime_vulkan_texture_create: invalid size");
			return 0;

		}

		bool mipmaps = (flags & LIME_VULKAN_TEXTURE_MIPMAPS) != 0;
		bool repeat = (flags & LIME_VULKAN_TEXTURE_REPEAT) != 0;
		bool nearest = (flags & LIME_VULKAN_TEXTURE_NEAREST) != 0;
		bool bgra = (flags & LIME_VULKAN_TEXTURE_BGRA) != 0;
		bool unpremultiply = (flags & LIME_VULKAN_TEXTURE_UNPREMULTIPLY) != 0;
		bool index8 = (flags & LIME_VULKAN_TEXTURE_INDEX8) != 0;
		int bytesPerPixel = index8 ? 1 : 4;
		VkFormat format = index8 ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;

		uint32_t mipLevels = 1;

		if (mipmaps) {

			uint32_t size = (uint32_t)std::max (width, height);

			while (size > 1) {

				size >>= 1;
				mipLevels++;

			}

		}

		Texture* texture = new Texture ();
		texture->width = width;
		texture->height = height;
		texture->bytesPerPixel = bytesPerPixel;

		VkImageCreateInfo imageInfo = {};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.format = format;
		imageInfo.extent.width = (uint32_t)width;
		imageInfo.extent.height = (uint32_t)height;
		imageInfo.extent.depth = 1;
		imageInfo.mipLevels = mipLevels;
		imageInfo.arrayLayers = 1;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
			(mipmaps ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
		imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

		if (dev.CreateImage (state.device, &imageInfo, NULL, &texture->image) != VK_SUCCESS) {

			SetError ("vkCreateImage(texture) failed");
			delete texture;
			return 0;

		}

		VkMemoryRequirements requirements;
		dev.GetImageMemoryRequirements (state.device, texture->image, &requirements);

		uint32_t memoryType = 0;

		if (!FindMemoryType (requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memoryType)) {

			SetError ("no device local memory type for the texture");
			DiscardTexture (texture);
			return 0;

		}

		VkMemoryAllocateInfo allocateInfo = {};
		allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocateInfo.allocationSize = requirements.size;
		allocateInfo.memoryTypeIndex = memoryType;

		if (dev.AllocateMemory (state.device, &allocateInfo, NULL, &texture->memory) != VK_SUCCESS) {

			SetError ("vkAllocateMemory(texture) failed");
			DiscardTexture (texture);
			return 0;

		}

		texture->allocation = requirements.size;
		state.memory.textures += requirements.size;
		state.memory.textureCount++;

		if (dev.BindImageMemory (state.device, texture->image, texture->memory, 0) != VK_SUCCESS) {

			SetError ("vkBindImageMemory failed");
			DiscardTexture (texture);
			return 0;

		}

		if (!QueueUpload (texture->image, false, rgba, width, height, mipLevels, bgra, unpremultiply, bytesPerPixel)) {

			DiscardTexture (texture);
			return 0;

		}

		// ---- view / sampler / descriptor ----

		VkImageViewCreateInfo viewInfo = {};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = texture->image;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = format;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.baseMipLevel = 0;
		viewInfo.subresourceRange.levelCount = mipLevels;
		viewInfo.subresourceRange.baseArrayLayer = 0;
		viewInfo.subresourceRange.layerCount = 1;

		if (dev.CreateImageView (state.device, &viewInfo, NULL, &texture->view) != VK_SUCCESS) {

			SetError ("vkCreateImageView(texture) failed");
			DiscardTexture (texture);
			return 0;

		}

		// Both filters are built here so a draw can pick one without rewriting a
		// descriptor set that a frame in flight may still be using.
		texture->samplers[FILTER_NEAREST] = SamplerFor (true, repeat, mipLevels);
		texture->samplers[FILTER_LINEAR] = SamplerFor (false, repeat, mipLevels);

		if (texture->samplers[FILTER_NEAREST] == VK_NULL_HANDLE || texture->samplers[FILTER_LINEAR] == VK_NULL_HANDLE) {

			DiscardTexture (texture);
			return 0;

		}

		VkDescriptorSetLayout layouts[2] = { state.batch.descriptorLayout, state.batch.descriptorLayout };

		VkDescriptorSetAllocateInfo setInfo = {};
		setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		setInfo.descriptorPool = state.batch.descriptorPool;
		setInfo.descriptorSetCount = 2;
		setInfo.pSetLayouts = layouts;

		VkDescriptorSet sets[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };

		if (dev.AllocateDescriptorSets (state.device, &setInfo, sets) != VK_SUCCESS) {

			SetError ("vkAllocateDescriptorSets failed (descriptor pool exhausted?)");
			DiscardTexture (texture);
			return 0;

		}

		texture->descriptors[FILTER_NEAREST] = sets[0];
		texture->descriptors[FILTER_LINEAR] = sets[1];

		VkDescriptorImageInfo descriptorImages[2] = {};
		VkWriteDescriptorSet writes[2] = {};

		for (int i = 0; i < 2; ++i) {

			descriptorImages[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			descriptorImages[i].imageView = texture->view;
			descriptorImages[i].sampler = texture->samplers[i];

			writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[i].dstSet = texture->descriptors[i];
			writes[i].dstBinding = 0;
			writes[i].descriptorCount = 1;
			writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			writes[i].pImageInfo = &descriptorImages[i];

		}

		dev.UpdateDescriptorSets (state.device, 2, writes, 0, NULL);

		uint64_t handle = state.nextTextureHandle++;

		if (handle == 0) handle = state.nextTextureHandle++;

		state.textures[handle] = texture;

		return handle;

	}

	int UpdateTexture (uint64_t handle, const void* rgba, int flags) {

		Texture* texture = FindTexture (handle);

		if (!texture || !rgba) return 0;

		if (S ().device == VK_NULL_HANDLE) return 0;

		bool bgra = (flags & LIME_VULKAN_TEXTURE_BGRA) != 0;
		bool unpremultiply = (flags & LIME_VULKAN_TEXTURE_UNPREMULTIPLY) != 0;

		if (!QueueUpload (texture->image, true, rgba, texture->width, texture->height, 1, bgra, unpremultiply, texture->bytesPerPixel)) {

			return 0;

		}

		return 1;

	}

	void RetireTexture (uint64_t handle) {

		State& state = S ();

		if (state.textures.find (handle) == state.textures.end ()) return;

		State::RetiredTexture entry;
		entry.handle = handle;
		entry.frame = state.framesPresented;
		state.retiredTextures.push_back (entry);

	}

	void DrainRetiredTextures () {

		State& state = S ();

		if (state.retiredTextures.empty ()) return;

		// Wait until the frames that were already in flight when the texture was
		// retired are done. One extra frame is cheap insurance; one frame too few
		// is a lost device.
		const uint64_t safeAfter = MAX_FRAMES_IN_FLIGHT + 1;

		for (size_t i = 0; i < state.retiredTextures.size (); ) {

			if (state.framesPresented >= state.retiredTextures[i].frame + safeAfter) {

				DestroyTexture (state.retiredTextures[i].handle);
				state.retiredTextures.erase (state.retiredTextures.begin () + (long)i);

			} else {

				++i;

			}

		}

	}

	void DestroyTexture (uint64_t handle) {

		State& state = S ();
		DeviceApi& dev = state.dev;

		std::map<uint64_t, Texture*>::iterator it = state.textures.find (handle);

		if (it == state.textures.end ()) return;

		Texture* texture = it->second;
		state.textures.erase (it);

		if (state.device == VK_NULL_HANDLE) {

			delete texture;
			return;

		}

		FreeTextureDescriptors (texture);

		if (texture->framebuffer != VK_NULL_HANDLE) dev.DestroyFramebuffer (state.device, texture->framebuffer, NULL);
		if (texture->view != VK_NULL_HANDLE) dev.DestroyImageView (state.device, texture->view, NULL);
		if (texture->memory != VK_NULL_HANDLE) dev.FreeMemory (state.device, texture->memory, NULL);
		if (texture->image != VK_NULL_HANDLE) dev.DestroyImage (state.device, texture->image, NULL);

		// Freed memory has to leave the totals as well, or lime_vulkan_batch_stats keeps
		// reporting every texture this session ever retired.
		state.memory.textures -= texture->allocation;
		if (texture->allocation > 0) state.memory.textureCount--;

		delete texture;

	}

	// ---------------------------------------------------------------------
	// Render targets
	// ---------------------------------------------------------------------

	/**
	 * A texture the batch can also draw into.
	 *
	 * The format is the swapchain's on purpose: a graphics pipeline is only
	 * compatible with render passes that use the same attachment formats, so
	 * sharing the format keeps every existing pipeline usable in both passes
	 * and costs no extra pipeline objects.
	 */
	uint64_t CreateRenderTarget (int width, int height) {

		State& state = S ();
		DeviceApi& dev = state.dev;

		if (state.device == VK_NULL_HANDLE || state.targetPass == VK_NULL_HANDLE) {

			SetError ("lime_vulkan_target_create before the device finished attaching");
			return 0;

		}

		if (width <= 0 || height <= 0) {

			SetError ("lime_vulkan_target_create: invalid size");
			return 0;

		}

		Texture* texture = new Texture ();
		texture->width = width;
		texture->height = height;
		texture->bytesPerPixel = 4;
		// Never drawn into yet, so the first pass may discard the contents.
		texture->layout = VK_IMAGE_LAYOUT_UNDEFINED;

		VkImageCreateInfo imageInfo = {};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.format = state.swapchainFormat;
		imageInfo.extent.width = (uint32_t)width;
		imageInfo.extent.height = (uint32_t)height;
		imageInfo.extent.depth = 1;
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = 1;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

		if (dev.CreateImage (state.device, &imageInfo, NULL, &texture->image) != VK_SUCCESS) {

			SetError ("vkCreateImage(target) failed");
			delete texture;
			return 0;

		}

		VkMemoryRequirements requirements;
		dev.GetImageMemoryRequirements (state.device, texture->image, &requirements);

		uint32_t memoryType = 0;

		if (!FindMemoryType (requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memoryType)) {

			SetError ("no device local memory type for the render target");
			DiscardTexture (texture);
			return 0;

		}

		VkMemoryAllocateInfo allocateInfo = {};
		allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocateInfo.allocationSize = requirements.size;
		allocateInfo.memoryTypeIndex = memoryType;

		if (dev.AllocateMemory (state.device, &allocateInfo, NULL, &texture->memory) != VK_SUCCESS) {

			SetError ("vkAllocateMemory(target) failed");
			DiscardTexture (texture);
			return 0;

		}

		texture->allocation = requirements.size;
		state.memory.textures += requirements.size;
		state.memory.textureCount++;

		if (dev.BindImageMemory (state.device, texture->image, texture->memory, 0) != VK_SUCCESS) {

			SetError ("vkBindImageMemory(target) failed");
			DiscardTexture (texture);
			return 0;

		}

		VkImageViewCreateInfo viewInfo = {};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = texture->image;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = state.swapchainFormat;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.baseMipLevel = 0;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.baseArrayLayer = 0;
		viewInfo.subresourceRange.layerCount = 1;

		if (dev.CreateImageView (state.device, &viewInfo, NULL, &texture->view) != VK_SUCCESS) {

			SetError ("vkCreateImageView(target) failed");
			DiscardTexture (texture);
			return 0;

		}

		VkFramebufferCreateInfo framebufferInfo = {};
		framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		framebufferInfo.renderPass = state.targetPass;
		framebufferInfo.attachmentCount = 1;
		framebufferInfo.pAttachments = &texture->view;
		framebufferInfo.width = (uint32_t)width;
		framebufferInfo.height = (uint32_t)height;
		framebufferInfo.layers = 1;

		if (dev.CreateFramebuffer (state.device, &framebufferInfo, NULL, &texture->framebuffer) != VK_SUCCESS) {

			SetError ("vkCreateFramebuffer(target) failed");
			DiscardTexture (texture);
			return 0;

		}

		// Sampling a target is always 1:1 (a filter pass reads the previous one
		// pixel for pixel), so both filters exist but only NEAREST is normally used.
		texture->samplers[FILTER_NEAREST] = SamplerFor (true, false, 1);
		texture->samplers[FILTER_LINEAR] = SamplerFor (false, false, 1);

		if (texture->samplers[FILTER_NEAREST] == VK_NULL_HANDLE || texture->samplers[FILTER_LINEAR] == VK_NULL_HANDLE) {

			DiscardTexture (texture);
			return 0;

		}

		VkDescriptorSetLayout layouts[2] = { state.batch.descriptorLayout, state.batch.descriptorLayout };

		VkDescriptorSetAllocateInfo setInfo = {};
		setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		setInfo.descriptorPool = state.batch.descriptorPool;
		setInfo.descriptorSetCount = 2;
		setInfo.pSetLayouts = layouts;

		VkDescriptorSet sets[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };

		if (dev.AllocateDescriptorSets (state.device, &setInfo, sets) != VK_SUCCESS) {

			SetError ("vkAllocateDescriptorSets failed (descriptor pool exhausted?)");
			DiscardTexture (texture);
			return 0;

		}

		texture->descriptors[FILTER_NEAREST] = sets[0];
		texture->descriptors[FILTER_LINEAR] = sets[1];

		VkDescriptorImageInfo descriptorImages[2] = {};
		VkWriteDescriptorSet writes[2] = {};

		for (int i = 0; i < 2; ++i) {

			descriptorImages[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			descriptorImages[i].imageView = texture->view;
			descriptorImages[i].sampler = texture->samplers[i];

			writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[i].dstSet = texture->descriptors[i];
			writes[i].dstBinding = 0;
			writes[i].descriptorCount = 1;
			writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			writes[i].pImageInfo = &descriptorImages[i];

		}

		dev.UpdateDescriptorSets (state.device, 2, writes, 0, NULL);

		uint64_t handle = state.nextTextureHandle++;

		if (handle == 0) handle = state.nextTextureHandle++;

		state.textures[handle] = texture;

		return handle;

	}

}
