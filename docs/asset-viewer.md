# Asset Viewer

The Asset Viewer inspects one Library asset at a time, outside any scene. Media
plays in it like an editor's source monitor: scrub it, mark In and Out, and cut
the range into the timeline. A model or projection mesh sits in a neutral
look-dev scene. A plateset opens on its own page (see
[the operator guide](operator-guide.md#vehicle-process-platesets)).

What the operator sees, and the keys, are in
[the operator guide](operator-guide.md#the-asset-viewer). This file records the
contracts the viewer keeps and where its code is.

## Rules

1. **One identity.** The viewer shows the existing `asset.*` node. A derivative
   (thumbnail, filmstrip, prepared texture) never becomes a Library asset and
   never replaces a placement's `asset_id`.
2. **Viewer state stays on the editor.** The viewer's frame, marks, zoom, pan,
   channel, exposure, look-dev scene, camera and shading are never replicated,
   digested, sent to nodes or saved in the project, the same as Preview's
   playhead. They reset with the project.
3. **The viewer shows what the renderer does.** Media decodes through the same
   source, proxy policy, colour path and alpha as a placement. Models resolve
   materials the way the renderer does. The viewer is not a second
   interpretation of the file.
4. **Bounded, and never at Program's expense.** The viewer renders on the
   editor window's device into a pane-sized target, created when an asset is
   first shown and released a few seconds after the panel is hidden. Decode,
   parsing and analysis run on workers, and frame recording stays inside fixed
   capacity. Over budget, the viewer refuses with a stated reason; it never
   stalls the control thread or lowers Program's quality.
5. **Edits go through the Director.** The only edit the viewer authors is a
   marked range cut into a track. It is one undo step, validated by the
   Director; a locked target or a transition refuses with a specific reason.

## Opening an asset

Double-clicking a Library asset, or selecting it and pressing Enter, opens it
in the viewer; dragging an asset out of the Library is how it is added to a
scene. **Open in Asset Viewer** on a Library or Timeline clip menu opens one
too. Single-clicking only selects, so the viewer keeps its asset until another
is opened. The back and forward buttons step through the last 32 assets opened
(`AssetViewerPanel::open`).

## Media

- `AssetViewerPanel` (`engine/editor/src/asset_viewer_panel.cpp`) is an
  immediate-mode panel. It takes the asset, the Director's answer, the rendered
  texture and the derivative and decode observations, and returns an
  `AssetViewerFrame` of intents: the request for the next frame, a source edit,
  a drag and its drop, a Used In selection and the pixel probe position.
- `asset_viewer_media_scene` (`engine/editor/asset_viewer.hpp`) turns a media
  asset, a source frame and the project's renderer settings into a
  `scene::Scene` holding one always-on media placement on a quad as wide as its
  picture. The picture goes through the same decode, planar video, proxy
  fallback and colour path as a real placement, without the wall's bloom, fog or
  temporal history, and the scene is testable headless. The viewer looks at it
  through an orthographic `asset_viewer_camera`; zoom and pan change that
  camera, not the target size, so a 4K source at 400% still renders at pane
  size.
- The Director resolves the request to the file this machine decodes, as it
  does for the Stage, and answers with `AssetViewerInput`: the scene, the
  `AssetViewerRendition` (source, proxy, proxy required but not ready, offline
  placeholder, or unchecked) and the asset's filmstrip derivative for the scrub
  bar.
- The viewer keeps its own clock at the asset's rate (`asset_viewer_rate`),
  taken from window time. Source timecode is the asset's start timecode plus
  the frame at its own rate, drop-frame when the start timecode is
  (`asset_viewer_timecode`).
- The channel view (RGB, R, G, B, luma), the display-only exposure offset and
  the pixel probe act on the viewer's image only; `OutputTransform` stays the
  wall's boundary and has no editor modes.

### Cutting a range into a track

The panel's range becomes a `SourceRangeEditIntent` (`editor_shell.hpp`):
asset, source in, source frames, mode (Insert, Overwrite or Append), scene,
lane, start and show rate. `plan_source_range_edit`
(`apps/server/source_range_edit.cpp`) builds the whole-source `scene.media`
placement a Library add would author, trims it to the range, converts the range
to show frames with playback's own rational floor mapping, and plans it through
the same path as a clipboard Insert or Overwrite. The edit is one replacement
document scheduled on the next sealed frame, with no confirmation step.

- Insert cuts a clip it lands inside and moves everything after the edit point
  along. Overwrite lays the range over the track in front of every clip already
  there; those stay whole underneath and nothing moves. Append lands after the
  lane's last clip and moves nothing.
- The target is the Timeline's: the targeted track, else the selected track,
  else the selected clip's track, else outside every track. The edit lands at
  the Stage scene's playhead. The playhead does not move, and the new clip is
  selected.
- Refusals are named and leave the document unchanged: `SOURCE NOT PROBED`,
  `RANGE OUTSIDE THE SOURCE`, `TARGET TRACK IS LOCKED OR STALE` and `CLIPS OR
  HANDLES CHANGED`. The viewer also refuses during a transition.
- Dragging the picture out carries the asset and range (`AssetViewerDrop`);
  where it lands is resolved in
  `engine/editor/src/shell/shell_frame_drops.cpp`.

## Models

`asset_viewer_model_scene` builds the look-dev scene: the model at the origin
turned by the turntable, a floor meeting its base as a Stage drop rests it, a
1.8 m figure, and the chosen lighting (`AssetViewerEnvironment`: the Studio rig
of key, fill and rim lights, the Stage scene's own sky and lights, or an
equirectangular HDRI, which needs the Advanced renderer). The camera is the
view's orbit, so shadows and detail are fitted to what is shown. Only the
project's backend, quality, colour and texture budget carry over, and the
ordinary renderer draws it, so materials resolve as they do on the wall.
`AssetViewerModelView` carries the orbit, shading, look-dev settings, turntable
angle, isolated part and animation clip, which the shell turns into the next
frame's scene.

The model's bounds come from the loaded package, else the Library's imported
bounds, else a 1 m cube (`asset_viewer_model_bounds`). The Details, Parts and
Animation tabs are drawn from `render::ModelPackageFacts`, gathered on a worker
so a large package never blocks the frame. A model's Library thumbnail is the
same view under the studio rig, drawn on the editor's GPU and kept in the
machine-local derivative cache.

## Limits

- No audio plays; the waveform derivative is shown in the Timeline only.
- Model assets cannot be put on a track, and the viewer does not edit a
  model's materials, pivot or units. A model placement is tied to its Library
  asset by its source path alone.
- Media interpretation (frame-rate override, alpha, input colour), poster
  frames and subclips are not authored on assets.
