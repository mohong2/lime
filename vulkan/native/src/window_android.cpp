#include "internal.h"

// Window discovery for Android, the counterpart of window_win32.cpp.
//
// There is nothing to enumerate here: an Android app draws into the
// ANativeWindow that belongs to its activity, and only the Java/SDL layer knows
// that pointer. It arrives through lime_vulkan_set_android_window instead of
// being guessed; until it does, every query reports 'unknown' and the runtime
// reports 'unavailable' rather than drawing into nothing.

#if defined(LIME_VULKAN_ANDROID)

#include <android/native_window.h>

namespace vulkan {

	// What the host last reported, and what the swapchain was actually built for. They
	// differ exactly between a surface being recreated and the render thread noticing it.
	static void* androidWindow = NULL;
	static void* appliedWindow = NULL;

	void SetAndroidWindow (void* window) {

		// Called from the Java/SDL thread: only a pointer store here. Rebuilding the
		// swapchain belongs to the render thread (see TakeChangedAndroidWindow).
		androidWindow = window;

	}

	void MarkAndroidWindowApplied (void* window) {

		appliedWindow = window;

	}

	bool TakeChangedAndroidWindow (void** window) {

		if (androidWindow == appliedWindow) return false;

		// Not marked applied here: the caller does that once the new surface really
		// exists, so a failed rebuild is retried instead of being silently skipped.
		if (window) *window = androidWindow;
		return true;

	}

	unsigned int FindProcessIdByName (const char* exeName) {

		// No pid to find: the host hands the window over directly.
		return 0;

	}

	void* FindProcessWindow (unsigned int pid) {

		return 0;

	}

	/** The window the host registered; NULL until it does. */
	void* SelfWindow () {

		return androidWindow;

	}

	bool WindowClientSize (void* window, int* width, int* height) {

		if (!window) return false;

		int w = (int)ANativeWindow_getWidth ((ANativeWindow*)window);
		int h = (int)ANativeWindow_getHeight ((ANativeWindow*)window);

		if (w <= 0 || h <= 0) return false;

		if (width) *width = w;
		if (height) *height = h;

		return true;

	}

	int WindowTitle (void* window, char* out, int capacity) {

		// An Android activity has no window title to report.
		return 0;

	}

	int SuspendProcess (unsigned int pid) {

		return 0;

	}

	int ResumeProcess (unsigned int pid) {

		return 0;

	}

}

#endif
