#include "internal.h"

// Window discovery and thread freezing.
//
// This layer only does Win32 queries, never touches Vulkan, and does not depend
// on SDL or lime. It lives in its own file so the attach tool, the engine entry
// point and CI share one rule for 'how to find the game window' instead of each
// writing its own EnumWindows.

#if defined(LIME_VULKAN_WINDOWS)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include <cstdlib>
#include <cstring>

namespace vulkan {

	// ---------------------------------------------------------------------
	// pid <- process name
	// ---------------------------------------------------------------------

	unsigned int FindProcessIdByName (const char* exeName) {

		if (!exeName || !*exeName) return 0;

		HANDLE snapshot = CreateToolhelp32Snapshot (TH32CS_SNAPPROCESS, 0);

		if (snapshot == INVALID_HANDLE_VALUE) return 0;

		PROCESSENTRY32 entry = {};
		entry.dwSize = sizeof (entry);

		unsigned int found = 0;

		if (Process32First (snapshot, &entry)) {

			do {

				// _stricmp: Windows file names are case insensitive anyway, and a
				// player may write SeiunEngine.exe or seiunengine.exe.
				if (_stricmp (entry.szExeFile, exeName) == 0) {

					found = (unsigned int)entry.th32ProcessID;
					break;

				}

			} while (Process32Next (snapshot, &entry));

		}

		CloseHandle (snapshot);
		return found;

	}

	// ---------------------------------------------------------------------
	// window <- pid
	// ---------------------------------------------------------------------

	struct WindowSearch {

		unsigned int pid;
		HWND best;
		long long bestArea;

	};

	static long long ClientArea (HWND hwnd) {

		RECT rect;

		if (!GetClientRect (hwnd, &rect)) return 0;

		long long width = rect.right - rect.left;
		long long height = rect.bottom - rect.top;

		if (width <= 0 || height <= 0) return 0;

		return width * height;

	}

	static BOOL CALLBACK CollectWindow (HWND hwnd, LPARAM parameter) {

		WindowSearch* search = (WindowSearch*)parameter;
		DWORD pid = 0;

		GetWindowThreadProcessId (hwnd, &pid);

		// Only top level windows of the target process that are visible and have
		// no owner. Owned windows are dialogs and tool windows, often 0x0 or
		// hidden, and attaching to one paints into nothing.
		if (pid != search->pid) return TRUE;
		if (!IsWindowVisible (hwnd)) return TRUE;
		if (GetWindow (hwnd, GW_OWNER) != NULL) return TRUE;

		long long area = ClientArea (hwnd);

		// The main window is the largest one. Chosen by area rather than by order
		// because engines create a 1x1 hidden window first for capability checks,
		// and picking the first would select that one.
		if (area > search->bestArea) {

			search->bestArea = area;
			search->best = hwnd;

		}

		return TRUE;

	}

	void* FindProcessWindow (unsigned int pid) {

		if (pid == 0) return NULL;

		WindowSearch search = {};
		search.pid = pid;
		search.best = NULL;
		search.bestArea = 0;

		EnumWindows (CollectWindow, (LPARAM)&search);

		return (void*)search.best;

	}

	/** The main window of this process: the caller needs neither its pid nor its exe name. */
	void* SelfWindow () {

		return FindProcessWindow ((unsigned int)GetCurrentProcessId ());

	}

	bool WindowClientSize (void* window, int* width, int* height) {

		if (!window) return false;

		RECT rect;

		if (!GetClientRect ((HWND)window, &rect)) return false;

		int w = rect.right - rect.left;
		int h = rect.bottom - rect.top;

		if (width) *width = w > 0 ? w : 0;
		if (height) *height = h > 0 ? h : 0;

		return w > 0 && h > 0;

	}

	int WindowTitle (void* window, char* out, int capacity) {

		if (!window) return 0;

		int length = GetWindowTextA ((HWND)window, out, capacity);

		if (length < 0) return 0;

		if (out && capacity > 0) out[length < capacity ? length : capacity - 1] = 0;

		return length;

	}

	// ---------------------------------------------------------------------
	// Freezing threads
	// ---------------------------------------------------------------------

	static int SetProcessSuspended (unsigned int pid, bool suspend) {

		if (pid == 0) return 0;

		HANDLE snapshot = CreateToolhelp32Snapshot (TH32CS_SNAPTHREAD, 0);

		if (snapshot == INVALID_HANDLE_VALUE) return 0;

		THREADENTRY32 entry = {};
		entry.dwSize = sizeof (entry);

		int handled = 0;

		if (Thread32First (snapshot, &entry)) {

			do {

				if (entry.th32OwnerProcessID != pid) continue;
				if (entry.dwSize < FIELD_OFFSET (THREADENTRY32, th32OwnerProcessID) + sizeof (DWORD)) continue;

				HANDLE thread = OpenThread (THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);

				if (!thread) continue;

				if (suspend) {

					if (SuspendThread (thread) != (DWORD)-1) handled++;

				} else {

					if (ResumeThread (thread) != (DWORD)-1) handled++;

				}

				CloseHandle (thread);

			} while (Thread32Next (snapshot, &entry));

		}

		CloseHandle (snapshot);
		return handled;

	}

	int SuspendProcess (unsigned int pid) {

		return SetProcessSuspended (pid, true);

	}

	int ResumeProcess (unsigned int pid) {

		return SetProcessSuspended (pid, false);

	}

}

#else

// Other platforms start empty: X11 and Android discovery come later.
namespace vulkan {

	unsigned int FindProcessIdByName (const char* exeName) { return 0; }
	void* FindProcessWindow (unsigned int pid) { return NULL; }
	bool WindowClientSize (void* window, int* width, int* height) { return false; }
	int WindowTitle (void* window, char* out, int capacity) { return 0; }
	int SuspendProcess (unsigned int pid) { return 0; }
	int ResumeProcess (unsigned int pid) { return 0; }

}

#endif
