# Program view plan

Program Output is the editor's 2D workspace: the place to lay layers out flat,
as the Timeline and Properties are the editing workflow, the Compositor the
node workflow and the Stage the 3D one. An operator should be able to move
between them as the task asks, with the same selection, the same undo and the
same values behind each.

Current behaviour is in
[the operator guide](docs/operator-guide.md#working-in-2d-on-program-output).
This file holds what is still to come.

## Where it stands

Done: the tool palette (Select, Hand, Zoom, Rectangle, Ellipse, Pen,
Gradient), zoom and pan, the transform box, the options bar, the Canvas view,
which follows the Stage into Preview, shapes drawn as masks or as new cards,
mask editing on Program by selection rather than by tab, gradient handles,
snapping, rulers and guides, safe areas, and Align and Distribute.

- A tool is one entry in `canvas_tools()` (`engine/editor/canvas_tool.hpp`)
  and one branch of `ViewportPanel`'s pointer handling
  (`src/viewport/viewport_canvas.cpp`). The palette, shortcuts and tooltips
  read the table.
- The box is pure geometry in `transform_box.hpp`. A drag resolves to the
  `TransformDelta` the Stage gizmo produces, so the shell authors it through
  the same path, auto-key included.
- The Canvas view of a Preview scene is drawn by the transition's second
  Program executor, which is idle outside a transition, so it costs a render
  and a display target and no new executor.
- A drawn shape reaches the Director by the routes that already existed: a
  `MaskDrawIntent` through the mask edit route, or an `AddIntent` carrying
  the card's size and its mask. `plan_shape_card`
  (`apps/server/mask_edit_integration.cpp`) makes the card and its mask one
  replacement document.
- A gradient's handles are `fill_handles.hpp`: the renderer's ramp
  arithmetic turned round. A drag writes the same four fill properties
  Properties does.

## Carried over from the first round

- The palette and its tools are Program's alone. Stage keeps its own toolbar;
  sharing the active tool between the two is not done.
- Arrow-key nudging: Left and Right step the playhead while the Timeline has
  focus, so a nudge needs a focus rule first.
- The box is round the primary layer only. A box round the whole selection
  needs a shared frame for layers that do not face the same way.
- The view (zoom, pan, Canvas or output) is not saved with the workspace.
- The Canvas view is refused for a Preview scene whose canvas differs from
  Program's.

## Carried over from the second round

- The Stage still edits masks by the Masks tab being in front; only Program
  chooses by selection. Bringing the Stage to the same rule means deciding
  what its left button does over a mask while the Masks tab is open.
- On Program with Select, the selected layer's masks are not drawn until one
  is selected, so a mask cannot yet be picked by clicking it there.
- A drawn card stands on the plane through the camera's target. There is no
  way to say which depth, and no drawing onto a warped output.
- Shapes have a fill and no stroke; a rectangle card has square corners.
- The Pen places and curves points but does not yet edit an earlier point
  of the curve under way, and a pen point's two handles always mirror.
- The Gradient tool does not turn a single-colour fill into a gradient, and
  has no handle for colour A's end separately from the middle.

## Carried over from the third round

- Only a move snaps. Resizing by a handle, turning, drawing a shape and
  placing a Pen point do not.
- Snap lines are gathered at the press from at most 256 other layers, by the
  upright box each covers; a turned layer offers its bounding box, not its
  own edges.
- Guides and the rulers, snap and safe-area switches are this editor's and
  are not saved with the workspace or the project.
- The safe areas are fixed at 93% and 90%.
- Align has no icons and no shortcuts, and no "align to the key layer".
- An authored anchor point is not started: today's movable pivot is the
  Stage's and is forgotten when the selection changes. It needs a schema
  field that is normalized, replicated and digested, so it is its own change.

## Next: the viewer

- Channel views (colour, alpha, matte) and solo for the selected layer.
- A wipe between Program and Preview.
- A pixel readout.
- Motion paths for keyed position, with keys that can be dragged.

## Later tools

Not shown in the palette until they work.

- **Motion tracker.** Needs a bounded worker that reads decoded frames off the
  control thread and reports a track as ordinary keys. Useful for mattes only
  once masks can be keyed, which they cannot be today.
- **Clone and paint.** Needs strokes as replicated document data that every
  node rasterises identically, and a contract for it in
  `effect-rendering-requirements.md`. Design that before any tool.
- **Corner pin**, **crop handles** for media, **text**, and **strokes** for
  shapes each need new render data.
