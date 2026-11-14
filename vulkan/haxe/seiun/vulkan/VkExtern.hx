// Binding to the Vulkan renderer inside Lime.ndll.
//
// The native half is linked into the shared library, so there is no import
// library and no second dll to deploy: every entry point is an hxcpp primitive
// that is looked up by name once, on first use. Pointers and texture handles
// travel as doubles, the same way Lime hands native pointers to Haxe.

package seiun.vulkan;

import cpp.ConstCharStar;
import cpp.Float32;
import cpp.Pointer;
import cpp.UInt32;
import cpp.UInt64;

#if (seiun_vulkan && cpp && windows)

class VkExtern
{

	static var _tried:Bool = false;
	static var _bound:Bool = false;

	static var _version:Dynamic;
	static var _available:Dynamic;
	static var _probeText:Dynamic;
	static var _statusText:Dynamic;
	static var _batchStatsText:Dynamic;
	static var _selfWindow:Dynamic;
	static var _windowClientSize:Dynamic;
	static var _probeWindowFlat:Dynamic;
	static var _contextCreateFlat:Dynamic;
	static var _contextDestroy:Dynamic;
	static var _contextAttached:Dynamic;
	static var _releaseGL:Dynamic;
	static var _resize:Dynamic;
	static var _notifyFallback:Dynamic;
	static var _setDialogEnabled:Dynamic;

	static var _textureCreate:Dynamic;
	static var _textureUpdate:Dynamic;
	static var _textureDestroy:Dynamic;
	static var _textureRetire:Dynamic;
	static var _textureSize:Dynamic;

	static var _batchBeginRgba:Dynamic;
	static var _batchEnd:Dynamic;
	static var _batchSetTexture:Dynamic;
	static var _batchSetFilter:Dynamic;
	static var _batchSetBlend:Dynamic;
	static var _batchSetScissor:Dynamic;
	static var _batchSetView:Dynamic;
	static var _batchQuad:Dynamic;
	static var _batchQuadsD:Dynamic;
	static var _batchSetProgram:Dynamic;

	static var _targetCreate:Dynamic;
	static var _targetValid:Dynamic;
	static var _batchBeginTarget:Dynamic;
	static var _batchEndTarget:Dynamic;

	static var _programCreateFlat:Dynamic;
	static var _programDestroy:Dynamic;
	static var _programValid:Dynamic;
	static var _programLog:Dynamic;
	static var _programUniformCount:Dynamic;
	static var _programUniformName:Dynamic;
	static var _programUniformType:Dynamic;
	static var _programUniformOffset:Dynamic;
	static var _programUniformSize:Dynamic;
	static var _programUniformLength:Dynamic;
	static var _programSamplerCount:Dynamic;
	static var _programSamplerName:Dynamic;
	static var _programSetSamplerTexture:Dynamic;
	static var _programWriteFloats:Dynamic;
	static var _programWriteInts:Dynamic;

	/** Whether the shared library exports the renderer. False when it was built with -Dlime-vulkan-off. */
	public static function ready():Bool
	{
		return bind ();
	}

	static function bind():Bool
	{
		if (_tried) return _bound;

		_tried = true;

		try
		{
			_version = load ("lime_vulkan_version_hx", 0);
			_available = load ("lime_vulkan_available_hx", 0);
			_probeText = load ("lime_vulkan_probe_text_hx", 0);
			_statusText = load ("lime_vulkan_status_text_hx", 0);
			_batchStatsText = load ("lime_vulkan_batch_stats_text_hx", 0);
			_selfWindow = load ("lime_vulkan_self_window_hx", 0);
			_windowClientSize = load ("lime_vulkan_window_client_size_hx", 3);
			_probeWindowFlat = load ("lime_vulkan_probe_window_flat_hx", 8);
			_contextCreateFlat = load ("lime_vulkan_context_create_flat_hx", 8);
			_contextDestroy = load ("lime_vulkan_context_destroy_hx", 0);
			_contextAttached = load ("lime_vulkan_context_attached_hx", 0);
			_releaseGL = load ("lime_vulkan_release_gl_hx", 0);
			_resize = load ("lime_vulkan_resize_hx", 2);
			_notifyFallback = load ("lime_vulkan_notify_fallback_hx", 1);
			_setDialogEnabled = load ("lime_vulkan_set_dialog_enabled_hx", 1);

			_textureCreate = load ("lime_vulkan_texture_create_hx", 4);
			_textureUpdate = load ("lime_vulkan_texture_update_hx", 3);
			_textureDestroy = load ("lime_vulkan_texture_destroy_hx", 1);
			_textureRetire = load ("lime_vulkan_texture_retire_hx", 1);
			_textureSize = load ("lime_vulkan_texture_size_hx", 3);

			_batchBeginRgba = load ("lime_vulkan_batch_begin_rgba_hx", 3);
			_batchEnd = load ("lime_vulkan_batch_end_hx", 0);
			_batchSetTexture = load ("lime_vulkan_batch_set_texture_hx", 1);
			_batchSetFilter = load ("lime_vulkan_batch_set_filter_hx", 1);
			_batchSetBlend = load ("lime_vulkan_batch_set_blend_hx", 1);
			_batchSetScissor = load ("lime_vulkan_batch_set_scissor_hx", 4);
			_batchSetView = load ("lime_vulkan_batch_set_view_hx", 1);
			_batchQuad = load ("lime_vulkan_batch_quad_hx", 9);
			_batchQuadsD = load ("lime_vulkan_batch_quads_d_hx", 7);
			_batchSetProgram = load ("lime_vulkan_batch_set_program_hx", 1);

			_targetCreate = load ("lime_vulkan_target_create_hx", 2);
			_targetValid = load ("lime_vulkan_target_valid_hx", 1);
			_batchBeginTarget = load ("lime_vulkan_batch_begin_target_hx", 2);
			_batchEndTarget = load ("lime_vulkan_batch_end_target_hx", 0);

			_programCreateFlat = load ("lime_vulkan_program_create_flat_hx", 4);
			_programDestroy = load ("lime_vulkan_program_destroy_hx", 1);
			_programValid = load ("lime_vulkan_program_valid_hx", 1);
			_programLog = load ("lime_vulkan_program_log_hx", 1);
			_programUniformCount = load ("lime_vulkan_program_uniform_count_hx", 1);
			_programUniformName = load ("lime_vulkan_program_uniform_name_hx", 2);
			_programUniformType = load ("lime_vulkan_program_uniform_type_hx", 2);
			_programUniformOffset = load ("lime_vulkan_program_uniform_offset_hx", 2);
			_programUniformSize = load ("lime_vulkan_program_uniform_size_hx", 2);
			_programUniformLength = load ("lime_vulkan_program_uniform_length_hx", 2);
			_programSamplerCount = load ("lime_vulkan_program_sampler_count_hx", 1);
			_programSamplerName = load ("lime_vulkan_program_sampler_name_hx", 2);
			_programSetSamplerTexture = load ("lime_vulkan_program_set_sampler_texture_hx", 3);
			_programWriteFloats = load ("lime_vulkan_program_write_floats_hx", 4);
			_programWriteInts = load ("lime_vulkan_program_write_ints_hx", 4);

			_bound = true;
		}
		catch (error:Dynamic)
		{
			_bound = false;
		}

		return _bound;
	}

	static function load(name:String, args:Int):Dynamic
	{
		var entry:Dynamic = null;

		if (lime.system.CFFI.enabled)
		{
			try entry = lime.system.CFFI.load ("lime", name, args) catch (error:Dynamic) entry = null;
		}

		if (entry == null) entry = cpp.Lib.load ("lime", name, args);

		return entry;
	}

	// Pointers and handles cross the boundary as doubles.

	static inline function ptr<T>(value:Pointer<T>):Float
	{
		return untyped __cpp__ ("(double)(intptr_t)((void*)({0}))", value);
	}

	static inline function nullPointer():Float
	{
		return 0.0;
	}

	static inline function window(value:Float):Pointer<cpp.Void>
	{
		return untyped __cpp__ ("(void*)(intptr_t)({0})", value);
	}

	static inline function handle(value:UInt64):Float
	{
		return untyped __cpp__ ("(double)({0})", value);
	}

	static inline function fromHandle(value:Float):UInt64
	{
		return untyped __cpp__ ("(uint64_t)({0})", value);
	}

	static inline function signed(value:UInt32):Int
	{
		return untyped __cpp__ ("(int)({0})", value);
	}

	public static function version():String
	{
		return bind () ? _version () : "unavailable";
	}

	public static function available():Int
	{
		return bind () ? _available () : 0;
	}

	public static function probeText():String
	{
		var text:String = bind () ? _probeText () : "";

		return text == null ? "" : text;
	}

	public static function statusText():String
	{
		var text:String = bind () ? _statusText () : "";

		return text == null ? "" : text;
	}

	public static function batchStatsText():String
	{
		var text:String = bind () ? _batchStatsText () : "";

		return text == null ? "" : text;
	}

	public static function selfWindow():Pointer<cpp.Void>
	{
		if (!bind ()) return null;

		var address:Float = _selfWindow ();

		return address == 0 ? null : window (address);
	}

	public static function windowClientSize(target:Pointer<cpp.Void>, width:Pointer<Int>, height:Pointer<Int>):Int
	{
		if (!bind ()) return 0;

		return _windowClientSize (ptr (target), ptr (width), ptr (height));
	}

	public static function probeWindowFlat(target:Pointer<cpp.Void>, width:Int, height:Int,
		deviceIndex:Int, presentMode:Int, enableValidation:Int, verbose:Int, fallbackDialog:Int):Int
	{
		if (!bind ()) return 0;

		return _probeWindowFlat (ptr (target), width, height, deviceIndex, presentMode,
			enableValidation, verbose, fallbackDialog);
	}

	public static function contextCreateFlat(target:Pointer<cpp.Void>, width:Int, height:Int,
		deviceIndex:Int, presentMode:Int, enableValidation:Int, verbose:Int, fallbackDialog:Int):Int
	{
		if (!bind ()) return 0;

		return _contextCreateFlat (ptr (target), width, height, deviceIndex, presentMode,
			enableValidation, verbose, fallbackDialog);
	}

	public static function contextDestroy():Void
	{
		if (bind ()) _contextDestroy ();
	}

	public static function contextAttached():Int
	{
		return bind () ? _contextAttached () : 0;
	}

	public static function releaseGL():Int
	{
		return bind () ? _releaseGL () : 1;
	}

	public static function resize(width:Int, height:Int):Int
	{
		return bind () ? _resize (width, height) : 0;
	}

	public static function notifyFallback(reason:ConstCharStar):Void
	{
		if (!bind ()) return;

		_notifyFallback (reason == null ? null : reason.toString ());
	}

	public static function setDialogEnabled(enabled:Int):Void
	{
		if (bind ()) _setDialogEnabled (enabled);
	}

	public static function textureCreate(rows:Pointer<cpp.Void>, width:Int, height:Int, flags:Int):UInt64
	{
		if (!bind ()) return 0;

		return fromHandle (_textureCreate (ptr (rows), width, height, flags));
	}

	/** Allocates an empty texture of that size, no pixel upload. */
	public static function textureCreateEmpty(width:Int, height:Int, flags:Int):UInt64
	{
		if (!bind ()) return 0;

		return fromHandle (_textureCreate (nullPointer (), width, height, flags));
	}

	public static function textureUpdate(texture:UInt64, rows:Pointer<cpp.Void>, flags:Int):Int
	{
		if (!bind ()) return 0;

		return _textureUpdate (handle (texture), ptr (rows), flags);
	}

	public static function textureDestroy(texture:UInt64):Void
	{
		if (bind ()) _textureDestroy (handle (texture));
	}

	public static function textureRetire(texture:UInt64):Void
	{
		if (bind ()) _textureRetire (handle (texture));
	}

	public static function textureSize(texture:UInt64, width:Pointer<Int>, height:Pointer<Int>):Int
	{
		if (!bind ()) return 0;

		return _textureSize (handle (texture), ptr (width), ptr (height));
	}

	public static function batchBeginRgba(screenWidth:Int, screenHeight:Int, argb:UInt32):Int
	{
		if (!bind ()) return 0;

		return _batchBeginRgba (screenWidth, screenHeight, signed (argb));
	}

	public static function batchEnd():Int
	{
		return bind () ? _batchEnd () : 0;
	}

	public static function batchSetTexture(texture:UInt64):Void
	{
		if (bind ()) _batchSetTexture (handle (texture));
	}

	public static function batchSetFilter(nearest:Int):Void
	{
		if (bind ()) _batchSetFilter (nearest);
	}

	public static function batchSetBlend(blend:Int):Void
	{
		if (bind ()) _batchSetBlend (blend);
	}

	public static function batchSetScissor(x:Int, y:Int, width:Int, height:Int):Void
	{
		if (bind ()) _batchSetScissor (x, y, width, height);
	}

	public static function batchSetView(matrix2x3:Pointer<Float>):Void
	{
		if (bind ()) _batchSetView (ptr (matrix2x3));
	}

	/** Resets the camera matrix to the identity. */
	public static function batchResetView():Void
	{
		if (bind ()) _batchSetView (nullPointer ());
	}

	public static function batchQuad(x:Float, y:Float, width:Float, height:Float,
		u0:Float, v0:Float, u1:Float, v1:Float, color:UInt32):Int
	{
		if (!bind ()) return 0;

		return _batchQuad (x, y, width, height, u0, v0, u1, v1, signed (color));
	}

	public static function batchQuadsD(rects:Pointer<Float>, transforms:Pointer<Float>, quadCount:Int,
		alphas:Pointer<Float>, uniformAlpha:Float32,
		colorMultipliers:Pointer<Float>, colorOffsets:Pointer<Float>):Int
	{
		if (!bind ()) return 0;

		return _batchQuadsD (ptr (rects), ptr (transforms), quadCount, ptr (alphas), uniformAlpha,
			ptr (colorMultipliers), ptr (colorOffsets));
	}

	public static function batchSetProgram(program:UInt64):Void
	{
		if (bind ()) _batchSetProgram (handle (program));
	}

	public static function targetCreate(width:Int, height:Int):UInt64
	{
		if (!bind ()) return 0;

		return fromHandle (_targetCreate (width, height));
	}

	public static function targetValid(target:UInt64):Int
	{
		return bind () ? _targetValid (handle (target)) : 0;
	}

	public static function batchBeginTarget(target:UInt64, argb:UInt32):Int
	{
		if (!bind ()) return 0;

		return _batchBeginTarget (handle (target), signed (argb));
	}

	public static function batchEndTarget():Int
	{
		return bind () ? _batchEndTarget () : 0;
	}

	public static function programCreateFlat(vertexSource:ConstCharStar, fragmentSource:ConstCharStar,
		flags:UInt32, cacheDirectory:ConstCharStar):UInt64
	{
		if (!bind ()) return 0;

		return fromHandle (_programCreateFlat (vertexSource == null ? null : vertexSource.toString (),
			fragmentSource == null ? null : fragmentSource.toString (), signed (flags),
			cacheDirectory == null ? null : cacheDirectory.toString ()));
	}

	public static function programDestroy(program:UInt64):Void
	{
		if (bind ()) _programDestroy (handle (program));
	}

	public static function programValid(program:UInt64):Int
	{
		return bind () ? _programValid (handle (program)) : 0;
	}

	public static function programLog(program:UInt64):String
	{
		if (!bind ()) return "";

		var text:String = _programLog (handle (program));

		return text == null ? "" : text;
	}

	public static function programUniformCount(program:UInt64):Int
	{
		return bind () ? _programUniformCount (handle (program)) : 0;
	}

	public static function programUniformName(program:UInt64, index:Int):String
	{
		if (!bind ()) return "";

		var name:String = _programUniformName (handle (program), index);

		return name == null ? "" : name;
	}

	public static function programUniformType(program:UInt64, index:Int):Int
	{
		return bind () ? _programUniformType (handle (program), index) : 0;
	}

	public static function programUniformOffset(program:UInt64, index:Int):UInt32
	{
		if (!bind ()) return 0;

		return _programUniformOffset (handle (program), index);
	}

	public static function programUniformSize(program:UInt64, index:Int):UInt32
	{
		if (!bind ()) return 0;

		return _programUniformSize (handle (program), index);
	}

	public static function programUniformLength(program:UInt64, index:Int):Int
	{
		return bind () ? _programUniformLength (handle (program), index) : 0;
	}

	public static function programSamplerCount(program:UInt64):Int
	{
		return bind () ? _programSamplerCount (handle (program)) : 0;
	}

	public static function programSamplerName(program:UInt64, index:Int):String
	{
		if (!bind ()) return "";

		var name:String = _programSamplerName (handle (program), index);

		return name == null ? "" : name;
	}

	public static function programSetSamplerTexture(program:UInt64, index:Int, texture:UInt64):Int
	{
		if (!bind ()) return 0;

		return _programSetSamplerTexture (handle (program), index, handle (texture));
	}

	public static function programWriteFloats(program:UInt64, offset:UInt32, values:Pointer<Float>, count:Int):Int
	{
		if (!bind ()) return 0;

		return _programWriteFloats (handle (program), signed (offset), ptr (values), count);
	}

	public static function programWriteInts(program:UInt64, offset:UInt32, values:Pointer<Int>, count:Int):Int
	{
		if (!bind ()) return 0;

		return _programWriteInts (handle (program), signed (offset), ptr (values), count);
	}
}

#else

class VkExtern
{
	public static function ready():Bool return false;
}

#end
