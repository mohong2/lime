#include <utils/FatalReport.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
	#ifndef WIN32_LEAN_AND_MEAN
	#define WIN32_LEAN_AND_MEAN
	#endif
	#ifndef NOMINMAX
	#define NOMINMAX
	#endif
	#include <windows.h>
	#include <direct.h>
#else
	#include <sys/stat.h>
	#include <sys/types.h>
	#include <fcntl.h>
	#include <unistd.h>
	#include <errno.h>
	#if defined(__ANDROID__)
		#include <android/log.h>
	#endif
	#if defined(__APPLE__)
		#include <TargetConditionals.h>
		#include <sys/wait.h>
		#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
			#include <os/log.h>
		#endif
	#endif
	#if defined(__linux__) && !defined(__ANDROID__) && !defined(EMSCRIPTEN)
		#include <sys/wait.h>
	#endif
#endif

#ifdef EMSCRIPTEN
	#include <emscripten.h>
#endif


namespace lime {


	namespace FatalReport {


		// One report per kind and process: a failing audio device must not write a
		// file per retry, and a fatal report must never be overwritten by a second
		// one (the first one has the real cause).
		#define LIME_FATAL_KINDS 8
		static char reportedKinds[LIME_FATAL_KINDS][32];
		static int reportedCount = 0;

		// Worst case file name: <dir 512> + "lime_" + kind + "_YYYYMMDD_HHMMSS.txt"
		static char cachedDir[640];
		static char reportPath[900];
		static char reportBody[32768];


		// ── UTF-8 <-> UTF-16 at the Win32 boundary ───────────────────────────
		//
		// The whole file works in UTF-8 (that is what Haxe strings and SDL's own
		// error messages are). Win32 *A functions would decode that with the ANSI
		// code page: away go the Chinese characters, and with them the report.
#if defined(_WIN32)
		static bool Utf8ToUtf16 (const char* src, wchar_t* dst, int dstCount) {

			if (!src || !dst || dstCount <= 0) return false;
			int n = MultiByteToWideChar (CP_UTF8, MB_ERR_INVALID_CHARS, src, -1, dst, dstCount);
			if (n == 0) n = MultiByteToWideChar (CP_UTF8, 0, src, -1, dst, dstCount);
			return n > 0;

		}


		static bool Utf16ToUtf8 (const wchar_t* src, char* dst, size_t dstCount) {

			if (!src || !dst || dstCount == 0) return false;
			dst[0] = 0;
			return WideCharToMultiByte (CP_UTF8, 0, src, -1, dst, (int)dstCount, 0, 0) > 0;

		}
#endif


		// ── paths ────────────────────────────────────────────────────────────

		static void EnsureDir (const char* dir) {

			if (!dir || !dir[0]) return;

		#if defined(_WIN32)
			wchar_t wdir[1024];
			if (!Utf8ToUtf16 (dir, wdir, 1024)) return;
			for (wchar_t* p = wdir + 1; *p; p++) {
				if (*p != L'\\' && *p != L'/') continue;
				if (*(p - 1) == L':') continue;
				wchar_t saved = *p;
				*p = 0;
				CreateDirectoryW (wdir, 0);
				*p = saved;
			}
			CreateDirectoryW (wdir, 0);
		#else
			char tmp[sizeof (cachedDir)];
			snprintf (tmp, sizeof (tmp), "%s", dir);
			for (char* p = tmp + 1; *p; p++) {
				if (*p != '/') continue;
				*p = 0;
				mkdir (tmp, 0755);
				*p = '/';
			}
			mkdir (tmp, 0755);
		#endif

		}


		#if !defined(_WIN32)
		static void DirUpTo (char* path, size_t cap) {

			(void)cap;
			char* slash = strrchr (path, '/');
			if (slash) *(slash + 1) = 0;

		}
		#endif


		const char* CrashDir () {

			if (cachedDir[0]) return cachedDir;

		#if defined(_WIN32)
			// 1) What the executable published (see seiun_set_crash_dir()).
			wchar_t wenv[1024];
			DWORD n = GetEnvironmentVariableW (L"SEIUN_CRASH_DIR", wenv, 1024);
			if (n > 0 && n < 1024) {
				Utf16ToUtf8 (wenv, cachedDir, sizeof (cachedDir));
			}
			if (!cachedDir[0]) {
				// 2) <dir of this ndll>/crash/ - on Windows the ndll sits next to the exe.
				wchar_t wmodule[1024];
				HMODULE module = 0;
				if (GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					(LPCWSTR)(LPCVOID)&CrashDir, &module) && module) {
					if (GetModuleFileNameW (module, wmodule, 1024)) {
						Utf16ToUtf8 (wmodule, cachedDir, sizeof (cachedDir));
						char* slash = strrchr (cachedDir, '\\');
						if (!slash) slash = strrchr (cachedDir, '/');
						if (slash) *(slash + 1) = 0;
						strncat (cachedDir, "crash\\", sizeof (cachedDir) - strlen (cachedDir) - 1);
					}
				}
			}
		#elif defined(__ANDROID__)
			if (!cachedDir[0]) {
				// Same location the native crash hooks use: the app's external files
				// dir, derived from the process name (= package name).
				char pkg[192] = { 0 };
				int fd = open ("/proc/self/cmdline", O_RDONLY);
				if (fd >= 0) {
					char buf[256];
					ssize_t r = read (fd, buf, sizeof (buf) - 1);
					close (fd);
					if (r > 0) {
						buf[r] = 0;
						size_t i = 0;
						while (i < (size_t)r && buf[i] && buf[i] != ':' && i < sizeof (pkg) - 1) {
							pkg[i] = buf[i];
							i++;
						}
						pkg[i] = 0;
					}
				}
				if (pkg[0]) {
					snprintf (cachedDir, sizeof (cachedDir),
						"/storage/emulated/0/Android/data/%s/files/crash/", pkg);
				} else {
					snprintf (cachedDir, sizeof (cachedDir), "crash/");
				}
			}
		#elif defined(__APPLE__) && defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
			if (!cachedDir[0]) {
				const char* home = getenv ("HOME");
				if (home && home[0]) snprintf (cachedDir, sizeof (cachedDir), "%s/Documents/crash/", home);
				else snprintf (cachedDir, sizeof (cachedDir), "crash/");
			}
		#else
			// 1) Published by the executable, 2) the binary's own directory.
			const char* env = getenv ("SEIUN_CRASH_DIR");
			if (env && env[0]) {
				snprintf (cachedDir, sizeof (cachedDir), "%s", env);
			} else {
				char exe[600] = { 0 };
				ssize_t n = readlink ("/proc/self/exe", exe, sizeof (exe) - 1);
				if (n > 0) {
					exe[n] = 0;
					DirUpTo (exe, sizeof (exe));
					snprintf (cachedDir, sizeof (cachedDir), "%scrash/", exe);
				} else {
					snprintf (cachedDir, sizeof (cachedDir), "crash/");
				}
			}
		#endif

			// Trailing separator, always.
			size_t l = strlen (cachedDir);
			if (l > 0 && cachedDir[l - 1] != '/' && cachedDir[l - 1] != '\\') {
				if (l + 2 < sizeof (cachedDir)) {
				#if defined(_WIN32)
					cachedDir[l] = '\\';
				#else
					cachedDir[l] = '/';
				#endif
					cachedDir[l + 1] = 0;
				}
			}

			return cachedDir;

		}


		static void MakeReportPath (const char* kind) {

			time_t t = time (0);
			struct tm tmv;
		#if defined(_WIN32)
			localtime_s (&tmv, &t);
		#else
			localtime_r (&t, &tmv);
		#endif

			char safe[64];
			int len = 0;
			for (const char* p = (kind && kind[0]) ? kind : "fatal"; *p && len < 63; p++) {
				char c = *p;
				safe[len++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
					(c >= '0' && c <= '9') || c == '-' || c == '_') ? c : '_';
			}
			safe[len] = 0;

			EnsureDir (CrashDir ());
			snprintf (reportPath, sizeof (reportPath), "%slime_%s_%04d%02d%02d_%02d%02d%02d.txt",
				CrashDir (), safe,
				tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
				tmv.tm_hour, tmv.tm_min, tmv.tm_sec);

		}


		static void Append (char* buf, size_t cap, int* len, const char* fmt, ...) {

			if (*len < 0 || (size_t)*len >= cap) return;
			va_list ap;
			va_start (ap, fmt);
			int l = vsnprintf (buf + *len, cap - (size_t)*len, fmt, ap);
			va_end (ap);
			if (l < 0) return;
			if ((size_t)*len + (size_t)l >= cap) {
				*len = (int)cap - 1;
				return;
			}
			*len += l;

		}


		static void DescribeOs (char* out, size_t cap) {

			out[0] = 0;
		#if defined(_WIN32)
			typedef LONG (WINAPI *RtlGetVersionFn) (OSVERSIONINFOW*);
			OSVERSIONINFOW vi;
			memset (&vi, 0, sizeof (vi));
			vi.dwOSVersionInfoSize = sizeof (vi);
			HMODULE ntdll = GetModuleHandleW (L"ntdll.dll");
			if (ntdll) {
				RtlGetVersionFn fn = (RtlGetVersionFn)GetProcAddress (ntdll, "RtlGetVersion");
				if (fn) fn (&vi);
			}
			SYSTEM_INFO si;
			memset (&si, 0, sizeof (si));
			GetNativeSystemInfo (&si);
			const char* arch = "unknown";
			switch (si.wProcessorArchitecture) {
				case PROCESSOR_ARCHITECTURE_AMD64: arch = "x64"; break;
				case PROCESSOR_ARCHITECTURE_INTEL: arch = "x86"; break;
				case PROCESSOR_ARCHITECTURE_ARM64: arch = "arm64"; break;
				case PROCESSOR_ARCHITECTURE_ARM: arch = "arm"; break;
			}
			snprintf (out, cap, "Windows %lu.%lu (build %lu) %s, %d-bit process",
				(unsigned long)vi.dwMajorVersion, (unsigned long)vi.dwMinorVersion,
				(unsigned long)vi.dwBuildNumber, arch, (int)(sizeof (void*) * 8));
		#elif defined(__ANDROID__)
			char model[92] = { 0 }, manuf[92] = { 0 }, release[92] = { 0 };
			__system_property_get ("ro.product.model", model);
			__system_property_get ("ro.product.manufacturer", manuf);
			__system_property_get ("ro.build.version.release", release);
			snprintf (out, cap, "Android %s %s %s, %d-bit process",
				release, manuf, model, (int)(sizeof (void*) * 8));
		#else
			snprintf (out, cap, "POSIX, %d-bit process", (int)(sizeof (void*) * 8));
		#endif

		}


		// FormatMessageW() keeps the "%1" placeholders when no insert list is
		// supplied ("error 193: %1 is not a valid Win32 application"), so they are
		// dropped here along with the spaces they leave at the front.
		static void StripInserts (char* text) {

			for (char* p = text; *p != 0; ) {
				if (*p == '%' && p[1] >= '0' && p[1] <= '9') {
					memmove (p, p + 2, strlen (p + 2) + 1);
					continue;
				}
				p++;
			}
			while (*text == ' ' || *text == '\t' || *text == ':') {
				memmove (text, text + 1, strlen (text));
			}

		}


		// GetLastError() belongs to whoever failed first. Writing the report creates
		// directories and opens a file, so the code is captured when a report starts
		// and restored just before it is described.
		static void SnapshotLastError (char* out, size_t cap) {

		#if defined(_WIN32)
			snprintf (out, cap, "%lu", (unsigned long)GetLastError ());
		#else
			snprintf (out, cap, "%d", errno);
		#endif

		}


		static void RestoreLastError (const char* snapshot) {

		#if defined(_WIN32)
			if (snapshot && snapshot[0]) SetLastError ((DWORD)strtoul (snapshot, 0, 10));
		#else
			if (snapshot && snapshot[0]) errno = (int)strtol (snapshot, 0, 10);
		#endif

		}


		static void DescribeLastError (char* out, size_t cap) {

		#if defined(_WIN32)
			DWORD err = GetLastError ();
			wchar_t wbuf[512];
			wbuf[0] = 0;
			if (FormatMessageW (FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, 0, err,
				MAKELANGID (LANG_NEUTRAL, SUBLANG_DEFAULT), wbuf,
				(DWORD)(sizeof (wbuf) / sizeof (wbuf[0])), 0) == 0) {
				wbuf[0] = 0;
			}
			char text[512];
			Utf16ToUtf8 (wbuf, text, sizeof (text));
			for (size_t i = strlen (text); i > 0; i--) {
				if (text[i - 1] == '\r' || text[i - 1] == '\n' || text[i - 1] == ' ') text[i - 1] = 0;
				else break;
			}
			StripInserts (text);
			if (err == 0) snprintf (out, cap, "0 (no error was recorded)");
			else snprintf (out, cap, "%lu (%s)", (unsigned long)err, text[0] ? text : "no system text");
		#else
			snprintf (out, cap, "errno=%d (%s)", errno, strerror (errno));
		#endif

		}


		// ── "what / why / how to fix" for the failures lime itself detects ───

		static void Explain (const char* kind, char* out, size_t cap) {

			out[0] = 0;
			int len = 0;
			const char* k = kind ? kind : "";

			if (strcmp (k, "sdl_init") == 0) {

				Append (out, cap, &len,
					"SDL could not start\n"
					"What : SDL_Init() failed, so there is no video or gamepad subsystem at all.\n"
					"Why  : outdated or broken GPU driver, remote desktop / VM without 3D,\n"
					"        display service unavailable, or SDL3 DLL and ndll from different builds.\n"
					"Fix  : update the GPU driver; run on the local machine instead of RDP;\n"
					"        confirm lime.ndll and the SDL3 DLL come from the same build.\n"
					"        Message is the raw SDL_GetError() text.\n");

			} else if (strcmp (k, "window") == 0 || strcmp (k, "window_forced") == 0) {

				Append (out, cap, &len,
					"Could not create the game window\n"
					"What : SDL_CreateWindow() failed; the process would keep running with no window.\n"
					"Why  : invalid display mode, GPU driver refusing a GL window, RDP session limits,\n"
					"        a broken multi-monitor layout, or out of video memory.\n"
					"Fix  : update the GPU driver; reset the resolution / window mode to the default;\n"
					"        retry in a clean local session. Message is the raw SDL_GetError() text.\n");

			} else if (strcmp (k, "gl") == 0) {

				Append (out, cap, &len,
					"No OpenGL context\n"
					"What : SDL_GL_CreateContext() and the software renderer fallback both failed.\n"
					"Why  : the driver does not expose the required GL version, opengl32.dll was\n"
					"        replaced (ReShade or another injector), or RDP has no hardware GL.\n"
					"Fix  : remove opengl32.dll / injectors from the game directory, update the\n"
					"        driver, and run on the local machine.\n");

			} else if (strcmp (k, "audio") == 0) {

				Append (out, cap, &len,
					"Could not open the audio device\n"
					"What : alcOpenDevice() returned null; the game keeps running silently.\n"
					"Why  : no playback device, the default device is exclusive, or the OpenAL Soft\n"
					"        DLL is missing.\n"
					"Fix  : select a working output device in the system sound settings, disable\n"
					"        exclusive mode, and reinstall / update the audio driver.\n");

			} else {

				Append (out, cap, &len,
					"Fatal error inside lime's native layer\n"
					"What : the failure happened below Haxe (window / graphics / audio setup), so\n"
					"        there is no Haxe-side report for it.\n"
					"Fix  : send this file to the developers; Message and Detail carry the raw\n"
					"        error text and the system error code.\n");

			}

		}


		// ── platform notification ────────────────────────────────────────────

		static void ShowDialog (const char* title, const char* message) {

			if (!title) title = "SeiunEngine";
			if (!message) message = "";

		#if defined(EMSCRIPTEN)
			EM_ASM ({
				console.error (UTF8ToString ($0) + ": " + UTF8ToString ($1));
			}, title, message);
			return;

		#elif defined(_WIN32)
			wchar_t wtitle[256];
			wchar_t wmsg[6000];
			if (!Utf8ToUtf16 (title, wtitle, 256)) return;
			if (!Utf8ToUtf16 (message, wmsg, 6000)) {
				Utf8ToUtf16 ("(message too long to display - see the report file)", wmsg, 6000);
			}
			HWND owner = GetActiveWindow ();
			bool shown = false;
			HMODULE comctl32 = LoadLibraryW (L"comctl32.dll");
			if (comctl32) {
				typedef HRESULT (WINAPI *TaskDialogFn) (HWND, HINSTANCE, PCWSTR, PCWSTR, PCWSTR, int, PCWSTR, int*);
				TaskDialogFn taskDialog = (TaskDialogFn)GetProcAddress (comctl32, "TaskDialog");
				if (taskDialog) {
					int button = 0;
					// 1 = TDCBF_OK_BUTTON, icon = TD_ERROR_ICON (-2) as an integer resource.
					if (taskDialog (owner, 0, wtitle, wmsg, 0, 1, (PCWSTR)(ULONG_PTR)(WORD)(-2), &button) == 0) {
						shown = true;
					}
				}
				FreeLibrary (comctl32);
			}
			if (!shown) {
				MessageBoxW (owner, wmsg, wtitle, MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
			}

		#elif defined(__ANDROID__)
			__android_log_print (ANDROID_LOG_FATAL, "SeiunEngine-Lime", "%s: %s", title, message);

		#elif defined(__APPLE__) && defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
			os_log_with_type (OS_LOG_DEFAULT, OS_LOG_TYPE_FAULT, "%{public}s: %{public}s", title, message);

		#elif defined(__APPLE__)
			// The text goes through a UTF-8 file: quotes, newlines and Chinese then
			// never need AppleScript/shell escaping.
			const char* tmpPath = "/tmp/seiun_lime_fatal.txt";
			FILE* f = fopen (tmpPath, "wb");
			if (f) {
				fputs (message, f);
				fclose (f);
				char script[1024];
				snprintf (script, sizeof (script),
					"set m to do shell script \"cat '%s'\"\n"
					"display alert \"SeiunEngine\" message m as critical", tmpPath);
				pid_t pid = fork ();
				if (pid == 0) {
					execl ("/usr/bin/osascript", "osascript", "-e", script, (char*)0);
					_exit (127);
				}
				if (pid > 0) {
					int status = 0;
					waitpid (pid, &status, 0);
					if (WIFEXITED (status) && WEXITSTATUS (status) == 0) return;
				}
			}
			fprintf (stderr, "SeiunEngine: %s\n%s\n", title, message);

		#else
			if (getenv ("DISPLAY") || getenv ("WAYLAND_DISPLAY")) {
				pid_t pid = fork ();
				if (pid == 0) {
					execlp ("zenity", "zenity", "--error", "--title=SeiunEngine", "--text", message, (char*)0);
					execlp ("kdialog", "kdialog", "--title", "SeiunEngine", "--error", message, (char*)0);
					execlp ("xmessage", "xmessage", "-center", message, (char*)0);
					_exit (127);
				}
				if (pid > 0) {
					int status = 0;
					waitpid (pid, &status, 0);
					if (WIFEXITED (status) && WEXITSTATUS (status) == 0) return;
				}
			}
			fprintf (stderr, "SeiunEngine: %s\n%s\n", title, message);
		#endif

		}


		static bool AlreadyReported (const char* kind) {

			const char* k = kind ? kind : "fatal";
			for (int i = 0; i < reportedCount; i++) {
				if (strcmp (reportedKinds[i], k) == 0) return true;
			}
			return false;

		}


		static void Remember (const char* kind) {

			if (reportedCount >= LIME_FATAL_KINDS) return;
			snprintf (reportedKinds[reportedCount], sizeof (reportedKinds[0]), "%s", kind ? kind : "fatal");
			reportedCount++;

		}


		static void WriteReport (const char* kind, const char* message, const char* detail) {

			// Before MakeReportPath() below touches the filesystem.
			char lastError[32];
			SnapshotLastError (lastError, sizeof (lastError));

			time_t t = time (0);
			struct tm tmv;
		#if defined(_WIN32)
			localtime_s (&tmv, &t);
		#else
			localtime_r (&t, &tmv);
		#endif

			MakeReportPath (kind);

			char* body = reportBody;
			size_t cap = sizeof (reportBody);
			int len = 0;

			Append (body, cap, &len, "=== SeiunEngine Crash Report ===\n");
			Append (body, cap, &len, "Kind: lime:%s (native layer of the lime ndll)\n", kind ? kind : "fatal");
			Append (body, cap, &len, "Date: %04d-%02d-%02d %02d:%02d:%02d\n",
				tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
			Append (body, cap, &len, "Message: %s\n", message ? message : "(none)");

			char explain[2048];
			Explain (kind, explain, sizeof (explain));
			Append (body, cap, &len, "\n--- Diagnosis ---\n%s", explain);

			Append (body, cap, &len, "\n--- Environment ---\n");
			char line[512];
			DescribeOs (line, sizeof (line));
			Append (body, cap, &len, "OS: %s\n", line);
			RestoreLastError (lastError);
			DescribeLastError (line, sizeof (line));
			Append (body, cap, &len, "LastError: %s\n", line);
			Append (body, cap, &len, "Crash dir: %s\n", CrashDir ());
		#if defined(_WIN32)
			Append (body, cap, &len, "Process: pid=%lu\n", (unsigned long)GetCurrentProcessId ());
			{
				wchar_t wexe[1024];
				char exe[1024];
				if (GetModuleFileNameW (0, wexe, 1024) && Utf16ToUtf8 (wexe, exe, sizeof (exe))) {
					Append (body, cap, &len, "Exe: %s\n", exe);
				}
			}
		#else
			Append (body, cap, &len, "Process: pid=%ld\n", (long)getpid ());
		#endif

			if (detail && detail[0]) {
				Append (body, cap, &len, "\n--- Detail ---\n%s\n", detail);
			}

			Append (body, cap, &len,
				"\n--- Note ---\n"
				"This failure happened inside lime's native layer, i.e. below Haxe, so no\n"
				"Haxe-side dump exists for it. The exe-side reporter writes its own files\n"
				"into the same crash directory (native_crash_*.txt / SeiunEngine_fatal_*.txt).\n"
				"\n--- Triage ---\n"
				"1. Read Message above: it is the raw error string (SDL_GetError /\n"
				"   alcGetString), not a paraphrase.\n"
				"2. Check LastError: Win32 error 193 = wrong bitness (32-bit ndll in a\n"
				"   64-bit process), 126 = a dependency DLL is missing.\n"
				"3. Make sure lime.ndll and the exe come from the same build:\n"
				"   haxelib run lime rebuild windows -64 -D lime-sdl3\n"
				"4. Graphics problems: remove opengl32.dll / ReShade / overlays from the\n"
				"   game directory and retry.\n");

			FILE* f = 0;
		#if defined(_WIN32)
			wchar_t wpath[1024];
			if (Utf8ToUtf16 (reportPath, wpath, 1024)) f = _wfopen (wpath, L"w");
		#else
			f = fopen (reportPath, "w");
		#endif
			if (f) {
				fputs ("\xEF\xBB\xBF", f);
				fputs (body, f);
				fclose (f);
			}

			// Mirror: survives an unwritable crash directory, and is the only
			// user-visible surface on Android/iOS.
		#if defined(__ANDROID__)
			__android_log_print (ANDROID_LOG_FATAL, "SeiunEngine-Lime", "%s", body);
		#elif defined(__APPLE__) && defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
			os_log_with_type (OS_LOG_DEFAULT, OS_LOG_TYPE_FAULT, "%{public}s", body);
		#elif defined(_WIN32)
			OutputDebugStringA (body);
		#else
			fprintf (stderr, "%s\n", body);
		#endif

		}


		void Report (const char* kind, const char* message, const char* detail) {

			if (AlreadyReported (kind)) {
				_exit (1);
			}
			Remember (kind);
			WriteReport (kind, message, detail);

			char dialog[4096];
			int len = 0;
			Append (dialog, sizeof (dialog), &len,
				"%s\n\nCrash report saved to:\n%s\n\n"
				"The game will now exit.\n"
				"Please send this report to the developers:\n"
				"https://github.com/mohong2/FNF-SeiunEngine/issues",
				message ? message : "", reportPath);

			char title[256];
			snprintf (title, sizeof (title), "SeiunEngine - startup failed");
			ShowDialog (title, dialog);

			// _exit: no atexit handlers and no DLL_PROCESS_DETACH on a process whose
			// init just failed (OpenAL's cleanup in a half-built state is exactly the
			// kind of second crash that would hide the first one).
			_exit (1);

		}


		void ReportNonFatal (const char* kind, const char* message, const char* detail) {

			if (AlreadyReported (kind)) return;
			Remember (kind);
			WriteReport (kind, message, detail);

		}


		bool TestFail (const char* what) {

			const char* env = getenv ("SEIUN_TEST_FATAL");
			if (!env || !env[0] || !what || !what[0]) return false;
			// Either the exact kind or "all".
			if (strcmp (env, "all") == 0) return true;
			return strcmp (env, what) == 0;

		}


	}


}
