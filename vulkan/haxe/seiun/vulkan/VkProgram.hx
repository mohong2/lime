// Maps an OpenFL / flixel shader onto one native Vulkan program.

package seiun.vulkan;

#if seiun_vulkan

import cpp.UInt64;
import flixel.system.FlxAssets.FlxShader;
import openfl.display.Shader;

/**
 * One compiled program per distinct (vertex, fragment) source pair.
 *
 * The stock flixel quad shader is not compiled here: it only reproduces what the
 * built-in pipeline already draws, so those sprites stay on the cheap path and
 * keep batching. Everything else gets its own program, its own uniforms and its
 * own draw.
**/
class VkProgram
{

	/** Native handle; 0 means the shader could not be translated or compiled. */
	public var handle(default, null):UInt64 = 0;

	public var uniformCount(default, null):Int = 0;

	public var samplerCount(default, null):Int = 0;

	/** Native program counter, for the stats line. */
	public static var created(default, null):Int = 0;

	/** Shaders that fell back to the built-in pipeline. */
	public static var failed(default, null):Int = 0;

	// lime_vulkan_uniform_type, mirrored from lime_vulkan.h.
	static inline var FLOAT:Int = 1;
	static inline var INT:Int = 2;
	static inline var UINT:Int = 3;
	static inline var BOOL:Int = 4;
	static inline var VEC2:Int = 5;
	static inline var VEC3:Int = 6;
	static inline var VEC4:Int = 7;
	static inline var IVEC2:Int = 8;
	static inline var IVEC3:Int = 9;
	static inline var IVEC4:Int = 10;
	static inline var UVEC2:Int = 11;
	static inline var UVEC3:Int = 12;
	static inline var UVEC4:Int = 13;
	static inline var BVEC2:Int = 14;
	static inline var BVEC3:Int = 15;
	static inline var BVEC4:Int = 16;
	static inline var MAT2:Int = 17;
	static inline var MAT3:Int = 18;
	static inline var MAT4:Int = 19;

	// The projection matrix sits at offset 0 of every block and the native side
	// rewrites it on each draw, so the shader data never has to describe it.
	static inline var PROJECTION_NAME = "lime_vulkan_Projection";

	var uniforms:Array<VkUniform> = [];
	var samplers:Array<VkSampler> = [];

	// Resolved per shader instance. A sprite drawn every frame should not pay for
	// a source-string lookup each time.
	static var instances:Map<Shader, VkProgram> = new Map ();

	static var instanceCount:Int = 0;

	// Shared by every instance built from the same sources, so a mod that creates
	// one shader object per sprite still compiles once.
	static var bySource:Map<String, VkProgram> = new Map ();

	static var stockVertex:String = null;
	static var stockFragment:String = null;
	static var stockChecked:Bool = false;

	public function new () {}

	public inline function isValid():Bool
	{
		// Handle ids come from a counter that never reaches 2^32, so the low word
		// is enough to answer "is this a live program".
		return handle.toInt () != 0;
	}

	/**
	 * Returns the program for `shader`, or null when the shader is the stock one
	 * (or could not be built), in which case the caller keeps the built-in path.
	**/
	public static function get(shader:Shader):VkProgram
	{
		if (shader == null) return null;

		if (instances.exists (shader)) return instances.get (shader);

		var program = resolve (shader);

		// Instance keys are strong references, so the map is pruned before it grows
		// without bound across state changes. Programs themselves stay in bySource.
		if (instanceCount >= 512)
		{
			instances.clear ();
			instanceCount = 0;
		}

		instances.set (shader, program);
		instanceCount++;

		return program;
	}

	static function resolve(shader:Shader):VkProgram
	{
		var vertex = shader.glVertexSource;
		var fragment = shader.glFragmentSource;

		if (vertex == null || fragment == null) return null;

		if (isStock (vertex, fragment)) return null;

		var key = vertex.length + ":" + fragment.length + ":" + vertex + fragment;

		var known = bySource.get (key);

		if (known != null) return known;

		var program = create (vertex, fragment);

		bySource.set (key, program);

		return program;
	}

	/** True when the sources are exactly the ones every FlxGraphic starts with. */
	static function isStock(vertex:String, fragment:String):Bool
	{
		if (!stockChecked)
		{
			stockChecked = true;

			try
			{
				var reference:Shader = new FlxShader();

				stockVertex = reference.glVertexSource;
				stockFragment = reference.glFragmentSource;
			}
			catch (error:Dynamic)
			{
				log ("could not build the reference shader: " + Std.string (error));
			}
		}

		return stockVertex != null && vertex == stockVertex && fragment == stockFragment;
	}
	static function create(vertex:String, fragment:String):VkProgram
	{
		var program = new VkProgram ();

		program.handle = VkNative.programCreate (vertex, fragment, "logs/vulkan-shaders");

		if (program.handle.toInt () == 0)
		{
			failed++;

			log ("shader program failed, drawing with the built-in pipeline:\n" + trim (VkNative.programLog (0)));

			reportFailure (vertex, fragment, VkNative.programLog (0));

			return program;
		}

		program.reflect ();

		created++;

		log ("shader program #" + created + ": " + program.uniformCount + " uniform(s), "
			+ program.samplerCount + " sampler(s)");

		return program;
	}

	function reflect():Void
	{
		uniforms = [];
		samplers = [];

		var count = VkNative.programUniformCount (handle);

		for (i in 0...count)
		{
			var name = VkNative.programUniformName (handle, i);

			if (name == PROJECTION_NAME) continue;

			var uniform = new VkUniform ();

			uniform.name = name;
			uniform.type = VkNative.programUniformType (handle, i);
			uniform.offset = VkNative.programUniformOffset (handle, i);
			uniform.size = VkNative.programUniformSize (handle, i);
			uniform.length = VkNative.programUniformLength (handle, i);
			uniform.components = componentsOf (uniform.type);

			uniforms.push (uniform);
		}

		var total = VkNative.programSamplerCount (handle);

		for (i in 0...total)
		{
			var sampler = new VkSampler ();

			sampler.name = VkNative.programSamplerName (handle, i);

			// These two carry the sprite's own texture, which the batch already binds.
			sampler.primary = sampler.name == "bitmap" || sampler.name == "openfl_Texture";

			samplers.push (sampler);
		}

		uniformCount = uniforms.length;
		samplerCount = samplers.length;
	}

	static function componentsOf(type:Int):Int
	{
		return switch (type)
		{
			case FLOAT, INT, UINT, BOOL: 1;
			case VEC2, IVEC2, UVEC2, BVEC2: 2;
			case VEC3, IVEC3, UVEC3, BVEC3: 3;
			case VEC4, IVEC4, UVEC4, BVEC4: 4;
			case MAT2: 4;
			case MAT3: 9;
			case MAT4: 16;
			default: 0;
		}
	}

	/**
	 * Writes `openfl_TextureSize` for the texture a draw samples.
	 *
	 * The GL renderer fills this in on every bitmap bind, filter passes included,
	 * and shaders divide by it (`1.0 / openfl_TextureSize`). The Vulkan path binds
	 * its own textures, so every draw path calls this before `bind`.
	**/
	public static function applyTextureSize(shader:Shader, texture:UInt64):Void
	{
		if (shader == null || shader.data == null) return;

		var parameter:Dynamic = Reflect.field (shader.data, "openfl_TextureSize");

		if (parameter == null) return;

		var size:Array<Int> = [0, 0];

		if (!VkNative.textureSize (texture, size)) return;

		parameter.value = [size[0], size[1]];
	}

	/** Pushes this draw's parameter values and sampler bindings to the device. */
	public function bind(shader:Shader):Void
	{
		var data = shader.data;

		for (uniform in uniforms)
		{
			// Palette index textures are R8 here, so the lookup always reads the
			// red channel no matter which format the OpenGL probe picked.
			if (uniform.type == BOOL && uniform.name == "paletteUseAlpha")
			{
				intScratch[0] = 0;
				VkNative.programWriteInts (handle, uniform.offset, intScratch, 1);
				continue;
			}

			var parameter:Dynamic = Reflect.field (data, uniform.name);

			if (parameter == null) continue;

			upload (uniform, parameter);
		}

		for (i in 0...samplers.length)
		{
			var texture:UInt64 = 0;

			if (!samplers[i].primary)
			{
				var input:Dynamic = Reflect.field (data, samplers[i].name);

				if (input != null && input.input != null)
				{
					texture = VkTextures.getBitmapData (input.input);
				}
			}

			VkNative.programSetSamplerTexture (handle, i, texture);
		}
	}
	function upload(uniform:VkUniform, parameter:Dynamic):Void
	{
		var values:Array<Dynamic> = parameter.value;

		if (values == null || values.length == 0) return;

		if (uniform.length > 0)
		{
			var stride = Std.int (uniform.size / uniform.length);
			var components = uniform.components;

			if (components <= 0) return;

			var elements = Std.int (values.length / components);

			if (elements > uniform.length) elements = uniform.length;

			for (i in 0...elements)
			{
				write (uniform, values, i * components, uniform.offset + i * stride);
			}

			return;
		}

		write (uniform, values, 0, uniform.offset);
	}

	function write(uniform:VkUniform, values:Array<Dynamic>, from:Int, offset:Int):Void
	{
		switch (uniform.type)
		{
			// std140 pads every column of a small matrix out to 16 bytes, so the
			// columns go in one at a time instead of one contiguous copy.
			case MAT2:
				writeFloats (offset, values, from, 2);
				writeFloats (offset + 16, values, from + 2, 2);

			case MAT3:
				writeFloats (offset, values, from, 3);
				writeFloats (offset + 16, values, from + 3, 3);
				writeFloats (offset + 32, values, from + 6, 3);

			default:
				if (isFloat (uniform.type)) writeFloats (offset, values, from, uniform.components);
				else writeInts (offset, values, from, uniform.components);
		}
	}

	static function isFloat(type:Int):Bool
	{
		return type == FLOAT || type == VEC2 || type == VEC3 || type == VEC4
			|| type == MAT2 || type == MAT3 || type == MAT4;
	}

	function writeFloats(offset:Int, values:Array<Dynamic>, from:Int, count:Int):Void
	{
		if (count <= 0 || from + count > values.length) return;

		for (i in 0...count)
		{
			floatScratch[i] = (values[from + i] : Float);
		}

		VkNative.programWriteFloats (handle, offset, floatScratch, count);
	}

	function writeInts(offset:Int, values:Array<Dynamic>, from:Int, count:Int):Void
	{
		if (count <= 0 || from + count > values.length) return;

		for (i in 0...count)
		{
			var value:Dynamic = values[from + i];

			intScratch[i] = Std.isOfType (value, Bool) ? ((value : Bool) ? 1 : 0) : Std.int (value);
		}

		VkNative.programWriteInts (handle, offset, intScratch, count);
	}

	// One scratch pair for every upload: the native side copies out of it right away.
	static var floatScratch:Array<Float> = [
		0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
		0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0
	];

	static var intScratch:Array<Int> = [
		0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0
	];

	static function trim(text:String):String
	{
		if (text == null) return "(no log)";

		if (text.length > 1500) return text.substr (0, 1500) + "\n... (truncated)";

		return text;
	}

	static function log(message:String):Void
	{
		Sys.println ("[vulkan] " + message);
	}

	/** Failed translations written to logs/vulkan-shaders.txt before giving up. */
	static inline var MAX_REPORTS = 20;

	static var failureReports:Int = 0;

	/**
	 * Appends a failed translation to logs/vulkan-shaders.txt.
	 *
	 * A shader that does not compile falls back to the built-in pipeline, so the
	 * only symptom in game is a missing effect. The file keeps the translated
	 * error next to the sources it came from, which is what a bug report needs.
	**/
	static function reportFailure(vertex:String, fragment:String, logText:String):Void
	{
		if (failureReports >= MAX_REPORTS) return;

		failureReports++;

		try
		{
			if (!sys.FileSystem.exists ("logs")) sys.FileSystem.createDirectory ("logs");

			var output = sys.io.File.append ("logs/vulkan-shaders.txt", false);

			output.writeString ("=== failed shader " + failureReports + " of at most " + MAX_REPORTS + " ===\n"
				+ "-- error --\n" + clip (logText, 4000) + "\n"
				+ "-- vertex --\n" + clip (vertex, 4000) + "\n"
				+ "-- fragment --\n" + clip (fragment, 4000) + "\n\n");

			output.close ();
		}
		catch (error:Dynamic) {}
	}

	static function clip(text:String, limit:Int):String
	{
		if (text == null) return "(none)";

		if (text.length <= limit) return text;

		return text.substr (0, limit) + "\n... (" + (text.length - limit) + " more characters)";
	}


}

class VkUniform
{

	public var name:String;

	public var type:Int;

	public var offset:Int;

	public var size:Int;

	public var length:Int;

	public var components:Int;

	public function new () {}

}

class VkSampler
{

	public var name:String;

	/** Bound to the batch texture instead of a BitmapData of its own. */
	public var primary:Bool = false;

	public function new () {}

}

#end