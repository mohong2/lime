// Process lifecycle: probe, attach, per-frame submit, fallback.

package seiun.vulkan;

import sys.FileSystem;
import sys.io.File;

class VkRuntime
{
	static inline var TAG = "[vulkan]";
	static inline var LIBRARY_NAME = "Lime.ndll";

	public static inline var compiled:Bool = #if seiun_vulkan true #else false #end;

	public static var enabled(default, null):Bool = false;

	public static var lastFailure(default, null):String = null;

	public static var windowLost(default, null):Bool = false;

	/** True once the OpenGL context that booted the window has been released. */
	public static var glReleased(default, null):Bool = false;

	public static var onFallback:String->Void = null;

	/** Counts opened frames; the overlay uses it to draw once per frame. */
	public static var frameId(default, null):Int = 0;

	/** Milliseconds spent in the end of frame call: submit, fence wait and present. */
	public static var endMs(default, null):Float = 0;

	static var frameOpen:Bool = false;
	static var started:Bool = false;
	static var glReleaseTried:Bool = false;
	static var loggedSkip:Bool = false;

	public static function requested():Bool
	{
		if (flag ("--no-vulkan")) return false;
		if (flag ("--vulkan")) return true;

		return envTrue ("SEIUN_VULKAN");
	}

	public static function start(verbose:Bool = false):Bool
	{
		if (started) return enabled;
		started = true;

		if (!requested ())
		{
			log ("not requested (pass --vulkan to try it)");
			return false;
		}

		if (!VkNative.ready ())
		{
			abort (LIBRARY_NAME + " does not contain the renderer (build it without -Dlime-vulkan-off)", true);
			return false;
		}

		var verboseMode = verbose || flag ("--vulkan-verbose") || envTrue ("SEIUN_VULKAN_VERBOSE");
		var validation = flag ("--vulkan-validation") || envTrue ("SEIUN_VULKAN_VALIDATION");

		log ("native renderer " + VkNative.version ());

		if (!VkNative.available ())
		{
			abort ("this machine has no usable Vulkan runtime (lime_vulkan_available = 0)", true);
			return false;
		}

		var window = VkNative.selfWindow ();

		if (window == null)
		{
			abort ("could not find the game window (lime_vulkan_self_window returned NULL)", true);
			return false;
		}

		if (!VkNative.probeWindow (window, verboseMode, validation))
		{
			abort ("window probe failed: " + errorLine (), true);
			return false;
		}

		if (!VkNative.contextCreate (window, verboseMode, validation))
		{
			abort ("could not create the Vulkan context: " + errorLine (), true);
			return false;
		}

		windowLost = true;
		enabled = true;
		lastFailure = null;

		log ("attached: " + field ("device") + ", " + windowSize ());
		log ("note: this window is now DXGI-composited; OpenGL output will not show");

		writeStatusRecord ("vulkan", "device: " + field ("device") + ", " + windowSize () + ", library: " + LIBRARY_NAME);

		return true;
	}

	/**
	 * Drops the OpenGL context lime created while booting the window. Vulkan owns
	 * the window by now, so nothing can be drawn through OpenGL any more and the
	 * context only holds driver memory.
	 */
	static function releaseOpenGL():Void
	{
		if (glReleased || glReleaseTried) return;

		glReleaseTried = true;

		if (flag ("--vulkan-keep-context"))
		{
			log ("the OpenGL context is kept (--vulkan-keep-context)");
			return;
		}

		var result = VkNative.releaseGL ();

		glReleased = result == 0;

		switch (result)
		{
			case 0:
				log ("released the OpenGL context");
			case 1:
				log ("no OpenGL context was current, nothing to release");
			case 2:
				log ("could not reach the OpenGL context: " + errorLine ());
			default:
				log ("the driver refused to delete the OpenGL context");
		}
	}

	public static function prepareFrame(clearArgb:Int):Bool
	{
		if (!enabled) return false;
		if (frameOpen) return true;

		var result = VkNative.batchBegin (clearArgb);

		if (result == 1)
		{
			frameOpen = true;
			frameId++;
			loggedSkip = false;

			VkQuads.resetCameraFx ();
			VkCameraFilters.reset ();

			return true;
		}

		if (result == 2)
		{
			if (!loggedSkip)
			{
				loggedSkip = true;
				log ("frame skipped: " + errorLine ());
			}

			return false;
		}

		abort ("lime_vulkan_batch_begin failed: " + errorLine (), true);
		return false;
	}

	public static function isFrameOpen():Bool
	{
		return frameOpen;
	}

	public static function endFrame():Void
	{
		if (!enabled || !frameOpen) return;

		// OpenFL overlay goes last so it sits on top of every camera. A frame the
		// game did not draw keeps the previous contents on screen: opening an empty
		// frame here would clear the window and leave only the overlay.
		try
		{
			// Cameras the frame never moved past still owe their fade or flash.
			VkQuads.flushCameraFx (0x7FFFFFFF);

			// One line per camera in logs/vulkan-filters.txt: a run can be
			// diagnosed from the log folder without a console window.
			VkCameraFilters.writeReport (frameId);

			VkOverlay.install ();

			// The stage renderer draws the overlay from inside OpenFL's render pass.
			// A frame whose stage render was skipped draws it here instead.
			if (VkOverlay.drawnFrame != frameId) VkOverlay.render ();
		}
		catch (error:Dynamic)
		{
			VkOverlay.enabled = false;
			log ("overlay disabled after an error: " + Std.string (error));
		}

		frameOpen = false;

		var started = haxe.Timer.stamp ();

		var ended = VkNative.batchEnd ();

		endMs = (haxe.Timer.stamp () - started) * 1000.0;

		if (!ended)
		{
			abort ("lime_vulkan_batch_end failed: " + errorLine (), true);
			return;
		}

		// The stage renderer is installed from inside endFrame, so the first frame
		// that reaches this point is the first one OpenFL rendered without OpenGL.
		if (VkOverlay.ownsStage ()) releaseOpenGL ();
	}

	public static function resize(width:Int, height:Int):Void
	{
		if (!enabled || width <= 0 || height <= 0) return;

		// The swapchain is rebuilt with a new format, so targets created for the
		// old one cannot be drawn into any more.
		VkRenderTargets.dispose ();

		VkNative.resize (width, height);
	}

	static function flag(name:String):Bool
	{
		for (arg in Sys.args ())
		{
			if (arg == name) return true;
		}

		return false;
	}

	static function envTrue(name:String):Bool
	{
		var value = Sys.getEnv (name);

		if (value == null) return false;

		var lower = value.toLowerCase ();

		return value == "1" || lower == "true" || lower == "on" || lower == "yes";
	}

	public static function fail(message:String):Void
	{
		abort (message, true);
	}

	public static function abort(reason:String, showDialog:Bool = true):Void
	{
		lastFailure = reason;
		enabled = false;
		frameOpen = false;

		log ("FAILED: " + reason);

		writeStatusRecord (windowLost ? "opengl (vulkan stopped; restart to be safe)" : "opengl", reason);

		if (windowLost)
		{
			log ("a swapchain already existed, so OpenGL cannot take this window back;"
				+ " the next launch will use OpenGL");
			writeFailureRecord (reason);
		}

		if (VkNative.ready ())
		{
			try VkNative.contextDestroy () catch (error:Dynamic) {}
		}

		if (showDialog && onFallback != null)
		{
			try
			{
				onFallback (dialogText (reason));
			}
			catch (error:Dynamic)
			{
				log ("the fallback dialog handler threw: " + Std.string (error));
			}
		}
	}

	static function dialogText(reason:String):String
	{
		var head = windowLost
			? "Vulkan rendering stopped. Restart the game to fall back to OpenGL."
			: "Vulkan is not available here; this session stays on OpenGL.";

		return head + "\n\n" + reason;
	}

	public static function reportUnsupportedBuild(show:String->Void):Bool
	{
		if (compiled) return false;
		if (!requested ()) return false;

		var reason = "This build was compiled without the Vulkan backend (missing -D seiun_vulkan);"
			+ " this session stays on OpenGL.";

		log ("requested, but this build has no Vulkan backend compiled in");

		writeStatusRecord ("opengl (not compiled into this build)", reason);

		if (show != null)
		{
			try show (reason) catch (error:Dynamic) {}
		}

		return true;
	}

	static function writeStatusRecord(mode:String, detail:String):Void
	{
		try
		{
			if (!FileSystem.exists ("logs")) FileSystem.createDirectory ("logs");

			var text = Date.now ().toString () + "\nrenderer: " + mode + "\n" + detail + "\n";

			if (VkNative.ready ())
			{
				try text += "\n" + status () catch (error:Dynamic) {}
			}

			File.saveContent ("logs/vulkan-status.txt", text);
		}
		catch (error:Dynamic) {}
	}

	static function writeFailureRecord(reason:String):Void
	{
		try
		{
			if (!FileSystem.exists ("logs")) FileSystem.createDirectory ("logs");

			File.saveContent ("logs/vulkan-last-failure.txt",
				Date.now ().toString () + "\n" + reason + "\n\n" + status ());
		}
		catch (error:Dynamic) {}
	}

	public static function status():String
	{
		return VkNative.statusText () + VkNative.batchStatsText ();
	}

	static function field(name:String):String
	{
		var prefix = name + "=";

		for (line in VkNative.statusText ().split ("\n"))
		{
			if (StringTools.startsWith (line, prefix)) return line.substr (prefix.length);
		}

		return "?";
	}

	static function errorLine():String
	{
		var value = field ("error");

		return value == "?" || value.length == 0 ? "(no error text from the native side)" : value;
	}

	static function windowSize():String
	{
		return field ("width") + "x" + field ("height");
	}

	static function log(message:String):Void
	{
		Sys.println (TAG + " " + message);
	}
}
