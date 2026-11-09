# ufbx

Unmodified `ufbx.c`, `ufbx.h` and `LICENSE` from
[ufbx/ufbx](https://github.com/ufbx/ufbx), tag `v0.23.1`
(commit `26a482ae66871d7de36eb722aa060bce95bce274`).

Media Server elects the MIT license (Alternative A in LICENSE). It is vendored
because the pinned vcpkg baseline has no ufbx port. `ufbx.c` is compiled as C
into a private static library linked only by `engine/render`, where
`model_loader_fbx.cpp` reads FBX models and UV projection meshes on the model
worker. Updates must repeat the `area-render` model and projection mesh loader
tests.
