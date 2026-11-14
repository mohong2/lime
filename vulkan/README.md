# Vulkan renderer

A Vulkan renderer for OpenFL and flixel on Windows x64, part of Lime.

The renderer takes over the drawing that OpenFL and flixel would normally send to
OpenGL: quad batches become one Vulkan draw per batch, camera filters become a
short offscreen pass chain, and the OpenFL display list is repainted on top of the
result. It is linked into the shared library (`Lime.ndll`), so an application needs
no extra file: rebuild once with `lime rebuild windows` and switch the renderer on
with `-Dseiun_vulkan`.

Every failure is quiet. A machine without a Vulkan driver, a window that cannot
hand its surface to the compositor, or a build without the renderer keeps OpenGL
and reports the reason instead of aborting.

## Layout

| Path | Contents |
|---|---|
| `native/include/lime_vulkan.h` | The public C API. |
| `native/src/` | Implementation: loader, device, surface, images, batches, GLSL translation, programs. |
| `native/src/haxe_api.cpp` | hxcpp primitives, the functions the Haxe side looks up at runtime. |
| `native/shaders/` | Built-in shaders, compiled to `*_spirv.h` by the build. |
| `native/shaderc/` | Command line shader compiler, used to validate GLSL offline. |
| `native/demo/` | Standalone Win32 window, the test bed for the pixel tests. |
| `native/attach/` | Draws into a window owned by another process. Debug tool. |
| `haxe/seiun/vulkan/` | Haxe side: runtime, binding, texture cache, quad batches, camera filters, overlay. |
| `third_party/` | Vulkan headers, so no SDK is needed to build. |
| `tools/` | Shader toolchain and capture helpers. |
| `tests/` | Pixel tests and the export check. |

## Requirements

* Windows x64 and MSVC 2019 or newer.
* Python 3 on `PATH` (shader compilation).
* Haxe 4.3 and hxcpp.

## Build

The renderer is compiled as part of the shared library:

```powershell
haxelib run lime rebuild windows
```

`-Dlime-vulkan-off` leaves it out of the library.

For the tests and the standalone tools, which link against the C API directly:

```powershell
pwsh -File build-native.ps1 -Targets dll   # lime-vulkan.dll only
pwsh -File build-native.ps1 -Targets all   # also demo, attach tool and shaderc
```

## Test

```powershell
pwsh -File tools/run-tests.ps1
```

The tests build the demo, run it for a fixed number of frames, read the
framebuffer back and compare pixels against expected colours. They cover batch
geometry, blend modes, texture formats and the shader toolchain.
`tests/check_exports.ps1` verifies that `Lime.ndll` exports every primitive the
Haxe side loads.

## Integration

1. Build the shared library once, as above.
2. Compile the application with `-D seiun_vulkan`; Lime puts `vulkan/haxe` on the
   class path for that define.
3. Call `VkRuntime.start()` once the stage exists, install `VkStageRenderer`
   through `VkOverlay.install()`, and call `VkRuntime.endFrame()` from the window
   render callback.

## Runtime flags

| Flag | Effect |
|---|---|
| `--vulkan-stats` | Print a per-second line with batch, filter, texture and overlay counters. |
| `--vulkan-trace` | Print the first draw items of a state switch. |
| `--vulkan-nofilters` | Run the cameras without filters, to compare against a filtered frame. |
| `--vulkan-nostagefilters` | Skip the filter chain only for the overlay stage. |
| `--vulkan-no-overlay` | Do not repaint the OpenFL display list. |
| `--vulkan-keep-gl` | Leave OpenFL in charge of the stage. |
| `--vulkan-keep-context` | Keep the OpenGL context alive after the swapchain exists. |
| `--vulkan-noscissor` | Draw without camera clipping. |
| `--vulkan-flat` | Draw every quad with a white texture, to inspect geometry. |
| `--vulkan-probe` | Draw one stretched quad per batch, to inspect texture assignment. |
| `--vulkan-magenta` | Clear to magenta, to see what is not covered. |
| `--vulkan-validation` | Enable the Vulkan validation layers. |
| `--vulkan-no-cache` | Upload textures every frame instead of caching them. |
| `--vulkan-texstats` | Report texture coverage. |
| `--vulkan-texdump` | Dump uploaded textures to `logs/texdump`. |

## Status

Supported today: quad batches, colour transform, palette textures, camera
filters (shader, blur, colour matrix, convolution), custom per-sprite shaders,
the OpenFL overlay (bitmap, text, graphics), and video frames that a mod feeds
into a sprite.

Not implemented yet: glow, drop shadow and displacement map filters; the
triangle path; offscreen render textures; platforms other than Windows.

## License

Same terms as the engine that loads it. The Vulkan headers under `third_party/`
are the Khronos ones, Apache 2.0.
