# Operator guide

Run commands below from the repository root. See the [README](../README.md) for building.

## Run the local editor demo

From the repository root, after a GUI build:

```powershell
.\scripts\run-editor-demo.ps1
```

The script starts a Director editor and a lifecycle-supervised local output
node. Dragging or editing content in the editor updates the replicated scene
shown by the node. Both halves read one temporary password file the script
creates, so Output Monitor controls the node securely without setup.

Other Windows demonstrations are:

```powershell
.\scripts\run-scene-demo.ps1  # Director preview plus two scene slices
.\scripts\run-wall-demo.ps1   # Director preview plus two feed-mapped outputs
```

## Find your way around the editor

The initial workspace puts Scene and Library on the left, Stage in the center,
Properties on the right, and Timeline below Stage. Compositor shares the bottom
area as a tab alongside Output Monitor; Program Output shares Stage's dock. Select an object on Stage or
in Scene to edit it in Properties. Import content in Library, then drag it onto
Stage or a Timeline track. Additional panels are available in the Window menu.
Saved workspaces retain their dock arrangement. **View > Reset Workspace**
restores this starting layout when you want to adopt it or undo dock rearrangements.
Panel dragging and other editor input target 60 Hz independently of the show's
frame rate. At faster show rates, previews can follow every control frame. A
scene that exceeds the machine's rendering budget can reduce that cadence.

Properties groups placement, source, appearance, and effects in that order.
Model animation, surface layers, baking, and Blender connection details open
when their section headers are clicked. Delete is at the end of the panel.
**Add effect...** opens a picker beside the Properties button. Choose an effect
to append it to the selected elements; its controls then appear in the stack.
The button stays available after adding an effect. Locked and incompatible
selection members are skipped, and one addition is one undo step.
Long numeric labels stack above their controls when the dock is narrow; shortened
names and paths show their full text on hover. Selecting another object returns
Properties to the top. Ctrl+= and Ctrl+- adjust interface size; Ctrl+0 resets it.

**Browse...** opens a native file picker for a light's IES profile, the renderer's
Show LUT (`.cube`), or the Environment map (`.hdr` / `.exr`). Choosing a file
applies that path; Cancel leaves the document unchanged. Typed IES paths use
**Apply**; **Clear** removes the profile. Baked lighting's **Bake folder** picker
sets the export destination, and **Bake lighting** starts the bake. Content's
Source row shows the file name with its full path on hover.

Object and effect icons use a neutral palette; shape and labels identify their
types. Blue marks selection and active tools, while health colors report actual
output states. User-assigned marker colors remain available.

## Run across two machines

The demo above puts everything on one machine. To drive a render machine from a
separate control machine, start the editor half on the control machine:

```powershell
.\scripts\run-editor.ps1
```

That opens the editor and a Director listener, and prints this machine's
address and the port. Without a script, double-click `media-server.exe` and
press **OPEN EDITOR** in the window it opens: the editor starts as this
machine's Director, listening on port 45060 on every interface unless File >
Preferences stores another Director endpoint, show ID or epoch. Its
write-ahead log lives in the per-user settings folder unless Preferences names
one. On a machine not set up as a render machine the small window then closes;
on a render machine it stays, so one computer can be both.

**A render machine needs no command line.** Put `media-server.exe` on it, with
the files beside it (the release package, or a GUI build directory), and
double-click it. The first time, a small window asks for three things:

- the control machine's numeric address, and the Director port (normally
  45060);
- this machine's node index - 0 for the first render machine, 1 for the next;
- the render node password from **File > Preferences** on the control machine,
  or nothing if none is set there.

**SAVE AND START** stores them in this machine's per-user Preferences - never
in a project - and starts the lifecycle agent, which starts the render process
straight away. From then on, double-clicking `media-server.exe` (or a shortcut
to it, or `media-server.exe --render-machine`) skips the form and runs with
those settings; **CHANGE SETTINGS** edits them. The window shows the render
process with local START, STOP and RESTART, the control port Output Monitor
reaches it on, whether it has a password, how many control requests were
accepted or refused, and this machine's addresses for entering it by hand.
Closing it asks first, because it stops the output. Tick **Start this render
machine when I sign in to Windows** to have it come back after a restart.
Launched from Explorer it closes the console window it was given; started from
a terminal it keeps logging there. `media-server.exe --render-machine
--validate-startup` prints what a launch would start without opening anything.

On Windows the window also checks for Media Server's inbound firewall rules -
the control port (TCP 47840 + node index) and discovery (UDP 47839) - and while
one is missing offers **ALLOW THROUGH FIREWALL**: Windows asks for an
administrator, then exactly those two rules are added, replacing any earlier
copy. Nothing else is changed. If Windows showed its own "allow access" prompt
when the agent started, Allow does the same job; Cancel can leave a block rule
for `media-server.exe`, which wins over any allow rule - remove it under
Windows Defender Firewall > Advanced settings > Inbound Rules.

The script does the same from a command line, and is still the way to pass
the extra options below:

```powershell
.\scripts\run-output-node.ps1 -DirectorHost 192.0.2.10 -NodeIndex 0
```

**Render node password.** Output Monitor starts, stops and inspects render
machines over an authenticated control channel. Set the password in **File >
Preferences > Render nodes** on the control machine - it applies as soon as it
is saved, no restart - and give each render machine the same password in its
setup window (`-Password` to the script, `--lifecycle-password=` on the
executable). With no password on
either side everything still works, but any Media Server on the network could
control those machines too; Output Monitor shows `No password` while that is
so, and a machine started with a different password reads `password rejected`.
The password is kept on this machine only as the key it derives
(PBKDF2-HMAC-SHA256), never in a project.

`-NodeIndex` selects which slice of the wall that machine drives and must be
distinct per render machine; the 128-bit node identity is derived from it.
`-Fullscreen` drives a real wall, `-Windowed` is easier while setting up.

Both scripts take `-Build auto|packaged|release|debug`. `auto` prefers a
packaged `media-server.exe` beside the scripts directory, then the
`windows-msvc-release-gui` build, then `windows-msvc-debug-gui`, so the same
scripts work from the repository and from an extracted release package on a
machine with no build tree. `-DryRun` prints the exact command line without
starting anything.

By default the node script runs the machine-local lifecycle agent, which owns
the render process and starts it immediately. That is what lets Output Monitor
START, RESTART, and STOP the node, and what reports its sync device, house
reference, and display lock state. Finish the link in the editor: open
Hardware Map, where the machine appears under **DETECTED ON NETWORK** along
the bottom of the map, and drag it on (or click it). The new Render Server card
already has the machine's name, address, and node index. By hand it is a Render
Server with that node index, the render machine's address, and control port
47840 + the node index (the script's default). Pass `-Direct` to skip the agent
and run the render process alone - no remote control, but one fewer moving part
when first proving connectivity.

Two things are commonly in the way. Windows Firewall must allow the Director
port inbound on the control machine, and on each render machine the lifecycle
port (TCP) and discovery (UDP 47839) inbound; the render-machine window adds
the latter on request, while both scripts print the `netsh` commands but
deliberately do not change firewall rules themselves. And every address here
must be numeric - the setup window's address, `-DirectorHost` and the Hardware
Map Host field take IPv4/IPv6 literals, not host names.

To open only the editor, provide a Director listener and stable show identity:

```powershell
out/build/windows-msvc-debug-gui/apps/server/media-server.exe `
  --role=director --director-listen=127.0.0.1:45000 `
  --show-id=00000000000000010000000000000002 `
  --director-epoch=00000000000000030000000000000004 `
  --playback --edit --surface=1600x900 --wal=editor-test.wal
```

Run `media-server.exe --help` for the complete option reference. `--playback`
starts the show clock; `--scene` opens an ordinary scene-output window; and
`--output` selects the feed-mapped output path. The editor is Director-only.

## Program and Preview

Program is what the wall is showing. Preview is where the next look is built.
The point of the pair is that an operator can do real work - scrub, play, move
elements, key them, rebuild the compositor graph - on a scene that is not on
air, and put it on air only when they decide it is ready.

The `PROGRAM` / `SWAP` / `PREVIEW` group at the top right is the whole control
surface. `PROGRAM` puts the editor back on the live scene; `PREVIEW` returns to
the scene being rehearsed; `SWAP` promotes Preview to Program. `PROJECT
SCENES`, at the top of the left-hand SCENE panel, is where a scene is chosen:
`PGM` marks the live one, `RHS` the rehearsed one, and clicking any other row
takes it into Preview. `+ NEW` creates an empty scene and `CREATE COPY`
duplicates the scene currently on the Stage, both straight into Preview. On a
project that has never used the switcher, the first `PREVIEW` press copies
Program into a new `<name> Rehearsal` scene and opens it, so there is always
something to work on.

**What Preview owns.** Entering Preview seeds a Director-local playhead from
wherever the show currently is and freezes it there. From that moment the
timeline - the playhead and its transport buttons, including step, jump, and
go-to-start - drives Preview alone. The show keeps running underneath at its
own frame, and the status bar reads `PREVIEW  PGM <frame>` so the live position
is always visible. The Program viewport and Output Monitor keep reporting the
live show, never the rehearsal. Preview's playhead is never replicated and
never written to the project: render nodes only ever hear about Program.

**Masking on the output.** A matte is judged against the picture it is cutting,
which is the one on the wall rather than the one in a perspective viewport. With
the Masks panel open on a target, the Program Output panel becomes a second
authoring surface for it: the same masks, the same selection, the same undo
gesture as the Stage, drawn over the connector image. Its overlay resolves
through the output's own projection, so a mask over a canvas slice registers
with the crop and one over a physical surface registers with the warp, and the
panel says `PROGRAM OUTPUT  /  MASKS` while it is doing so.
Both mask-authoring viewports display `OBJECT SELECTION LOCKED WHILE MASKS IS
OPEN`, making the temporary interaction domain explicit instead of leaving an
ordinary selection click to appear broken.

Primitive masks are transformed on the picture itself. Selecting a rectangle
or ellipse exposes four resize handles and a rotation handle; dragging the body
moves it, dragging a corner resizes it, and Shift constrains a resize or snaps
rotation. The normalized bounds remain available in Masks for exact entry, but
they are a precision path rather than the primary crop gesture. Each mask row
owns its Delete action beside its name, while dragging the row continues to
reorder the mask stack.

A layer's mask is pinned to the layer's surface, not to the frame: it moves,
turns and foreshortens with the layer, and the outline on Stage or Program
Output is exactly where the render cuts. Feather and expansion are canvas
pixels at the size the layer appears on the canvas. Only flat layers - quads,
planes and media - have one surface for a mask to sit on; a mask on a volume or
imported model reports `SOURCE MASK MAPPING UNAVAILABLE` on Stage, and the
render refuses the frame rather than cut it somewhere arbitrary.

Program Output authors masks only while the Stage is on the Program scene. In
Preview the two panels are showing different scenes, and a mask dragged over a
picture that does not contain the layer it belongs to would be authored
somewhere nobody is looking; the panel stays an ordinary output view until the
Stage comes back. A running dissolve is excluded for the same reason - that
picture is a mix of two committed scenes and belongs to neither - and a locked
target still draws its overlay while refusing to author, exactly as on Stage.

Scenes own their content, so editing in Preview cannot reach Program. That
includes the compositor graph: graph objects carry the scene that owns them,
Program composites through its scene's graph, and the Compositor panel edits
the graph of whatever scene is on the Stage. Copying a scene copies its graph
with it, with scene-layer references repointed at the copied elements, so a
promoted copy looks exactly like the scene it came from before you change it.
Scene Layer nodes show their referenced element's current name from Scene, so
renaming an element updates its compositor label too.

**What is shared, deliberately.** Show length, loop, and frame rate are one
project-wide setting; Preview follows Program for those. So are the Hardware
Map and the Feed Map - the physical wall does not change because a different
scene is being rehearsed - and surface edits made while in Preview key against
the Program playhead, which is the frame the mapping panels draw.

**Promoting.** `SWAP` exchanges the two slots at an exact control frame, so
every node performs the same transition on the same frame. The default is a
30-control-frame dissolve. A dissolve renders both scenes and mixes two
finished pictures, which is only defined when the two scenes share a canvas;
when they do not, the button reads `CUT` and promotes on a single frame
instead. `FADING` means a transition is already running, during which scene,
graph, and switcher edits are all refused - both inputs of a dissolve are
committed pictures. After a swap, Preview holds the scene that just came off
air and its playhead re-seeds to the live frame.

`SWAP` is held by exactly one thing beyond the switcher's own preconditions: a
project that asks for physical output synchronization under a STRICT failure
policy, whose timing the cluster cannot prove. The button then reads `HELD`,
and Output Monitor's timing line names the unmet condition. Nothing else blocks
a promotion. In particular, whether render nodes hold the media a scene needs
is reported on the Output Monitor and never gates the switcher - distributing
content to nodes is an external concern in this build.

**Scenes from Blender.** A project has one `.blend` beside it with a Blender
scene per Media Server scene. The orange `B` before a `PROJECT SCENES` row's name
(shown on hover for other rows) opens it (created the first time) with the LiveLink
add-on running and shows that scene. It stays on scenes Blender holds: filled for the
one Blender syncs, outlined otherwise. A linked scene is live wherever it is - Blender edits reach Program and every
output node as the artist drags - and Media Server's edits to Blender objects are
written back into them, so the `.blend` always holds what the wall shows. The
add-on keeps that true on disk: a few seconds after syncing goes quiet following
a change Media Server accepted or wrote back, it saves the `.blend` (Keep file
saved, on by default). A Blender the `B` starts later - Blender closed, a crash,
"Don't Save" - therefore opens the scene Media Server holds, with the same link
identities, instead of an older file whose objects Media Server's edits no
longer attach to. Take a linked scene to air with the ordinary switcher. See
[integrations/blender](../integrations/blender/README.md).

## Editing clips on the timeline

New projects start with a five-minute timeline: 7,200 frames at the default
24 fps, from frame 0 through frame 7,199 inclusive. Saved projects retain their
authored range; `--show-range=FIRST:LAST` overrides the startup range.

A media element is a *placement* of a source, not a copy of it. The document
carries where the clip starts on the show timeline, how many show frames it
occupies, which source frame its first show frame shows, and how long its head
and tail opacity ramps are. All of them are exact frame counts rather than
durations in seconds, for the same reason show time is an ordinal: every node
has to agree on the frame a cut lands on, and a seconds value would already
have been rounded.

### A track is a screen

Placements belong to scene-owned tracks, and a track owns the plane its clips
are drawn on. It is a screen: it has a place in the scene, a format, an
opacity, a blend mode, and a grade, and the clips filed under it are what is
playing on it. Nine driving plates for nine angles are nine screens, each
positioned where that part of the volume is and graded for it, and cutting a
plate is an edit to what plays on one screen rather than to where a picture
sits in the world.

Selecting a track header selects the track, and the Inspector shows the same
sections it shows for anything else - transform, compositing, effects - plus a
`SCREEN` section carrying the raster the screen drives and one aspect control.
With the lock closed the plane's height follows its width and the raster, so
media on the screen cannot be stretched by a scale gesture; the lock is applied
in evaluation rather than in the authoring path, because a gizmo drag, a
keyframe, and a typed field all have to obey it and only evaluation sees all
three. The raster is authored, never read from whatever clip happens to be up:
an operator sets a screen to the format of the wall, and a plate of the wrong
size should look wrong rather than quietly reshape the wall around itself.

A clip on a screen has no transform of its own to offer, so the Inspector says
which screen it is playing on instead of showing nine fields that move nothing.
Its transform properties are still in the document and untouched - they are
what it returns to when it is dragged out of the track - they are simply not
what it renders with. A track's grade runs after the clip's, because the look
an operator sets on a wall is meant to survive whichever plate is up.

Screens are outlined in the Stage viewport, so a track with nothing on it, a
muted one, and one between clips are all still findable and still have a gizmo.
They are carried in the evaluated scene rather than re-derived by each panel,
which is what makes the editor and the renderer agree about where a screen is.

A track holds any number of clips from any number of sources. Start and
duration decide when a clip plays; explicit clip order decides where it sits in
the layer stack even when two clips have different start times. The list runs
top-down through that compositing stack - what is in front is at the top, for
tracks and for the clips inside them - which is the reverse of the ascending
order the model reports them in. A track header carries its own mute and lock
toggles; a second click on the header opens its name for editing, because the
first one selects the track. The toolbar keeps only the operations that need a
confirmed destination. Track order and each clip's explicit order are also the
deterministic media compositing order. That order stacks clips sharing one
surface, where every clip is at the same depth - which every clip on one screen
is, by construction; it does not reorder clips across surfaces at different
depths, which composite back-to-front from the camera like everything else in
the scene. Creating or deleting a track is an ordinary Scene edit. A non-empty
track never disappears on a bare delete: choose `DELETE+CLIPS`, or choose a
named destination and `MOVE+DELETE` its placements first.

A placement carrying no track belongs to none. It stands on its own transform
like any other scene element, and is drawn as an ordinary top-level row rather
than under a header of its own, because a clip outside every track is already
visibly outside every track, and inventing a container for it would be
inventing one the document does not have. Clips move between tracks the way
they move anywhere else: drag a clip up or down onto a track's band - its
header or any clip already in it - and the band outlines as a real nesting
target. The clip bar previews locally and one placement edit is authored on
release. The media row is also a full-width lift handle, so a clip that is
offscreen or only a few pixels wide does not require hunting for its bar.
Dropping on any row outside every band takes the clip out of its track again,
and back onto its own transform. A locked track refuses an arriving clip.

Every orderable layer row has the same full-width lift gesture. Dragging a clip
within its current track reorders that track's clips; dragging an object, model,
light, group, or trackless clip inserts it before or after another sibling. The target
is one insertion line, not a highlighted row: highlighting a peer would imply
nesting, while reparenting is a separate hierarchy operation. Release authors
one dense sibling order as one undo step. For 3D drawables camera depth remains
the primary visibility rule; authored layer order resolves equal-depth
translucent, blended, or isolated layers and determines the editor stack.

The timeline nests Blender collections, source objects, and material parts using
the same parent relationships as the Scene outliner. Authored transform groups
also contain their children. Parents start collapsed; opening a parent reveals
its children and a separate Properties foldout for its animation channels.
Search keeps matching descendants with their parent context, and selecting a
part in another panel reveals its ancestors. Live-link updates retain the
timeline's open folders.

A track's header has a twirl too: closed, it lists only the header, and a
search still looks inside it. Anything closed summarises what it hides, the
way a collapsed layer or compound clip does elsewhere. A closed track, group,
collection or hierarchy parent draws a thin bar over the frames its clips and
keys span, from the earliest clip start or key to the latest clip end or key,
and a small tick on every frame anything below it is keyed. A layer whose
properties are folded - or a parent's folded Properties - summarises its own
keys the same way; a clip's ticks ride on its bar. A tick takes the channel's
colour when every key on that frame belongs to one channel, is neutral where
channels meet, and is larger where several keys share the frame. Opening a
parent moves its summary down to whatever below it is still closed. A layer
with nothing placed or keyed draws nothing in its lane. Summaries are for
reading only: they are not dragged, and pressing one acts on the lane
beneath.

Track order is changed by dragging a header up or down the stack. The target is
shown as a single insertion cursor between tracks, never as a highlighted
track: the operation is ordering, not nesting. Crossing several tracks previews
locally and reports one absolute reorder on release, so the whole drag is one
undo step. A track whose name is open for editing is grabbed by the strip left
of its name field, because the rest of the row is that field.

Compositing a track is still one draw per on-air clip at the screen's
transform, not a render of the screen to its own buffer. The two are the same
picture except where clips overlap - during a cross dissolve, a screen-level
opacity or a non-linear screen grade applies per clip rather than to the
blended result. That is a real difference and it is the reason this is written
down rather than assumed away.

The document keeps calling this a lane - `lane_id` on a placement, and
`timeline_lane` nodes - because those names are in saved projects and renaming
them would not open an older show.

Drag the In or Out handle on the ruler to move that end of the range; each
handle answers anywhere down the ruler's height, not only on the range bar.
A marker or cue on the strip below the ruler is grabbed by its tag, which also
answers a short way left of its line.
The exact IN and OUT number fields remain available for typed or dragged range
entry. The adjacent Set In and Set Out controls take the current playhead in
one click and preserve Loop. If a local Preview playhead is outside the old
range, the other endpoint moves only as far as needed to keep the new range
valid.

### Cache bar and render-node playheads

A green bar along the top of the marker strip marks frames this editor
machine has already decoded, much like a RAM-preview bar. A frame is green
when every video playing on it has its picture decoded and in memory, so it
will play without waiting on the decoder. Frames with no video on them are
never green, and stills and image sequences don't count. The bar covers about
300 frames either side of the playhead. The editor keeps only a few dozen
decoded frames (48 at most, fewer at 4K) and reads three frames ahead, so the
bar usually runs a little ahead of the playhead and leaves a short trail
behind it. A clip just placed or scrubbed to stays uncoloured until its
frames are decoded.

One render node's position is drawn as a thin white playhead, with a small
tag on the ruler's lower edge. To choose the node, select its Render Server
card in Hardware Map and tick **Main node** on the Timeline row under Node
index. The first Render Server placed in a show is ticked for you. A show has
one main node, so ticking it on one card clears it on the
others, and one undo puts the previous choice back. The card must be cabled
(or have an output) to count. With no main node chosen, the playhead appears
only when exactly one node is connected.

A node shows its pictures a little after the editor, by its output buffer
(twelve frames by default), and further behind after it misses frames. Its
playhead therefore trails the editor's while the show plays and meets it when
the show stops. It appears once the node has put a complete frame on its
outputs. Hover the tag to see the node's name, the frame it is showing, and
how many received frames it still has to show. The node playhead appears only
while the Stage shows the scene that is on Program.

The playhead-over-bar button after the cue button in the Timeline toolbar
turns both overlays on and off. The bar describes this machine only: it says
nothing about what a render node has decoded.

On a media layer row the Timeline draws that placement as a bar:

- With **Selection (`V`)**, **drag the bar** to move the clip. **Drag either
  end** to trim it; each end also answers a few units outside the bar, so a
  short clip's end can be taken from beside it. Trimming the head converts the
  removed show frames to the source frame rate and wraps within the source,
  retaining the first source frame at the new cut. Moving the bar keeps its
  duration and source offset.
- **SPLIT** cuts every selected clip at the playhead into two placements,
  preserving the source frame at the cut. Nothing is copied or re-encoded, and
  a split that would leave an empty half is not offered.
- **The two handles on the bar's top edge** set the head and tail ramps, drawn
  as the diagonals an editor expects. A handle answers around its knob, above
  the bar and across its top corner; lower on the same end, the press trims. A tail ramp over the scene's black
  surround is a fade to black; overlapping one clip's tail ramp with the next
  clip's head ramp is a cross dissolve.
- Drags snap to the playhead, the show range, and the ends of every other clip,
  so butt-joining two clips and cutting on the playhead do not need pixel
  accuracy. `SNAP` turns that off, and holding Alt suspends it for one gesture.
  Move, trim, and fade bars preview during the lift and commit once on release.
- Where these targets overlap - on a short clip, or a key close to another -
  one press goes to exactly one of them: a key, then a fade handle, then a trim
  end, then the bar, then the empty lane, and among two of the same kind the
  nearer. The pointer's cursor and highlight show which one a press would
  take before it is made.
  The dropped bar stays at the proposed position while the Director adopts
  the edit. Holding a dragged clip at either edge scrolls the view. Playback
  In/Out does not limit clip placement: clips can cross frame zero or be parked
  after Out. Pan left with a middle drag or Shift+wheel to see negative frames;
  the view also pans past Out. Playback and seeking remain at frame zero or
  later, so the part of a clip before zero is outside playback. Its source
  continues from the signed placement start when playback reaches zero.

**Auto Key**, beside Timeline search, controls whether editing an animated
property writes a key at the current playhead. `K` toggles it while Timeline
has keyboard focus. With Auto Key off, edits change the underlying base value;
existing animation continues to override that value during playback. The
property diamonds still explicitly add or remove keys in either mode. This
setting covers the Inspector, Stage transforms, physical surface transforms,
and animated environment values.
Adding a key through the Inspector or Timeline samples the same interpolation
as playback, including cubic tangents and weights, so the inserted key keeps
the evaluated value at that frame.

Each property row in the layer list shows its value at the playhead, between
its name and its key diamond: the animation evaluated there, or the base value
of a property with no keys. A closed **Position**, **Rotation**, **Scale** or
**Color** group shows its components as `x, y, z`. On an animated property
with Auto Key on, the value is a field: drag it sideways to scrub, or click it
and type, and the value is keyed at the playhead; one drag is one undo step.
Otherwise the value is only shown - key the property with its diamond to edit
it here, or change its base value in the Inspector. When the list is narrow,
the name is shortened before the value.

The arrows either side of a row's diamond move the playhead to that
property's previous or next key; on a group they step to the nearest key on
any of its components. An arrow is dimmed when there is no key that way.
`[` and `]` do the same for every animated property of the selected layers,
their effects included, while Timeline has keyboard focus.

A key's shape shows how it leaves for the next key, in the dope sheet and on
the Curves graph alike: a diamond moves in a straight line, an hourglass
eases along a curve (Cubic), and a square holds its value until the next key.

Right-click a key, in the dope sheet or on the Curves graph, to ease the
selected keys; a key outside the selection is selected on its own first.
**Linear** and **Hold** set how a key leaves for the next one and remove its
handles. **Auto Bezier** makes it a curve whose handles follow its
neighbours. **Easy Ease** (`F9`) flattens both handles, so the value slows
into and out of the key. **Ease In** (`Shift+F9`) flattens only the incoming
handle and **Ease Out** (`Ctrl+F9`) only the outgoing one, each keeping the
other side's current slope. Easy Ease and Ease In also turn a Linear key just
before an eased key into Auto Bezier, because that key decides the shape of
the segment coming in; a Hold before it stays a hold. Once a selected key is
Bezier, the menu also offers **Aligned Handles**, **Broken Handles** and
**Flat Handles**, which keep the curve's current slopes as their starting
point; they skip Linear and Hold keys. Each choice or shortcut is one undo
step, and locked layers are left as they are.

The `EDIT` picker selects context-sensitive advanced tools. Ripple Trim, Roll,
Slip, and Slide commit one signed-frame edit when the drag ends. Ripple Delete
requires one valid contiguous selected span. Insert Clipboard and Overwrite
Clipboard target the active unlocked track at the playhead, preview their exact
affected span, and run only after `GO`; Overwrite retains both uncovered split
remainders and their retargeted effects/key channels. Handle limits, adjacency,
locks, rational source rates, and stale selections are validated by the
Director, and every accepted operation is one undo step.
The six editing tool buttons use enlarged icons. Over the timeline editing
surface, the pointer matches the active Ripple Trim, Roll, Slip, Slide or Razor
tool. Selection (`V`) shows move, trim and fade cursors at the corresponding
clip handles. Tool cursors remain visible while dragging outside the panel.

Slip (`Y`) changes which source frame plays at the clip's start, keeping its
placement and duration. Drag right to advance the source and left to move it
back; the offset wraps at either end, matching the clip's looping playback.
This also works when the placement uses the whole source or loops it several
times. A readout below the tracks shows the zero-based source frame, frame
count, and whether the gesture wrapped. Invalid source metadata gets an
explicit message; one-frame media explains that its image cannot change.
The cached filmstrip previews the offset while dragging, and the final preview
and readout remain after release as the edit reaches the Director. The Stage
updates when the edit is committed. **Slide (`U`)** is a separate operation
that adjusts the neighboring cuts; use Selection to freely move a clip.

**Curves** turns the lanes into one graph the full height of the layer
list. Every channel listed on screen below an open layer - and every
animated channel of a selected layer - is drawn there as its curve, overlaid against one value
scale whose round values are labelled down the graph's left edge. The curve is
the one playback evaluates, Hold steps, Cubic tangents and weights included,
sampled at every show frame on screen. Each channel keeps its colour: the X,
Y and Z of a position, rotation or scale - and a light's red, green and blue -
are red, green and blue, and every other channel has its own hue, shown on
its name and its swatch in the list. The list stays on the left as the
channel picker: a channel's swatch shows or hides it on the graph and in the
dope sheet alike. Entering Curves fits the value scale to everything shown,
overshoot included. Curves and the dope sheet share one time view and one key
selection, so zooming either moves the other, and keys chosen in one are still
chosen after switching. On the graph a key is pressed, Shift- or Ctrl-clicked,
boxed and dragged in time exactly as in the dope sheet; its value is typed in
the KEYS popup.

A selected Bezier key shows its tangent handles on the graph. The outgoing
handle shows whenever there is a key after it, and the incoming handle when
the key before is also Bezier. Drag a handle to set that side's slope, and
its length, which sets how long the curve holds that slope. The curve follows
while you drag, and the change is one undo step when you let go. An Auto,
Aligned or Flat key becomes Aligned, so the opposite handle stays in line
while keeping its own length. A Broken key moves only the dragged side, and
holding `Alt` breaks any key that way. A handle cannot be made longer than
its interval allows alongside the handle facing it across that interval. If
moving or time-scaling keys would leave a handle too long for its new
interval, that channel is left unchanged and reported as skipped. A
handle answers the pointer as far out as a key does, or halfway to its key
where the two are closer, and wins over the key there.

Navigating the timeline is the wheel and the middle button, on both axes.
Wheel zooms time about the frame under the pointer and Shift+wheel pans it; in
`CURVES` mode Alt+wheel zooms values about the value under the pointer and
Ctrl+wheel pans them, and a middle drag pans time with rows or, held with Ctrl,
values. Because all six of those are gestures, the `KEYS` panel carries no
stepper buttons for them - only the two things a wheel cannot do, `FIT SEL` and
`FIT ALL`, which put a selection or the whole animation back on screen, in
time and on the Curves graph in value. Track
mute and lock are the layer list's eye and padlock rather than a letter, so the
state of a stack reads from across a stage. The stage's LED surfaces follow the
scene's elements in both the Timeline and the Scene outliner, each with the same
padlock - the lock Feed Mapping and Stage picking already honour. A surface has
no eye, and clicking its Scene row selects it on Stage.
A selected, unlocked surface shows its **Position**, **Yaw** (in turns) and
**Size** in **Properties**; a UV-mesh surface shows its **Origin** and Yaw and
takes its size from the mesh. Shape, pixels and wall colour stay in Feed
Mapping. With Auto Key on, Properties edits to an animated Position or Yaw key
it at the Program playhead, as Stage drags do.
To animate an LED surface, select it on Stage or its row in **Scene** and
unlock its padlock if needed. Park the playhead, then click the diamond beside
**Position** or **Yaw** in **Properties**, or on the expanded surface in
**Timeline**, to add a key; a filled diamond removes the key there. Cameras
key the same way from their Properties diamonds. A tracked camera follows the
tracker while it is live and falls back to its keyed pose when it is not. These keys support the same
selection, dragging, scaling, copying, and deletion as scene-element keys.

A ramp *scales* whatever opacity was authored or keyed rather than replacing
it, so a dissolve composes with an animated layer. A clip the playhead has
moved past keeps its row and its bar - it is marked `off air` rather than
disappearing, because a clip you cannot see is still a clip you have to be able
to drag. Every authored media item is an explicit placement with a positive
duration; an invalid placement is reported rather than reinterpreted as an
always-on layer.

## Project settings

**File > Project Settings** holds the numbers that decide *how* an effect is
computed, as opposed to how much of it an operator asked for. They live in the
project rather than in machine-local preferences because they change the image:
two nodes filtering with different kernels would render a visibly different
frame of the same show, which is exactly the divergence the scene digest exists
to catch. Applying them is an ordinary document edit, so it undoes, saves, and
replicates.

- **Blur** - Gaussian kernel taps per axis, sigma as a fraction of the authored
  radius, and how many times the separable pair runs. A wide radius stays smooth
  at a modest tap count by repeating passes rather than by widening the kernel.
  **Directional Blur taps** is a separate bounded odd gather count; Box Blur
  uses its exact per-effect integer radius and pass count.
- **Brightness** - whether the multiplier is a scene-referred *exposure*,
  applied before the tone map so highlights roll off, or a display-referred
  *gain* applied to the finished picture, which clips the way a vision mixer's
  gain does. A response gamma shapes the multiplier once, over the whole folded
  stack, so two brightness effects of 4 and 0.5 still mean one of 2.
- **Saturation** follows that same response choice: linear-light colourfulness
  in Exposure mode, encoded/display-referred colourfulness in Gain mode. The
  per-effect Amount remains in the Inspector; only its project-wide response
  domain is selected here.

None of this appears beside an individual effect in the Inspector. A kernel
width is set once per show and then left alone; five such controls on every
blur node would be five things an operator scrolls past forever.

## Preferences

The machine-local settings an operator would otherwise retype on every launch
live in **File > Preferences**: the Director listen endpoint, show ID, Director
epoch, render node password, and write-ahead log path. They describe this
installation, not the show, so none of them is written into a `.mstage` or sent
to a render node.

Precedence is **command line > preferences > built-in default**. An argument
given this run always wins, which keeps scripted deployments and the bundled
demos exactly reproducible on a machine whose operator has saved settings; the
panel names the value actually in use beside any field an argument displaced,
so an argument-driven launch never reads as settings being ignored. An empty
field means "no preference" and restores the built-in default.

Saved settings take effect the next time the program starts. Nothing here is
applied live: the listener, the WAL, and the lifecycle credential are all bound
during start-up, and the panel says so rather than implying otherwise.

Settings are stored per user beside the recent-project record, written to an
exclusive sibling temporary file and atomically installed, so an interrupted
save leaves the previous settings intact. A damaged settings file is reported
at start-up and the built-in defaults are used; it is never silently ignored.

A render machine has its own small window instead of this panel (see [Run
across two machines](#run-across-two-machines)). Its settings - the control
machine's address, node index, control port and show ID - live in the same
per-user record, and the render node password is one setting whichever end of
the connection the machine is: on a machine that runs both, the editor and its
render machine agree by construction. Each side saves only its own fields.
The Director's listen endpoint, show ID and epoch apply only to a Director
launch, so a render process started on the operator's own machine never
inherits them. A render process started with explicit arguments, and a
`--lifecycle-listen` agent, read none of this: their configuration is on the
command line, local and immutable by design.

## Project assets

Library imports are persistent project assets, separate from scene placement.
An import records a reference: the Director probes the file for metadata
(dimensions, frame count, rate, triangle and vertex counts), streams it once to
record the expected byte length and SHA-256, and stores its **absolute path** in
the project. The source itself is not copied into the project. Reimporting the
same exact source updates the existing asset record without changing its stable
project ID; accepting changed bytes is therefore an explicit new revision of
that one identity, not a derivative asset.

### The Library panel

The Library has two views, switched from the icons at the left of its footer:

- **List** is a table with one row per asset. Its columns show Type,
  Resolution, Frame Rate, Duration, Start TC, End TC, Codec, Vertices,
  Triangles, Texture Size (the model's largest texture), File Size, the Proxy
  policy chip and runtime Status. Frames, Materials, Textures and Path are
  hidden by default. Right-click the header to show or hide any column after
  Name. Click a header to sort by it, and click it again to reverse the sort.
  Drag a header's right edge to resize the column. When the columns are
  wider than the panel, drag the scrollbar under the rows (or press its
  track) or use Shift+wheel to scroll sideways. Generators, folders and
  assets are separate groups with a gap between them, and folders always
  come before assets. A value that was never probed always
  sorts last. End TC is the last frame (start plus duration minus one). A
  file with no timecode of its own starts at 00:00:00:00, and drop-frame
  timecode uses `;` as editors do.
- **Grid** shows one folder at a time as thumbnail tiles. The path above the
  tiles goes back up a level (Backspace does too). Double-click a folder tile
  to open it. Each tile names its duration and resolution, or its vertex
  count and texture size.

The footer slider sets the thumbnail size, and so does Ctrl+wheel over the
content. Each view keeps its own size.

Click an asset to select it. Ctrl toggles assets in and out of the selection,
and Shift extends it. Double-click an asset, or press Enter, to place it in
the scene. The Generators section lists the built-in primitives, a
Background card, lights and camera, and a single click adds one. Drag a Box,
Plane, Sphere or Background onto Stage instead to place it where it lands, the
way a dropped asset is: an outline previews where it will rest. Lights and
the camera are added where their defaults put them. Right-click an asset, folder or empty
space for **Add to Scene**, **Rename** (F2), **New Folder**, **Move to Top
Level**, **Remove from Library** (Delete), **Import Media...**, **Import
Model...** and **Rescan Sources**.

**Folders** are organisation only. A folder is an `asset.folder` document
node, and an asset names its folder in a `folder` field. Neither changes how
anything renders, but both are replicated and digested like every other
asset field, and every folder edit is one Undo step. The toolbar's folder
button makes a new folder, already open for naming. In List view the new
folder goes inside the selected folder; in Grid view it goes in the folder on
screen. Drag assets or folders onto a folder to file them there. In List view,
releasing on empty space files them back at the top level; in Grid view, drop
them on a step of the path instead. A folder can never be filed inside itself.
Deleting a folder keeps everything it held, one level up. Reimporting a file
the Library already holds keeps it in its folder.

**Dragging out.** A selection dragged out of the Library carries a thumbnail of
the asset you pressed on, with a count when more than one asset is dragged.
What happens on release depends on where it lands:

- **Stage:** each asset lands on the surface under the pointer, side by side.
  While you drag over Stage, an outline shows where each will land and a
  label names the surface it will rest on. An asset rests on that surface
  rather than sinking into it: a model's lowest point touches it, and a
  screen stands on its bottom edge, turned to face the Stage camera. A
  screen dropped on a wall hangs flat against it, facing out. Locked set
  pieces still count as surfaces. With nothing under the pointer, the asset
  rests on the stage floor where the grid is drawn. With snapping on (or
  **Ctrl** held while it is off), the landing point steps to the 1 m grid
  across a level or upright surface, and a screen's turn steps by 5
  degrees. An imported model is hit on its own triangles a moment after it
  first appears. Until then, the Stage uses its bounding box.
- **An empty part of the Timeline:** each asset becomes a new layer.
- **A compatible Timeline row or Compositor Scene Layer:** the asset replaces
  that source, as described below.
- **A Compositor image input:** the asset is wired into it.

**Files from the desktop** can be dragged straight into the window. Dropped on
the Library, they are imported into the folder under the pointer. Dropped
anywhere else, they are imported at the Library's top level. Video and images
are probed. Models go through the model worker. Files that are neither are
named in the status line rather than imported. Import also records the video
codec, the file's start timecode, and a model's vertex count, texture count
and largest texture size. Assets imported before this was recorded show those
columns blank until they are imported again, or until a changed file is
accepted as described below.

**Changed source files.** When a file the Library holds is edited or replaced
on disk, `RESCAN` notices. It checks every Library asset, used or not, with
one stat per file: a size or modification time that differs from what was
recorded marks the asset as changed. The asset gets an amber corner on its
thumbnail and **SOURCE CHANGED** in its Status column, and an amber chip at
the top left of the Library counts the changed files. Accept them with:

- the chip, which accepts every changed source;
- **Accept Changed Source** at the top of a changed asset's right-click
  menu, which accepts every changed asset in the selection;
- **Accept All Changed Sources** in the menu on the Library's empty space.

Accepting re-reads each file on the same background workers import uses. It
records the new duration, resolution, rate, codec, timecode, model counts and
fingerprint on the same asset. The asset keeps its ID, name, folder and
proxy policy, and its placements keep pointing at it; their clip timing and
trims are not changed. Everything one Accept asked
for is a single Undo. Thumbnails and proxies are keyed by the fingerprint, so
they regenerate for the new revision. A file that has gone missing is not
offered; it is reported as missing instead.

To replace an existing source, drag a Library asset onto its **Timeline layer
row** or the layer's **Scene Layer node in Compositor**. A compatible target is
outlined and marked **Replace source** before release. Media assets replace
media clips, and model assets replace standalone models; cross-kind drops,
locked targets, and Blender-owned sources are refused. Replacement keeps the
placement's identity, name, transform, animation, timing, trim, fades, effects,
and graph connections. It updates the source reference and the imported media's
frame count and cadence. The same placement changes in both views, and one Undo
restores its previous source. Source preparation uses the already imported asset.

The model this rests on is that a path means the same file everywhere: a clip
the operator opens from `D:\Media\clip.mov` is opened from `D:\Media\clip.mov`
by every render node. Use identical local layouts, a mounted share, or a UNC
path that resolves the same way on every machine. Sources must be absolute; a
relative or `..`-bearing path is refused at import, because it would otherwise
resolve against whichever directory each process happened to start in.

Getting source media onto those paths is external sync tooling's job, not this
program's. What the program does is distinguish a missing path, wrong byte
length, wrong hash, and a local verification failure instead of treating any
file with the right name as ready - when it is asked to. Routinely it asks the
cheaper question.

Every machine answers the same question about the paths the current document
actually needs — scene media, scene models, and imported projection surfaces.
Library assets nothing currently uses are not counted, so an operator can keep
a shelf of unused imports. A placement whose Library record was deleted is
still counted, because the renderer still opens that file.

Output Monitor displays required/ready counts per render node, including
missing, byte-mismatched, hash-mismatched, and verification-failed totals.
Readiness is reported over the authenticated lifecycle listener, so it is
available while the render child is stopped; the agent answers only the exact
manifest requirements and never returns a directory listing, rewritten path,
or file contents. Missing media remains a warning and draws a grey box with an
X on its existing surface; output continues, and START and RESTART remain
available when media is the only missing content. Missing models, calibration
or lighting assets, stale bytes and verification failures still disable those
controls. STOP remains available. See [offline media](media-playback.md#offline-media).

Asset readiness is reported, not enforced against the switcher. A disconnect,
restart, stale manifest, duplicate node index, or pending Hardware Map change
shows on the Output Monitor, but it does not hold a Program promotion: the
operator, not the readiness gate, decides when a look goes on air.

**Presence is routine; content is asked for.** When the manifest revision
changes, every machine stats the required paths and reports present or missing.
That is one stat per asset, it reads nothing, and it cannot report a content
mismatch because it never looks at content.

`RESCAN`, in the Library, is the other question: it streams each required file
and reports wrong byte lengths, wrong hashes, and local verification failures.
It is the only thing in the program that reads a source's bytes for this
purpose, and the only place a mismatch can come from. It also finds Library
files that changed since they were recorded and offers to accept them; see
[Changed source files](#the-library-panel).

Nothing rescans on a timer. An earlier build re-verified every fingerprinted
source every five seconds for as long as anything was missing, which on a 40 MB
plate stalled the control thread for two and a half seconds at a time, forever,
because a missing file never stops being missing. A share that mounts late is
picked up by pressing `RESCAN` - which is a worse answer than automatic
recovery only if the automatic version is free, and that one was not.

Thumbnails are derivatives, not assets. Their key contains the persistent Asset
NodeId, expected fingerprint, frozen `thumb-320x180-srgb` profile, and generator
version. The Director chooses the first valid video frame at or after 10% of
duration (then follows the specified fallback order), corrects orientation and
pixel aspect, and writes a transparent-letterboxed PNG through an atomic,
validated machine-local cache envelope. Library requests only visible rows and
a bounded prefetch window; leaving that window cancels work. Ready PNGs are
decoded through the renderer and uploaded on the editor's own GPU device into a
64-entry retained texture cache. Cache files are disposable and never enter a
project save, command stream, placement, or content manifest.

Leaving Library or Timeline removes unused previews from lookup immediately.
Their textures remain alive until earlier UI frames finish using them on the
GPU. Both preview caches have a bounded retirement reserve; when it is full,
new preview uploads retry after completion instead of waiting on the control
thread. Editor shutdown finishes GPU work before releasing scene and UI images.

## Render-node lifecycle control

The normal Director/node session exists only after a node process is running.
Lifecycle control is therefore a separate, per-machine listener that supervises
one fixed child configuration. The listener accepts only `status`, `start`,
`restart`, and `stop`; executable paths and child arguments remain local and
never cross the network.

Running render nodes reconnect automatically when the editor returns on the
same Director address, port and show ID. Output Monitor distinguishes the
process from its editor connection: **CONNECTING** means the process is running
but has no live session, **CATCHING UP** means it is adopting replicated state,
and **LIVE** means the session is under editor control. A reconnecting node
takes the editor's current state directly, so CATCHING UP normally lasts only a
few frames however long the node was away. Expand the node row to
see its acknowledged control frame and the Director's current target. A
persistent CONNECTING state calls for checking the render machine's saved
Director address and show ID, plus network reachability.

Startup and replacement output windows initialize to black before showing.
A disconnected node holds its last complete image while it retries. A node
ahead of the reopened Director requests an authoritative checkpoint; state is
replaced only at a frame boundary. The node refreshes scene, mapping, asset
and tracking state for a replacement stream, including when it reuses a
document version number. Restarts retry an old connection's lease for up to
fifteen seconds before reporting a persistent duplicate identity.
A permanent session fault exits the render process so the agent can report
the failure and Output Monitor can start it again.

A render machine set up through its window runs exactly this agent, with the
render process arguments `run-output-node.ps1` would pass. The command-line
form below is for services and scripts, and is unchanged.

Choose a render node password, set it in the editor's Preferences, and give
every render machine's agent the same one with `--lifecycle-password=TEXT` or
`--lifecycle-secret-file=PATH` (a file holding the password, which keeps it
out of process listings; restrict it to the service account). An agent with
neither runs unsecured and accepts any Media Server on the network. For
example, this agent supervises node 0:

```powershell
media-server.exe `
  --lifecycle-listen=10.20.0.21:47840 `
  --lifecycle-node-index=0 `
  --lifecycle-secret-file=C:\ProgramData\MediaServer\lifecycle.secret `
  --lifecycle-child=C:\MediaServer\media-server.exe `
  --lifecycle-autostart `
  --lifecycle-child-arg=--role=render,output `
  --lifecycle-child-arg=--director-connect=10.20.0.10:45000 `
  --lifecycle-child-arg=--show-id=00000000000000010000000000000002 `
  --lifecycle-child-arg=--node-id=00000000000000030000000000000004 `
  --lifecycle-child-arg=--node-incarnation=00000000000000050000000000000006 `
  --lifecycle-child-arg=--playback `
  --lifecycle-child-arg=--output
```

`--lifecycle-autostart` is optional. It starts the configured child before the
listener accepts control requests; without it, Output Monitor starts the child
on demand. The controller authenticates requests with HMAC-SHA256 over a key
derived from the password, with a fresh nonce, but it is designed for a
trusted, isolated production-control LAN. Use host firewalls, choose a real
password, and do not expose the listener to the public Internet.

**Finding render machines.** Every lifecycle agent answers discovery on UDP
47839 (`--lifecycle-discovery-port=N` to move it, `0` to turn it off), and the
editor asks every two seconds - by broadcast on each IPv4 interface and on
loopback - listing what answered under **DETECTED ON NETWORK** at the bottom of
Hardware Map. Drag a machine onto the map, or click it to drop it at a free
spot, and its Render Server card starts with the machine's name, address, and
node index; the first one placed also moves the project's control port to
where that machine listens. A machine already on the map reads `ON MAP` and
selects its card. Otherwise a card says what would stop it working before it
is placed: its node index already used by another card (a click selects that
card, often the same machine at an old address), a control port the project's
range does not reach, or a password mismatch. The answer carries the machine's
name, node index, control port, and whether it has a password - never the
password - and authorizes nothing; control still goes over the authenticated
lifecycle channel. Discovery stays on one broadcast domain, so a machine on
another subnet is entered by hand.

In Hardware Map, set the project-wide control port and assign each Render
Server a unique numeric node index plus a numeric IPv4/IPv6 address (a blank
address means local loopback). Output Monitor derives the endpoint, rejects
duplicate node indices from lifecycle actions, and displays listener/process
state plus runtime inventory. Only a cabled card is part of the show: a Render
Server no cable touches and no output binding names reads `PARKED` on the map -
an alternative or future configuration - and is never launched, never holds the
Program gate or the asset cohort for the rest of the cluster, and cannot make a
cabled card's node index a duplicate. A newly placed card takes the next free
node index. **Main node**, under Node index, chooses the one render node whose
playhead the Timeline draws beside the editor's; a parked card is never the
main node. The first Render Server placed in a show starts ticked; later
cards start unticked. Display targets are observations, not persistent
project state; on Windows they are active DisplayConfig targets, not a promise
of unused physical connectors.

For physical output deployment, start with the node graph after the Render
Server listener reports active displays. Drag a cable from the server's live
video-output pin to the corresponding video input. That one gesture creates
the cable, a persistent binding to the display's stable observed identity, and
a full-canvas Feed Mapping region. Repeat for each display driven by the same
render process. Select a binding row to tune its exact Feed index, Window mode,
Width, Height, Refresh numerator, and Rate divisor. Disconnecting the graph
cable removes its generated binding and region; a region shared by another
binding is retained. Feed indices refer to the numbered rows in Feed Mapping
and are never inferred from physical display order.

Each binding drives the display its saved target names, chosen by that
identity rather than by where the window happens to land. Two active displays
can share a desktop position - a mirrored pair does exactly that - and a
binding resolved by position alone would then drive whichever of them the
machine listed first. A target that is mirroring another is refused with that
reason instead; extend the desktop on that machine to drive it on its own.

Fullscreen applies an exclusive native mode and refuses launch when the exact
raster and rational refresh are not advertised. Windowed and borderless modes
apply the saved raster at the detected target's desktop position. The Program
gate also requires authenticated inventory to contain every saved target; an
exclusive target must report the requested applied mode, and the live render
process must resolve every bound Feed index before acknowledging readiness. A
missing target stays
on its authored logical port and is labelled `MISSING` rather than compacting a
different connector into its place. Select that binding and use `REPAIR WITH
NEXT ACTIVE OUTPUT` to explicitly adopt a replacement. Discovery and hotplug
never rewrite bindings, cables, or project files on their own.

A node that could not open its displays reads `OUTPUT FAILED` in Output
Monitor, and its row names the reason: the bound display not connected, present
but switched off, mirroring another, claimed by two bindings, or not offering
the saved mode. That is reported separately from asset readiness on purpose. A
node with no outputs has no renderer to report either, which on the Director is
indistinguishable from one still loading, so the row used to say the assets
were not ready while the fault was a display.

One lifecycle agent still supervises one fixed render-process identity. That
child now opens one presenter per saved output binding and selects each binding's
Feed index. Changing bindings is replicated as project state; Restart from
Output Monitor relaunches the same process, which reapplies the saved deployment.
Projects without output-binding nodes use the single-window output model.

## Output synchronization

Set the project policy in Hardware Map under `SYNC POLICY`: UNSYNCED,
FRAMELOCK, or EXTERNAL GENLOCK, the required raster and rational refresh, the
vendor swap-group and present-barrier identifiers, whether a failure is STRICT
or DEGRADED, and whether a late frame holds the whole group or lets outputs
advance independently. It is ordinary replicated document state and undoes like
any other edit.

Synchronization groups are derived from the cables already drawn in the same
panel: the render-server video outputs that reach a common LED processor or
wall are one group. Two outputs of one machine driving unrelated walls are two
groups. Group timing is validated exactly, so 60/1 and 60000/1001 are never
treated as the same rate.

Output Monitor reports what the cluster can actually prove. The machine-local
lifecycle agent answers for the sync device, the house reference, and each
display target's lock state; the render process answers for present-barrier
membership and its missed, late, held, and repeated frame counts. Until every
condition holds the surface names the specific unmet one - `SYNC UNVERIFIED`,
`REFERENCE LOST`, `SWAP BARRIER INCOMPLETE`, `DISPLAY TIMING MISMATCH` -
instead of a generic green state, and a STRICT policy refuses to promote
Program.

This build carries no vendor sync integration, and SDL_GPU swapchains cannot
join a present barrier, so a physical policy correctly reports
`SWAP BARRIER INCOMPLETE` on this presentation path. To rehearse the operator
procedures without a genlock card, pass `-SyncSimulate locked`,
`reference-lost`, `reference-idle`, or `no-device` to
`scripts/run-editor-demo.ps1`; simulated samples are marked as such all the
way to the surface and can satisfy only a DEGRADED policy.

Under HOLD GROUP a render process that cannot produce a complete frame for one
of its outputs repeats the previous complete frame on all of them: on an LED
wall one repeated frame everywhere is far less visible than one segment
advancing while its neighbour stands still.

## Cluster reliability and timing

Output Monitor now exposes current control-plane health, live and catching-up
session counts, release/pacer lag, cumulative incidents, and whether the
rotating post-show diagnostic log is recording. It reports the exact
synchronization verdict beside the control-plane one. A node acknowledgement
proves replicated state adoption, never simultaneous physical scan-out or even
physical cable/LED presence, so the two lines never merge into one green
online state.

The currently certified baseline is one Director plus two render nodes at 120
control frames per second for ten minutes with bounded replay and edit-shaped
durability. Node loss/restart, checkpoint/replay convergence, heartbeat
timeout, slow-node, WAL, and release-ledger recovery are automated Release
tests. Larger clusters and real project/media/device loads require their own
pre-show soak; the 64-session runtime ceiling is not a 64-node performance
claim.

See [cluster-operations.md](../cluster-operations.md) for the exact supported
envelope, soak commands, JSONL diagnostic location, failure recovery procedure,
firewall/service/secret-rotation guidance, and the explicit future
PTP/genlock/frame-lock integration boundary.

## Authoring and project files

Use Library to import media or models, then drag them into Stage. The Inspector
and Timeline author ordinary document commands, so their changes participate in
replication and undo/redo. The conventional **Edit** menu carries Undo, Redo,
Copy, and Paste, with **Ctrl+Z** for undo and **Ctrl+Shift+Z** or **Ctrl+Y** for redo. A
new edit after undo starts a new branch and clears redo only for that authoring
context. Selecting a Scene/Stage item or a Compositor node/canvas chooses which
of those two histories the global actions navigate. A clip dropped into a
scene lands *placed* on the
timeline, with its own natural length in show frames, and is edited there - see
[Editing clips on the timeline](#editing-clips-on-the-timeline).

A video clip's **Inspector > Content > Resolution** chooses the size its decoded
frames are held at. **Half** holds a quarter of the video memory per frame -
useful for a plate that is always shown small, or on a node short of memory -
at the cost of a softer picture wherever the clip is shown larger than half its
source size. It does not make decoding any cheaper. Every node honours the
choice, so the wall stays consistent. See
[Holding a video clip at half resolution](media-playback.md#holding-a-video-clip-at-half-resolution).

Select a built-in generator such as a box or plane and open **Inspector > Material** to assign Base color, Metal / rough, Normal, Occlusion or Emission textures. Images are sampled using the generator's UVs, and **Clear** removes an assigned channel. Metal / rough uses roughness in green and metallic in blue; normal maps use tangent space. PNG, JPEG, TGA and BMP images are supported. Keep the referenced image available at the same path on every render node.

**Background cards.** **Background** in the Library's Generators is the
compositor's Background node as a card on Stage: a 16:9 card two metres tall
that stands facing the camera like a screen. It is named Background in Scene. Select it and
**Inspector > Fill** has the same controls as the node: **Type** (Solid,
Linear gradient or Radial gradient), **Colour A** and **Colour B** with their
alphas, **Angle Turns** for a linear ramp, and **Center** and **Size**. They
mean what they do on the node, measured on the card instead of the canvas:
Center and Size are fractions of the card's own width and height in its real
proportions, so a radial gradient on a wide card is a circle, and resizing the
card scales the gradient with it. A colour with alpha below 1 blends the card
over what is behind it.

The fill multiplies the card's base and emissive colours. A new card has a
black, non-reflective base and white emission, so it shows the fill as its own
light: exposed and tone-mapped like the rest of the scene, and exact in the
Stage's **Unlit** view. Raise its Base Color and lower Emissive under
**Material** to make it a lit, painted board instead.

Select one imported model and open **Inspector > Model Materials** to assign
texture images. Choose the numbered **Material part**, then use **Browse...** for
Base color, Normal, Metal / rough, Occlusion or Emission. **Clear** removes that
channel's image from the selected part. Metal / rough expects a packed image with
roughness in green and metallic in blue; normal maps use tangent space. The part
must have UVs. PNG, JPEG, TGA and BMP are supported up to 16384 pixels per dimension
and 64 megapixels. A chosen color or emission image starts at full strength; the
fallback factor controls below apply to source parts without materials.

Texture preparation runs on the model worker while the existing model stays
visible. The result embeds the image in a new immutable model package under
`out/material-edits` beside the source, then replaces only that selected model's
source in one undoable edit. Other placements keep their appearance. Keep that
referenced package available at the same path on every render node. Blender-linked
models continue to take textures from their Blender materials; create an independent
snapshot before editing their images in Media Server.

Effects can be applied to a selected element or connected in that scene's
compositor graph. The graph's **Add Node** palette uses searchable, expandable
Inputs, Color, Keying, Blur, Texture, Spatial / 3D, and Combine & Output
categories instead of giving every node equal weight in one toolbar. Array
nodes live under Spatial / 3D. Every newly-created composition starts with its
required Output node. Dragging a layer or Track row from the Timeline onto empty graph space creates
its Scene Layer node at the release point. Select several layers or Tracks with
Ctrl-click or Shift-click, then drag any selected row into the Compositor to add
them together. The selection stays intact during the drag; new nodes are
stacked in timeline order with space between them, and one Undo removes the
whole drop. Inputs already represented in the graph are skipped. A Track remains
one stable graph input whose picture follows the clips currently on air, including transparent
timeline gaps. Dragging from Effects creates the matching effect node there.
Both gestures outline the graph and name the pending node before release.
Drop onto an **input** instead and what you drop arrives connected to it: a
Timeline layer or Library asset on an image input, an effect on an
**EFFECTS** input and a blur or key on a **FILTERS** input. The input is
ringed and labelled, for example **Connect to
Back** or **Replace Image**, before release. The new node is placed to the
input's left. A layer that already has a Scene Layer node is wired through
that node rather than given a second one. An input that takes one wire takes
the first dropped layer and replaces what it had; an input that takes several,
such as Composite's Layers, takes them all. A Library asset dropped on an
input becomes a placement in the scene with its Scene Layer node and wire, and
one Undo in the graph takes all of it back. An input outranks the node around
it, so drop on the node's header or detail line to replace a layer instead.
Drop an effect from Effects onto a wire of its own kind, away from any node,
and the new effect joins that wire's input right after the wire's own effect:
the wire is lit and labelled **Insert after Brightness**, for example. A
Library asset cannot be dropped on an image wire, because no effect node takes
an image in.

**Effects and filters.** A Scene Layer has two inputs for them.
**EFFECTS** (a violet diamond) takes Brightness, Saturation, Levels,
Lift/Gamma/Gain, Contrast/Pivot, Colour Matrix, White Balance, Curves and Film
Grain, which are applied as the layer is drawn. **FILTERS** (a green triangle)
takes the blurs, Chroma Key and Luma Key, which work on the finished layer.
Everything on EFFECTS always runs before everything on FILTERS, so, for
example, a key and a blur can be used on a layer that is also colour corrected
without any special wiring. An effect cannot be wired into FILTERS, or a
filter into EFFECTS; the wire is refused with the reason. LED Car Process has
the same pair, SETUP EFFECTS and SETUP FILTERS, applied to every plate after
the plate's own. Projects made before this split show a filter wired into
EFFECTS as an error until it is reconnected to FILTERS.

Where an input takes several wires, each carries a number near the input
giving the order it applies them, first to last; the order matters within
FILTERS, since keying then blurring softens the keyed edge while blurring then
keying keys a softened picture. To change it, drag a wire's number onto
another number on the same input, and the wire takes that place; or
right-click the wire and choose **Move Earlier** or **Move Later**. Each move
is one Undo.
Drag a **single Timeline layer onto an existing Scene Layer node** to replace
that input's layer binding. Matching layer kinds are required: a video layer
can replace another video layer, and a model can replace another model. The node
is outlined and marked **Replace layer** while hovered. Its position, custom
label, outgoing connections, and downstream effects stay in place; both Timeline
layers remain intact. One Undo restores the previous binding. Incompatible drops
and drops of several selected layers onto an occupied node do nothing. Library
assets dropped on a Scene Layer node instead replace its referenced placement's
source, as described under Project assets.

The graph is an infinite canvas: use the mouse wheel to zoom around the point
under the cursor and middle-mouse drag to pan. The toolbar also provides zoom
buttons, a percentage reset, and a full view reset. Node moves and Effects or
Timeline drops remain in graph coordinates, so navigating never changes the
authored layout or makes a dropped node jump away from the cursor. A node
stays exactly where it is let go while the move is applied.

Each node is sized to its own title, source name, and port names, and its text
grows and shrinks with the zoom. Zoomed far out, the smaller lines (the
detail under the title, then port names) are left out rather than squeezed.
Ports are coloured and shaped by what they carry: blue circles for images,
violet diamonds for effects, and green squares for array instances. A filled
port is connected; a ring is open, and an open required input is drawn in the
warning colour. A node's header carries the colour of its Add Node category.

Wires are curves. To connect, drag from an output port - its dot or its name -
and release over an input. The whole input row is the target, and the wire
also snaps to a compatible port when released close to it. While a wire is
being drawn it lands on the port it would connect to, and ports it cannot
connect to fade back. Hold it over an input it cannot feed and a label says
why: the wrong kind of value, a wire that is already there, or a loop.
Let a wire go on empty canvas and the node search opens there, offering only
nodes with an input the wire can feed; the one you pick arrives connected.
Press **Tab** or right-click the canvas to open the same search with nothing
attached. Type to filter, use the arrow keys and **Enter**, or click; **Escape**
closes it.

Click a wire to select it. Selecting a node highlights every wire on its way
in and on to Output. **Alt**-click a port to break its wires, or Alt-click a
wire to break that wire. **Ctrl**-drag from a connected input to move its wire
to another input, or from a connected output to move all of its wires to
another output; let go on empty canvas to disconnect them. Drag a node with no
wires of its own onto a wire it can carry - Composite onto an image wire, say -
and it is spliced in. Double-click a wire to add a reroute point where you
clicked, drag a point to route the wire, and double-click a point to remove it.
Reroute points are layout only and never change the picture. A node whose
output reaches nothing is drawn faded. A node with a problem carries a mark
beside its title - filled for an error, a ring for a warning - and hovering
the mark names every problem; a problem with a wire is marked halfway along
it.

The values you reach for most are on the node itself: Brightness and
Saturation's Amount, Blur's Radius, Contrast and Pivot, White Balance's
Kelvin and Tint, and so on. Drag a value to scrub it or click it to type, as
in Parameters; pressing one also selects its node. Zoomed out below 90%, the
values are shown but not edited there. The chevron at the right of a node's
title collapses the node to its title and the ports that are wired, and
opens it again; with several nodes selected it collapses or opens them all,
as does **Collapse** or **Expand** on the node's menu. Collapsing is layout
only and never changes the picture.

**Bypass** passes a node through as if it did nothing: press **D**, choose
Bypass on the node's menu, or click **BYPASS** at the top of its Parameters.
A bypassed node renders exactly as the node at its neutral settings - no
change to the picture and no extra render pass - however its values are set,
so you can compare with and without an effect while keeping its settings.
It is marked **BYPASSED** under its title in amber, its body is dimmed and
its wire fades. Press D again, or **Restore**, to put it back to work; with
several nodes selected, D bypasses them all unless every one already is.
Effects, Radial Array and Merge can be bypassed; a bypassed Merge passes its
BASE through and draws nothing wired to its LAYERS. Scene Layer, Background,
Matte, LED Car Process, Composite and Output cannot. Bypass is part of the
project and reaches every render node, unlike collapsing.

Choosing a layer on the Timeline brings its Scene Layer node - or the one for
the Track it plays on - into view and outlines it for a moment, and choosing
a Scene Layer node scrolls the Timeline to its layer or track, opening the
track if it was closed. Neither changes the other panel's selection.

**Selecting and arranging.** Click a node to select it. **Ctrl**-click adds
or removes one node, and **Shift**-click adds one. Drag on empty canvas for a
rubber band: dragged rightwards it takes the nodes it wholly contains, and
leftwards every node it touches; hold Shift to add to the selection or Ctrl
to take from it. Dragging any selected node moves them all together as one
Undo. The arrow keys nudge the selection 10 units, or 1 with Shift, and a
run of nudges in one direction is one Undo. **Delete** removes every selected
node except Output.

**Ctrl+C** copies the selected nodes, the wires between them, and a selected
comment box. **Ctrl+V** pastes them at the pointer, and **Ctrl+D** duplicates
the selection just below and to the right. Pasted or duplicated nodes become
the selection. A graph has one Output and a source one Scene Layer, so those
are not pasted or duplicated where they would be a second; a note says so.

**F** frames the selection, or the whole graph when nothing is selected, and
**Home** frames the whole graph. **MAP** in the toolbar shows a minimap of
the graph in the canvas's corner: the outline is what is on screen, and
pressing or dragging in it moves the view there. Whether it is shown is kept
with the workspace layout.

Right-click a node for its menu: Rename, Duplicate, Copy and Delete; Align
Left, Right, Top or Bottom and Distribute Across or Down for a selection;
**Straighten Connection** (also **Q**), which moves each selected node so its
input wire runs level; **Arrange**, which lays the selection out in columns
leading right to Output (the whole graph if one node is selected);
**Comment Selection** (also **C**); **Collapse** or **Expand**; and
**Bypass** or **Restore** (also **D**). A wire's menu adds a reroute point or
disconnects it. Right-clicking empty canvas opens the node search.

A comment box is a titled, coloured frame around part of the graph. Drag its
title bar to move it and the nodes wholly inside it, drag its corner to
resize it, double-click its title to rename it, and right-click its title to
change its colour or delete it. Double-click a node's title, or press **F2**,
to rename the node; **Enter** keeps the new name and **Escape** abandons it.
Comment boxes are layout only and never change the picture.

The **Parameters** panel beside the graph is resized by dragging its edge. Its
width is kept with the workspace layout, so it survives a restart. Labels get
the room they need, or move above their fields when the panel is narrow. The
panel scrolls with the mouse wheel or its scrollbar when a node has more fields
than fit. LED Car Process groups its fields into Position, Rotation, Plates,
and Ceiling Reflection, and Levels has one section per channel. Click a
section's heading to collapse or expand it.

**Chroma Key** shows the key colour as a swatch above its Key Red, Green and
Blue values, then Tolerance, Softness, Edge px (whole pixels: positive grows
the kept area, negative shrinks it), Spill and Mix. **Luma Key** has Low and
High thresholds, which stop at each other, Low and High Softness, a Matte
switch between Normal and Inverted, and Mix. A Mix of zero passes a key
through untouched. **Matte** cuts its **Source Image**, a Scene Layer, to the
coverage of its **Matte Image**: another Scene Layer, or a Background. It
switches between Alpha and Luma mode, Normal and Inverted, and whether its
input is clamped first. The matte applies after the source layer's own
filters and mask, and keeps the source's depth, so the cut layer still sits
behind anything in front of it. A Scene Layer used as a matte is never shown
itself; giving it a second, visible path to Output is marked as a problem.

**Combining images.** **Composite** stacks its Layers in wire order, each
landing with its own blend on whatever is below it. **Merge**, also under
Combine & Output, lays images over another as one picture: wire what is
underneath into **BASE** and what goes over it into **LAYERS**. Each layer
lands in wire order with the Merge's **Blend** - Normal, Add, Multiply,
Screen, Subtract, Overlay, Lighten, Darken or Difference - and **Opacity**,
and the finished Merge then lands on what is below it as one image, so its
blend never reaches anything outside it. With nothing wired to LAYERS, BASE
is blended with the node's **Value** instead, only where BASE has coverage:
Multiply by 0.5 halves its brightness, Add 0.1 lifts it, and Opacity mixes
the result back toward BASE. Normal has nothing to lay over BASE, so a new
Merge changes nothing until another blend is chosen. Opacity and Value are on
the node; Value shows a dash while it does not apply, and Parameters says
why. A Merge, or a Composite feeding a Merge or another Composite, renders
into an intermediate image of its own. Up to four can be stacked inside one
another; a deeper graph is marked on Output with the reason.

**Background**, under Inputs, generates an image: a **Solid** colour, or a
**Linear** or **Radial** gradient from Colour A to Colour B, each with its own
alpha. Colours are picked as display colours and the ramp runs between them
as picked. A Linear gradient runs along its Angle in turns, clockwise from
left-to-right; Center places its midpoint and Size is its length as a
fraction of the canvas measured along that angle. A Radial gradient runs from
Colour A at its Center to Colour B at Size times half the canvas diagonal, so
a Size of 1 reaches the corners. Positions are measured on the authored
canvas, so every node of a wall draws exactly its own part of one gradient;
an output view with no canvas, such as a tracked plate, spans its own raster.
Use a Background as a Merge's BASE to put a colour or gradient behind layers,
in its LAYERS with Multiply to tint or vignette them, or as a Matte's Matte
Image to fade a layer out across the frame: a gradient from opaque to
transparent in Alpha mode, or from white to black in Luma mode. Like any image on
Output's path, an opaque Background covers the scene drawables the graph does
not reference; give those Scene Layers of their own to place them over it. For
the same fill on a surface in the scene instead of the whole canvas, place a
[Background card](#authoring-and-project-files) from the Library.

The Spatial / 3D category also contains **LED Car Process**, a purpose-built
nine-socket virtual-production rig. Any subset of its optional Front, four
three-quarter/side, Back, and Ceiling Reflection sockets can be used. Connected
wall plates arrange around the vehicle and the reflection plate sits overhead.
Position and yaw/pitch/roll move the complete rig; Plate Scale changes the wall
plate size, Array Scale changes their distance from the centre, and independent
Reflection Height and Reflection Scale controls tune the overhead plate.
Horizontal Blend feathers only the wall plates. The node's Setup Effects socket
is evaluated after each connected layer's own effects. Slot transforms likewise
parent, rather than replace, the layer or Track transform, so per-plate
positioning, effects, and masks remain local corrections that travel with the
array. Stage selection outlines and gizmos follow those composed positions while
edits continue to author the layer-local offset.

Each effect in the Inspector carries the layer list's eye. Switching one off
authors the document's own enabled flag, so the effect, its parameters, and its
key channels all survive - which is what comparing a look against the one before
it needs, and what deleting the effect instead would throw away. A switched-off
effect keeps its row rather than disappearing from the panel that would switch
it back on. The Color Correction panel grades what is reaching the picture, so
it says when a layer's colour effects are off instead of offering controls that
would do nothing. That panel exposes the structured tonal controls without
flattening their authored order, and how those effects are computed is set once
per project under [Project settings](#project-settings). The Texture panel
exposes Film Grain's exact seed, pixel scale, chroma ratio, and monochrome/RGB
mode while Amount is also keyframeable in Inspector and Timeline. Each scene
owns its own graph, which is what lets the Compositor panel be used in Preview
without touching what Program is rendering - see
[Program and Preview](#program-and-preview).

The Effects browser groups effects by Color, Keying, Blur, and Texture, has a
search field, and is strictly a drag source. In Color Correction, Levels exposes draggable input and
output rails, Lift/Gamma/Gain uses colour wheels with luminance rails, and
Curves points are dragged directly on the graph; clicking empty graph space
adds a point there. Numeric fields remain alongside these surfaces for exact
entry and keyboard work, not as substitutes for the visual gestures.
The contextual note on a structured color or texture effect has an adjacent
button that activates its `COLOR` or `TEXTURE` dock tab. Chroma Key exposes key
RGB, tolerance, softness, signed edge growth/erosion, spill suppression, and
Mix; Luma Key exposes its low/high thresholds, independent softness, matte
invert, and Mix directly in Properties.
Chroma Key's colour well has an adjacent eyedropper icon: activate it, then
click the exact Stage or Program Output pixel to set key RGB; Escape cancels
without authoring. The viewport sampler and icon are target-agnostic so another
three-component colour control can reuse the same interaction.

Cameras are placed like every other element: a Position and a Rotation (yaw,
pitch and roll in turns, turning a camera that looks down -Z with +Y up) in the
Inspector's TRANSFORM section, keyable from the Inspector and the Timeline. The
CAMERA section exposes field of view, shutter time, focus distance, and aperture
with explicit key diamonds. The same four channels appear when the camera is
expanded in Timeline and evaluate at the exact show frame. A camera saved with the older eye
and look-at target reads as the rotation that gives the same view.
Every camera of the scene on Stage draws its frustum there, highlighted while
selected. Select a camera in Scene and it takes the Stage gizmo and the G/R
keys like any element; it has no scale.

On Stage a click picks the nearest element under the pointer. Press and hold
still for about half a second to choose instead: a menu lists everything under
the pointer, front to back - including elements the camera is partly inside,
such as a floor, a wall or a Blender room, which a plain click leaves to the
camera. The menu uses the same authored names shown in Scene and Inspector.

Stage shows `LOADING STAGE ASSETS...` while its models, material textures,
media or display color transform are being prepared. The notice clears when
Stage is ready; Program and output nodes may finish loading earlier. A failed
asset shows a failure message instead. Selection and camera navigation remain
available during loading.

The Stage camera is your own view and is navigated as in the Unreal Editor.
It never changes the scene, works during a rehearsal lock, and hides the
cursor while a drag moves it.

| Input | Camera |
| --- | --- |
| Right drag | Look around in place |
| Right drag + W A S D | Fly forward, left, back, right |
| Right drag + Q / E | Fly down / up |
| Right drag + wheel | Fly faster or slower (the speed shows briefly) |
| Middle drag, or Alt+left drag | Orbit the pivot |
| Left drag on bare canvas | Move across the ground and turn |
| Left+right drag | Pan |
| Alt+middle or Shift+middle drag | Pan, with the scene following the mouse |
| Alt+right drag | Dolly toward or away from the pivot |
| Wheel | Move forward or back |
| F | Frame the selection |
| Ctrl+1 to Ctrl+9 | Keep this view in that slot |
| 1 to 9 | Return to the view kept in that slot |

Kept views belong to this editor session, not to the show; the show's cameras
are never moved by them. The floor grid follows the view: it centres under
what the camera looks at and widens a decade at a time as you pull back, from
1 cm close in to 1 km far out, with a brighter line every ten. The bottom
right of Stage names the current spacing.

**Top, Front and Side views.** The menu in the top-right corner of the Stage
image names the view's projection (**Perspective** at first) and switches it;
a tick marks the current one.
With the pointer over Stage the keys do the same:

| Keys | View |
| --- | --- |
| Alt+J | Top: looking straight down, north (-Z) up the screen |
| Alt+H | Front: looking along -Z |
| Alt+K | Side: looking along -X |
| Alt+P | Perspective again, as you left it |

Top, Front and Side are orthographic: nothing shrinks with distance, so sizes
and alignments read true across the whole view. They show the same scale the
perspective view shows at its pivot, and they never turn. Every drag pans
with the scene following the pointer; the wheel and Alt+right drag zoom; and
with the right button held, A/D and Q/E pan while W/S zoom. **F** frames the
selection, and picking, the gizmo, Measure, snapping and Library drops all
work in them as they do in perspective. Front and Side show the upright grid
through the origin, with the vertical Y axis in green. A kept view (Ctrl+1 to
Ctrl+9) remembers its projection.

**Four views.** Choose **Four views** in the same menu to split Stage into
Top (top left), Front (top right), Side (bottom left) and the perspective
view (bottom right); choosing the ticked **Four views** again in any view's
menu goes back to the single view, which is the bottom-right one. The first time, the three new views centre on what the
perspective view looks at, at its zoom. Each view has its own camera, menu
and gizmo: the camera keys, F, Ctrl+1 to Ctrl+9 and the Alt projection keys
act on the view under the pointer, a camera drag stays with the view it began
in, and a click picks what that view shows under the pointer. Selection,
Measure, snapping and Library drops work in every view, and the one toolbar
above them sets the tools for all four. While Masks or a colour eyedropper is
in use, Stage shows the bottom-right view alone. Each view renders separately,
so four views cost about as much GPU time as one Stage of the same total
size plus the fixed per-frame work of three more renders.

**Shading.** The menu left of the view menu, marked by a half-lit ball, names
how the view shades and
switches it, and with the pointer over a view the Unreal keys do the same:

| Keys | Shading |
| --- | --- |
| Alt+4 | Lit: the picture the show renders |
| Alt+3 | Unlit: each surface's own colour and glow, with no lighting |
| Alt+6 | Lights only: every surface a neutral grey, so only the light on it shows; clips show as flat grey cards |
| Alt+2 | Wireframe: every triangle edge, seen through the others; clips' edges in orange |

Shading belongs to the view: in four views each can shade differently, and
Program and the outputs always show the lit picture. Unlit and Wireframe draw
no atmosphere (the renderer reports fog idle there), and nothing is mirrored
in a planar reflection while they are on. Picking, the gizmo, Measure and
snapping work the same in every shading. The Bounds toggle outlines
bounding boxes in any of them.

An orthographic view's eye stands 200 m behind what it shows, so atmosphere
(fog) is not drawn in it - it would be the air of that whole distance - and
the renderer reports fog as idle there with that reason. Lighting, shadows,
reflections and temporal smoothing are drawn as in perspective, with every
pixel seeing along the same direction. Only the Stage has these views; a
show's own cameras are always perspective, and an output node refuses a scene
whose camera is not.

Directional lights have no scene position. Stage draws their pickable marker
near the world origin, offset opposite the light direction. The marker stays
there as the view turns or frames another object; its direction arrows keep a
readable on-screen size while zooming. The marker and arrows disappear when
that location leaves the view.

The fly keys go by position, so they sit under the left hand on any keyboard
layout; while a camera drag is held they never trigger timeline shortcuts.
Alt+left and a left press during a right or middle drag never pick or move what
is under the pointer.

### Moving, rotating and scaling on Stage

The Stage toolbar is a row of icons; hover over one for its name and keys.
From the left:

| Icon | Does |
| --- | --- |
| Four-way arrow, turning arrow, arrow out of a corner | Move, Rotate and Scale tools |
| Ruler | Measure tool (**M**) |
| Globe / cube | Gizmo on world axes / on the element's own axes |
| Magnet | Snapping on or off (Shift+Tab) |
| Grid with a distance, angle with degrees | The move and rotate snap steps; greyed while snapping is off |
| Box resting on a line | Surface snapping |
| Grid, frame corners, three axes, corner brackets | Show the floor grid, guides, gizmo and every element's bounds |
| Screen with Full / Half / Quarter | The resolution Stage renders at |

The rate at the right end is how fast Stage is drawing. Over the image's
top-right corner, the shading menu and the view menu name how this view
shades and what it looks through.

The Move, Rotate and Scale tools choose the gizmo drawn on the
selection. It sits at the selection's median point, and everything selected
moves, turns and scales about that point. Move shows axis arrows, Rotate shows
an axis ring per axis plus an outer white ring that turns about your view, and
Scale shows axis handles plus an outer ring that scales uniformly. A plain drag
on an element still moves it across the view. The globe / cube sets whether
arrows and rings follow the world axes or the primary element's own; scale
handles always use the element's own axes.

The Blender keys work while the pointer is over Stage, with no button held:

| Keys | Transform |
| --- | --- |
| G / R / S | Move / rotate / scale with the pointer (also picks the gizmo) |
| X, Y or Z | Constrain to that axis; again for the other orientation; a third time frees it |
| Shift+X, Y or Z | Constrain to the plane without that axis (scale the other two) |
| Digits, `.`, `-` | Type an exact distance in metres, angle in degrees, or factor |
| Shift (held) | Move the pointer at a tenth of the speed |
| Ctrl (held) | Invert snapping for this transform or drag |
| Enter or left click | Confirm |
| Esc or right click | Cancel and put the selection back |
| Alt+G / Alt+R / Alt+S | Clear position, rotation or scale |
| Shift+Tab | Toggle snapping |

While a keyboard transform runs, a bar at the top of Stage names the tool,
constraint and value, and the keyboard belongs to it: timeline, cue and fly
keys stand down until it ends, and the whole transform is one undo step.
Snapping rounds the change rather than the absolute value. The two menus
beside the magnet choose the steps: a move step from 0.01 m to 5 m, and an angle from 1 to 90
degrees (scale steps by 0.1). With Shift held the steps are a tenth of the
distance and scale step and a fifth of the angle. A component that is keyed takes a key at the current frame, exactly as
an Inspector edit would. A Blender-linked object takes a rotate or scale as
a placement override on its occurrence, the same way it takes a move. Lights
are only placed from the viewport, and a camera turns but has no scale.

**Snapping to other elements.** With surface snapping on, an element dragged by its
body rides over whatever is under the pointer and rests on it, as a Library
asset does when dropped: its bounds touch the surface, and snapping steps the
landing point across a level or upright surface. Hold **V** while moving -
dragging the body or a gizmo arrow - to put the dragged element's vertex
nearest where you grabbed it onto the vertex (or edge midpoint) under the
pointer, which lines up LED panels and set pieces corner to corner. A square
marks the vertex it will meet. Neither ever lands on what is being dragged.

With a selection, the bottom left of Stage shows its size along the world
axes in metres (`W`, `H`, `D`). A screen is flat, so it has no depth.
The Bounds toggle outlines every element's bounding box, selected or not.
When Stage is too narrow for the four display toggles, they fold into one
menu behind an eye, with a tick by each one that is on.

**Selecting with a box.** Ctrl+Alt+drag on Stage draws a selection box; every
unlocked element it touches becomes the selection, or joins it with Shift
held. An empty box clears the selection.

**Duplicating as you move.** Alt+drag a gizmo arrow, ring or handle to leave
a copy of the selection where it stood and carry the originals away, as in
Unreal. The copies keep their effects, masks and animation. Groups and
Blender-linked objects are not copied. The copy and the move undo
separately.

**Moving the pivot.** Middle-drag from the centre of the gizmo to move the
point the selection turns and scales about. The pivot snaps like the Measure
tool: to a vertex, an edge midpoint or a surface. A middle click on the
gizmo's centre puts it back, and it is forgotten when the selection changes.
Alt+middle still pans; it is Unreal's pivot chord, but here it was already
the camera's.

### Measuring on Stage

The ruler in the Stage toolbar, or **M** over Stage, picks up the Measure tool.
Click two points and Stage draws the line between them with its length in
metres, and under it the X, Y and Z parts and its slope. As the pointer nears a
vertex it snaps to it, shown as a filled square; failing that it snaps to the
middle of an edge (a triangle), and otherwise takes the surface itself (a hollow
square) or, with nothing under it, the stage floor. A label beside the pointer
names which. Shift+click adds points to a path and shows its total. A plain
click after a finished measurement starts a new one. **Esc** clears the
measurement and a second **Esc**, **M**, or another tool leaves. Measuring never
selects or moves anything, and nothing measured is saved with the show.

An imported model is measured on its own triangles once a moment has passed
after it first appears; until then points land on its bounding box.

Scene, Stage, and Timeline share one typed multi-selection for scene elements.
Ctrl toggles membership and Shift selects a visible range; the Inspector shows
`MIXED` for disagreeing compatible values and applies an accepted edit only to
unlocked compatible members. Select top-level elements, right-click one of
them in the Scene panel and choose **Group** (or **Group Selection**) to create
a replicated transform group without moving them in world space. A group's
right-click menu offers **Ungroup**, which preserves its direct children and
selects them. Rejected hierarchy or transform operations create no history
entry. The Scene footer only names the selection.

Groups can also start empty, as folders to file things into later.
Right-click the Scene list's empty space for **New Group** or **New Vehicle
Process Group** at the top level, or a group's row for **New Group Inside** and
**New Vehicle Process Group Inside**. The new group lands at the front of its
siblings, is selected, and opens its name for typing. Drag rows onto it to file
them. A vehicle process group is shown with a vehicle icon and is otherwise an
ordinary group for now; it is the container the vehicle processing workflow
will build on.

**Highlight Color...** in a row's right-click menu marks the selected rows with
one of the eight marker colours - a tint across the row and a bar at its right
edge that stays visible while it is selected - or **None** to clear it. A
highlight is saved with the project, replicated and undoable, and never
changes the picture. Blender-linked rows do not take one.

Hardware Map and Feed Map are setup panels. Feed mappings support flat,
horizontal-arc, and imported-mesh surfaces. For an imported projection mesh,
split the calibrated LED geometry externally, retain a shared model origin, and
import each feed mesh with `+ IMPORT UV MESH`. Its persistent projection asset
is referenced by absolute path and reported for readiness exactly like media
and models. UV0 represents the full connector image; it is not material UV
space.

A surface reaches an output by being dragged onto it. Drag a row out of
`PROJECTION SURFACES` and drop it on the output that drives that part of the
wall - its row in the list, or its rectangle in the mapping area, whichever the
operator is already looking at - and the drop switches that output to the
surface and takes its whole raster as the region. Every output that can accept
the drop outlines itself while one is in flight. The details rail names the
surface an output is on rather than offering a stepper to hunt through them:
cycling blindly past eight walls to find the ninth is what a control looks like
when the gesture was never decided.

Viewport and surface rows can be named, reordered and copied in the list.
Double-click a row, or click it and press F2, to type a name; Enter keeps it
and Escape leaves the old one. An unnamed viewport is listed by its window and
position, such as `WINDOW 01 | VIEWPORT 03`. Drag a viewport row up or down
among the rows of its own window to reorder it - that order is also the order
viewports draw in, so a later viewport covers an earlier one where they
overlap. Drag a surface row onto another surface row to reorder the surfaces;
dropping it on a viewport instead still assigns it. The copy icon on a row
duplicates it directly below with every setting the same, including source
crop, destination and surface calibration; a named item's copy is named
"<name> copy". Each row's second line shows its domain and destination
rectangle, and hovering a row or its rectangle shows the whole source and
destination. A locked surface is unlocked with its padlock before it can be
renamed or dragged.

Feed Mapping treats application windows and viewports as separate spaces. A
window's raster comes from its Hardware Map output binding and stays fixed;
the colored rectangle inside it is the feed's destination viewport. Moving or
resizing that rectangle changes only `viewport_x`, `viewport_y`,
`viewport_width`, and `viewport_height`, leaving the scene/surface source crop
unchanged. Pixels in the window outside the viewport are black. The panel lays
configured windows side by side. A four-node setup can therefore show four
3840x2160 windows across a 15360x2160 mapping area, each with a 3456x1356
viewport. An NVIDIA Mosaic setup can instead author one 15360x1546 window with
four independent 3456x1536 viewports at x=0, 3840, 7680, and 11520, y=0. Any number
of viewport rows may target the same logical window index; they are composited
into that window in stable document order. Every viewport explicitly authors
all four viewport properties.

Select a viewport and use the **OUTPUT WINDOW / DESTINATION PIXELS** fields
below the mapping canvas to enter its X, Y, W and H exactly. The separate
**CANVAS / SOURCE PIXELS** or **LED SURFACE / SOURCE PIXELS** fields control
the content crop without moving the viewport. Each heading states the extent
its fields are held within: the output window's raster, the scene canvas, or
the assigned LED surface's **RASTER** (set under Surface Calibration, and
independent of the canvas). A physical crop that stops short means that
surface's raster is smaller than the region wanted. Imported UV meshes use their full
surface raster, so source crop fields are hidden for those mappings. Click a
number to type or drag it to scrub. **+ ADD VIEWPORT** duplicates the selected
source and viewport size in the same window; set each copy's destination X to
the required spacing.
For the Mosaic example the last viewport ends at x=14976, leaving a 384-pixel
right margin and 10 black rows below the viewports. The source crops are
independent: use consecutive 3456-pixel crops for continuous canvas content,
or assign individual calibrated surfaces.

Canvas crops are measured in the scene's canvas, so a wall wider than one
connector needs a canvas as large as the wall's whole raster. Set it in
Hardware Map under **PROJECT SETUP** with **Canvas W** and **Canvas H** (up to
16384 pixels per axis). The value applies to every scene, so Program
transitions always join scenes of one size; the edit replicates to every node
and one undo restores the previous size.
For four 2880x1536 wall sections, an 11520x1536 canvas with canvas crops at
x=0, 2880, 5760 and 8640 shows one continuous image across the wall. Shrinking
the canvas below an existing crop leaves that feed refused until the crop is
corrected.

Window setup, launch arguments and synchronization timing accept dimensions up
to 16384 pixels per axis. GPU allocation and physical display-mode support are
still required; dimension acceptance does not establish Mosaic or genlock
qualification on a particular machine.

Start, save and open complete projects through File > New Project / Save
Project / Open Project. File > Recent Projects opens a submenu of the five most
recently opened or saved projects, newest first. Hover over Recent Projects,
then choose a path to open it with the same unsaved-changes prompt as Open
Project. New Project replaces the open project
(asking first if it has unsaved changes) with an untitled one holding a single
empty 1920x1080
scene and a full-canvas Feed Map for node 0, the same as a first launch without
the demo content; Save Project then asks where to put it.
`.mstage` saves are checksummed and atomically replaced. Recovered WAL state
takes precedence at startup; otherwise the last successfully opened/saved
project is offered through recent-project recovery. A damaged or inaccessible
project safely falls back to untitled startup.
