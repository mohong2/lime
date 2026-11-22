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
		block (NULL),
		offset (0),
		memory (VK_NULL_HANDLE),
		view (VK_NULL_HANDLE),
		width (0),
		height (0),
		format (VK_FORMAT_R8G8B8A8_UNORM),
		bytesPerPixel (4),
		allocation (0),
		reserved (0),
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
	// Image memory: blocks with suballocations
	//
	// One vkAllocateMemory per texture hits a driver limit (typically 4096 live
	// allocations per heap) as soon as a modded song loads its sprites, and every
	// allocation has its own driver bookkeeping. So images are carved out of
	// larger blocks per memory type instead, with a first-fit free list and
	// coalescing on release.
	//
	// A block that ends up with no live image is destroyed, which is what keeps
	// the total flat across "load a song, leave, load another one" instead of
	// reserving the high water mark for the session.
	//
	// Deliberately not VMA: the renderer needs no defragmentation, no budget
	// callbacks and no virtual addresses, and 200 lines here beat a dependency.

	struct MemoryBlock {

		VkDeviceMemory memory;
		uint32_t typeIndex;
		VkDeviceSize size;
		int live;

		struct Range { VkDeviceSize offset, size; };

		std::vector<Range> freeRanges;

	};

	static std::vector<MemoryBlock*> s_blocks;
	/** Grows with the number of blocks so a small app does not reserve 64 MB. */
	static VkDeviceSize s_nextBlockSize = 8u << 20;
	static const VkDeviceSize kBlockMax = 64u << 20;

	static void DestroyBlock (MemoryBlock* block) {

		State& state = S ();

		if (block->memory != VK_NULL_HANDLE && state.device != VK_NULL_HANDLE) {

			state.dev.FreeMemory (state.device, block->memory, NULL);

		}

		state.memory.reserved -= block->size;
		delete block;

	}

	/** Inserts a freed range and merges it with the neighbours it touches. */
	static void ReturnRange (MemoryBlock* block, VkDeviceSize offset, VkDeviceSize size) {

		size_t index = 0;

		while (index < block->freeRanges.size () && block->freeRanges[index].offset < offset) index++;

		MemoryBlock::Range range;
		range.offset = offset;
		range.size = size;

		block->freeRanges.insert (block->freeRanges.begin () + (long)index, range);

		// Merge with the following range first, then with the previous one.
		if (index + 1 < block->freeRanges.size ()
			&& block->freeRanges[index].offset + block->freeRanges[index].size == block->freeRanges[index + 1].offset) {

			block->freeRanges[index].size += block->freeRanges[index + 1].size;
			block->freeRanges.erase (block->freeRanges.begin () + (long)index + 1);

		}

		if (index > 0
			&& block->freeRanges[index - 1].offset + block->freeRanges[index - 1].size == block->freeRanges[index].offset) {

			block->freeRanges[index - 1].size += block->freeRanges[index].size;
			block->freeRanges.erase (block->freeRanges.begin () + (long)index);

		}

	}

	static MemoryBlock* CreateBlock (uint32_t typeIndex, VkDeviceSize bytes) {

		State& state = S ();
		DeviceApi& dev = state.dev;

		VkMemoryAllocateInfo allocateInfo = {};
		allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocateInfo.allocationSize = bytes;
		allocateInfo.memoryTypeIndex = typeIndex;

		MemoryBlock* block = new MemoryBlock ();
		block->memory = VK_NULL_HANDLE;
		block->typeIndex = typeIndex;
		block->size = 0;
		block->live = 0;

		if (dev.AllocateMemory (state.device, &allocateInfo, NULL, &block->memory) != VK_SUCCESS) {

			delete block;
			return NULL;

		}

		block->size = bytes;

		MemoryBlock::Range whole;
		whole.offset = 0;
		whole.size = bytes;
		block->freeRanges.push_back (whole);

		state.memory.allocations++;
		state.memory.reserved += bytes;

		s_blocks.push_back (block);

		return block;

	}

	/**
	 * Carves image memory out of a block.
	 *
	 * @return false when even a fresh block could not be allocated; the caller
	 *         then fails the texture and reports it, it never aborts.
	 */
	static bool AllocateImageMemory (const VkMemoryRequirements& requirements, uint32_t typeIndex,
		MemoryBlock** outBlock, VkDeviceSize* outOffset, VkDeviceSize* outReserved) {

		const VkDeviceSize alignment = requirements.alignment > 256 ? requirements.alignment : 256;

		for (size_t i = 0; i < s_blocks.size (); ++i) {

			MemoryBlock* block = s_blocks[i];

			if (block->typeIndex != typeIndex) continue;

			for (size_t r = 0; r < block->freeRanges.size (); ++r) {

				VkDeviceSize offset = block->freeRanges[r].offset;
				VkDeviceSize aligned = (offset + alignment - 1) & ~(alignment - 1);
				VkDeviceSize padding = aligned - offset;
				VkDeviceSize available = block->freeRanges[r].size > padding ? block->freeRanges[r].size - padding : 0;

				if (available < requirements.size) continue;

				// Keep a usable gap: a remainder smaller than the alignment can never
				// hold another image, so it is handed out with this one instead.
				VkDeviceSize take = requirements.size;

				if (available - take < alignment) take = available;

				if (padding > 0) {

					block->freeRanges[r].offset += padding;
					block->freeRanges[r].size -= padding;

				}

				block->freeRanges[r].offset += take;
				block->freeRanges[r].size -= take;

				if (block->freeRanges[r].size == 0) {

					block->freeRanges.erase (block->freeRanges.begin () + (long)r);

				}

				block->live++;

				*outBlock = block;
				*outOffset = aligned;
				if (outReserved) *outReserved = take;
				return true;

			}

		}

		// Nothing fits: a new block, sized for the request but never above the cap.
		VkDeviceSize bytes = s_nextBlockSize;

		if (bytes < requirements.size + alignment) bytes = requirements.size + alignment;

		if (bytes > kBlockMax) bytes = requirements.size + alignment > kBlockMax ? requirements.size + alignment : kBlockMax;

		MemoryBlock* block = CreateBlock (typeIndex, bytes);

		if (!block) {

			SetError ("vkAllocateMemory(image block) failed");
			return false;

		}

		if (s_nextBlockSize < kBlockMax) s_nextBlockSize <<= 1;

		if (!AllocateImageMemory (requirements, typeIndex, outBlock, outOffset, outReserved)) {

			// A brand new block cannot fail for a request it was sized for; if it
			// does, say so instead of handing out memory the driver refused.
			SetError ("image suballocation failed inside a fresh block");
			return false;

		}

		return true;

	}

	/** Gives the range back; a block without a live image is destroyed. */
	static void ReleaseImageMemory (MemoryBlock* block, VkDeviceSize offset, VkDeviceSize size) {

		if (!block) return;

		block->live--;

		ReturnRange (block, offset, size);

		if (block->live > 0) return;

		for (size_t i = 0; i < s_blocks.size (); ++i) {

			if (s_blocks[i] != block) continue;

			s_blocks.erase (s_blocks.begin () + (long)i);
			break;

		}

		DestroyBlock (block);

	}

	/** Releases every block. Only valid with the device idle. */
	static void ReleaseImageBlocks () {

		for (size_t i = 0; i < s_blocks.size (); ++i) DestroyBlock (s_blocks[i]);

		s_blocks.clear ();
		s_nextBlockSize = 8u << 20;

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

	/**
	 * One upload channel per frame in flight: its own command buffer, its own
	 * staging ring, and a 'ready' flag that says the frame which last used it has
	 * finished.
	 *
	 * The previous design had a single channel and waited for its fence - and for
	 * the previous frame's fence - before recording the next batch. Both waits sat
	 * in the middle of the frame, which is the one place the CPU must not stop:
	 * "the previous frame has finished" is exactly the pipeline it is supposed to
	 * run ahead of. On a device whose driver reports its uploads slowly (or a
	 * phone whose writes to external storage stall a frame), that wait is several
	 * milliseconds spent doing nothing.
	 *
	 * It was not the cause of the 33 fps of November 22 - that was the swapchain
	 * being rebuilt on every present (see the SUBOPTIMAL note in device.cpp) - but
	 * it is the same class of mistake and it showed up in the stats as one wait per
	 * frame.
	 *
	 * With two slots, the fence BeginFrame already waits for is all the
	 * synchronization this path needs, and the copies are submitted on the same
	 * queue immediately before the frame's own commands, so the pipeline barrier at
	 * the end of the upload buffer orders them against that frame's draws.
	 */
	struct UploadSlot {

		VkCommandBuffer commandBuffer;
		VkBuffer staging;
		VkDeviceMemory stagingMemory;
		void* mapped;
		VkDeviceSize size;
		VkDeviceSize used;
		/** True once the frame that last used this slot finished, so the ring can be
		 *  overwritten. Set by BeginFrame right after its fence wait. */
		bool ready;

		UploadSlot () :
			commandBuffer (VK_NULL_HANDLE),
			staging (VK_NULL_HANDLE),
			stagingMemory (VK_NULL_HANDLE),
			mapped (NULL),
			size (0),
			used (0),
			ready (true) {}

	};

	static UploadSlot s_slots[MAX_FRAMES_IN_FLIGHT];

	static VkCommandPool s_uploadPool = VK_NULL_HANDLE;
	/** Only used by the synchronous path: an upload queued with no frame open (a
	 *  loader handing textures over) has nothing else to tell it the copy landed. */
	static VkFence s_uploadFence = VK_NULL_HANDLE;
	/** Fence waits give up instead of blocking forever: a hung copy would
	 *  otherwise freeze the loader with no message at all. */
	static const uint64_t kUploadTimeoutNs = 5000000000ull;

	/** The channel the current frame records into. */
	static int UploadSlotIndex () {

		uint32_t index = S ().frameIndex;

		return index < (uint32_t)MAX_FRAMES_IN_FLIGHT ? (int)index : 0;

	}

	/** Called from BeginFrame once this slot's fence signalled: the ring is free. */
	void BeginUploadFrame () {

		UploadSlot& slot = s_slots[UploadSlotIndex ()];

		// Rewind only when nothing is still queued: an upload made outside a frame
		// (the loader hands textures over before the first draw) owns the bytes at
		// the bottom of this ring until the next flush picks them up, and rewinding
		// would let the next allocation overwrite them.
		if (s_pending.empty ()) slot.used = 0;

		slot.ready = true;

	}

	/** True once nothing is reading this slot's ring any more. Outside a frame the
	 *  frame fence has to be waited for; inside one BeginFrame already did it. */
	static bool SlotReady (int index) {

		State& state = S ();

		UploadSlot& slot = s_slots[index];

		if (slot.ready) return true;

		if (state.device != VK_NULL_HANDLE && (size_t)index < state.inFlight.size ()
			&& state.inFlight[index] != VK_NULL_HANDLE) {

			VkResult result = state.dev.WaitForFences (state.device, 1, &state.inFlight[index], VK_TRUE, kUploadTimeoutNs);

			if (result != VK_SUCCESS) {

				state.upload.failures++;

				std::string message = "vkWaitForFences(upload slot) failed: ";
				message += ResultText (result);
				SetError (message.c_str ());
				return false;

			}

		}

		slot.used = 0;
		slot.ready = true;

		return true;

	}

	// Uploads copy through a ring buffer that lives in host visible memory, which
	// is plain RAM. It is kept small on purpose: a ring sized for a whole burst
	// parks hundreds of megabytes there for the rest of the session. One ring per
	// frame slot, so two frames of uploads can be in flight at once.
	static const VkDeviceSize kStagingRing = 8u << 20;

	static bool FlushPendingUploads (bool synchronous);
	static void ReleaseStaging (int index);
	static void ReleaseAllStaging ();

	static std::map<uint64_t, VkSampler> s_samplers;

	/** Frees the upload objects. Only safe while nothing is in flight: the device
	 *  teardown path waits for the device before it gets here. */
	static void ReleaseUpload () {

		State& state = S ();
		DeviceApi& dev = state.dev;

		if (state.device != VK_NULL_HANDLE) {

			if (s_uploadFence != VK_NULL_HANDLE) dev.DestroyFence (state.device, s_uploadFence, NULL);
			// Destroying the pool frees the command buffers allocated from it.
			if (s_uploadPool != VK_NULL_HANDLE) dev.DestroyCommandPool (state.device, s_uploadPool, NULL);

		}

		for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {

			s_slots[i].commandBuffer = VK_NULL_HANDLE;
			s_slots[i].ready = true;

		}

		s_uploadPool = VK_NULL_HANDLE;
		s_uploadFence = VK_NULL_HANDLE;

	}

	static bool EnsureUploadObjects () {

		State& state = S ();
		DeviceApi& dev = state.dev;

		if (state.device == VK_NULL_HANDLE) return false;

		bool complete = s_uploadPool != VK_NULL_HANDLE && s_uploadFence != VK_NULL_HANDLE;

		for (int i = 0; complete && i < MAX_FRAMES_IN_FLIGHT; ++i) {

			if (s_slots[i].commandBuffer == VK_NULL_HANDLE) complete = false;

		}

		// Every handle or none: a half created channel used to leave the pool
		// alive with a null fence, and the next flush re-used a stale handle.
		if (complete) return true;

		// The device is idle here in every path that gets this far (a rebuild waits
		// before touching the channel), so the old objects can go as a unit.
		ReleaseUpload ();

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
		allocateInfo.commandBufferCount = MAX_FRAMES_IN_FLIGHT;

		VkCommandBuffer buffers[MAX_FRAMES_IN_FLIGHT] = {};

		if (dev.AllocateCommandBuffers (state.device, &allocateInfo, buffers) != VK_SUCCESS) {

			SetError ("vkAllocateCommandBuffers(upload) failed");
			return false;

		}

		for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {

			s_slots[i].commandBuffer = buffers[i];
			s_slots[i].ready = true;

		}

		VkFenceCreateInfo fenceInfo = {};
		fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

		if (dev.CreateFence (state.device, &fenceInfo, NULL, &s_uploadFence) != VK_SUCCESS) {

			SetError ("vkCreateFence(upload) failed");
			return false;

		}

		return true;

	}

	/**
	 * Waits for the upload fence, which only the synchronous path uses.
	 *
	 * A failed wait leaves the fence alone on purpose: the GPU may still be reading
	 * the staging ring, and reusing it would corrupt the copy that is already
	 * queued. The error carries the place and the result code, so a timeout and a
	 * lost device do not look the same in the log.
	 */
	static bool WaitUploadFence (const char* where) {

		State& state = S ();

		if (state.device == VK_NULL_HANDLE || s_uploadFence == VK_NULL_HANDLE) return true;

		state.upload.waits++;

		VkResult result = state.dev.WaitForFences (state.device, 1, &s_uploadFence, VK_TRUE, kUploadTimeoutNs);

		if (result != VK_SUCCESS) {

			state.upload.failures++;

			std::string message = "vkWaitForFences(upload) failed in ";
			message += where;
			message += ": ";
			message += ResultText (result);

			SetError (message.c_str ());
			return false;

		}

		state.dev.ResetFences (state.device, 1, &s_uploadFence);

		return true;

	}

	/** Gives one slot's ring back to the driver. The next upload creates it again. */
	static void ReleaseStaging (int index) {

		State& state = S ();

		UploadSlot& slot = s_slots[index];

		if (state.device != VK_NULL_HANDLE) {

			if (slot.mapped != NULL) state.dev.UnmapMemory (state.device, slot.stagingMemory);
			if (slot.stagingMemory != VK_NULL_HANDLE) state.dev.FreeMemory (state.device, slot.stagingMemory, NULL);
			if (slot.staging != VK_NULL_HANDLE) state.dev.DestroyBuffer (state.device, slot.staging, NULL);

		}

		if (state.memory.staging >= slot.size) state.memory.staging -= slot.size;
		else state.memory.staging = 0;

		slot.staging = VK_NULL_HANDLE;
		slot.stagingMemory = VK_NULL_HANDLE;
		slot.mapped = NULL;
		slot.size = 0;
		slot.used = 0;

	}

	static void ReleaseAllStaging () {

		for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) ReleaseStaging (i);

	}

	/**
	 * Reserves staging space in this frame's ring.
	 *
	 * Nothing is waited for here. The ring belongs to one frame slot and
	 * BeginFrame only marks it writable again once that slot's fence signalled, so
	 * the GPU is finished with it by construction. A ring that is too small for a
	 * whole frame of uploads grows instead of being flushed twice, because a second
	 * flush of the same ring inside one frame would put two readers on those bytes.
	 */
	static bool StagingAlloc (VkDeviceSize bytes, void** data, VkDeviceSize* offset) {

		State& state = S ();
		DeviceApi& dev = state.dev;

		int index = UploadSlotIndex ();

		if (!SlotReady (index)) return false;

		UploadSlot& slot = s_slots[index];

		// A ring that grew for one large image goes back to the default size once
		// the GPU stopped reading it, instead of staying resident for the session.
		if (slot.size > kStagingRing && slot.used == 0) ReleaseStaging (index);

		if (slot.used + bytes > slot.size) {

			VkDeviceSize size = slot.size > 0 ? slot.size : kStagingRing;

			while (size < slot.used + bytes) size <<= 1;

			VkBuffer previousBuffer = slot.staging;
			VkDeviceMemory previousMemory = slot.stagingMemory;
			void* previousMapped = slot.mapped;
			VkDeviceSize carried = slot.used;

			slot.staging = VK_NULL_HANDLE;
			slot.stagingMemory = VK_NULL_HANDLE;
			slot.mapped = NULL;
			slot.size = 0;

			VkBufferCreateInfo bufferInfo = {};
			bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
			bufferInfo.size = size;
			bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
			bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

			if (dev.CreateBuffer (state.device, &bufferInfo, NULL, &slot.staging) != VK_SUCCESS) {

				SetError ("vkCreateBuffer(staging) failed");
				return false;

			}

			VkMemoryRequirements requirements;
			dev.GetBufferMemoryRequirements (state.device, slot.staging, &requirements);

			uint32_t type = 0;

			if (!FindMemoryType (requirements.memoryTypeBits,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &type)) {

				SetError ("no host visible memory for the staging buffer");
				dev.DestroyBuffer (state.device, slot.staging, NULL);
				slot.staging = VK_NULL_HANDLE;
				return false;

			}

			VkMemoryAllocateInfo allocateInfo = {};
			allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
			allocateInfo.allocationSize = requirements.size;
			allocateInfo.memoryTypeIndex = type;

			if (dev.AllocateMemory (state.device, &allocateInfo, NULL, &slot.stagingMemory) != VK_SUCCESS
				|| dev.BindBufferMemory (state.device, slot.staging, slot.stagingMemory, 0) != VK_SUCCESS) {

				SetError ("staging buffer allocation failed");

				if (slot.stagingMemory) dev.FreeMemory (state.device, slot.stagingMemory, NULL);

				dev.DestroyBuffer (state.device, slot.staging, NULL);

				slot.staging = VK_NULL_HANDLE;
				slot.stagingMemory = VK_NULL_HANDLE;

				return false;

			}

			if (dev.MapMemory (state.device, slot.stagingMemory, 0, size, 0, &slot.mapped) != VK_SUCCESS) {

				SetError ("vkMapMemory(staging) failed");

				dev.FreeMemory (state.device, slot.stagingMemory, NULL);
				dev.DestroyBuffer (state.device, slot.staging, NULL);

				slot.staging = VK_NULL_HANDLE;
				slot.stagingMemory = VK_NULL_HANDLE;
				slot.mapped = NULL;

				return false;

			}

			// Queued uploads reference the staging buffer by offset, so the pixels
			// already written have to move with it.
			if (carried > 0 && previousMapped != NULL) {

				memcpy (slot.mapped, previousMapped, (size_t)carried);

			}

			if (previousBuffer != VK_NULL_HANDLE) {

				if (previousMapped != NULL) dev.UnmapMemory (state.device, previousMemory);
				dev.FreeMemory (state.device, previousMemory, NULL);
				dev.DestroyBuffer (state.device, previousBuffer, NULL);

			}

			slot.size = size;
			state.memory.staging = size;

		}

		*offset = slot.used;
		*data = (unsigned char*)slot.mapped + slot.used;

		slot.used += (bytes + 15) & ~(VkDeviceSize)15;

		return true;

	}

	/**
	 * Queues one upload. The pixels are copied into staging now; the GPU copy is
	 * recorded by FlushPendingUploads.
	 *
	 * @param rows NULL queues only the layout transition of a fresh image.
	 */
	static bool QueueUpload (VkImage image, bool initialized, const void* rows, int width, int height,
		uint32_t mipLevels, bool swizzleBgra, bool unpremultiply, int bytesPerPixel) {

		VkDeviceSize bytes = rows ? (VkDeviceSize)width * (VkDeviceSize)height * (VkDeviceSize)bytesPerPixel : 0;
		VkDeviceSize offset = 0;

		if (bytes > 0) {

			void* data = NULL;

			if (!StagingAlloc (bytes, &data, &offset) || data == NULL) return false;

			if (bytesPerPixel == 4 && (swizzleBgra || unpremultiply)) {

				// Format normalisation happens here, once, so the renderer does not
				// have to know about byte order.
				unsigned char* target = (unsigned char*)data;
				const unsigned char* source = (const unsigned char*)rows;
				const uint32_t pixels = (uint32_t)width * (uint32_t)height;

				for (uint32_t i = 0; i < pixels; ++i) {

					const unsigned char* from = source + (size_t)i * 4u;
					unsigned char* to = target + (size_t)i * 4u;

					unsigned char red = swizzleBgra ? from[2] : from[0];
					const unsigned char green = from[1];
					unsigned char blue = swizzleBgra ? from[0] : from[2];
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
	 * The copies go into the current frame slot's own command buffer and are
	 * submitted on the graphics queue immediately, before the frame's commands. The
	 * queue order plus the pipeline barrier at the end of the buffer is what makes
	 * them visible to the draws of that frame, so no semaphore is handed to the
	 * frame submit and the CPU never waits here.
	 *
	 * @param synchronous the caller has no frame in flight to hang the copy on (a
	 *        loader handing textures over): the submit is then waited for, because
	 *        nothing else would tell the caller the pixels landed.
	 */
	static bool FlushPendingUploads (bool synchronous) {

		State& state = S ();
		DeviceApi& dev = state.dev;

		if (s_pending.empty ()) return true;

		double flushStart = PerfNow ();

		if (!EnsureUploadObjects ()) {

			state.waits.uploadFlush += (float)((PerfNow () - flushStart) * 1000.0);
			s_pending.clear ();
			return false;

		}

		int index = UploadSlotIndex ();

		if (!SlotReady (index)) {

			state.waits.uploadFlush += (float)((PerfNow () - flushStart) * 1000.0);
			s_pending.clear ();
			return false;

		}

		UploadSlot& slot = s_slots[index];

		VkCommandBuffer uploadBuffer = slot.commandBuffer;
		VkBuffer stagingBuffer = slot.staging;

		state.waits.uploadFlush += (float)((PerfNow () - flushStart) * 1000.0);

		flushStart = PerfNow ();

		dev.ResetCommandBuffer (uploadBuffer, 0);

		VkCommandBufferBeginInfo beginInfo = {};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

		if (dev.BeginCommandBuffer (uploadBuffer, &beginInfo) != VK_SUCCESS) {

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

			dev.CmdPipelineBarrier (uploadBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
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

				dev.CmdCopyBufferToImage (uploadBuffer, stagingBuffer, entry.image,
					VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

				state.upload.copies++;
				state.upload.bytes += (uint64_t)entry.width * (uint64_t)entry.height * 4ull;

			}

			if (entry.mipLevels > 1) {

				VkImageMemoryBarrier toSource = barrier;
				toSource.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				toSource.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
				toSource.subresourceRange.levelCount = 1;
				toSource.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				toSource.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

				dev.CmdPipelineBarrier (uploadBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
					0, 0, NULL, 0, NULL, 1, &toSource);

				int sourceWidth = entry.width;
				int sourceHeight = entry.height;

				for (uint32_t level = 1; level < entry.mipLevels; ++level) {

					VkImageMemoryBarrier toDestination = barrier;
					toDestination.subresourceRange.baseMipLevel = level;
					toDestination.subresourceRange.levelCount = 1;

					dev.CmdPipelineBarrier (uploadBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
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

					dev.CmdBlitImage (uploadBuffer, entry.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
						entry.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

					VkImageMemoryBarrier sourceToDestination = toSource;
					sourceToDestination.subresourceRange.baseMipLevel = level;
					sourceToDestination.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
					sourceToDestination.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

					dev.CmdPipelineBarrier (uploadBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
						0, 0, NULL, 0, NULL, 1, &sourceToDestination);

					if (sourceWidth > 1) sourceWidth /= 2;
					if (sourceHeight > 1) sourceHeight /= 2;

				}

				VkImageMemoryBarrier toRead = barrier;
				toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
				toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				toRead.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
				toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

				dev.CmdPipelineBarrier (uploadBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
					0, 0, NULL, 0, NULL, 1, &toRead);

			} else {

				VkImageMemoryBarrier toRead = barrier;
				toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

				dev.CmdPipelineBarrier (uploadBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
					0, 0, NULL, 0, NULL, 1, &toRead);

			}

		}

		s_pending.clear ();

		if (dev.EndCommandBuffer (uploadBuffer) != VK_SUCCESS) {

			SetError ("vkEndCommandBuffer(upload) failed");
			return false;

		}

		// One submit on the graphics queue, before the frame's own commands. A
		// synchronous batch (no frame open) gets a fence because nothing else would
		// report the copy; the frame path gets none, because the frame's fence
		// already covers everything submitted ahead of it on that queue.
		VkSubmitInfo submitInfo = {};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &uploadBuffer;

		if (synchronous) {

			if (s_uploadFence == VK_NULL_HANDLE) {

				VkFenceCreateInfo fenceInfo = {};
				fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

				if (dev.CreateFence (state.device, &fenceInfo, NULL, &s_uploadFence) != VK_SUCCESS) {

					SetError ("vkCreateFence(upload) failed");
					return false;

				}

			}

		}

		VkResult submitResult = dev.QueueSubmit (state.graphicsQueue, 1, &submitInfo,
			synchronous ? s_uploadFence : VK_NULL_HANDLE);

		if (submitResult != VK_SUCCESS) {

			std::string message = "vkQueueSubmit(upload) failed: ";
			message += ResultText (submitResult);
			SetError (message.c_str ());
			return false;

		}

		state.upload.flushes++;
		state.waits.uploadQueue += (float)((PerfNow () - flushStart) * 1000.0);

		if (synchronous) {

			if (WaitUploadFence ("sync")) return true;

			return false;

		}

		return true;

	}

	bool FlushTextureUploads (bool haveFrame) {

		// With a frame being submitted the copy is part of that frame's queue
		// traffic and its fence covers it; without one it has to be waited for, or
		// the caller would hand a texture over before its pixels landed.
		return FlushPendingUploads (!haveFrame);

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

		// The device is idle at this point (the caller waited), so the fence wait
		// cannot block and the upload objects can go away as a unit.
		if (state.device != VK_NULL_HANDLE) WaitUploadFence ("destroytex");

		for (std::map<uint64_t, VkSampler>::iterator it = s_samplers.begin (); it != s_samplers.end (); ++it) {

			if (state.device != VK_NULL_HANDLE) dev.DestroySampler (state.device, it->second, NULL);

		}

		s_samplers.clear ();

		ReleaseAllStaging ();

		// Every texture is gone by now, so the remaining blocks are empty. This is a
		// safety net: a block with no live image destroys itself in ReleaseImageMemory.
		ReleaseImageBlocks ();

		// One place for the pool, the command buffer, the fence and the semaphore:
		// a device rebuild that only half released them was the source of the
		// stale fence handle that used to be waited for forever.
		ReleaseUpload ();

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
			if (texture->image != VK_NULL_HANDLE) dev.DestroyImage (state.device, texture->image, NULL);

			// The image is gone before its memory is handed back: a block that is
			// destroyed while an image still points into it is a driver fault.
			ReleaseImageMemory (texture->block, texture->offset, texture->reserved);

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

		// A BGRA source gets a BGRA image: the shader cannot tell the difference, and
		// the copy path then never has to touch a pixel (video frames are the case
		// that used to pay for this on every frame).
		VkFormat format = index8 ? VK_FORMAT_R8_UNORM : (bgra ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM);

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
		texture->format = format;
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

		if (!AllocateImageMemory (requirements, memoryType, &texture->block, &texture->offset, &texture->reserved)) {

			SetError ("could not suballocate memory for a texture");
			DiscardTexture (texture);
			return 0;

		}

		texture->memory = texture->block->memory;
		texture->allocation = requirements.size;
		state.memory.textures += requirements.size;
		state.memory.textureCount++;

		if (dev.BindImageMemory (state.device, texture->image, texture->memory, texture->offset) != VK_SUCCESS) {

			SetError ("vkBindImageMemory failed");
			DiscardTexture (texture);
			return 0;

		}

		// The image format already matches a BGRA source, so only an unpremultiply
		// pass is left (and only when the caller asked for one).
		if (!QueueUpload (texture->image, false, rgba, width, height, mipLevels, false, unpremultiply, bytesPerPixel)) {

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

		bool swizzle = bgra != (texture->format == VK_FORMAT_B8G8R8A8_UNORM);

		if (!QueueUpload (texture->image, true, rgba, texture->width, texture->height, 1, swizzle, unpremultiply, texture->bytesPerPixel)) {

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
		if (texture->image != VK_NULL_HANDLE) dev.DestroyImage (state.device, texture->image, NULL);

		ReleaseImageMemory (texture->block, texture->offset, texture->reserved);

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
		texture->format = state.swapchainFormat;
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

		if (!AllocateImageMemory (requirements, memoryType, &texture->block, &texture->offset, &texture->reserved)) {

			SetError ("could not suballocate memory for a render target");
			DiscardTexture (texture);
			return 0;

		}

		texture->memory = texture->block->memory;
		texture->allocation = requirements.size;
		state.memory.textures += requirements.size;
		state.memory.textureCount++;

		if (dev.BindImageMemory (state.device, texture->image, texture->memory, texture->offset) != VK_SUCCESS) {

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
