// hxcpp primitives for the Vulkan renderer.
//
// The renderer is linked into Lime.ndll, so Haxe cannot call it at link time
// the way an extension binds to its own dll. Every entry point is exported as a
// primitive instead, which Haxe loads by name at runtime (cpp.Lib.load).
//
// Pointers and 64 bit handles cross the boundary as doubles: a primitive
// argument is a hxcpp value, and a 48 bit pointer fits a double without loss.

#include <hx/CFFI.h>

#include "lime_vulkan.h"

typedef lime_vulkan_texture_handle TextureHandle;

static inline void* AsPointer (double value)
{
	return (void*)(intptr_t)value;
}

static inline double AsNumber (const void* pointer)
{
	return (double)(intptr_t)pointer;
}

static inline double AsNumber (TextureHandle handle)
{
	return (double)handle;
}

static inline TextureHandle AsHandle (double value)
{
	return (TextureHandle)(intptr_t)value;
}

static const char* OptionalText (value text)
{
	return val_is_null (text) ? nullptr : val_string (text);
}

static value ToText (const char* text)
{
	return alloc_string (text ? text : "");
}

// ---------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------

static value lime_vulkan_version_hx ()
{
	return ToText (lime_vulkan_version ());
}

DEFINE_PRIM (lime_vulkan_version_hx, 0);

static value lime_vulkan_available_hx ()
{
	return alloc_int (lime_vulkan_available ());
}

DEFINE_PRIM (lime_vulkan_available_hx, 0);

static value lime_vulkan_probe_text_hx ()
{
	return ToText (lime_vulkan_probe_text ());
}

DEFINE_PRIM (lime_vulkan_probe_text_hx, 0);

static value lime_vulkan_status_text_hx ()
{
	return ToText (lime_vulkan_status_text ());
}

DEFINE_PRIM (lime_vulkan_status_text_hx, 0);

static value lime_vulkan_batch_stats_text_hx ()
{
	return ToText (lime_vulkan_batch_stats_text ());
}

DEFINE_PRIM (lime_vulkan_batch_stats_text_hx, 0);

static value lime_vulkan_self_window_hx ()
{
	return alloc_float (AsNumber (lime_vulkan_self_window ()));
}

DEFINE_PRIM (lime_vulkan_self_window_hx, 0);

static value lime_vulkan_window_client_size_hx (value window, value width, value height)
{
	return alloc_int (lime_vulkan_window_client_size (AsPointer (val_number (window)),
		(int*)(intptr_t)val_number (width), (int*)(intptr_t)val_number (height)));
}

DEFINE_PRIM (lime_vulkan_window_client_size_hx, 3);

static value lime_vulkan_context_create_flat_hx (value window, value width, value height,
	value deviceIndex, value presentMode, value validation, value verbose, value fallbackDialog)
{
	return alloc_int (lime_vulkan_context_create_flat (AsPointer (val_number (window)),
		val_int (width), val_int (height), val_int (deviceIndex), val_int (presentMode),
		val_int (validation), val_int (verbose), val_int (fallbackDialog)));
}

DEFINE_PRIM (lime_vulkan_context_create_flat_hx, 8);

static value lime_vulkan_probe_window_flat_hx (value window, value width, value height,
	value deviceIndex, value presentMode, value validation, value verbose, value fallbackDialog)
{
	return alloc_int (lime_vulkan_probe_window_flat (AsPointer (val_number (window)),
		val_int (width), val_int (height), val_int (deviceIndex), val_int (presentMode),
		val_int (validation), val_int (verbose), val_int (fallbackDialog)));
}

DEFINE_PRIM (lime_vulkan_probe_window_flat_hx, 8);

static value lime_vulkan_context_destroy_hx ()
{
	lime_vulkan_context_destroy ();
	return alloc_null ();
}

DEFINE_PRIM (lime_vulkan_context_destroy_hx, 0);

static value lime_vulkan_context_attached_hx ()
{
	return alloc_int (lime_vulkan_context_attached ());
}

DEFINE_PRIM (lime_vulkan_context_attached_hx, 0);

static value lime_vulkan_release_gl_hx ()
{
	return alloc_int (lime_vulkan_release_gl ());
}

DEFINE_PRIM (lime_vulkan_release_gl_hx, 0);

static value lime_vulkan_resize_hx (value width, value height)
{
	return alloc_int (lime_vulkan_resize (val_int (width), val_int (height)));
}

DEFINE_PRIM (lime_vulkan_resize_hx, 2);

static value lime_vulkan_notify_fallback_hx (value reason)
{
	lime_vulkan_notify_fallback (OptionalText (reason));
	return alloc_null ();
}

DEFINE_PRIM (lime_vulkan_notify_fallback_hx, 1);

static value lime_vulkan_set_dialog_enabled_hx (value enabled)
{
	lime_vulkan_set_dialog_enabled (val_int (enabled));
	return alloc_null ();
}

DEFINE_PRIM (lime_vulkan_set_dialog_enabled_hx, 1);

// ---------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------

static value lime_vulkan_texture_create_hx (value rows, value width, value height, value flags)
{
	return alloc_float (AsNumber (lime_vulkan_texture_create (AsPointer (val_number (rows)),
		val_int (width), val_int (height), val_int (flags))));
}

DEFINE_PRIM (lime_vulkan_texture_create_hx, 4);

static value lime_vulkan_texture_update_hx (value texture, value rows, value flags)
{
	return alloc_int (lime_vulkan_texture_update (AsHandle (val_number (texture)),
		AsPointer (val_number (rows)), val_int (flags)));
}

DEFINE_PRIM (lime_vulkan_texture_update_hx, 3);

static value lime_vulkan_texture_destroy_hx (value texture)
{
	lime_vulkan_texture_destroy (AsHandle (val_number (texture)));
	return alloc_null ();
}

DEFINE_PRIM (lime_vulkan_texture_destroy_hx, 1);

static value lime_vulkan_texture_retire_hx (value texture)
{
	lime_vulkan_texture_retire (AsHandle (val_number (texture)));
	return alloc_null ();
}

DEFINE_PRIM (lime_vulkan_texture_retire_hx, 1);

static value lime_vulkan_texture_size_hx (value texture, value width, value height)
{
	return alloc_int (lime_vulkan_texture_size (AsHandle (val_number (texture)),
		(int*)(intptr_t)val_number (width), (int*)(intptr_t)val_number (height)));
}

DEFINE_PRIM (lime_vulkan_texture_size_hx, 3);

// ---------------------------------------------------------------------
// Frame and draw state
// ---------------------------------------------------------------------

static value lime_vulkan_batch_begin_rgba_hx (value width, value height, value argb)
{
	return alloc_int (lime_vulkan_batch_begin_rgba (val_int (width), val_int (height), (unsigned int)val_int (argb)));
}

DEFINE_PRIM (lime_vulkan_batch_begin_rgba_hx, 3);

static value lime_vulkan_batch_end_hx ()
{
	return alloc_int (lime_vulkan_batch_end ());
}

DEFINE_PRIM (lime_vulkan_batch_end_hx, 0);

static value lime_vulkan_batch_set_texture_hx (value texture)
{
	lime_vulkan_batch_set_texture (AsHandle (val_number (texture)));
	return alloc_null ();
}

DEFINE_PRIM (lime_vulkan_batch_set_texture_hx, 1);

static value lime_vulkan_batch_set_filter_hx (value nearest)
{
	lime_vulkan_batch_set_filter (val_int (nearest));
	return alloc_null ();
}

DEFINE_PRIM (lime_vulkan_batch_set_filter_hx, 1);

static value lime_vulkan_batch_set_blend_hx (value blend)
{
	lime_vulkan_batch_set_blend (val_int (blend));
	return alloc_null ();
}

DEFINE_PRIM (lime_vulkan_batch_set_blend_hx, 1);

static value lime_vulkan_batch_set_scissor_hx (value x, value y, value width, value height)
{
	lime_vulkan_batch_set_scissor (val_int (x), val_int (y), val_int (width), val_int (height));
	return alloc_null ();
}

DEFINE_PRIM (lime_vulkan_batch_set_scissor_hx, 4);

static value lime_vulkan_batch_set_view_hx (value matrix)
{
	lime_vulkan_batch_set_view ((const double*)(intptr_t)val_number (matrix));
	return alloc_null ();
}

DEFINE_PRIM (lime_vulkan_batch_set_view_hx, 1);

static value lime_vulkan_batch_quad_hx (value x, value y, value width, value height,
	value u0, value v0, value u1, value v1, value color)
{
	return alloc_int (lime_vulkan_batch_quad ((float)val_number (x), (float)val_number (y),
		(float)val_number (width), (float)val_number (height), (float)val_number (u0), (float)val_number (v0),
		(float)val_number (u1), (float)val_number (v1), (unsigned int)val_int (color)));
}

DEFINE_PRIM (lime_vulkan_batch_quad_hx, 9);

static value lime_vulkan_batch_quads_d_hx (value rects, value transforms, value quadCount,
	value alphas, value uniformAlpha, value colorMultipliers, value colorOffsets)
{
	return alloc_int (lime_vulkan_batch_quads_d ((const double*)(intptr_t)val_number (rects),
		(const double*)(intptr_t)val_number (transforms), val_int (quadCount),
		(const double*)(intptr_t)val_number (alphas), (float)val_number (uniformAlpha),
		(const double*)(intptr_t)val_number (colorMultipliers), (const double*)(intptr_t)val_number (colorOffsets)));
}

DEFINE_PRIM (lime_vulkan_batch_quads_d_hx, 7);

static value lime_vulkan_batch_set_program_hx (value program)
{
	lime_vulkan_batch_set_program ((lime_vulkan_program)(intptr_t)val_number (program));
	return alloc_null ();
}

DEFINE_PRIM (lime_vulkan_batch_set_program_hx, 1);

// ---------------------------------------------------------------------
// Render targets
// ---------------------------------------------------------------------

static value lime_vulkan_target_create_hx (value width, value height)
{
	return alloc_float (AsNumber (lime_vulkan_target_create (val_int (width), val_int (height))));
}

DEFINE_PRIM (lime_vulkan_target_create_hx, 2);

static value lime_vulkan_target_valid_hx (value target)
{
	return alloc_int (lime_vulkan_target_valid (AsHandle (val_number (target))));
}

DEFINE_PRIM (lime_vulkan_target_valid_hx, 1);

static value lime_vulkan_batch_begin_target_hx (value target, value argb)
{
	return alloc_int (lime_vulkan_batch_begin_target (AsHandle (val_number (target)), (unsigned int)val_int (argb)));
}

DEFINE_PRIM (lime_vulkan_batch_begin_target_hx, 2);

static value lime_vulkan_batch_end_target_hx ()
{
	return alloc_int (lime_vulkan_batch_end_target ());
}

DEFINE_PRIM (lime_vulkan_batch_end_target_hx, 0);

// ---------------------------------------------------------------------
// Shader programs
// ---------------------------------------------------------------------

static value lime_vulkan_program_create_flat_hx (value vertex, value fragment, value flags, value cache)
{
	return alloc_float (AsNumber (lime_vulkan_program_create_flat (val_string (vertex), val_string (fragment),
		(unsigned int)val_int (flags), OptionalText (cache))));
}

DEFINE_PRIM (lime_vulkan_program_create_flat_hx, 4);

static value lime_vulkan_program_destroy_hx (value program)
{
	lime_vulkan_program_destroy ((lime_vulkan_program)(intptr_t)val_number (program));
	return alloc_null ();
}

DEFINE_PRIM (lime_vulkan_program_destroy_hx, 1);

static value lime_vulkan_program_valid_hx (value program)
{
	return alloc_int (lime_vulkan_program_valid ((lime_vulkan_program)(intptr_t)val_number (program)));
}

DEFINE_PRIM (lime_vulkan_program_valid_hx, 1);

static value lime_vulkan_program_log_hx (value program)
{
	return ToText (lime_vulkan_program_log ((lime_vulkan_program)(intptr_t)val_number (program)));
}

DEFINE_PRIM (lime_vulkan_program_log_hx, 1);

static value lime_vulkan_program_uniform_count_hx (value program)
{
	return alloc_int (lime_vulkan_program_uniform_count ((lime_vulkan_program)(intptr_t)val_number (program)));
}

DEFINE_PRIM (lime_vulkan_program_uniform_count_hx, 1);

static value lime_vulkan_program_uniform_name_hx (value program, value index)
{
	return ToText (lime_vulkan_program_uniform_name ((lime_vulkan_program)(intptr_t)val_number (program), val_int (index)));
}

DEFINE_PRIM (lime_vulkan_program_uniform_name_hx, 2);

static value lime_vulkan_program_uniform_type_hx (value program, value index)
{
	return alloc_int (lime_vulkan_program_uniform_type ((lime_vulkan_program)(intptr_t)val_number (program), val_int (index)));
}

DEFINE_PRIM (lime_vulkan_program_uniform_type_hx, 2);

static value lime_vulkan_program_uniform_offset_hx (value program, value index)
{
	return alloc_int ((int)lime_vulkan_program_uniform_offset ((lime_vulkan_program)(intptr_t)val_number (program), val_int (index)));
}

DEFINE_PRIM (lime_vulkan_program_uniform_offset_hx, 2);

static value lime_vulkan_program_uniform_size_hx (value program, value index)
{
	return alloc_int ((int)lime_vulkan_program_uniform_size ((lime_vulkan_program)(intptr_t)val_number (program), val_int (index)));
}

DEFINE_PRIM (lime_vulkan_program_uniform_size_hx, 2);

static value lime_vulkan_program_uniform_length_hx (value program, value index)
{
	return alloc_int (lime_vulkan_program_uniform_length ((lime_vulkan_program)(intptr_t)val_number (program), val_int (index)));
}

DEFINE_PRIM (lime_vulkan_program_uniform_length_hx, 2);

static value lime_vulkan_program_sampler_count_hx (value program)
{
	return alloc_int (lime_vulkan_program_sampler_count ((lime_vulkan_program)(intptr_t)val_number (program)));
}

DEFINE_PRIM (lime_vulkan_program_sampler_count_hx, 1);

static value lime_vulkan_program_sampler_name_hx (value program, value index)
{
	return ToText (lime_vulkan_program_sampler_name ((lime_vulkan_program)(intptr_t)val_number (program), val_int (index)));
}

DEFINE_PRIM (lime_vulkan_program_sampler_name_hx, 2);

static value lime_vulkan_program_set_sampler_texture_hx (value program, value index, value texture)
{
	return alloc_int (lime_vulkan_program_set_sampler_texture ((lime_vulkan_program)(intptr_t)val_number (program),
		val_int (index), AsHandle (val_number (texture))));
}

DEFINE_PRIM (lime_vulkan_program_set_sampler_texture_hx, 3);

static value lime_vulkan_program_write_floats_hx (value program, value offset, value values, value count)
{
	return alloc_int (lime_vulkan_program_write_floats ((lime_vulkan_program)(intptr_t)val_number (program),
		(unsigned int)val_int (offset), (const double*)(intptr_t)val_number (values), val_int (count)));
}

DEFINE_PRIM (lime_vulkan_program_write_floats_hx, 4);

static value lime_vulkan_program_write_ints_hx (value program, value offset, value values, value count)
{
	return alloc_int (lime_vulkan_program_write_ints ((lime_vulkan_program)(intptr_t)val_number (program),
		(unsigned int)val_int (offset), (const int*)(intptr_t)val_number (values), val_int (count)));
}

DEFINE_PRIM (lime_vulkan_program_write_ints_hx, 4);