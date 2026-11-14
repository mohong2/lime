#include "internal.h"

#if defined(LIME_VULKAN_ANDROID)

#include <android/native_window.h>

namespace vulkan {

	// Android has no game window to discover: the surface comes from the
	// ANativeWindow the activity created, which the host registers through
	// lime_vulkan_set_android_window. Until it does, SelfWindow stays NULL and the
	// renderer reports "not available" instead of guessing.
	const char* PlatformName () {

		return "android";

	}

	bool CreateSurfaceExtensions (std::vector<const char*>& extensions) {

		extensions.push_back ("VK_KHR_surface");
		extensions.push_back ("VK_KHR_android_surface");
		return true;

	}

	bool CreatePlatformSurface (VkInstance instance, void* window, VkSurfaceKHR* surface) {

		Api& api = S ().api;

		// The host registers the window; a caller that has no handle may pass NULL.
		if (!window) window = SelfWindow ();

		if (!window) {

			SetError ("no ANativeWindow was supplied");
			return false;

		}

		if (!api.CreateAndroidSurfaceKHR) {

			SetError ("vkCreateAndroidSurfaceKHR unavailable (instance extension missing?)");
			return false;

		}

		VkAndroidSurfaceCreateInfoKHR info = {};
		info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
		info.window = (ANativeWindow*)window;

		VkResult result = api.CreateAndroidSurfaceKHR (instance, &info, NULL, surface);

		if (result != VK_SUCCESS) {

			std::string message = "vkCreateAndroidSurfaceKHR failed: ";
			message += ResultText (result);
			SetError (message.c_str ());
			return false;

		}

		return true;

	}

	void* CreateHiddenWindow (int width, int height) {

		// A hidden window would have to be a second Surface from the activity;
		// the self test does not run on Android.
		return 0;

	}

	void DestroyHiddenWindow (void* window) {

	}

}

#endif
