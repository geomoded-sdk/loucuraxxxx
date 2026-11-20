# Game compatibility (planned)

This document is deliberately cautious. Running a modern game with a demanding
shader pack is a **long-term test goal**, not a current feature. Nothing here is
claimed to work.

## The stated goal

The project's stated test target is **Minecraft** with:

* the **Continuum RT 2.1** shader pack, and
* a **Thunder Definition 42080x**-class texture pack.

These are the hardest realistic cases for a graphics layer: Continuum RT is a
ray-traced path-tracing pack, and a texture pack of that size stresses memory
residency and bandwidth. They are named here as targets so progress can be
measured against something concrete — not as supported configurations.

## What would have to be true first

A game does not talk to this layer directly. For any game to run *through*
LoucuraXXXX GPU, every one of the following must exist and work:

1. **A real GPU backend.** A game renders through its own graphics API
   (OpenGL, Direct3D, Vulkan, Metal). The layer must present itself as a device
   that API can use. The filter pipeline now runs on real GPU backends —
   Direct3D 11 on Windows, OpenGL, and opt-in Vulkan — and each pass is tested
   against the CPU reference output. What does **not** exist is a graphics
   *device driver* that a game's own API can enumerate and render through: the
   backends are consumer-side, driven by this project's renderer, not a driver
   the game selects. Until that driver path exists and is tested, no game can
   run through the layer.
2. **Shader translation.** Continuum RT ships its own shader programs in the
   format its target API expects. Running them means either accepting them in
   that API or translating them faithfully. The project has a shader library and
   a filter pipeline; it does **not** yet translate arbitrary third-party shader
   packs.
3. **Texture residency at scale.** A 42080x-class pack is far larger than any
   reservation this project would grant on a typical machine. It would have to
   stream from disk with a cache that respects the `MemoryBudget`. That cache is
   not implemented.
4. **Ray tracing.** Continuum RT path-traces. The layer has no acceleration
   structure support. Without a GPU backend that exposes ray tracing, this pack
   cannot run regardless of anything else.

## What the project can do today

The pieces that would eventually matter are real and tested:

* the filter pipeline (tone, blur, sharpen, bloom, vignette) runs over frames
  end to end on the CPU backend and on the Direct3D 11 and OpenGL backends,
  with each pass checked against the CPU reference output;
* the manager places work using a measured cost model and enforces a memory
  budget;
* the renderer reports measured frame times and FPS.

These are building blocks. None of them is a game-compatibility claim.

## The rule

**No compatibility is declared before the real rendering pipeline and the
shader pack's actual requirements have been tested.** When that testing happens,
the results go here with the exact host, driver versions, shader pack version
and configuration — or this section keeps saying "not yet".
