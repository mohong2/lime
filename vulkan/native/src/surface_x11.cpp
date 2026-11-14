#include "internal.h"

#if defined(LIME_VULKAN_LINUX)

#include <X11/Xlib.h>

#include <map>

namespace vulkan {

	// One display connection for the whole library: the surface, the window
	// lookups and the hidden window all come from here, and XOpenDisplay is
	// expensive enough that opening it per call would be visible.
	//
	// Not called 'Display': a function with that name at namespace scope hides
	// the X11 typedef for everything below it, and every later 'Display*' then
	// fails to compile on Linux.
	static Display* X11Display () {

		static Display* display = NULL;

		if (!display) display = XOpenDisplay (NULL);

		return display;

	}

	// Hidden windows created for the self test, so DestroyHiddenWindow can close
	// the right display connection.
	static std::map<void*, Display*> hiddenDisplays;

	const char* PlatformName () {

		return "x11";

	}

	bool CreateSurfaceExtensions (std::vector<const char*>& extensions) {

		extensions.push_back ("VK_KHR_surface");
		extensions.push_back ("VK_KHR_xlib_surface");
		return true;

	}

	bool CreatePlatformSurface (VkInstance instance, void* window, VkSurfaceKHR* surface) {

		Api& api = S ().api;

		if (!api.CreateXlibSurfaceKHR) {

			SetError ("vkCreateXlibSurfaceKHR unavailable (instance extension missing?)");
			return false;

		}

		Display* display = X11Display ();

		if (!display) {

			SetError ("XOpenDisplay failed (no DISPLAY?)");
			return false;

		}

		VkXlibSurfaceCreateInfoKHR info = {};
		info.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
		info.dpy = display;
		info.window = (::Window)(uintptr_t)window;

		VkResult result = api.CreateXlibSurfaceKHR (instance, &info, NULL, surface);

		if (result != VK_SUCCESS) {

			std::string message = "vkCreateXlibSurfaceKHR failed: ";
			message += ResultText (result);
			SetError (message.c_str ());
			return false;

		}

		return true;

	}

	void* CreateHiddenWindow (int width, int height) {

		Display* display = X11Display ();

		if (!display) return 0;

		::Window window = XCreateSimpleWindow (display, DefaultRootWindow (display),
			0, 0, (unsigned int)width, (unsigned int)height, 0, 0, 0);

		if (!window) return 0;

		// The swapchain of the self test needs a mapped window; it is never shown
		// on screen because nothing is painted by the window manager.
		XMapWindow (display, window);
		XFlush (display);

		hiddenDisplays[(void*)(uintptr_t)window] = display;

		return (void*)(uintptr_t)window;

	}

	void DestroyHiddenWindow (void* window) {

		if (!window) return;

		std::map<void*, Display*>::iterator it = hiddenDisplays.find (window);

		if (it != hiddenDisplays.end ()) {

			XDestroyWindow (it->second, (::Window)(uintptr_t)window);
			XFlush (it->second);
			hiddenDisplays.erase (it);

		}

	}

}

#endif
