// Stage renderer slot.
//
// OpenFL keeps the display list, the transform pass and the Cairo text
// rasterization, and loses only the draw call: the traversal ends in Vulkan
// quads instead of OpenGL state changes.

package seiun.vulkan;

#if seiun_vulkan

import openfl.display.CairoRenderer;
import openfl.display.DisplayObjectRenderer;
import openfl.display.Graphics;
import openfl.display.IBitmapDrawable;
import openfl.display.OpenGLRenderer;
import openfl.display._internal.CairoGraphics;
import openfl.display._internal.CairoTextField;
import openfl.geom.Matrix;
import openfl.text.TextField;

@:access(openfl.display.OpenGLRenderer)
@:access(openfl.display.DisplayObjectRenderer)
@:access(openfl.display.DisplayObject)
@:access(openfl.display.Graphics)
@:access(openfl.display3D.Context3D)
@:access(openfl.text.TextField)
class VkStageRenderer extends OpenGLRenderer
{
	public function new(source:OpenGLRenderer)
	{
		super (source.__context3D);

		// Stage only assigns this when it creates a renderer, so a replacement has
		// to inherit it: __resize reads it, and a resize can arrive at any time.
		__stage = source.__stage;

		__worldTransform = source.__worldTransform;
		__pixelRatio = source.__pixelRatio;
		__allowSmoothing = source.__allowSmoothing;

		__softwareRenderer.__worldTransform = new Matrix ();
	}

	/** Rasterizes a Graphics into Graphics.__bitmap when it is out of date. */
	public function paintGraphics(graphics:Graphics):Void
	{
		var software:CairoRenderer = cast __softwareRenderer;

		software.__pixelRatio = __pixelRatio;

		var cache = software.__worldTransform;

		software.__worldTransform = __worldTransform;

		CairoGraphics.render (graphics, software);

		software.__worldTransform = cache;
	}

	/** Rebuilds the text layout and rasterizes it into Graphics.__bitmap. */
	public function paintText(textField:TextField):Void
	{
		var software:CairoRenderer = cast __softwareRenderer;

		software.__pixelRatio = __pixelRatio;

		if (software.__worldTransform == null) software.__worldTransform = __worldTransform;

		CairoTextField.render (textField, software, textField.__worldTransform);

		textField.__graphics.__hardwareDirty = false;
	}

	@:noCompletion override function __resize(width:Int, height:Int):Void
	{
		// A renderer that was replaced before the stage attached it has nothing to
		// measure; the same holds while the window is being torn down.
		if (__stage == null && __defaultRenderTarget == null) return;

		super.__resize (width, height);
	}

	@:noCompletion override function __render(object:IBitmapDrawable):Void
	{
		// A fallback before the swapchain exists can still hand the window back, so
		// this renderer behaves exactly like the one it replaced.
		if (!VkRuntime.enabled)
		{
			// The window was handed to Vulkan, and the OpenGL context went with it: running
			// openfl's GL path here would throw instead of falling back.
			if (VkRuntime.glReleased) return;

			super.__render (object);
			return;
		}

		try
		{
			VkOverlay.renderStage (this);
		}
		catch (error:Dynamic)
		{
			VkOverlay.enabled = false;
			Sys.println ("[vulkan] overlay disabled after an error: " + Std.string (error));
		}

		// Stage.__render drops the window render event when this stays false, which
		// would also skip the end of frame listener.
		__context3D.__present = true;
		__cleared = true;
	}

	@:noCompletion override function __clear():Void
	{
		if (!VkRuntime.enabled)
		{
			if (VkRuntime.glReleased) return;

			super.__clear ();
			return;
		}

		__cleared = true;
	}
}

#end
