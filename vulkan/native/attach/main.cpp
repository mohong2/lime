// lime-vulkan-attach.exe - draws frames with our Vulkan backend on the window of an
// already running process.
//
// This is the forensics tool: no Haxe changes, no rebuild of the game, the
// swapchain is attached straight to the HWND that SeiunEngine.exe created.
//
// What it proves:
//   * our surface works on a window somebody else created;
//   * swapchain, pipelines and present work at the real window size;
//   * a failure takes the fallback path instead of killing the process.
//
// What it does not prove:
//   * that the game image is drawn by Vulkan. That needs the renderer takeover;
//     what is drawn here is our own test pattern, layered on top of the game.
//
// Usage:
//   lime-vulkan-attach.exe                                  finds SeiunEngine.exe, 10 s gradient
//   lime-vulkan-attach.exe --exe=SeiunEngine.exe --seconds=20
//   lime-vulkan-attach.exe --pid=1234 --pattern=triangle
//   lime-vulkan-attach.exe --list                           only list that process's windows
//   lime-vulkan-attach.exe --keep                           stay on the last frame until Enter
//   lime-vulkan-attach.exe --suspend                        freeze the game threads first

#include <lime_vulkan.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char* ArgValue (const char* argument, const char* prefix) {

	size_t length = strlen (prefix);

	if (strncmp (argument, prefix, length) == 0) return argument + length;
	return NULL;

}

// Picks the error= line out of the status text, so only the reason is shown.
static const char* ExtractError (const char* status) {

	static char line[512];
	line[0] = 0;

	const char* cursor = status;

	while (cursor && *cursor) {

		const char* end = strchr (cursor, '\n');
		size_t length = end ? (size_t)(end - cursor) : strlen (cursor);

		if (length > 6 && strncmp (cursor, "error=", 6) == 0) {

			size_t copy = length - 6;
			if (copy > sizeof (line) - 1) copy = sizeof (line) - 1;
			memcpy (line, cursor + 6, copy);
			line[copy] = 0;
			return line;

		}

		cursor = end ? end + 1 : 0;

	}

	return "unknown";

}

static BOOL CALLBACK PrintWindow (HWND hwnd, LPARAM parameter) {

	unsigned int pid = (unsigned int)(size_t)parameter;
	DWORD owner = 0;

	GetWindowThreadProcessId (hwnd, &owner);

	if (owner != pid) return TRUE;

	char title[256] = { 0 };
	char className[128] = { 0 };
	RECT client;

	GetWindowTextA (hwnd, title, sizeof (title));
	GetClassNameA (hwnd, className, sizeof (className));
	GetClientRect (hwnd, &client);

	printf ("  hwnd=%p visible=%d ownerless=%d client=%ldx%ld class=%s title=\"%s\"\n",
		(void*)hwnd,
		IsWindowVisible (hwnd) ? 1 : 0,
		GetWindow (hwnd, GW_OWNER) == NULL ? 1 : 0,
		(long)(client.right - client.left),
		(long)(client.bottom - client.top),
		className,
		title);

	return TRUE;

}

int main (int argc, char** argv) {

	setvbuf (stdout, NULL, _IONBF, 0);

	const char* exeName = "SeiunEngine.exe";
	unsigned int pid = 0;
	int pattern = LIME_VULKAN_PATTERN_GRADIENT;
	int patternSet = 0;
	double seconds = 10.0;
	int maxFrames = -1;
	bool suspend = false;
	bool keep = false;
	bool verbose = false;
	bool listOnly = false;
	bool probeOnly = false;
	int deviceIndex = -1;

	for (int i = 1; i < argc; ++i) {

		const char* value = NULL;

		if ((value = ArgValue (argv[i], "--exe=")) != NULL) exeName = value;
		else if ((value = ArgValue (argv[i], "--pid=")) != NULL) pid = (unsigned int)atoi (value);
		else if ((value = ArgValue (argv[i], "--seconds=")) != NULL) seconds = atof (value);
		else if ((value = ArgValue (argv[i], "--frames=")) != NULL) maxFrames = atoi (value);
		else if ((value = ArgValue (argv[i], "--pattern=")) != NULL) {

			patternSet = 1;
			pattern = (strcmp (value, "triangle") == 0) ? LIME_VULKAN_PATTERN_TRIANGLE : LIME_VULKAN_PATTERN_GRADIENT;

		}
		else if ((value = ArgValue (argv[i], "--device=")) != NULL) deviceIndex = atoi (value);
		else if (strcmp (argv[i], "--probe") == 0) probeOnly = true;
		else if (strcmp (argv[i], "--suspend") == 0) suspend = true;
		else if (strcmp (argv[i], "--keep") == 0) keep = true;
		else if (strcmp (argv[i], "--verbose") == 0) verbose = true;
		else if (strcmp (argv[i], "--list") == 0) listOnly = true;
		else {

			printf ("[attach] unknown argument: %s\n", argv[i]);
			return 1;

		}

	}

	if (!patternSet) pattern = LIME_VULKAN_PATTERN_GRADIENT;

	// --pid wins when given, otherwise the process name is used.
	if (pid == 0) {

		pid = lime_vulkan_process_id (exeName);

		if (pid == 0) {

			printf ("[attach] no process named '%s'\n", exeName);
			printf ("[attach] start the game first, or pass --pid=N\n");
			return 2;

		}

	}

	printf ("[attach] pid=%u (%s)\n", pid, exeName);

	if (listOnly) {

		printf ("[attach] windows of pid %u:\n", pid);
		EnumWindows (PrintWindow, (LPARAM)(size_t)pid);
		return 0;

	}

	void* window = lime_vulkan_process_window (pid);

	if (!window) {

		printf ("[attach] pid %u has no visible top-level window\n", pid);
		return 2;

	}

	char title[512] = { 0 };
	char originalTitle[512] = { 0 };
	int width = 0;
	int height = 0;

	lime_vulkan_window_title (window, title, sizeof (title));
	memcpy (originalTitle, title, sizeof (originalTitle));

	lime_vulkan_window_client_size (window, &width, &height);

	printf ("[attach] hwnd=%p title=\"%s\" client=%dx%d\n", window, title, width, height);

	if (width <= 0 || height <= 0) {

		printf ("[attach] window has no client area (minimized?)\n");
		return 2;

	}

	if (IsIconic ((HWND)window)) ShowWindow ((HWND)window, SW_RESTORE);

	SetForegroundWindow ((HWND)window);

	if (suspend) {

		int threads = lime_vulkan_process_suspend (pid);
		printf ("[attach] suspended %d thread(s) of pid %u (resume on exit)\n", threads, pid);

		if (threads == 0) {

			printf ("[attach] nothing suspended; continuing without freezing\n");

		}

	}

	lime_vulkan_options options;
	lime_vulkan_default_options (&options);
	options.verbose = verbose ? 1 : 0;
	options.deviceIndex = deviceIndex;
	options.fallbackDialog = 0; // a forensics tool should not open a modal dialog, a log line is enough

	if (probeOnly) {

		// Probe only, no swapchain: afterwards the window should still draw GL normally.
		int ok = lime_vulkan_probe_window (window, width, height, &options);

		char probeStatus[1024] = { 0 };
		lime_vulkan_status (probeStatus, sizeof (probeStatus));

		printf ("[attach] probe=%s\n%s\n", ok ? "ok" : "fail", probeStatus);

		if (suspend) lime_vulkan_process_resume (pid);

		return ok ? 0 : 3;

	}

	lime_vulkan_set_pattern (pattern);

	printf ("[attach] lime-vulkan %s creating context on foreign window...\n", lime_vulkan_version ());

	if (!lime_vulkan_context_create (window, width, height, &options)) {

		char status[1024] = { 0 };
		lime_vulkan_status (status, sizeof (status));

		printf ("[attach] FAILED to attach: %s\n", ExtractError (status));
		printf ("%s\n", status);

		if (suspend) lime_vulkan_process_resume (pid);

		return 3;

	}

	char status[1024] = { 0 };
	lime_vulkan_status (status, sizeof (status));
	printf ("[attach] attached.\n%s\n", status);

	const float clearColor[4] = { 0.03f, 0.03f, 0.05f, 1.0f };

	LARGE_INTEGER frequency;
	LARGE_INTEGER previousCounter;
	LARGE_INTEGER startCounter;

	QueryPerformanceFrequency (&frequency);
	QueryPerformanceCounter (&previousCounter);
	startCounter = previousCounter;

	int frames = 0;
	int reportedFrames = 0;
	bool windowTitleHijacked = true;
	char hijack[256];

	while (true) {

		if (!lime_vulkan_present (clearColor)) {

			lime_vulkan_status (status, sizeof (status));
			printf ("[attach] present failed: %s\n", ExtractError (status));
			break;

		}

		frames++;

		LARGE_INTEGER now;
		QueryPerformanceCounter (&now);

		double elapsed = (double)(now.QuadPart - startCounter.QuadPart) / (double)frequency.QuadPart;
		double slice = (double)(now.QuadPart - previousCounter.QuadPart) / (double)frequency.QuadPart;

		if (slice >= 0.5) {

			double fps = (double)(frames - reportedFrames) / slice;

			// Temporarily puts our probe information into the game window title, which
			// makes the title bar part of the evidence. Restored on exit, and the game
			// keeps calling SetWindowText itself, so this only borrows it.
			snprintf (hijack, sizeof (hijack), "%s  [lime-vulkan %.0f fps]", originalTitle[0] ? originalTitle : "window", fps);
			SetWindowTextA ((HWND)window, hijack);

			printf ("[attach] %.0f fps, %d frame(s), %.1fs\n", fps, frames, elapsed);

			reportedFrames = frames;
			previousCounter = now;

		}

		if (seconds > 0 && elapsed >= seconds) break;
		if (maxFrames > 0 && frames >= maxFrames) break;

	}

	lime_vulkan_status (status, sizeof (status));
	printf ("[attach] stopping after %d frame(s).\n%s\n", frames, status);

	if (keep) {

		printf ("[attach] holding the last frame — press Enter to detach\n");
		getchar ();

	}

	lime_vulkan_context_destroy ();

	if (windowTitleHijacked && originalTitle[0]) {

		SetWindowTextA ((HWND)window, originalTitle);

	}

	if (suspend) {

		int threads = lime_vulkan_process_resume (pid);
		printf ("[attach] resumed %d thread(s)\n", threads);

	}

	printf ("[attach] done.\n");
	return frames > 0 ? 0 : 4;

}
