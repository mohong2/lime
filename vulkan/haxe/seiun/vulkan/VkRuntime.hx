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

	#if seiun_vulkan_perf
	// Temporary probe (-D seiun_vulkan_perf): writes the frame cost counters so a
	// slow frame can be measured from the log folder instead of guessed at.
	static var perfBudget:Int = 0;
	static var overlayMs:Float = 0;
	static var frameMs:Float = 0;
	static var lastTick:Float = 0;
	#end
	static var glReleaseTried:Bool = false;
	static var loggedSkip:Bool = false;

	/**
	 * Extra answer for "did the application ask for Vulkan?".
	 *
	 * Flags and SEIUN_VULKAN still come first - they are the escape hatch for a machine
	 * whose saved setting is the thing that will not start. This hook is what a game uses
	 * for its own preference, so picking a renderer in a menu can start the backend without
	 * faking a command line. Returns false when nothing is set.
	 */
	public static var preferred:Void->Bool = null;

	public static function requested():Bool
	{
		if (flag ("--no-vulkan")) return false;
		if (flag ("--vulkan")) return true;

		if (envTrue ("SEIUN_VULKAN")) return true;

		// Any other value for the variable is an explicit switch-off: do not fall through to
		// the saved preference, that would make the override impossible to trust.
		var env = Sys.getEnv ("SEIUN_VULKAN");

		if (env != null && env != "") return false;

		if (preferred != null)
		{
			try return preferred () catch (error:Dynamic) return false;
		}

		return false;
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

		// A driver whose timestamps are slow or wrong can turn them off; the native
		// side then reports gpuSupported=0 instead of a fake GPU time.
		if (flag ("--vulkan-no-gputime")) Sys.putEnv ("SEIUN_VULKAN_NO_GPUTIME", "1");

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
	 * Drops the OpenGL context lime created while booting the window.
	 *
	 * Windows calls this once Vulkan owns the window: nothing can be drawn through OpenGL any
	 * more and the context only holds driver memory. Android calls it *before* the backend
	 * starts, because there the EGL surface is what keeps the ANativeWindow away from Vulkan
	 * (the driver answers VK_ERROR_NATIVE_WINDOW_IN_USE_KHR otherwise).
	 *
	 * Public so an application can hand the window over itself; glReleased() tells it whether
	 * OpenGL can still draw afterwards.
	 */
	public static function releaseOpenGL():Void
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

			#if seiun_vulkan_perf
			VkQuads.resetProfile ();
			#end

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

		#if seiun_vulkan_perf
		var overlayStart = haxe.Timer.stamp ();
		#end

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

		#if seiun_vulkan_perf
		var now = haxe.Timer.stamp ();

		overlayMs = (now - overlayStart) * 1000.0;
		frameMs = lastTick > 0 ? (now - lastTick) * 1000.0 : 0.0;
		lastTick = now;
		#end

		frameOpen = false;

		var started = haxe.Timer.stamp ();

		var ended = VkNative.batchEnd ();

		endMs = (haxe.Timer.stamp () - started) * 1000.0;

		if (!ended)
		{
			abort ("lime_vulkan_batch_end failed: " + errorLine (), true);
			return;
		}

		#if seiun_vulkan_perf
		perfDump ();
		#end

		// The stage renderer is installed from inside endFrame, so the first frame
		// that reaches this point is the first one OpenFL rendered without OpenGL.
		if (VkOverlay.ownsStage ()) releaseOpenGL ();
	}

	#if seiun_vulkan_perf
	/** One line per 300 frames: where the frame time goes, per counter group. */
	static function perfDump():Void
	{
		if (perfBudget > 0)
		{
			perfBudget--;
			return;
		}

		perfBudget = 300;

		var lines:Array<String> = [];

		lines.push ("frame=" + frameId + " frameMs=" + round2 (frameMs) + " overlayMs=" + round2 (overlayMs)
			+ " endMs=" + round2 (endMs) + " quads=" + VkQuads.submitted + " overlayQuads=" + VkOverlay.drawn);

		lines.push ("profile submitCalls=" + VkQuads.submitCalls + " submitQuads=" + VkQuads.submitQuads
			+ " submitMs=" + round2 (VkQuads.submitMs)
			+ " overlayWalkMs=" + round2 (VkOverlay.renderMs) + " filterPasses=" + VkCameraFilters.passes);

		lines.push ("updates=" + VkTextures.takeUpdates ().join (", "));
		lines.push (VkTextures.stats ());

		for (line in status ().split ("\n"))
		{
			if (StringTools.startsWith (line, "draw") || StringTools.startsWith (line, "q")) continue;

			lines.push (line);
		}

		var text = Date.now ().toString () + "\n" + lines.join ("\n") + "\n";

		try
		{
			if (!FileSystem.exists ("logs")) FileSystem.createDirectory ("logs");

			File.saveContent ("logs/vulkan-perf.txt", text);
		}
		catch (error:Dynamic) {}

		Sys.println ("[vulkan-perf] " + lines.join (" "));
	}

	static function round2(value:Float):Float
	{
		return Std.int (value * 100.0) / 100.0;
	}
	#end

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
