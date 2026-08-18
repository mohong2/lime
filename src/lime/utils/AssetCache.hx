package lime.utils;

import haxe.macro.Compiler;
import lime.media.AudioBuffer;
import lime.graphics.Image;
import lime.system.System;
#if !(macro || commonjs)
import lime._internal.macros.AssetsMacro;
#end

#if !lime_debug
@:fileXml('tags="haxe,release"')
@:noDebug
#end
class AssetCache
{
	/**
	 * Maximum estimated memory (in bytes) the cache is allowed to consume.
	 * Set to 0 to disable automatic eviction.
	 * Default: 3 GB on desktop, 768 MB on mobile (auto-detected).
	 */
	public var maxMemoryBytes:Float = 3.0 * 1024 * 1024 * 1024; // 3 GB default

	/**
	 * Maximum number of cached entries. When exceeded, oldest entries
	 * are evicted first. Set to 0 to disable. Default: 0 (unlimited).
	 * On mobile, consider setting to ~2000 to prevent Map overhead.
	 */
	public var maxEntryCount:Int = 0;

	/**
	 * Current estimated memory usage of all cached assets, in bytes.
	 */
	public var estimatedMemoryBytes(default, null):Float = 0;

	/**
	 * Total number of cache evictions performed since creation.
	 */
	public var totalEvictions(default, null):Int = 0;

	/**
	 * Total number of cached entries across all types.
	 */
	public var totalEntries(get, never):Int;

	public var audio:Map<String, AudioBuffer>;
	public var enabled:Bool = true;
	public var image:Map<String, Image>;
	public var font:Map<String, Dynamic /*Font*/>;
	public var version:Int;

	@:noCompletion private var __accessCounters:Map<String, Int>;
	@:noCompletion private var __globalAccessCounter:Int = 0;
	@:noCompletion private var __assetSizes:Map<String, Float>;

	public function new()
	{
		audio = new Map<String, AudioBuffer>();
		font = new Map<String, Dynamic /*Font*/>();
		image = new Map<String, Image>();
		__accessCounters = new Map<String, Int>();
		__assetSizes = new Map<String, Float>();

		// Auto-detect mobile and apply tighter limits
		if (System.isMobile)
		{
			configureForMobile();
		}

		#if (macro || commonjs || lime_disable_assets_version)
		version = 0;
		#elseif lime_assets_version
		version = Std.parseInt(Compiler.getDefine("lime-assets-version"));
		#else
		version = AssetsMacro.cacheVersion();
		#end
	}

	/**
	 * Apply mobile-appropriate memory limits.
	 * - 768 MB cache cap (mobile GPU shares system RAM)
	 * - 3000 max entries (prevents HashMap bloat)
	 *
	 * Call this manually if you need even tighter settings, e.g.:
	 *   Assets.cache.configureForMobile();
	 *   Assets.cache.maxMemoryBytes = 512 * 1024 * 1024; // 512 MB
	 */
	public function configureForMobile():Void
	{
		maxMemoryBytes = 768 * 1024 * 1024; // 768 MB for mobile
		maxEntryCount = 3000;
	}

	public function exists(id:String, ?type:AssetType):Bool
	{
		if (type == AssetType.IMAGE || type == null)
		{
			if (image.exists(id)) return true;
		}

		if (type == AssetType.FONT || type == null)
		{
			if (font.exists(id)) return true;
		}

		if (type == AssetType.SOUND || type == AssetType.MUSIC || type == null)
		{
			if (audio.exists(id)) return true;
		}

		return false;
	}

	@:noCompletion private function get_totalEntries():Int
	{
		var count:Int = 0;
		for (_ in image) count++;
		for (_ in font) count++;
		for (_ in audio) count++;
		return count;
	}

	/**
	 * Returns a diagnostic snapshot of the cache state.
	 * Useful for debugging memory issues at runtime.
	 * @return An object with count, memoryBytes, evictions, etc.
	 */
	public function getStats():CacheStats
	{
		var imageBytes:Float = 0;
		var audioBytes:Float = 0;
		var imageCount:Int = 0;
		var audioCount:Int = 0;
		var fontCount:Int = 0;

		for (key => size in __assetSizes)
		{
			if (image.exists(key)) { imageBytes += size; imageCount++; }
			else if (audio.exists(key)) { audioBytes += size; audioCount++; }
			else { fontCount++; } // font entries have size=0
		}

		return {
			totalEntries: totalEntries,
			imageCount: imageCount,
			audioCount: audioCount,
			fontCount: fontCount,
			estimatedMemoryBytes: estimatedMemoryBytes,
			imageMemoryBytes: imageBytes,
			audioMemoryBytes: audioBytes,
			totalEvictions: totalEvictions,
			enabled: enabled,
			maxMemoryBytes: maxMemoryBytes
		};
	}

	/**
	 * Record access to an asset for LRU tracking.
	 */
	public function touch(id:String):Void
	{
		if (__accessCounters.exists(id))
		{
			__accessCounters.set(id, ++__globalAccessCounter);
		}
	}

	/**
	 * Remove a specific asset from the cache by id (searches all types).
	 * @return true if an asset was found and removed.
	 */
	public function remove(id:String):Bool
	{
		if (disposeAndRemove(id))
		{
			totalEvictions++;
			return true;
		}
		return false;
	}

	/**
	 * Evict least-recently-used assets until `bytesToFree` bytes are freed.
	 * @return actual bytes freed.
	 */
	public function evict(bytesToFree:Float):Float
	{
		if (bytesToFree <= 0) return 0;

		var freed:Float = 0;

		var entries:Array<{key:String, lastAccess:Int, size:Float}> = [];
		for (key => counter in __accessCounters)
		{
			var size = __assetSizes.exists(key) ? __assetSizes.get(key) : 0;
			entries.push({key: key, lastAccess: counter, size: size});
		}

		entries.sort(function(a, b) return a.lastAccess - b.lastAccess);

		for (entry in entries)
		{
			if (freed >= bytesToFree) break;

			if (disposeAndRemove(entry.key))
			{
				freed += entry.size;
				totalEvictions++;
			}
		}

		return freed;
	}

	/**
	 * Evict assets that haven't been accessed in the last `maxFramesUnused`
	 * touch cycles. At 360 FPS, `maxFramesUnused=360` ≈ 1 second of inactivity.
	 * This is lighter-weight than full memory-pressure eviction.
	 *
	 * Call this once per frame (or every N frames) for best results.
	 */
	public function evictByFrames(maxFramesUnused:Int):Int
	{
		if (maxFramesUnused <= 0 || __globalAccessCounter <= maxFramesUnused) return 0;

		var threshold:Int = __globalAccessCounter - maxFramesUnused;
		var removed:Int = 0;

		// Collect stale keys first to avoid modifying map during iteration
		var staleKeys:Array<String> = [];
		for (key => lastAccess in __accessCounters)
		{
			if (lastAccess > 0 && lastAccess < threshold)
			{
				staleKeys.push(key);
			}
		}

		for (key in staleKeys)
		{
			if (disposeAndRemove(key))
			{
				removed++;
				totalEvictions++;
			}
		}

		return removed;
	}

	/**
	 * Evict entries until total count is <= maxEntryCount.
	 * If maxEntryCount is 0, does nothing.
	 */
	public function evictByCount():Int
	{
		if (maxEntryCount <= 0 || totalEntries <= maxEntryCount) return 0;

		var toRemove:Int = totalEntries - maxEntryCount;
		var removed:Int = 0;

		var entries:Array<{key:String, lastAccess:Int, size:Float}> = [];
		for (key => counter in __accessCounters)
		{
			var size = __assetSizes.exists(key) ? __assetSizes.get(key) : 0;
			entries.push({key: key, lastAccess: counter, size: size});
		}

		entries.sort(function(a, b) return a.lastAccess - b.lastAccess);

		for (entry in entries)
		{
			if (removed >= toRemove) break;
			if (disposeAndRemove(entry.key))
			{
				removed++;
				totalEvictions++;
			}
		}

		return removed;
	}

	/**
	 * Dispose pixel data of cached images without removing entries.
	 * Images will need reloading on next use.
	 */
	public function disposeImageData(prefix:String = null):Void
	{
		for (key => img in image)
		{
			if (prefix == null || StringTools.startsWith(key, prefix))
			{
				if (img != null && img.buffer != null && img.buffer.data != null)
				{
					img.buffer.data = null;
				}
			}
		}
	}

	/**
	 * Aggressive memory release: frees CPU-side pixel data from ALL cached
	 * images AND audio buffers, but keeps cache entries so assets can be
	 * re-loaded on demand.
	 *
	 * Call this on mobile `onLowMemory` events or when entering a memory-
	 * intensive scene. Typically frees 60-80% of cache memory instantly.
	 */
	public function releaseAllData():Void
	{
		for (key => img in image)
		{
			if (img != null && img.buffer != null) img.buffer.releaseData();
		}
		for (key => buf in audio)
		{
			if (buf != null) buf.releaseData();
		}
		// Recalculate: images now have zero data size, audio same
		estimatedMemoryBytes = 0;
		for (key => size in __assetSizes)
		{
			if (audio.exists(key)) estimatedMemoryBytes += size;
			// Images not counted after releaseData
		}
	}

	/**
	 * Respond to an OS-level low-memory warning (especially Android).
	 * Evicts 50% of memory usage immediately, then releases data from
	 * the remaining entries.
	 *
	 * Call this from your application's memory warning handler:
	 *   Application.current.onLowMemory.add(() -> Assets.cache.onLowMemory());
	 */
	public function onLowMemory():Void
	{
		if (estimatedMemoryBytes > 0)
		{
			// First, evict half the memory by removing LRU entries
			var targetFree:Float = estimatedMemoryBytes * 0.5;
			evict(targetFree);
		}
		// Then release raw data from remaining entries
		releaseAllData();
		totalEvictions += 1; // count as one low-memory event
	}

	public function set(id:String, type:AssetType, asset:Dynamic):Void
	{
		removeTracking(id);

		switch (type)
		{
			case FONT:
				font.set(id, asset);

			case IMAGE:
				if (!(asset is Image)) throw "Cannot cache non-Image asset: " + asset + " as Image";
				image.set(id, asset);
				var img:Image = cast asset;
				if (img != null)
				{
					var size:Float = img.width * img.height * 4.0;
					__assetSizes.set(id, size);
					estimatedMemoryBytes += size;
				}

			case SOUND, MUSIC:
				if (!(asset is AudioBuffer)) throw "Cannot cache non-AudioBuffer asset: " + asset + " as AudioBuffer";
				audio.set(id, asset);
				var buf:AudioBuffer = cast asset;
				if (buf != null && buf.data != null)
				{
					var size:Float = buf.data.length * 2.0;
					__assetSizes.set(id, size);
					estimatedMemoryBytes += size;
				}

			default:
				throw type + " assets are not cachable";
		}

		__accessCounters.set(id, 0);

		if (maxMemoryBytes > 0 && estimatedMemoryBytes > maxMemoryBytes)
		{
			evict(estimatedMemoryBytes - maxMemoryBytes);
		}

		if (maxEntryCount > 0 && totalEntries > maxEntryCount)
		{
			evictByCount();
		}
	}

	public function clear(prefix:String = null):Void
	{
		if (prefix == null)
		{
			for (img in image)
			{
				if (img != null && img.buffer != null) img.dispose();
			}

			audio = new Map<String, AudioBuffer>();
			font = new Map<String, Dynamic /*Font*/>();
			image = new Map<String, Image>();
			__accessCounters = new Map<String, Int>();
			__assetSizes = new Map<String, Float>();
			estimatedMemoryBytes = 0;
			__globalAccessCounter = 0;
		}
		else
		{
			var keys = audio.keys();

			for (key in keys)
			{
				if (StringTools.startsWith(key, prefix))
				{
					audio.remove(key);
					removeTracking(key);
				}
			}

			var keys = font.keys();

			for (key in keys)
			{
				if (StringTools.startsWith(key, prefix))
				{
					font.remove(key);
					removeTracking(key);
				}
			}

			var keys = image.keys();

			for (key in keys)
			{
				if (StringTools.startsWith(key, prefix))
				{
					var img = image.get(key);
					if (img != null && img.buffer != null) img.dispose();
					image.remove(key);
					removeTracking(key);
				}
			}
		}
	}

	@:noCompletion private function removeTracking(id:String):Void
	{
		__accessCounters.remove(id);
		if (__assetSizes.exists(id))
		{
			estimatedMemoryBytes -= __assetSizes.get(id);
			__assetSizes.remove(id);
		}
	}

	/**
	 * Dispose and remove a single cached entry by key.
	 * Used internally by all eviction methods.
	 * @return true if an entry was found and removed.
	 */
	@:noCompletion private function disposeAndRemove(id:String):Bool
	{
		var removed = false;

		if (image.exists(id))
		{
			var img = image.get(id);
			if (img != null && img.buffer != null) img.dispose();
			image.remove(id);
			removed = true;
		}
		else if (audio.exists(id))
		{
			audio.remove(id);
			removed = true;
		}
		else if (font.exists(id))
		{
			font.remove(id);
			removed = true;
		}

		if (removed) removeTracking(id);
		return removed;
	}
}

/**
 * Diagnostic snapshot returned by `AssetCache.getStats()`.
 */
typedef CacheStats =
{
	/** Total cached entries (images + audio + fonts). */
	var totalEntries:Int;
	/** Number of cached images. */
	var imageCount:Int;
	/** Number of cached audio buffers. */
	var audioCount:Int;
	/** Number of cached fonts. */
	var fontCount:Int;
	/** Total estimated memory (bytes) across all cached assets. */
	var estimatedMemoryBytes:Float;
	/** Estimated memory used by images alone (bytes). */
	var imageMemoryBytes:Float;
	/** Estimated memory used by audio alone (bytes). */
	var audioMemoryBytes:Float;
	/** Cumulative evictions since cache was created. */
	var totalEvictions:Int;
	/** Whether the cache is enabled. */
	var enabled:Bool;
	/** Configured memory limit (0 = unlimited). */
	var maxMemoryBytes:Float;
}
