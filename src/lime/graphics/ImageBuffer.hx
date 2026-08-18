package lime.graphics;

import haxe.io.Bytes;
import lime.graphics.cairo.CairoSurface;
import lime.utils.UInt8Array;
#if (js && html5)
import lime._internal.graphics.ImageCanvasUtil;
import js.html.CanvasElement;
import js.html.CanvasRenderingContext2D;
import js.html.Image as HTMLImage;
import js.html.ImageData;
import js.Browser;
#if haxe4
import js.lib.Uint8ClampedArray;
#else
import js.html.Uint8ClampedArray;
#end
#elseif flash
import flash.display.BitmapData;
#end

/**
	`ImageBuffer` is a simple object for storing image data.

	For higher-level operations, use the `Image` class.
**/
#if !lime_debug
@:fileXml('tags="haxe,release"')
@:noDebug
#end
#if hl
@:keep
#end
@:allow(lime.graphics.Image)
class ImageBuffer
{
	/**
		The number of bits per pixel in this image data
	**/
	public var bitsPerPixel:Int;

	/**
		The data for this image, represented as a `UInt8Array`
	**/
	public var data:UInt8Array;

	/**
		The `PixelFormat` for this image data
	**/
	public var format:PixelFormat;

	/**
		The height of this image data
	**/
	public var height:Int;

	/**
		Whether the image data has premultiplied alpha
	**/
	public var premultiplied:Bool;

	/**
		The data for this image, represented as a `js.html.CanvasElement`, `js.html.Image` or `flash.display.BitmapData`
	**/
	public var src(get, set):Dynamic;

	/**
		The stride, or number of data values (in bytes) per row in the image data
	**/
	public var stride(get, never):Int;

	/**
		Whether this image data is transparent
	**/
	public var transparent:Bool;

	/**
		The width of this image data
	**/
	public var width:Int;

	@:noCompletion private var __srcBitmapData:#if flash BitmapData #else Dynamic #end;
	@:noCompletion private var __srcCanvas:#if (js && html5) CanvasElement #else Dynamic #end;
	@:noCompletion private var __srcContext:#if (js && html5) CanvasRenderingContext2D #else Dynamic #end;
	@:noCompletion private var __srcCustom:Dynamic;
	@:noCompletion private var __srcImage:#if (js && html5) HTMLImage #else Dynamic #end;
	@:noCompletion private var __srcImageData:#if (js && html5) ImageData #else Dynamic #end;

	#if commonjs
	private static function __init__()
	{
		var p = untyped ImageBuffer.prototype;
		untyped Object.defineProperties(p,
			{
				"src": {get: p.get_src, set: p.set_src},
				"stride": {get: p.get_stride}
			});
	}
	#end

	/**
		Creates a new `ImageBuffer` instance
		@param	data	(Optional) Initial `UInt8Array` data
		@param	width	(Optional) An initial `width` value
		@param	height	(Optional) An initial `height` value
		@param	bitsPerPixel	(Optional) The `bitsPerPixel` of the data (default is 32)
		@param	format	(Optional) The `PixelFormat` of this image buffer
	**/
	public function new(data:UInt8Array = null, width:Int = 0, height:Int = 0, bitsPerPixel:Int = 32, format:PixelFormat = null)
	{
		this.data = data;
		this.width = width;
		this.height = height;
		this.bitsPerPixel = bitsPerPixel;
		this.format = (format == null ? RGBA32 : format);
		premultiplied = false;
		transparent = true;
	}

	/**
		Creates a duplicate of this `ImageBuffer`

		If the current `ImageBuffer` has `data` or `src` information, this will be
		cloned as well.
		@return	A new `ImageBuffer` with duplicate values
	**/
	public function clone():ImageBuffer
	{
		var buffer = new ImageBuffer(data, width, height, bitsPerPixel);

		#if kha
		// TODO
		#elseif flash
		if (__srcBitmapData != null) buffer.__srcBitmapData = __srcBitmapData.clone();
		#elseif (js && html5)
		if (data != null)
		{
			// Fast path: UInt8Array constructor already copies the source,
			// no need for extra .set() call
			buffer.data = new UInt8Array(data);
		}
		else if (__srcImageData != null)
		{
			buffer.__srcCanvas = cast Browser.document.createElement("canvas");
			buffer.__srcContext = cast buffer.__srcCanvas.getContext("2d");
			buffer.__srcCanvas.width = __srcImageData.width;
			buffer.__srcCanvas.height = __srcImageData.height;
			// Uint8ClampedArray constructor performs the copy directly
			buffer.__srcImageData = buffer.__srcContext.createImageData(__srcImageData.width, __srcImageData.height);
			buffer.__srcImageData.data.set(new Uint8ClampedArray(__srcImageData.data));
		}
		else if (__srcCanvas != null)
		{
			buffer.__srcCanvas = cast Browser.document.createElement("canvas");
			buffer.__srcContext = cast buffer.__srcCanvas.getContext("2d");
			buffer.__srcCanvas.width = __srcCanvas.width;
			buffer.__srcCanvas.height = __srcCanvas.height;
			buffer.__srcContext.drawImage(__srcCanvas, 0, 0);
		}
		else
		{
			buffer.__srcImage = __srcImage;
		}
		#elseif nodejs
		if (data != null)
		{
			buffer.data = new UInt8Array(data);
		}
		buffer.__srcCustom = __srcCustom;
		#else
		if (data != null)
		{
			// Native targets: use Bytes.blit for bulk memory copy
			var bytes = Bytes.alloc(data.byteLength);
			bytes.blit(0, data.buffer, Std.int(data.byteOffset), data.byteLength);
			buffer.data = new UInt8Array(bytes);
		}
		#end

		buffer.bitsPerPixel = bitsPerPixel;
		buffer.format = format;
		buffer.premultiplied = premultiplied;
		buffer.transparent = transparent;
		return buffer;
	}

	// Get & Set Methods
	@:noCompletion private function get_src():Dynamic
	{
		#if (js && html5)
		if (__srcImage != null) return __srcImage;
		return __srcCanvas;
		#elseif flash
		return __srcBitmapData;
		#else
		return __srcCustom;
		#end
	}

	@:noCompletion private function set_src(value:Dynamic):Dynamic
	{
		#if (js && html5)
		if ((value is HTMLImage))
		{
			__srcImage = cast value;
		}
		else if ((value is CanvasElement))
		{
			__srcCanvas = cast value;
			__srcContext = cast __srcCanvas.getContext("2d");
		}
		#elseif flash
		__srcBitmapData = cast value;
		#else
		__srcCustom = value;
		#end

		return value;
	}

	@:noCompletion private function get_stride():Int
	{
		return width * Std.int(bitsPerPixel / 8);
	}

	/**
	 * Frees the pixel data (`data` and `src` resources) held by this buffer.
	 * After calling this, the buffer should no longer be used.
	 */
	public function dispose():Void
	{
		data = null;
		#if flash
		if (__srcBitmapData != null) { __srcBitmapData.dispose(); __srcBitmapData = null; }
		#end
		#if (js && html5)
		__srcCanvas = null;
		__srcContext = null;
		__srcImage = null;
		__srcImageData = null;
		#end
		__srcCustom = null;
	}

	/**
	 * Release only the raw CPU-side pixel data (`data` UInt8Array),
	 * while preserving `src` (Canvas, BitmapData, Image element, etc.).
	 *
	 * Use this after uploading a texture to the GPU to reclaim CPU memory.
	 * This is especially important on mobile where GPU and CPU share RAM
	 * (unified memory architecture) — keeping both copies wastes memory.
	 *
	 * After calling this, `data` will be null but `src` remains usable.
	 * Call `readDataFromSource()` to re-populate `data` from `src` if needed.
	 */
	public function releaseData():Void
	{
		data = null;
	}

	/**
	 * Re-populate `data` from the current `src` (Canvas/BitmapData/etc.)
	 * after a previous `releaseData()` call.
	 * @return true if data was successfully restored.
	 */
	public function readDataFromSource():Bool
	{
		if (data != null) return true; // already have data

		#if flash
		if (__srcBitmapData != null)
		{
			var bytes = __srcBitmapData.getPixels(__srcBitmapData.rect);
			data = UInt8Array.fromBytes(bytes);
			return true;
		}
		#elseif (js && html5)
		if (__srcCanvas != null)
		{
			var ctx = __srcCanvas.getContext("2d");
			var imageData = ctx.getImageData(0, 0, __srcCanvas.width, __srcCanvas.height);
			data = new UInt8Array(imageData.data.buffer);
			return true;
		}
		#end
		return false;
	}
}
