// Camera filters: the whole camera layer goes through its OpenFL ShaderFilters.
//
// flixel hangs the filters on "camera.flashSprite", which is the container of the
// entire camera output, so the OpenGL renderer draws the camera into an offscreen
// texture and runs one fullscreen pass per filter. The Vulkan renderer bypasses
// that path completely, so this class does the same thing on top of the batch:
//
//   camera content -> target A
//   filter 0: A -> B      filter 1: B -> A      filter 2: A -> B   ...
//   the last target is composited over the window where the camera sits
//
// Only ShaderFilter runs here. The stock BitmapFilter effects (blur, glow,
// drop shadow) need multi pass setups with an extra "preserve the original"
// input, so they are skipped and the camera is drawn unfiltered instead.

package seiun.vulkan;

#if seiun_vulkan

import cpp.UInt64;
import flixel.FlxCamera;
import flixel.FlxG;
import sys.FileSystem;
import sys.io.File;
import openfl.display.Shader;
import openfl.filters.BitmapFilter;
import openfl.filters.BlurFilter;
import openfl.filters.ColorMatrixFilter;
import openfl.filters.ConvolutionFilter;
import openfl.filters.ShaderFilter;

@:access(openfl.filters.BitmapFilter)
class VkCameraFilters
{

	/** A sanity cap: a broken filter list must not stall the frame. */
	static inline var MAX_PASSES = 16;

	public static var enabled(default, null):Bool = true;

	/** Filter passes run this frame. */
	public static var passes(default, null):Int = 0;

	/** Filters skipped because this backend cannot run them. */
	public static var unsupported(default, null):Int = 0;

	/** The camera whose render target is open right now, or null. */
	public static var layer(default, null):FlxCamera = null;

	static var source:UInt64 = 0;
	static var scratch:UInt64 = 0;

	// Where the layer sits on screen, in device pixels.
	static var layerX:Float = 0;
	static var layerY:Float = 0;
	static var layerWidth:Float = 0;
	static var layerHeight:Float = 0;

	/** Layers that were opened and composited back, for the stats line. */
	public static var composites(default, null):Int = 0;

	/** Layer binding attempts that did not open a target, with the reason. */
	public static var shadow(default, null):String = "-";

	static var parsed:Bool = false;
	static var warned:Bool = false;

	// Report throttling: enough frames to see the menu and the first seconds of
	// a song without writing a file every frame for an hour.
	static var reports:Int = 0;
	static var reportTimer:Int = 0;

	/** The last few distinct filter setups, so one run shows the whole timeline. */
	static var history:Array<String> = [];
	static var historyKey:String = "";

	/** Called when a frame opens: no camera owns a target yet. */
	public static function reset():Void
	{
		parseFlags ();

		VkRenderTargets.trim ();

		layer = null;
		source = 0;
		scratch = 0;
		passes = 0;
		unsupported = 0;
		composites = 0;
	}

	static function parseFlags():Void
	{
		if (parsed) return;

		parsed = true;

		for (arg in Sys.args ())
		{
			if (arg == "--vulkan-nofilters") enabled = false;
		}
	}

	public static function isLayer(camera:FlxCamera):Bool
	{
		return layer != null && layer == camera;
	}

	/**
	 * Points the batch at this camera's own render target, finishing whatever
	 * layer was open before. Does nothing when the camera has no usable filter,
	 * in which case its content keeps going straight to the window.
	 */
	public static function beginLayer(camera:FlxCamera):Void
	{
		if (layer == camera) return;

		finishLayer ();

		if (!hasUsable (camera))
		{
			if (enabled && camera != null && camera.filtersEnabled)
			{
				var filters = filtersOf (camera);
				shadow = "no-usable:" + (filters == null ? "null" : Std.string (filters.length));
			}
			else
			{
				shadow = "disabled";
			}

			return;
		}

		// Computed here and not by the caller: this runs once per camera per frame,
		// while beginSubmit runs for every draw item.
		var rect = VkQuads.cameraRect (camera);

		if (rect[2] < 1 || rect[3] < 1) return;

		var width = Std.int (Math.round (rect[2]));
		var height = Std.int (Math.round (rect[3]));

		if (width < 1 || height < 1) return;

		source = VkRenderTargets.get (width, height, 0);
		scratch = VkRenderTargets.get (width, height, 1);

		if (!VkNative.valid (source) || !VkNative.valid (scratch))
		{
			shadow = "no-target:" + width + "x" + height + " a=" + source + " b=" + scratch;
			source = 0;
			scratch = 0;
			return;
		}

		layerX = rect[0];
		layerY = rect[1];
		layerWidth = width;
		layerHeight = height;

		if (!VkNative.targetBegin (source, layerBackground (camera)))
		{
			shadow = "begin-target-failed:" + nativeError ();
			source = 0;
			scratch = 0;
			report ("could not open a render target (" + width + "x" + height + "), drawing the camera unfiltered");
			return;
		}

		layer = camera;
		shadow = "bound:" + width + "x" + height;
	}

	/**
	 * Runs the filter chain of the open layer and composites the result over the
	 * window. Safe to call when no layer is open.
	 */
	public static function finishLayer():Void
	{
		if (layer == null) return;

		var filters = usable (layer);

		// Filters of a camera that did get composited but cannot run here. The
		// report shows the count next to the pass count, so an effect a mod asks
		// for and does not get has a number behind it.
		var declared = filtersOf (layer);

		if (declared != null)
		{
			var running = filters == null ? 0 : filters.length;

			if (declared.length > running) unsupported += declared.length - running;
		}

		var current = source;
		var other = scratch;
		var count = 0;

		if (filters != null)
		{
			var smooth = false;
			var stopped = false;

			for (filter in filters)
			{
				var total = passCount (filter);

				smooth = smoothOf (filter);

				for (pass in 0...total)
				{
					if (count >= MAX_PASSES) break;

					var shader = passShader (filter, pass);

					if (shader == null) break;

					// A shader that will not compile leaves the camera unfiltered from
					// this point on instead of dropping the frame.
					if (!runPass (shader, current, other, smooth))
					{
						stopped = true;
						break;
					}

					var swap = current;
					current = other;
					other = swap;
					count++;
					passes++;
				}

				if (stopped || count >= MAX_PASSES) break;
			}
		}

		VkNative.targetEnd ();

		if (current.toInt () != 0)
		{
			// Back on the window pass: premultiplied source over what is already
			// there, exactly like a filtered display object in OpenGL.
			VkNative.setView (null);
			VkNative.setFilter (true);
			VkNative.setBlend (0);
			VkNative.setScissor (0, 0, 0, 0);
			VkNative.setTexture (current);
			VkNative.quad (layerX, layerY, layerWidth, layerHeight, 0, 0, 1, 1, 0xFFFFFFFF);
		}

		if (current.toInt () != 0) composites++;

		layer = null;
		source = 0;
		scratch = 0;
	}

	/** One filter pass: sample "input", write "output", 1:1 in pixels. */
	static function runPass(shader:Shader, input:UInt64, output:UInt64, smooth:Bool):Bool
	{
		if (input.toInt () == 0 || output.toInt () == 0) return false;

		var program = VkProgram.get (shader);

		if (program == null || !program.isValid ()) return false;

		// Flushes the camera content first: the pass is a different target. No
		// background here: a filter pass replaces every pixel it covers.
		if (!VkNative.targetBegin (output, 0)) return false;

		VkNative.setProgram (program.handle);
		VkNative.setView (null);
		VkNative.setFilter (!smooth);
		VkNative.setBlend (0);
		VkNative.setScissor (0, 0, 0, 0);
		VkNative.setTexture (input);

		// The GL renderer fills this in for every bitmap it binds, filter passes
		// included. Mod shaders divide by it, so a pass without it turns the whole
		// camera into NaN.
		VkProgram.applyTextureSize (shader, input);

		// The uniforms live on the shader object, which the mod keeps updating
		// every frame, so they are read here and not cached.
		program.bind (shader);

		VkNative.quad (0, 0, layerWidth, layerHeight, 0, 0, 1, 1, 0xFFFFFFFF);

		// Draws the pass while the uniform block still holds those values.
		VkNative.setProgram (0);

		return true;
	}

	/**
	 * Moves camera space quads into the layer's own pixel space. Returns false
	 * when this camera is not the open layer, in which case the matrix is left
	 * alone.
	 */
	public static function toLayer(camera:FlxCamera, matrix:Array<Float>):Bool
	{
		if (layer == null || camera != layer) return false;

		matrix[4] -= layerX;
		matrix[5] -= layerY;

		return true;
	}

	/**
	 * Filters this backend can run.
	 *
	 * Everything except the multi pass effects that need the untouched image as a
	 * second input (glow, drop shadow, displacement map). Those would need a third
	 * render target per layer and a copy pass; skipping them leaves the camera
	 * readable instead of half filtered.
	 */
	static function supported(filter:BitmapFilter):Bool
	{
		if (filter == null) return false;

		return Std.isOfType (filter, ShaderFilter)
			|| Std.isOfType (filter, BlurFilter)
			|| Std.isOfType (filter, ColorMatrixFilter)
			|| Std.isOfType (filter, ConvolutionFilter);
	}

	/** How many fullscreen passes this filter needs. */
	@:access(openfl.filters.BitmapFilter)
	static function passCount(filter:BitmapFilter):Int
	{
		return filter.__numShaderPasses;
	}

	/** The shader of one pass; the filter fills its uniforms in on the way out. */
	@:access(openfl.filters.BitmapFilter)
	static function passShader(filter:BitmapFilter, pass:Int):Shader
	{
		return filter.__initShader (null, pass, null);
	}

	/** OpenFL samples the filter input smooth when the filter asks for it. */
	@:access(openfl.filters.BitmapFilter)
	static function smoothOf(filter:BitmapFilter):Bool
	{
		return filter.__smooth;
	}

	/** True when at least one of the camera's filters can run here. */
	static function hasUsable(camera:FlxCamera):Bool
	{
		if (!enabled || camera == null) return false;
		if (!camera.filtersEnabled) return false;

		var filters = filtersOf (camera);

		if (filters == null) return false;

		return usable (camera) != null;
	}

	static function usable(camera:FlxCamera):Array<BitmapFilter>
	{
		if (!enabled || camera == null || !camera.filtersEnabled) return null;

		var filters = filtersOf (camera);

		if (filters == null) return null;

		var list:Array<BitmapFilter> = [];

		for (filter in filters)
		{
			if (supported (filter) && passCount (filter) > 0) list.push (filter);
		}

		return list.length == 0 ? null : list;
	}

	/**
	 * The colour a camera paints under its sprites, premultiplied.
	 *
	 * flixel calls "camera.fill (bgColor)" before drawing the camera, and that fill
	 * is part of the layer the filters see, so the target is cleared with it
	 * instead of transparent black.
	 */
	static function layerBackground(camera:FlxCamera):Int
	{
		var color:Int = camera.bgColor;

		var alpha:Int = (color >> 24) & 0xFF;

		if (alpha <= 0) return 0;

		var red:Int = ((color >> 16) & 0xFF) * alpha;
		var green:Int = ((color >> 8) & 0xFF) * alpha;
		var blue:Int = (color & 0xFF) * alpha;

		return (alpha << 24) | (Std.int (red / 255) << 16) | (Std.int (green / 255) << 8) | Std.int (blue / 255);
	}

	/**
	 * flixel assigns "filtersEnabled ? filters : null" to flashSprite every
	 * update, so that array is the authoritative one; the camera's own field is
	 * the fallback for a camera that has not updated yet.
	 */
	static function filtersOf(camera:FlxCamera):Array<BitmapFilter>
	{
		var sprite = camera.flashSprite;

		if (sprite != null && sprite.filters != null && sprite.filters.length > 0) return sprite.filters;

		return camera.filters;
	}

	static function report(message:String):Void
	{
		if (warned) return;

		warned = true;
		Sys.println ("[vulkan] " + message);
	}


	/**
	 * Writes a short report into logs/vulkan-filters.txt so a run can be
	 * diagnosed from the log folder instead of a console window.
	 */
	public static function writeReport(frameId:Int):Void
	{
		if (reports >= 60) return;

		if (frameId > 20)
		{
			if (reportTimer > 0)
			{
				reportTimer--;
				return;
			}

			reportTimer = 300;
		}

		reports++;

		try
		{
			var lines:Array<String> = [];

			lines.push ("frame=" + frameId + " enabled=" + enabled + " passes=" + passes
				+ " composites=" + composites + " unsupported=" + unsupported
				+ " targets=" + VkRenderTargets.created
				+ " submitted=" + VkQuads.submitted + " fx=" + VkQuads.fxQuads
				+ " shadow=" + VkCameraFilters.shadow);

			lines.push ("error=" + nativeError ());

			var list = FlxG.cameras == null ? null : FlxG.cameras.list;

			if (list == null)
			{
				lines.push ("cameras=none");
			}
			else
			{
				for (i in 0...list.length)
				{
					var camera = list[i];

					if (camera == null)
					{
						lines.push ("cam" + i + "=null");
						continue;
					}

					var sprite = camera.flashSprite;
					var onSprite:Int = sprite == null || sprite.filters == null ? -1 : sprite.filters.length;
					var own:Int = camera.filters == null ? -1 : camera.filters.length;

					var status = "";

					if (camera.filters != null)
					{
						for (filter in camera.filters)
						{
							if (filter == null)
							{
								status += " [null]";
								continue;
							}

							var name = Type.getClassName (Type.getClass (filter)).split (".").pop ();

							status += " [" + name + (supported (filter) ? ":" + passCount (filter) : ":skipped") + "]";
						}
					}

					lines.push ("cam" + i + " filtersEnabled=" + camera.filtersEnabled
						+ " own=" + own + " onSprite=" + onSprite
						+ " rect=" + cameraRectText (camera) + status);
				}
			}

			// The timeline only looks at the camera lines: the frame counter and the
			// per frame pass totals change every frame and would defeat the comparison.
			remember (frameId, lines.slice (2).join ("\n"));

			if (!FileSystem.exists ("logs")) FileSystem.createDirectory ("logs");

			var text = Date.now ().toString () + "\n" + lines.join ("\n") + "\n";

			if (history.length > 0) text += "\n--- filter setups seen this session ---\n" + history.join ("\n") + "\n";

			File.saveContent ("logs/vulkan-filters.txt", text);
		}
		catch (error:Dynamic) {}
	}

	/**
	 * Keeps the last ten distinct camera/filter setups. A run that walks from the
	 * menu into a song leaves a short timeline behind instead of one snapshot.
	 */
	static function remember(frameId:Int, key:String):Void
	{
		if (key == historyKey) return;

		historyKey = key;

		var line = "f=" + frameId + " " + key.split ("\n").join (" | ");

		history.push (line);

		while (history.length > 10) history.shift ();
	}

	/** The last error the native side reported, so a silent fallback is visible. */
	static function nativeError():String
	{
		for (line in VkNative.statusText ().split ("\n"))
		{
			if (StringTools.startsWith (line, "error=")) return line.substr (6);
		}

		return "?";
	}

	static function cameraRectText(camera:FlxCamera):String
	{
		var rect = VkQuads.cameraRect (camera);

		return Std.int (rect[0]) + "," + Std.int (rect[1]) + "," + Std.int (rect[2]) + "x" + Std.int (rect[3]);
	}

	public static function debugLine():String
	{
		return "filterPasses=" + passes + " filterLayers=" + composites
			+ (unsupported > 0 ? " filterSkipped=" + unsupported : "");
	}
}

#end
