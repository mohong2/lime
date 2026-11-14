// Binding layer. cpp pointers, arrays and bytes cross the boundary here.

package seiun.vulkan;

import cpp.Pointer;

#if (seiun_vulkan && cpp && (windows || linux || android))

class VkNative
{

	/** Whether the shared library exports the renderer (false with -Dlime-vulkan-off). */
	public static function ready():Bool
	{
		return VkExtern.ready ();
	}

	public static function version():String
	{
		return VkExtern.version ().toString ();
	}

	public static function available():Bool
	{
		return VkExtern.available () != 0;
	}

	public static function probeText():String
	{
		return VkExtern.probeText ().toString ();
	}

	public static function statusText():String
	{
		return VkExtern.statusText ().toString ();
	}

	public static function batchStatsText():String
	{
		return VkExtern.batchStatsText ().toString ();
	}

	public static function selfWindow():Pointer<cpp.Void>
	{
		return VkExtern.selfWindow ();
	}

	public static function windowClientSize(window:Pointer<cpp.Void>, size:Array<Int>):Bool
	{
		return VkExtern.windowClientSize (window, cpp.NativeArray.address (size, 0), cpp.NativeArray.address (size, 1)) != 0;
	}

	public static function probeWindow(window:Pointer<cpp.Void>, verbose:Bool, validation:Bool):Bool
	{
		return VkExtern.probeWindowFlat (window, 0, 0, -1, 0, validation ? 1 : 0, verbose ? 1 : 0, 1) != 0;
	}

	// 0 = auto (uncapped), 3 = vsync. Passed through to the present mode picker.
	public static function contextCreate(window:Pointer<cpp.Void>, verbose:Bool, validation:Bool):Bool
	{
		var presentMode = 0;

		for (arg in Sys.args ())
		{
			if (arg == "--vulkan-vsync") presentMode = 3;
		}

		return VkExtern.contextCreateFlat (window, 0, 0, -1, presentMode, validation ? 1 : 0, verbose ? 1 : 0, 1) != 0;
	}

	public static function contextDestroy():Void
	{
		VkExtern.contextDestroy ();
	}

	public static function attached():Bool
	{
		return VkExtern.contextAttached () != 0;
	}

	// 0 = released, 1 = nothing was current, 2/3 = the driver refused.
	public static function releaseGL():Int
	{
		return VkExtern.releaseGL ();
	}

	public static function resize(width:Int, height:Int):Void
	{
		VkExtern.resize (width, height);
	}

	public static function notifyFallback(reason:String):Void
	{
		VkExtern.notifyFallback (cpp.ConstCharStar.fromString (reason));
	}

	public static function textureCreate(pixels:Pointer<cpp.Void>, width:Int, height:Int, flags:Int):cpp.UInt64
	{
		return VkExtern.textureCreate (pixels, width, height, flags);
	}

	public static function textureSize(texture:cpp.UInt64, size:Array<Int>):Bool
	{
		return VkExtern.textureSize (texture, cpp.NativeArray.address (size, 0), cpp.NativeArray.address (size, 1)) != 0;
	}

	// Overwrites an existing texture when the size did not change, so a redrawn
	// text or overlay costs a copy instead of a new image and a new allocation.
	public static function textureUpdate(texture:cpp.UInt64, bytes:haxe.io.Bytes, flags:Int):Bool
	{
		if (bytes == null) return false;

		var pointer:Pointer<cpp.UInt8> = @:privateAccess cpp.NativeArray.address (bytes.b, 0);

		return VkExtern.textureUpdate (texture, cast pointer, flags) != 0;
	}

	public static function textureDestroy(texture:cpp.UInt64):Void
	{
		if (valid (texture)) VkExtern.textureDestroy (texture);
	}

	public static function textureRetire(texture:cpp.UInt64):Void
	{
		if (valid (texture)) VkExtern.textureRetire (texture);
	}

	public static function batchBegin(clearArgb:Int):Int
	{

		return VkExtern.batchBeginRgba (0, 0, (clearArgb : cpp.UInt32));
	}

	public static function batchEnd():Bool
	{
		return VkExtern.batchEnd () != 0;
	}

	public static function setTexture(texture:cpp.UInt64):Void
	{
		VkExtern.batchSetTexture (texture);
	}

	/** Nearest sampling when `nearest` is true, linear otherwise. */
	public static function setFilter(nearest:Bool):Void
	{
		VkExtern.batchSetFilter (nearest ? 1 : 0);
	}

	public static function setBlend(blend:Int):Void
	{
		VkExtern.batchSetBlend (blend);
	}

	public static function setScissor(x:Int, y:Int, width:Int, height:Int):Void
	{
		VkExtern.batchSetScissor (x, y, width, height);
	}

	public static function setProgram(program:cpp.UInt64):Void
	{
		VkExtern.batchSetProgram (program);
	}

	/** An offscreen colour target; the handle is also a sampleable texture. */
	public static function targetCreate(width:Int, height:Int):cpp.UInt64
	{
		return VkExtern.targetCreate (width, height);
	}

	public static function targetValid(target:cpp.UInt64):Bool
	{
		return VkExtern.targetValid (target) != 0;
	}

	/**
	 * Points the batch at the target; the window pass is closed first.
	 * `argb` is the colour the target is cleared to.
	 */
	public static function targetBegin(target:cpp.UInt64, argb:Int):Bool
	{
		return VkExtern.batchBeginTarget (target, (argb : cpp.UInt32)) != 0;
	}

	/** Ends the target pass; the next draw reopens the window pass. */
	public static function targetEnd():Bool
	{
		return VkExtern.batchEndTarget () != 0;
	}

	public static function programCreate(vertexSource:String, fragmentSource:String, cacheDirectory:String = null):cpp.UInt64
	{
		return VkExtern.programCreateFlat (cpp.ConstCharStar.fromString (vertexSource),
			cpp.ConstCharStar.fromString (fragmentSource), 0,
			cacheDirectory == null ? null : cpp.ConstCharStar.fromString (cacheDirectory));
	}

	public static function programValid(program:cpp.UInt64):Bool
	{
		return VkExtern.programValid (program) != 0;
	}

	/** Translation and compile report. A zero handle returns the last failure instead. */
	public static function programLog(program:cpp.UInt64):String
	{
		var text = VkExtern.programLog (program);

		return text == null ? "" : text.toString ();
	}

	public static function programUniformCount(program:cpp.UInt64):Int
	{
		return VkExtern.programUniformCount (program);
	}

	public static function programUniformName(program:cpp.UInt64, index:Int):String
	{
		var name = VkExtern.programUniformName (program, index);

		return name == null ? "" : name.toString ();
	}

	public static function programUniformType(program:cpp.UInt64, index:Int):Int
	{
		return VkExtern.programUniformType (program, index);
	}

	public static function programUniformOffset(program:cpp.UInt64, index:Int):Int
	{
		return (VkExtern.programUniformOffset (program, index) : Int);
	}

	public static function programUniformSize(program:cpp.UInt64, index:Int):Int
	{
		return (VkExtern.programUniformSize (program, index) : Int);
	}

	public static function programUniformLength(program:cpp.UInt64, index:Int):Int
	{
		return VkExtern.programUniformLength (program, index);
	}

	public static function programSamplerCount(program:cpp.UInt64):Int
	{
		return VkExtern.programSamplerCount (program);
	}

	public static function programSamplerName(program:cpp.UInt64, index:Int):String
	{
		var name = VkExtern.programSamplerName (program, index);

		return name == null ? "" : name.toString ();
	}

	public static function programSetSamplerTexture(program:cpp.UInt64, index:Int, texture:cpp.UInt64):Void
	{
		VkExtern.programSetSamplerTexture (program, index, texture);
	}

	public static function programWriteFloats(program:cpp.UInt64, offset:Int, values:Array<Float>, count:Int):Void
	{
		if (count <= 0) return;

		VkExtern.programWriteFloats (program, (offset : cpp.UInt32), cpp.NativeArray.address (values, 0), count);
	}

	public static function programWriteInts(program:cpp.UInt64, offset:Int, values:Array<Int>, count:Int):Void
	{
		if (count <= 0) return;

		VkExtern.programWriteInts (program, (offset : cpp.UInt32), cpp.NativeArray.address (values, 0), count);
	}

	public static function setView(view:Array<Float>):Void
	{
		if (view == null)
		{
			VkExtern.batchResetView ();
			return;
		}

		VkExtern.batchSetView (cpp.NativeArray.address (view, 0));
	}

	public static function quad(x:Float, y:Float, width:Float, height:Float,
		u0:Float, v0:Float, u1:Float, v1:Float, argb:Int):Bool
	{
		return VkExtern.batchQuad (x, y, width, height, u0, v0, u1, v1, (argb : cpp.UInt32)) != 0;
	}

	public static function quads(rects:Array<Float>, transforms:Array<Float>, quadCount:Int,
		alphas:Array<Float>, uniformAlpha:Float,
		colorMultipliers:Array<Float>, colorOffsets:Array<Float>):Bool
	{
		return VkExtern.batchQuadsD (
			cpp.NativeArray.address (rects, 0),
			cpp.NativeArray.address (transforms, 0),
			quadCount,
			alphas == null ? null : cpp.NativeArray.address (alphas, 0),
			(uniformAlpha : cpp.Float32),
			colorMultipliers == null ? null : cpp.NativeArray.address (colorMultipliers, 0),
			colorOffsets == null ? null : cpp.NativeArray.address (colorOffsets, 0)) != 0;
	}

	public static inline var TEXTURE_BGRA:Int = 8;
	public static inline var TEXTURE_UNPREMULTIPLY:Int = 16;
	public static inline var TEXTURE_INDEX8:Int = 32;

	public static function textureCreateFromBytes(bytes:haxe.io.Bytes, width:Int, height:Int, flags:Int):cpp.UInt64
	{
		if (bytes == null) return VkExtern.textureCreateEmpty (width, height, flags);

		var pointer:Pointer<cpp.UInt8> = @:privateAccess cpp.NativeArray.address (bytes.b, 0);

		return VkExtern.textureCreate (cast pointer, width, height, flags);
	}

	static inline var NO_HANDLE:cpp.UInt64 = 0;

	public static inline function valid(handle:cpp.UInt64):Bool
	{
		return handle != NO_HANDLE;
	}
}

#else

class VkNative
{
	public static function ready():Bool return false;
	public static function version():String return "unavailable";
	public static function available():Bool return false;
	public static function probeText():String return "";
	public static function statusText():String return "";
	public static function batchStatsText():String return "";

	public static function selfWindow():Dynamic return null;
	public static function windowClientSize(window:Dynamic, size:Array<Int>):Bool return false;

	public static function probeWindow(window:Dynamic, verbose:Bool, validation:Bool):Bool return false;
	public static function contextCreate(window:Dynamic, verbose:Bool, validation:Bool):Bool return false;
	public static function contextDestroy():Void {}
	public static function attached():Bool return false;
	public static function resize(width:Int, height:Int):Void {}
	public static function notifyFallback(reason:String):Void {}

	public static function textureCreate(pixels:Dynamic, width:Int, height:Int, flags:Int):Dynamic return 0;
	public static function textureCreateFromBytes(bytes:Dynamic, width:Int, height:Int, flags:Int):Dynamic return 0;
	public static function textureUpdate(texture:Dynamic, bytes:Dynamic, flags:Int):Bool return false;
	public static function textureDestroy(texture:Dynamic):Void {}
	public static function textureRetire(texture:Dynamic):Void {}
	public static function textureSize(texture:Dynamic, size:Array<Int>):Bool return false;

	public static inline var TEXTURE_BGRA:Int = 8;
	public static inline var TEXTURE_UNPREMULTIPLY:Int = 16;
	public static inline var TEXTURE_INDEX8:Int = 32;

	public static function batchBegin(clearArgb:Int):Int return 0;
	public static function batchEnd():Bool return false;
	public static function setTexture(texture:Dynamic):Void {}
	public static function setFilter(nearest:Bool):Void {}
	public static function setBlend(blend:Int):Void {}
	public static function setScissor(x:Int, y:Int, width:Int, height:Int):Void {}
	public static function setProgram(program:Dynamic):Void {}
	public static function targetCreate(width:Int, height:Int):Dynamic return 0;
	public static function targetValid(target:Dynamic):Bool return false;
	public static function targetBegin(target:Dynamic, argb:Int):Bool return false;
	public static function targetEnd():Bool return false;

	public static function programCreate(vertexSource:String, fragmentSource:String, cacheDirectory:String = null):Dynamic return 0;
	public static function programValid(program:Dynamic):Bool return false;
	public static function programLog(program:Dynamic):String return "";
	public static function programUniformCount(program:Dynamic):Int return 0;
	public static function programUniformName(program:Dynamic, index:Int):String return "";
	public static function programUniformType(program:Dynamic, index:Int):Int return 0;
	public static function programUniformOffset(program:Dynamic, index:Int):Int return 0;
	public static function programUniformSize(program:Dynamic, index:Int):Int return 0;
	public static function programUniformLength(program:Dynamic, index:Int):Int return 0;
	public static function programSamplerCount(program:Dynamic):Int return 0;
	public static function programSamplerName(program:Dynamic, index:Int):String return "";
	public static function programSetSamplerTexture(program:Dynamic, index:Int, texture:Dynamic):Void {}
	public static function programWriteFloats(program:Dynamic, offset:Int, values:Array<Float>, count:Int):Void {}
	public static function programWriteInts(program:Dynamic, offset:Int, values:Array<Int>, count:Int):Void {}
	public static function setView(view:Array<Float>):Void {}
	public static function quads(rects:Array<Float>, transforms:Array<Float>, quadCount:Int,
		alphas:Array<Float>, uniformAlpha:Float,
		colorMultipliers:Array<Float>, colorOffsets:Array<Float>):Bool return false;
	public static function quad(x:Float, y:Float, width:Float, height:Float,
		u0:Float, v0:Float, u1:Float, v1:Float, argb:Int):Bool return false;

	public static inline function valid(handle:Dynamic):Bool return false;
}

#end
