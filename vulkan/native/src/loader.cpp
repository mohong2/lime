#include "internal.h"

#include <algorithm>

#if defined(LIME_VULKAN_WINDOWS)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace vulkan {

	static void* s_library = 0;
	static PFN_vkGetInstanceProcAddr s_gipa = 0;
	static uint32_t s_loaderVersion = VK_API_VERSION_1_0;
	static bool s_loadAttempted = false;
	static bool s_loadFailed = false;
	static bool s_verbose = false;

	State::State () {

		api.Clear ();
		dev.Clear ();

		options.deviceIndex = -1;
		options.presentMode = 0;
		options.enableValidation = 0;
		options.verbose = 0;
		options.fallbackDialog = 1;

		instance = VK_NULL_HANDLE;
		physicalDevice = VK_NULL_HANDLE;
		device = VK_NULL_HANDLE;
		graphicsQueue = VK_NULL_HANDLE;
		presentQueue = VK_NULL_HANDLE;
		graphicsFamily = 0;
		presentFamily = 0;
		surface = VK_NULL_HANDLE;
		window = 0;
		swapchain = VK_NULL_HANDLE;
		swapchainFormat = VK_FORMAT_UNDEFINED;
		swapchainColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		extent.width = 0;
		extent.height = 0;
		renderPass = VK_NULL_HANDLE;
		pipelineLayout = VK_NULL_HANDLE;
		pipeline = VK_NULL_HANDLE;
		vertexBuffer = VK_NULL_HANDLE;
		vertexMemory = VK_NULL_HANDLE;
		commandPool = VK_NULL_HANDLE;
		frameCommandBuffer = VK_NULL_HANDLE;
		currentImageIndex = 0;
		inFrame = false;
		nextTextureHandle = 1;
		nextProgramHandle = 0;

		for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {

			frameDescriptorPool[i] = VK_NULL_HANDLE;

		}
		whiteTexture = 0;
		frameIndex = 0;
		imageCount = 0;
		attached = false;
		resized = false;
		dialogEnabled = true;
		framesPresented = 0;
		pattern = LIME_VULKAN_PATTERN_TRIANGLE;

	}

	State& S () {

		static State state;
		return state;

	}

	void Log (const char* format, ...) {

		char buffer[2048];

		va_list args;
		va_start (args, format);
		vsnprintf (buffer, sizeof (buffer), format, args);
		va_end (args);

		if (!s_verbose && strncmp (buffer, "error", 5) != 0) return;

		printf ("[lime-vulkan] %s\n", buffer);

		#if defined(LIME_VULKAN_WINDOWS)
		OutputDebugStringA ("[lime-vulkan] ");
		OutputDebugStringA (buffer);
		OutputDebugStringA ("\n");
		#endif

	}

	void SetError (const char* message) {

		S ().lastError = message ? message : "";
		Log ("error: %s", S ().lastError.c_str ());

	}

	static void* OpenLibrary () {

		#if defined(LIME_VULKAN_WINDOWS)
		const char* candidates[] = { "vulkan-1.dll", "vulkan-1.dll" };
		for (int i = 0; i < 2; ++i) {

			HMODULE module = LoadLibraryA (candidates[i]);
			if (module) return (void*)module;

		}
		return 0;
		#else
		const char* candidates[] = { "libvulkan.so.1", "libvulkan.so" };
		for (int i = 0; i < 2; ++i) {

			void* handle = dlopen (candidates[i], RTLD_NOW | RTLD_LOCAL);
			if (handle) return handle;

		}
		return 0;
		#endif

	}

	static void* LibrarySymbol (void* library, const char* name) {

		#if defined(LIME_VULKAN_WINDOWS)
		return (void*)GetProcAddress ((HMODULE)library, name);
		#else
		return dlsym (library, name);
		#endif

	}

	bool LoadLoader (bool verbose) {

		if (verbose) s_verbose = true;
		if (s_loadAttempted) return !s_loadFailed;

		s_loadAttempted = true;
		s_library = OpenLibrary ();

		if (!s_library) {

			s_loadFailed = true;
			SetError ("no Vulkan loader (vulkan-1.dll / libvulkan.so.1 not found)");
			return false;

		}

		s_gipa = (PFN_vkGetInstanceProcAddr)LibrarySymbol (s_library, "vkGetInstanceProcAddr");

		if (!s_gipa) {

			s_loadFailed = true;
			SetError ("loader has no vkGetInstanceProcAddr");
			return false;

		}

		PFN_vkEnumerateInstanceVersion enumerateVersion = (PFN_vkEnumerateInstanceVersion)s_gipa (VK_NULL_HANDLE, "vkEnumerateInstanceVersion");

		if (enumerateVersion) {

			uint32_t version = VK_API_VERSION_1_0;
			if (enumerateVersion (&version) == VK_SUCCESS) s_loaderVersion = version;

		}

		Log ("loader ready (Vulkan %s)", VersionText (s_loaderVersion).c_str ());
		return true;

	}

	uint32_t LoaderVersion () {

		return s_loaderVersion;

	}

	static PFN_vkGetInstanceProcAddr GiPA () {

		return s_gipa;

	}

	bool LoadInstanceApi (VkInstance instance) {

		Api& api = S ().api;
		std::string missing;

		#define LIME_VULKAN_LOAD(fn) \
			api.fn = (PFN_vk##fn)GiPA () (instance, "vk" #fn); \
			if (!api.fn) { missing += " vk" #fn; }

		LIME_VULKAN_LOAD (DestroyInstance);
		LIME_VULKAN_LOAD (EnumeratePhysicalDevices);
		LIME_VULKAN_LOAD (GetPhysicalDeviceProperties);
		LIME_VULKAN_LOAD (GetPhysicalDeviceFeatures);
		LIME_VULKAN_LOAD (GetPhysicalDeviceQueueFamilyProperties);
		LIME_VULKAN_LOAD (GetPhysicalDeviceMemoryProperties);
		LIME_VULKAN_LOAD (GetPhysicalDeviceSurfaceCapabilitiesKHR);
		LIME_VULKAN_LOAD (GetPhysicalDeviceSurfaceFormatsKHR);
		LIME_VULKAN_LOAD (GetPhysicalDeviceSurfacePresentModesKHR);
		LIME_VULKAN_LOAD (GetPhysicalDeviceSurfaceSupportKHR);
		LIME_VULKAN_LOAD (EnumerateDeviceExtensionProperties);
		LIME_VULKAN_LOAD (DestroySurfaceKHR);
		LIME_VULKAN_LOAD (CreateDevice);
		LIME_VULKAN_LOAD (GetDeviceProcAddr);

		#undef LIME_VULKAN_LOAD

		#if defined(LIME_VULKAN_WINDOWS)
		api.CreateWin32SurfaceKHR = (PFN_vkCreateWin32SurfaceKHR)GiPA () (instance, "vkCreateWin32SurfaceKHR");
		if (!api.CreateWin32SurfaceKHR) missing += " vkCreateWin32SurfaceKHR";
		#elif defined(LIME_VULKAN_ANDROID)
		api.CreateAndroidSurfaceKHR = (PFN_vkCreateAndroidSurfaceKHR)GiPA () (instance, "vkCreateAndroidSurfaceKHR");
		if (!api.CreateAndroidSurfaceKHR) missing += " vkCreateAndroidSurfaceKHR";
		#else
		api.CreateXlibSurfaceKHR = (PFN_vkCreateXlibSurfaceKHR)GiPA () (instance, "vkCreateXlibSurfaceKHR");
		if (!api.CreateXlibSurfaceKHR) missing += " vkCreateXlibSurfaceKHR";
		#endif

		api.EnumerateInstanceExtensionProperties = (PFN_vkEnumerateInstanceExtensionProperties)GiPA () (VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
		api.EnumerateInstanceLayerProperties = (PFN_vkEnumerateInstanceLayerProperties)GiPA () (VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties");

		if (!missing.empty ()) {

			std::string message = "instance entry points missing:" + missing;
			SetError (message.c_str ());
			return false;

		}

		return true;

	}

	bool LoadDeviceApi (VkDevice device) {

		DeviceApi& dev = S ().dev;
		PFN_vkGetDeviceProcAddr gdpa = S ().api.GetDeviceProcAddr;

		if (!gdpa) {

			SetError ("vkGetDeviceProcAddr unavailable");
			return false;

		}

		std::string missing;

		#define LIME_VULKAN_LOAD_DEV(fn) \
			dev.fn = (PFN_vk##fn)gdpa (device, "vk" #fn); \
			if (!dev.fn) { missing += " vk" #fn; }

		LIME_VULKAN_LOAD_DEV (DestroyDevice);
		LIME_VULKAN_LOAD_DEV (DeviceWaitIdle);
		LIME_VULKAN_LOAD_DEV (GetDeviceQueue);
		LIME_VULKAN_LOAD_DEV (QueueSubmit);
		LIME_VULKAN_LOAD_DEV (QueueWaitIdle);
		LIME_VULKAN_LOAD_DEV (QueuePresentKHR);
		LIME_VULKAN_LOAD_DEV (CreateSwapchainKHR);
		LIME_VULKAN_LOAD_DEV (DestroySwapchainKHR);
		LIME_VULKAN_LOAD_DEV (GetSwapchainImagesKHR);
		LIME_VULKAN_LOAD_DEV (AcquireNextImageKHR);
		LIME_VULKAN_LOAD_DEV (CreateImageView);
		LIME_VULKAN_LOAD_DEV (DestroyImageView);
		LIME_VULKAN_LOAD_DEV (CreateRenderPass);
		LIME_VULKAN_LOAD_DEV (DestroyRenderPass);
		LIME_VULKAN_LOAD_DEV (CreateFramebuffer);
		LIME_VULKAN_LOAD_DEV (DestroyFramebuffer);
		LIME_VULKAN_LOAD_DEV (CreateShaderModule);
		LIME_VULKAN_LOAD_DEV (DestroyShaderModule);
		LIME_VULKAN_LOAD_DEV (CreatePipelineLayout);
		LIME_VULKAN_LOAD_DEV (DestroyPipelineLayout);
		LIME_VULKAN_LOAD_DEV (CreateGraphicsPipelines);
		LIME_VULKAN_LOAD_DEV (DestroyPipeline);
		LIME_VULKAN_LOAD_DEV (CreateBuffer);
		LIME_VULKAN_LOAD_DEV (DestroyBuffer);
		LIME_VULKAN_LOAD_DEV (GetBufferMemoryRequirements);
		LIME_VULKAN_LOAD_DEV (AllocateMemory);
		LIME_VULKAN_LOAD_DEV (FreeMemory);
		LIME_VULKAN_LOAD_DEV (BindBufferMemory);
		LIME_VULKAN_LOAD_DEV (MapMemory);
		LIME_VULKAN_LOAD_DEV (UnmapMemory);
		LIME_VULKAN_LOAD_DEV (CreateCommandPool);
		LIME_VULKAN_LOAD_DEV (DestroyCommandPool);
		LIME_VULKAN_LOAD_DEV (AllocateCommandBuffers);
		LIME_VULKAN_LOAD_DEV (BeginCommandBuffer);
		LIME_VULKAN_LOAD_DEV (EndCommandBuffer);
		LIME_VULKAN_LOAD_DEV (ResetCommandBuffer);
		LIME_VULKAN_LOAD_DEV (CmdBeginRenderPass);
		LIME_VULKAN_LOAD_DEV (CmdEndRenderPass);
		LIME_VULKAN_LOAD_DEV (CmdBindPipeline);
		LIME_VULKAN_LOAD_DEV (CmdBindVertexBuffers);
		LIME_VULKAN_LOAD_DEV (CmdPushConstants);
		LIME_VULKAN_LOAD_DEV (CmdDraw);
		LIME_VULKAN_LOAD_DEV (CmdSetViewport);
		LIME_VULKAN_LOAD_DEV (CmdSetScissor);
		LIME_VULKAN_LOAD_DEV (CmdBindIndexBuffer);
		LIME_VULKAN_LOAD_DEV (CmdDrawIndexed);
		LIME_VULKAN_LOAD_DEV (CmdPipelineBarrier);
		LIME_VULKAN_LOAD_DEV (CmdCopyBuffer);
		LIME_VULKAN_LOAD_DEV (CmdCopyBufferToImage);
		LIME_VULKAN_LOAD_DEV (CmdBlitImage);
		LIME_VULKAN_LOAD_DEV (CreateImage);
		LIME_VULKAN_LOAD_DEV (DestroyImage);
		LIME_VULKAN_LOAD_DEV (GetImageMemoryRequirements);
		LIME_VULKAN_LOAD_DEV (BindImageMemory);
		LIME_VULKAN_LOAD_DEV (CreateSampler);
		LIME_VULKAN_LOAD_DEV (DestroySampler);
		LIME_VULKAN_LOAD_DEV (CreateDescriptorSetLayout);
		LIME_VULKAN_LOAD_DEV (DestroyDescriptorSetLayout);
		LIME_VULKAN_LOAD_DEV (CreateDescriptorPool);
		LIME_VULKAN_LOAD_DEV (DestroyDescriptorPool);
		LIME_VULKAN_LOAD_DEV (AllocateDescriptorSets);
		LIME_VULKAN_LOAD_DEV (UpdateDescriptorSets);
		LIME_VULKAN_LOAD_DEV (FreeDescriptorSets);
		LIME_VULKAN_LOAD_DEV (CmdBindDescriptorSets);
		LIME_VULKAN_LOAD_DEV (ResetDescriptorPool);
		LIME_VULKAN_LOAD_DEV (CreateSemaphore);
		LIME_VULKAN_LOAD_DEV (DestroySemaphore);
		LIME_VULKAN_LOAD_DEV (CreateFence);
		LIME_VULKAN_LOAD_DEV (DestroyFence);
		LIME_VULKAN_LOAD_DEV (WaitForFences);
		LIME_VULKAN_LOAD_DEV (ResetFences);
		LIME_VULKAN_LOAD_DEV (FreeCommandBuffers);

		#undef LIME_VULKAN_LOAD_DEV

		if (!missing.empty ()) {

			std::string message = "device entry points missing:" + missing;
			SetError (message.c_str ());
			return false;

		}

		return true;

	}

	std::string VersionText (uint32_t version) {

		char buffer[64];
		snprintf (buffer, sizeof (buffer), "%u.%u.%u", VK_API_VERSION_MAJOR (version), VK_API_VERSION_MINOR (version), VK_API_VERSION_PATCH (version));
		return std::string (buffer);

	}

	const char* DeviceTypeText (VkPhysicalDeviceType type) {

		switch (type) {
			case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
			case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
			case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
			case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
			default: return "other";
		}

	}

	const char* PresentModeText (VkPresentModeKHR mode) {

		switch (mode) {
			case VK_PRESENT_MODE_IMMEDIATE_KHR: return "immediate";
			case VK_PRESENT_MODE_MAILBOX_KHR: return "mailbox";
			case VK_PRESENT_MODE_FIFO_KHR: return "fifo";
			case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "fifo_relaxed";
			default: return "other";
		}

	}

	const char* ResultText (VkResult result) {

		switch (result) {
			case VK_SUCCESS: return "VK_SUCCESS";
			case VK_NOT_READY: return "VK_NOT_READY";
			case VK_TIMEOUT: return "VK_TIMEOUT";
			case VK_INCOMPLETE: return "VK_INCOMPLETE";
			case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
			case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
			case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
			case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
			case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
			case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
			case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
			case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
			case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
			case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
			case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
			case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
			default: return "VK_ERROR_<other>";
		}

	}

	int DeviceScore (const VkPhysicalDeviceProperties& properties) {

		int score = 50;

		switch (properties.deviceType) {
			case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: score = 1000; break;
			case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: score = 500; break;
			case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: score = 300; break;
			case VK_PHYSICAL_DEVICE_TYPE_CPU: score = 100; break;
			default: break;
		}

		return score + (int)VK_API_VERSION_MINOR (properties.apiVersion);

	}

	static bool HasInstanceLayer (const char* name) {

		PFN_vkEnumerateInstanceLayerProperties enumerate = (PFN_vkEnumerateInstanceLayerProperties)GiPA () (VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties");
		if (!enumerate) return false;

		uint32_t count = 0;
		if (enumerate (&count, NULL) != VK_SUCCESS || count == 0) return false;

		std::vector<VkLayerProperties> layers (count);
		if (enumerate (&count, layers.data ()) != VK_SUCCESS) return false;

		for (uint32_t i = 0; i < count; ++i) {
			if (strcmp (layers[i].layerName, name) == 0) return true;
		}

		return false;

	}

	bool CreateInstance (bool validation, bool verbose, VkInstance* outInstance) {

		*outInstance = VK_NULL_HANDLE;

		if (!LoadLoader (verbose)) return false;

		std::vector<const char*> extensions;

		if (!CreateSurfaceExtensions (extensions)) return false;

		std::vector<const char*> layers;

		if (validation && HasInstanceLayer ("VK_LAYER_KHRONOS_validation")) {

			layers.push_back ("VK_LAYER_KHRONOS_validation");

		}

		VkApplicationInfo applicationInfo = {};
		applicationInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		applicationInfo.pApplicationName = "lime-vulkan";
		applicationInfo.applicationVersion = VK_MAKE_VERSION (0, 1, 0);
		applicationInfo.pEngineName = "lime-vulkan";
		applicationInfo.engineVersion = VK_MAKE_VERSION (0, 1, 0);
		applicationInfo.apiVersion = VK_API_VERSION_1_0;

		VkInstanceCreateInfo createInfo = {};
		createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		createInfo.pApplicationInfo = &applicationInfo;
		createInfo.enabledExtensionCount = (uint32_t)extensions.size ();
		createInfo.ppEnabledExtensionNames = extensions.data ();
		createInfo.enabledLayerCount = (uint32_t)layers.size ();
		createInfo.ppEnabledLayerNames = layers.data ();

		PFN_vkCreateInstance createInstance = (PFN_vkCreateInstance)GiPA () (VK_NULL_HANDLE, "vkCreateInstance");

		if (!createInstance) {

			SetError ("vkCreateInstance unavailable");
			return false;

		}

		VkResult result = createInstance (&createInfo, NULL, outInstance);

		if (result != VK_SUCCESS) {

			std::string message = "vkCreateInstance failed: ";
			message += ResultText (result);
			SetError (message.c_str ());
			return false;

		}

		S ().api.CreateInstance = createInstance;
		Log ("instance created (%u extension(s))", (unsigned int)extensions.size ());
		return true;

	}

}
