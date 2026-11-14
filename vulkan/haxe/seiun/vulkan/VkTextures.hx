// FlxGraphic -> texture cache. One texture per graphic.
//
// CPU pixels are released after a successful upload when the engine installed a
// reload callback for the bitmap, so textures do not keep a second copy alive.

package seiun.vulkan;

#if seiun_vulkan

import cpp.UInt64;
import flixel.graphics.FlxGraphic;
import sys.FileSystem;
import sys.io.File;
import lime.graphics.Image;
import lime.graphics.PixelFormat;
import openfl.display.BitmapData;
import openfl.utils._internal.UInt8Array;

@:access(openfl.display.BitmapData)
class VkTextures
{
	static inline var SWEEP_INTERVAL = 120;

	static var handles:Map<FlxGraphic, UInt64> = new Map ();

	static var versions:Map<FlxGraphic, Int> = new Map ();

	// The bitmap the cached texture was built from. A graphic can swap its bitmap
	// (a video replaces it when the stream's format changes), and the texture of
	// the old one would keep the old size.
	static var sources:Map<FlxGraphic, BitmapData> = new Map ();

	// Whether the cached texture holds palette indices, so a bitmap that switches
	// representation is uploaded again instead of drawn from the wrong format.
	static var paletteState:Map<FlxGraphic, Bool> = new Map ();

	// Bitmaps that no FlxGraphic owns: OpenFL's rasterized graphics and plain
	// BitmapData created by overlays.
	static var bitmapHandles:Map<BitmapData, UInt64> = new Map ();

	static var bitmapVersions:Map<BitmapData, Int> = new Map ();

	// Last frame each BitmapData was drawn. OpenFL rebuilds overlay bitmaps when a
	// label changes, and a strong map entry would keep the old one alive forever.
	static var bitmapFrame:Map<BitmapData, Int> = new Map ();

	static inline var BITMAP_LIFETIME = 600;

	static var textureCount:Int = 0;
	static var cached:Int = 0;

	// Device memory held by textures, split by pixel format so the cost of an
	// RGBA upload is visible next to what an index texture would take.
	public static var rgbaBytes(default, null):Float = 0;
	public static var indexBytes(default, null):Float = 0;
	public static var rgbaTextureCount(default, null):Int = 0;
	public static var indexTextureCount(default, null):Int = 0;

	static var graphicBytes:Map<FlxGraphic, Float> = new Map ();
	static var bitmapBytes:Map<BitmapData, Float> = new Map ();

	// Last frame each graphic was drawn, for the memory budget below.
	static var graphicFrame:Map<FlxGraphic, Int> = new Map ();

	// --vulkan-texbudget=MB. A negative value means "no budget": textures then live
	// as long as their graphic does, which is what the cache did before.
	static var budgetBytes:Float = -1;
	public static var budgetEvicted(default, null):Int = 0;
	static var frame:Int = 0;
	static var nextSweep:Int = SWEEP_INTERVAL;

	public static var uploaded(default, null):Int = 0;
	public static var missed(default, null):Int = 0;
	public static var unsupported(default, null):Int = 0;
	public static var evicted(default, null):Int = 0;
	public static var restored(default, null):Int = 0;
	public static var pixelless(default, null):Int = 0;
	public static var dropped(default, null):Int = 0;
	public static var updated(default, null):Int = 0;

	static var loggedFormat:Bool = false;
	static var loggedBytes:Bool = false;
	static var loggedStride:Bool = false;

	static var noCache:Bool = false;
	static var texStats:Bool = false;
	static var texDump:String = null;
	static var argsParsed:Bool = false;

	public static function get(graphic:FlxGraphic):UInt64
	{
		parseArgs ();

		if (graphic == null) return skip ("graphic-null", null);

		var bitmap = graphic.bitmap;

		if (bitmap == null) return skip ("bitmap-null", graphic);

		graphicFrame.set (graphic, frame);

		if (noCache) forget (graphic);

		// A bitmap can switch between palette indices and RGBA pixels, and the two
		// need textures of different formats, so the cached one cannot be reused.
		if (handles.exists (graphic) && paletteState.get (graphic) != bitmap.paletteApplied) forget (graphic);

		if (handles.exists (graphic) && sources.get (graphic) != bitmap) forget (graphic);

		var handle = handles.get (graphic);

		if (handle != null)
		{
			if (!VkNative.valid (handle))
			{
				forget (graphic);
			}
			else
			{
				var known:Image = bitmap.image;
				var version:Null<Int> = versions.get (graphic);

				// Pixels are back in memory and were edited since the upload.
				if (known == null || version == null || known.version == version) return handle;

				if (refresh (handle, bitmap))
				{
					versions.set (graphic, known.version);
					return handle;
				}

				forget (graphic);
			}
		}

		var image:Image = bitmap.image;

		// Palette bitmaps are uploaded from their index bytes, so a released RGBA
		// copy is not a reason to give up on them.
		if (image == null && !bitmap.paletteApplied)
		{
			try
			{
				bitmap.restoreCPU ();
			}
			catch (error:Dynamic) {}

			image = bitmap.image;

			if (image == null) return skip ("image-null", graphic);
		}

		handle = upload (bitmap);

		if (!VkNative.valid (handle))
		{
			missed++;
			logLine ("upload failed key=" + (graphic.key == null ? "?" : graphic.key)
				+ " " + bitmap.width + "x" + bitmap.height + " " + nativeError ());
			return 0;
		}

		// Read this before the upload releases the pixels.
		var uploadedImage:Image = bitmap.image;

		var bytes:Float = bitmap.width * bitmap.height * (bitmap.paletteApplied ? 1.0 : 4.0);

		graphicBytes.set (graphic, bytes);
		account (bytes, bitmap.paletteApplied);

		handles.set (graphic, handle);
		sources.set (graphic, bitmap);
		paletteState.set (graphic, bitmap.paletteApplied);
		versions.set (graphic, uploadedImage == null ? -1 : uploadedImage.version);
		cached++;
		textureCount++;
		uploaded++;

		return handle;
	}

	/**
	 * Texture for a BitmapData that is not backed by a FlxGraphic.
	 *
	 * Re-uploaded when the bitmap image version changes, which is what OpenFL
	 * bumps after it re-rasterizes graphics or text.
	 */
	public static function getBitmapData(bitmap:BitmapData):UInt64
	{
		parseArgs ();

		if (bitmap == null) return 0;

		bitmapFrame.set (bitmap, frame);

		var handle = bitmapHandles.get (bitmap);

		if (handle != null)
		{
			if (!VkNative.valid (handle))
			{
				forgetBitmapData (bitmap);
			}
			else
			{
				var current:Image = bitmap.image;
				var version:Null<Int> = bitmapVersions.get (bitmap);

				if (current == null || version == null || current.version == version) return handle;

				if (refresh (handle, bitmap))
				{
					bitmapVersions.set (bitmap, current.version);
					return handle;
				}

				forgetBitmapData (bitmap);
			}
		}

		handle = upload (bitmap);

		if (!VkNative.valid (handle))
		{
			missed++;
			return 0;
		}

		var uploadedImage:Image = bitmap.image;

		var bytes:Float = bitmap.width * bitmap.height * (bitmap.paletteApplied ? 1.0 : 4.0);

		bitmapBytes.set (bitmap, bytes);
		account (bytes, bitmap.paletteApplied);

		bitmapHandles.set (bitmap, handle);
		bitmapVersions.set (bitmap, uploadedImage == null ? -1 : uploadedImage.version);
		cached++;
		textureCount++;
		uploaded++;

		return handle;
	}

	/**
	 * True when a live texture already holds this graphic. The engine uses it as
	 * proof that the GPU copy exists, which is what lets the CPU pixels go.
	**/
	public static function holdsGraphic (graphic:FlxGraphic):Bool
	{
		if (graphic == null || graphic.bitmap == null) return false;

		var handle = handles.get (graphic);

		if (handle == null) return false;

		if (!VkNative.valid (handle))
		{
			forget (graphic);
			return false;
		}

		return true;
	}

	/**
	 * Uploads a graphic the engine wants resident, through the same path the
	 * renderer uses, so one texture table serves both. The upload releases the
	 * CPU pixels itself.
	**/
	public static function uploadGraphic (graphic:FlxGraphic):Bool
	{
		if (graphic == null || graphic.bitmap == null) return false;

		return VkNative.valid (get (graphic));
	}

	static function account(bytes:Float, index:Bool):Void
	{
		if (index)
		{
			indexBytes += bytes;
			indexTextureCount++;
		}
		else
		{
			rgbaBytes += bytes;
			rgbaTextureCount++;
		}
	}

	static function unaccount(bytes:Null<Float>, index:Null<Bool>):Void
	{
		if (bytes == null || bytes <= 0) return;

		if (index == true)
		{
			indexBytes -= bytes;
			indexTextureCount--;
		}
		else
		{
			rgbaBytes -= bytes;
			rgbaTextureCount--;
		}
	}

	static function forgetBitmapData(bitmap:BitmapData):Void
	{
		var handle = bitmapHandles.get (bitmap);

		unaccount (bitmapBytes.get (bitmap), false);
		bitmapBytes.remove (bitmap);
		bitmapHandles.remove (bitmap);
		bitmapVersions.remove (bitmap);
		bitmapFrame.remove (bitmap);

		if (handle == null) return;

		cached--;

		VkNative.textureRetire (handle);
		textureCount--;
		evicted++;
	}

	static function forget(graphic:FlxGraphic):Void
	{
		var handle = handles.get (graphic);

		unaccount (graphicBytes.get (graphic), paletteState.get (graphic));
		graphicBytes.remove (graphic);
		handles.remove (graphic);
		paletteState.remove (graphic);
		versions.remove (graphic);
		sources.remove (graphic);
		graphicFrame.remove (graphic);

		if (handle == null) return;

		cached--;

		VkNative.textureRetire (handle);
		textureCount--;
		evicted++;
	}

	static var reasons:Map<String, Int> = new Map ();
	static var reasonSamples:Map<String, String> = new Map ();

	static function skip(reason:String, graphic:FlxGraphic):UInt64
	{
		var count = reasons.get (reason);
		reasons.set (reason, (count == null ? 0 : count) + 1);

		if (!reasonSamples.exists (reason))
		{
			var key = graphic == null ? "(null graphic)" : (graphic.key == null ? "?" : graphic.key);
			reasonSamples.set (reason, key);
		}

		return 0;
	}

	public static function tick():Void
	{
		frame++;

		if (frame >= nextSweep) sweep (false);

		enforceBudget ();

		// One line every other second. A window build has no console, so this file
		// is the only place a missing texture or a fallback can be seen.
		if (frame % 60 == 0) logLine ("frame=" + frame + " " + nativeError () + VkQuads.slimLine () + " | " + stats ());
	}

	static var logLines:Int = 0;
	static var logPath:String = "logs/vulkan-textures.txt";

	/** The native error line, so a log entry says why and not only that. */
	public static function nativeError():String
	{
		for (line in VkNative.statusText ().split ("\n"))
		{
			if (StringTools.startsWith (line, "error=")) return line + " ";
		}

		return "";
	}

	static var logAll:Bool = false;
	static var logArgs:Bool = false;

	/**
	 * Appends one diagnostic line, capped so a long session cannot fill the disk.
	 *
	 * Off unless the game is started with --vulkan-texlog: this is a probe, and a
	 * probe that writes files on every run is a cost nobody asked for.
	 */
	public static function logLine(text:String):Void
	{
		if (!logArgs)
		{
			logArgs = true;

			for (arg in Sys.args ()) if (arg == "--vulkan-texlog") logAll = true;
		}

		if (!logAll) return;

		if (logLines >= 4000) return;

		try
		{
			if (logLines == 0)
			{
				if (!FileSystem.exists ("logs")) FileSystem.createDirectory ("logs");
				File.saveContent (logPath, Date.now ().toString () + "\n");
			}

			var out = File.append (logPath, false);
			out.writeString (text + "\n");
			out.close ();

			logLines++;
		}
		catch (error:Dynamic) {}
	}

	/**
	 * Evicts the least recently drawn textures once the resident total goes past
	 * --vulkan-texbudget.
	 *
	 * Only graphics that were not drawn in the last two frames are candidates: a
	 * texture on screen would be re-uploaded immediately, which is the one thing a
	 * budget must not cause. Anything that cannot be restored (no CPU pixels left
	 * and no restorable bitmap) is left alone rather than evicted into a hole.
	 */
	static function enforceBudget():Void
	{
		if (budgetBytes <= 0) return;

		var total = rgbaBytes + indexBytes;

		if (total <= budgetBytes) return;

		var candidates:Array<FlxGraphic> = [];

		for (graphic in handles.keys ())
		{
			if (graphic == null || graphic.bitmap == null) continue;

			var last:Null<Int> = graphicFrame.get (graphic);

			if (last != null && frame - last <= 2) continue;

			if (graphic.bitmap.image == null && !graphic.bitmap.restorable) continue;

			candidates.push (graphic);
		}

		candidates.sort (function(a, b)
		{
			var fa:Null<Int> = graphicFrame.get (a);
			var fb:Null<Int> = graphicFrame.get (b);

			return (fa == null ? -1 : fa) - (fb == null ? -1 : fb);
		});

		// Down to 90% of the budget, so the next few uploads do not immediately
		// trigger another eviction round.
		var target = budgetBytes * 0.9;

		for (graphic in candidates)
		{
			if (total <= target) break;

			var bytes:Null<Float> = graphicBytes.get (graphic);
			total -= (bytes == null ? 0.0 : bytes);

			forget (graphic);
			budgetEvicted++;
		}
	}

	public static function sweep(all:Bool):Void
	{
		nextSweep = frame + SWEEP_INTERVAL;

		var dead:Array<FlxGraphic> = [];

		for (graphic in handles.keys ())
		{
			if (all || graphic == null || graphic.bitmap == null) dead.push (graphic);
		}

		for (graphic in dead)
		{
			forget (graphic);
		}

		// Rasterized graphics bitmaps die with the object that owned them; the
		// image is the only handle left on that lifetime.
		var deadBitmaps:Array<BitmapData> = [];

		for (bitmap in bitmapHandles.keys ())
		{
			if (all || bitmap == null || bitmap.image == null)
			{
				deadBitmaps.push (bitmap);
				continue;
			}

			var last:Null<Int> = bitmapFrame.get (bitmap);

			if (last == null || frame - last > BITMAP_LIFETIME) deadBitmaps.push (bitmap);
		}

		for (bitmap in deadBitmaps)
		{
			forgetBitmapData (bitmap);
		}
	}

	static function parseArgs():Void
	{
		if (argsParsed) return;

		argsParsed = true;

		for (arg in Sys.args ())
		{
			if (arg == "--vulkan-no-cache") noCache = true;
			if (arg == "--vulkan-texstats") texStats = true;
			if (arg == "--vulkan-texdump") texDump = "logs/texdump";

			if (StringTools.startsWith (arg, "--vulkan-texbudget="))
			{
				var megabytes:Float = Std.parseFloat (arg.substr ("--vulkan-texbudget=".length));

				// Bytes, and never below a few megabytes: a budget of 0 would evict
				// every texture the moment it is created.
				if (megabytes >= 8) budgetBytes = megabytes * 1048576.0;
			}
		}
	}

	// The engine frees CPU pixels through BitmapData.dropCPU(), which needs a GL
	// texture and a GL context. Neither exists here, so without this every bitmap
	// would keep its pixels for the whole session. Same effect, gated on a reload
	// callback being installed.
	static function releaseCPU(bitmap:BitmapData):Void
	{
		if (bitmap == null || !bitmap.restorable) return;

		try
		{
			bitmap.image = null;
			bitmap.__surface = null;
			bitmap.__readable = false;
			dropped++;
		}
		catch (error:Dynamic) {}
	}

	static function pixels(bitmap:BitmapData):{ bytes:haxe.io.Bytes, width:Int, height:Int, flags:Int }
	{
		if (bitmap == null) return null;

		// A palette bitmap has no RGBA copy worth uploading: its pixels are one
		// index byte each and the 256 entry palette is sampled separately, which
		// keeps the GPU footprint at a quarter of the RGBA size.
		if (bitmap.paletteApplied)
		{
			var indices:UInt8Array = bitmap.__paletteIndices;

			if (indices == null || bitmap.width <= 0 || bitmap.height <= 0) return null;

			return { bytes: indices.toBytes (), width: bitmap.width, height: bitmap.height, flags: VkNative.TEXTURE_INDEX8 };
		}

		var image = bitmap.image;
		if (image == null) return null;

		var buffer = image.buffer;

		if (buffer == null || buffer.data == null)
		{
			try
			{
				bitmap.restoreCPU ();
			}
			catch (error:Dynamic) {}

			image = bitmap.image;
			buffer = image == null ? null : image.buffer;

			if (buffer == null || buffer.data == null)
			{
				pixelless++;
				return null;
			}

			restored++;
		}

		if (buffer.width <= 0 || buffer.height <= 0) return null;

		if (buffer.stride != buffer.width * 4)
		{
			if (!loggedStride)
			{
				loggedStride = true;
				Sys.println ("[vulkan] texture stride " + buffer.stride + " != width*4 ("
					+ (buffer.width * 4) + ") - such bitmaps are skipped");
			}

			return null;
		}

		var flags = 0;

		switch (buffer.format)
		{
			case RGBA32:

			case BGRA32:
				flags |= VkNative.TEXTURE_BGRA;

			default:
				if (!loggedFormat)
				{
					loggedFormat = true;
					logLine ("unsupported format " + Std.string (buffer.format) + " - such bitmaps are skipped");
					Sys.println ("[vulkan] unsupported bitmap format " + Std.string (buffer.format)
						+ " - such bitmaps are skipped");
				}

				unsupported++;
				return null;
		}

		var bytes:haxe.io.Bytes = buffer.data.toBytes ();

		if (!loggedBytes)
		{
			loggedBytes = true;

			var sample:String = "";
			var middle = Std.int (bytes.length / 2) & ~3;
			for (offset in [0, middle])
			{
				if (offset + 4 <= bytes.length)
				{
					sample += " @" + offset + "=" + bytes.get (offset) + "," + bytes.get (offset + 1)
						+ "," + bytes.get (offset + 2) + "," + bytes.get (offset + 3);
				}
			}

			Sys.println ("[vulkan] first texture: " + buffer.width + "x" + buffer.height
				+ " format=" + Std.string (buffer.format) + " stride=" + buffer.stride
				+ " premultiplied=" + buffer.premultiplied + " bytes=" + bytes.length + sample);
		}

		if (texDump != null)
		{
			try
			{
				if (!sys.FileSystem.exists (texDump)) sys.FileSystem.createDirectory (texDump);
				sys.io.File.saveBytes (texDump + "/" + buffer.width + "x" + buffer.height + "_" + uploaded + ".raw", bytes);
			}
			catch (error:Dynamic) {}
		}

		if (texStats)
		{
			var covered:Int = 0;
			var index:Int = 3;
			var total:Int = buffer.width * buffer.height;

			while (index < bytes.length)
			{
				if (bytes.get (index) > 8) covered++;
				index += 4;
			}

			var percent:Float = total <= 0 ? 0.0 : (covered * 100.0 / total);

			Sys.println ("[vulkan] tex " + buffer.width + "x" + buffer.height
				+ " coverage=" + Std.int (percent) + "%");
		}

		return { bytes: bytes, width: buffer.width, height: buffer.height, flags: flags };
	}

	static function upload(bitmap:BitmapData):UInt64
	{
		var data = pixels (bitmap);

		if (data == null) return 0;

		var handle:UInt64 = VkNative.textureCreateFromBytes (data.bytes, data.width, data.height, data.flags);

		if (VkNative.valid (handle)) releaseCPU (bitmap);

		return handle;
	}

	/**
	 * Re-uploads into the existing texture when the size did not change.
	 *
	 * Text and overlay graphics are rasterized again every time they change, and
	 * the usual path allocates a new image, a new descriptor and a new allocation
	 * for each of those. This path copies the pixels into the image that is
	 * already bound.
	 */
	static function refresh(handle:UInt64, bitmap:BitmapData):Bool
	{
		// Index and palette textures are immutable, and the RGBA bytes of a palette
		// bitmap must never be copied into its one byte image.
		if (!VkNative.valid (handle) || bitmap == null || bitmap.image == null || bitmap.paletteApplied) return false;

		var data = pixels (bitmap);

		if (data == null) return false;

		var size:Array<Int> = [0, 0];

		if (!VkNative.textureSize (handle, size)) return false;
		if (size[0] != data.width || size[1] != data.height) return false;

		if (!VkNative.textureUpdate (handle, data.bytes, data.flags)) return false;

		updated++;
		releaseCPU (bitmap);

		return true;
	}

	static inline function mb(bytes:Float):String
	{
		return Std.string (Math.round (bytes / 1048576.0 * 10) / 10);
	}

	public static function stats():String
	{
		var why:String = "";

		for (reason in reasons.keys ())
		{
			why += " " + reason + "=" + reasons.get (reason) + "(" + reasonSamples.get (reason) + ")";
		}

		return "vram=" + mb (rgbaBytes + indexBytes) + "MB rgba=" + mb (rgbaBytes) + "MB/"
			+ rgbaTextureCount + " index=" + mb (indexBytes) + "MB/" + indexTextureCount
			+ " textures=" + textureCount + " cached=" + cached + " uploaded=" + uploaded
			+ " missed=" + missed + " unsupported=" + unsupported + " evicted=" + evicted
			+ " restored=" + restored + " pixelless=" + pixelless + " dropped=" + dropped
			+ " updated=" + updated
			+ (budgetBytes > 0 ? " budget=" + mb (budgetBytes) + "MB budgetEvicted=" + budgetEvicted : "")
			+ why;
	}
}

#end
