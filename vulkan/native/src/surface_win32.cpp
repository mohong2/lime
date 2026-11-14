#include "internal.h"

#if defined(LIME_VULKAN_WINDOWS)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace vulkan {

	const char* PlatformName () {

		return "win32";

	}

	bool CreateSurfaceExtensions (std::vector<const char*>& extensions) {

		extensions.push_back ("VK_KHR_surface");
		extensions.push_back ("VK_KHR_win32_surface");
		return true;

	}

	bool CreatePlatformSurface (VkInstance instance, void* window, VkSurfaceKHR* surface) {

		Api& api = S ().api;

		if (!api.CreateWin32SurfaceKHR) {

			SetError ("vkCreateWin32SurfaceKHR unavailable (instance extension missing?)");
			return false;

		}

		VkWin32SurfaceCreateInfoKHR info = {};
		info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
		info.hinstance = GetModuleHandleA (NULL);
		info.hwnd = (HWND)window;

		VkResult result = api.CreateWin32SurfaceKHR (instance, &info, NULL, surface);

		if (result != VK_SUCCESS) {

			std::string message = "vkCreateWin32SurfaceKHR failed: ";
			message += ResultText (result);
			SetError (message.c_str ());
			return false;

		}

		return true;

	}

	// Hidden window for the self test: pure Win32, no UI framework involved.
	static const char* kHiddenClass = "limeVulkanSelfTest";

	static LRESULT CALLBACK HiddenProc (HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {

		return DefWindowProcA (hwnd, message, wparam, lparam);

	}

	void* CreateHiddenWindow (int width, int height) {

		static bool registered = false;

		HINSTANCE instance = GetModuleHandleA (NULL);

		if (!registered) {

			WNDCLASSA windowClass = {};
			windowClass.lpfnWndProc = HiddenProc;
			windowClass.hInstance = instance;
			windowClass.lpszClassName = kHiddenClass;

			if (!RegisterClassA (&windowClass)) return 0;

			registered = true;

		}

		HWND hwnd = CreateWindowExA (0, kHiddenClass, "lime-vulkan self test", WS_POPUP,
			0, 0, width, height, NULL, NULL, instance, NULL);

		return (void*)hwnd;

	}

	void DestroyHiddenWindow (void* window) {

		if (window) DestroyWindow ((HWND)window);

	}

}

#endif
