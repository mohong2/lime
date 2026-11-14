#include "glsl.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace vulkan {
namespace glsl {

	// ---------------------------------------------------------------------
	// Type table
	// ---------------------------------------------------------------------

	namespace {

		struct TypeInfo {

			const char* name;
			uint32_t size;      // std140 size
			uint32_t alignment; // std140 alignment

		};

		const TypeInfo kTypes[] = {
			{ "", 0, 0 },              // TYPE_UNKNOWN
			{ "float", 4, 4 },
			{ "int", 4, 4 },
			{ "uint", 4, 4 },
			{ "bool", 4, 4 },
			{ "vec2", 8, 8 },
			{ "vec3", 12, 16 },
			{ "vec4", 16, 16 },
			{ "ivec2", 8, 8 },
			{ "ivec3", 12, 16 },
			{ "ivec4", 16, 16 },
			{ "uvec2", 8, 8 },
			{ "uvec3", 12, 16 },
			{ "uvec4", 16, 16 },
			{ "bvec2", 8, 8 },
			{ "bvec3", 12, 16 },
			{ "bvec4", 16, 16 },
			{ "mat2", 32, 16 },        // under std140 a matrix is an array of columns, each padded to 16 bytes
			{ "mat3", 48, 16 },
			{ "mat4", 64, 16 },
			{ "sampler2D", 0, 0 },
			{ "samplerCube", 0, 0 },
		};

		const int kTypeCount = (int)(sizeof (kTypes) / sizeof (kTypes[0]));

		Type ParseType (const std::string& name) {

			for (int i = 1; i < kTypeCount; ++i) {

				if (name == kTypes[i].name) return (Type)i;

			}

			return TYPE_UNKNOWN;

		}

		uint32_t RoundUp (uint32_t value, uint32_t alignment) {

			if (alignment <= 1) return value;

			return (value + alignment - 1) / alignment * alignment;

		}

		bool IsIdentStart (char c) {

			return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';

		}

		bool IsIdentChar (char c) {

			return IsIdentStart (c) || (c >= '0' && c <= '9');

		}

		bool StartsWith (const std::string& text, const char* prefix) {

			size_t length = strlen (prefix);

			return text.size() >= length && text.compare (0, length, prefix) == 0;

		}

		std::string Trim (const std::string& text) {

			size_t start = 0;
			size_t end = text.size();

			while (start < end && (text[start] == ' ' || text[start] == '\t' || text[start] == '\r')) ++start;
			while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) --end;

			return text.substr (start, end - start);

		}

	}

	// ---------------------------------------------------------------------
	// The OpenFL / flixel standard header.
	//
	// This is a public contract, not a copy of the implementation: OpenFL replaces
	// #pragma header with these declarations while Haxe compiles, so the string
	// that arrives here no longer contains the pragma. The copy exists so that
	//   (1) the command line tool can take an unexpanded shader source;
	//   (2) a caller that bypasses ShaderMacro can still compile.
	// ---------------------------------------------------------------------

	static const char* kOpenflVertexHeader =
		"attribute float openfl_Alpha;\n"
		"attribute vec4 openfl_ColorMultiplier;\n"
		"attribute vec4 openfl_ColorOffset;\n"
		"attribute vec4 openfl_Position;\n"
		"attribute vec2 openfl_TextureCoord;\n"
		"\n"
		"varying float openfl_Alphav;\n"
		"varying vec4 openfl_ColorMultiplierv;\n"
		"varying vec4 openfl_ColorOffsetv;\n"
		"varying vec2 openfl_TextureCoordv;\n"
		"\n"
		"uniform mat4 openfl_Matrix;\n"
		"uniform bool openfl_HasColorTransform;\n"
		"uniform vec2 openfl_TextureSize;\n";

	static const char* kOpenflVertexBody =
		"openfl_Alphav = openfl_Alpha;\n"
		"openfl_TextureCoordv = openfl_TextureCoord;\n"
		"if (openfl_HasColorTransform) {\n"
		"openfl_ColorMultiplierv = openfl_ColorMultiplier;\n"
		"openfl_ColorOffsetv = openfl_ColorOffset / 255.0;\n"
		"}\n"
		"gl_Position = openfl_Matrix * openfl_Position;\n";

	static const char* kOpenflFragmentHeader =
		"varying float openfl_Alphav;\n"
		"varying vec4 openfl_ColorMultiplierv;\n"
		"varying vec4 openfl_ColorOffsetv;\n"
		"varying vec2 openfl_TextureCoordv;\n"
		"\n"
		"uniform bool openfl_HasColorTransform;\n"
		"uniform vec2 openfl_TextureSize;\n"
		"uniform sampler2D bitmap;\n";

	// The fragment header of FlxGraphicsShader (flixel's colour transform path).
	// The semantics have to match character for character, or the same shader comes
	static const char* kFlixelFragmentHeader =
		"uniform bool hasTransform;\n"
		"uniform bool hasColorTransform;\n"
		"\n"
		"vec4 flixel_texture2D (sampler2D bitmap, vec2 coord) {\n"
		"vec4 color = texture2D (bitmap, coord);\n"
		"if (!hasTransform) { return color; }\n"
		"if (color.a == 0.0) { return vec4 (0.0, 0.0, 0.0, 0.0); }\n"
		"if (!hasColorTransform) { return color * openfl_Alphav; }\n"
		"color = vec4 (color.rgb / color.a, color.a);\n"
		"color = clamp (openfl_ColorOffsetv + vec4 (color.rgb * openfl_ColorMultiplierv.rgb, color.a * openfl_ColorMultiplierv.w), 0.0, 1.0);\n"
		"if (color.a > 0.0) { return vec4 (color.rgb * color.a * openfl_Alphav, color.a * openfl_Alphav); }\n"
		"return vec4 (0.0, 0.0, 0.0, 0.0);\n"
		"}\n";

	// ---------------------------------------------------------------------
	// Pre-pass 1: expand #pragma header / #pragma body
	// ---------------------------------------------------------------------

	/**
	 * True when the source already defines "flixel_texture2D".
	 *
	 * The distinction matters: a source OpenFL already expanded carries the
	 * definition, and adding a second one is a compile error. A source that only
	 * *calls* it (or maps "texture" onto it with a #define) needs the definition
	 * added, or every call is an unknown function — which is what happened to the
	 * TVStatic / saturation / rain filters.
	 *
	 * A definition always has a return type in front of the name; a call never does.
	 */
	static bool DefinesFlixelTexture (const std::string& source) {

		const std::string name = "flixel_texture2D";
		size_t position = 0;

		while ((position = source.find (name, position)) != std::string::npos) {

			size_t wordEnd = position;

			while (wordEnd > 0 && (source[wordEnd - 1] == ' ' || source[wordEnd - 1] == '	' || source[wordEnd - 1] == '')) --wordEnd;

			for (int qualifier = 0; qualifier < 2; ++qualifier) {

				size_t wordStart = wordEnd;

				while (wordStart > 0 && (isalnum ((unsigned char)source[wordStart - 1]) || source[wordStart - 1] == '_')) --wordStart;

				if (wordStart == wordEnd) break;

				const std::string word = source.substr (wordStart, wordEnd - wordStart);

				if (word == "vec4" || word == "float4") return true;

				// A precision qualifier may sit between the type and the name.
				if (word != "highp" && word != "mediump" && word != "lowp") break;

				wordEnd = wordStart;

				while (wordEnd > 0 && (source[wordEnd - 1] == ' ' || source[wordEnd - 1] == '	' || source[wordEnd - 1] == '')) --wordEnd;

			}

			position += name.size ();

		}

		return false;

	}

	static std::string ExpandPragmas (const std::string& source, bool isVertex, bool openfl) {

		std::string out;
		out.reserve (source.size() + 1024);

		// A shader that ships its own flixel_texture2D keeps it: a second definition
		// of the same function is a compile error in GLSL.
		const bool ownFlixelTexture = DefinesFlixelTexture (source);

		size_t position = 0;

		while (position < source.size()) {

			size_t end = source.find ('\n', position);

			if (end == std::string::npos) end = source.size();

			std::string line = source.substr (position, end - position);
			std::string trimmed = Trim (line);

			if (StartsWith (trimmed, "#pragma")) {

				std::string rest = Trim (trimmed.substr (7));

				if (openfl && rest == "header") {

					if (isVertex) {

						out += kOpenflVertexHeader;

					} else {

						out += kOpenflFragmentHeader;

						if (!ownFlixelTexture) out += kFlixelFragmentHeader;

					}

				} else if (openfl && rest == "body") {

					if (isVertex) out += kOpenflVertexBody;

				}

				// Every other #pragma is dropped: glslang does not know them and keeping them only produces warnings.

			} else {

				out += line;
				out += '\n';

			}

			position = end + 1;

		}

		return out;

	}

	// ---------------------------------------------------------------------
	// Pre-pass 2: strip comments (replaced by spaces, newlines kept so line numbers stay valid)
	// ---------------------------------------------------------------------

	static std::string StripComments (const std::string& source) {

		std::string out;
		out.reserve (source.size());

		size_t position = 0;

		while (position < source.size()) {

			char c = source[position];

			if (c == '/' && position + 1 < source.size() && source[position + 1] == '/') {

				while (position < source.size() && source[position] != '\n') {

					out += ' ';
					++position;

				}

				continue;

			}

			if (c == '/' && position + 1 < source.size() && source[position + 1] == '*') {

				position += 2;
				out += ' ';
				out += ' ';

				while (position + 1 < source.size() && !(source[position] == '*' && source[position + 1] == '/')) {

					out += (source[position] == '\n') ? '\n' : ' ';
					++position;

				}

				position += 2;

				continue;

			}

			out += c;
			++position;

		}

		return out;

	}

	// ---------------------------------------------------------------------
	// Single stage scan
	// ---------------------------------------------------------------------

	namespace {

		struct Declaration {

			std::string name;
			Type type;
			int arraySize;

		};

		bool HasDeclaration (const std::vector<Declaration>& list, const std::string& name) {

			for (size_t i = 0; i < list.size(); ++i) {

				if (list[i].name == name) return true;

			}

			return false;

		}

		bool HasName (const std::vector<std::string>& list, const std::string& name) {

			for (size_t i = 0; i < list.size(); ++i) {

				if (list[i] == name) return true;

			}

			return false;

		}

		struct StageScan {

			std::string body;
			std::vector<Declaration> uniforms;
			std::vector<Declaration> attributes;
			std::vector<Declaration> varyings;
			std::vector<std::string> samplers; /**< name, in declaration order */
			bool usesFragColor;
			std::string log;
			bool fatal;

			StageScan () : usesFragColor (false), fatal (false) {}

		};

		// Identifier rewrite table. None of these exist, or keep their name, in ES 3.10.
		struct IdentMapping {

			const char* from;
			const char* to;

		};

		// texture2D is a reserved word in ES 3.10, so the old names are pointed at thin
		// forwarders (kFragmentTextureWrappers / kVertexTextureWrappers) with lime_vulkan_
		// prefixed names. A shader's own "#define texture flixel_texture2D" cannot
		// capture those.
		const IdentMapping kMappings[] = {
			{ "texture2DProjLod", "lime_vulkan_texture2DProjLod" },
			{ "texture2DProj", "lime_vulkan_texture2DProj" },
			{ "texture2DLod", "lime_vulkan_texture2DLod" },
			{ "texture2D", "lime_vulkan_texture2D" },
			{ "textureCubeLod", "lime_vulkan_textureCubeLod" },
			{ "textureCube", "lime_vulkan_textureCube" },
			{ "gl_FragColor", "lime_vulkan_FragColor" },
		};

		void AppendLog (std::string& log, const char* format, ...) {

			char buffer[512];
			va_list args;

			va_start (args, format);
			vsnprintf (buffer, sizeof (buffer), format, args);
			va_end (args);

			log += buffer;
			log += '\n';

		}

		std::string ReadIdent (const std::string& text, size_t* position) {

			size_t start = *position;

			while (*position < text.size() && IsIdentChar (text[*position])) ++(*position);

			return text.substr (start, *position - start);

		}

		void SkipSpace (const std::string& text, size_t* position) {

			while (*position < text.size()) {

				char c = text[*position];

				if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;

				++(*position);

			}

		}

		bool IsPrecisionQualifier (const std::string& word) {

			return word == "lowp" || word == "mediump" || word == "highp";

		}

		// The vertex stream is a single one: pos(vec2) / uv(vec2) / color(u32).
		// Every attribute OpenFL and flixel pass in collapses onto it.
		const char* kOpenflAttributeNames[] = {
			"openfl_Position",
			"openfl_TextureCoord",
			"openfl_Alpha",
			"openfl_ColorMultiplier",
			"openfl_ColorOffset",
			"alpha",
			"colorMultiplier",
			"colorOffset",
		};

		const int kOpenflAttributeCount = (int)(sizeof (kOpenflAttributeNames) / sizeof (kOpenflAttributeNames[0]));

		bool IsKnownOpenflAttribute (const std::string& name) {

			for (int i = 0; i < kOpenflAttributeCount; ++i) {

				if (name == kOpenflAttributeNames[i]) return true;

			}

			return false;

		}

		// Parses one uniform / attribute / varying declaration up to the semicolon.
		// Handles several names per statement, a precision prefix and 1D constant arrays.
		bool ParseDeclaration (const std::string& text, size_t* position, bool isVertex,
			const char* kind, StageScan* scan) {

			SkipSpace (text, position);

			std::string word = ReadIdent (text, position);

			while (IsPrecisionQualifier (word)) {

				SkipSpace (text, position);
				word = ReadIdent (text, position);

			}

			Type type = ParseType (word);

			if (type == TYPE_UNKNOWN) {

				AppendLog (scan->log, "unsupported %s type '%s'", kind, word.c_str());
				scan->fatal = true;
				return false;

			}

			// `uniform float[] a;` puts the brackets before the name. That is the
			// GLSL 1.10 spelling of an array whose length the caller decides, so it
			// becomes a one element array here.
			bool typeArray = false;

			SkipSpace (text, position);

			if (*position < text.size() && text[*position] == '[') {

				typeArray = true;

				while (*position < text.size() && text[*position] != ']') ++(*position);
				if (*position < text.size()) ++(*position);

			}

			for (;;) {

				SkipSpace (text, position);

				std::string name = ReadIdent (text, position);

				if (name.empty()) {

					AppendLog (scan->log, "malformed %s declaration of type '%s'", kind, word.c_str());
					scan->fatal = true;
					return false;

				}

				int arraySize = typeArray ? 1 : 0;

				SkipSpace (text, position);

				if (!typeArray && *position < text.size() && text[*position] == '[') {

					++(*position);
					SkipSpace (text, position);

					size_t digits = *position;

					while (*position < text.size() && text[*position] >= '0' && text[*position] <= '9') ++(*position);

					if (digits == *position) {

						AppendLog (scan->log, "%s '%s' has a non constant array size", kind, name.c_str());
						scan->fatal = true;
						return false;

					}

					arraySize = atoi (text.substr (digits, *position - digits).c_str());

					while (*position < text.size() && text[*position] != ']') ++(*position);
					if (*position < text.size()) ++(*position);

				}

				Declaration declaration;
				declaration.name = name;
				declaration.type = type;
				declaration.arraySize = arraySize;

				// A source that carries its own declarations AND an OpenFL header can
				// name the same symbol twice. The second one adds nothing.
				if (IsSampler (type)) {

					if (strcmp (kind, "uniform") != 0 || arraySize != 0) {

						AppendLog (scan->log, "sampler '%s' must be a plain uniform sampler2D", name.c_str());
						scan->fatal = true;
						return false;

					}

					if (!HasName (scan->samplers, name)) scan->samplers.push_back (name);

				} else if (strcmp (kind, "uniform") == 0) {

					if (!HasDeclaration (scan->uniforms, name)) scan->uniforms.push_back (declaration);

				} else if (strcmp (kind, "attribute") == 0) {

					if (!isVertex) {

						AppendLog (scan->log, "attribute '%s' declared in the fragment shader", name.c_str());
						scan->fatal = true;
						return false;

					}

					if (!HasDeclaration (scan->attributes, name)) scan->attributes.push_back (declaration);

				} else {

					if (!HasDeclaration (scan->varyings, name)) scan->varyings.push_back (declaration);

				}

				SkipSpace (text, position);

				// `uniform float x = 0.5;` is legal in desktop GLSL and turns up in mod
				// shaders. A uniform block member cannot carry an initializer, so the
				// value is dropped; it always comes from the shader data anyway.
				if (*position < text.size() && text[*position] == '=') {

					++(*position);

					int parentheses = 0;

					while (*position < text.size()) {

						char current = text[*position];

						if (current == '(') ++parentheses;
						else if (current == ')') --parentheses;
						else if (parentheses == 0 && (current == ';' || current == ',')) break;

						++(*position);

					}

					SkipSpace (text, position);

				}

				if (*position < text.size() && text[*position] == ',') {

					++(*position);
					continue;

				}

				if (*position < text.size() && text[*position] == ';') {

					++(*position);
					return true;

				}

				AppendLog (scan->log, "unterminated %s declaration for '%s'", kind, name.c_str());
				scan->fatal = true;
				return false;

			}

		}

		// One pass over the source:
		//   - lift the uniform / attribute / varying / precision declarations out
		//     (top level ones, brace depth 0);
		//   - in the body, rewrite ES 1.00 identifiers to their 3.10 names;
		//   - drop #version, obsolete #extension and #line.
		// Declarations are taken from where they were, so a declaration inside an
		// #ifdef ends up outside the block. That is deliberate: both stages must
		// see exactly the same uniform block, otherwise the offsets differ.
		bool ScanStage (const std::string& source, bool isVertex, StageScan* scan) {

			size_t position = 0;
			int depth = 0;
			bool lineStart = true;

			std::string& body = scan->body;
			body.reserve (source.size());

			while (position < source.size()) {

				char c = source[position];

				if (c == '\n') {

					body += c;
					++position;
					lineStart = true;
					continue;

				}

				if (lineStart) {

					size_t probe = position;

					while (probe < source.size() && (source[probe] == ' ' || source[probe] == '\t')) ++probe;

					if (probe < source.size() && source[probe] == '#') {

						size_t end = source.find ('\n', probe);

						if (end == std::string::npos) end = source.size();

						std::string line = Trim (source.substr (probe, end - probe));

						if (StartsWith (line, "#version")) {

							// The version is written by us, so the one in the source is dropped (it is an error anywhere but the first line).

						} else if (StartsWith (line, "#extension")) {

							// These are core in ES 3.10, keeping them only produces 'unsupported extension'.
							if (line.find ("GL_OES_standard_derivatives") != std::string::npos
								|| line.find ("GL_EXT_shader_frag_depth") != std::string::npos
								|| line.find ("GL_EXT_shader_texture_lod") != std::string::npos
								|| line.find ("GL_OES_EGL_image_external") != std::string::npos) {

								AppendLog (scan->log, "dropped obsolete extension directive: %s", line.c_str());

							} else {

								body += line;

							}

						} else if (StartsWith (line, "#line")) {

							// Dropped: the line numbers would not line up and would mislead.

						} else {

							body += line;

						}

						position = end;
						lineStart = false;
						continue;

					}

				}

				lineStart = false;

				if (c == ' ' || c == '\t' || c == '\r') {

					body += c;
					++position;
					continue;

				}

				if (c == '{') {

					++depth;
					body += c;
					++position;
					continue;

				}

				if (c == '}') {

					if (depth > 0) --depth;
					body += c;
					++position;
					continue;

				}

				if (IsIdentStart (c)) {

					size_t start = position;
					std::string word = ReadIdent (source, &position);

					if (depth == 0 && word == "uniform") {

						if (!ParseDeclaration (source, &position, isVertex, "uniform", scan)) return false;
						continue;

					}

					if (depth == 0 && word == "attribute") {

						if (!ParseDeclaration (source, &position, isVertex, "attribute", scan)) return false;
						continue;

					}

					if (depth == 0 && word == "varying") {

						if (!ParseDeclaration (source, &position, isVertex, "varying", scan)) return false;
						continue;

					}

					if (depth == 0 && word == "precision") {

						// The precision is injected centrally, so the one in the source is dropped and every platform behaves the same.
						while (position < source.size() && source[position] != ';') ++position;
						if (position < source.size()) ++position;
						continue;

					}

					if (word == "gl_FragColor") scan->usesFragColor = true;

					if (word == "gl_FragData") {

						AppendLog (scan->log, "gl_FragData (multiple render targets) is not supported");
						scan->fatal = true;
						return false;

					}

					bool mapped = false;

					for (int i = 0; i < (int)(sizeof (kMappings) / sizeof (kMappings[0])); ++i) {

						if (word == kMappings[i].from) {

							body += kMappings[i].to;
							mapped = true;
							break;

						}

					}

					if (!mapped) body.append (source, start, position - start);

					continue;

				}

				body += c;
				++position;

			}

			return true;

		}

	}

	// ---------------------------------------------------------------------
	// Small helpers
	// ---------------------------------------------------------------------

	Variable::Variable () :
		type (TYPE_UNKNOWN),
		arraySize (0),
		offset (0),
		size (0),
		stride (0),
		location (-1) {
	}

	Translated::Translated () :
		ok (false),
		uniformSize (0) {
	}

	Options::Options () :
		openfl (true),
		injectPrecision (true) {
	}

	const char* TypeName (Type type) {

		if (type <= TYPE_UNKNOWN || type >= kTypeCount) return "unknown";

		return kTypes[type].name;

	}

	uint32_t TypeSize (Type type) {

		if (type <= TYPE_UNKNOWN || type >= kTypeCount) return 0;

		return kTypes[type].size;

	}

	uint32_t TypeAlignment (Type type) {

		if (type <= TYPE_UNKNOWN || type >= kTypeCount) return 4;

		return kTypes[type].alignment;

	}

	bool IsSampler (Type type) {

		return type == TYPE_SAMPLER2D || type == TYPE_SAMPLER_CUBE;

	}

	// ---------------------------------------------------------------------
	// Merge, lay out, generate
	// ---------------------------------------------------------------------

	namespace {

		// Compatibility macros: they map the OpenFL / flixel attribute names onto this
		// backend's single vertex stream.
		//
		// Vertex colour is (tint.rgb, alpha). OpenFL and flixel have two vertex
		// boilerplates:
		//   GraphicsShader  : openfl_Alphav = openfl_Alpha; the multipliers are
		//                     assigned inside if (openfl_HasColorTransform)
		//   FlxGraphicsShader: adds openfl_Alphav = openfl_Alpha * alpha
		//
		// Both have to produce the same result, so:
		//   openfl_Alpha  -> lime_vulkan_Color.a   (object alpha)
		//   alpha         -> 1.0           (already folded into the vertex colour, so it
		//                                   must not multiply again)
		//   openfl_HasColorTransform -> 1  (force the colour transform path)
		// With only the OpenFL boilerplate the result is a * mult(lime_vulkan_Color), and with
		// both the product is still a.
		const char* kOpenflMacros =
			"#define openfl_Position        vec4 (lime_vulkan_Position, 0.0, 1.0)\n"
			"#define openfl_TextureCoord    lime_vulkan_TexCoord\n"
			"#define openfl_Alpha           lime_vulkan_Color.a\n"
			"#define openfl_ColorMultiplier vec4 (lime_vulkan_Color.rgb, 1.0)\n"
			"#define openfl_ColorOffset     (lime_vulkan_ColorOffset * 255.0)\n"
			"#define openfl_Matrix          lime_vulkan_Projection\n"
			"#define openfl_HasColorTransform true\n"
			"#define alpha                  1.0\n"
			"#define colorMultiplier        vec4 (lime_vulkan_Color.rgb, 1.0)\n"
			// Both sample vertexes divide by 255 before storing the offset in a varying,
			// so hand them the 0..255 form OpenFL expects and let the division happen
			// where it already is. The attribute itself is already -1..1.
			"#define colorOffset            (lime_vulkan_ColorOffset * 255.0)\n";

		// ES 1.00 samplers are read with texture2D and friends, which ES 3.10 replaced
		// with a single overload set. Thin forwarders keep both spellings working and,
		// unlike a macro, they do not collide with a shader's own "#define texture".
		// Implicit LOD sampling is a fragment-only operation, so the vertex stage gets
		// the explicit-LOD form of every wrapper.
		const char* kFragmentTextureWrappers =
			"vec4 lime_vulkan_texture2D (sampler2D s, vec2 c) { return texture (s, c); }\n"
			"vec4 lime_vulkan_texture2D (sampler2D s, vec2 c, float bias) { return texture (s, c, bias); }\n"
			"vec4 lime_vulkan_texture2DProj (sampler2D s, vec3 c) { return textureProj (s, c); }\n"
			"vec4 lime_vulkan_texture2DProj (sampler2D s, vec4 c) { return textureProj (s, c); }\n"
			"vec4 lime_vulkan_texture2DLod (sampler2D s, vec2 c, float lod) { return textureLod (s, c, lod); }\n"
			"vec4 lime_vulkan_texture2DProjLod (sampler2D s, vec3 c, float lod) { return textureProjLod (s, c, lod); }\n"
			"vec4 lime_vulkan_textureCube (samplerCube s, vec3 c) { return texture (s, c); }\n"
			"vec4 lime_vulkan_textureCubeLod (samplerCube s, vec3 c, float lod) { return textureLod (s, c, lod); }\n";

		const char* kVertexTextureWrappers =
			"vec4 lime_vulkan_texture2D (sampler2D s, vec2 c) { return textureLod (s, c, 0.0); }\n"
			"vec4 lime_vulkan_texture2D (sampler2D s, vec2 c, float lod) { return textureLod (s, c, lod); }\n"
			"vec4 lime_vulkan_texture2DProj (sampler2D s, vec4 c) { return textureProjLod (s, c, 0.0); }\n"
			"vec4 lime_vulkan_texture2DLod (sampler2D s, vec2 c, float lod) { return textureLod (s, c, lod); }\n"
			"vec4 lime_vulkan_texture2DProjLod (sampler2D s, vec4 c, float lod) { return textureProjLod (s, c, lod); }\n"
			"vec4 lime_vulkan_textureCube (samplerCube s, vec3 c) { return textureLod (s, c, 0.0); }\n"
			"vec4 lime_vulkan_textureCubeLod (samplerCube s, vec3 c, float lod) { return textureLod (s, c, lod); }\n";

		const char* kFragmentColorDefine = "#define gl_FragColor lime_vulkan_FragColor\n";

		void AppendLocatedDeclaration (std::string& text, const Variable& variable, const char* storage, int location) {

			char header[192];

			snprintf (header, sizeof (header), "layout(location = %d) %s ", location, storage);

			text += header;
			text += TypeName (variable.type);
			text += ' ';
			text += variable.name;

			if (variable.arraySize > 0) {

				char suffix[32];
				snprintf (suffix, sizeof (suffix), "[%d]", variable.arraySize);
				text += suffix;

			}

			text += ";\n";

		}

		void AppendUniformBlock (std::string& text, const std::vector<Variable>& uniforms) {

			text += "layout(std140, set = 0, binding = 0) uniform lime_vulkan_Uniforms {\n";

			for (size_t i = 0; i < uniforms.size(); ++i) {

				text += '\t';
				text += TypeName (uniforms[i].type);
				text += ' ';
				text += uniforms[i].name;

				if (uniforms[i].arraySize > 0) {

					char suffix[32];
					snprintf (suffix, sizeof (suffix), "[%d]", uniforms[i].arraySize);
					text += suffix;

				}

				text += ";\n";

			}

			text += "};\n\n";

		}

	}

	namespace {

		// ES 3.10 reserves these names, so a shader that declared its own version
		// for ES 1.00 stops compiling once it is retargeted. ShaderToy ports do this
		// most often with round() and texture().
		const char* kBuiltinFunctions[] = {
			"radians", "degrees", "sin", "cos", "tan", "asin", "acos", "atan",
			"sinh", "cosh", "tanh", "asinh", "acosh", "atanh",
			"pow", "exp", "log", "exp2", "log2", "sqrt", "inversesqrt",
			"abs", "sign", "floor", "trunc", "round", "roundEven", "ceil", "fract",
			"mod", "modf", "min", "max", "clamp", "mix", "step", "smoothstep",
			"isnan", "isinf", "length", "distance", "dot", "cross", "normalize",
			"faceforward", "reflect", "refract", "matrixCompMult", "outerProduct",
			"transpose", "determinant", "inverse", "lessThan", "lessThanEqual",
			"greaterThan", "greaterThanEqual", "equal", "notEqual", "any", "all", "not",
			"texture", "textureProj", "textureLod", "textureOffset", "texelFetch",
			"textureSize", "textureGrad", "dFdx", "dFdy", "fwidth"
		};

		const int kBuiltinFunctionCount = (int)(sizeof (kBuiltinFunctions) / sizeof (kBuiltinFunctions[0]));

		bool IsBuiltinFunction (const std::string& name) {

			for (int i = 0; i < kBuiltinFunctionCount; ++i) {

				if (name == kBuiltinFunctions[i]) return true;

			}

			return false;

		}

		// Words that are free identifiers in ES 1.00 and reserved by ES 3.10. Shader
		// ports use them as variable names constantly (input, output, sample, filter).
		const char* kReservedIdentifiers[] = {
			"sample", "filter", "input", "output", "precise", "coherent",
			"volatile", "restrict", "readonly", "writeonly"
		};

		const int kReservedIdentifierCount = (int)(sizeof (kReservedIdentifiers) / sizeof (kReservedIdentifiers[0]));

		bool IsTypeKeyword (const std::string& word) {

			return word == "void" || word == "float" || word == "int" || word == "uint"
				|| word == "bool" || word == "vec2" || word == "vec3" || word == "vec4"
				|| word == "ivec2" || word == "ivec3" || word == "ivec4"
				|| word == "uvec2" || word == "uvec3" || word == "uvec4"
				|| word == "bvec2" || word == "bvec3" || word == "bvec4"
				|| word == "mat2" || word == "mat3" || word == "mat4";

		}

		/** Replaces whole identifiers only, so lime_vulkan_round and roundTrip stay untouched. */
		void ReplaceIdentifier (std::string* text, const std::string& from, const std::string& to) {

			std::string out;
			out.reserve (text->size());

			size_t position = 0;

			while (position < text->size()) {

				char c = (*text)[position];

				if (!IsIdentStart (c)) {

					out += c;
					++position;
					continue;

				}

				size_t start = position;

				while (position < text->size() && IsIdentChar ((*text)[position])) ++position;

				std::string word = text->substr (start, position - start);

				out += (word == from) ? to : word;

			}

			*text = out;

		}

		void RenameReservedIdentifiers (std::string* body, std::string* log) {

			for (int i = 0; i < kReservedIdentifierCount; ++i) {

				const std::string word = kReservedIdentifiers[i];

				if (body->find (word) == std::string::npos) continue;

				AppendLog (*log, "'%s' is reserved in ES 3.10; renamed to 'lime_vulkan_%s'", word.c_str(), word.c_str());

				ReplaceIdentifier (body, word, "lime_vulkan_" + word);

			}

		}

		/** Renames top level function definitions that shadow an ES 3.10 built-in. */
		void RenameShadowedBuiltins (std::string* body, std::string* log) {

			std::vector<std::string> names;

			size_t position = 0;
			int depth = 0;
			bool lineStart = true;

			while (position < body->size()) {

				char c = (*body)[position];

				if (c == '\n') {

					lineStart = true;
					++position;
					continue;

				}

				if (lineStart) {

					size_t probe = position;

					while (probe < body->size() && ((*body)[probe] == ' ' || (*body)[probe] == '\t')) ++probe;

					if (probe < body->size() && (*body)[probe] == '#') {

						while (position < body->size() && (*body)[position] != '\n') ++position;
						continue;

					}

				}

				lineStart = false;

				if (c == '{' || c == '}') {

					if (c == '{') ++depth;
					else if (depth > 0) --depth;

					++position;
					continue;

				}

				if (depth == 0 && IsIdentStart (c)) {

					size_t start = position;

					while (position < body->size() && IsIdentChar ((*body)[position])) ++position;

					std::string word = body->substr (start, position - start);

					if (IsTypeKeyword (word)) {

						while (position < body->size() && ((*body)[position] == ' ' || (*body)[position] == '\t')) ++position;

						size_t nameStart = position;

						while (position < body->size() && IsIdentChar ((*body)[position])) ++position;

						std::string name = body->substr (nameStart, position - nameStart);

						size_t probe = position;

						while (probe < body->size() && ((*body)[probe] == ' ' || (*body)[probe] == '\t')) ++probe;

						if (!name.empty() && probe < body->size() && (*body)[probe] == '('
							&& name != "main" && IsBuiltinFunction (name)) {

							names.push_back (name);

						}

					}

					continue;

				}

				++position;

			}

			for (size_t i = 0; i < names.size(); ++i) {

				AppendLog (*log, "function '%s' shadows an ES 3.10 built-in; renamed to 'lime_vulkan_%s'",
					names[i].c_str(), names[i].c_str());

				ReplaceIdentifier (body, names[i], "lime_vulkan_" + names[i]);

			}

		}

	}

	bool Translate (const char* vertexSource, int vertexLength,
		const char* fragmentSource, int fragmentLength,
		const Options& options, Translated* out) {

		if (!vertexSource || vertexLength <= 0 || !fragmentSource || fragmentLength <= 0) {

			out->log = "Translate: empty shader source\n";
			out->ok = false;
			return false;

		}

		std::string vertexText (vertexSource, vertexLength);
		std::string fragmentText (fragmentSource, fragmentLength);

		vertexText = StripComments (ExpandPragmas (vertexText, true, options.openfl));
		fragmentText = StripComments (ExpandPragmas (fragmentText, false, options.openfl));

		StageScan vertexScan;
		StageScan fragmentScan;

		if (!ScanStage (vertexText, true, &vertexScan)) {

			out->log = "vertex shader: " + vertexScan.log;
			out->ok = false;
			return false;

		}

		if (!ScanStage (fragmentText, false, &fragmentScan)) {

			out->log = "fragment shader: " + fragmentScan.log;
			out->ok = false;
			return false;

		}

		std::string log = vertexScan.log + fragmentScan.log;
		bool fatal = false;

		RenameShadowedBuiltins (&vertexScan.body, &log);
		RenameShadowedBuiltins (&fragmentScan.body, &log);
		RenameReservedIdentifiers (&vertexScan.body, &log);
		RenameReservedIdentifiers (&fragmentScan.body, &log);

		// A macro can hide the gl_FragColor write from the rename pass, so the
		// source text decides whether the output variable has to exist.
		const bool mentionsFragColor = fragmentText.find ("gl_FragColor") != std::string::npos;

		// ---- Uniform merge: the vertex stage declares first, the fragment adds to it ----
		// The order has to be stable: both stages generate the same block, which keeps the offsets equal.

		std::vector<Variable> uniforms;
		std::map<std::string, int> uniformIndex;

		Variable projection;
		projection.name = "lime_vulkan_Projection";
		projection.type = TYPE_MAT4;
		projection.offset = 0;
		projection.size = 64;

		uniforms.push_back (projection);
		uniformIndex["lime_vulkan_Projection"] = 0;

		uint32_t offset = 64;

		StageScan* order[2] = { &vertexScan, &fragmentScan };

		for (int stage = 0; stage < 2; ++stage) {

			for (size_t i = 0; i < order[stage]->uniforms.size(); ++i) {

				const Declaration& declaration = order[stage]->uniforms[i];

				// The compatibility layer maps openfl_Matrix onto lime_vulkan_Projection, so it cannot stay a separate uniform.
				if (options.openfl && declaration.name == "openfl_Matrix") continue;
				if (declaration.name == "lime_vulkan_Projection") continue;

				if (IsSampler (declaration.type)) continue;

				std::map<std::string, int>::iterator existing = uniformIndex.find (declaration.name);

				if (existing != uniformIndex.end()) {

					if (uniforms[existing->second].type != declaration.type) {

						AppendLog (log, "uniform '%s' has different types in the two stages", declaration.name.c_str());

					}

					continue;

				}

				Variable variable;
				variable.name = declaration.name;
				variable.type = declaration.type;
				variable.arraySize = declaration.arraySize;

				uint32_t alignment = TypeAlignment (variable.type);
				uint32_t size = TypeSize (variable.type);

				if (variable.arraySize > 0) {

					// std140: the element stride is padded to a multiple of 16 and the array as a whole is 16 aligned.
					variable.stride = RoundUp (size, 16);
					alignment = 16;
					size = variable.stride * (uint32_t)variable.arraySize;

				}

				offset = RoundUp (offset, alignment);

				variable.offset = offset;
				variable.size = size;

				offset += size;

				uniformIndex[variable.name] = (int)uniforms.size();
				uniforms.push_back (variable);

			}

		}

		out->uniforms = uniforms;
		out->uniformSize = RoundUp (offset, 16);

		// ---- Samplers: binding 0 is the uniform block, samplers start at 1 ----

		std::vector<std::string> samplerNames;
		std::map<std::string, int> samplerBinding;

		for (int stage = 0; stage < 2; ++stage) {

			for (size_t i = 0; i < order[stage]->samplers.size(); ++i) {

				const std::string& name = order[stage]->samplers[i];

				if (samplerBinding.find (name) != samplerBinding.end()) continue;

				samplerBinding[name] = 1 + (int)samplerNames.size();
				samplerNames.push_back (name);

			}

		}

		if (samplerNames.size() > 15) {

			out->log = "too many samplers (limit is 15)\n";
			out->ok = false;
			return false;

		}

		out->samplers.clear();

		for (size_t i = 0; i < samplerNames.size(); ++i) {

			Variable variable;
			variable.name = samplerNames[i];
			variable.type = TYPE_SAMPLER2D;
			variable.location = samplerBinding[samplerNames[i]];

			out->samplers.push_back (variable);

		}

		// ---- Varyings: locations follow the vertex declaration order, the fragment matches by name ----

		out->varyings.clear();

		std::map<std::string, int> varyingLocation;

		for (size_t i = 0; i < vertexScan.varyings.size(); ++i) {

			Variable variable;
			variable.name = vertexScan.varyings[i].name;
			variable.type = vertexScan.varyings[i].type;
			variable.arraySize = vertexScan.varyings[i].arraySize;
			variable.location = (int)out->varyings.size();

			varyingLocation[variable.name] = variable.location;
			out->varyings.push_back (variable);

		}

		for (size_t i = 0; i < fragmentScan.varyings.size(); ++i) {

			const Declaration& declaration = fragmentScan.varyings[i];

			std::map<std::string, int>::iterator found = varyingLocation.find (declaration.name);

			if (found != varyingLocation.end()) {

				Variable& vertexSide = out->varyings[(size_t)found->second];

				if (vertexSide.type != declaration.type) {

					AppendLog (log, "varying '%s' is %s in the vertex shader but %s in the fragment shader",
						declaration.name.c_str(), TypeName (vertexSide.type), TypeName (declaration.type));

				}

				continue;

			}

			// Fragment only: usually carried in from the OpenFL header without the vertex
			// stage writing it. It gets a location anyway and reads an undefined value, which matches GL.
			AppendLog (log, "varying '%s' exists only in the fragment shader", declaration.name.c_str());

			Variable variable;
			variable.name = declaration.name;
			variable.type = declaration.type;
			variable.arraySize = declaration.arraySize;
			variable.location = (int)out->varyings.size();

			varyingLocation[variable.name] = variable.location;
			out->varyings.push_back (variable);

		}

		// ---- attribute -----------------------------------------------------

		out->attributes.clear();

		if (options.openfl) {

			Variable position;
			position.name = "lime_vulkan_Position";
			position.type = TYPE_VEC2;
			position.location = 0;
			out->attributes.push_back (position);

			Variable texCoord;
			texCoord.name = "lime_vulkan_TexCoord";
			texCoord.type = TYPE_VEC2;
			texCoord.location = 1;
			out->attributes.push_back (texCoord);

			Variable color;
			color.name = "lime_vulkan_Color";
			color.type = TYPE_VEC4;
			color.location = 2;
			out->attributes.push_back (color);

			Variable colorOffset;
			colorOffset.name = "lime_vulkan_ColorOffset";
			colorOffset.type = TYPE_VEC4;
			colorOffset.location = 3;
			out->attributes.push_back (colorOffset);

			for (size_t i = 0; i < vertexScan.attributes.size(); ++i) {

				if (IsKnownOpenflAttribute (vertexScan.attributes[i].name)) continue;

				AppendLog (log, "attribute '%s' has no slot in the 24 byte vertex layout", vertexScan.attributes[i].name.c_str());
				fatal = true;

			}

		} else {

			if (vertexScan.attributes.size() > 15) {

				out->log = "too many vertex attributes (limit is 15)\n";
				out->ok = false;
				return false;

			}

			for (size_t i = 0; i < vertexScan.attributes.size(); ++i) {

				Variable variable;
				variable.name = vertexScan.attributes[i].name;
				variable.type = vertexScan.attributes[i].type;
				variable.arraySize = vertexScan.attributes[i].arraySize;
				variable.location = (int)i;

				out->attributes.push_back (variable);

			}

		}

		// ---- Generate the vertex shader ----

		std::string vertex;
		vertex.reserve (vertexScan.body.size() + 2048);

		vertex += "#version 450\n";

		if (options.openfl) {

			// Mod shaders initialise globals from uniforms, which desktop GL accepts
			// and ES 3.10 only allows with this extension.
			vertex += "#extension GL_EXT_shader_non_constant_global_initializers : enable\n";

		}

		if (options.injectPrecision) {

			vertex += "precision highp float;\nprecision highp int;\n\n";

		}

		AppendUniformBlock (vertex, out->uniforms);

		if (options.openfl) {

			vertex += "layout(location = 0) in vec2 lime_vulkan_Position;\n";
			vertex += "layout(location = 1) in vec2 lime_vulkan_TexCoord;\n";
			vertex += "layout(location = 2) in vec4 lime_vulkan_Color;\n";
			vertex += "layout(location = 3) in vec4 lime_vulkan_ColorOffset;\n\n";

		} else {

			for (size_t i = 0; i < out->attributes.size(); ++i) {

				AppendLocatedDeclaration (vertex, out->attributes[i], "in", out->attributes[i].location);

			}

			vertex += "\n";

		}

		for (size_t i = 0; i < out->varyings.size(); ++i) {

			AppendLocatedDeclaration (vertex, out->varyings[i], "out", out->varyings[i].location);

		}

		std::vector<std::string> emitted;

		for (size_t i = 0; i < vertexScan.samplers.size(); ++i) {

			if (HasName (emitted, vertexScan.samplers[i])) continue;

			emitted.push_back (vertexScan.samplers[i]);

			char line[192];
			snprintf (line, sizeof (line), "layout(set = 0, binding = %d) uniform sampler2D %s;\n",
				samplerBinding[vertexScan.samplers[i]], vertexScan.samplers[i].c_str());
			vertex += line;

		}

		vertex += "\n";
		vertex += kVertexTextureWrappers;

		if (options.openfl) {

			vertex += "\n";
			vertex += kOpenflMacros;

		}

		// Compatibility fill-in: the OpenFL / flixel vertex boilerplate writes the colour
		// transform into the varyings, while a custom vertex shader often only writes
		// gl_Position. This used to append an assignment after main, but it can only be
		// appended for a varying the vertex stage already declared: a vertex shader that
		// declares no openfl_Alphav at all (the vertical blur pass of BlurFilter, for
		// example) fails with 'undeclared identifier' and the whole pass is lost.
		//
		// The real missing case (declared but never written) cannot be told apart by
		// searching the text, so the patch only runs when the vertex stage declared the
		// varying and the fragment stage uses it; otherwise the source is left as is.
		bool fixAlpha = false;
		bool fixCoord = false;
		bool fixMultiplier = false;
		bool fixOffset = false;

		if (options.openfl) {

			const std::string& fragmentBody = fragmentScan.body;
			const std::string& vertexBody = vertexScan.body;

			if (vertexBody.find ("openfl_Alphav") != std::string::npos
				&& fragmentBody.find ("openfl_Alphav") != std::string::npos
				&& vertexBody.find ("openfl_Alphav =") == std::string::npos) fixAlpha = true;

			if (vertexBody.find ("openfl_TextureCoordv") != std::string::npos
				&& fragmentBody.find ("openfl_TextureCoordv") != std::string::npos
				&& vertexBody.find ("openfl_TextureCoordv =") == std::string::npos) fixCoord = true;

			if (vertexBody.find ("openfl_ColorMultiplierv") != std::string::npos
				&& fragmentBody.find ("openfl_ColorMultiplierv") != std::string::npos
				&& vertexBody.find ("openfl_ColorMultiplierv =") == std::string::npos) fixMultiplier = true;

			if (vertexBody.find ("openfl_ColorOffsetv") != std::string::npos
				&& fragmentBody.find ("openfl_ColorOffsetv") != std::string::npos
				&& vertexBody.find ("openfl_ColorOffsetv =") == std::string::npos) fixOffset = true;

		}

		bool needsFixup = fixAlpha || fixCoord || fixMultiplier || fixOffset;

		if (needsFixup) {

			AppendLog (log, "the vertex shader does not write every OpenFL varying; a compat prologue was added");

			vertex += "#define main lime_vulkan_shaderMain\n";

		}

		vertex += "\n";
		vertex += vertexScan.body;
		vertex += "\n";

		if (needsFixup) {

			vertex += "#undef main\n";
			vertex += "void main () {\n\n";
			vertex += "\tlime_vulkan_shaderMain ();\n\n";

			if (fixAlpha) vertex += "\topenfl_Alphav = lime_vulkan_Color.a;\n";
			if (fixCoord) vertex += "\topenfl_TextureCoordv = lime_vulkan_TexCoord;\n";
			if (fixMultiplier) vertex += "\topenfl_ColorMultiplierv = vec4 (lime_vulkan_Color.rgb, 1.0);\n";
			if (fixOffset) vertex += "\topenfl_ColorOffsetv = lime_vulkan_ColorOffset;\n";

			vertex += "\n}\n";

		}

		// ---- Generate the fragment shader ----

		std::string fragment;
		fragment.reserve (fragmentScan.body.size() + 2048);

		fragment += "#version 450\n";

		if (options.openfl) {

			fragment += "#extension GL_EXT_shader_non_constant_global_initializers : enable\n";

		}

		if (options.injectPrecision) {

			fragment += "precision highp float;\nprecision highp int;\n\n";

		}

		AppendUniformBlock (fragment, out->uniforms);

		for (size_t i = 0; i < out->varyings.size(); ++i) {

			AppendLocatedDeclaration (fragment, out->varyings[i], "in", out->varyings[i].location);

		}

		emitted.clear();

		for (size_t i = 0; i < fragmentScan.samplers.size(); ++i) {

			if (HasName (emitted, fragmentScan.samplers[i])) continue;

			emitted.push_back (fragmentScan.samplers[i]);

			char line[192];
			snprintf (line, sizeof (line), "layout(set = 0, binding = %d) uniform sampler2D %s;\n",
				samplerBinding[fragmentScan.samplers[i]], fragmentScan.samplers[i].c_str());
			fragment += line;

		}

		if (fragmentScan.usesFragColor || mentionsFragColor) {

			fragment += "layout(location = 0) out vec4 lime_vulkan_FragColor;\n";

		} else {

			AppendLog (log, "the fragment shader never writes gl_FragColor");

		}

		fragment += "\n";
		fragment += kFragmentTextureWrappers;
		fragment += kFragmentColorDefine;
		fragment += "\n";
		fragment += fragmentScan.body;
		fragment += "\n";

		out->vertex = vertex;
		out->fragment = fragment;
		out->log = log;
		out->ok = !fatal;

		return out->ok;

	}

	std::string Describe (const Translated& translated) {

		std::string text;
		char line[256];

		snprintf (line, sizeof (line), "uniform block: %u bytes\n", translated.uniformSize);
		text += line;

		for (size_t i = 0; i < translated.uniforms.size(); ++i) {

			const Variable& variable = translated.uniforms[i];

			snprintf (line, sizeof (line), "  +%-4u %-10s %s\n", variable.offset, TypeName (variable.type),
				variable.name.c_str());
			text += line;

		}

		for (size_t i = 0; i < translated.samplers.size(); ++i) {

			snprintf (line, sizeof (line), "  binding %-2d sampler2D %s\n", translated.samplers[i].location,
				translated.samplers[i].name.c_str());
			text += line;

		}

		for (size_t i = 0; i < translated.attributes.size(); ++i) {

			snprintf (line, sizeof (line), "  attr %-2d %-10s %s\n", translated.attributes[i].location,
				TypeName (translated.attributes[i].type), translated.attributes[i].name.c_str());
			text += line;

		}

		for (size_t i = 0; i < translated.varyings.size(); ++i) {

			snprintf (line, sizeof (line), "  vary %-2d %-10s %s\n", translated.varyings[i].location,
				TypeName (translated.varyings[i].type), translated.varyings[i].name.c_str());
			text += line;

		}

		return text;

	}

}
}
