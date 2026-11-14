// Render targets: textures the batch can draw into and then sample.
//
// A camera filter chain needs two of them per camera size, and cameras keep
// their size for the whole session, so the cache hands the same pair back
// every frame instead of allocating per frame.

package seiun.vulkan;

#if seiun_vulkan

import cpp.UInt64;

class VkRenderTargets
{

	/** Two slots per size: a filter chain ping-pongs between them. */
	public static inline var SLOTS:Int = 2;

	/**
	 * Hard cap on cached targets (eight camera sizes with both slots).
	 *
	 * Cameras keep their size for a session, so this never triggers in practice.
	 * It exists because the alternative failure is unbounded video memory: a mod
	 * that animates a camera's size would otherwise add a fullscreen target pair
	 * every frame.
	 */
	public static inline var MAX_TARGETS:Int = 16;

	static var targets:Map<String, UInt64> = new Map ();

	/** Entries in "targets"; kept by hand because Map has no size on every target. */
	static var count:Int = 0;

	/** Targets created this session, for the stats line. */
	public static var created(default, null):Int = 0;

	public static function get(width:Int, height:Int, slot:Int):UInt64
	{
		if (width <= 0 || height <= 0) return 0;
		if (slot < 0 || slot >= SLOTS) return 0;

		var key = width + "x" + height + "#" + slot;
		var handle = targets.get (key);

		if (handle != null && VkNative.targetValid (handle)) return handle;

		handle = VkNative.targetCreate (width, height);

		if (!VkNative.valid (handle))
		{
			log ("could not create a " + width + "x" + height + " render target");
			return 0;
		}

		created++;
		count++;
		targets.set (key, handle);

		return handle;
	}

	/**
	 * Recycles the cache when it outgrew the cap.
	 *
	 * Only called at frame start: a target retired in the middle of a frame would
	 * still be bound by the command buffer that is being recorded.
	 */
	public static function trim():Void
	{
		if (count >= MAX_TARGETS) dispose ();
	}

	/**
	 * Drops every cached target. A window resize rebuilds the swapchain, and a
	 * target that was created for the old format must not be reused.
	 */
	public static function dispose():Void
	{
		for (handle in targets)
		{
			VkNative.textureRetire (handle);
		}

		targets.clear ();
		count = 0;
	}

	static function log(message:String):Void
	{
		Sys.println ("[vulkan] " + message);
	}
}

#end
