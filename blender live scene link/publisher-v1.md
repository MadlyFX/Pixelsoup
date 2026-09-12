# Blender publisher v1

Step 05 implementation: `integrations/blender/mediaserver_livelink`. Current version 0.2.0
adds [step 06 shot placement](shot-placement-v1.md); step 05 was qualified as 0.1.0,
qualified on Blender **5.2.0 LTS / fbe6228777e7**, Windows. The add-on checks both
version tuple and build hash before connecting. Other Blender builds are refused
until qualified. No third-party Python package is needed.

## Execution and scheduling

Persistent dependency-graph handlers collect scalar session IDs and invalidation
flags. Message-bus notifications cover UI names, visibility and mode changes.
Handlers do not export content. A Blender application timer polls nonblocking IPC
and processes pending edits after 200 ms of quiet. Idle polling performs no source
scan, mesh export, hashing, filesystem I/O or network waits. It only polls the
bounded socket, reaps terminated child processes and redraws the relevant panel.

All RNA and dependency-graph access occurs on Blender's main thread. A dispatch
extracts at most one evaluated prototype with `evaluated_get` / `to_mesh` and bulk
`foreach_get` array copies. `to_mesh_clear` runs in `finally`, including validation
failures. The original mesh, modifier stack and node setup are never applied or
replaced. Completed copies contain numeric arrays and plain metadata only.

Serialization, hashing, GLB generation, resource transfer and Director receipts
run in a **separate Python process** launched with Blender's bundled interpreter.
The add-on starts no Python threads. This follows Blender's documented
[threading limitations](https://docs.blender.org/api/main/info_gotchas_threading.html).
The [timer API](https://docs.blender.org/api/current/bpy.app.timers.html) provides
the scheduling context; evaluated mesh lifetime follows the
[dependency-graph API](https://docs.blender.org/api/5.0/bpy.types.Depsgraph.html).
The actual pinned build was exercised in background tests and the native GUI.

IPC uses a private loopback listener with a random process handshake. Main-thread
sends are nonblocking and capped at 256 KiB per dispatch. One extraction batch and
one process request are admitted; while a request runs, newer edits only update a
bounded dirty set. A newer dependency generation discards an incomplete batch.
After a receipt, the newest pending source state is extracted. Arrays do not form
an unbounded queue. One evaluation/copy is not preemptible: expensive Blender
modifiers can still stall Blender itself, and the panel reports the last duration.
The declared input limits do not promise a hard latency bound on Blender's own
dependency evaluation. Split expensive content into manageable prototypes.

Edit/Sculpt mode defers extraction until Object Mode. Undo/redo invalidates copied
geometry but retains only scalar identity information; no invalid RNA wrapper
survives between timer calls. File load stops the process, clears the pairing and
per-file runtime identity maps, and leaves reconnection explicit. Disconnect and
unregister close sockets and terminate the owned child without a UI-thread join.
Disable also removes timers, handlers and message-bus subscriptions. On application
exit, socket closure ends the child's worker loop.

## Identity and evaluated content

Scenes, collections, objects, meshes and materials carry `ms_link_id` UUIDs.
The source UUID is derived from the persisted scene and selected-collection UUIDs.
The collection stores version/build/source metadata separately from the pairing.
Renames preserve identity. Session-ID ownership repairs copied custom UUIDs and
keeps source IDs through Blender undo/redo. As with other Blender document edits,
save the blend file to retain newly stamped IDs across process exit.

Read-only library IDs use an authored UUID if available, otherwise an explicit
`ms_link_library_ids` mapping on the local scene. Fallback keys are library path,
ID type and library name. Renaming/moving an external library entry with no authored
UUID changes that key; author persistent IDs in the library for rename-stable
external references. Ordinary editable object identities never use file paths or
names. The mapping is bounded to 4096 entries and session identity bookkeeping to
8192 entries, with an explicit refusal when full.

Each source object has its own persistent evaluated-prototype UUID derived from
its object UUID. This conservatively preserves per-object modifier results and
bindings even for shared mesh datablocks. Identical GLB bytes still deduplicate by
hash. Prototype hashes identify revisions; they are never object or occurrence IDs.
Transform-only ancestors outside the collection retain hierarchy and placement.

The serializer writes embedded static GLB with identity nodes, triangle indices,
split corner normals, active UV0 and per-primitive scalar material slots. Geometry
and placement follow the established metre/Y-up conversion: `(x, y, z)` becomes
`(x, z, -y)`; scene unit scale affects geometry and translations once. Origin
revision stays `origin-v1`. Unchanged prototypes are reused for sparse object
deltas. Material edits conservatively honor Blender's geometry invalidations;
the process can rebuild bindings from retained mesh copies when no geometry
invalidation is reported.

The initial subset is mesh objects, empty/transform parents, evaluated modifiers,
realized procedural meshes and untextured opaque Principled scalar factors.
Unsupported object types, non-realized instances (including mixed mesh/instance
outputs), excluded collections, invalid geometry, reflected/singular drawable
placement, texture/node-driven shader inputs, transparency and modifier-generated
material slots produce specific diagnostics. Artist-side realization/baking remains
an explicit choice. Broad shader translation, scatter and animation interchange
belong to later steps. Visibility combines object, viewport and selected collection
hierarchy visibility. A visible child under a hidden selected parent is refused:
linked hierarchy visibility propagates to children, while Blender's object render
visibility can be independent. External ancestors are visible transform-only
records so they cannot accidentally hide the selected content.

Steps 09 and 10 extend that initial subset with shared scatter, terrain/LOD,
modifier-generated material slots, stable material children, saved role-specific
images, a bounded Principled subset and typed native lights. The current contract
is [Materials and lighting v1](materials-and-lighting-v1.md); its explicit base
color bake produces a supported material copy while retaining the source.

## Bounds and recovery

| Work or resource | Limit |
| --- | --- |
| Linked source objects including ancestors | 512 |
| Evaluated prototypes | 64 |
| Collection hierarchy / view-layer instance enumeration | 1024 / 2048 entries |
| Evaluated prototype | 300,000 vertices/corners; 100,000 triangles; 48 MiB copied arrays |
| Copied scene / retained raw arrays / retained GLBs | 128 MiB each |
| Dirty IDs | 1024, then collapse to one full invalidation flag |
| IPC metadata / response | 2 MiB / 16 KiB |
| IPC send work | 256 KiB per 50 ms active dispatch |
| Director manifest | 1 MiB / 1024 records |

The existing Director also limits a pairing to 64 distinct completed resource
hashes, 256 MiB of resource bytes and 256 final receipts. These session limits can
be reached during continued editing even if the current collection is small.
Restart Scene Link and copy its new details to continue; retained resources remain
subject to the existing cache/disk policy. Large-environment capacity is step 09.

The worker pairs and registers first, uploads complete resources, then sends a
snapshot. Later changes use `MSDELTA` against the last acknowledged revision;
unchanged states send no transaction. Deletions erase source records and become
Director tombstones. Reintroduction retains the same object UUID and shot state.
The Director validates the full candidate and schedules one authoring undo action.
Step 06 isolates incoming authoring history from native local Scene Undo.

Resync starts a fresh worker, reads the Director's actual revision and publishes
the current collection as a new snapshot. Socket failures, rejections and stale
bases stop publishing with a visible diagnostic. There is no implicit overwrite
retry after a conflict. Keepalives detect Director-side revision changes. Live
updates can pause while the last submitted transaction finishes. The service is
local and Preview-only. Step 06 adds reverse placement; Program adoption remains step 08.
