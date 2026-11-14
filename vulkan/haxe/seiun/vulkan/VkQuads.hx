// Seam between flixel's quad batch (FlxDrawQuadsItem) and the native batch API.

package seiun.vulkan;

#if seiun_vulkan

import cpp.UInt64;
import flixel.FlxCamera;
import flixel.FlxG;
import flixel.graphics.tile.FlxDrawQuadsItem;
import flixel.util.FlxColor;
import openfl.display.BitmapData;
import openfl.display.BlendMode;
import openfl.display.Shader;

@:access(flixel.FlxCamera)
@:access(flixel.graphics.tile.FlxDrawQuadsItem)
@:access(openfl.display.DisplayObject)
class VkQuads
{

	public static var submitted(default, null):Int = 0;

	public static var skipped(default, null):Int = 0;

	public static var customShaders(default, null):Int = 0;

	static var view:Array<Float> = [1.0, 0.0, 0.0, 1.0, 0.0, 0.0];
	static var flat:Bool = false;
	static var probe:Bool = false;
	static var flatParsed:Bool = false;
	static var whiteTexture:UInt64 = 0;
	static var hasWhiteTexture:Bool = false;

	/** Fade and flash rects drawn this frame. */
	public static var fxQuads(default, null):Int = 0;

	/** Cameras up to this list index already had their fade and flash drawn. */
	static var fxIndex:Int = 0;

	static var probeLine:String = "(no batch yet)";

	static var traceFrames:Int = 0;
	static var traceIndex:Int = 0;

	/**
	 * Opens the frame for this tick, before an item knows whether it draws anything:
	 * a frame with no sprites still needs the clear, the camera effects and the overlay.
	**/
	public static function beginSubmit(camera:FlxCamera):Void
	{
		if (!VkRuntime.enabled || camera == null) return;

		parseFlags ();

		if (!VkRuntime.prepareFrame (clearColor ())) return;

		// The camera the frame moved on from is finished: its fade and flash belong
		// under the next camera's sprites, exactly where flixel would have put them.
		var cameraIndex:Int = cameraList ().indexOf (camera);

		if (cameraIndex > fxIndex) flushCameraFx (cameraIndex);

		// A filtered camera draws into its own target; the filter chain runs when
		// the frame moves past it (see flushCameraFx).
		VkCameraFilters.beginLayer (camera);
	}

	static function parseFlags():Void
	{
		if (flatParsed) return;

		flatParsed = true;

		for (arg in Sys.args ())
		{
			if (arg == "--vulkan-flat") flat = true;
			if (arg == "--vulkan-probe") probe = true;
		}
	}

	public static function submit(item:FlxDrawQuadsItem, camera:FlxCamera, shader:Shader):Bool
	{
		if (!VkRuntime.enabled) return false;
		if (item == null || camera == null) return false;

		try
		{
			return submitInner (item, camera, shader);
		}
		catch (error:Dynamic)
		{
			VkRuntime.fail ("VkQuads.submit threw: " + Std.string (error)
				+ "\n" + haxe.CallStack.toString (haxe.CallStack.exceptionStack (true)));
			return false;
		}
	}

	static function submitInner(item:FlxDrawQuadsItem, camera:FlxCamera, shader:Shader):Bool
	{
		// A skipped frame keeps the window contents; the frame was never opened.
		if (!VkRuntime.isFrameOpen ()) return true;

		if (traceFrames == 0 && traceIndex == 0)
		{
			for (arg in Sys.args ())
			{
				if (arg == "--vulkan-trace") traceFrames = 40;
			}
		}

		var quadCount:Int = item.quadCount;

		if (quadCount <= 0) return true;

		// A shader can be fed a bitmap other than the batch graphic: flixel hands
		// palette sprites a plain RGBA copy when a per-sprite shader cannot expand
		// indices. That copy is the texture this draw has to sample.
		var bitmapInput:Dynamic = shader != null ? Reflect.field (shader, "bitmap") : null;
		var input:BitmapData = (bitmapInput != null) ? bitmapInput.input : null;

		var texture:UInt64;

		if (input != null && (item.graphics == null || input != item.graphics.bitmap))
		{
			texture = VkTextures.getBitmapData (input);
		}
		else
		{
			texture = VkTextures.get (item.graphics);
		}

		if (flat && quadCount == 1 && item.rectsArray != null
			&& item.rectsArray[2] >= 600 && item.rectsArray[3] >= 400)
		{

			return true;
		}

		if (flat)
		{
			texture = white ();
		}

		noteTexture (item, texture, quadCount);

		if (!VkNative.valid (texture))
		{

			skipped += quadCount;
			reportMissing (item, quadCount);
			return true;
		}

		if (probeLine == "(no batch yet)") probeLine = describe (item, quadCount);

		if (probe)
		{
			VkNative.setTexture (texture);
			VkNative.setView (null);
			VkNative.setBlend (0);
			VkNative.setScissor (0, 0, 0, 0);
			VkNative.quad (0, 0, 1921, 1081, 0, 0, 1, 1, 0xFFFFFFFF);
			return true;
		}

		var matrix = viewOf (camera);

		// Camera space -> the layer's own pixel space, when this camera is filtered.
		VkCameraFilters.toLayer (camera, matrix);

		// Same rule flixel uses for the GL sampler: smooth only when the camera or
		// the sprite asked for it, so pixel art stays nearest by default.
		var smooth:Bool = camera.antialiasing || item.antialiasing;

		if (item.graphics != null && item.graphics.bitmap != null && item.graphics.bitmap.paletteApplied) smooth = false;

		// A mod shader gets its own pipeline; the stock one keeps the built-in path,
		// which draws the same pixels without a program switch per batch.
		var program:VkProgram = shader != null ? VkProgram.get (shader) : null;
		var custom:Bool = program != null && program.isValid ();

		// Switches the program and flushes the previous batch, so it has to come
		// before this item's texture and blend state are set.
		if (custom) VkNative.setProgram (program.handle);

		VkNative.setView (matrix);
		VkNative.setFilter (!smooth);
		VkNative.setTexture (texture);
		VkNative.setBlend (blendOf (item.blend));
		scissorFor (camera);

		// Uniforms and samplers belong to the program, and the batch is drawn when
		// the program is switched back, so they are filled in before the quads go in.
		if (custom)
		{
			// OpenFL writes this one from the bound bitmap on every draw. A shader
			// that samples its neighbours (texel = 1.0 / openfl_TextureSize) divides
			// by zero when the uniform keeps its default.
			VkProgram.applyTextureSize (shader, texture);
			program.bind (shader);
		}

		if (traceFrames > 0)
		{
			traceFrames--;
			var size:Array<Int> = [0, 0];
			VkNative.textureSize (texture, size);
			var gkey:String = item.graphics == null ? "?" : (item.graphics.key == null ? "?" : item.graphics.key);
			var fromShader:Bool = input != null && (item.graphics == null || input != item.graphics.bitmap);
			var texBitmap:BitmapData = input != null ? input : (item.graphics != null ? item.graphics.bitmap : null);
			Sys.println ('[vulkan] item #' + traceIndex + ' key=' + gkey + ' bmp=' + (fromShader ? "shader" : "graphic")
				+ ' idx8=' + (texBitmap != null && texBitmap.paletteApplied ? 1 : 0) + ': quads=' + quadCount
				+ ' rect=[' + item.rectsArray[0] + ',' + item.rectsArray[1] + ','
				+ item.rectsArray[2] + ',' + item.rectsArray[3] + ']'
				+ ' m=[' + item.transformsArray[0] + ',' + item.transformsArray[2] + ','
				+ item.transformsArray[4] + ',' + item.transformsArray[5] + ']'
				+ ' alpha=' + (item.uniformAlpha ? item.firstAlpha : -1)
				+ ' colored=' + item.hasVertexColors
				+ ' nAlpha=' + (item.alphas == null ? -1 : item.alphas.length)
				+ ' nMult=' + (item.colorMultipliers == null ? -1 : item.colorMultipliers.length)
				+ ' blend=' + item.blend + ' tex=' + texture
				+ ' view=' + matrix[0] + ',' + matrix[2] + ',' + matrix[4] + ',' + matrix[5]
				+ ' cam=' + camera.x + ',' + camera.y + ',' + camera.width + 'x' + camera.height
				+ ' idx=' + (FlxG.cameras != null && FlxG.cameras.list != null ? FlxG.cameras.list.indexOf (camera) : -9)
				+ ' texSize=' + size[0] + 'x' + size[1]
				+ ' uv=' + (item.rectsArray[0] + item.rectsArray[2]) / (size[0] == 0 ? 1 : size[0])
				+ ',' + (item.rectsArray[1] + item.rectsArray[3]) / (size[1] == 0 ? 1 : size[1]));
			traceIndex++;
		}

		var alphas:Array<Float> = item.uniformAlpha ? null : item.alphas;
		var multipliers:Array<Float> = item.hasVertexColors ? item.colorMultipliers : null;
		var offsets:Array<Float> = item.hasVertexColors ? item.colorOffsets : null;

		if (VkNative.quads (item.rectsArray, item.transformsArray, quadCount,
			alphas, item.uniformAlpha ? item.firstAlpha : 1.0, multipliers, offsets))
		{
			submitted += quadCount;
		}
		else
		{
			skipped += quadCount;
		}

		if (custom)
		{
			// Draws this batch while the program's uniform block still holds the
			// values above, then returns to the built-in pipeline.
			VkNative.setProgram (0);
			customShaders++;
		}

		return true;
	}

	/** Starts a frame: no camera has had its special effects drawn yet. */
	public static function resetCameraFx():Void
	{
		fxIndex = 0;
		fxQuads = 0;
	}

	/**
		Draws the fade and flash rect of every camera the frame has moved past.

		flixel records these as a plain filled rect on the camera canvas, which is the
		one part of the camera output that never reaches the quad batch.
	**/
	public static function flushCameraFx(upTo:Int):Void
	{
		var list = cameraList ();

		if (upTo > list.length) upTo = list.length;

		while (fxIndex < upTo)
		{
			var camera = list[fxIndex];

			// A camera that drew nothing still owns its layer: bind it so the fade
			// lands inside the target, then run the chain over the finished layer.
			VkCameraFilters.beginLayer (camera);

			drawCameraFx (camera);

			VkCameraFilters.finishLayer ();

			fxIndex++;
		}
	}

	static function drawCameraFx(camera:FlxCamera):Void
	{
		if (camera == null) return;

		var flash:Float = camera._fxFlashAlpha;

		if (flash > 0) fillRect (camera, camera._fxFlashColor & 0xFFFFFF, fxAlpha (camera._fxFlashColor, flash));

		var fade:Float = camera._fxFadeAlpha;

		if (fade > 0) fillRect (camera, camera._fxFadeColor & 0xFFFFFF, fxAlpha (camera._fxFadeColor, fade));
	}

	static inline function fxAlpha(color:FlxColor, amount:Float):Float
	{
		var level:Int = color.alpha;

		return ((level <= 0 ? 0xFF : level) * amount) / 255.0;
	}

	static function fillRect(camera:FlxCamera, rgb:Int, alpha:Float):Void
	{
		if (alpha <= 0) return;

		var texture:UInt64 = white ();

		if (!VkNative.valid (texture)) return;

		var matrix = viewOf (camera);

		VkCameraFilters.toLayer (camera, matrix);

		VkNative.setView (matrix);
		VkNative.setFilter (true);
		VkNative.setTexture (texture);
		VkNative.setBlend (0);
		scissorFor (camera);

		var level = Std.int (alpha * 255);

		if (level < 0) level = 0;
		if (level > 255) level = 255;

		// The vertex colour multiplies a premultiplied texel, so the tint has to be
		// premultiplied too or a light flash would blow out instead of fading.
		var r = Std.int (((rgb >> 16) & 0xFF) * level / 255.0);
		var g = Std.int (((rgb >> 8) & 0xFF) * level / 255.0);
		var b = Std.int ((rgb & 0xFF) * level / 255.0);

		if (VkNative.quad (camera.viewOffsetX - 1, camera.viewOffsetY - 1,
			camera.viewWidth + 2, camera.viewHeight + 2, 0, 0, 1, 1,
			(level << 24) | (r << 16) | (g << 8) | b))
		{
			fxQuads++;
		}
	}

	static function white():UInt64
	{
		if (!hasWhiteTexture)
		{
			var bytes:haxe.io.Bytes = haxe.io.Bytes.alloc (4);
			bytes.set (0, 255); bytes.set (1, 255); bytes.set (2, 255); bytes.set (3, 255);
			whiteTexture = VkNative.textureCreateFromBytes (bytes, 1, 1, 0);
			hasWhiteTexture = true;
		}

		return whiteTexture;
	}

	static inline function cameraList():Array<FlxCamera>
	{
		return (FlxG.cameras != null && FlxG.cameras.list != null) ? FlxG.cameras.list : [];
	}

	public static function backgroundColor():Int
	{
		return clearColor ();
	}

	static function clearColor():Int
	{

		for (arg in Sys.args ())
		{
			if (arg == "--vulkan-magenta") return 0xFFFF00FF;
		}

		var color:Int = FlxG.cameras != null ? FlxG.cameras.bgColor : 0xFF000000;

		return color | 0xFF000000;
	}

	static function viewOf(camera:FlxCamera):Array<Float>
	{
		var canvas = camera.canvas;

		if (canvas == null)
		{
			view[0] = 1; view[1] = 0; view[2] = 0; view[3] = 1; view[4] = 0; view[5] = 0;
			return view;
		}

		var matrix = canvas.__getWorldTransform ();

		view[0] = matrix.a;
		view[1] = matrix.b;
		view[2] = matrix.c;
		view[3] = matrix.d;
		view[4] = matrix.tx;
		view[5] = matrix.ty;

		return view;
	}

	/**
	 * Where a camera sits on screen, in device pixels: x, y, width, height.
	 * Zoom only changes how much world the camera covers, not the area it owns.
	**/
	public static function cameraRect(camera:FlxCamera):Array<Float>
	{
		var rect:Array<Float> = [0.0, 0.0, 0.0, 0.0];

		if (camera == null) return rect;

		var width = camera.width;
		var height = camera.height;

		if (width <= 0 || height <= 0) return rect;

		var scale = FlxG.scaleMode != null && FlxG.scaleMode.scale != null ? FlxG.scaleMode.scale : null;
		var scaleX:Float = scale != null ? scale.x : 1.0;
		var scaleY:Float = scale != null ? scale.y : 1.0;
		var zoom:Float = camera.initialZoom > 0 ? camera.initialZoom : 1.0;

		rect[0] = camera.x * scaleX;
		rect[1] = camera.y * scaleY;
		rect[2] = width * zoom * scaleX;
		rect[3] = height * zoom * scaleY;

		return rect;
	}

	static var noScissor:Bool = false;
	static var scissorParsed:Bool = false;

	// Clip region of a camera in screen pixels. This is the camera rect, not the
	// camera rect scaled by zoom: zoom only changes how much world the camera
	// covers, the area it occupies on screen stays the same.
	static function scissorFor(camera:FlxCamera):Void
	{
		if (!scissorParsed)
		{
			scissorParsed = true;

			for (arg in Sys.args ())
			{
				if (arg == "--vulkan-noscissor") noScissor = true;
			}
		}

		if (noScissor)
		{
			VkNative.setScissor (0, 0, 0, 0);
			return;
		}

		// Inside its own render target the camera fills the whole target, so the
		// clip is the target and not the camera's place on screen.
		if (VkCameraFilters.isLayer (camera))
		{
			VkNative.setScissor (0, 0, 0, 0);
			return;
		}

		var rect = cameraRect (camera);

		if (rect[2] <= 0 || rect[3] <= 0)
		{
			VkNative.setScissor (0, 0, 0, 0);
			return;
		}

		var x0 = Std.int (Math.floor (rect[0]));
		var y0 = Std.int (Math.floor (rect[1]));
		var x1 = Std.int (Math.ceil (rect[0] + rect[2]));
		var y1 = Std.int (Math.ceil (rect[1] + rect[3]));

		VkNative.setScissor (x0, y0, x1 - x0, y1 - y0);
	}

	static function describe(item:FlxDrawQuadsItem, quadCount:Int):String
	{
		var rects = item.rectsArray;
		var transforms = item.transformsArray;

		if (rects == null || transforms == null || rects.length < 4 || transforms.length < 6)
		{
			return "rects=" + (rects == null ? "null" : Std.string (rects.length))
				+ " transforms=" + (transforms == null ? "null" : Std.string (transforms.length));
		}

		return "quadCount=" + quadCount
			+ " rect=[" + rects[0] + "," + rects[1] + "," + rects[2] + "," + rects[3] + "]"
			+ " matrix=[" + transforms[0] + "," + transforms[1] + "," + transforms[2] + ","
			+ transforms[3] + "," + transforms[4] + "," + transforms[5] + "]"
			+ " alpha=" + item.firstAlpha + " uniformAlpha=" + item.uniformAlpha
			+ " colored=" + item.hasVertexColors
			+ " tex=" + (item.graphics != null ? item.graphics.width + "x" + item.graphics.height : "none");
	}

	public static function restartTrace():Void
	{
		for (arg in Sys.args ())
		{
			if (arg == "--vulkan-trace")
			{
				traceFrames = 40;
				traceIndex = 0;
				Sys.println ('[vulkan] trace restart at state ' + (FlxG.state == null ? "?" : Type.getClassName (Type.getClass (FlxG.state))));
				return;
			}
		}
	}

	static var reportedMissing:Map<String, Bool> = new Map ();
	static var seenTextures:Map<String, Bool> = new Map ();

	/** Compact line for the periodic log: the counters, no draw list. */
	public static function slimLine():String
	{
		// The native text carries a per draw record list; only the counters are wanted here.
		var kept:String = "";

		for (line in VkNative.batchStatsText ().split ("\n"))
		{
			if (StringTools.startsWith (line, "draw") || StringTools.startsWith (line, "q")) continue;

			kept += line + " ";
		}

		return "quads=" + submitted + " skipped=" + skipped + " shaders=" + customShaders + "/" + VkProgram.created
			+ " end=" + Std.int (VkRuntime.endMs * 100.0) / 100.0 + "ms " + VkCameraFilters.debugLine ()
			+ " " + kept;
	}

	/** Names every texture the frame draws for the first time, so a quad can be traced to an asset. */
	static function noteTexture(item:FlxDrawQuadsItem, texture:UInt64, quads:Int):Void
	{
		var id:String = Std.string (texture);

		if (seenTextures.exists (id)) return;

		seenTextures.set (id, true);

		var size:Array<Int> = [0, 0];
		VkNative.textureSize (texture, size);

		var key:String = item.graphics == null ? "(none)" : (item.graphics.key == null ? "?" : item.graphics.key);

		VkTextures.logLine ("newtex handle=" + id + " key=" + key + " tex=" + size[0] + "x" + size[1]
			+ " quads=" + quads + " rect=" + item.rectsArray[0] + "," + item.rectsArray[1] + ","
			+ item.rectsArray[2] + "," + item.rectsArray[3]
			+ " colored=" + item.hasVertexColors + " alpha=" + (item.uniformAlpha ? item.firstAlpha : -1));
	}

	/** A draw without a texture leaves a hole; name the graphic once so the hole is traceable. */
	static function reportMissing(item:FlxDrawQuadsItem, quads:Int):Void
	{
		var key:String = item.graphics == null ? "(none)" : (item.graphics.key == null ? "?" : item.graphics.key);

		if (reportedMissing.exists (key)) return;

		reportedMissing.set (key, true);

		var bitmap:String = (item.graphics == null || item.graphics.bitmap == null) ? "null" : "ok";

		VkTextures.logLine ("missing key=" + key + " quads=" + quads + " bitmap=" + bitmap
			+ " rect=" + item.rectsArray[0] + "," + item.rectsArray[1] + "," + item.rectsArray[2] + "," + item.rectsArray[3]);
	}

	public static function debugLine():String
	{
		var stats = VkNative.batchStatsText ().split ("\n").join (" ");

		var cameras:String = "";

		if (FlxG.cameras != null && FlxG.cameras.list != null)
		{
			for (i in 0...FlxG.cameras.list.length)
			{
				var camera = FlxG.cameras.list[i];

				if (camera == null) continue;

				var matrix = viewOf (camera);

				cameras += " [cam" + i + " " + camera.width + "x" + camera.height
					+ " zoom=" + camera.zoom + " angle=" + camera.angle
					+ " view=" + matrix[0] + "," + matrix[1] + "," + matrix[2] + ","
					+ matrix[3] + "," + matrix[4] + "," + matrix[5] + "]";
			}
		}

		return "submitted=" + submitted + " skipped=" + skipped + " fx=" + fxQuads
			+ " " + VkCameraFilters.debugLine ()
			+ " shaders=" + customShaders + "/" + VkProgram.created
			+ (VkProgram.failed > 0 ? " shaderFail=" + VkProgram.failed : "")
			+ " end=" + Std.int (VkRuntime.endMs * 100.0) / 100.0 + "ms" + cameras
			+ " view=[" + view[0] + "," + view[1] + "," + view[2] + "," + view[3] + "," + view[4] + "," + view[5] + "]"
			+ " | " + stats + " | first: " + probeLine;
	}

	public static function blendOf(mode:BlendMode):Int
	{
		if (mode == null) return 0;

		// Mirrors lime_vulkan_blend in the public header. Modes the fixed function
		// blend unit cannot express (invert, difference, overlay, hardlight) fall back
		// to normal instead of drawing nothing: wrong looking beats invisible.
		return switch ((mode : String))
		{
			case "add": 1;
			case "multiply": 2;
			case "screen": 3;
			case "erase": 4;
			case "subtract": 6;
			case "darken": 7;
			case "lighten": 8;
			default: 0;
		}
	}
}

#end
