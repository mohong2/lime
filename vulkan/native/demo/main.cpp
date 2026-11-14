// lime-vulkan standalone demo: a plain Win32 window plus our own Vulkan backend.
//
// The point of the demo is to be visible:
//   * it depends on nothing but lime_vulkan_* itself, no lime, no SDL, no OpenFL;
//   * the image is real Vulkan output, not a placeholder;
//   * the shaders are SPIR-V built from native/shaders/*.vert|frag (headers
//     generated at build time).
//
// Scenes:
//   --scene=pattern   spinning triangle / full screen gradient
//   --scene=batch     N textured sprites through the 2D batch, 10000 by default
//
// Usage:
//   lime-vulkan-demo.exe                          pattern scene, ESC quits
//   lime-vulkan-demo.exe --scene=batch            ten thousand sprites
//   lime-vulkan-demo.exe --scene=batch --quads=50000
//   lime-vulkan-demo.exe --frames=120             exit after 120 frames (CI and screenshots)
//   lime-vulkan-demo.exe --verbose                detailed log

#include <lime_vulkan.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static bool g_running = true;
static bool g_probe = false;
static bool g_resized = false;
static int g_width = 0;
static int g_height = 0;

static LRESULT CALLBACK WindowProc (HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {

	switch (message) {

		case WM_SIZE:
			g_width = LOWORD (lparam);
			g_height = HIWORD (lparam);
			g_resized = true;
			return 0;

		case WM_KEYDOWN:
			if (wparam == VK_ESCAPE) {
				g_running = false;
				return 0;
			}
			break;

		case WM_CLOSE:
			g_running = false;
			return 0;

		case WM_DESTROY:
			PostQuitMessage (0);
			return 0;

		case WM_PAINT: {

			PAINTSTRUCT paint;
			BeginPaint (hwnd, &paint);
			EndPaint (hwnd, &paint);
			return 0;

		}

	}

	return DefWindowProcA (hwnd, message, wparam, lparam);

}

// Picks the error= line out of the status text, so the dialog shows the reason instead of a screen of key=value.
static const char* ExtractError (const char* status) {

	static char line[512];
	line[0] = 0;

	const char* cursor = status;

	while (cursor && *cursor) {

		const char* end = strchr (cursor, '\n');
		size_t length = end ? (size_t)(end - cursor) : strlen (cursor);

		if (length > 6 && strncmp (cursor, "error=", 6) == 0) {

			size_t copy = length - 6;
			if (copy > sizeof (line) - 1) copy = sizeof (line) - 1;
			memcpy (line, cursor + 6, copy);
			line[copy] = 0;

			return line;

		}

		cursor = end ? end + 1 : 0;

	}

	return "unknown";

}

// ---------------------------------------------------------------------------
// Batch scene: one procedurally generated sprite plus N instances
// ---------------------------------------------------------------------------

static const int kTextureSize = 128;
static unsigned char g_texturePixels[kTextureSize * kTextureSize * 4];

/** Builds a sprite with a soft edged circle and a white ring, so scaling, alpha blending and sampling are visible at a glance. */
static void BuildDemoTexture () {

	for (int y = 0; y < kTextureSize; ++y) {

		for (int x = 0; x < kTextureSize; ++x) {

			float dx = ((float)x + 0.5f) / (float)kTextureSize * 2.0f - 1.0f;
			float dy = ((float)y + 0.5f) / (float)kTextureSize * 2.0f - 1.0f;
			float radius = sqrtf (dx * dx + dy * dy);

			float alpha = 1.0f - radius;
			if (alpha < 0.0f) alpha = 0.0f;
			if (alpha > 1.0f) alpha = 1.0f;
			alpha *= alpha;

			float glow = 1.0f - radius * 0.55f;
			if (glow < 0.0f) glow = 0.0f;

			float red = glow;
			float green = glow * 0.82f + 0.18f;
			float blue = glow * 0.55f + 0.45f;

			float ring = fabsf (radius - 0.86f);

			if (ring < 0.05f) {

				red = 1.0f;
				green = 1.0f;
				blue = 1.0f;
				alpha = 1.0f;

			}

			unsigned char* pixel = g_texturePixels + ((size_t)y * kTextureSize + x) * 4;
			pixel[0] = (unsigned char)(red * 255.0f);
			pixel[1] = (unsigned char)(green * 255.0f);
			pixel[2] = (unsigned char)(blue * 255.0f);
			pixel[3] = (unsigned char)(alpha * 255.0f);

		}

	}

}

/** Scene layout: scattered at random (stress test) or a polar spiral (visible shape, used for screenshots). */
enum {

	LAYOUT_SPIRAL = 0,
	LAYOUT_RANDOM = 1

};

static int g_layout = LAYOUT_SPIRAL;

// ---------------------------------------------------------------------------
// Baseline scenes for the perf harness
//
// 'empty' only clears, 'sprites' pushes ten thousand quads through one batch, and
// 'cameras' mirrors what a game frame actually costs: three camera layers, each
// drawn offscreen and blurred with two extra passes before they are composited.
// The three together are what PERF.md tracks, so a change is always measured
// against the same shapes rather than against a screenshot that "feels slower".
// ---------------------------------------------------------------------------

static bool g_camerasScene = false;
static bool g_texAllocScene = false;
static int g_texAllocCount = 1000;
static int g_texAllocSize = 512;
static lime_vulkan_texture_handle* g_texAllocHandles = 0;
static int g_cameraSprites = 1500;
static bool g_perfOutput = false;
static const char* g_sceneName = "empty";

static lime_vulkan_texture_handle g_cameraTargets[3] = { 0, 0, 0 };
static lime_vulkan_texture_handle g_blurScratch[3] = { 0, 0, 0 };
static lime_vulkan_program g_blurProgram = 0;

// The vertex shader every OpenFL sprite program uses. Kept inline so the scene
// needs no extra file next to the executable.
static const char* kCameraVertex =
	"#pragma header\n"
	"attribute float alpha;\n"
	"attribute vec4 colorMultiplier;\n"
	"attribute vec4 colorOffset;\n"
	"uniform bool hasColorTransform;\n"
	"void main(void) {\n"
	"\t#pragma body\n"
	"\topenfl_Alphav = openfl_Alpha * alpha;\n"
	"\tif (hasColorTransform) {\n"
	"\t\topenfl_ColorOffsetv = colorOffset / 255.0;\n"
	"\t\topenfl_ColorMultiplierv = colorMultiplier;\n"
	"\t}\n"
	"}\n";

// Five taps through linear filtering, nine samples: a real blur, not a box.
static const char* kBlurFragment =
	"#pragma header\n"
	"uniform vec2 uDir;\n"
	"void main(void) {\n"
	"\tvec2 uv = openfl_TextureCoordv;\n"
	"\tvec4 sum = texture2D(bitmap, uv) * 0.227027;\n"
	"\tsum += (texture2D(bitmap, uv + uDir * 1.384615) + texture2D(bitmap, uv - uDir * 1.384615)) * 0.316216;\n"
	"\tsum += (texture2D(bitmap, uv + uDir * 3.230769) + texture2D(bitmap, uv - uDir * 3.230769)) * 0.070270;\n"
	"\tgl_FragColor = sum * openfl_Alphav;\n"
	"}\n";

/** Reads a 'key=value' line out of the stats text. Returns false when the key is absent. */
static bool StatField (const char* stats, const char* key, char* out, size_t capacity) {

	size_t keyLength = strlen (key);
	const char* cursor = stats;

	while (cursor && *cursor) {

		if (strncmp (cursor, key, keyLength) == 0 && cursor[keyLength] == '=') {

			const char* value = cursor + keyLength + 1;
			const char* end = strchr (value, '\n');
			size_t length = end ? (size_t)(end - value) : strlen (value);

			if (length >= capacity) length = capacity - 1;

			memcpy (out, value, length);
			out[length] = 0;
			return true;

		}

		const char* newline = strchr (cursor, '\n');
		cursor = newline ? newline + 1 : 0;

	}

	return false;

}

static double StatNumber (const char* stats, const char* key);

/** Creates N same-size textures and reports what the allocator did, then keeps them. */
static void CreateTexAllocScene () {

	g_texAllocHandles = (lime_vulkan_texture_handle*)calloc ((size_t)g_texAllocCount, sizeof (lime_vulkan_texture_handle));

	size_t bytes = (size_t)g_texAllocSize * (size_t)g_texAllocSize * 4u;
	unsigned char* pixels = (unsigned char*)malloc (bytes);

	for (size_t i = 0; i < bytes; i += 4) {

		pixels[i + 0] = (unsigned char)(i & 0xFF);
		pixels[i + 1] = (unsigned char)((i >> 8) & 0xFF);
		pixels[i + 2] = 0x40;
		pixels[i + 3] = 0xFF;

	}

	int created = 0;

	for (int i = 0; i < g_texAllocCount; ++i) {

		g_texAllocHandles[i] = lime_vulkan_texture_create (pixels, g_texAllocSize, g_texAllocSize, LIME_VULKAN_TEXTURE_NONE);

		if (g_texAllocHandles[i]) created++;

	}

	free (pixels);

	printf ("[demo] texalloc created=%d of %d (%dx%d)\n", created, g_texAllocCount, g_texAllocSize, g_texAllocSize);

	char stats[4096] = { 0 };
	lime_vulkan_batch_stats (stats, sizeof (stats));

	printf ("[demo] texalloc memAllocs=%d memReserved=%.1fMB memTex=%.1fMB\n",
		(int)StatNumber (stats, "memAllocs"), StatNumber (stats, "memReserved"), StatNumber (stats, "memTex"));

}

static void DestroyTexAllocScene () {

	if (!g_texAllocHandles) return;

	for (int i = 0; i < g_texAllocCount; ++i) {

		if (g_texAllocHandles[i]) lime_vulkan_texture_destroy (g_texAllocHandles[i]);

		g_texAllocHandles[i] = 0;

	}

	free (g_texAllocHandles);
	g_texAllocHandles = 0;

	char stats[4096] = { 0 };
	lime_vulkan_batch_stats (stats, sizeof (stats));

	printf ("[demo] texalloc after destroy memAllocs=%d memReserved=%.1fMB memTex=%.1fMB\n",
		(int)StatNumber (stats, "memAllocs"), StatNumber (stats, "memReserved"), StatNumber (stats, "memTex"));

}

static double StatNumber (const char* stats, const char* key) {

	char value[64];

	if (!StatField (stats, key, value, sizeof (value))) return 0.0;

	return atof (value);

}

/**
 * Position, size and tint of every sprite derive from its index: a frame is
 * always identical, which is what makes pixel comparison possible.
 * Fully synchronous, no random numbers, no allocations.
 */
static void DrawSprite (int index, float time, int screenWidth, int screenHeight) {

	float seed = (float)index;
	float size = 10.0f + fmodf (seed * 13.0f, 34.0f);
	float px = 0.0f;
	float py = 0.0f;

	if (g_layout == LAYOUT_RANDOM) {

		float speedX = 20.0f + fmodf (seed * 7.0f, 60.0f);
		float speedY = 12.0f + fmodf (seed * 3.7f, 40.0f);

		float spanX = (float)screenWidth + 160.0f;
		float spanY = (float)screenHeight + 160.0f;

		px = fmodf (seed * 37.0f + time * speedX, spanX) - 80.0f;
		py = fmodf (seed * 91.0f + time * speedY, spanY) - 80.0f;

	} else {

		// Galaxy: a sqrt radius distribution keeps the area even, so the centre does not pile up into a white blob
		float radius = 30.0f + sqrtf (fmodf (seed * 0.37f, 1.0f)) * 520.0f;
		float angle = seed * 2.39996f + time * (0.35f + radius * 0.0012f);

		px = (float)screenWidth * 0.5f + cosf (angle) * radius - size * 0.5f;
		py = (float)screenHeight * 0.5f + sinf (angle) * radius * 0.62f - size * 0.5f;

		size *= 0.55f + 0.45f * (radius / 550.0f);

	}

	static const unsigned int palette[8] = {
		0xFFFF6B6B, 0xFF4ECDC4, 0xFFFFE66D, 0xFF95E1D3,
		0xFFF38181, 0xFFAA96DA, 0xFFFCBAD3, 0xFFA8D8EA
	};

	unsigned int tint = palette[index & 7];

	// Alpha varies with the index so the difference between blend modes shows;
	// capped at 0x60 so ten thousand stacked sprites do not blow out to white.
	unsigned int alpha = 0x20 + (unsigned int)(fmodf (seed * 17.0f, 0x40));
	tint = (alpha << 24) | (tint & 0x00FFFFFFu);

	lime_vulkan_batch_quad (px, py, size, size, 0.0f, 0.0f, 1.0f, 1.0f, tint);

}

/**
 * Pixel self test for the blend modes the fixed function unit can express:
 * subtract, darken and lighten. Four bands over a mid grey clear, each with a
 * colour whose expected result is a single number:
 *
 *   band 0  darken   red    on grey 128 -> (128,   0,   0)
 *   band 1  lighten  red    on grey 128 -> (255, 128,   0)
 *   band 2  subtract red    on grey 128 -> (  0, 128, 128)   dst - src, clamped
 *   band 3  darken   black  on grey 128 -> (  0,   0,   0)
 */
/** 1x1 texture with nothing in it, for the colour offset band. */
static lime_vulkan_texture_handle g_emptyTexture = 0;

static bool DrawBlendScene () {

	int screenWidth = g_width;
	int screenHeight = g_height;
	int band = screenHeight / 6;

	if (g_emptyTexture == 0) {

		const unsigned char empty[4] = { 0, 0, 0, 0 };
		g_emptyTexture = lime_vulkan_texture_create (empty, 1, 1, LIME_VULKAN_TEXTURE_NEAREST);

	}

	const float grey[4] = { 0.5f, 0.5f, 0.5f, 1.0f };

	if (!lime_vulkan_batch_begin (screenWidth, screenHeight, grey)) return false;

	// Handle 0 is the 1x1 white texture, so the quad colour is the fragment colour.
	lime_vulkan_batch_set_texture (0);

	lime_vulkan_batch_set_blend (LIME_VULKAN_BLEND_DARKEN);
	lime_vulkan_batch_quad (0.0f, 0.0f, (float)screenWidth, (float)band, 0.0f, 0.0f, 1.0f, 1.0f, 0xFFFF0000u);

	lime_vulkan_batch_set_blend (LIME_VULKAN_BLEND_LIGHTEN);
	lime_vulkan_batch_quad (0.0f, (float)band, (float)screenWidth, (float)band, 0.0f, 0.0f, 1.0f, 1.0f, 0xFFFF0000u);

	lime_vulkan_batch_set_blend (LIME_VULKAN_BLEND_SUBTRACT);
	lime_vulkan_batch_quad (0.0f, (float)(band * 2), (float)screenWidth, (float)band, 0.0f, 0.0f, 1.0f, 1.0f, 0xFFFF0000u);

	lime_vulkan_batch_set_blend (LIME_VULKAN_BLEND_DARKEN);
	lime_vulkan_batch_quad (0.0f, (float)(band * 3), (float)screenWidth, (float)band, 0.0f, 0.0f, 1.0f, 1.0f, 0xFF000000u);

	// Colour offsets (colorTransform.redOffset and friends) are additive, and the
	// texture is premultiplied: an offset added without scaling by alpha turns a
	// fully transparent texel into a solid block of the offset colour. Bands 4 and
	// 5 cover both halves of that rule.
	lime_vulkan_batch_set_blend (LIME_VULKAN_BLEND_NORMAL);

	const double rect[4] = { 0.0, 0.0, 1.0, 1.0 };
	double matrix[6] = { (double)screenWidth, 0.0, 0.0, (double)band, 0.0, (double)(band * 4) };
	double multipliers[16];
	double offsets[16];

	for (int i = 0; i < 4; ++i) {

		multipliers[i * 4 + 0] = 1.0;
		multipliers[i * 4 + 1] = 1.0;
		multipliers[i * 4 + 2] = 1.0;
		multipliers[i * 4 + 3] = 1.0;
		offsets[i * 4 + 0] = 255.0;
		offsets[i * 4 + 1] = 255.0;
		offsets[i * 4 + 2] = 255.0;
		offsets[i * 4 + 3] = 0.0;

	}

	lime_vulkan_batch_set_texture (g_emptyTexture);
	lime_vulkan_batch_quads_d (rect, matrix, 1, NULL, 1.0f, multipliers, offsets);

	// The same offset on an opaque texel has to apply: white minus 255 is black.
	for (int i = 0; i < 4; ++i) {

		offsets[i * 4 + 0] = -255.0;
		offsets[i * 4 + 1] = -255.0;
		offsets[i * 4 + 2] = -255.0;

	}

	matrix[5] = (double)(band * 5);
	lime_vulkan_batch_set_texture (0);
	lime_vulkan_batch_quads_d (rect, matrix, 1, NULL, 1.0f, multipliers, offsets);

	return lime_vulkan_batch_end () != 0;

}

/**
 * Pixel self test: four horizontal bands, each checking one thing. The colours
 * are exactly what is written, with no antialiasing and no gradient, so
 * tests/test_batch_pixels.py can assert the numbers directly.
 *
 *   band 0  red 0xFFFF0000, NORMAL, full width       -> expect (255,   0,   0)
 *   band 1  green 0x8000FF00, alpha 50%, full width  -> expect (  0, 128,   0)  [black underneath]
 *   band 2  blue 0xFF0000FF, scissored to the middle -> the sides must stay black
 *   band 3  white 0x40FFFFFF, ADD                    -> expect ( 64,  64,  64)
 */
static bool DrawTestScene (lime_vulkan_texture_handle texture) {

	int screenWidth = g_width;
	int screenHeight = g_height;
	int band = screenHeight / 4;

	const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

	if (!lime_vulkan_batch_begin (screenWidth, screenHeight, black)) return false;

	// Handle 0 selects the built-in 1x1 white texture, which draws a flat colour:
	// the self test needs a known colour, not the soft edged demo sprite.
	(void)texture;
	lime_vulkan_batch_set_texture (0);
	lime_vulkan_batch_set_blend (LIME_VULKAN_BLEND_NORMAL);

	lime_vulkan_batch_quad (0.0f, 0.0f, (float)screenWidth, (float)band, 0.0f, 0.0f, 1.0f, 1.0f, 0xFFFF0000u);
	lime_vulkan_batch_quad (0.0f, (float)band, (float)screenWidth, (float)band, 0.0f, 0.0f, 1.0f, 1.0f, 0x8000FF00u);

	lime_vulkan_batch_set_scissor (screenWidth / 4, band * 2, screenWidth / 2, band);
	lime_vulkan_batch_quad (0.0f, (float)(band * 2), (float)screenWidth, (float)band, 0.0f, 0.0f, 1.0f, 1.0f, 0xFF0000FFu);
	lime_vulkan_batch_set_scissor (0, 0, 0, 0);

	lime_vulkan_batch_set_blend (LIME_VULKAN_BLEND_ADD);
	lime_vulkan_batch_quad (0.0f, (float)(band * 3), (float)screenWidth, (float)(screenHeight - band * 3), 0.0f, 0.0f, 1.0f, 1.0f, 0x40FFFFFFu);

	return lime_vulkan_batch_end () != 0;

}

/**
 * Coordinate probe: five bands with no scissor and no blend change, to check the
 * pixel space to screen mapping on its own. Red on top, white at the bottom,
 * green, blue and yellow in between, so the order is readable at a glance.
 */
static bool DrawProbeScene () {

	int screenWidth = g_width;
	int screenHeight = g_height;
	int band = screenHeight / 5;

	const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

	if (!lime_vulkan_batch_begin (screenWidth, screenHeight, black)) return false;

	lime_vulkan_batch_set_texture (0);
	lime_vulkan_batch_set_blend (LIME_VULKAN_BLEND_NONE);   // plain overwrite, alpha does not apply

	const unsigned int colors[5] = { 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFF00u, 0xFFFFFFFFu };

	for (int i = 0; i < 5; ++i) {

		lime_vulkan_batch_quad (0.0f, (float)(band * i), (float)screenWidth, (float)band,
			0.0f, 0.0f, 1.0f, 1.0f, colors[i]);

	}

	return lime_vulkan_batch_end () != 0;

}

static bool DrawBatchScene (float time, lime_vulkan_texture_handle texture, int quads) {

	int screenWidth = g_width;
	int screenHeight = g_height;

	const float clearColor[4] = { 0.04f, 0.04f, 0.07f, 1.0f };

	if (!lime_vulkan_batch_begin (screenWidth, screenHeight, clearColor)) return false;

	lime_vulkan_batch_set_texture (texture);

	int normal = (int)((long long)quads * 85 / 100);
	int add = (int)((long long)quads * 10 / 100);

	lime_vulkan_batch_set_blend (LIME_VULKAN_BLEND_NORMAL);

	for (int i = 0; i < normal; ++i) DrawSprite (i, time, screenWidth, screenHeight);

	lime_vulkan_batch_set_blend (LIME_VULKAN_BLEND_ADD);

	for (int i = normal; i < normal + add; ++i) DrawSprite (i, time, screenWidth, screenHeight);

	lime_vulkan_batch_set_blend (LIME_VULKAN_BLEND_MULTIPLY);

	for (int i = normal + add; i < quads; ++i) DrawSprite (i, time, screenWidth, screenHeight);

	return lime_vulkan_batch_end () != 0;

}


// ---------------------------------------------------------------------------
// flixel batch scene
//
// What differs from DrawBatchScene is the data shape: this goes through
// lime_vulkan_batch_quads_d, which is exactly what flixel's FlxDrawQuadsItem hands to the
// renderer: a source rectangle, a 2D matrix per quad, optional per-vertex alpha
// and colour multipliers, all inside a camera canvas matrix.
//
// The quad times canvas transform is pinned down before the engine is involved,
// because it is the one place that looks right while being wrong: swapping the
// order shrinks everything into a corner, a missing divide by the texture size
// samples edge pixels, and a flipped y axis turns the image upside down.
//
// The scene is assertable (client area 1280x720, texture 128x128 with four
// quadrants, view matrix (2,0,0,2, 64,32)):
//   Q0  source (0,0,128,128)  identity           -> screen (64,32)-(320,288), four quadrants
//   Q1  source (0,0,64,64)    translate (128,0)  -> screen (320,32)-(448,160), solid red
//   Q2  source (64,0,64,64)   translate (400,200)-> screen (992,432)-(1120,560), solid green
//       Q2 uses the per-vertex alpha and colour multiplier array branch (all
//       identity values, but the branch really runs)
// ---------------------------------------------------------------------------

static bool DrawQuadsScene (lime_vulkan_texture_handle texture) {

	const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

	if (!lime_vulkan_batch_begin (g_width, g_height, clearColor)) return false;

	lime_vulkan_batch_set_texture (texture);
	lime_vulkan_batch_set_blend (LIME_VULKAN_BLEND_NORMAL);

	const double view[6] = { 2.0, 0.0, 0.0, 2.0, 64.0, 32.0 };
	lime_vulkan_batch_set_view (view);

	{
		const double rects[4] = { 0.0, 0.0, (double)kTextureSize, (double)kTextureSize };
		const double transforms[6] = { 1.0, 0.0, 0.0, 1.0, 0.0, 0.0 };

		lime_vulkan_batch_quads_d (rects, transforms, 1, NULL, 1.0f, NULL, NULL);

	}

	{
		const double half = (double)kTextureSize * 0.5;
		const double rects[4] = { 0.0, 0.0, half, half };
		const double transforms[6] = { 1.0, 0.0, 0.0, 1.0, 128.0, 0.0 };

		lime_vulkan_batch_quads_d (rects, transforms, 1, NULL, 1.0f, NULL, NULL);

	}

	{
		const double half = (double)kTextureSize * 0.5;
		const double rects[4] = { half, 0.0, half, half };
		const double transforms[6] = { 1.0, 0.0, 0.0, 1.0, 400.0, 200.0 };
		const double alphas[4] = { 1.0, 1.0, 1.0, 1.0 };
		const double multipliers[16] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
		const double offsets[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

		lime_vulkan_batch_quads_d (rects, transforms, 1, alphas, 1.0f, multipliers, offsets);

	}

	lime_vulkan_batch_set_view (NULL);

	return lime_vulkan_batch_end () != 0;

}

// ---------------------------------------------------------------------------
// Shader scene
//
// One quad with a GLSL program compiled at runtime. The texture has four opaque
// quadrants (red top left, green top right, blue bottom left, white bottom
// right) so every pixel can be asserted.
// ---------------------------------------------------------------------------

static const char* g_shaderVertexPath = NULL;
static const char* g_shaderFragmentPath = NULL;
static const char* g_shaderCache = NULL;

static lime_vulkan_program g_program = 0;
static lime_vulkan_texture_handle g_shaderTexture = 0;
static bool g_shaderScene = false;
static bool g_quadsScene = false;
static int g_shaderBlend = LIME_VULKAN_BLEND_NORMAL;
static unsigned int g_shaderTint = 0xFFFFFFFFu;
static unsigned int g_shaderFlags = LIME_VULKAN_PROGRAM_DEFAULT;
static int g_quadRect[4] = { 64, 64, 192, 192 };
static int g_shaderSprites = 1;

/** Automated runs must not show a modal dialog: it parks the process and keeps the exe locked for the next link. */
static bool g_noDialog = false;

struct DemoUniform {

	char name[64];
	float values[16];
	int count;

};

static DemoUniform g_uniforms[32];
static int g_uniformCount = 0;

static bool ReadWholeTextFile (const char* path, char** out) {

	FILE* file = NULL;

	if (fopen_s (&file, path, "rb") != 0 || !file) return false;

	fseek (file, 0, SEEK_END);
	long size = ftell (file);
	fseek (file, 0, SEEK_SET);

	if (size < 0) {

		fclose (file);
		return false;

	}

	char* text = (char*)malloc ((size_t)size + 1);

	if (!text) {

		fclose (file);
		return false;

	}

	size_t read = size > 0 ? fread (text, 1, (size_t)size, file) : 0;
	text[read] = 0;

	fclose (file);

	*out = text;
	return true;

}

static void BuildQuadrantTexture () {

	for (int y = 0; y < kTextureSize; ++y) {

		for (int x = 0; x < kTextureSize; ++x) {

			bool left = x < kTextureSize / 2;
			bool top = y < kTextureSize / 2;

			unsigned char red = 0;
			unsigned char green = 0;
			unsigned char blue = 0;

			if (top && left) red = 255;
			else if (top && !left) green = 255;
			else if (!top && left) blue = 255;
			else { red = 255; green = 255; blue = 255; }

			unsigned char* pixel = g_texturePixels + ((size_t)y * kTextureSize + x) * 4;
			pixel[0] = red;
			pixel[1] = green;
			pixel[2] = blue;
			pixel[3] = 255;

		}

	}

}

static void ParseUniformSpec (const char* text) {

	if (g_uniformCount >= 32) return;

	DemoUniform& uniform = g_uniforms[g_uniformCount];

	const char* equals = strchr (text, '=');

	if (!equals) {

		printf ("[demo] --uniform needs name=value, got '%s'\n", text);
		return;

	}

	size_t length = (size_t)(equals - text);

	if (length >= sizeof (uniform.name)) length = sizeof (uniform.name) - 1;

	memcpy (uniform.name, text, length);
	uniform.name[length] = 0;

	uniform.count = 0;

	const char* cursor = equals + 1;

	while (*cursor && uniform.count < 16) {

		char* end = NULL;
		float value = (float)strtod (cursor, &end);

		if (end == cursor) break;

		uniform.values[uniform.count++] = value;
		cursor = end;

		while (*cursor == ',' || *cursor == ' ') ++cursor;

	}

	g_uniformCount++;

}

/** Writes a uniform by its reflected type: arrays use float_array, vectors vecN and bools int. */
static void ApplyUniform (lime_vulkan_program program, const char* name, const float* values, int count) {

	int total = lime_vulkan_program_uniform_count (program);

	for (int i = 0; i < total; ++i) {

		lime_vulkan_uniform_info info;
		memset (&info, 0, sizeof (info));

		if (!lime_vulkan_program_uniform_info (program, i, &info)) continue;
		if (strcmp (info.name, name) != 0) continue;

		if (info.count > 0) {

			lime_vulkan_program_set_float_array (program, name, values, count);
			return;

		}

		switch (info.type) {

			case LIME_VULKAN_UNIFORM_FLOAT:
				lime_vulkan_program_set_float (program, name, values[0]);
				break;

			case LIME_VULKAN_UNIFORM_INT:
			case LIME_VULKAN_UNIFORM_BOOL:
				lime_vulkan_program_set_int (program, name, (int)values[0]);
				break;

			case LIME_VULKAN_UNIFORM_VEC2:
				lime_vulkan_program_set_vec2 (program, name, values);
				break;

			case LIME_VULKAN_UNIFORM_VEC3:
				lime_vulkan_program_set_vec3 (program, name, values);
				break;

			case LIME_VULKAN_UNIFORM_VEC4:
				lime_vulkan_program_set_vec4 (program, name, values);
				break;

			case LIME_VULKAN_UNIFORM_MAT4:
				lime_vulkan_program_set_mat4 (program, name, values);
				break;

			default:
				break;

		}

		return;

	}

	printf ("[demo] uniform '%s' is not declared by the shader\n", name);

}

static void ApplyAllUniforms (float time) {

	if (!g_program) return;

	lime_vulkan_program_set_float (g_program, "iTime", time);
	lime_vulkan_program_set_float (g_program, "uTime", time);

	float resolution[2] = { (float)g_width, (float)g_height };
	lime_vulkan_program_set_vec2 (g_program, "iResolution", resolution);

	for (int i = 0; i < g_uniformCount; ++i) {

		ApplyUniform (g_program, g_uniforms[i].name, g_uniforms[i].values, g_uniforms[i].count);

	}

}

static bool DrawShaderScene (float time) {

	const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

	if (!lime_vulkan_batch_begin (g_width, g_height, clearColor)) return false;

	lime_vulkan_batch_set_texture (g_shaderTexture);
	lime_vulkan_batch_set_blend (g_shaderBlend);
	lime_vulkan_batch_set_program (g_program);

	ApplyAllUniforms (time);

	if (g_shaderSprites <= 1) {

		lime_vulkan_batch_quad ((float)g_quadRect[0], (float)g_quadRect[1],
			(float)g_quadRect[2], (float)g_quadRect[3], 0.0f, 0.0f, 1.0f, 1.0f, g_shaderTint);

	} else {

		// Stress test: one program drawing N sprites. The texture never changes and
		// the whole batch is one draw call, so what is measured is the shader cost,
		// not the number of submissions.
		for (int i = 0; i < g_shaderSprites; ++i) {

			DrawSprite (i, time, g_width, g_height);

		}

	}

	lime_vulkan_batch_set_program (0);

	return lime_vulkan_batch_end () != 0;

}

static void ApplyBlurDirection (const float direction[2]) {

	lime_vulkan_program_set_vec2 (g_blurProgram, "uDir", direction);

}

static bool CreateBlurProgram () {

	lime_vulkan_program_desc desc;
	memset (&desc, 0, sizeof (desc));
	desc.vertexSource = kCameraVertex;
	desc.fragmentSource = kBlurFragment;
	desc.flags = 0;
	desc.cacheDirectory = g_shaderCache;

	g_blurProgram = lime_vulkan_program_create (&desc);

	if (!g_blurProgram) {

		char status[1024] = { 0 };
		lime_vulkan_status (status, sizeof (status));
		printf ("[demo] blur program failed: %s\n", ExtractError (status));
		return false;

	}

	return true;

}

/** Three camera layers with a blur on each, the shape a game frame actually has. */
static bool CreateCamerasScene () {

	if (!CreateBlurProgram ()) return false;

	for (int i = 0; i < 3; ++i) {

		g_cameraTargets[i] = lime_vulkan_target_create (g_width, g_height);
		g_blurScratch[i] = lime_vulkan_target_create (g_width, g_height);

		if (!lime_vulkan_target_valid (g_cameraTargets[i]) || !lime_vulkan_target_valid (g_blurScratch[i])) {

			char status[512] = { 0 };
			lime_vulkan_status (status, sizeof (status));
			printf ("[demo] could not create the camera targets: %s\n", ExtractError (status));
			return false;

		}

	}

	printf ("[demo] scene=cameras targets=3x2 sprites=%d\n", g_cameraSprites);
	return true;

}

static void DestroyCamerasScene () {

	for (int i = 0; i < 3; ++i) {

		if (g_cameraTargets[i]) lime_vulkan_texture_destroy (g_cameraTargets[i]);
		if (g_blurScratch[i]) lime_vulkan_texture_destroy (g_blurScratch[i]);

		g_cameraTargets[i] = 0;
		g_blurScratch[i] = 0;

	}

	if (g_blurProgram) lime_vulkan_program_destroy (g_blurProgram);
	g_blurProgram = 0;

}

/**
 * One camera layer: draw the sprites offscreen with a panning view matrix, blur
 * it horizontally and vertically, then composite it into the window.
 */
static void DrawCameraLayer (int index, float time, lime_vulkan_texture_handle spriteTexture) {

	int width = g_width;
	int height = g_height;

	// Each camera pans at its own speed so the three layers never line up.
	double pan = (double)time * (8.0 + index * 5.0);

	double view[6] = { 1.0, 0.0, 0.0, 1.0, -pan, 0.0 };

	// ---- sprites, offscreen ----
	lime_vulkan_batch_begin_target (g_cameraTargets[index], 0x00000000);

	lime_vulkan_batch_set_view (view);
	lime_vulkan_batch_set_program (0);
	lime_vulkan_batch_set_texture (spriteTexture);
	lime_vulkan_batch_set_blend (LIME_VULKAN_BLEND_NORMAL);

	for (int i = 0; i < g_cameraSprites; ++i) {

		DrawSprite (i + index * 4096, time, width, height);

	}

	lime_vulkan_batch_end_target ();

	// ---- blur, horizontal ----
	double identity[6] = { 1.0, 0.0, 0.0, 1.0, 0.0, 0.0 };
	float horizontal[2] = { 1.0f / (float)width, 0.0f };

	lime_vulkan_batch_begin_target (g_blurScratch[index], 0x00000000);
	lime_vulkan_batch_set_view (identity);
	lime_vulkan_batch_set_program (g_blurProgram);
	lime_vulkan_batch_set_texture (g_cameraTargets[index]);
	lime_vulkan_batch_set_blend (LIME_VULKAN_BLEND_NORMAL);

	ApplyBlurDirection (horizontal);
	lime_vulkan_batch_quad (0.0f, 0.0f, (float)width, (float)height, 0.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFFu);

	lime_vulkan_batch_end_target ();

	// ---- blur, vertical, back into the camera target ----
	float vertical[2] = { 0.0f, 1.0f / (float)height };

	lime_vulkan_batch_begin_target (g_cameraTargets[index], 0x00000000);
	lime_vulkan_batch_set_view (identity);
	lime_vulkan_batch_set_program (g_blurProgram);
	lime_vulkan_batch_set_texture (g_blurScratch[index]);

	ApplyBlurDirection (vertical);
	lime_vulkan_batch_quad (0.0f, 0.0f, (float)width, (float)height, 0.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFFu);

	lime_vulkan_batch_end_target ();

	lime_vulkan_batch_set_program (0);

}

static bool DrawCamerasScene (float time, lime_vulkan_texture_handle spriteTexture) {

	const float clearColor[4] = { 0.02f, 0.02f, 0.05f, 1.0f };

	if (!lime_vulkan_batch_begin (g_width, g_height, clearColor)) return false;

	for (int i = 0; i < 3; ++i) {

		DrawCameraLayer (i, time, spriteTexture);

	}

	// ---- composite: the first layer opaque, the other two added, like a HUD on top ----
	double identity[6] = { 1.0, 0.0, 0.0, 1.0, 0.0, 0.0 };
	lime_vulkan_batch_set_view (identity);
	lime_vulkan_batch_set_program (0);

	for (int i = 0; i < 3; ++i) {

		lime_vulkan_batch_set_texture (g_cameraTargets[i]);
		lime_vulkan_batch_set_blend (i == 0 ? LIME_VULKAN_BLEND_NORMAL : LIME_VULKAN_BLEND_ADD);

		lime_vulkan_batch_quad (0.0f, 0.0f, (float)g_width, (float)g_height,
			0.0f, 0.0f, 1.0f, 1.0f, i == 0 ? 0xFFFFFFFFu : 0x40FFFFFFu);

	}

	return lime_vulkan_batch_end () != 0;

}

/** Builds a program. On failure it prints the translation and compile log, since this path is where things break. */
static bool CreateShaderSceneProgram () {

	if (!g_shaderVertexPath || !g_shaderFragmentPath) {

		printf ("[demo] --scene=shader needs --vert=<file> and --frag=<file>\n");
		return false;

	}

	char* vertexSource = NULL;
	char* fragmentSource = NULL;

	if (!ReadWholeTextFile (g_shaderVertexPath, &vertexSource)) {

		printf ("[demo] could not read %s\n", g_shaderVertexPath);
		return false;

	}

	if (!ReadWholeTextFile (g_shaderFragmentPath, &fragmentSource)) {

		printf ("[demo] could not read %s\n", g_shaderFragmentPath);
		free (vertexSource);
		return false;

	}

	lime_vulkan_program_desc desc;
	memset (&desc, 0, sizeof (desc));
	desc.vertexSource = vertexSource;
	desc.fragmentSource = fragmentSource;
	desc.flags = g_shaderFlags;
	desc.cacheDirectory = g_shaderCache;

	// Program creation includes the GLSL translation and the driver's pipeline
	// compile, which is where a cold start spends its time. Printed so the effect
	// of the on-disk pipeline cache can be measured instead of guessed.
	LARGE_INTEGER programStart, programEnd, programFrequency;
	QueryPerformanceFrequency (&programFrequency);
	QueryPerformanceCounter (&programStart);

	g_program = lime_vulkan_program_create (&desc);

	QueryPerformanceCounter (&programEnd);

	printf ("[demo] program created in %.1f ms\n",
		(double)(programEnd.QuadPart - programStart.QuadPart) * 1000.0 / (double)programFrequency.QuadPart);

	free (vertexSource);
	free (fragmentSource);

	if (!g_program) {

		char status[1024] = { 0 };
		lime_vulkan_status (status, sizeof (status));
		printf ("[demo] program creation failed: %s\n", status);
		return false;

	}

	const char* log = lime_vulkan_program_log (g_program);

	if (log && *log) printf ("[demo] program log:\n%s", log);

	printf ("[demo] program %llu created (uniforms=%d)\n",
		(unsigned long long)g_program, lime_vulkan_program_uniform_count (g_program));

	return true;

}

// ---------------------------------------------------------------------------

int main (int argc, char** argv) {

	setvbuf (stdout, NULL, _IONBF, 0);
	setvbuf (stderr, NULL, _IONBF, 0);

	printf ("[demo] start\n");

	HINSTANCE instance = GetModuleHandleA (NULL);
	int showCommand = SW_SHOW;

	int maxFrames = -1;
	bool verbose = false;
	int deviceIndex = -1;
	int quadCount = 10000;
	bool batchScene = false;
	bool testScene = false;
	int pattern = LIME_VULKAN_PATTERN_TRIANGLE;

	for (int i = 1; i < argc; ++i) {

		const char* value = NULL;

		if (strncmp (argv[i], "--frames=", 9) == 0) maxFrames = atoi (argv[i] + 9);
		else if (strncmp (argv[i], "--device=", 9) == 0) deviceIndex = atoi (argv[i] + 9);
		else if (strncmp (argv[i], "--quads=", 8) == 0) quadCount = atoi (argv[i] + 8);
		else if (strncmp (argv[i], "--scene=", 8) == 0) {

			value = argv[i] + 8;
			batchScene = (strcmp (value, "batch") == 0) || (strcmp (value, "sprites") == 0);
			testScene = (strcmp (value, "test") == 0);
			g_probe = (strcmp (value, "probe") == 0);
			g_shaderScene = (strcmp (value, "shader") == 0);
			g_quadsScene = (strcmp (value, "quads") == 0);
			g_camerasScene = (strcmp (value, "cameras") == 0);
			g_texAllocScene = (strcmp (value, "texalloc") == 0);
			g_sceneName = value;
			if (g_probe) batchScene = true;
			if (g_shaderScene) batchScene = true;
			if (g_quadsScene) batchScene = true;
			if (g_camerasScene) batchScene = true;

			if (testScene) batchScene = true;  // same batch path

		}
		else if (strncmp (argv[i], "--pattern=", 10) == 0) {

			value = argv[i] + 10;
			pattern = (strcmp (value, "gradient") == 0) ? LIME_VULKAN_PATTERN_GRADIENT : LIME_VULKAN_PATTERN_TRIANGLE;

		}
		else if (strncmp (argv[i], "--layout=", 9) == 0) {

			value = argv[i] + 9;
			g_layout = (strcmp (value, "random") == 0) ? LAYOUT_RANDOM : LAYOUT_SPIRAL;

		}
		else if (strncmp (argv[i], "--vert=", 7) == 0) g_shaderVertexPath = argv[i] + 7;
		else if (strncmp (argv[i], "--frag=", 7) == 0) g_shaderFragmentPath = argv[i] + 7;
		else if (strncmp (argv[i], "--cache=", 8) == 0) g_shaderCache = argv[i] + 8;
		else if (strncmp (argv[i], "--blend=", 8) == 0) g_shaderBlend = atoi (argv[i] + 8);
		else if (strncmp (argv[i], "--tint=", 7) == 0) g_shaderTint = (unsigned int)strtoul (argv[i] + 7, NULL, 0);
		else if (strncmp (argv[i], "--uniform=", 10) == 0) ParseUniformSpec (argv[i] + 10);
		else if (strncmp (argv[i], "--quad=", 7) == 0) {

			sscanf_s (argv[i] + 7, "%d,%d,%d,%d", &g_quadRect[0], &g_quadRect[1], &g_quadRect[2], &g_quadRect[3]);

		}
		else if (strncmp (argv[i], "--sprites=", 10) == 0) { g_shaderSprites = atoi (argv[i] + 10); g_cameraSprites = g_shaderSprites; }
		else if (strcmp (argv[i], "--perf") == 0) g_perfOutput = true;
		else if (strncmp (argv[i], "--textures=", 11) == 0) g_texAllocCount = atoi (argv[i] + 11);
		else if (strncmp (argv[i], "--size=", 7) == 0) g_texAllocSize = atoi (argv[i] + 7);
	else if (strcmp (argv[i], "--plain") == 0) g_shaderFlags |= LIME_VULKAN_PROGRAM_PLAIN;
		else if (strcmp (argv[i], "--straight-alpha") == 0) g_shaderFlags |= LIME_VULKAN_PROGRAM_STRAIGHT_ALPHA;
		else if (strcmp (argv[i], "--no-dialog") == 0) g_noDialog = true;
	else if (strcmp (argv[i], "--verbose") == 0) verbose = true;

	}

	if (quadCount < 1) quadCount = 1;
	if (g_texAllocCount < 1) g_texAllocCount = 1;
	if (g_texAllocCount > 4096) g_texAllocCount = 4096;
	if (g_texAllocSize < 16) g_texAllocSize = 16;

	WNDCLASSA windowClass = {};
	windowClass.style = CS_HREDRAW | CS_VREDRAW;
	windowClass.lpfnWndProc = WindowProc;
	windowClass.hInstance = instance;
	windowClass.hCursor = LoadCursorA (NULL, IDC_ARROW);
	windowClass.lpszClassName = "limeVulkanDemo";

	if (!RegisterClassA (&windowClass)) {

		MessageBoxA (NULL, "RegisterClass failed", "lime-vulkan demo", MB_OK | MB_ICONERROR);
		return 1;

	}

	g_width = 1280;
	g_height = 720;

	RECT rect = { 0, 0, g_width, g_height };
	AdjustWindowRect (&rect, WS_OVERLAPPEDWINDOW, FALSE);

	HWND hwnd = CreateWindowExA (0, "limeVulkanDemo", "lime-vulkan demo (ESC to quit)",
		WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
		rect.right - rect.left, rect.bottom - rect.top, NULL, NULL, instance, NULL);

	if (!hwnd) {

		MessageBoxA (NULL, "CreateWindow failed", "lime-vulkan demo", MB_OK | MB_ICONERROR);
		return 1;

	}

	lime_vulkan_options options;
	lime_vulkan_default_options (&options);
	options.verbose = verbose ? 1 : 0;
	options.deviceIndex = deviceIndex;
	options.fallbackDialog = g_noDialog ? 0 : 1;

	printf ("[demo] lime-vulkan version %s, available=%d\n", lime_vulkan_version (), lime_vulkan_available ());
	printf ("[demo] creating context on hwnd %p (%dx%d)\n", (void*)hwnd, g_width, g_height);

	if (!lime_vulkan_context_create ((void*)hwnd, g_width, g_height, &options)) {

		char status[512] = { 0 };
		lime_vulkan_status (status, sizeof (status));

		// lime_vulkan_notify_fallback already logs and shows the dialog (which can be turned off), so no second one here.
		lime_vulkan_notify_fallback (ExtractError (status));

		return 2;

	}

	printf ("[demo] context attached=%d\n", lime_vulkan_context_attached ());

	lime_vulkan_texture_handle texture = 0;

	if (g_shaderScene) {

		BuildQuadrantTexture ();

		g_shaderTexture = lime_vulkan_texture_create (g_texturePixels, kTextureSize, kTextureSize, LIME_VULKAN_TEXTURE_NONE);

		if (g_shaderTexture == 0) {

			char status[512] = { 0 };
			lime_vulkan_status (status, sizeof (status));
			MessageBoxA (hwnd, status, "lime-vulkan demo: texture upload failed", MB_OK | MB_ICONERROR);
			return 3;

		}

		if (!CreateShaderSceneProgram ()) {

			char status[1024] = { 0 };
			lime_vulkan_status (status, sizeof (status));

			// With handle 0, lime_vulkan_program_log returns the details of the last failure,
			// including the translated GLSL. The line numbers in the error refer to that
			// translated source, and without it the original line cannot be found.
			const char* failure = lime_vulkan_program_log (0);

			if (failure && *failure) printf ("[demo] program failure detail:\n%s\n", failure);

			if (!g_noDialog) MessageBoxA (hwnd, status, "lime-vulkan demo: shader program failed", MB_OK | MB_ICONERROR);
			return 4;

		}

		printf ("[demo] scene=shader quad=(%d,%d,%d,%d) blend=%d tint=0x%08X\n",
			g_quadRect[0], g_quadRect[1], g_quadRect[2], g_quadRect[3], g_shaderBlend, g_shaderTint);

	} else if (g_quadsScene) {

		// Four quadrant texture: one solid colour per corner, which is what makes coordinate assertions meaningful
		BuildQuadrantTexture ();

		texture = lime_vulkan_texture_create (g_texturePixels, kTextureSize, kTextureSize, LIME_VULKAN_TEXTURE_NONE);

		if (texture == 0) {

			char status[512] = { 0 };
			lime_vulkan_status (status, sizeof (status));
			MessageBoxA (hwnd, status, "lime-vulkan demo: texture upload failed", MB_OK | MB_ICONERROR);
			return 3;

		}

		printf ("[demo] scene=quads texture=%dx%d view=(2,0,0,2,64,32) quads=3\n", kTextureSize, kTextureSize);

	} else if (g_camerasScene) {

		BuildDemoTexture ();

		texture = lime_vulkan_texture_create (g_texturePixels, kTextureSize, kTextureSize, LIME_VULKAN_TEXTURE_NONE);

		if (texture == 0) {

			char status[512] = { 0 };
			lime_vulkan_status (status, sizeof (status));
			MessageBoxA (hwnd, status, "lime-vulkan demo: texture upload failed", MB_OK | MB_ICONERROR);
			return 3;

		}

		if (!CreateCamerasScene ()) return 5;

	} else if (batchScene) {

		BuildDemoTexture ();

		texture = lime_vulkan_texture_create (g_texturePixels, kTextureSize, kTextureSize, LIME_VULKAN_TEXTURE_NONE);

		if (texture == 0) {

			char status[512] = { 0 };
			lime_vulkan_status (status, sizeof (status));
			MessageBoxA (hwnd, status, "lime-vulkan demo: texture upload failed", MB_OK | MB_ICONERROR);
			return 3;

		}

		printf ("[demo] texture %dx%d uploaded, scene=batch quads=%d\n", kTextureSize, kTextureSize, quadCount);

	} else if (strcmp (g_sceneName, "blend") == 0) {

		printf ("[demo] scene=blend bands=4\n");
		if (texture) lime_vulkan_texture_destroy (texture);

	} else if (g_texAllocScene) {

		CreateTexAllocScene ();

	} else {

		lime_vulkan_set_pattern (pattern);
		printf ("[demo] scene=pattern pattern=%d\n", pattern);

	}

	ShowWindow (hwnd, showCommand);
	UpdateWindow (hwnd);

	// Demo clear colour: dark blue purple, easy to tell apart from the default background of the OpenGL path
	const float clearColor[4] = { 0.09f, 0.05f, 0.16f, 1.0f };

	LARGE_INTEGER frequency;
	LARGE_INTEGER previousCounter;
	LARGE_INTEGER startCounter;
	QueryPerformanceFrequency (&frequency);
	QueryPerformanceCounter (&previousCounter);
	startCounter = previousCounter;

	int frames = 0;
	int displayedFrames = 0;
	double elapsedSeconds = 0.0;
	char title[256];

	while (g_running) {

		MSG message;

		while (PeekMessageA (&message, NULL, 0, 0, PM_REMOVE)) {

			if (message.message == WM_QUIT) {

				g_running = false;
				break;

			}

			TranslateMessage (&message);
			DispatchMessageA (&message);

		}

		if (!g_running) break;

		if (g_resized) {

			lime_vulkan_resize (g_width, g_height);
			g_resized = false;

		}

		LARGE_INTEGER now;
		QueryPerformanceCounter (&now);
		float totalSeconds = (float)((double)(now.QuadPart - startCounter.QuadPart) / (double)frequency.QuadPart);

		bool ok = false;

		if (strcmp (g_sceneName, "blend") == 0) {

			ok = DrawBlendScene ();

		} else if (g_camerasScene) {

			ok = DrawCamerasScene (totalSeconds, texture);

		} else if (g_shaderScene) {

			ok = DrawShaderScene (totalSeconds);

		} else if (g_quadsScene) {

			ok = DrawQuadsScene (texture);

		} else if (g_probe) {

			ok = DrawProbeScene ();

		} else if (testScene) {

			ok = DrawTestScene (texture);

		} else if (batchScene) {

			ok = DrawBatchScene (totalSeconds, texture, quadCount);

		} else {

			ok = lime_vulkan_present (clearColor) != 0;

		}

		if (!ok) {

			char status[512] = { 0 };
			lime_vulkan_status (status, sizeof (status));
			printf ("[demo] frame failed: %s\n", ExtractError (status));
			MessageBoxA (hwnd, status, "lime-vulkan demo: frame failed", MB_OK | MB_ICONERROR);
			break;

		}

		frames++;

		elapsedSeconds += (double)(now.QuadPart - previousCounter.QuadPart) / (double)frequency.QuadPart;
		previousCounter = now;

		if (elapsedSeconds >= 0.5) {

			double fps = (double)(frames - displayedFrames) / elapsedSeconds;

			if (batchScene) {

				char stats[256] = { 0 };
				lime_vulkan_batch_stats (stats, sizeof (stats));

				int quadsDrawn = 0;
				int drawCalls = 0;
				const char* cursor = stats;

				while (cursor && *cursor) {

					const char* end = strchr (cursor, '\n');
					size_t length = end ? (size_t)(end - cursor) : strlen (cursor);

					if (length > 5 && strncmp (cursor, "quads=", 6) == 0) quadsDrawn = atoi (cursor + 6);
					if (length > 9 && strncmp (cursor, "drawCalls=", 10) == 0) drawCalls = atoi (cursor + 10);

					cursor = end ? end + 1 : 0;

				}

				snprintf (title, sizeof (title), "lime-vulkan batch - %.0f fps - %d quads / %d draw call(s) - ESC to quit",
					fps, quadsDrawn, drawCalls);

			} else {

				snprintf (title, sizeof (title), "lime-vulkan demo - %.0f fps - %d frame(s) - ESC to quit", fps, frames);

			}

			SetWindowTextA (hwnd, title);
			printf ("[demo] %s\n", title);

			displayedFrames = frames;
			elapsedSeconds = 0.0;

		}

		if (maxFrames > 0 && frames >= maxFrames) break;

	}

	// One machine readable line, printed while the device is still up: the perf
	// harness in tools/perf-baseline.ps1 parses it into PERF.md.
	if (g_perfOutput) {

		char stats[4096] = { 0 };
		lime_vulkan_batch_stats (stats, sizeof (stats));

		char perfFrames[32] = "0";
		StatField (stats, "perfFrames", perfFrames, sizeof (perfFrames));

		printf ("[demo] perf scene=%s frames=%s cpuP50=%.2f cpuP95=%.2f cpuP99=%.2f cpuMax=%.2f "
			"gpuP50=%.2f gpuP95=%.2f gpuP99=%.2f gpuSupported=%d draws=%d uploadMB=%.2f\n",
			g_sceneName, perfFrames,
			StatNumber (stats, "cpuP50"), StatNumber (stats, "cpuP95"),
			StatNumber (stats, "cpuP99"), StatNumber (stats, "cpuMax"),
			StatNumber (stats, "gpuP50"), StatNumber (stats, "gpuP95"), StatNumber (stats, "gpuP99"),
			(int)StatNumber (stats, "gpuSupported"),
			(int)StatNumber (stats, "drawCalls"), StatNumber (stats, "uploadMB"));

	}

	// The context goes first: it waits for the device to go idle. Destroying a
	// texture or a program while a submitted frame still references it is a
	// use-after-free on the GPU, and the demo used to do exactly that at exit.
	lime_vulkan_context_destroy ();

	DestroyTexAllocScene ();
	DestroyCamerasScene ();

	if (texture) lime_vulkan_texture_destroy (texture);
	if (g_shaderTexture) lime_vulkan_texture_destroy (g_shaderTexture);
	if (g_program) lime_vulkan_program_destroy (g_program);
	DestroyWindow (hwnd);

	{
		LARGE_INTEGER end;
		QueryPerformanceCounter (&end);

		double seconds = (double)(end.QuadPart - startCounter.QuadPart) / (double)frequency.QuadPart;

		printf ("[demo] %d frames in %.3f s (%.1f fps average)\n", frames, seconds,
			seconds > 0.0 ? (double)frames / seconds : 0.0);
	}

	{
		char status[512] = { 0 };
		lime_vulkan_status (status, sizeof (status));
		printf ("[demo] status:\n%s\n", status);
	}

	return 0;

}
