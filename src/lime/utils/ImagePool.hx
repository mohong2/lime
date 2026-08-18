package lime.utils;

import lime.graphics.Image;
import lime.graphics.ImageBuffer;
import lime.graphics.PixelFormat;

/**
 * A simple object pool for reusing Image/ImageBuffer objects.
 *
 * FNF creates many temporary images during gameplay (render targets,
 * sprite caches, etc.). At 360fps, the GC overhead from constantly
 * allocating and discarding Image objects adds up significantly.
 *
 * This pool reuses released Image and ImageBuffer instances,
 * drastically reducing allocation pressure.
 *
 * Usage in FNF:
 *   // Instead of `new ImageBuffer(null, w, h)`:
 *   var buf = ImagePool.allocateBuffer(w, h);
 *
 *   // Instead of `new Image(buf)`:
 *   var img = ImagePool.allocateFromBuffer(buf);
 *
 *   // When done:
 *   ImagePool.release(img);
 *   ImagePool.releaseBuffer(buf);
 */
class ImagePool
{
	/**
	 * Maximum pool size to prevent unbounded growth.
	 * Default: 64 entries per size bucket.
	 */
	public static var maxPoolSize:Int = 64;

	@:noCompletion private static var __bufferPool:Map<String, Array<ImageBuffer>> = new Map<String, Array<ImageBuffer>>();
	@:noCompletion private static var __imagePool:Array<Image> = [];

	/**
	 * Get a pooled ImageBuffer with the given dimensions,
	 * or create a new one if none are available.
	 */
	public static function allocateBuffer(width:Int, height:Int, bitsPerPixel:Int = 32, format:PixelFormat = null):ImageBuffer
	{
		if (format == null) format = RGBA32;
		var key:String = width + "x" + height + "x" + bitsPerPixel;

		var pool = __bufferPool.get(key);
		if (pool != null && pool.length > 0)
		{
			var buf = pool.pop();
			buf.width = width;
			buf.height = height;
			buf.bitsPerPixel = bitsPerPixel;
			buf.format = format;
			buf.premultiplied = false;
			buf.transparent = true;

			// Ensure data buffer is large enough
			var neededSize:Int = width * height * Std.int(bitsPerPixel / 8);
			if (buf.data == null || buf.data.byteLength < neededSize)
			{
				buf.data = new UInt8Array(neededSize);
			}
			return buf;
		}

		return new ImageBuffer(new UInt8Array(width * height * Std.int(bitsPerPixel / 8)), width, height, bitsPerPixel, format);
	}

	/**
	 * Return an ImageBuffer to the pool for reuse.
	 * The data is NOT cleared — it will be overwritten on next allocation.
	 */
	public static function releaseBuffer(buffer:ImageBuffer):Void
	{
		if (buffer == null) return;

		var key:String = buffer.width + "x" + buffer.height + "x" + buffer.bitsPerPixel;
		var pool = __bufferPool.get(key);
		if (pool == null)
		{
			pool = [];
			__bufferPool.set(key, pool);
		}

		if (pool.length < maxPoolSize)
		{
			// Clear src references to avoid memory leaks
			buffer.src = null;
			pool.push(buffer);
		}
	}

	/**
	 * Get a pooled Image wrapping an ImageBuffer.
	 */
	public static function allocateFromBuffer(buffer:ImageBuffer):Image
	{
		if (__imagePool.length > 0)
		{
			var img = __imagePool.pop();
			img.buffer = buffer;
			img.offsetX = 0;
			img.offsetY = 0;
			img.width = buffer.width;
			img.height = buffer.height;
			img.dirty = true;
			img.version++;
			return img;
		}
		return new Image(buffer, 0, 0, buffer.width, buffer.height);
	}

	/**
	 * Return an Image to the pool.
	 * The underlying ImageBuffer is NOT released — call releaseBuffer() separately
	 * or use releaseAll() to free both.
	 */
	public static function release(image:Image):Void
	{
		if (image == null) return;
		if (__imagePool.length < maxPoolSize)
		{
			image.scaledVersions = new Map<String, Image>();
			image.buffer = null;
			__imagePool.push(image);
		}
	}

	/**
	 * Release both an Image and its ImageBuffer to their respective pools.
	 * Convenience method for the common case.
	 */
	public static function releaseAll(image:Image):Void
	{
		if (image == null) return;
		releaseBuffer(image.buffer);
		release(image);
	}

	/**
	 * Clear all pooled objects, freeing memory.
	 * Call this during scene transitions when you know old resources are done.
	 */
	public static function clear():Void
	{
		__bufferPool = new Map<String, Array<ImageBuffer>>();
		__imagePool = [];
	}

	/**
	 * Get diagnostic stats about the pool.
	 */
	public static function getStats():{bufferCount:Int, imageCount:Int, sizeBuckets:Int}
	{
		var bufCount:Int = 0;
		for (arr in __bufferPool)
		{
			bufCount += arr.length;
		}
		return {
			bufferCount: bufCount,
			imageCount: __imagePool.length,
			sizeBuckets: Lambda.count(__bufferPool)
		};
	}
}
