# Vulkan renderer

A Vulkan renderer for OpenFL and flixel, part of Lime. Windows x64 and Linux x64
are complete (Win32 and X11 surfaces). Android carries the same library code plus the
`ANativeWindow` surface: lime's SDL3 Android window registers its handle through
`lime_vulkan_set_android_window()` and re-registers it after the activity recreates
the surface, so the renderer can attach there like on the other platforms (see
"Android" under Integration).

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
| `native/src/` | Implementation: loader, device, images, batches, GLSL translation, programs, and the platform layer (`surface_*.cpp` / `window_*.cpp`). |
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

* Windows x64 and MSVC 2019 or newer, or
* Linux x64 with the X11 development headers, or
* Android with the NDK (armv7 / arm64).
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

The fork's CI runs on every push: it rebuilds `Lime.ndll` for Windows, Linux,
macOS, Android and iOS, and on Windows it runs `tests/check_exports.ps1` against
the fresh library, so a missing export fails the build rather than the game.
The Windows, Linux and Android jobs compile the renderer; macOS and iOS build
without it.

Only the configuration the compiler runs on is covered by a normal build. The
other halves can be syntax-checked anywhere: the Android NDK's clang compiles
for `aarch64-linux-android` with the X11 headers stubbed out, and `-U__ANDROID__`
turns the same run into the Linux configuration. CI is still the authoritative
gate, because it compiles and links each platform for real.

## Integration

1. Build the shared library once, as above.
2. Compile the application with `-D seiun_vulkan`; Lime puts `vulkan/haxe` on the
   class path for that define.
3. Call `VkRuntime.start()` once the stage exists, install `VkStageRenderer`
   through `VkOverlay.install()`, and call `VkRuntime.endFrame()` from the window
   render callback.
4. Android only: nothing there can find the game window, so it has to be handed over
   with `lime_vulkan_set_android_window()` — the pointer is the one SDL3 exposes as
   `SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER`. **Lime does this itself** now
   (`src/backend/sdl3/SDLWindow.cpp`): the handle is registered when the window is
   created, re-registered by an SDL event watch after the activity recreates the
   surface (resume, rotation, split screen), and set to `NULL` when the window goes
   away — a stale `ANativeWindow` is worse than a fallback. An application only has
   to start the runtime, as on every other platform.

### Android

The window handle is the only part Android needs from the host; the rest of the
sequence is the same as on Windows. Two details are worth knowing:

* The renderer picks the new handle up at a **frame boundary** on the render thread
  (`TakeChangedAndroidWindow` in `BeginFrame`), never from the event callback: the
  activity posts its surface change on the Java thread, and rebuilding a swapchain
  there would tear down a frame that is still in flight. The old `VkSurfaceKHR` is
  destroyed and rebuilt for the new window (`RecreateAndroidSurface`).
* If the new surface has no client area yet, the frame is skipped instead of being
  turned into a failure, exactly like a minimised window on the desktop.
* The swapchain is built with `VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR`, not with the
  surface's `currentTransform`. A landscape game on a portrait panel gets
  `currentTransform = ROTATE_90`, and asking for that makes the compositor rotate an
  image the engine already drew upright: the game fills the screen sideways and
  squeezed (seen on a Mali-G610, reported as "portrait aspect ratio"). Identity is
  what every other platform implies, and the presentation engine maps the image 1:1
  onto the window. The extent stays `currentExtent`, so the viewport, the projection
  and the scissor all keep using the size lime reports.

### Frames per second: what not to wait for

Two traps cost this renderer a factor of four on a Mali-G610 and neither shows up
as a slow call in a profiler:

* **`VK_SUBOPTIMAL_KHR` from `vkQueuePresentKHR` is advisory.** The renderer used
  to rebuild the whole swapchain on it - `vkDeviceWaitIdle`, nine swapchain images,
  nine framebuffers and all the sync objects - which took 26 ms and made the game
  run at 33 fps. Because the Android swapchain is pre-rotated by the engine
  (identity `preTransform`, see above), the driver there answers SUBOPTIMAL on
  *every* present, so that branch ran on *every* frame. It now only rebuilds when
  the window size really changed, which is the only thing SUBOPTIMAL can mean here:
  rotation arrives as a new `ANativeWindow`, a resize sets `state.resized`.
* **The upload channel used to be a single slot.** Recording the next batch waited
  for the previous upload's fence and for the previous frame's fence, so the CPU
  could never run ahead of the GPU. It is now one channel per frame in flight: the
  copies are submitted on the graphics queue immediately before the frame's own
  commands, and the barrier at the end of the upload buffer is what orders them
  against that frame's draws. Inside a frame nothing waits; only an upload queued
  with no frame open (a loader) waits for its fence.

The stats text (`lime_vulkan_batch_stats`) reports both: `swapchainRebuilds`,
`suboptimal`, and a split of the frame into the waits it is blocked on
(`tFence`, `tAcquire`, `tSubmit`, `tPresent`, `tBegin`, `tEnd`, `tBatch`,
`tBeginPass`, `tEndPass`, `tEndBuffer`, `tUpload*`). A frame that takes 30 ms while
every one of those is under 1 ms means the time is in a driver call that is not
being measured - which is how the swapchain rebuild above was found.

### Rebuilding after a header change

Lime's Android build recompiles a source file when that `.cpp` is newer than its
object, and it does **not** track header dependencies. Editing `native/src/internal.h`
(the `State` layout lives there, and every translation unit indexes it) therefore
leaves some objects compiled against the old layout: the process then reads and
writes the wrong fields, which surfaces as unrelated crashes (a null vertex buffer
here, a SIGSEGV inside the driver there). After touching a header, delete that
target's objects first:

```powershell
Get-ChildItem .haxelib/lime/git/project/obj/androidarm64-sdl3-64 -Filter "c42a0508_*.obj" | Remove-Item -Force
haxelib run lime rebuild android -arm64
```

The `c42a0508_` prefix is the hash lime gives `vulkan/native/src`; `lime rebuild`
lists the files it recompiled, and that list is the check that the objects match the
header.

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

Build-time probes, off unless the define is passed:

| Define | Effect |
|---|---|
| `-D seiun_vulkan_perf` | Write `logs/vulkan-perf.txt` every 300 frames: the stats text plus the frame split, the per-frame texture updates by size, and the Haxe side of the frame (`submitMs`, `overlayWalkMs`). This is what found the swapchain rebuild; on the phone it is the only way to see the numbers, because a release build has no console. |

## Status

Supported today: quad batches, colour transform, palette textures, camera
filters (shader, blur, colour matrix, convolution), custom per-sprite shaders,
the OpenFL overlay (bitmap, text, graphics), and video frames that a mod feeds
into a sprite.

Platform layer: Win32 finds the largest visible window of the process, X11 walks
the tree for the window whose `_NET_WM_PID` is this process, and both create and
size the surface from it. Android cannot enumerate windows, so lime's SDL3 window
passes the `ANativeWindow` to `lime_vulkan_set_android_window()` and keeps it
current across surface recreation; with no registered handle the backend reports
"unavailable" instead of guessing. Wayland is not covered (X11 only).

Not implemented yet: glow, drop shadow and displacement map filters; the
triangle path; offscreen render textures; macOS and iOS (no Vulkan driver, and
MoltenVK is not bundled).

## License

Same terms as the engine that loads it. The Vulkan headers under `third_party/`
are the Khronos ones, Apache 2.0.
