# ChatGPT Emscripten WebGPU demo for the Armchair Engine

This demo integrates with OpenAI's completions API (i.e. ChatGPT) using Emscripten's fetch mechanism in the browser, providing a simple interface with [Dear Imgui](https://github.com/ocornut/imgui).

This is part four of a simple proof of concept, a minimal 3D engine written in C++, compiled to WASM with Emscripten.  Running in the browser, rendering with WebGPU.

For the previous demos, see:
- https://github.com/Armchair-Software/webgpu-demo
- https://github.com/Armchair-Software/webgpu-demo2
- https://github.com/Armchair-Software/webgpu-demo3 (may be private to this org)

## Live demo
Live demo: https://armchair-software.github.io/chatgpt-emscripten-demo/

This requires Firefox Nightly, or a recent version of Chrome or Chromium, with webgpu and Vulkan support explicitly enabled.

## Dependencies
- [Emscripten](https://emscripten.org/)
- CMake
- [VectorStorm](https://github.com/Armchair-Software/vectorstorm) (included)
- [LogStorm](https://github.com/VoxelStorm-Ltd/logstorm) (included)
- [magic_enum](https://github.com/Neargye/magic_enum) (included)
- [dear imgui](https://github.com/ocornut/imgui) with the proposed `imgui_impl_emscripten` backend (included)

## Building
The easiest way to assemble everything is to use the included build script:
```sh
./build.sh
```

To launch a local server and bring up a browser:
```sh
./run.sh
```

For manual builds with CMake, and to adjust how the example is run locally, inspect the `build.sh` and `run.sh` scripts.
