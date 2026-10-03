#ifndef LIME_SDL_APPLICATION_H
#define LIME_SDL_APPLICATION_H


#include <SDL3/SDL.h>
#include <app/Application.h>
#include <app/ApplicationEvent.h>
#include <graphics/RenderEvent.h>
#include <system/ClipboardEvent.h>
#include <system/SensorEvent.h>
#include <ui/DropEvent.h>
#include <ui/GamepadEvent.h>
#include <ui/JoystickEvent.h>
#include <ui/KeyEvent.h>
#include <ui/MouseEvent.h>
#include <ui/TextEvent.h>
#include <ui/TouchEvent.h>
#include <ui/WindowEvent.h>
#include "SDLWindow.h"

#ifdef ANDROID
#include <sys/system_properties.h>
#endif


namespace lime {


	// SeiunEngine: opt-in, no-rebuild diagnostic switches.
	//
	// Every switch reads SEIUN_<NAME> from the environment, or the Android system
	// property debug.seiun.<name>, and is enabled only by a value starting with
	// '1'. They exist so a behaviour change can be A/B tested on a real phone with
	// one adb command instead of a rebuild, and so any change can be rolled back
	// on-device without shipping a new APK. **The default of every switch is the
	// pre-change behaviour.**
	//
	//   adb shell setprop debug.seiun.<name> 1     (then force-stop + restart)
	//
	// See temp/perf/lime-mobile-findings.md section 6.6 for the full switch table.
	inline bool SeiunLever (const char* envName, const char* propertyName) {

		const char* env = SDL_getenv (envName);

		if (env && env[0] == '1') return true;

		#ifdef ANDROID
		{
			char value[PROP_VALUE_MAX] = { 0 };
			if (__system_property_get (propertyName, value) > 0 && value[0] == '1') return true;
		}
		#endif

		return false;

	}


	// Reads a lever once per process and logs the first decision, so callers can
	// test it every frame (cheap) without paying for a system property lookup per
	// frame or spamming logcat.
	inline bool SeiunLeverCached (const char* envName, const char* propertyName, int* cache, const char* logMessage) {

		if (*cache < 0) {

			*cache = SeiunLever (envName, propertyName) ? 1 : 0;

			if (*cache && logMessage) {
				SDL_LogWarn (SDL_LOG_CATEGORY_APPLICATION, "%s", logMessage);
			}

		}

		return *cache == 1;

	}


	// E1 diagnostic: force SDL_GL_SetSwapInterval(1) at window creation so the
	// display drives frame pacing instead of lime's software wall-clock scheduler,
	// and let Update() skip its sub-3ms spin alignment (otherwise the scheduler
	// and the panel both try to align the same frame). NOT a shipping default --
	// with vsync on, eglSwapBuffers blocks until the next vblank, so if the
	// engine's framePeriod does not match the real panel refresh rate the probe
	// reports ~100% slow frames, which is itself the finding.
	inline bool SeiunForceVSync () {

		static int cache = -1;

		return SeiunLeverCached ("SEIUN_VSYNC", "debug.seiun.vsync", &cache,
			"[SEIUN] forcing SDL_GL_SetSwapInterval(1)");

	}


	// R1 rollback: restore the pre-fix behaviour of rendering once for EVERY
	// SDL_EVENT_WINDOW_EXPOSED / _PIXEL_SIZE_CHANGED / _RESIZED, instead of gating
	// that on the frame schedule. One Android surface change is reported as two of
	// those events, so the legacy path presents 2-3 frames per iteration.
	inline bool SeiunLegacyRenderEvents () {

		static int cache = -1;

		return SeiunLeverCached ("SEIUN_LEGACY_RENDER_EVENTS", "debug.seiun.legacy_render_events", &cache,
			"[SEIUN] legacy unconditional render on window events restored");

	}


	class SDLApplication : public Application {

		public:

			SDLApplication ();
			~SDLApplication ();

			virtual int Exec ();
			virtual void Init ();
			virtual int Quit ();
			virtual void SetFrameRate (double frameRate);
			virtual bool Update ();

			void RegisterWindow (SDLWindow *window);

		private:

			void HandleEvent (SDL_Event* event);
			bool RenderForWindowEvent ();
			void ProcessClipboardEvent (SDL_Event* event);
			void ProcessDropEvent (SDL_Event* event);
			void ProcessGamepadEvent (SDL_Event* event);
			void ProcessJoystickEvent (SDL_Event* event);
			void ProcessKeyEvent (SDL_Event* event);
			void ProcessMouseEvent (SDL_Event* event);
			void ProcessSensorEvent (SDL_Event* event);
			void ProcessTextEvent (SDL_Event* event);
			void ProcessTouchEvent (SDL_Event* event);
			void ProcessWindowEvent (SDL_Event* event);
			int WaitEvent (SDL_Event* event);
			int WaitEventTimeout (SDL_Event* event, int timeout);

			static void UpdateFrame ();
			static void UpdateFrame (void*);

			static SDLApplication* currentApplication;

			bool active;
			ApplicationEvent applicationEvent;
			ClipboardEvent clipboardEvent;
			double currentUpdate; // Performance-counter time in ms.
			double framePeriod;
			DropEvent dropEvent;
			GamepadEvent gamepadEvent;
			JoystickEvent joystickEvent;
			KeyEvent keyEvent;
			double lastUpdate; // Performance-counter time in ms.
			MouseEvent mouseEvent;
			double nextUpdate; // Double precision avoids truncating the frame period.
			RenderEvent renderEvent;
			SensorEvent sensorEvent;
			TextEvent textEvent;
			TouchEvent touchEvent;
			WindowEvent windowEvent;

	};


}


#endif
