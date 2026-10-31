#ifndef LIME_FATAL_REPORT_H
#define LIME_FATAL_REPORT_H


namespace lime {


	/**
	 * SeiunEngine: the ndll's own fatal reporter.
	 *
	 * A shared library cannot call into the executable that loaded it (on Windows
	 * the exe exports nothing, on Android liblime.so must not depend on the app),
	 * so every failure that happens inside lime's native layer - SDL_Init,
	 * SDL_CreateWindow, the GL context, the audio device - is reported here
	 * instead of being printf'd into a GUI build's void:
	 *
	 *   <crash dir>/lime_<kind>_<YYYYMMDD_HHMMSS>.txt   (UTF-8 + BOM)
	 *   + the platform's own notification (MessageBoxW / TaskDialog on Windows,
	 *     osascript on macOS, zenity/kdialog on Linux, logcat on Android,
	 *     os_log on iOS)
	 *   + the same "what / why / how to fix" text the Haxe-side reports use.
	 *
	 * The crash dir is the one the executable published in SEIUN_CRASH_DIR (see
	 * seiun_set_crash_dir() in the game), so both modules write into one folder;
	 * without it the exe directory (desktop) or the app's private files dir
	 * (mobile) is used.
	 */
	namespace FatalReport {


		/** Directory reports are written to (never NULL, always has a trailing slash). */
		const char* CrashDir ();

		/**
		 * Write the report, mirror it to the platform log, show the platform
		 * dialog and terminate the process (exit code 1).
		 *
		 * Use for failures the engine cannot continue without.
		 * @param kind    short tag, also part of the file name ("window", "sdl_init")
		 * @param message one line, shown in the dialog (usually SDL_GetError())
		 * @param detail  optional multi-line text (driver, flags, ...)
		 */
		void Report (const char* kind, const char* message, const char* detail = 0);

		/**
		 * Same as Report(), but returns: for failures with a usable degraded path
		 * (a missing audio device must not kill the game). The dialog is skipped,
		 * the file and the platform log are still written once per kind.
		 */
		void ReportNonFatal (const char* kind, const char* message, const char* detail = 0);

		/**
		 * Test hook, no effect unless SEIUN_TEST_FATAL names this kind.
		 *
		 * Verifying "the reporter actually reports" otherwise needs a patched,
		 * rebuilt ndll every time; with this the shipped build can be checked
		 * end to end:
		 *   set SEIUN_TEST_FATAL=window  &  SeiunEngine.exe
		 * (accepted: "window", "gl", "sdl_init", "window_forced", ...)
		 */
		bool TestFail (const char* what);


	}


}


#endif
