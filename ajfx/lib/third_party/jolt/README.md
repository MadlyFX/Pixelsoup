# Jolt Physics

Unmodified `Jolt/` and `LICENSE` from
[jrouwe/JoltPhysics](https://github.com/jrouwe/JoltPhysics), tag `v5.6.0`,
commit `e77f175595e64cb44218cc9d9d56fc365ad0e36a`. MIT licensed.

ajfx's rigid emitters (`ajfx::rigid`) step their particles as Jolt bodies.
`CMakeLists.txt` here replaces Jolt's own `Build/CMakeLists.txt` and sets the
build up for determinism:
- `CROSS_PLATFORM_DETERMINISTIC`, with `/fp:precise` and no floating-point
  contraction. Every node must compile this exact source with these exact
  defines.
- SSE4.2 only (no AVX, FMA or BMI). The same binary then runs on every node's
  CPU.
- No GPU compute, debug renderer, profiler, object streams or floating-point
  exceptions. None of them are used, and some would change the defines a node
  compiles with.

To update, replace `Jolt/` and `LICENSE` from the new tag, update the tag and
commit above, and re-run the rigid tests. A new Jolt version changes results,
so every node must update together.
