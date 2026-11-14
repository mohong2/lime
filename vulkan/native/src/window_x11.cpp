#include "internal.h"

// Window discovery for X11, the counterpart of window_win32.cpp.
//
// The renderer presents outside SDL, so it needs the game's X11 window id:
// every window still carries _NET_WM_PID (SDL sets it), which is enough to find
// the one this process owns without linking against SDL.
//
// The process helpers exist for the attach tool only; they use signals, which
// is what a debugging attach needs on Linux.

#if defined(LIME_VULKAN_LINUX)

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <signal.h>
#include <unistd.h>

#include <cstring>
#include <vector>

namespace vulkan {

	// Not called 'Display': a function with that name at namespace scope hides the
	// X11 typedef for everything below it, and every later 'Display*' then fails
	// to compile on Linux.
	static Display* X11Display () {

		static Display* display = NULL;

		if (!display) display = XOpenDisplay (NULL);

		return display;

	}

	static Atom NetWmPidAtom (Display* display) {

		static Atom atom = None;

		if (!atom) atom = XInternAtom (display, "_NET_WM_PID", True);

		return atom;

	}

	/** The pid a window belongs to, or 0 when the property is absent. */
	static unsigned int WindowPid (Display* display, ::Window window) {

		Atom atom = NetWmPidAtom (display);

		if (atom == None) return 0;

		Atom type = None;
		int format = 0;
		unsigned long count = 0;
		unsigned long after = 0;
		unsigned char* data = NULL;

		if (XGetWindowProperty (display, window, atom, 0, 1, False, XA_CARDINAL,
				&type, &format, &count, &after, &data) != Success || !data)
			return 0;

		unsigned int pid = (count > 0 && format == 32) ? (unsigned int)((unsigned long*)data)[0] : 0;
		XFree (data);

		return pid;

	}

	static ::Window SearchTree (Display* display, ::Window window, unsigned int pid, int depth) {

		if (depth > 8) return 0;

		if (WindowPid (display, window) == pid) return window;

		::Window root = 0;
		::Window parent = 0;
		::Window* children = NULL;
		unsigned int count = 0;

		if (!XQueryTree (display, window, &root, &parent, &children, &count)) return 0;

		::Window found = 0;

		for (unsigned int i = 0; i < count && !found; i++)
			found = SearchTree (display, children[i], pid, depth + 1);

		if (children) XFree (children);

		return found;

	}

	void* FindProcessWindow (unsigned int pid) {

		Display* display = X11Display ();

		if (!display || !pid) return 0;

		return (void*)(uintptr_t)SearchTree (display, DefaultRootWindow (display), pid, 0);

	}

	unsigned int FindProcessIdByName (const char* exeName) {

		// Not needed on this platform: SelfWindow goes through _NET_WM_PID, so
		// there is no name-to-pid lookup to do.
		return 0;

	}

	void* SelfWindow () {

		return FindProcessWindow ((unsigned int)getpid ());

	}

	bool WindowClientSize (void* window, int* width, int* height) {

		Display* display = X11Display ();

		if (!display || !window) return false;

		XWindowAttributes attributes;

		if (!XGetWindowAttributes (display, (::Window)(uintptr_t)window, &attributes)) return false;

		if (width) *width = attributes.width;
		if (height) *height = attributes.height;

		return true;

	}

	int WindowTitle (void* window, char* out, int capacity) {

		Display* display = X11Display ();

		if (!display || !window || !out || capacity <= 0) return 0;

		char* name = NULL;

		if (!XFetchName (display, (::Window)(uintptr_t)window, &name) || !name) return 0;

		int length = (int)strlen (name);

		if (length > capacity - 1) length = capacity - 1;

		memcpy (out, name, (size_t)length);
		out[length] = 0;

		XFree (name);

		return length;

	}

	int SuspendProcess (unsigned int pid) {

		if (!pid) return 0;

		return kill ((pid_t)pid, SIGSTOP) == 0 ? 1 : 0;

	}

	int ResumeProcess (unsigned int pid) {

		if (!pid) return 0;

		return kill ((pid_t)pid, SIGCONT) == 0 ? 1 : 0;

	}

}

#endif
