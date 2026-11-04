# Android Vulkan backend: status

`sdk/runtime/gpu/vulkan.cpp` implements `gpu::Backend` with Vulkan 1.3 (dynamic rendering, synchronization2, push
descriptors). The shaders are the HLSL `shaders.cpp` already writes for D3D12, compiled to SPIR-V at run time by
`gpu/spirv.cpp` (glslang's HLSL front end).

What is checked so far:

- `vulkan.cpp` and `spirv.cpp` compile for arm64 with NDK 28 (`-fsyntax-only`).
- 44 generated shaders (fixed function vertex shaders, pixel combiners, target / shadow / quad variants) compile to
  SPIR-V (`tools-build`, target `shader_probe`). SPIR-V validation is not run: glslang was built without spirv-tools.

Nothing has run on a device or an emulator yet.

## Resource layout (descriptor set 0, push descriptors)

| binding | HLSL | what |
|---|---|---|
| 0 | `b0` | vertex constants (192 x float4) |
| 1 | `b1` | `DrawConstants` |
| 2-5 | `t0`-`t3` | sampled images |
| 6-9 | `s0`-`s3` | samplers |

Vertex inputs: `ATTRn` is location n (`[[vk::location(n)]]` is added to the vertex input struct before compiling).

## Not done yet

| Area | State |
|---|---|
| Point lists | Not drawn. D3D12 expands points with a geometry shader (`PointHlsl`); Mali GPUs often lack geometry shaders. Needs a vertex-shader or instanced quad expansion. |
| Render target read-back (`Sync`, `SyncLate`) | No-op. Anything the title reads back from the GPU (CPU-side effects, screenshots) is wrong. |
| CPU writes to render targets (`LatchCpuWrite`, `UploadTarget`) | Not implemented. Text the title draws with the CPU into a target will not show. |
| Sampling the target being rendered to | A dummy texture is read; D3D12 keeps a feedback copy. |
| Partial-channel colour clears | Clears all four channels. |
| Occlusion queries | `GetReport` always reports 0x100 visible pixels. |
| Gamma ramp (`SetGamma`) | Ignored. |
| Render scale, extensions, texture pack, capture, perf log | Not ported. |
| Memory | One `vkAllocateMemory` per image; some drivers cap allocations near 4096. Needs a sub-allocator. |
| Texture upload | Hash of the guest bytes once per frame per texture, as D3D12 does; no sweep of unused textures. |
| BC textures | Decoded on the CPU when the GPU lacks `textureCompressionBC`. |
| Swapchain | FIFO, identity transform; no rotation handling. |
| Vulkan version | Requires a 1.3 device (older phones are skipped with a log line). |

## Not part of the backend, still to write

- Android CMake project: SDL3 activity, glslang (`glslang`, `SPIRV`, `glslang-default-resource-limits`), the generated
  code linked in (no `X86_GAME_CODE`), `gpu/d3d12.cpp` and `audio/xaudio2.cpp` left out.
- Audio backend (Oboe / AAudio / SDL audio) in place of `audio/xaudio2.cpp`.
- App shell for `app/*` and `core/input.cpp`: touch and gamepad input, picking the game folder.
