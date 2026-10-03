#include "SDLWindow.h"
#include "SDLCursor.h"
#include "SDLApplication.h"
#include "../../graphics/opengl/OpenGL.h"
#include "../../graphics/opengl/OpenGLBindings.h"

#ifdef HX_WINDOWS
#include <SDL3/SDL_properties.h>
#include <Windows.h>
#undef CreateWindow
#endif

namespace lime {


	// SeiunEngine: opt-in switch for SDL's touch -> mouse event synthesis.
	//
	// By default SDL reports every finger contact twice: once as a touch event
	// (SDL_EVENT_FINGER_*) and once as a synthesized mouse event
	// (SDL_EVENT_MOUSE_* with which == SDL_TOUCH_MOUSEID). lime hands both of
	// them to Haxe, so one tap costs two complete input pipelines - two event
	// objects, two OpenFL hit tests, two Flixel input-manager updates - and a
	// drag costs two per motion sample.
	//
	// Games that read touches directly (Psych Engine's on-screen controls go
	// through FlxG.touches) can turn the synthesis off and roughly halve their
	// per-touch input cost. It is opt-in, with no change to the default,
	// because code that only listens to FlxG.mouse would stop seeing taps:
	//
	//   Android: adb shell setprop debug.seiun.no_touch_mouse 1  (restart app)
	//   Other:   set SEIUN_NO_TOUCH_MOUSE=1
	static const char* SeiunTouchMouseEventsHint () {

		static int resolved = -1;

		if (resolved < 0) {

			SeiunLeverCached ("SEIUN_NO_TOUCH_MOUSE", "debug.seiun.no_touch_mouse", &resolved,
				"[SEIUN] touch->mouse synthesis disabled");

		}

		return resolved ? "0" : "1";

	}


	static Cursor currentCursor = DEFAULT;

	SDL_Cursor* SDLCursor::arrowCursor = 0;
	SDL_Cursor* SDLCursor::crosshairCursor = 0;
	SDL_Cursor* SDLCursor::moveCursor = 0;
	SDL_Cursor* SDLCursor::pointerCursor = 0;
	SDL_Cursor* SDLCursor::resizeNESWCursor = 0;
	SDL_Cursor* SDLCursor::resizeNSCursor = 0;
	SDL_Cursor* SDLCursor::resizeNWSECursor = 0;
	SDL_Cursor* SDLCursor::resizeWECursor = 0;
	SDL_Cursor* SDLCursor::textCursor = 0;
	SDL_Cursor* SDLCursor::waitCursor = 0;
	SDL_Cursor* SDLCursor::waitArrowCursor = 0;

	static bool displayModeSet = false;

#if defined (HX_WINDOWS) && !defined (HX_WINRT)
	static HWND GetWin32Window (SDL_Window* sdlWindow) {

		if (!sdlWindow) return NULL;
		return (HWND)SDL_GetPointerProperty (SDL_GetWindowProperties (sdlWindow), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);

	}
#endif


	SDLWindow::SDLWindow (Application* application, int width, int height, int flags, const char* title) {

		sdlTexture = 0;
		sdlRenderer = 0;
		context = 0;

		contextWidth = 0;
		contextHeight = 0;

		currentApplication = application;
		this->flags = flags;

		Uint64 sdlWindowFlags = 0;

		if (flags & WINDOW_FLAG_FULLSCREEN) sdlWindowFlags |= SDL_WINDOW_FULLSCREEN;
		if (flags & WINDOW_FLAG_RESIZABLE) sdlWindowFlags |= SDL_WINDOW_RESIZABLE;
		if (flags & WINDOW_FLAG_BORDERLESS) sdlWindowFlags |= SDL_WINDOW_BORDERLESS;
		if (flags & WINDOW_FLAG_HIDDEN) sdlWindowFlags |= SDL_WINDOW_HIDDEN;
		if (flags & WINDOW_FLAG_MINIMIZED) sdlWindowFlags |= SDL_WINDOW_MINIMIZED;
		if (flags & WINDOW_FLAG_MAXIMIZED) sdlWindowFlags |= SDL_WINDOW_MAXIMIZED;

		#ifndef EMSCRIPTEN
		if (flags & WINDOW_FLAG_ALWAYS_ON_TOP) sdlWindowFlags |= SDL_WINDOW_ALWAYS_ON_TOP;
		#endif

		#if defined (HX_WINDOWS) && defined (NATIVE_TOOLKIT_SDL_ANGLE) && !defined (HX_WINRT)
		OSVERSIONINFOEXW osvi = { sizeof (osvi), 0, 0, 0, 0, {0}, 0, 0 };
		DWORDLONG const dwlConditionMask = VerSetConditionMask (VerSetConditionMask (VerSetConditionMask (0, VER_MAJORVERSION, VER_GREATER_EQUAL), VER_MINORVERSION, VER_GREATER_EQUAL), VER_SERVICEPACKMAJOR, VER_GREATER_EQUAL);
		osvi.dwMajorVersion = HIBYTE (_WIN32_WINNT_VISTA);
		osvi.dwMinorVersion = LOBYTE (_WIN32_WINNT_VISTA);
		osvi.wServicePackMajor = 0;

		if (VerifyVersionInfoW (&osvi, VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR, dwlConditionMask) == FALSE) {

			flags &= ~WINDOW_FLAG_HARDWARE;

		}
		#endif

		#if !defined(EMSCRIPTEN) && !defined(LIME_SWITCH)
		SDL_SetHint (SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "0");
		SDL_SetHint (SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
		SDL_SetHint (SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
		// SeiunEngine: "1" (SDL's default) makes every touch arrive twice, once
		// as a touch event and once as a synthesized mouse event. See
		// SeiunTouchMouseEventsHint() above for the opt-in switch.
		SDL_SetHint (SDL_HINT_TOUCH_MOUSE_EVENTS, SeiunTouchMouseEventsHint ());
		#endif

		if (flags & WINDOW_FLAG_HARDWARE) {

			sdlWindowFlags |= SDL_WINDOW_OPENGL;

			if (flags & WINDOW_FLAG_ALLOW_HIGHDPI) {

				sdlWindowFlags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;

			}

			#if defined (HX_WINDOWS) && defined (NATIVE_TOOLKIT_SDL_ANGLE)
			SDL_GL_SetAttribute (SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
			SDL_GL_SetAttribute (SDL_GL_CONTEXT_MAJOR_VERSION, 2);
			SDL_GL_SetAttribute (SDL_GL_CONTEXT_MINOR_VERSION, 0);
			SDL_SetHint (SDL_HINT_VIDEO_WIN_D3DCOMPILER, "d3dcompiler_47.dll");
			#endif

			#if defined (RASPBERRYPI)
			SDL_GL_SetAttribute (SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
			SDL_GL_SetAttribute (SDL_GL_CONTEXT_MAJOR_VERSION, 2);
			SDL_GL_SetAttribute (SDL_GL_CONTEXT_MINOR_VERSION, 0);
			SDL_SetHint (SDL_HINT_RENDER_DRIVER, "opengles2");
			#endif

			#if defined (IPHONE) || defined (APPLETV)
			SDL_GL_SetAttribute (SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
			SDL_GL_SetAttribute (SDL_GL_CONTEXT_MAJOR_VERSION, 3);
			#endif

			if (flags & WINDOW_FLAG_DEPTH_BUFFER) {

				// SeiunEngine: the original expression was
				//   32 - (flags & WINDOW_FLAG_STENCIL_BUFFER) ? 8 : 0
				// which parses as ((32 - x) != 0) ? 8 : 0 and therefore always
				// asked for an 8-bit depth buffer, never the intended 24/32-bit
				// one, regardless of the stencil flag. Parenthesised the same way
				// upstream lime fixed it.
				SDL_GL_SetAttribute (SDL_GL_DEPTH_SIZE, 32 - ((flags & WINDOW_FLAG_STENCIL_BUFFER) ? 8 : 0));

			}

			if (flags & WINDOW_FLAG_STENCIL_BUFFER) {

				SDL_GL_SetAttribute (SDL_GL_STENCIL_SIZE, 8);

			}

			if (flags & WINDOW_FLAG_HW_AA_HIRES) {

				SDL_GL_SetAttribute (SDL_GL_MULTISAMPLEBUFFERS, true);
				SDL_GL_SetAttribute (SDL_GL_MULTISAMPLESAMPLES, 4);

			} else if (flags & WINDOW_FLAG_HW_AA) {

				SDL_GL_SetAttribute (SDL_GL_MULTISAMPLEBUFFERS, true);
				SDL_GL_SetAttribute (SDL_GL_MULTISAMPLESAMPLES, 2);

			}

			if (flags & WINDOW_FLAG_COLOR_DEPTH_32_BIT) {

				SDL_GL_SetAttribute (SDL_GL_RED_SIZE, 8);
				SDL_GL_SetAttribute (SDL_GL_GREEN_SIZE, 8);
				SDL_GL_SetAttribute (SDL_GL_BLUE_SIZE, 8);
				SDL_GL_SetAttribute (SDL_GL_ALPHA_SIZE, 8);

			} else {

				SDL_GL_SetAttribute (SDL_GL_RED_SIZE, 5);
				SDL_GL_SetAttribute (SDL_GL_GREEN_SIZE, 6);
				SDL_GL_SetAttribute (SDL_GL_BLUE_SIZE, 5);

			}

		}

		sdlWindow = SDL_CreateWindow (title, width, height, sdlWindowFlags);

		#if defined (IPHONE) || defined (APPLETV)
		if (sdlWindow && !SDL_GL_CreateContext (sdlWindow)) {

			SDL_DestroyWindow (sdlWindow);
			SDL_GL_SetAttribute (SDL_GL_CONTEXT_MAJOR_VERSION, 2);

			sdlWindow = SDL_CreateWindow (title, width, height, sdlWindowFlags);

		}
		#endif

		if (!sdlWindow) {

			printf ("Could not create SDL window: %s.\n", SDL_GetError ());
			return;

		}

		#if defined (HX_WINDOWS) && !defined (HX_WINRT)

		HINSTANCE handle = ::GetModuleHandle (nullptr);
		HICON icon = ::LoadIcon (handle, MAKEINTRESOURCE (1));

		if (icon != nullptr) {

			HWND hwnd = GetWin32Window (sdlWindow);

			if (hwnd) {

				#ifdef _WIN64
				::SetClassLongPtr (hwnd, GCLP_HICON, reinterpret_cast<LONG_PTR>(icon));
				#else
				::SetClassLong (hwnd, GCL_HICON, reinterpret_cast<LONG>(icon));
				#endif

			}

		}

		#endif

		if (flags & WINDOW_FLAG_HARDWARE) {

			SDL_GL_LoadLibrary (NULL);

			context = SDL_GL_CreateContext (sdlWindow);

			if (context && SDL_GL_MakeCurrent (sdlWindow, context)) {

				// SeiunEngine: SeiunForceVSync() (SeiunLevers.h) is the opt-in
				// diagnostic for findings E1. Default path unchanged.
				if (SeiunForceVSync () || (flags & WINDOW_FLAG_VSYNC)) {

					SDL_GL_SetSwapInterval (1);

				} else {

					SDL_GL_SetSwapInterval (0);

				}

				OpenGLBindings::Init ();

				#ifndef LIME_GLES

				int version = 0;
				glGetIntegerv (GL_MAJOR_VERSION, &version);

				if (version == 0) {

					float versionScan = 0;
					sscanf ((const char*)glGetString (GL_VERSION), "%f", &versionScan);
					version = versionScan;

				}

				if (version < 2 && !strstr ((const char*)glGetString (GL_VERSION), "OpenGL ES")) {

					SDL_GL_DestroyContext (context);
					context = 0;

				}

				#elif defined(IPHONE) || defined(APPLETV)

				glGetIntegerv (GL_FRAMEBUFFER_BINDING, &OpenGLBindings::defaultFramebuffer);
				glGetIntegerv (GL_RENDERBUFFER_BINDING, &OpenGLBindings::defaultRenderbuffer);

				#endif

			} else {

				SDL_GL_DestroyContext (context);
				context = NULL;

			}

		}

		if (!context) {

			sdlRenderer = SDL_CreateRenderer (sdlWindow, "software");

			if (!sdlRenderer) {

				sdlRenderer = SDL_CreateRenderer (sdlWindow, NULL);

			}

		}

		if (context || sdlRenderer) {

			((SDLApplication*)currentApplication)->RegisterWindow (this);

		} else {

			printf ("Could not create SDL renderer: %s.\n", SDL_GetError ());

		}

	}


	SDLWindow::~SDLWindow () {

		if (sdlWindow) {

			SDL_DestroyWindow (sdlWindow);
			sdlWindow = 0;

		}

		if (sdlRenderer) {

			SDL_DestroyRenderer (sdlRenderer);

		} else if (context) {

			SDL_GL_DestroyContext (context);

		}

	}


	void SDLWindow::Alert (const char* message, const char* title) {

		#if defined (HX_WINDOWS) && !defined (HX_WINRT)

		int count = 0;
		int speed = 0;
		bool stopOnForeground = true;

		FLASHWINFO fi;
		fi.cbSize = sizeof (FLASHWINFO);
		fi.hwnd = GetWin32Window (sdlWindow);
		fi.dwFlags = stopOnForeground ? FLASHW_ALL | FLASHW_TIMERNOFG : FLASHW_ALL | FLASHW_TIMER;
		fi.uCount = count;
		fi.dwTimeout = speed;
		FlashWindowEx (&fi);

		#endif

		if (message) {

			SDL_ShowSimpleMessageBox (SDL_MESSAGEBOX_INFORMATION, title, message, sdlWindow);

		}

	}


	void SDLWindow::Close () {

		if (sdlWindow) {

			SDL_DestroyWindow (sdlWindow);
			sdlWindow = 0;

		}

	}


	void SDLWindow::ContextFlip () {

		if (context && !sdlRenderer) {

			SDL_GL_SwapWindow (sdlWindow);

		} else if (sdlRenderer) {

			SDL_RenderPresent (sdlRenderer);

		}

	}


	void* SDLWindow::ContextLock (bool useCFFIValue) {

		if (sdlRenderer) {

			int width;
			int height;

			SDL_GetCurrentRenderOutputSize (sdlRenderer, &width, &height);

			if (width != contextWidth || height != contextHeight) {

				if (sdlTexture) {

					SDL_DestroyTexture (sdlTexture);

				}

				sdlTexture = SDL_CreateTexture (sdlRenderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width, height);

				contextWidth = width;
				contextHeight = height;

			}

			void *pixels;
			int pitch;

			if (useCFFIValue) {

				if (SDL_LockTexture (sdlTexture, NULL, &pixels, &pitch)) {

					value result = alloc_empty_object ();
					alloc_field (result, val_id ("width"), alloc_int (contextWidth));
					alloc_field (result, val_id ("height"), alloc_int (contextHeight));
					alloc_field (result, val_id ("pixels"), alloc_float ((uintptr_t)pixels));
					alloc_field (result, val_id ("pitch"), alloc_int (pitch));
					return result;

				} else {

					return alloc_null ();

				}

			} else {

				const int id_width = hl_hash_utf8 ("width");
				const int id_height = hl_hash_utf8 ("height");
				const int id_pixels = hl_hash_utf8 ("pixels");
				const int id_pitch = hl_hash_utf8 ("pitch");

				if (SDL_LockTexture (sdlTexture, NULL, &pixels, &pitch)) {

					vdynamic* result = (vdynamic*)hl_alloc_dynobj();
					hl_dyn_seti (result, id_width, &hlt_i32, contextWidth);
					hl_dyn_seti (result, id_height, &hlt_i32, contextHeight);
					hl_dyn_setd (result, id_pixels, (uintptr_t)pixels);
					hl_dyn_seti (result, id_pitch, &hlt_i32, pitch);
					return result;

				} else {

					return 0;

				}

			}

		} else {

			if (useCFFIValue) {

				return alloc_null ();

			} else {

				return 0;

			}

		}

	}


	void SDLWindow::ContextMakeCurrent () {

		if (sdlWindow && context) {

			SDL_GL_MakeCurrent (sdlWindow, context);

		}

	}


	void SDLWindow::ContextUnlock () {

		if (sdlTexture) {

			SDL_UnlockTexture (sdlTexture);
			SDL_RenderClear (sdlRenderer);
			SDL_RenderTexture (sdlRenderer, sdlTexture, NULL, NULL);

		}

	}


	void SDLWindow::Focus () {

		SDL_RaiseWindow (sdlWindow);

	}


	void* SDLWindow::GetContext () {

		return context;

	}


	const char* SDLWindow::GetContextType () {

		if (context) {

			return "opengl";

		} else if (sdlRenderer) {

			return "software";

		}

		return "none";

	}


	int SDLWindow::GetDisplay () {

		SDL_DisplayID* displays = NULL;
		int displayCount = 0;
		int displayIndex = 0;
		SDL_DisplayID displayID = SDL_GetDisplayForWindow (sdlWindow);

		displays = SDL_GetDisplays (&displayCount);

		if (displays) {

			for (int i = 0; i < displayCount; i++) {

				if (displays[i] == displayID) {

					displayIndex = i;
					break;

				}

			}

			SDL_free (displays);

		}

		return displayIndex;

	}


	void SDLWindow::GetDisplayMode (DisplayMode* displayMode) {

		const SDL_DisplayMode* fullscreenMode = SDL_GetWindowFullscreenMode (sdlWindow);
		const SDL_DisplayMode* currentMode = SDL_GetCurrentDisplayMode (SDL_GetDisplayForWindow (sdlWindow));
		const SDL_DisplayMode* mode = fullscreenMode ? fullscreenMode : currentMode;

		if (!mode) {

			displayMode->width = GetWidth ();
			displayMode->height = GetHeight ();
			displayMode->pixelFormat = RGBA32;
			displayMode->refreshRate = 60;
			return;

		}

		displayMode->width = mode->w;
		displayMode->height = mode->h;

		switch (mode->format) {

			case SDL_PIXELFORMAT_ARGB8888:

				displayMode->pixelFormat = ARGB32;
				break;

			case SDL_PIXELFORMAT_BGRA8888:
			case SDL_PIXELFORMAT_BGRX8888:

				displayMode->pixelFormat = BGRA32;
				break;

			default:

				displayMode->pixelFormat = RGBA32;

		}

		displayMode->refreshRate = (int)mode->refresh_rate;

	}


	int SDLWindow::GetHeight () {

		// SeiunEngine: initialise, because SDL_GetWindowSize() can fail (for
		// example while Android is between surfaces after an IME show/hide or a
		// rotation) and would otherwise return stack garbage here.
		int width = 0;
		int height = 0;

		SDL_GetWindowSize (sdlWindow, &width, &height);

		return height;

	}


	uint32_t SDLWindow::GetID () {

		return SDL_GetWindowID (sdlWindow);

	}


	bool SDLWindow::GetMouseLock () {

		return SDL_GetWindowRelativeMouseMode (sdlWindow);

	}


	double SDLWindow::GetScale () {

		// SeiunEngine: guard against a zero window size. On Android the window
		// can briefly report width == 0 (or fail) while the surface is being
		// recreated - IME show/hide, rotation, a popup dialog - and
		// outputWidth / 0 is +Inf, which then propagates into
		// lime.ui.Window.scale -> the OpenFL stage scale and produces a broken
		// render target for that frame. Upstream lime added the same guard.
		if (sdlRenderer) {

			int outputWidth = 0;
			int outputHeight = 0;

			SDL_GetCurrentRenderOutputSize (sdlRenderer, &outputWidth, &outputHeight);

			int width = 0;
			int height = 0;

			SDL_GetWindowSize (sdlWindow, &width, &height);

			if (width <= 0) return 1;

			double scale = double (outputWidth) / width;
			return scale;

		} else if (context) {

			int outputWidth = 0;
			int outputHeight = 0;

			SDL_GetWindowSizeInPixels (sdlWindow, &outputWidth, &outputHeight);

			int width = 0;
			int height = 0;

			SDL_GetWindowSize (sdlWindow, &width, &height);

			if (width <= 0) return 1;

			double scale = double (outputWidth) / width;
			return scale;

		}

		return 1;

	}


	bool SDLWindow::GetTextInputEnabled () {

		return SDL_TextInputActive (sdlWindow);

	}


	int SDLWindow::GetWidth () {

		int width = 0;
		int height = 0;

		SDL_GetWindowSize (sdlWindow, &width, &height);

		return width;

	}


	int SDLWindow::GetX () {

		int x;
		int y;

		SDL_GetWindowPosition (sdlWindow, &x, &y);

		return x;

	}


	int SDLWindow::GetY () {

		int x;
		int y;

		SDL_GetWindowPosition (sdlWindow, &x, &y);

		return y;

	}


	void SDLWindow::Move (int x, int y) {

		SDL_SetWindowPosition (sdlWindow, x, y);

	}


	void SDLWindow::ReadPixels (ImageBuffer *buffer, Rectangle *rect) {

		if (sdlRenderer) {

			SDL_Rect bounds = { 0, 0, 0, 0 };

			if (rect) {

				bounds.x = rect->x;
				bounds.y = rect->y;
				bounds.w = rect->width;
				bounds.h = rect->height;

			} else {

				SDL_GetCurrentRenderOutputSize (sdlRenderer, &bounds.w, &bounds.h);

			}

			buffer->Resize (bounds.w, bounds.h, 32);

			SDL_Surface* surface = SDL_RenderReadPixels (sdlRenderer, &bounds);

			if (surface) {

				SDL_Surface* converted = SDL_ConvertSurface (surface, SDL_PIXELFORMAT_ABGR8888);
				SDL_Surface* source = converted ? converted : surface;
				Uint8* input = (Uint8*)source->pixels;
				Uint8* output = (Uint8*)buffer->data->buffer->b;
				int copyPitch = source->pitch < buffer->Stride () ? source->pitch : buffer->Stride ();

				for (int y = 0; y < bounds.h; y++) {

					memcpy (output + y * buffer->Stride (), input + y * source->pitch, copyPitch);

				}

				if (converted) {

					SDL_DestroySurface (converted);

				}

				SDL_DestroySurface (surface);

			}

		} else if (context) {

			// TODO

		}

	}


	void SDLWindow::Resize (int width, int height) {

		SDL_SetWindowSize (sdlWindow, width, height);

	}


	bool SDLWindow::SetBorderless (bool borderless) {

		if (borderless) {

			SDL_SetWindowBordered (sdlWindow, false);

		} else {

			SDL_SetWindowBordered (sdlWindow, true);

		}

		return borderless;

	}


	void SDLWindow::SetCursor (Cursor cursor) {

		if (cursor != currentCursor) {

			if (currentCursor == HIDDEN) {

				SDL_ShowCursor ();

			}

			switch (cursor) {

				case HIDDEN:

					SDL_HideCursor ();

				case CROSSHAIR:

					if (!SDLCursor::crosshairCursor) {

						SDLCursor::crosshairCursor = SDL_CreateSystemCursor (SDL_SYSTEM_CURSOR_CROSSHAIR);

					}

					SDL_SetCursor (SDLCursor::crosshairCursor);
					break;

				case MOVE:

					if (!SDLCursor::moveCursor) {

						SDLCursor::moveCursor = SDL_CreateSystemCursor (SDL_SYSTEM_CURSOR_MOVE);

					}

					SDL_SetCursor (SDLCursor::moveCursor);
					break;

				case POINTER:

					if (!SDLCursor::pointerCursor) {

						SDLCursor::pointerCursor = SDL_CreateSystemCursor (SDL_SYSTEM_CURSOR_POINTER);

					}

					SDL_SetCursor (SDLCursor::pointerCursor);
					break;

				case RESIZE_NESW:

					if (!SDLCursor::resizeNESWCursor) {

						SDLCursor::resizeNESWCursor = SDL_CreateSystemCursor (SDL_SYSTEM_CURSOR_NESW_RESIZE);

					}

					SDL_SetCursor (SDLCursor::resizeNESWCursor);
					break;

				case RESIZE_NS:

					if (!SDLCursor::resizeNSCursor) {

						SDLCursor::resizeNSCursor = SDL_CreateSystemCursor (SDL_SYSTEM_CURSOR_NS_RESIZE);

					}

					SDL_SetCursor (SDLCursor::resizeNSCursor);
					break;

				case RESIZE_NWSE:

					if (!SDLCursor::resizeNWSECursor) {

						SDLCursor::resizeNWSECursor = SDL_CreateSystemCursor (SDL_SYSTEM_CURSOR_NWSE_RESIZE);

					}

					SDL_SetCursor (SDLCursor::resizeNWSECursor);
					break;

				case RESIZE_WE:

					if (!SDLCursor::resizeWECursor) {

						SDLCursor::resizeWECursor = SDL_CreateSystemCursor (SDL_SYSTEM_CURSOR_EW_RESIZE);

					}

					SDL_SetCursor (SDLCursor::resizeWECursor);
					break;

				case TEXT:

					if (!SDLCursor::textCursor) {

						SDLCursor::textCursor = SDL_CreateSystemCursor (SDL_SYSTEM_CURSOR_TEXT);

					}

					SDL_SetCursor (SDLCursor::textCursor);
					break;

				case WAIT:

					if (!SDLCursor::waitCursor) {

						SDLCursor::waitCursor = SDL_CreateSystemCursor (SDL_SYSTEM_CURSOR_WAIT);

					}

					SDL_SetCursor (SDLCursor::waitCursor);
					break;

				case WAIT_ARROW:

					if (!SDLCursor::waitArrowCursor) {

						SDLCursor::waitArrowCursor = SDL_CreateSystemCursor (SDL_SYSTEM_CURSOR_PROGRESS);

					}

					SDL_SetCursor (SDLCursor::waitArrowCursor);
					break;

				default:

					if (!SDLCursor::arrowCursor) {

						SDLCursor::arrowCursor = SDL_CreateSystemCursor (SDL_SYSTEM_CURSOR_DEFAULT);

					}

					SDL_SetCursor (SDLCursor::arrowCursor);
					break;

			}

			currentCursor = cursor;

		}

	}


	void SDLWindow::SetDisplayMode (DisplayMode* displayMode) {

		Uint32 pixelFormat = 0;

		switch (displayMode->pixelFormat) {

			case ARGB32:

				pixelFormat = SDL_PIXELFORMAT_ARGB8888;
				break;

			case BGRA32:

				pixelFormat = SDL_PIXELFORMAT_BGRA8888;
				break;

			default:

				pixelFormat = SDL_PIXELFORMAT_RGBA8888;

		}

		SDL_DisplayMode mode = {
			SDL_GetDisplayForWindow (sdlWindow),
			(SDL_PixelFormat)pixelFormat,
			displayMode->width,
			displayMode->height,
			SDL_GetWindowPixelDensity (sdlWindow),
			(float)displayMode->refreshRate,
			0,
			0,
			NULL
		};

		if (SDL_SetWindowFullscreenMode (sdlWindow, &mode)) {

			displayModeSet = true;

			if (SDL_GetWindowFlags (sdlWindow) & SDL_WINDOW_FULLSCREEN) {

				SDL_SetWindowFullscreen (sdlWindow, true);

			}

		}

	}


	bool SDLWindow::SetFullscreen (bool fullscreen) {

		bool currentlyFullscreen = (SDL_GetWindowFlags (sdlWindow) & SDL_WINDOW_FULLSCREEN) != 0;

		if (fullscreen && currentlyFullscreen) {

			return fullscreen;

		}

		if (fullscreen) {

			bool borderless = (SDL_GetWindowFlags (sdlWindow) & SDL_WINDOW_BORDERLESS) != 0;
			if (borderless) {
				SDL_SetWindowFullscreenMode (sdlWindow, NULL);
			} else if (displayModeSet) {
			}

			SDL_SetWindowFullscreen (sdlWindow, true);

		} else {

			SDL_SetWindowFullscreen (sdlWindow, false);

		}

		return fullscreen;

	}


	void SDLWindow::SetIcon (ImageBuffer *imageBuffer) {

		SDL_Surface *surface = SDL_CreateSurfaceFrom (imageBuffer->width, imageBuffer->height, SDL_GetPixelFormatForMasks (imageBuffer->bitsPerPixel, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000), imageBuffer->data->buffer->b, imageBuffer->Stride ());

		if (surface) {

			SDL_SetWindowIcon (sdlWindow, surface);
			SDL_DestroySurface (surface);

		}

	}


	bool SDLWindow::SetMaximized (bool maximized) {

		if (maximized) {

			SDL_MaximizeWindow (sdlWindow);

		} else {

			SDL_RestoreWindow (sdlWindow);

		}

		return maximized;

	}


	bool SDLWindow::SetMinimized (bool minimized) {

		if (minimized) {

			SDL_MinimizeWindow (sdlWindow);

		} else {

			SDL_RestoreWindow (sdlWindow);

		}

		return minimized;

	}


	void SDLWindow::SetMouseLock (bool mouseLock) {

		SDL_SetWindowRelativeMouseMode (sdlWindow, mouseLock);

	}


	bool SDLWindow::SetResizable (bool resizable) {

		#ifndef EMSCRIPTEN

		if (resizable) {

			SDL_SetWindowResizable (sdlWindow, true);

		} else {

			SDL_SetWindowResizable (sdlWindow, false);

		}

		return (SDL_GetWindowFlags (sdlWindow) & SDL_WINDOW_RESIZABLE);

		#else

		return resizable;

		#endif

	}


	void SDLWindow::SetTextInputEnabled (bool enabled) {

		if (enabled) {

			SDL_StartTextInput (sdlWindow);

		} else {

			SDL_StopTextInput (sdlWindow);

		}

	}


	void SDLWindow::SetTextInputRect (Rectangle * rect) {

		SDL_Rect bounds = { 0, 0, 0, 0 };

		if (rect) {

			bounds.x = rect->x;
			bounds.y = rect->y;
			bounds.w = rect->width;
			bounds.h = rect->height;

		}

		SDL_SetTextInputArea (sdlWindow, &bounds, 0);
	}


	const char* SDLWindow::SetTitle (const char* title) {

		SDL_SetWindowTitle (sdlWindow, title);

		return title;

	}


	void SDLWindow::WarpMouse (int x, int y){

		SDL_WarpMouseInWindow (sdlWindow, x, y);

	}


	Window* CreateWindow (Application* application, int width, int height, int flags, const char* title) {

		return new SDLWindow (application, width, height, flags, title);

	}


}
