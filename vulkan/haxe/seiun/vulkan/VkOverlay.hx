// Repaints the OpenFL children of the stage that are not part of the game sprite.
//
// OpenFL keeps walking its display list while Vulkan owns the window. Text and
// any Graphics that OpenFL cannot tessellate are rasterized by Cairo into
// Graphics.__bitmap, so this class only turns those bitmaps and plain Bitmaps
// into quads.

package seiun.vulkan;

#if seiun_vulkan

import flixel.FlxCamera;
import flixel.FlxG;
import openfl.display.Bitmap;
import openfl.display.BitmapData;
import openfl.display.BlendMode;
import openfl.display.DisplayObject;
import openfl.display.DisplayObjectContainer;
import openfl.display.Graphics;
import openfl.display.OpenGLRenderer;
import openfl.display.Stage;
import openfl.geom.Matrix;
import openfl.text.TextField;

@:access(openfl.display.DisplayObject)
@:access(openfl.display.DisplayObjectContainer)
@:access(openfl.display.Graphics)
@:access(openfl.display.Bitmap)
@:access(openfl.display.Stage)
class VkOverlay
{
	public static var enabled:Bool = true;

	/** Quads emitted for the last frame. */
	public static var drawn(default, null):Int = 0;

	/** Visible graphics with no rasterized bitmap and no drawable quad. */
	public static var missing(default, null):Int = 0;

	/** Frame the overlay was last drawn on. */
	public static var drawnFrame(default, null):Int = -1;

	static var stageRenderer:VkStageRenderer = null;

	static var view:Array<Float> = [1.0, 0.0, 0.0, 1.0, 0.0, 0.0];
	static var matrix:Matrix = new Matrix ();

	// The camera canvas holds the flixel draw data and is painted by VkQuads, so
	// rasterizing it here would paint the background colour over the frame.
	static var flixelSurfaces:Array<DisplayObject> = [];

	static var parsed:Bool = false;
	static var keepGL:Bool = false;
	static var dumpLeft:Int = 0;

	/**
		Hands the stage draw call to Vulkan. Runs once the stage exists; a renderer
		OpenFL created again (context restore) is replaced on the next frame.
	**/
	public static function install():Void
	{
		parseArgs ();

		if (keepGL) return;

		var stage = FlxG.stage;

		if (stage == null) return;

		var current = stage.__renderer;

		if (Std.isOfType (current, VkStageRenderer))
		{
			stageRenderer = cast current;
			return;
		}

		if (!Std.isOfType (current, OpenGLRenderer)) return;

		var replacement = new VkStageRenderer (cast current);

		stage.__renderer = replacement;
		stageRenderer = replacement;

		Sys.println ("[vulkan] stage renderer replaced: OpenFL issues no draw calls");
	}

	/** True when the stage is drawn through Vulkan, so OpenFL issues no GL calls. */
	public static function ownsStage():Bool
	{
		return stageRenderer != null;
	}

	public static function render():Int
	{
		return renderStage (stageRenderer);
	}

	public static function renderStage(renderer:VkStageRenderer):Int
	{
		if (renderer != null) stageRenderer = renderer;

		drawn = 0;
		missing = 0;

		if (!enabled) return 0;
		if (!VkRuntime.isFrameOpen ()) return 0;

		// The cameras are finished before the stage overlay draws: the overlay
		// belongs on top of the composited cameras, not inside the last camera's
		// render target where the HUD filters would blur it.
		VkQuads.flushCameraFx (0x7FFFFFFF);

		var stage = FlxG.stage;

		if (stage == null) return 0;

		var game:Dynamic = FlxG.game;
		var children = stage.__children;

		if (children == null) return 0;

		drawnFrame = VkRuntime.frameId;

		collectFlixelSurfaces ();

		for (child in children)
		{
			if (child == null) continue;

			walk (child, 0);
		}

		// Leave the batch without a view or a clip, so whatever draws next starts
		// from a known state.
		VkNative.setView (null);
		VkNative.setScissor (0, 0, 0, 0);

		return drawn;
	}

	static function walk(object:DisplayObject, depth:Int):Void
	{
		if (object == null || !object.visible) return;

		for (surface in flixelSurfaces)
		{
			if (surface == object)
			{
				cleanupRemoved (object);
				return;
			}
		}

		var alpha:Float = object.__worldAlpha;

		if (alpha <= 0) return;

		if (dumpLeft > 0) describe (object, alpha, depth);

		var cache = object.__cacheBitmap;

		if (cache != null)
		{
			// cacheAsBitmap: OpenFL would have drawn this instead of the children.
			if (cache.__bitmapData != null)
				quad (cache.__bitmapData, object.__renderTransform, alpha, object.__worldBlendMode, cache.smoothing);
			return;
		}

		var textField:TextField = Std.isOfType (object, TextField) ? cast object : null;

		if (object.__graphics != null) drawGraphics (object, textField, alpha);

		var container = Std.isOfType (object, DisplayObjectContainer) ? (cast object : DisplayObjectContainer) : null;

		if (container != null)
		{
			container.__cleanupRemovedChildren ();

			var children = container.__children;

			if (children != null)
			{
				for (child in children) walk (child, depth + 1);
			}

			return;
		}

		if (Std.isOfType (object, Bitmap))
		{
			var bitmap:Bitmap = cast object;

			if (bitmap.__bitmapData != null)
			{
				quad (bitmap.__bitmapData, object.__renderTransform, alpha, object.__worldBlendMode, bitmap.smoothing);
			}
		}
	}

	/** Records the display objects flixel draws itself, one entry per camera. */
	static function collectFlixelSurfaces():Void
	{
		flixelSurfaces = [];

		var cameras = FlxG.cameras == null ? null : FlxG.cameras.list;

		if (cameras == null) return;

		for (camera in cameras)
		{
			if (camera == null) continue;

			if (camera.canvas != null) flixelSurfaces.push (camera.canvas);
		}
	}

	static function cleanupRemoved(object:DisplayObject):Void
	{
		var container = Std.isOfType (object, DisplayObjectContainer) ? (cast object : DisplayObjectContainer) : null;

		if (container == null) return;

		container.__cleanupRemovedChildren ();

		var children = container.__children;

		if (children != null)
		{
			for (child in children) cleanupRemoved (child);
		}
	}

	static function drawGraphics(object:DisplayObject, textField:TextField, alpha:Float):Void
	{
		var graphics:Graphics = object.__graphics;

		if (stageRenderer != null)
		{
			// Text is laid out on its first paint: before that the graphics holds no
			// bitmap and reports itself invisible, so painting has to come first.
			if (textField != null) stageRenderer.paintText (textField);
			else if (graphics.__visible && object.__renderable) stageRenderer.paintGraphics (graphics);
		}

		if (!graphics.__visible || !object.__renderable) return;

		var bitmap = graphics.__bitmap;

		if (bitmap == null || bitmap.image == null)
		{
			if (!graphics.__managed && graphics.__commands != null && graphics.__commands.length > 0) missing++;
			return;
		}

		// The bitmap holds the graphics in bitmap pixels; this is the transform back
		// to stage space, the same one OpenFL uses when it draws that texture.
		var scale = graphics.__bitmapScale > 0 ? 1.0 / graphics.__bitmapScale : 1.0;

		matrix.identity ();
		matrix.scale (scale, scale);
		matrix.concat (graphics.__worldTransform);

		// Text is rasterized without the owning alpha, so it is applied here. Other
		// graphics bake that alpha into their pixels already.
		quad (bitmap, matrix, textField != null ? alpha : 1.0, object.__worldBlendMode, true);
	}

	static function quad(bitmap:BitmapData, transform:Matrix, alpha:Float, blend:BlendMode, smooth:Bool):Void
	{
		if (bitmap == null || bitmap.width <= 0 || bitmap.height <= 0) return;

		var texture = VkTextures.getBitmapData (bitmap);

		if (!VkNative.valid (texture))
		{
			missing++;
			return;
		}

		view[0] = transform.a;
		view[1] = transform.b;
		view[2] = transform.c;
		view[3] = transform.d;
		view[4] = transform.tx;
		view[5] = transform.ty;

		VkNative.setView (view);
		VkNative.setFilter (!smooth);
		VkNative.setTexture (texture);
		VkNative.setBlend (VkQuads.blendOf (blend));
		VkNative.setScissor (0, 0, 0, 0);

		var level = Std.int (alpha * 255);

		if (level < 0) level = 0;
		if (level > 255) level = 255;

		if (VkNative.quad (0, 0, bitmap.width, bitmap.height, 0, 0, 1, 1, (level << 24) | 0xFFFFFF))
		{
			drawn++;
		}
		else
		{
			missing++;
		}
	}

	static function parseArgs():Void
	{
		if (parsed) return;

		parsed = true;

		for (arg in Sys.args ())
		{
			if (arg == "--vulkan-no-overlay") enabled = false;
			if (arg == "--vulkan-overlay-dump") dumpLeft = 2;
			if (arg == "--vulkan-keep-gl") keepGL = true;
		}
	}

	/** One line per node: type, alpha, transform, and what would be sampled. */
	static function describe(object:DisplayObject, alpha:Float, depth:Int):Void
	{
		var name = Type.getClassName (Type.getClass (object));
		var graphics:Graphics = object.__graphics;
		var source = "none";

		if (object.__cacheBitmap != null)
		{
			source = "cache=" + object.__cacheBitmap.width + "x" + object.__cacheBitmap.height;
		}
		else if (Std.isOfType (object, Bitmap))
		{
			var bitmap:Bitmap = cast object;
			source = bitmap.__bitmapData == null ? "bitmap=null" : "bitmap=" + bitmap.__bitmapData.width + "x" + bitmap.__bitmapData.height;
		}
		else if (graphics != null)
		{
			source = graphics.__bitmap == null
				? "graphics=unrasterized(" + (graphics.__commands == null ? -1 : graphics.__commands.length) + " cmds)"
				: "graphics=" + graphics.__bitmap.width + "x" + graphics.__bitmap.height
					+ " scale=" + graphics.__bitmapScale;
		}

		var transform = object.__renderTransform;
		var pad = "";
		for (i in 0...depth) pad += "  ";

		var container = Std.isOfType (object, DisplayObjectContainer) ? (cast object : DisplayObjectContainer) : null;
		var count = container == null || container.__children == null ? 0 : container.__children.length;

		Sys.println ('[vulkan] overlay ' + pad + name + " id=" + (object.name == null ? "-" : object.name)
			+ " kids=" + count + " alpha=" + alpha + " " + source
			+ " m=[" + transform.a + "," + transform.b + "," + transform.c + "," + transform.d
			+ "," + transform.tx + "," + transform.ty + "]");
	}

	public static function stats():String
	{
		return "overlay=" + drawn + " missing=" + missing + " stage=" + (stageRenderer != null ? "vulkan" : "gl");
	}
}

#end
