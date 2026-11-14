#ifndef LIME_VULKAN_GLSL_H
#define LIME_VULKAN_GLSL_H

// GLSL ES 1.00 -> GLSL 450 translator with reflection.
//
// Why glslang is not pointed at GLSL ES 1.00 directly:
//   1. Vulkan only takes SPIR-V, and glslang refuses to emit a Vulkan target
//      below #version 140;
//   2. Vulkan has no plain uniforms (a default uniform block is required) and
//      ES 1.00 is nothing but plain uniforms;
//   3. attribute / varying / texture2D / gl_FragColor do not exist in 450.
//
// The target is desktop 450 rather than 310 es: the game runs on a desktop, and
// the shaders as written (uniform initialisers, float times int) are things
// desktop GLSL 1.20 allows and ES 3.10 rejects. Translating to 450 means no
// explicit casts have to be added to every expression.
// All three are mechanical transforms, and doing them here is more controllable
// than pulling in a third party converter: a failure points at one line.
//
// The shape of the output (both stages share one uniform block so the offsets
// line up):
//
//   #version 450
//   precision highp float; ...
//   layout(std140, set = 0, binding = 0) uniform lime_vulkan_Uniforms { mat4 lime_vulkan_Projection; ... };
//   layout(location = 0) in vec2 lime_vulkan_Position;           // vertex input
//   layout(location = 0) out vec2 openfl_TextureCoordv;  // varying, same location in both stages
//   layout(set = 0, binding = 1) uniform sampler2D bitmap;
//   layout(location = 0) out vec4 lime_vulkan_FragColor;         // fragment output
//   <body>

#include <cstdint>
#include <string>
#include <vector>

namespace vulkan {
namespace glsl {

	enum Type {
		TYPE_UNKNOWN = 0,
		TYPE_FLOAT,
		TYPE_INT,
		TYPE_UINT,
		TYPE_BOOL,
		TYPE_VEC2,
		TYPE_VEC3,
		TYPE_VEC4,
		TYPE_IVEC2,
		TYPE_IVEC3,
		TYPE_IVEC4,
		TYPE_UVEC2,
		TYPE_UVEC3,
		TYPE_UVEC4,
		TYPE_BVEC2,
		TYPE_BVEC3,
		TYPE_BVEC4,
		TYPE_MAT2,
		TYPE_MAT3,
		TYPE_MAT4,
		TYPE_SAMPLER2D,
		TYPE_SAMPLER_CUBE
	};

	/** One uniform, attribute, varying or sampler. offset only means something for uniforms. */
	struct Variable {

		std::string name;
		Type type;
		int arraySize;   /**< 0 when not an array */
		uint32_t offset; /**< std140 offset */
		uint32_t size;   /**< bytes used (the whole array for an array) */
		uint32_t stride; /**< array element stride, 0 when not an array */
		int location;    /**< attribute or varying location, or the sampler binding */

		Variable ();

	};

	struct Options {

		/** Enables the OpenFL / flixel compatibility layer: maps attributes such as
		 *  openfl_Position onto this backend's single vertex stream (pos vec2 /
		 *  uv vec2 / color vec4) and injects the macros flixel expects. */
		bool openfl;

		/** Injects the default precision. With it off the source has to declare
		 *  everything itself, and an ES 3.10 fragment shader has no default float
		 *  precision, so turning it off usually fails to compile. */
		bool injectPrecision;

		Options ();

	};

	struct Translated {

		bool ok;

		std::string vertex;
		std::string fragment;

		std::vector<Variable> uniforms;   /**< includes lime_vulkan_Projection, sorted by offset */
		std::vector<Variable> attributes; /**< vertex inputs */
		std::vector<Variable> varyings;   /**< declared in both stages, same location */
		std::vector<Variable> samplers;   /**< location is the binding number */

		uint32_t uniformSize; /**< total uniform block size, 16 byte aligned */
		std::string log;

		Translated ();

	};

	/** Translates both stages together: the uniform block and the varying locations have to match. */
	bool Translate (const char* vertexSource, int vertexLength,
		const char* fragmentSource, int fragmentLength,
		const Options& options, Translated* out);

	const char* TypeName (Type type);

	/** Bytes taken under std140 (mat2 is 32, not 16). */
	uint32_t TypeSize (Type type);

	/** Alignment required under std140. */
	uint32_t TypeAlignment (Type type);

	bool IsSampler (Type type);

	/** Readable dump of the reflection, used by the log and the command line. */
	std::string Describe (const Translated& translated);

}
}
#endif
