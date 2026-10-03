#include "SDLApplication.h"
#include "SDLGamepad.h"
#include "SDLJoystick.h"
#include <system/System.h>

// SDL_SetMainReady() is declared in SDL_main.h. We must NOT let SDL_main.h
// redefine main() (hxcpp provides its own entry point), so define
// SDL_MAIN_HANDLED before including it. This still declares SDL_SetMainReady.
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL3/SDL_main.h>

#ifdef HX_MACOS
#include <CoreFoundation/CoreFoundation.h>
#endif

#ifdef EMSCRIPTEN
#include "emscripten.h"
#endif

#ifdef ANDROID
#include <sys/system_properties.h>
#endif


namespace lime {


	AutoGCRoot* Application::callback = 0;
	SDLApplication* SDLApplication::currentApplication = 0;

	const int analogAxisDeadZone = 1000;
	std::map<int, std::map<int, int> > gamepadsAxisMap;
	bool inBackground = false;

	// High-resolution time in milliseconds from the performance counter.
	static double HiResMs () {

		static double invFreq = -1.0;
		if (invFreq < 0) invFreq = 1000.0 / (double) SDL_GetPerformanceFrequency ();
		return (double) SDL_GetPerformanceCounter () * invFreq;

	}

	// -----------------------------------------------------------------------
	// SeiunEngine on-device frame probe.
	//
	// There is no way to reproduce a phone-level frame drop on a desktop, so
	// instead of guessing this ships a forensic build: when enabled, the C++
	// frame driver logs one line per *slow* frame plus a periodic summary
	// through SDL_Log, which the Android backend writes to logcat. Per frame it
	// records:
	//
	//   delta  - wall time since the previous frame dispatch. This is the
	//            number that actually decides whether the player saw a hitch.
	//   drain  - time spent inside the SDL event drain loop. That is the input
	//            cost, i.e. "what a button press costs before the engine runs".
	//   work   - time spent dispatching UPDATE + RENDER (engine + GPU submit).
	//   events - how many SDL events were handled, with a per-type histogram for
	//            slow frames, so "pressing a button drops a frame" becomes
	//            "finger_down x1 drain=18.3ms" instead of a feeling.
	//
	// The probe is compiled in unconditionally but is inert unless a flag is
	// set, so enabling it never requires a rebuild:
	//
	//   Android: adb shell setprop debug.seiun.frame_probe 1   (then restart the app)
	//   Desktop/other: SEIUN_FRAME_PROBE=1 in the environment
	//
	// Read it with:
	//   adb logcat -c ; adb logcat -s SDL:V    (then grep for SEIUN)
	//
	// When disabled the cost is one predictable branch per SDL event and a
	// single cached property/environment lookup at startup.
	// -----------------------------------------------------------------------

	static int probeEnabled = -1;
	static Uint64 probeFrame = 0;
	static Uint64 probeSlowFrames = 0;
	static Uint64 probeDumps = 0;
	static Uint64 probeLastDumpFrame = 0;
	static Uint32 probeEventCount = 0;
	static Uint32 probeEventTotal = 0;
	static Uint16 probeTypes[24];
	static Uint32 probeTypeCounts[24];
	static Uint32 probeTypeKinds = 0;
	static double probeFrameStart = 0.0;
	static double probeDrainStart = 0.0;
	static double probeDeltaSum = 0.0;
	static double probeDeltaMax = 0.0;
	static double probeDrainMax = 0.0;
	static double probeWorkSum = 0.0;
	static double probeWorkMax = 0.0;

	// Frame-time histogram, in units of the frame budget. One increment per
	// frame, so it costs nothing, and it is what turns "it feels stuttery" into
	// a p95/p99 that can be quoted: the periodic summary prints the cumulative
	// counts and temp/perf/lime-mobile-probe-analyze.ps1 reads them back.
	// Edges: <1.0x, <1.25x, <1.5x, <2x, <3x, <4x, <8x, >=8x of the budget.
	#define PROBE_HISTOGRAM_BUCKETS 8
	static Uint32 probeHistogram[PROBE_HISTOGRAM_BUCKETS] = { 0, 0, 0, 0, 0, 0, 0, 0 };

	static const char* ProbeEventName (Uint16 type) {

		switch (type) {

			case SDL_EVENT_KEY_DOWN: return "key_down";
			case SDL_EVENT_KEY_UP: return "key_up";
			case SDL_EVENT_TEXT_INPUT: return "text_input";
			case SDL_EVENT_TEXT_EDITING: return "text_editing";
			case SDL_EVENT_MOUSE_MOTION: return "mouse_motion";
			case SDL_EVENT_MOUSE_BUTTON_DOWN: return "mouse_down";
			case SDL_EVENT_MOUSE_BUTTON_UP: return "mouse_up";
			case SDL_EVENT_MOUSE_WHEEL: return "mouse_wheel";
			case SDL_EVENT_FINGER_DOWN: return "finger_down";
			case SDL_EVENT_FINGER_UP: return "finger_up";
			case SDL_EVENT_FINGER_MOTION: return "finger_motion";
			case SDL_EVENT_FINGER_CANCELED: return "finger_canceled";
			case SDL_EVENT_WINDOW_EXPOSED: return "win_exposed";
			case SDL_EVENT_WINDOW_RESIZED: return "win_resized";
			case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: return "win_pixel_size";
			case SDL_EVENT_WINDOW_SHOWN: return "win_shown";
			case SDL_EVENT_WINDOW_HIDDEN: return "win_hidden";
			case SDL_EVENT_WINDOW_FOCUS_GAINED: return "win_focus_in";
			case SDL_EVENT_WINDOW_FOCUS_LOST: return "win_focus_out";
			case SDL_EVENT_WINDOW_CLOSE_REQUESTED: return "win_close";
			case SDL_EVENT_WINDOW_MINIMIZED: return "win_minimized";
			case SDL_EVENT_WINDOW_MAXIMIZED: return "win_maximized";
			case SDL_EVENT_WINDOW_RESTORED: return "win_restored";
			case SDL_EVENT_WINDOW_MOUSE_ENTER: return "win_enter";
			case SDL_EVENT_WINDOW_MOUSE_LEAVE: return "win_leave";
			case SDL_EVENT_WINDOW_MOVED: return "win_moved";
			case SDL_EVENT_WINDOW_SAFE_AREA_CHANGED: return "win_safe_area";
			case SDL_EVENT_GAMEPAD_AXIS_MOTION: return "pad_axis";
			case SDL_EVENT_GAMEPAD_BUTTON_DOWN: return "pad_down";
			case SDL_EVENT_GAMEPAD_BUTTON_UP: return "pad_up";
			case SDL_EVENT_JOYSTICK_AXIS_MOTION: return "joy_axis";
			case SDL_EVENT_JOYSTICK_BUTTON_DOWN: return "joy_down";
			case SDL_EVENT_JOYSTICK_BUTTON_UP: return "joy_up";
			case SDL_EVENT_RENDER_DEVICE_RESET: return "render_device_reset";
			case SDL_EVENT_WILL_ENTER_BACKGROUND: return "app_will_bg";
			case SDL_EVENT_DID_ENTER_BACKGROUND: return "app_did_bg";
			case SDL_EVENT_WILL_ENTER_FOREGROUND: return "app_will_fg";
			case SDL_EVENT_DID_ENTER_FOREGROUND: return "app_did_fg";
			case SDL_EVENT_USER: return "user";
			default: return "other";

		}

	}

	static void ProbeResolveEnabled () {

		if (probeEnabled >= 0) return;

		probeEnabled = 0;

		const char* env = SDL_getenv ("SEIUN_FRAME_PROBE");

		if (env && env[0] && env[0] != '0') probeEnabled = 1;

		#ifdef ANDROID
		if (!probeEnabled) {
			char value[PROP_VALUE_MAX] = { 0 };
			if (__system_property_get ("debug.seiun.frame_probe", value) > 0 && value[0] == '1') probeEnabled = 1;
		}
		#endif

		if (probeEnabled) {

			probeFrameStart = HiResMs ();
			SDL_LogWarn (SDL_LOG_CATEGORY_APPLICATION, "[SEIUN] frame probe enabled");

		}

	}

	static inline void ProbeCountEvent (Uint32 type) {

		if (probeEnabled != 1) return;

		probeEventCount++;
		probeEventTotal++;

		Uint16 smallType = (Uint16)type;

		for (Uint32 i = 0; i < probeTypeKinds; i++) {
			if (probeTypes[i] == smallType) {
				probeTypeCounts[i]++;
				return;
			}
		}

		if (probeTypeKinds < (sizeof (probeTypes) / sizeof (probeTypes[0]))) {
			probeTypes[probeTypeKinds] = smallType;
			probeTypeCounts[probeTypeKinds] = 1;
			probeTypeKinds++;
		}

	}

	// Logs the environment the frame numbers were produced in: swap interval
	// (the real vsync state, so it is verifiable instead of assumed), logical vs
	// pixel window size (i.e. whether the backbuffer is at native device
	// resolution, which is the fillrate question) and the panel refresh rate.
	// Printed with every summary so ONE logcat capture can tell apart
	// input-bound / engine-bound / pacing / too-many-pixels.
	static void ProbeLogEnvironment () {

		int windowCount = 0;
		SDL_Window** windows = SDL_GetWindows (&windowCount);

		if (!windows) return;

		for (int i = 0; i < windowCount; i++) {

			int width = 0;
			int height = 0;
			int pixelWidth = 0;
			int pixelHeight = 0;

			SDL_GetWindowSize (windows[i], &width, &height);
			SDL_GetWindowSizeInPixels (windows[i], &pixelWidth, &pixelHeight);

			const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode (SDL_GetDisplayForWindow (windows[i]));
			double refresh = (mode && mode->refresh_rate > 0.0) ? mode->refresh_rate : 0.0;

			int swapInterval = 0;
			const char* vsync = SDL_GL_GetSwapInterval (&swapInterval) ? (swapInterval == 0 ? "off" : "on") : "n/a";

			SDL_LogWarn (SDL_LOG_CATEGORY_APPLICATION,
				"[SEIUN] env win=%dx%d pixel=%dx%d scale=%.2f refresh=%.0fHz vsync=%s(%d)",
				width, height, pixelWidth, pixelHeight,
				width > 0 ? (double)pixelWidth / (double)width : 1.0, refresh, vsync, swapInterval);

		}

		SDL_free (windows);

	}


	// Called once per frame, right after UPDATE + RENDER were dispatched.
	static void ProbeLogFrame (double framePeriodMs, double drainMs, double workMs) {

		if (probeEnabled != 1) return;

		double now = HiResMs ();
		double delta = now - probeFrameStart;

		probeFrameStart = now;
		probeDrainStart = now;
		probeFrame++;

		if (probeFrame <= 1) return; // discard warmup

		probeDeltaSum += delta;
		if (delta > probeDeltaMax) probeDeltaMax = delta;
		if (drainMs > probeDrainMax) probeDrainMax = drainMs;
		probeWorkSum += workMs;
		if (workMs > probeWorkMax) probeWorkMax = workMs;

		double budget = (framePeriodMs > 0.0) ? framePeriodMs : (1000.0 / 60.0);

		double ratio = delta / budget;

		probeHistogram[
			ratio < 1.0 ? 0 :
			ratio < 1.25 ? 1 :
			ratio < 1.5 ? 2 :
			ratio < 2.0 ? 3 :
			ratio < 3.0 ? 4 :
			ratio < 4.0 ? 5 :
			ratio < 8.0 ? 6 : 7
		]++;

		if (delta > budget * 1.5) {

			probeSlowFrames++;

			// Throttle: at most one detailed dump per 30 frames so the probe
			// itself cannot become the bottleneck on a device that is slow
			// every frame.
			if (probeDumps < 400 && (probeFrame - probeLastDumpFrame) >= 30) {

				probeDumps++;
				probeLastDumpFrame = probeFrame;

				SDL_LogWarn (SDL_LOG_CATEGORY_APPLICATION,
					"[SEIUN] slow frame=%llu delta=%.2fms budget=%.2fms drain=%.2fms work=%.2fms events=%u",
					(unsigned long long)probeFrame, delta, budget, drainMs, workMs, (unsigned)probeEventCount);

				for (Uint32 i = 0; i < probeTypeKinds; i++) {

					SDL_LogWarn (SDL_LOG_CATEGORY_APPLICATION, "[SEIUN]   ev %s(type=0x%04X) x%u",
						ProbeEventName (probeTypes[i]), (unsigned)probeTypes[i], (unsigned)probeTypeCounts[i]);

				}

				if (drainMs > budget * 0.5) {

					SDL_LogWarn (SDL_LOG_CATEGORY_APPLICATION,
						"[SEIUN]   -> event/input handling alone ate %.2fms of this frame", drainMs);

				} else if (workMs > budget) {

					SDL_LogWarn (SDL_LOG_CATEGORY_APPLICATION,
						"[SEIUN]   -> UPDATE+RENDER alone took %.2fms (engine or GPU bound)", workMs);

				}

			}

		}

		if (probeFrame % 300 == 0) {

			SDL_LogWarn (SDL_LOG_CATEGORY_APPLICATION,
				"[SEIUN] summary frame=%llu budget=%.2fms avgDelta=%.2fms maxDelta=%.2fms maxDrain=%.2fms avgWork=%.2fms maxWork=%.2fms slow=%llu/%llu events=%llu hist=%u,%u,%u,%u,%u,%u,%u,%u",
				(unsigned long long)probeFrame, budget, probeDeltaSum / (double)(probeFrame - 1), probeDeltaMax, probeDrainMax,
				probeWorkSum / (double)(probeFrame - 1), probeWorkMax,
				(unsigned long long)probeSlowFrames, (unsigned long long)probeFrame, (unsigned long long)probeEventTotal,
				(unsigned)probeHistogram[0], (unsigned)probeHistogram[1], (unsigned)probeHistogram[2], (unsigned)probeHistogram[3],
				(unsigned)probeHistogram[4], (unsigned)probeHistogram[5], (unsigned)probeHistogram[6], (unsigned)probeHistogram[7]);

			ProbeLogEnvironment ();

		}

		probeEventCount = 0;
		probeTypeKinds = 0;

	}



	SDLApplication::SDLApplication () {

		// SDL3 requires SDL_SetMainReady() before SDL_Init() when not using
		// the SDL_main entry point mechanism (hxcpp provides its own main)
		SDL_SetMainReady ();

		Uint32 initFlags = SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK;
		#if defined(LIME_MOJOAL) || defined(LIME_OPENALSOFT)
		initFlags |= SDL_INIT_AUDIO;
		#endif

		if (!SDL_Init (initFlags)) {

			printf ("Could not initialize SDL: %s.\n", SDL_GetError ());
			exit (1);

		}

		SDL_SetLogPriority (SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_WARN);

		currentApplication = this;

		framePeriod = 1000.0 / 60.0;

		#ifdef EMSCRIPTEN
		emscripten_cancel_main_loop ();
		emscripten_set_main_loop (UpdateFrame, 0, 0);
		emscripten_set_main_loop_timing (EM_TIMING_RAF, 1);
		#endif

		currentUpdate = 0;
		lastUpdate = 0;
		nextUpdate = 0;

		ApplicationEvent applicationEvent;
		ClipboardEvent clipboardEvent;
		DropEvent dropEvent;
		GamepadEvent gamepadEvent;
		JoystickEvent joystickEvent;
		KeyEvent keyEvent;
		MouseEvent mouseEvent;
		RenderEvent renderEvent;
		SensorEvent sensorEvent;
		TextEvent textEvent;
		TouchEvent touchEvent;
		WindowEvent windowEvent;

		SDL_SetEventEnabled (SDL_EVENT_DROP_FILE, true);
		SDLJoystick::Init ();

		#ifdef HX_MACOS
		CFURLRef resourcesURL = CFBundleCopyResourcesDirectoryURL (CFBundleGetMainBundle ());
		char path[PATH_MAX];

		if (CFURLGetFileSystemRepresentation (resourcesURL, TRUE, (UInt8 *)path, PATH_MAX)) {

			chdir (path);

		}

		CFRelease (resourcesURL);
		#endif

	}


	SDLApplication::~SDLApplication () {



	}


	int SDLApplication::Exec () {

		Init ();

		#if defined(IPHONE) || defined(EMSCRIPTEN)

		return 0;

		#else

		while (active) {

			Update ();

		}

		return Quit ();

		#endif

	}


	void SDLApplication::HandleEvent (SDL_Event* event) {

		#if defined(IPHONE) || defined(EMSCRIPTEN)

		int top = 0;
		gc_set_top_of_stack(&top,false);

		#endif

		// SeiunEngine frame probe: account for every event that reaches Haxe.
		ProbeCountEvent (event->type);

		switch (event->type) {

			case SDL_EVENT_USER:

				if (!inBackground) {

					currentUpdate = HiResMs ();
					applicationEvent.type = UPDATE;
					applicationEvent.deltaTime = currentUpdate - lastUpdate;
					lastUpdate = currentUpdate;

					nextUpdate += framePeriod;

					while (nextUpdate <= currentUpdate) {

						nextUpdate += framePeriod;

					}

					double probeRenderStart = HiResMs ();

					ApplicationEvent::Dispatch (&applicationEvent);
					RenderEvent::Dispatch (&renderEvent);

					{
						double probeWorkEnd = HiResMs ();
						ProbeLogFrame (framePeriod, probeRenderStart - probeDrainStart, probeWorkEnd - probeRenderStart);
					}

				}

				break;

			case SDL_EVENT_WILL_ENTER_BACKGROUND:

				inBackground = true;

				windowEvent.type = WINDOW_DEACTIVATE;
				WindowEvent::Dispatch (&windowEvent);
				break;

			case SDL_EVENT_WILL_ENTER_FOREGROUND:

				break;

			case SDL_EVENT_DID_ENTER_FOREGROUND:

				windowEvent.type = WINDOW_ACTIVATE;
				WindowEvent::Dispatch (&windowEvent);

				inBackground = false;
				break;

			case SDL_EVENT_CLIPBOARD_UPDATE:

				ProcessClipboardEvent (event);
				break;

			case SDL_EVENT_GAMEPAD_AXIS_MOTION:
			case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
			case SDL_EVENT_GAMEPAD_BUTTON_UP:
			case SDL_EVENT_GAMEPAD_ADDED:
			case SDL_EVENT_GAMEPAD_REMOVED:

				ProcessGamepadEvent (event);
				break;

			case SDL_EVENT_DROP_FILE:

				ProcessDropEvent (event);
				break;

			case SDL_EVENT_FINGER_MOTION:
			case SDL_EVENT_FINGER_DOWN:
			case SDL_EVENT_FINGER_UP:
			case SDL_EVENT_FINGER_CANCELED:

				ProcessTouchEvent (event);
				break;

			case SDL_EVENT_JOYSTICK_AXIS_MOTION:

				if (SDLJoystick::IsAccelerometer (event->jaxis.which)) {

					ProcessSensorEvent (event);

				} else {

					ProcessJoystickEvent (event);

				}

				break;

			case SDL_EVENT_JOYSTICK_BALL_MOTION:
			case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
			case SDL_EVENT_JOYSTICK_BUTTON_UP:
			case SDL_EVENT_JOYSTICK_HAT_MOTION:
			case SDL_EVENT_JOYSTICK_ADDED:
			case SDL_EVENT_JOYSTICK_REMOVED:

				ProcessJoystickEvent (event);
				break;

			case SDL_EVENT_KEY_DOWN:
			case SDL_EVENT_KEY_UP:

				ProcessKeyEvent (event);
				break;

			case SDL_EVENT_MOUSE_MOTION:
			case SDL_EVENT_MOUSE_BUTTON_DOWN:
			case SDL_EVENT_MOUSE_BUTTON_UP:
			case SDL_EVENT_MOUSE_WHEEL:

				ProcessMouseEvent (event);
				break;

			#ifndef EMSCRIPTEN
			case SDL_EVENT_RENDER_DEVICE_RESET:

				renderEvent.type = RENDER_CONTEXT_LOST;
				RenderEvent::Dispatch (&renderEvent);

				renderEvent.type = RENDER_CONTEXT_RESTORED;
				RenderEvent::Dispatch (&renderEvent);

				renderEvent.type = RENDER;
				break;
			#endif

			case SDL_EVENT_TEXT_INPUT:
			case SDL_EVENT_TEXT_EDITING:

				ProcessTextEvent (event);
				break;

			case SDL_EVENT_WINDOW_MOUSE_ENTER:
			case SDL_EVENT_WINDOW_MOUSE_LEAVE:
			case SDL_EVENT_WINDOW_SHOWN:
			case SDL_EVENT_WINDOW_HIDDEN:
			case SDL_EVENT_WINDOW_FOCUS_GAINED:
			case SDL_EVENT_WINDOW_FOCUS_LOST:
			case SDL_EVENT_WINDOW_MAXIMIZED:
			case SDL_EVENT_WINDOW_MINIMIZED:
			case SDL_EVENT_WINDOW_MOVED:
			case SDL_EVENT_WINDOW_RESTORED:

				ProcessWindowEvent (event);
				break;

			case SDL_EVENT_WINDOW_EXPOSED:

				ProcessWindowEvent (event);

				// SeiunEngine: do NOT force a render here. This event fires on
				// every surface change (resume, IME show/hide, notification
				// shade, rotation, popup) and the main loop already dispatches
				// UPDATE + RENDER as soon as the frame is due. See
				// RenderForWindowEvent() for why the unconditional render was a
				// hitch source on mobile, and for the rollback lever.
				RenderForWindowEvent ();

				break;

			case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
			case SDL_EVENT_WINDOW_RESIZED:

				ProcessWindowEvent (event);

				// SeiunEngine: one Android surface change is reported as BOTH
				// SDL_EVENT_WINDOW_RESIZED and SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED
				// (lib/sdl3/src/events/SDL_windowevents.c -> SDL_OnWindowResized()
				// -> SDL_CheckWindowPixelSizeChanged()), so the old unconditional
				// render here presented two or three full frames - two
				// eglSwapBuffers among them - inside a single main-loop
				// iteration. Gate it on the frame schedule instead.
				RenderForWindowEvent ();

				break;

			case SDL_EVENT_WINDOW_CLOSE_REQUESTED:

				ProcessWindowEvent (event);

				// Avoid handling SDL_EVENT_QUIT if in response to window.close
				SDL_Event event;

				if (SDL_PollEvent (&event)) {

					if (event.type != SDL_EVENT_QUIT) {

						HandleEvent (&event);

					}

				}
				break;

			case SDL_EVENT_QUIT:

				active = false;
				break;

		}

	}


	void SDLApplication::Init () {

		active = true;
		lastUpdate = HiResMs ();
		nextUpdate = lastUpdate;

	}


	void SDLApplication::ProcessClipboardEvent (SDL_Event* event) {

		if (ClipboardEvent::callback) {

			clipboardEvent.type = CLIPBOARD_UPDATE;

			ClipboardEvent::Dispatch (&clipboardEvent);

		}

	}


	void SDLApplication::ProcessDropEvent (SDL_Event* event) {

		if (DropEvent::callback) {

			dropEvent.type = DROP_FILE;
			dropEvent.file = (vbyte*)event->drop.data;

			DropEvent::Dispatch (&dropEvent);

			// SDL3 owns event->drop.data: it is created with
			// SDL_CreateTemporaryString() and freed automatically by
			// SDL_FreeTemporaryMemory() at the start of the next
			// SDL_PumpEvents() call (see SDL_events.c). Unlike SDL2, the
			// application must NOT call SDL_free() on it - doing so causes
			// a double free and heap corruption (the game freezes on drop).
			// If the string must be kept beyond this frame, claim it with
			// SDL_ClaimTemporaryMemory() instead.

		}

	}


	void SDLApplication::ProcessGamepadEvent (SDL_Event* event) {

		if (GamepadEvent::callback) {

			switch (event->type) {

				case SDL_EVENT_GAMEPAD_AXIS_MOTION:

					if (gamepadsAxisMap[event->gaxis.which].empty ()) {

						gamepadsAxisMap[event->gaxis.which][event->gaxis.axis] = event->gaxis.value;

					} else if (gamepadsAxisMap[event->gaxis.which][event->gaxis.axis] == event->gaxis.value) {

						break;

					}

					gamepadEvent.type = GAMEPAD_AXIS_MOVE;
					gamepadEvent.axis = event->gaxis.axis;
					gamepadEvent.id = event->gaxis.which;

					if (event->gaxis.value > -analogAxisDeadZone && event->gaxis.value < analogAxisDeadZone) {

						if (gamepadsAxisMap[event->gaxis.which][event->gaxis.axis] != 0) {

							gamepadsAxisMap[event->gaxis.which][event->gaxis.axis] = 0;
							gamepadEvent.axisValue = 0;
							GamepadEvent::Dispatch (&gamepadEvent);

						}

						break;

					}

					gamepadsAxisMap[event->gaxis.which][event->gaxis.axis] = event->gaxis.value;
					gamepadEvent.axisValue = event->gaxis.value / (event->gaxis.value > 0 ? 32767.0 : 32768.0);

					GamepadEvent::Dispatch (&gamepadEvent);
					break;

				case SDL_EVENT_GAMEPAD_BUTTON_DOWN:

					gamepadEvent.type = GAMEPAD_BUTTON_DOWN;
					gamepadEvent.button = event->gbutton.button;
					gamepadEvent.id = event->gbutton.which;

					GamepadEvent::Dispatch (&gamepadEvent);
					break;

				case SDL_EVENT_GAMEPAD_BUTTON_UP:

					gamepadEvent.type = GAMEPAD_BUTTON_UP;
					gamepadEvent.button = event->gbutton.button;
					gamepadEvent.id = event->gbutton.which;

					GamepadEvent::Dispatch (&gamepadEvent);
					break;

				case SDL_EVENT_GAMEPAD_ADDED:

					if (SDLGamepad::Connect (event->gdevice.which)) {

						gamepadEvent.type = GAMEPAD_CONNECT;
						gamepadEvent.id = SDLGamepad::GetInstanceID (event->gdevice.which);

						GamepadEvent::Dispatch (&gamepadEvent);

					}

					break;

				case SDL_EVENT_GAMEPAD_REMOVED: {

					gamepadEvent.type = GAMEPAD_DISCONNECT;
					gamepadEvent.id = event->gdevice.which;

					GamepadEvent::Dispatch (&gamepadEvent);
					SDLGamepad::Disconnect (event->gdevice.which);
					break;

				}

			}

		}

	}


	void SDLApplication::ProcessJoystickEvent (SDL_Event* event) {

		if (JoystickEvent::callback) {

			switch (event->type) {

				case SDL_EVENT_JOYSTICK_AXIS_MOTION:

					if (!SDLJoystick::IsAccelerometer (event->jaxis.which)) {

						joystickEvent.type = JOYSTICK_AXIS_MOVE;
						joystickEvent.index = event->jaxis.axis;
						joystickEvent.x = event->jaxis.value / (event->jaxis.value > 0 ? 32767.0 : 32768.0);
						joystickEvent.id = event->jaxis.which;

						JoystickEvent::Dispatch (&joystickEvent);

					}
					break;

				case SDL_EVENT_JOYSTICK_BALL_MOTION:

					if (!SDLJoystick::IsAccelerometer (event->jball.which)) {

						joystickEvent.type = JOYSTICK_TRACKBALL_MOVE;
						joystickEvent.index = event->jball.ball;
						joystickEvent.x = event->jball.xrel / (event->jball.xrel > 0 ? 32767.0 : 32768.0);
						joystickEvent.y = event->jball.yrel / (event->jball.yrel > 0 ? 32767.0 : 32768.0);
						joystickEvent.id = event->jball.which;

						JoystickEvent::Dispatch (&joystickEvent);

					}
					break;

				case SDL_EVENT_JOYSTICK_BUTTON_DOWN:

					if (!SDLJoystick::IsAccelerometer (event->jbutton.which)) {

						joystickEvent.type = JOYSTICK_BUTTON_DOWN;
						joystickEvent.index = event->jbutton.button;
						joystickEvent.id = event->jbutton.which;

						JoystickEvent::Dispatch (&joystickEvent);

					}
					break;

				case SDL_EVENT_JOYSTICK_BUTTON_UP:

					if (!SDLJoystick::IsAccelerometer (event->jbutton.which)) {

						joystickEvent.type = JOYSTICK_BUTTON_UP;
						joystickEvent.index = event->jbutton.button;
						joystickEvent.id = event->jbutton.which;

						JoystickEvent::Dispatch (&joystickEvent);

					}
					break;

				case SDL_EVENT_JOYSTICK_HAT_MOTION:

					if (!SDLJoystick::IsAccelerometer (event->jhat.which)) {

						joystickEvent.type = JOYSTICK_HAT_MOVE;
						joystickEvent.index = event->jhat.hat;
						joystickEvent.eventValue = event->jhat.value;
						joystickEvent.id = event->jhat.which;

						JoystickEvent::Dispatch (&joystickEvent);

					}
					break;

				case SDL_EVENT_JOYSTICK_ADDED:

					if (SDLJoystick::Connect (event->jdevice.which)) {

						joystickEvent.type = JOYSTICK_CONNECT;
						joystickEvent.id = SDLJoystick::GetInstanceID (event->jdevice.which);

						JoystickEvent::Dispatch (&joystickEvent);

					}
					break;

				case SDL_EVENT_JOYSTICK_REMOVED:

					if (!SDLJoystick::IsAccelerometer (event->jdevice.which)) {

						joystickEvent.type = JOYSTICK_DISCONNECT;
						joystickEvent.id = event->jdevice.which;

						JoystickEvent::Dispatch (&joystickEvent);
						SDLJoystick::Disconnect (event->jdevice.which);

					}
					break;

			}

		}

	}


	void SDLApplication::ProcessKeyEvent (SDL_Event* event) {

		if (KeyEvent::callback) {

			switch (event->type) {

				case SDL_EVENT_KEY_DOWN: keyEvent.type = KEY_DOWN; break;
				case SDL_EVENT_KEY_UP: keyEvent.type = KEY_UP; break;

			}

			keyEvent.keyCode = event->key.key;
			keyEvent.modifier = event->key.mod;
			keyEvent.windowID = event->key.windowID;

			if (keyEvent.type == KEY_DOWN) {

				if (keyEvent.keyCode == SDLK_CAPSLOCK) keyEvent.modifier |= SDL_KMOD_CAPS;
				if (keyEvent.keyCode == SDLK_LALT) keyEvent.modifier |= SDL_KMOD_LALT;
				if (keyEvent.keyCode == SDLK_LCTRL) keyEvent.modifier |= SDL_KMOD_LCTRL;
				if (keyEvent.keyCode == SDLK_LGUI) keyEvent.modifier |= SDL_KMOD_LGUI;
				if (keyEvent.keyCode == SDLK_LSHIFT) keyEvent.modifier |= SDL_KMOD_LSHIFT;
				if (keyEvent.keyCode == SDLK_MODE) keyEvent.modifier |= SDL_KMOD_MODE;
				if (keyEvent.keyCode == SDLK_NUMLOCKCLEAR) keyEvent.modifier |= SDL_KMOD_NUM;
				if (keyEvent.keyCode == SDLK_RALT) keyEvent.modifier |= SDL_KMOD_RALT;
				if (keyEvent.keyCode == SDLK_RCTRL) keyEvent.modifier |= SDL_KMOD_RCTRL;
				if (keyEvent.keyCode == SDLK_RGUI) keyEvent.modifier |= SDL_KMOD_RGUI;
				if (keyEvent.keyCode == SDLK_RSHIFT) keyEvent.modifier |= SDL_KMOD_RSHIFT;

			}

			KeyEvent::Dispatch (&keyEvent);

		}

	}


	void SDLApplication::ProcessMouseEvent (SDL_Event* event) {

		if (MouseEvent::callback) {

			switch (event->type) {

				case SDL_EVENT_MOUSE_MOTION:

					mouseEvent.type = MOUSE_MOVE;
					mouseEvent.x = event->motion.x;
					mouseEvent.y = event->motion.y;
					mouseEvent.movementX = event->motion.xrel;
					mouseEvent.movementY = event->motion.yrel;
					break;

				case SDL_EVENT_MOUSE_BUTTON_DOWN:

					SDL_CaptureMouse (true);

					mouseEvent.type = MOUSE_DOWN;
					mouseEvent.button = event->button.button - 1;
					mouseEvent.x = event->button.x;
					mouseEvent.y = event->button.y;
					break;

				case SDL_EVENT_MOUSE_BUTTON_UP:

					SDL_CaptureMouse (false);

					mouseEvent.type = MOUSE_UP;
					mouseEvent.button = event->button.button - 1;
					mouseEvent.x = event->button.x;
					mouseEvent.y = event->button.y;
					break;

				case SDL_EVENT_MOUSE_WHEEL:

					mouseEvent.type = MOUSE_WHEEL;

					if (event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {

						mouseEvent.x = -event->wheel.x;
						mouseEvent.y = -event->wheel.y;

					} else {

						mouseEvent.x = event->wheel.x;
						mouseEvent.y = event->wheel.y;

					}
					break;

			}

			mouseEvent.windowID = event->button.windowID;
			MouseEvent::Dispatch (&mouseEvent);

		}

	}


	void SDLApplication::ProcessSensorEvent (SDL_Event* event) {

		if (SensorEvent::callback) {

			double value = event->jaxis.value / 32767.0f;

			switch (event->jaxis.axis) {

				case 0: sensorEvent.x = value; break;
				case 1: sensorEvent.y = value; break;
				case 2: sensorEvent.z = value; break;
				default: break;

			}

			SensorEvent::Dispatch (&sensorEvent);

		}

	}


	void SDLApplication::ProcessTextEvent (SDL_Event* event) {

		if (TextEvent::callback) {

			switch (event->type) {

				case SDL_EVENT_TEXT_INPUT:

					textEvent.type = TEXT_INPUT;
					break;

				case SDL_EVENT_TEXT_EDITING:

					textEvent.type = TEXT_EDIT;
					textEvent.start = event->edit.start;
					textEvent.length = event->edit.length;
					break;

			}

			if (textEvent.text) {

				free (textEvent.text);

			}

			textEvent.text = (vbyte*)malloc (strlen (event->text.text) + 1);
			strcpy ((char*)textEvent.text, event->text.text);

			textEvent.windowID = event->text.windowID;
			TextEvent::Dispatch (&textEvent);

		}

	}


	void SDLApplication::ProcessTouchEvent (SDL_Event* event) {

		if (TouchEvent::callback) {

			switch (event->type) {

				case SDL_EVENT_FINGER_MOTION:

					touchEvent.type = TOUCH_MOVE;
					break;

				case SDL_EVENT_FINGER_DOWN:

					touchEvent.type = TOUCH_START;
					break;

				case SDL_EVENT_FINGER_UP:
				// 取消的触摸视同抬起: SDL 内部已删除该手指, 上层必须同样结束它,
				// 否则 lime currentTouches / flixel FlxTouch 会永久残留。
				case SDL_EVENT_FINGER_CANCELED:

					touchEvent.type = TOUCH_END;
					break;

			}

			touchEvent.x = event->tfinger.x;
			touchEvent.y = event->tfinger.y;
			touchEvent.id = event->tfinger.fingerID;
			touchEvent.dx = event->tfinger.dx;
			touchEvent.dy = event->tfinger.dy;
			touchEvent.pressure = event->tfinger.pressure;
			touchEvent.device = event->tfinger.touchID;

			TouchEvent::Dispatch (&touchEvent);

		}

	}


	void SDLApplication::ProcessWindowEvent (SDL_Event* event) {

		if (WindowEvent::callback) {

			switch (event->type) {

				case SDL_EVENT_WINDOW_SHOWN: windowEvent.type = WINDOW_ACTIVATE; break;
				case SDL_EVENT_WINDOW_CLOSE_REQUESTED: windowEvent.type = WINDOW_CLOSE; break;
				case SDL_EVENT_WINDOW_HIDDEN: windowEvent.type = WINDOW_DEACTIVATE; break;
				case SDL_EVENT_WINDOW_MOUSE_ENTER: windowEvent.type = WINDOW_ENTER; break;
				case SDL_EVENT_WINDOW_FOCUS_GAINED: windowEvent.type = WINDOW_FOCUS_IN; break;
				case SDL_EVENT_WINDOW_FOCUS_LOST: windowEvent.type = WINDOW_FOCUS_OUT; break;
				case SDL_EVENT_WINDOW_MOUSE_LEAVE: windowEvent.type = WINDOW_LEAVE; break;
				case SDL_EVENT_WINDOW_MAXIMIZED: windowEvent.type = WINDOW_MAXIMIZE; break;
				case SDL_EVENT_WINDOW_MINIMIZED: windowEvent.type = WINDOW_MINIMIZE; break;
				case SDL_EVENT_WINDOW_EXPOSED: windowEvent.type = WINDOW_EXPOSE; break;

				case SDL_EVENT_WINDOW_MOVED:

					windowEvent.type = WINDOW_MOVE;
					windowEvent.x = event->window.data1;
					windowEvent.y = event->window.data2;
					break;

				case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
				case SDL_EVENT_WINDOW_RESIZED:

					windowEvent.type = WINDOW_RESIZE;

					// SDL_EVENT_WINDOW_RESIZED is in screen coordinates (points
					// on macOS), but SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED is in
					// physical pixels (2x on Retina displays). Lime window sizes
					// are in screen coordinates, so resolve pixel-size events
					// back to the window size instead of using the event data.
					if (event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {

						SDL_Window* resizedWindow = SDL_GetWindowFromID (event->window.windowID);

						if (resizedWindow) {

							SDL_GetWindowSize (resizedWindow, &windowEvent.width, &windowEvent.height);
							break;

						}

					}

					windowEvent.width = event->window.data1;
					windowEvent.height = event->window.data2;
					break;

				case SDL_EVENT_WINDOW_RESTORED: windowEvent.type = WINDOW_RESTORE; break;

			}

			windowEvent.windowID = event->window.windowID;
			WindowEvent::Dispatch (&windowEvent);

		}

	}


	int SDLApplication::Quit () {

		applicationEvent.type = EXIT;
		ApplicationEvent::Dispatch (&applicationEvent);

		SDL_Quit ();

		return 0;

	}


	void SDLApplication::RegisterWindow (SDLWindow *window) {

		#ifdef IPHONE
		SDL_SetiOSAnimationCallback (window->sdlWindow, 1, UpdateFrame, NULL);
		#endif

	}


	void SDLApplication::SetFrameRate (double frameRate) {

		if (frameRate > 0) {

			framePeriod = 1000.0 / frameRate;

		} else {

			framePeriod = 1000.0;

		}

	}


	static SDL_TimerID timerID = 0;
	bool timerActive = false;
	bool firstTime = true;

	Uint32 OnTimer (void*, SDL_TimerID, Uint32 interval) {

		SDL_Event event;
		SDL_UserEvent userevent;
		userevent.type = SDL_EVENT_USER;
		userevent.code = 0;
		userevent.data1 = NULL;
		userevent.data2 = NULL;
		event.type = SDL_EVENT_USER;
		event.user = userevent;

		timerActive = false;
		timerID = 0;

		SDL_PushEvent (&event);

		return 0;

	}


	// Dispatch UPDATE + RENDER because a window event (expose/resize) asked for
	// one, but only when a frame is actually due.
	//
	// Why this exists: on Android a single surface change is reported as *two*
	// SDL events. The Android backend posts SDL_EVENT_WINDOW_RESIZED, and SDL's
	// own SDL_OnWindowResized() then calls SDL_CheckWindowPixelSizeChanged(),
	// which posts SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED as well (see
	// lib/sdl3/src/events/SDL_windowevents.c and lib/sdl3/src/video/SDL_video.c).
	// The previous code dispatched a full RenderEvent for each of them, i.e. two
	// or three presented frames - two eglSwapBuffers among them - inside one
	// main-loop iteration. On a phone that is a visible hitch every time the
	// window state changes (resume, IME show/hide, notification shade, rotation,
	// a popup dialog), and it also runs the OpenFL stage render + framebuffer
	// reallocation several times for the same resize.
	//
	// The main loop already dispatches UPDATE + RENDER whenever a frame is due,
	// immediately after the event drain loop, so an immediate render is only
	// needed when the event arrived *after* the frame became due. That is the
	// only case handled here, and nextUpdate is advanced exactly the way
	// Update() does it so the loop does not emit a second frame for the same
	// period.
	bool SDLApplication::RenderForWindowEvent () {

		if (SeiunLegacyRenderEvents ()) {

			// Rollback lever (debug.seiun.legacy_render_events=1): the pre-fix
			// behaviour -- render immediately for every expose/resize event,
			// even when a frame is not due. One Android surface change produces
			// two of those events, so this is what presented 2-3 frames (and
			// two or three eglSwapBuffers) inside a single main-loop iteration.
			if (inBackground) return false;

			RenderEvent::Dispatch (&renderEvent);
			return true;

		}

		if (inBackground) return false;

		double now = HiResMs ();

		if (now < nextUpdate) return false;

		#if defined(IPHONE) || defined(EMSCRIPTEN)
		// The legacy path drives frames through SDL_EVENT_USER; cancel a timer
		// that is already armed so it cannot push a second frame.
		if (timerActive) {
			SDL_RemoveTimer (timerID);
			timerActive = false;
			timerID = 0;
		}
		#endif

		int catchup = 0;

		do {
			nextUpdate += framePeriod;
			catchup++;
		} while (nextUpdate <= now && catchup < 4);

		if (catchup >= 4) {
			nextUpdate = now + framePeriod;
		}

		applicationEvent.type = UPDATE;
		applicationEvent.deltaTime = now - lastUpdate;
		lastUpdate = now;

		double probeRenderStart = HiResMs ();

		ApplicationEvent::Dispatch (&applicationEvent);
		RenderEvent::Dispatch (&renderEvent);

		{
			double probeWorkEnd = HiResMs ();
			ProbeLogFrame (framePeriod, probeRenderStart - probeDrainStart, probeWorkEnd - probeRenderStart);
		}

		return true;

	}


	// Android is NOT in the IPHONE/EMSCRIPTEN branch below, so Android uses the
	// wall-clock scheduler. The probe and the resize gating above both rely on
	// that.
	bool SDLApplication::Update () {

		SDL_Event event;
		event.type = -1;

		ProbeResolveEnabled ();
		probeDrainStart = HiResMs ();

		#if (!defined (IPHONE) && !defined (EMSCRIPTEN))

		while (SDL_PollEvent (&event)) {

			if (event.type != SDL_EVENT_USER) {

				HandleEvent (&event);

			}

			event.type = -1;

			if (!active)
				return active;

		}

		// SeiunEngine frame probe: how much of this frame was spent merely
		// absorbing input events, before the engine was even called.
		double probeDrainEnd = HiResMs ();

		currentUpdate = HiResMs ();

		if (!active)
			return active;

		if (currentUpdate >= nextUpdate) {

			// Due frame: advance by wall clock; resync after long stalls to avoid catch-up bursts.
			int catchup = 0;

			do {

				nextUpdate += framePeriod;
				catchup++;

			} while (nextUpdate <= currentUpdate && catchup < 4);

			if (catchup >= 4) {
				nextUpdate = currentUpdate + framePeriod;
			}

			applicationEvent.type = UPDATE;
			applicationEvent.deltaTime = currentUpdate - lastUpdate;
			lastUpdate = currentUpdate;

			ApplicationEvent::Dispatch (&applicationEvent);
			RenderEvent::Dispatch (&renderEvent);

			{
				double probeWorkEnd = HiResMs ();
				ProbeLogFrame (framePeriod, probeDrainEnd - probeDrainStart, probeWorkEnd - probeDrainEnd);
			}

		} else if (!inBackground && nextUpdate > currentUpdate) {

			double remainMs = nextUpdate - currentUpdate;

			// SeiunEngine: with the vsync diagnostic lever on, the DISPLAY is the
			// frame pacer - eglSwapBuffers already blocks until the next vblank -
			// so the sub-3ms NS-sleep + busy spin below would just burn CPU
			// before blocking in the swap anyway, and would leave lime's
			// scheduler and the panel both trying to align the same frame. Hand
			// the whole remainder to the event wait instead. With the lever off
			// this branch is unchanged (default behaviour).
			bool displayPaced = SeiunForceVSync ();

			if (remainMs > 3.0 || displayPaced) {

				// Long wait: timed event wait keeps input responsive.
				int timeout = (int) (remainMs - 2.0);

				if (displayPaced && timeout < 1) timeout = 1;

				if (timeout > 0 && WaitEventTimeout (&event, timeout)) {

					if (event.type != SDL_EVENT_USER) {
						HandleEvent (&event);
					}

				}

			} else {

				// Final <=3ms alignment: NS sleep plus a short spin to remove the 1ms floor.
				while ((remainMs = nextUpdate - HiResMs ()) > 0) {

					if (remainMs > 0.55) {
						SDL_DelayNS ((Uint64) ((remainMs - 0.30) * 1000000.0));
					} else {
						while (nextUpdate - HiResMs () > 0) { }
						break;
					}

				}

			}

		} else {

			// Background: block until a real event rather than burning CPU.
			if (WaitEvent (&event) && event.type != SDL_EVENT_USER) {
				HandleEvent (&event);
			}

		}

		return active;

		#else

		// Original IPHONE / EMSCRIPTEN path.
		if (active && (firstTime || WaitEvent (&event))) {

			firstTime = false;

			HandleEvent (&event);
			event.type = -1;
			if (!active)
				return active;

			while (SDL_PollEvent (&event)) {

				HandleEvent (&event);
				event.type = -1;
				if (!active)
					return active;

			}

			currentUpdate = SDL_GetTicks ();

			if (currentUpdate >= nextUpdate) {

				if (timerActive) SDL_RemoveTimer (timerID);
				OnTimer (0, 0, 0);

			} else if (!timerActive) {

				timerActive = true;
				timerID = SDL_AddTimer ((Uint32) (nextUpdate - currentUpdate), OnTimer, 0);

			}

		}

		return active;

		#endif

	}


	void SDLApplication::UpdateFrame () {

		currentApplication->Update ();

	}


	void SDLApplication::UpdateFrame (void*) {

		UpdateFrame ();

	}


	int SDLApplication::WaitEvent (SDL_Event *event) {

		#if defined(HX_MACOS) || defined(ANDROID)

		System::GCEnterBlocking ();
		int result = SDL_WaitEvent (event);
		System::GCExitBlocking ();
		return result;

		#else

		bool isBlocking = false;

		for(;;) {

			SDL_PumpEvents ();

			switch (SDL_PeepEvents (event, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST)) {

				case -1:

					if (isBlocking) System::GCExitBlocking ();
					return 0;

				case 1:

					if (isBlocking) System::GCExitBlocking ();
					return 1;

				default:

					if (!isBlocking) System::GCEnterBlocking ();
					isBlocking = true;
					SDL_Delay (1);
					break;

			}

		}

		#endif

	}


	// Wait up to timeout ms for an event; returns 1 on event, 0 on timeout.
	int SDLApplication::WaitEventTimeout (SDL_Event *event, int timeout) {

		if (timeout <= 0) return 0;

		#if defined(HX_MACOS) || defined(ANDROID)

		System::GCEnterBlocking ();
		int result = SDL_WaitEventTimeout (event, timeout);
		System::GCExitBlocking ();
		return result;

		#else

		bool isBlocking = false;
		Uint32 deadline = SDL_GetTicks () + (Uint32)timeout;

		for(;;) {

			SDL_PumpEvents ();

			switch (SDL_PeepEvents (event, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST)) {

				case -1:

					if (isBlocking) System::GCExitBlocking ();
					return 0;

				case 1:

					if (isBlocking) System::GCExitBlocking ();
					return 1;

				default:

					if ((Sint32)(deadline - SDL_GetTicks ()) <= 0) {
						if (isBlocking) System::GCExitBlocking ();
						return 0;
					}

					if (!isBlocking) System::GCEnterBlocking ();
					isBlocking = true;
					SDL_Delay (1);
					break;

			}

		}

		#endif

	}


	Application* CreateApplication () {

		return new SDLApplication ();

	}


}


#ifdef ANDROID
int SDL_main (int argc, char *argv[]) { return 0; }
#endif
