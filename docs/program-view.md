# Program view

Program Output is the editor's 2D workspace: the place to lay layers out flat,
as the Timeline and Properties are the editing workflow, the Compositor the
node workflow and the Stage the 3D one. An operator can move between them as
the task asks, with the same selection, the same undo and the same values
behind each.

The operator-facing description (tools, keys, the box, shapes, snapping,
rulers, Align, the Canvas view) is in
[the operator guide](operator-guide.md#working-in-2d-on-program-output). This
file records how it is built and where its limits are.

## How it is built

- A tool is one entry in `canvas_tools()` (`engine/editor/canvas_tool.hpp`)
  and one branch of `ViewportPanel`'s pointer handling
  (`src/viewport/viewport_canvas.cpp`). The palette, shortcuts and tooltips
  read the table. The tools are Select, Hand, Zoom, Rectangle, Ellipse, Pen and
  Gradient.
- The transform box is pure geometry in `transform_box.hpp`. A drag resolves
  to the `TransformDelta` the Stage gizmo produces, so the shell authors it
  through the same path, auto-key included.
- The Canvas view of a Preview scene is drawn by the transition's second
  Program executor, which is idle outside a transition, so it costs a render
  and a display target and no new executor. The view follows the Stage into
  Preview unless a transition is running or the scene's canvas differs from
  Program's; either leaves it on Program
  (`engine/editor/src/shell/shell_frame_start.cpp`).
- A drawn shape reaches the Director by routes that already exist: a
  `MaskDrawIntent` through the mask edit route, or an `AddIntent` carrying the
  card's size and its mask. `plan_shape_card`
  (`apps/server/mask_edit_integration.cpp`) makes the card and its mask one
  replacement document.
- A gradient's handles are `fill_handles.hpp`: the renderer's ramp arithmetic
  turned round. A drag writes the same four fill properties Properties does.
- Snapping, rulers, guides, safe areas and Align live in `canvas_snap.hpp` and
  `src/viewport/viewport_layout.cpp`.
- Program edits masks by selection (a selected mask of the selected layer, with
  Select active), not by a tab being in front.

## Limits

- The palette and its tools are Program's alone. The Stage keeps its own
  toolbar and edits masks while its Masks tab is in front.
- The box is round the primary (last selected) layer; the other selected
  layers follow it.
- Only a move snaps. Resizing by a handle, turning, drawing a shape and placing
  a Pen point do not. Snap lines are gathered at the press from at most 256
  other layers, by the upright box each covers.
- The view (zoom, pan, Canvas or output), guides, and the ruler, snap and
  safe-area switches belong to this editor and are saved neither with the
  workspace nor the project.
- The safe areas are fixed at 93% (action) and 90% (title).
- A drawn card stands on the plane through the camera's target, and cannot be
  drawn on a physical or warped output surface. Shapes have a fill and no
  stroke.
- A single-colour fill has no gradient handles until it is made a gradient in
  Properties.
- A movable pivot is the Stage's and is forgotten when the selection changes;
  there is no authored anchor point.
