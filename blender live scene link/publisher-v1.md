# Blender publisher v1

The publisher is the add-on in `integrations/blender/mediaserver_livelink`
(version in `bl_info`, currently 0.10.1), for Blender **5.2.0 LTS / fbe6228777e7**
or newer on Windows. The add-on checks the Blender version tuple before linking.
No third-party Python package is needed for the link itself. The workflow it
serves is in the [add-on README](../integrations/blender/README.md);
Pixelsoup's edits written back into Blender objects are described under
[Writeback](protocol-v1.md#writeback).

## Execution and scheduling

Persistent dependency-graph handlers collect scalar session IDs and invalidation
flags. Message-bus notifications cover UI names, visibility and mode changes.
Handlers do not export content. A Blender application timer polls nonblocking IPC
and processes pending edits once changes have been quiet for 150 ms (or 500 ms
after the first unpublished change). Idle polling performs no source scan, mesh
export, hashing, filesystem I/O or network waits. It only polls the bounded
socket, reaps terminated child processes and redraws the relevant panel.

All RNA and dependency-graph access occurs on Blender's main thread. A dispatch
extracts at most one evaluated prototype with `evaluated_get` / `to_mesh` and bulk
`foreach_get` array copies. `to_mesh_clear` runs in `finally`, including validation
failures. The original mesh, modifier stack and node setup are never applied or
replaced. Completed copies contain numeric arrays and plain metadata only.

Serialization, hashing, GLB generation, resource transfer and Director receipts
run in a **separate Python process** launched with Blender's bundled interpreter.
The add-on starts no Python threads, following Blender's documented
[threading limitations](https://docs.blender.org/api/main/info_gotchas_threading.html).
The [timer API](https://docs.blender.org/api/current/bpy.app.timers.html) provides
the scheduling context; evaluated mesh lifetime follows the
[dependency-graph API](https://docs.blender.org/api/5.0/bpy.types.Depsgraph.html).

IPC uses a private loopback listener with a random process handshake. Main-thread
sends are nonblocking and capped at 256 KiB per dispatch. One extraction batch and
one process request are admitted; while a request runs, newer edits only update a
bounded dirty set. A newer dependency generation discards an incomplete batch.
After a receipt, the newest pending source state is extracted. Arrays do not form
an unbounded queue. One evaluation/copy is not preemptible: expensive Blender
modifiers can still stall Blender itself, and the panel reports the last duration.
The declared input limits do not promise a hard latency bound on Blender's own
dependency evaluation. Split expensive content into manageable prototypes.

Edit/Sculpt mode defers extraction until Object Mode, except that Edit Mode
geometry is flushed to the evaluated mesh without leaving Edit Mode for ordinary
live sync. Undo/redo invalidates copied geometry but retains only scalar identity
information; no invalid RNA wrapper survives between timer calls. File load stops
the process and clears per-file runtime identity maps. Disconnect and unregister
close sockets and terminate the owned child without a UI-thread join. Disable also
removes timers, handlers and message-bus subscriptions. On application exit,
socket closure ends the child's worker loop.

## Identity and evaluated content

Scenes, collections, objects, meshes and materials carry `ms_link_id` UUIDs.
The source UUID is derived from the persisted scene and selected-collection UUIDs.
The collection stores version/build/source metadata separately from the pairing.
Renames preserve identity. Session-ID ownership repairs copied custom UUIDs and
keeps source IDs through Blender undo/redo. As with other Blender document edits,
save the blend file to retain newly stamped IDs across process exit (the add-on's
**Keep file saved** option does this after sync goes quiet). The identity rules,
including read-only library data, are in
[Identity and overrides](identity-and-overrides-v1.md).

Each source object has its own persistent evaluated-prototype UUID derived from
its object UUID. This preserves per-object modifier results and bindings even for
shared mesh datablocks. Identical GLB bytes still deduplicate by hash. Prototype
hashes identify revisions; they are never object or occurrence IDs.
Transform-only ancestors outside the collection retain hierarchy and placement.

The serializer writes embedded static GLB with identity nodes, triangle indices,
split corner normals, active UV0, tangents and per-primitive material slots.
Geometry and placement follow the metre/Y-up conversion `(x, y, z) -> (x, z, -y)`;
scene unit scale affects geometry and translations once. Origin revision stays
`origin-v1`. Unchanged prototypes are reused for sparse object deltas. Material
edits honor Blender's geometry invalidations; the process can rebuild bindings
from retained mesh copies when no geometry invalidation is reported.

Supported content is mesh objects, curves/text/surfaces copied as evaluated
meshes, empty/transform parents, lights, evaluated modifiers and realized
procedural meshes. Unsupported object types, non-realized instances (including
mixed mesh/instance outputs), excluded collections, invalid geometry,
reflected/singular drawable placement and unsupported shader inputs produce
specific diagnostics; artist-side realization or baking remains an explicit
choice. Visibility combines object, viewport and selected collection hierarchy
visibility. A visible child under a hidden selected parent is refused: linked
hierarchy visibility propagates to children, while Blender's object render
visibility can be independent. External ancestors are visible transform-only
records so they cannot accidentally hide the selected content. Scatter, terrain
and LOD, material and light translation, and animation assemblies are specified
in [Environment scale](environment-scale-v1.md),
[Materials and lighting](materials-and-lighting-v1.md) and
[Animation and stage](animation-and-stage-v1.md).

## Bounds and recovery

| Work or resource | Limit |
| --- | --- |
| Linked source objects including ancestors | 512 |
| Evaluated prototypes | 1024 |
| Collection hierarchy / view-layer instance enumeration | 1024 / 2048 entries |
| Evaluated prototype | 4 GiB copied arrays per mesh, split into resources of 300,000 corners / 100,000 triangles |
| Copied scene / retained raw arrays / retained GLBs | 8 GiB / 8 GiB / 4 GiB |
| Dirty IDs | 1024, then collapse to one full invalidation flag |
| IPC metadata / response | 2 MiB / 16 KiB |
| IPC send work | 256 KiB per 50 ms active dispatch |
| Director manifest | 1 MiB / 4096 records |

The Director also limits a pairing to 4096 distinct completed resource hashes,
16 GiB of resource bytes and 256 final receipts. These session limits can be
reached during continued editing even if the current collection is small; **Resync
Scene** starts a fresh pairing, and retained resources remain subject to the
cache/disk policy in the [protocol](protocol-v1.md#resources-and-capacities).

The worker pairs and registers first, uploads complete resources, then sends a
snapshot. Later changes use `MSDELTA` against the last acknowledged revision;
unchanged states send no transaction. Deletions erase source records and become
Director tombstones. Reintroduction retains the same object UUID and shot state.
The Director validates the full candidate and schedules one authoring undo action
in a context separate from local Scene Undo.

Resync starts a fresh worker, reads the Director's actual revision and sends the
current scene as a new snapshot. Interrupted connections retry with bounded
backoff; rejections and stale bases stop publishing with a visible diagnostic,
and there is no implicit overwrite retry after a conflict. Keepalives detect
Director-side revision changes. Live updates can pause while the last submitted
transaction finishes.
