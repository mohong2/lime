[![MIT License](https://img.shields.io/badge/license-MIT-blue.svg?style=flat)](LICENSE.md) [![Haxelib Version](https://img.shields.io/github/tag/openfl/lime.svg?style=flat&label=haxelib)](http://lib.haxe.org/p/lime) [![Build Status](https://img.shields.io/github/actions/workflow/status/openfl/lime/main.yml?branch=develop)](https://github.com/openfl/lime/actions) [![Community](https://img.shields.io/discourse/posts?color=24afc4&server=https%3A%2F%2Fcommunity.openfl.org&label=community)](https://community.openfl.org/c/lime/19) [![Discord Server](https://img.shields.io/discord/415681294446493696.svg?color=7289da)](https://discordapp.com/invite/tDgq8EE)

Lime
====

Lime is a flexible, lightweight layer for Haxe cross-platform developers.

Lime supports native, Flash and HTML5 targets with unified support for:

 * Windowing
 * Input
 * Events
 * Audio
 * Render contexts
 * Network access
 * Assets

Lime does not include a renderer, but exposes the current context:

 * Cairo
 * Canvas
 * DOM
 * Flash
 * GL

The GL context is based upon the WebGL standard, implemented for both OpenGL and OpenGL ES as needed.

This fork also carries an experimental **Vulkan** renderer, which draws the same scene
without the GL context: see [Vulkan renderer](#vulkan-renderer-this-fork) below.

Lime provides a unified audio API, but also provides access to OpenAL for advanced audio on native targets.


Vulkan renderer (this fork)
===========================

`vulkan/` adds a Vulkan renderer for the OpenGL/flixel draw path on Windows x64, Linux x64
and Android. It takes over
the drawing that would otherwise go through GL: quad batches become one Vulkan draw each,
camera filters become a short offscreen pass chain, and the OpenFL display list is repainted
on top of the result. The native half is compiled into `Lime.ndll` itself, so an application
needs no extra file next to it.

 * **Built by default.** `project/Build.xml` sets `LIME_VULKAN` on Windows, Linux and
   Android; build with `-Dlime-vulkan-off` to leave the renderer out of the shared library.
 * **Opt-in per application.** Compiling the application with `-D seiun_vulkan` puts
   `vulkan/haxe` on the class path. At runtime the application still asks for it
   (`--vulkan`, or `SEIUN_VULKAN=1`) and stays on OpenGL otherwise; a machine without a
   Vulkan driver, a window that refuses the surface, or a library built without the renderer
   falls back quietly and reports the reason.
 * **Windows, Linux and Android.** Those three compile the renderer into
   `Lime.ndll` by default; macOS and iOS have no Vulkan driver and build without
   it. Windows (Win32) and Linux (X11) find the game window and create the
   surface themselves. Android cannot enumerate windows, so the host hands the
   `ANativeWindow` to `lime_vulkan_set_android_window()`; until something calls
   it, the session stays on OpenGL there.
 * **Checked on every push.** CI rebuilds the library for all five platforms and, on Windows,
   runs `vulkan/tests/check_exports.ps1` so a missing export fails the build instead of the
   game at runtime.

`vulkan/README.md` has the directory layout, the build and test commands, the runtime flags
and the list of what is implemented and what is not yet.


License
=======

Lime is free, open-source software under the [MIT license](LICENSE.md).


Installation
============

First install the latest version of [Haxe](http://www.haxe.org/download).


Development Builds
==================

When there are changes, Lime is built nightly. Builds are available for download [here](https://github.com/openfl/lime/actions?query=branch%3Adevelop+is%3Asuccess).

In this fork every push runs the CI workflow, which rebuilds `Lime.ndll` for Windows,
Linux, macOS, Android and iOS and uploads each one as an artefact of the run.

To install a development build, use the "haxelib local" command:

    haxelib local lime-haxelib.zip


Building from Source
====================

Clone the Lime repository, as well as the submodules:

    git clone --recursive https://github.com/openfl/lime

Tell haxelib where your development copy of Lime is installed:

    haxelib dev lime lime

The first time you run the "lime" command, it will attempt to build the Lime standard binary for your desktop platform as the command-line tools. To build these manually, use the following command (using "mac" or "linux" if appropriate):

    haxelib install format
    haxelib install hxp
    lime rebuild windows

You can build additional binaries, or rebuild binaries after making changes, using "lime rebuild":

    lime rebuild windows
    lime rebuild linux -64 -release -clean

You can also rebuild the tools if you make changes to them:

    lime rebuild tools

On a Windows machine, you should have Microsoft Visual Studio C++ (Express is just fine) installed. You will need Xcode on a Mac. To build on a Linux machine, you may need the following packages (or similar):

    sudo apt-get install libgl1-mesa-dev libglu1-mesa-dev g++ g++-multilib gcc-multilib libasound2-dev libx11-dev libxext-dev libxi-dev libxrandr-dev libxinerama-dev

To switch away from a source build, use:

    haxelib dev lime


Sample
======

You can build a sample Lime project with the following commands:

    lime create HelloWorld
    cd HelloWorld
    lime test neko

You can also list other projects that are available using "lime create".


Targets
=======

Lime currently supports the following targets:

    lime test windows
    lime test mac
    lime test linux
    lime test android
    lime test ios
    lime test html5
    lime test flash
    lime test air
    lime test neko
    lime test hl

Desktop builds are currently designed to be built on the same host OS
