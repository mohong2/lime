#include "internal.h"

#include <algorithm>
#include <cstdlib>

// Frame timings.
//
// Two numbers per frame, both reported as percentiles over a rolling window:
//
//   * cpu: wall clock from the start of BeginFrame to the end of EndFrame, which
//     is everything the renderer costs the game thread (quad writing, command
//     recording, submit, present). This is the number a CPU side change moves;
//   * gpu: VK_QUERY_TYPE_TIMESTAMP around the submitted work. It is read two
//     frames late, when that slot's fence signals in BeginFrame anyway, so the
//     CPU never waits for a value the current frame does not need.
//
// Percentiles, not a mean: the mean hides exactly the stalls a player feels (one
// 60 ms hitch among 300 frames of 2 ms). The ring keeps the last
// FramePerf::HISTORY frames.
//
// A queue family that reports no timestamp support leaves the GPU numbers at zero
// with 'gpuSupported=0' in the stats text, instead of printing a fake 0.00 ms.

namespace vulkan {

#if defined(LIME_VULKAN_WINDOWS)

	#include <windows.h>

	double PerfNow () {

		static LARGE_INTEGER frequency;
		static bool started = false;

		if (!started) {

			QueryPerformanceFrequency (&frequency);
			started = true;

		}

		LARGE_INTEGER counter;
		QueryPerformanceCounter (&counter);

		return (double)counter.QuadPart / (double)frequency.QuadPart;

	}

#else

	#include <chrono>

	double PerfNow () {

		return (double)std::chrono::duration_cast<std::chrono::nanoseconds> (
			std::chrono::steady_clock::now ().time_since_epoch ()).count () / 1.0e9;

	}

#endif

	FramePerf::FramePerf () :
		next (0),
		count (0),
		frames (0),
		frameStart (0.0),
		queries (VK_NULL_HANDLE),
		timestamps (false),
		period (1.0f) {

		for (int i = 0; i < HISTORY; ++i) {

			cpu[i] = 0.0f;
			gpu[i] = -1.0f;

		}

		for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {

			pendingCpu[i] = 0.0f;
			pendingCpuValid[i] = false;

		}

	}

	/** Two timestamps per frame slot: start and end of the submitted work. */
	bool CreatePerfQueries () {

		State& state = S ();
		DeviceApi& dev = state.dev;

		DestroyPerfQueries ();

		if (state.device == VK_NULL_HANDLE) return false;

		// Timestamp support is a queue family property, not a device one: a family
		// may report zero valid bits, and a query written there silently reads 0.
		uint32_t familyCount = 0;
		state.api.GetPhysicalDeviceQueueFamilyProperties (state.physicalDevice, &familyCount, NULL);

		if (familyCount == 0) return false;

		std::vector<VkQueueFamilyProperties> families (familyCount);
		state.api.GetPhysicalDeviceQueueFamilyProperties (state.physicalDevice, &familyCount, families.data ());

		if (state.graphicsFamily >= families.size ()) return false;

		// A driver with slow or wrong timestamps can be told to skip them entirely;
		// the stats text then reports gpuSupported=0 instead of a fake number.
		if (getenv ("SEIUN_VULKAN_NO_GPUTIME") != NULL) {

			state.perf.timestamps = false;
			return false;

		}

		// Not a failure: the stats text reports the GPU column as unavailable.
		if (families[state.graphicsFamily].timestampValidBits == 0) return false;

		VkPhysicalDeviceProperties properties;
		state.api.GetPhysicalDeviceProperties (state.physicalDevice, &properties);

		state.perf.period = properties.limits.timestampPeriod;

		if (state.perf.period <= 0.0f) state.perf.period = 1.0f;

		VkQueryPoolCreateInfo poolInfo = {};
		poolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
		poolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
		poolInfo.queryCount = MAX_FRAMES_IN_FLIGHT * 2;

		if (dev.CreateQueryPool (state.device, &poolInfo, NULL, &state.perf.queries) != VK_SUCCESS) {

			state.perf.queries = VK_NULL_HANDLE;
			return false;

		}

		state.perf.timestamps = true;
		return true;

	}

	void DestroyPerfQueries () {

		State& state = S ();

		if (state.perf.queries != VK_NULL_HANDLE && state.device != VK_NULL_HANDLE
			&& state.dev.DestroyQueryPool) {

			state.dev.DestroyQueryPool (state.device, state.perf.queries, NULL);

		}

		state.perf.queries = VK_NULL_HANDLE;
		state.perf.timestamps = false;

	}

	void PerfBeginFrame (VkCommandBuffer commandBuffer, uint32_t slot) {

		State& state = S ();
		FramePerf& perf = state.perf;

		perf.frameStart = PerfNow ();

		if (!perf.timestamps || commandBuffer == VK_NULL_HANDLE) return;

		// Resetting this slot's two queries before rewriting them is what stops the
		// next read from returning a value from an older frame.
		state.dev.CmdResetQueryPool (commandBuffer, perf.queries, slot * 2, 2);
		state.dev.CmdWriteTimestamp (commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, perf.queries, slot * 2);

	}

	void PerfEndFrame (VkCommandBuffer commandBuffer, uint32_t slot) {

		State& state = S ();

		if (!state.perf.timestamps || commandBuffer == VK_NULL_HANDLE) return;

		state.dev.CmdWriteTimestamp (commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, state.perf.queries, slot * 2 + 1);

	}

	void PerfCloseFrame (uint32_t slot) {

		State& state = S ();
		FramePerf& perf = state.perf;

		if (slot >= (uint32_t)MAX_FRAMES_IN_FLIGHT) return;

		perf.pendingCpu[slot] = (float)((PerfNow () - perf.frameStart) * 1000.0);
		perf.pendingCpuValid[slot] = true;

	}

	/**
	 * Called from BeginFrame once the fence of this slot signalled, which means the
	 * frame that last used it has finished and its timestamps can be read.
	 */
	void PerfCollectFrame (uint32_t slot) {

		State& state = S ();
		FramePerf& perf = state.perf;

		if (slot >= (uint32_t)MAX_FRAMES_IN_FLIGHT) return;
		if (!perf.pendingCpuValid[slot]) return;

		float gpuMs = -1.0f;

		if (perf.timestamps && perf.queries != VK_NULL_HANDLE && state.dev.GetQueryPoolResults) {

			uint64_t stamps[2] = { 0, 0 };

			VkResult result = state.dev.GetQueryPoolResults (state.device, perf.queries, slot * 2, 2,
				sizeof (stamps), stamps, sizeof (uint64_t), VK_QUERY_RESULT_64_BIT);

			if (result == VK_SUCCESS && stamps[1] >= stamps[0]) {

				gpuMs = (float)((double)(stamps[1] - stamps[0]) * (double)perf.period / 1000000.0);

			}

		}

		uint32_t index = perf.next;

		perf.cpu[index] = perf.pendingCpu[slot];
		perf.gpu[index] = gpuMs;

		perf.next = (perf.next + 1) % FramePerf::HISTORY;

		if (perf.count < (uint32_t)FramePerf::HISTORY) perf.count++;

		perf.frames++;
		perf.pendingCpuValid[slot] = false;

	}

	static float Percentile (const float* values, uint32_t count, float percentile) {

		if (count == 0) return 0.0f;

		std::vector<float> sorted (values, values + count);

		std::sort (sorted.begin (), sorted.end ());

		// Nearest rank, clamped: with 240 samples p95 is one real frame, not a mix of
		// a hitch and a normal frame.
		uint32_t index = (uint32_t)(percentile * (float)(count - 1) + 0.5f);

		if (index >= count) index = count - 1;

		return sorted[index];

	}

	int PerfText (char* out, int capacity) {

		State& state = S ();
		FramePerf& perf = state.perf;
		Batch& batch = state.batch;

		static float cpu[FramePerf::HISTORY];
		static float gpu[FramePerf::HISTORY];

		uint32_t gpuCount = 0;

		for (uint32_t i = 0; i < perf.count; ++i) {

			cpu[i] = perf.cpu[i];

			// A GPU sample that never arrived stays out of the percentile: a zero
			// would read as a very fast frame.
			if (perf.gpu[i] >= 0.0f) gpu[gpuCount++] = perf.gpu[i];

		}

		float cpuP50 = Percentile (cpu, perf.count, 0.50f);
		float cpuP95 = Percentile (cpu, perf.count, 0.95f);
		float cpuP99 = Percentile (cpu, perf.count, 0.99f);
		float cpuMax = Percentile (cpu, perf.count, 1.00f);

		float gpuP50 = Percentile (gpu, gpuCount, 0.50f);
		float gpuP95 = Percentile (gpu, gpuCount, 0.95f);
		float gpuP99 = Percentile (gpu, gpuCount, 0.99f);

		char text[1200];

		int written = snprintf (text, sizeof (text),
			"perfFrames=%u\n"
			"cpuP50=%.2f\ncpuP95=%.2f\ncpuP99=%.2f\ncpuMax=%.2f\n"
			"gpuP50=%.2f\ngpuP95=%.2f\ngpuP99=%.2f\n"
			"gpuSupported=%d\n"
			"uploadMB=%.1f\nuploadCopies=%llu\nuploadFlushes=%llu\nuploadWaits=%llu\nuploadFailures=%llu\n"
			"bindTextures=%u\nbindPipelines=%u\nbindScissors=%u\n",
			perf.frames,
			cpuP50, cpuP95, cpuP99, cpuMax,
			gpuP50, gpuP95, gpuP99,
			perf.timestamps ? 1 : 0,
			(double)state.upload.bytes / 1048576.0,
			(unsigned long long)state.upload.copies,
			(unsigned long long)state.upload.flushes,
			(unsigned long long)state.upload.waits,
			(unsigned long long)state.upload.failures,
			batch.bindTextures, batch.bindPipelines, batch.bindScissors);

		if (written < 0) return 0;
		if (written >= (int)sizeof (text)) written = (int)sizeof (text) - 1;

		if (out == NULL || capacity <= 0) return written;

		int copy = written < capacity - 1 ? written : capacity - 1;

		memcpy (out, text, (size_t)copy);
		out[copy] = 0;

		return copy;

	}

}
