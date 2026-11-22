#include "internal.h"

#if defined(LIME_VULKAN_ANDROID)

#include <android/native_window.h>

namespace vulkan {

	// Provided by lime's SDL3 window (SDLWindow.cpp). Weak on purpose: the standalone
	// library build has no lime to ask, and a missing hook must not break the link.
	extern "C" void* lime_vulkan_android_window (void) __attribute__ ((weak));

	// Same source, kept for the caller that wants to know whether OpenGL can still draw:
	// declared here only so this file documents the contract it relies on.
	extern "C" int lime_vulkan_android_release_gl (void) __attribute__ ((weak));

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

		// The registered handle can be older than the surface the activity is showing right
		// now (a resume that recreated it, a rotation, a display cutout change): ask lime for
		// the live ANativeWindow and prefer that. A destroyed ANativeWindow is the difference
		// between "Vulkan starts" and VK_ERROR_UNKNOWN out of vkCreateAndroidSurfaceKHR.
		if (lime_vulkan_android_window) {

			void* live = lime_vulkan_android_window ();

			if (live && live != window) {

				Log ("using the live ANativeWindow (%p) instead of the registered one (%p)", live, window);
				window = live;
				SetAndroidWindow (live);
				MarkAndroidWindowApplied (live);
				S ().window = live;

			}

		}

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

			// The driver's verdict is the only thing that tells a stale window

			// (VK_ERROR_UNKNOWN) apart from a missing extension or an incompatible driver,
			// and the enum names do not cover every value, so the number goes in too.
			std::string message = "vkCreateAndroidSurfaceKHR failed: ";
			message += ResultText (result);
			message += " (";
			message += std::to_string ((int)result);
			message += ")";
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
