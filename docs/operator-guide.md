# Operator guide

Run commands below from the repository root. See the [README](../README.md) for building.

## Run the editor

From the repository root, after a GUI build:

```powershell
.\scripts\run-editor.ps1
```

The script starts a Director editor. On each output machine,
`.\scripts\run-output-node.ps1 -DirectorHost <editor address>` starts a
lifecycle-supervised render node. Dragging or editing content in the editor
updates the replicated scene shown by the node. Output Monitor controls the
node with the render node password set in the editor's Preferences; give each
node the same one with `-Password` (none by default).

Other Windows demonstrations are:

```powershell
.\scripts\run-scene-demo.ps1  # Director preview plus two scene slices
.\scripts\run-wall-demo.ps1   # Director preview plus two feed-mapped outputs
```

## Find your way around the editor

The initial workspace puts Scene and Library on the left, Stage in the center,
Properties on the right, and Timeline below Stage. Compositor shares the bottom
area as a tab alongside Output Monitor; Program Output and the Asset Viewer
share Stage's dock. Select an object on Stage or in Scene to edit it in
Properties. Import content in Library, then drag it onto Stage or a Timeline
track; double-click it to look at it in the Asset Viewer first. Additional
panels are available in the Window menu.
Saved workspaces retain their dock arrangement. A workspace saved by an older
build is not read; the editor opens this starting layout instead. Panels added
since a workspace was saved start closed - open them from the Window menu.
**View > Reset Workspace** restores this starting layout when you want to adopt
it or undo dock rearrangements.
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

The Environment page's sky, atmosphere, baked lighting and reflections draw
only on the Advanced renderer, so a project set to Basic shows a note and
**Use Advanced renderer** there instead. Their values are kept and return with
Advanced. A physical sky left on stops Basic drawing the frame, so the page also
offers **Turn off physical sky** while one is on.

Object and effect icons use a neutral palette; shape and labels identify their
types. Colour is kept for state, one meaning per hue: green is on air,
healthy and playing; cyan marks Preview, selection, focus and the active tool;
magenta is the cue stack and its GO; amber asks for a look (degraded,
connecting, changed on disk, hidden or locked); red is faulted, missing or
destructive. Everything else is grey, so the picture on the wall stays the
most colourful thing on screen. User-assigned marker colors remain available.

**The show bar** along the top of the window keeps the show's state in one
place, never behind a tab. From the left: the project's name and whether it is
saved, the menus, then the **Program** and **Preview** cards with **Take**
between them, the next cue with its **GO** key, the render-server lights and
the night dim. Program glows green because it is on the wall and shows the
picture it is sending; the card for the scene the editor is working on is
ringed. Ctrl+Enter is Take from the keyboard, whenever no field or menu has
it. The render-server lights are one per
server - green live, amber starting or catching up, red faulted - with the
cluster's health beside them; click them to bring the Output Monitor forward.
GO does what the Cue Stack's GO does. The moon turns on the **night dim**,
which lowers the brightness of every panel and glow for a long night without
touching the picture; the editor remembers it, with the workspace, on this
machine. **Stop all** in Output Monitor asks twice: the first press arms it
and turns it red, and a second press within a few seconds stops every output.

Small motions show where things went: a reordered or newly twirled layer
glides to its new place in the Timeline, a compositor node lifts while it is
dragged and settles where it is let go, and controls ease into their hover
colours. They take a fraction of a second and never delay a click.

**Colours** are picked in a colour well: click any colour swatch - a light's
colour, a material's base colour, a fill, a chroma key, a Background node's
colours - to open the colour picker under it. Drag in the large field for
saturation and brightness and along the rail for hue, or type a hex code or
R, G and B from 0 to 255. The left half of the comparison swatch is the colour
as it was; click it to go back. Recently picked colours and the LED primaries
sit along the bottom. Escape restores the colour the picker opened with; Enter
or a click outside keeps the new one. Everything done in one visit to the
picker is one undo step.

**Reset to default.** A value that differs from where it starts carries a
small amber arrow at the end of its label, as in Unreal's details panel:
Properties, the Compositor's Parameters, Scene Settings and the Color panel
all show it. Hover it to see the default; click it to put the value back.
Every component of a row that differs is put back at once, as one undo step.
Defaults are those of a newly made element, node or effect of the same kind -
an object's position is the origin, a new Blur starts at 8 pixels - and an
animated value is reset at the playhead the way typing it would be.

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
double-click it. The first time, a small window discovers the editor on the
local network and asks for this machine's settings:

- leave the control machine address empty for automatic discovery; its
  current address and port appear in the window when found;
- leave Node index and the advanced Show ID empty too: the editor assigns a
  free node index and supplies its show ID;
- the render node password from **File > Preferences** on the control machine,
  or nothing if none is set there.

Setup generates and saves a unique machine ID once. The editor saves its
assignment of that ID to an output node index before starting the node. Several
new nodes can enroll together without choosing the same index, and the same
machine keeps its index after node or editor restarts. These records live
beside the editor's WAL as `<wal>.nodes-<show-id>.bin`; retain them along with
the render machine's Preferences when preserving a setup. A missing table
starts a new assignment history; an unreadable table refuses enrollment.

The node discovers the editor again on every render-process start, including
Start and Restart from Output Monitor. It waits if no matching editor is running.
If several editors are visible, close the extra editors or select one with a
manual address and port or a Show ID filter. A manual node index requests that
specific index; an index already reserved for another identity is refused.
Discovery uses UDP 47839 on the editor and render machines; the show connection
also needs the editor's TCP port, normally 45060. Discovery is limited to the
local IPv4 network. Use a manual address across routed networks or when UDP
broadcast is blocked. To replace a previously saved address with discovery,
clear it under **Change settings** and save. Clear Node index and Show ID as
well to enable their automatic values. A discovered address is displayed
without being stored as a fixed IP. Fully manual address, show and index
settings remain available for networks where discovery is disabled.

The render-process status describes the supervised process. An editor appearing
in discovery confirms that it advertised its listener; Output Monitor's session
and readiness state confirms whether the node has joined and can render.

**SAVE AND START** stores them in this machine's per-user Preferences - never
in a project - and waits for an editor to assign identity, then starts the
lifecycle agent and render process. From then on, double-clicking `media-server.exe` (or a shortcut
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

The **Program**, **Take** and **Preview** cards in the middle of the show bar
are the whole control surface. **Program** puts the editor back on the live
scene; **Preview** returns to the scene being rehearsed; **Take** promotes
Preview to Program. **Scenes**, at the top of the left-hand Scene panel, is
where a scene is chosen: `ON AIR` marks the live one, `PREVIEW` the rehearsed
one, and clicking any other row takes it into Preview. The **+** button
creates an empty scene and the copy button duplicates the scene currently on
the Stage, both straight into Preview. On a
project that has never used the switcher, the first **Preview** press copies
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
Both mask-authoring viewports display `Can't select while masking`, making
the temporary interaction domain explicit instead of leaving an ordinary
selection click to appear broken.

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

**Promoting.** **Take** exchanges the two slots at an exact control frame, so
every node performs the same transition on the same frame. The default is a
30-control-frame dissolve. A dissolve renders both scenes and mixes two
finished pictures, which is only defined when the two scenes share a canvas;
when they do not, the key reads **Cut** and promotes on a single frame
instead, and **Fading** fills the key while a transition runs, during which
scene, graph, and switcher edits are all refused - both inputs of a dissolve are
committed pictures. After a swap, Preview holds the scene that just came off
air and its playhead re-seeds to the live frame.

**Take** is held by exactly one thing beyond the switcher's own
preconditions: a project that asks for physical output synchronization under a
STRICT failure policy, whose timing the cluster cannot prove. The key then
reads **Held**,
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

Adding a video whose frame rate differs from the timeline's, from the Library
or the Asset Viewer onto the Timeline or Stage, opens a small menu at the
pointer: **Change the timeline to** the clip's rate, or **Keep the timeline
at** its current rate. The clip goes in after the answer; when the timeline
changes, the clip waits until the new rate is in force so its length is
measured at that rate. Clicking away keeps the timeline's rate and still adds
the clip. Only rates the timeline offers (23.976, 24, 25, 29.97, 30, 50,
59.94 and 60 fps) ask; stills, image sequences and other rates go straight in.

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

Clicking a picture on the Stage selects the screen it is playing on, since
that is what stands there and what a drag moves: click a plate and drag, and
that one screen moves. Under an LED Car Process the screen's own transform
is an offset inside the rig's placement for its slot, so each angle can be
nudged on its own while the rig still moves them all. The clip itself is
selected from the Timeline. Screens are outlined in the Stage viewport, so
a track with nothing on it, a muted one, and one between clips are all
still findable and still have a gizmo.
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
named destination and `MOVE+DELETE` its placements first. The + at the head
of the list adds a track, as does **New Track** on a track's menu. A track is
listed once, by its header; selecting the header is how the Inspector reaches
the screen.

Beside its padlock a track header carries three icon switches, each lit while
it is on (hover one for its name):

- **Target** (a ring with sights) aims Insert and Overwrite (`,` and `.`, or the menu) at this track,
  whatever is selected. Without a target they go to the selected track, or to
  the selected clip's track (the clip you chose last when the clips are on
  several), and with no track selected - or no tracks at all - outside every
  track, among the clips filed under none. A locked target refuses them. One
  track at a time is the target; the target is the operator's own and is not
  saved in the project.
- **Sync lock** (a chain link) sync-locks the track, and is saved with it. A ripple trim, ripple
  delete or insert made on another track moves this track's clips at or after
  the edit point by the same amount, so what plays on it stays in time with
  the track being cut. A clip that straddles the edit point stays where it is,
  as does a locked clip; and a locked track never moves, sync locked or not.
  Where a moving clip would run into one that stays, a ripple trim stops
  short of it and a ripple delete or insert is refused with nothing changed.
  The Stage preview of a ripple drag shows the synced tracks moving too.
- **Solo** (headphones) solos the track on the Stage; see **Solo** below.

**Track bar.** A track holding placed clips draws a bar in its header's lane
from its first clip's head to its last clip's tail, ticked at every cut and
labelled with its clip count. The bar edits the track as though it were one
clip: drag it to move every clip on the track by the same amount, snapping
either end to cuts on other tracks, the playhead and markers; drag its left or
right end to trim the clip that opens or closes the track; with the
**Razor (`C`)** click it to split the track in two there, as the razor splits
a clip in two. Ripple, Roll, Slip and Slide have no single cut to act on, so the bar
answers them as Selection does. Carry the bar onto another track's band and
the track takes that place in the stack, the same reorder a header drag makes;
moved in time as well, both are one undo step. A track with a locked clip
refuses the drag rather than leave that clip behind.

A placed plateset's tracks are listed together under one **take row**; see
[Vehicle process platesets](#vehicle-process-platesets).

**Splitting a track.** The razor on a track's bar, **Split Track at Playhead**
on its menu, or `Ctrl+K` with the track selected and no clip selected cuts the
track into two tracks. The track keeps what plays before the cut; a new track
directly in front of it - the same screen, format, blend, grade, effects and
keyframes, named after it ("Plates" gives "Plates (2)") - takes what plays from
the cut on. A clip straddling the cut is split the way the clip razor splits
it, its tail going to the new track. The split is one undo step. It needs
something on both sides of the cut, and is refused on a locked track or when
it would move or cut a locked clip. Shift with the razor still cuts every clip
on every track where it lands, without making tracks. A compositor Scene Layer
that names the original track does not pick up the new one.

**Keyframing a track.** An open track lists a **Properties** foldout above its
clips, as a group lists one above its children. It holds the screen's
Position, Rotation, Scale and Opacity and any keyable effect on the track,
with the same key diamonds, key steps and Curves as a layer's channels, and a
closed track's header shows its own keys among its clips'. `[` and `]` step
through a selected track's keys, and **Clear Keyframes** on the track's menu
removes them. With the aspect lock closed a screen's height follows its width,
so key Scale X; a Scale Y key is ignored. `X` with a track selected (and no
clip) sets In/Out around every clip on it, as **Set In/Out to Track** does.

**Solo.** Every row the Stage draws - an object, a model, a clip, a light, a
group, a Blender collection and a track - carries a **Solo** switch just
inside its padlock. While anything is soloed, the Stage draws only what is soloed and
what is inside it: a soloed group or collection brings its children, and a
soloed track its clips. Lights keep lighting the Stage as they are, unless a
light is among what is soloed - soloed itself or inside a soloed group - in
which case only those lights light it. Solo several rows to see them together;
a caption along the Stage's foot says the Stage is soloed. Solo is the
operator's alone: Program, the render nodes and the outputs keep drawing
everything, nothing is authored, and it is not saved with the project.

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

Every orderable layer row has the same full-width lift gesture, and so does a
clip's bar: drag the bar up or down within its own track - or among the loose
layers, for a clip on no track - and it lands above or below the clip it is
dropped on, alongside whatever the drag did to its time, in one undo step.
Dragging a clip within its current track reorders that track's clips; dragging an object, model,
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
way a collapsed layer or compound clip does elsewhere. A track always draws
its track bar (see **Track bar** above), open or closed. A closed group,
collection or hierarchy parent draws a thin bar over the frames its clips and
keys span, from the earliest clip start or key to the latest clip end or key,
and a small tick on every frame anything below it is keyed. A layer whose
properties are folded - or a parent's folded Properties - summarises its own
keys the same way; a clip's ticks ride on its bar. A tick takes the channel's
colour when every key on that frame belongs to one channel, is neutral where
channels meet, and is larger where several keys share the frame. Opening a
parent moves its summary down to whatever below it is still closed. A layer
with nothing placed or keyed draws nothing in its lane. Summaries other than
the track bar are for reading only: they are not dragged, and pressing one
acts on the lane beneath.

The edge between the track list and the lanes is a seam, marked by a grip at
its top: drag it to widen the list, or double-click it to widen the list until
every name on screen reads whole. A name too long for its row is shortened in
the middle, so names that share a long beginning still read apart, and shows
whole on hover.

Track order is changed by dragging a header - or its bar - up or down the
stack. The target is
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
one click and preserve Loop. The playhead can be parked outside In and Out -
scrub, click or type past either end - so I or O can extend the range there;
if it is outside, the other endpoint moves only as far as needed to keep the
new range valid. Playing from outside the range starts at In. Frames outside In and Out are dimmed from the ruler down through every
track, so the part of the show that plays reads at a glance.

The ruler labels whole seconds and minutes, and fills the space between with
minor ticks on whole frames, the half-way tick taller. While the view stays
inside the show's first hour the labels leave the hour off (`04:20:13` rather
than `00:04:20:13`). Right-click the ruler and choose **Show Frames** to label
it in frame counts instead. The playhead carries a flag on the ruler with the
time it stands on; the flag stays whole at either edge of the lanes.

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

On a media layer row the Timeline draws that placement as a bar.

Each placed clip row carries its decode resolution inside its padlock as an
icon: a filled square for **Full**, or a quarter-filled square lit in the
warning colour for **Half**, with `HALF` also written on the clip's bar. Click it to switch; it is the Inspector's Resolution setting,
authored the same way and undone the same way. Half holds each decoded frame
at half width and height: a quarter of the video memory, softer wherever the
clip is shown larger than half its source size. A locked clip shows its state
and refuses the switch.

Clip rows can be made taller so their pictures and sound can be read.
**CLIP ROWS** at the foot of the layer list sets every clip row to **S**
(one list row), **M** or **L**. Drag a clip row's lower edge in the layer
list to size that row on its own, or hold Shift while dragging to size every
clip row together; choosing S, M or L again puts rows sized on their own back
with the rest. On a tall row the filmstrip shows whole frames side by side at
the source's shape, each the source frame nearest its left edge, and the
waveform gets its own band under the pictures. The clip's name keeps to a
band along the top. Row heights are a view setting, like the list width:
they are not saved in the project, and rows sized on their own are forgotten
when another project is opened.

Editing the bar:

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
  A snap draws a line down the lanes with an arrowhead at each end, pinching
  the frame it caught on. Move, trim, and fade bars preview during the lift
  and commit once on release.
- **A chip beside the pointer** reads out every clip, key and marker drag in
  the ruler's units: how far it has gone and where it lands
  (`+00:00:12  >  00:01:04:07`), the new length for a trim, and what a snap
  caught (`SNAP playhead`, `SNAP In`, a marker's name, or `clip edge`). It
  keeps inside the panel, flipping above the pointer at the foot.
- **The Stage follows the edit point** while a trim, ripple trim, roll, slip
  or slide is dragged. It shows the frame the edit now falls on, with the edit
  applied exactly as release will author it, later clips moved by a ripple
  included: an in point's first frame, an out point's last frame, the last
  frame before a rolled cut, and a slipped or slid clip's new first frame. A
  caption along the foot of the Stage names the frame. Letting go returns the
  Stage to the playhead. Only the Stage picture changes: Program, the render
  nodes, the Timeline and the Inspector stay on the playhead, and nothing is
  authored until release. Moves and fades leave the Stage where it is.
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
property writes a key at the current playhead. `Shift+K` toggles it while
Timeline has keyboard focus; bare `K` stops the playhead. With Auto Key off, edits change the underlying base value;
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
requires one valid contiguous selected span. Ripple Trim, Ripple Delete and
Insert also move sync-locked tracks (see **SYNC** above). Insert Clipboard and
Overwrite Clipboard (`,` and `.`) land where the selection is (see **Target**
above), outside every track when no track is chosen, at the playhead, and
commit at once, as an editor's do. Outside every track, Insert moves only the
later clips filed under no track, and Overwrite stacks the paste in front of
them.
Overwrite lays the clipboard over the track, in front of every clip already
on it, keeping the pasted clips' own stacking: the clips it covers stay whole
underneath, nothing moves, and the track keeps its length. While the Asset
Viewer has the keyboard, the same keys
cut its marked range instead (Insert Source and Overwrite Source). Handle
limits, adjacency, locks, rational source rates, and stale selections are
validated by the Director, and every accepted operation is one undo step.
**F** (match frame) opens the selected clip under the playhead in the Asset
Viewer at the source frame it is showing, with In and Out around the frames
the clip plays; a clip's right-click menu offers the same as **Open in Asset
Viewer**.
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

### Timeline keys

Press `?` while Timeline has keyboard focus, or the **?** button at the foot
of the layer list, for a sheet of every key the Timeline answers; any press or
`Esc` closes it. The editing keys follow the conventions of other editors:

- **`J`, `K`, `L`** play backwards, stop and play forwards from wherever the
  playhead stands, at the show rate; pressing `J` while playing forwards turns
  the show round in place, and `L` turns it back. `Space` and the play button
  always play forwards. There is no faster shuttle: one control frame always
  advances one show frame. In Preview the keys drive the Preview playhead;
  otherwise they drive the live show, as `Space` does.
- **`Alt+Q`** and **`Alt+W`** ripple trim each selected clip the playhead is
  inside to the playhead: `Alt+Q` removes the frames before it, keeping the
  clip's start so what was under the playhead now starts there, and `Alt+W`
  removes the frames from the playhead on. Later clips on the track close up.
  Bare `Q` stays the hold cue key. With no selected clip under the playhead
  the panel says so and nothing changes.
- **`Alt+Left`** and **`Alt+Right`** nudge the selection a frame, or a second
  with Shift. Selected keys move first; otherwise the selected clips are
  nudged with the active tool: Selection moves them, Ripple Trim moves their
  out point and what follows, Roll moves the cut after each clip with the
  clip that starts on it, Slip changes the source frames they show, and Slide
  moves them between their neighbours. Ripple, Roll and Slide act on the
  earliest selected clip of each track. Without Alt the arrows step the
  playhead as before.

`Ctrl+K` cuts the selected clips at the playhead (a selected track, with no
clip selected, splits into two tracks), `Shift+Delete` ripple deletes them,
`I` and `O` set the range, `X` sets it around the selected clips or track,
`Up` and `Down` jump between cuts and markers, and `=`, `-` and `\` zoom.

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
time and on the Curves graph in value.

The navigator along the Timeline's foot is the whole show in miniature: the
In/Out range, every placed clip, the markers and the playhead, with the part
on screen as a thumb. Drag the thumb to scroll, drag either end of it to zoom
while the other end stays put, or click the bar beside the thumb to jump
there. It covers the show range and anything authored outside it, and never
seeks the playhead. Track
mute and lock are the layer list's eye and padlock rather than a letter, so the
state of a stack reads from across a stage. The stage's LED surfaces follow the
scene's elements in both the Timeline and the Scene outliner, each with the same
padlock - the lock Feed Mapping and Stage picking already honour. A surface has
no eye, and clicking its Scene row selects it on Stage.
A selected, unlocked surface shows its **Position**, **Yaw** and **Pitch** (in
turns) and **Size** in **Properties**; a UV-mesh surface shows its **Origin**,
Yaw and Pitch and takes its size from the mesh. Pitch is not keyable; an
animated Yaw keeps the authored pitch. Shape, pixels and wall colour stay in Feed
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

### Camera tracking feeds

A tracked camera takes its pose from a **feed**: a tracker protocol the
Director listens to on a UDP port. Feeds are project state. Select a camera,
press **+ New feed** in its TRACKING section, pick the feed from the **Feed**
list, and its settings appear below: **State** (Listening or not), **Protocol**,
**Port**, the camera or rigid-body id, the sensor and fallback lens the plate is
sized from, and the **Up axis**, **Heading**, **Origin** and **Scale** that place
the tracker's numbers on the stage. Only the Director opens the socket; every
render node receives the pose the Director sampled, stamped for an exact
control frame, so two nodes never disagree about where the camera is.

Three protocols are supported:

- **FreeD** - the 29-byte D1 datagram most lens and crane encoders send. The
  **Camera ID** matches the id byte in every datagram, so two cameras can share
  a port.
- **OpenTrackIO** - the JSON sample from the SMPTE RIS. The camera id matches
  the sample's `sourceNumber`; a feed authored for camera 0 takes samples that
  name no camera.
- **NatNet** - OptiTrack Motive's streaming protocol. The camera is a rigid
  body in the volume. **Rigid body** is its streaming id from Motive's Assets
  pane, or 0 for the first body in every frame. **Port** is Motive's data
  port (1511), **Multicast** is the group Motive streams to (239.255.42.99),
  and **Command port** (1510) is where the Director asks Motive which NatNet
  version it speaks. Motive's streaming pane must be set to **Multicast**;
  unicast is not supported. Frames that arrive before Motive has answered are
  counted under **No handshake** in Live Data and nothing is tracked until it
  does, because the frame layout depends on the version and a guessed one
  tracks the wrong bytes. A body Motive lists but cannot solve this frame is
  counted under **Not tracked** and the last good pose holds. Motive tracks the
  body, not the lens, so the plate is sized from the feed's fallback focal
  length or the camera's lens profile. Set **Up axis** to **Z up** if Motive is
  configured for Z-up; the rigid body's orientation is remapped along with its
  position. Orient the rigid body in Motive so that at zero rotation the
  camera looks along stage forward; a residual heading offset is corrected
  with **Heading** like any other tracker.

**Window > Live Data** shows each feed's link state, counters and the pose the
wall is drawing. Without a tracker, `scripts/send-freed-tracking.ps1` and
`scripts/send-natnet-tracking.ps1` stand in for a FreeD encoder and for
Motive; each script's help describes the feed settings it expects.

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

A video clip's Status says how its editor proxy is coming along: **Editor
proxy queued**, **Editor proxy 40%** with a cyan bar filling under
the row, **Editor proxy ready**, or **Editor proxy failed** in amber. Until
it is ready, Stage, Program and the Asset Viewer play that clip's source,
which is slower; the part under the playhead is made first, so playback
there gets faster within seconds. Hover the Status for the details. See
[Editor proxies](media-playback.md#editor-proxies).

The footer slider sets the thumbnail size, and so does Ctrl+wheel over the
content. Each view keeps its own size.

Click an asset to select it. Ctrl toggles assets in and out of the selection,
and Shift extends it. Double-click an asset, or press Enter, to open it in
the [Asset Viewer](#the-asset-viewer); dragging it out is how it reaches a
scene. The Generators section lists the built-in primitives, a
Background card, lights and camera, and a single click adds one. Drag a Box,
Plane, Sphere or Background onto Stage instead to place it where it lands, the
way a dropped asset is: an outline previews where it will rest. Lights and
the camera are added where their defaults put them. Right-click an asset, folder or empty
space for **Open in Asset Viewer** (Enter), **Rename** (F2), **New Folder**, **Move to Top
Level**, **Remove from Library** (Delete), **Import Media...**, **Import
Model...**, **Import Particle Effect...** and **Rescan Sources**.

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
  first appears. Until then, the Stage uses its bounding box. A particle
  effect has no size until it runs, so its origin lands on the point under
  the pointer.
- **An empty part of the Timeline:** each asset becomes a new layer.
- **A compatible Timeline row or Compositor Scene Layer:** the asset replaces
  that source, as described below.
- **A Compositor image input:** the asset is wired into it.

**Files from the desktop** can be dragged straight into the window. Dropped on
the Library, they are imported into the folder under the pointer. Dropped
anywhere else, they are imported at the Library's top level. Video and images
are probed. Models go through the model worker. Particle effects (`.ajfx`
packages exported by ajfx Studio) need no probe. Files that are none of these
are named in the status line rather than imported. Import also records the video
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

**Particle effects** are `.ajfx` packages, exported by ajfx Studio. A
package holds an effect, its precompiled GPU kernels, and any textures its
sprites use. Smoke and other see-through effects draw back to front from
every camera, so they layer correctly as you orbit, and the parts of one
effect (sparks inside smoke, say) layer by depth with each other too. A package that is
missing a texture its effect names does not play; the Stage status names
the texture. Drag one onto Stage to place it, then
select it to set its **Seed** and **Start frame** in the Inspector's
PARTICLES section. **Start at the playhead** sets the start frame to the
current show frame. Two placements of one package never draw the same
particles, even with equal seeds. Below them are the **parameters the
package publishes** (a spawn rate, a wind, a tint): one row each, with a
colour well for a colour, a reset arrow back to the package's default, and a
key diamond. Keys are kept under the parameter's name, so they stay on the
right parameter after the package is exported again. Once a parameter is
keyed, typing or dragging a value sets a key at the playhead. Each keyed
parameter also gets a row under the effect on the Timeline (`wind X` for one
component of a vector), where its keys move, delete and ease like any
other. Copy and paste of keys skips parameter keys for now. The effect
reads keyed values at every simulation step, between show frames as well
as on them. A parameter shapes everything the effect has done so far, so
changing a value or a key runs the effect again from its start. It holds
until it has caught up with the playhead. Effects with solid pieces that
knock into each other and pile up (debris, a heap of rocks that collapses)
work their physics out on a thread of their own, a moment ahead of the
playhead. After a jump the effect holds until that thread has caught up, as
any effect does. A package exported with its cache beside it (the
`.ajfxcache` file of the same name, which ajfx Studio writes for a performed
scene) plays the performance exactly as it was recorded: the effect's own
seed and actions travel in the cache, so every placement and every node
plays the same bodies, and a jump is read from the cache rather than worked
out again. If the cache no longer matches the package, the effect does not
play and Program says why; export both again, or delete the cache to
simulate the effect instead. Effects play only in the Advanced renderer.
In Basic, the Stage and Program status say so, and Program is not ready
while an effect is due to play. After you export a package again in place,
**Rescan Sources** makes every renderer read it again. Each keeps drawing the
old version until the new one is loaded, then switches to it.

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

### The Asset Viewer

The Asset Viewer shows one Library asset at a time on its own: media as a
source monitor, and a model or projection mesh in a neutral look-dev scene
(see [Models in the Asset Viewer](#models-in-the-asset-viewer) below).
Double-click an asset in the Library (or select it and press Enter), drop one
onto the viewer, or choose **Open in Asset Viewer** from a Library or Timeline
menu. It is a tab beside Stage and Program Output, and **Window > Asset
Viewer** shows it. The arrows at the top right step back and forward through
the assets opened.

Nothing the viewer holds is part of the project: its frame, marks, zoom and
channel view are this editor's own, are never sent to a render node, and reset
when another project opens. The only thing that leaves it as an edit is a range
cut into a track, which is one ordinary undo step.

**Playing.** With the viewer focused (click in it), Space plays and pauses,
J, K and L play backwards, stop and play forwards at the source's own rate,
Left and Right step a frame (a second with Shift), and Home and End go to In
and Out, or to the first and last frame. The scrub bar under the picture
carries the asset's filmstrip; drag along it to scrub. **Loop** repeats
playback and **In > Out** plays the marked range. The readout gives the
source's own timecode - its start timecode plus the frame, drop-frame when the
file's is - and the frame number. No sound plays: the program has no audio
path.

**What the header says.** **SOURCE** is the asset's own file, required by the
project and found on this machine. **SOURCE - NOT CHECKED** is the asset's own
file when nothing in the project uses it: presence is only asked of required
content, so a missing file shows the grey placeholder rather than a warning.
**PROXY** is a video asset's 720p editor proxy, or the proxy its Prefer or
Require Proxy policy asks for. **PROXY REQUIRED - NOT READY** draws nothing,
as the wall would, and **OFFLINE** is a missing file. The **Proxy / Full res**
menu in the header chooses which file the viewer decodes; Full res shows the
source's own pixels for checking detail, whatever Stage and Program use.
While the open clip's proxy is still being made, the Proxy menu carries a
bar along its foot that fills as the proxy is made; its tooltip gives the
percentage.
A status that does not fit beside a long name is left out. **DECODING** means frames are still arriving; the last complete one is
held. The viewer decodes through its own cache, so scrubbing it never counts
against Program's asset readiness.

**Marking and cutting.** I and O (or the **In** and **Out** buttons) mark the
range; **Clear** removes both. `,` inserts the range at the Stage playhead on
the target track and `.` overwrites; the **Insert**, **Overwrite** and
**Append** buttons do the same, Append landing after the track's last clip.
Overwrite lays the range over the track in front of every clip already on
it, as Overwrite Clipboard does: those clips stay whole underneath, nothing
moves, and the edit keeps its length.
The range lands where Insert and Overwrite Clipboard do - the targeted track,
then the selected track, then the selected clip's track, and otherwise outside
every track - and the edit row names it ("outside every track" when no track
is chosen). An Insert that lands inside a clip cuts that clip there, as
the Razor does, and its second half moves along after the new clip; clipboard
Insert still refuses a point inside a clip. With no marks the whole source is
used. The range plays
for the fewest show frames that reach its last frame at the show rate, so
every marked frame plays; a still keeps its five-second default. The cut
commits at once, as one undo step, and the new clip is selected. The Stage
playhead does not move: on Program it is the live show's clock. Aimed at a
locked track, during a transition, or before the file has been probed, the
viewer says why and nothing changes.

**Dragging out.** Drag the picture out of the viewer to place the marked range.
Let go on a track's band in the Timeline and it is laid over that track from
the frame under the pointer, as Overwrite does; anywhere else on the Timeline
it becomes a new layer
starting there; on the Stage it becomes a new placement starting at the
playhead, resting where a Library drop would.

**Looking closely.** The wheel zooms about the pointer, a middle drag pans,
**Fit** (or F) fits the picture and **1:1** shows one source pixel to one
screen pixel. The channel menu shows red, green, blue or luma alone as grey,
and the EV buttons add a display-only exposure offset (click the value to
reset it). Under the picture, the readout gives the source pixel under the
pointer and its display values after the project's colour transform. **Used**
lists this asset's placements in the scene on the Stage; choosing one selects
it.

The viewer renders through its own renderer on the editor's GPU, made when it
first shows an asset and released a few seconds after it is hidden.

#### Models in the Asset Viewer

A model or projection mesh opens at the origin of a look-dev scene: a grey
floor meeting its base (where a drop on the Stage would rest it), ruled every
metre, with the lines through its pivot red along X and blue along Z; a
1.8 m figure beside it for scale; and a studio rig of three lights - key,
fill and rim - with the key casting shadows in the Advanced renderer. It
opens in a three-quarter view from the front right. Nothing about the scene
or the view is authored: like everything in the viewer, it is this editor's
own and resets with the project. A model is placed by dragging it from the
Library, as before.

**Looking around.** A left drag orbits about the pivot, a middle drag pans,
and a right drag or the wheel moves in and out. **Frame** (F) fits the whole
model and **Home** returns to the opening view. **Turntable** (T) turns the
model about its pivot, one turn in 10, 20 or 40 seconds; a Shift+drag turns
the lights instead. **Floor** and **Figure** hide them.

**Lighting.** The environment menu chooses the **Studio** rig; the **Stage
scene**'s own sky, image, physical sky and directional lights, at its
camera's exposure; or an **HDRI image...**, an equirectangular HDR or EXR
file chosen from disk, turned by Shift+drag. An image lights a scene only in
the Advanced renderer, as on the wall, and the header says so when the
project renders Basic. The viewer draws with the project's renderer, quality,
colour and texture budget, so materials resolve as they do on the wall.

**Shading.** The shading menu has the Stage's Lit, Unlit, Lights only and
Wireframe, and two more for checking an asset: **UV checker** shows its
texture mapping and **World normals** its facing. A projection mesh opens in
UV checker, since its UVs are the mapping the wall is calibrated with.

**Under the picture** the readout gives the width (x), depth (z) and height
(y) in metres, and where the pivot sits from the middle of the base. Three
tabs below:

- **Details**: the counts - vertices, triangles, parts, materials, textures
  and the memory they need at full resolution, levels of detail and
  animation clips - and the checks, warnings first: textures over the
  project's texture budget (Project Settings > Geometry), a normal map on
  vertices without a tangent frame, a textured part without UVs, textures
  larger than 4096 px, a size suggesting the wrong units (over 100 m or under
  1 cm across), a pivot far outside the geometry, an animated model whose
  placements on the Stage are still set to Bake Geometry: Static, and
  vertices without UV0 on a projection mesh. They are worked out away from
  the editor's frame, so a large model shows "Checking the package" for a
  moment.
- **Parts**: one row per material run at full detail, with its colour,
  roughness and metal and each texture's size and format. Click a row to
  show that part alone, and click it again (or **Show all**) to show every
  part.
- **Animation**: the package's clips by name and length. Click one to play
  it on the viewer's own clock; Space plays and pauses, Left and Right step a
  frame (a second with Shift), **Loop** repeats it, and the bar scrubs it.
  **Bind pose** stops.

While a model's geometry or textures are arriving the header says
**LOADING**, and Details says what it is waiting for, as the Stage does. A
model the importer refused says **DID NOT LOAD**; one the renderer could not
put on the GPU says **LOAD FAILED** with its reason.

**Library thumbnails.** A model's Library thumbnail is the same three-quarter
view under the studio rig, on a plain floor without the grid or the figure,
drawn on the editor's GPU when the Library first
shows it and kept in this machine's derivative cache like a media thumbnail
(the `model-turntable-320x180-srgb` profile). It is drawn again when the file
changes.

### Vehicle process platesets

A plateset is the camera angles of one driving take, each filed in the LED
Car Process slot it plays in: Front, the four three-quarter and side
positions, Back, and the Ceiling Reflection. It is a Library asset like any
other - it lists, renames, files into folders and drags - but it names
media assets rather than a file, so it is never copied to a render node on
its own.

**Making one.** Select a take's plates in the Library, right-click and choose
**New Plateset from Selection**. Each plate goes in the slot its file name
gives (`FC`, `FRONT`, `FL`, `REAR_RIGHT`, `SL`, `TOP`, `UP` and similar; the
last such tag in the name counts), the set is named after what the names
share, and it opens in the Asset Viewer. **New Plateset** on the Library's
background menu makes an empty one. A plate the names place nowhere goes in
the first empty slot.

**The plateset page.** The Asset Viewer shows the rig from above, front up,
with a tile per slot. Drag a plate from the Library onto a tile to play it
there; drag several onto the rig to fill the empty slots their names
propose. Each tile's mark lights the plate it is for - the ceiling plate
is the one over the car - until a plate is in it; then it shows that
plate's thumbnail. The x on a tile empties it. **3 / 5 / 9 camera** chooses which
slots the page offers; a plate in a slot the template does not offer stays.
Each tile's **SYNC** field is that plate's offset: the source frames it
skips so every plate starts on the same instant. A scrub or a typed value is
one undo step.

**Readiness** lists what stands between the set and the stage, slot by
slot: no front plate, a missing or unprobed plate, plates at different
frame rates or mixing drop-frame and non-drop timecode, and no frame every
plate covers after its offset. These refuse placement. **Decode** compares
what the plates would decode per second with what this installation was
measured to keep up with (four 3840x2160 24 fps plates on the reference
Director); over it is a warning, not a refusal, with **Prefer 720p proxies
for every plate** to bring it in.

**Half resolution decode (all plates)** holds every placed plate of the set
at half size: a quarter of the memory per decoded frame. It does not make
decoding cheaper. Ticking it changes every placement in one undo step, and
new placements follow it.

**Sync by Timecode** lines the plates up by the start timecode each file
carries and says which plates stopped it when it cannot. **Sync by Motion**
decodes every plate in the background, once - signatures are cached by
content - and lines them up by the bumps every camera on the car shares.
It reports each plate's offset with a dot for how sure it is (green is
applied, amber and red keep their offsets) and how far it drifts between
the start and the end of the take. **Apply** authors only the sure ones.
Footage that has been stabilised, or that is already in sync and smooth,
can share too little shake to sync from the picture; the page says so
rather than guessing. Light is deliberately not used: a shadow reaches the
front camera before the rear one.

**Find Moments** reads the front plate alone - a ninth of the decoding -
for **Stopped**, **Turning right/left**, **Bump** and **Into shade**, each
at the frame it begins (Sync by Motion finds them too). Click one to keep
it, again to let it go, or **Keep All**. Kept moments belong to the
plateset, in front plate frames: wherever the set is placed they are drawn
in a **Moments** band under the Timeline's marker strip, and they move with
the footage when the take is moved, trimmed or slipped. Each kind always
has its own colour - a stop red, a turn amber, a bump blue, shade purple -
and says what it is beside its diamond: in full where the next moment
leaves room, as a short tag (**Stop**, **Left**, **Right**, **Bump**,
**Shade**) where it leaves less, and only on hover where moments crowd
together; zoom in to read them. Click one to go there. The
diamond-on-a-staff button in the Timeline toolbar shows or hides the band.
They are not cues and never stop the show.

**Placing a plateset.** Drag it from the Library onto the Timeline (it
starts where it lands) or the Stage (at the playhead). Each plate gets a
track named after its slot and the take, its raster set to the plate's,
and a clip lasting the frames every plate covers. The scene's compositor
gets a Scene Layer per track wired to the matching socket of an LED Car
Process, created and fed to Output (composited over what Output already
shows) when the scene has none. When the scene's rig already plays
something on a socket the plates need, a menu asks whether to replace those
plates, add a second LED Car Process, or keep its wires and fill only the
free sockets. The whole placement is one undo step.

**The take row.** On the Timeline a placed plateset reads as one track: a
take row, marked with the plateset's car and named after the set (as the
Library names it, or after what its tracks' names share), with how many
plates it holds - and how many takes, once a cut has made more than one. It
starts closed, listing only the row, whose lane draws a bar per take from
its first frame to its last, with its fade ramps and a hairline per plate.
The bar edits the take as a clip: drag it to move the take, drag an end to
trim it, pull a fade knob, Slip it, or click it with the **Razor (`C`)** to
cut it (`Shift` cuts every clip on every track, as on a clip). Each is the
same edit the front plate's own clip makes, so it is one undo step and every
plate of the take follows; a move never changes tracks or stack order.
Ripple, Roll and Slide would move one plate's neighbours alone, so the bar
answers them as Selection does. A take with a locked plate or track takes no
drag. Clicking the row or a bar selects every plate it stands for - the
row all of its takes, a bar its own - with the front plate shown in the
Inspector, so Delete and Copy act on the whole take.

The row's eye, **Solo** and padlock switch every track of the set at once:
each reads lit while all of its tracks are, and a click brings every track
to the same state. Drag the row by its name to move all of its tracks up or
down the stack together, in their order; a track dropped onto a closed row
goes in front of or behind the whole set, never between its tracks. A set
with a locked track stays where it is. Right-click the row to **List** or
**Fold Plate Tracks**, **Mute** or **Lock Plates**, **Split Take at
Playhead**, **Set In/Out to Plateset**, **Open Plateset in Asset Viewer**,
or **Delete Plateset Tracks and Plates**, which removes every track of the
set with its plate in one undo step.

Twirl the row open, or double-click it or a bar, to list the plates' tracks
under it, one indent in. Each is named by its slot alone - **LEFT**, or its
tag **L** when the list is too narrow - since the row above names the set;
hover for the track's full name, which renaming still starts from. Each is
still its own track and screen: its own grade, mute, lock, solo, target,
keys and clips, edited as before, and the row stays above them with its
bars. Open or closed is the operator's view,
not saved in the project; a search opens it, and pointing at a plate from
another panel opens it. A track joins the row while every clip on it is a
plate of that set; tracks that share a take are one placement, so two
placements of one set are two rows, and the halves of a razored take stay in
one. The row sits where its frontmost track would be, and its tracks keep
their numbers whether it is open or closed. A track moved in between a
set's tracks parts the row in two, one above it and one below, so the list
always shows the stacking order the Stage draws; moving it out joins them
again. The document still holds one track per
plate; the row is only how the Timeline shows them, and changes nothing the
Stage, Program or the render nodes draw.

**Editing a take.** Moving, trimming, fading, slipping or cutting one plate
of a placed take does the same to every plate of it: plates sharing a take,
a start and a length are one unit. A plate moved on its own leaves its take,
and its take row shows it as a bar of its own.
Changing the set's offsets moves every placed plate's source in by the
difference, and shortens the take if a plate would now run out. Drop
another plateset onto a placed plate, or onto its take row (a bar there
picks that take), to **swap the take**: each plate plays
the new set's media for its slot at the same take position, keeping its
trims, track, grade and wiring; tracks named after the old take are
renamed, and plates the new set has no slot for stay as they were.

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

**A node's own log.** An expanded node row has **Show log**. It reads the last
lines the render process wrote to its log on that machine, through the
machine's lifecycle agent, and keeps them fresh every few seconds while open.
It works whatever the node is doing - hung, out of lockstep, reconnecting or
stopped - and after a crash or restart the previous run's last lines appear
above a **Current run** divider. Lines with errors, failures or lockstep
trouble are shown in red. The line above the log says when it was read and
whether the node's disk writer dropped anything. The refresh arrow reads it
again at once. Every process keeps such a log; see "Process logs" in
`cluster-operations.md` for where the files are.

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
where that machine listens. A machine already on the map has a green outline
and selects its card. Otherwise a card says, in amber, what would stop it
working before it is placed: its node index already used by another card (a click selects that
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
cabled card's node index a duplicate. The Director still asks a parked card's
machine for its status and displays, so its real video outputs show on the
card before the first cable; it stays out of Output Monitor. A newly placed card takes the next free
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
Width, Height and Refresh. Refresh, on bindings, cards and the sync policy
alike, is one field in hertz: type `23.976`, `29.97`, `59.94` or `50`, and the
NTSC rates are kept as their exact fractions (24000/1001 and so on), which is
what the GPU and the sync check compare. Disconnecting the graph
cable removes its generated binding and region; a region shared by another
binding is retained. Feed indices refer to the numbered rows in Feed Mapping
and are never inferred from physical display order.

To move a cable to a different output of the same machine, drag that output
onto the same input: an input holding a cable from the same machine lights green and reads
`RELEASE TO MOVE THE CABLE HERE`, and the drop swaps the cable in one undoable
step, handing the old cable's feed and its Feed Mapping region to the new
output. The machine stays cabled throughout. An input cabled from a different
machine still refuses the drop.

**One output, several processors (NVIDIA Mosaic).** Mosaic joins several
physical connectors into one surface, so Windows - and the render machine's
card - shows one output, say 7680x1080 for four 1920x1080 connectors. Cable
that one output to every processor it feeds: a Render Server video output takes
any number of cables (other outputs still take one). Each cable carries a
region of the output, shown under **Carries** when the cable is selected:

- A lone cable carries the whole output.
- Several cables share it. When the render machine reports the Mosaic grid,
  each cable takes a display of it - reading **Its Mosaic display** - in
  order: cables by where their processors sit on the map, left to right, then
  by input; displays row by row. Otherwise they take equal slices of the
  output's width, left to right (**An even share of the output**).
- **X**, **Y**, **Width** and **Height** set a cable's region by hand - a 2x2
  grid, uneven displays, or processors placed out of order - and the reset
  arrow beside **Carries** gives it back its share.

The signal check compares each processor input with its cable's region, and
**Build from hardware** crops each processor's viewport from that region of the
one window. Removing one of an output's cables leaves its binding and Feed
Mapping region in place for the others; the last cable takes them with it.
Reading the grid from the NVIDIA driver needs a build with
`MEDIASERVER_ENABLE_NVAPI=ON`; without it, set 2x2 and other grids by hand.

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

## Hardware Map and LED processors

**Setup** (or **Window > Hardware Map** / **Feed Mapping**) opens both setup
panels together as one large floating window, tabbed, over most of the editor.
It never blocks the panels around it: Program, the Timeline and the Stage stay
usable beside it. A setup panel dropped into a dock by hand is lifted back out
into the floating window when it is opened. Close it with its cross; the next Setup brings it back.

The map is a small isometric stage. Each device stands on a floor tile as a
pixel-art model - a tower for a Render Server, a desk for an Operator Machine,
a rack box for an LED Processor (a NovaStar one has a front LCD and an orange
key, a Brompton one is a slim box with a blue light bar) and a cabinet wall
for an LED Wall - with its name tag above it. Its ports are sockets on its
faces: inputs down its left face, outputs down its right. A cable runs out of
its socket, down to the floor and along the grid to the input it plugs into,
so the map reads like the stage it describes. Hover or select a device to see
its port names and state; the selected device's tile is ringed. **Frame every
device** (the corners icon beside the zoom readout) centres the stage on what
is on it; the map also frames itself when it opens or a project loads. The
art is `engine/editor/assets/hardware_sprites.png`, made by
`scripts/generate-hardware-sprites.py`; an artist may repaint it in place,
keeping each sprite's rectangle.

An LED Processor is **Generic** until it is given a protocol: a box with
cables that nothing asks about. A processor card has no resolution or rate of
its own: what each input receives is the mode of the render output cabled to
it, and what the LEDs show is the size on the LED Wall card it drives. Drag **NovaStar processor** from the palette,
or set **Protocol** to **NovaStar COEX** in its details, and give it the
controller's **Address** (the IP on its front LCD; `ip:port` reaches a
simulator). The Director then reads it every two seconds over the COEX HTTP
API on port 8001, from a thread of its own: its name and model, each input -
what arrives on it, and the mode its EDID offers - and its screens, with their
raster, cabinets and which part of which input fills them. The card's light
is green while it answers, amber while it is first checked, red when it does
not answer, and its inputs become the card's VIDEO IN sockets, named as the
processor names them. A processor that VMP has locked answers with a refusal,
shown as `Refused:` and the controller's message; release it in VMP. A
processor that stops answering keeps its last report, marked as such. One that
answers but describes no screen with a size says so on a **Screen** line in
its details, in amber, instead of listing nothing.

The first time a processor reports its screens in a session, and nothing is
cabled to its LED OUT, an LED Wall card appears for each screen beside it,
cabled from its LED OUT, with the screen's raster and rate (a 23.976 Hz screen
becomes 24000/1001). It is a starting point, placed as one step that undo
takes back: rename, resize or delete the walls as you would any card, and a
deleted one does not come back while the project is open. A processor's LED
OUT may drive several walls, one per screen.

A **Brompton processor** (Protocol **Brompton Tessera**) is read the same way
through Tessera's IP control API on port 80, which software 3.1 and later
offer once IP control is switched on in the processor's Live Control tile; a
processor with it off answers `Refused:` and says so. Tessera reports less
than COEX: the one input in use (its port, raster and rate) as VIDEO IN 1,
the model and software, the active preset, how many panels it controls,
answer and report faults, and whether blackout, freeze or a test pattern is
on. It reports no EDID and no screen layout. A Brompton card's light turns
amber, and its status says why, when an override is on or panels are offline
or faulted; its details list the panels, preset and overrides. Cable the
render output a Tessera takes into its input 1; cables into other inputs are
not checked, since the processor does not say what they receive.

Processors do not announce themselves. **Find processors**, at the right of
the DETECTED ON NETWORK strip, searches once: it tries port 8001 (NovaStar)
and port 80 (Brompton - many devices answer there, and only one that names a
Tessera model counts) on this machine and every host of each local IPv4
subnet (the /22 around this machine on a larger one), and lists each
processor that answers as a card. Drag it onto the map, or click it, and the
processor card arrives with its name, address and protocol. A processor
already on the map selects its card.

Every cable from a Render Server output into a monitored processor input is
checked against the output's mode - its binding's raster and rate, or the card's
own for an unbound card. A cable turns amber, with a warning at its turn on
the floor, when the input's **EDID** offers another mode (the render machine's
GPU is then never offered the mode the output is set to), when the input
**receives** a different mode, or when it sees **no signal**. Rates within a
hundredth of a hertz are the same rate; 59.94 and 60 are not. Select the cable
to read which; on an EDID mismatch **Match processor EDID** asks the processor
to offer the output's mode on that input, and a processor's details offer the
same for each mismatched input. This is a request to the device, not a project
edit: there is no undo, and the outcome shows in the processor's details.

In Feed Mapping, **Build from hardware** lays the Feed Map out from the
cabling and what the processors report. Each LED screen gets its place in the
canvas, side by side in map order (a new row past 16384 pixels), and the canvas
is sized to hold them; in every window a processor input shows, the viewport is
the part of the window the processor takes, cropping the canvas pixels that
land there on the wall. A processor that reports nothing - Generic, not yet
answering, or a Brompton, whose IP control has no layout - gets its windows
side by side, each shown whole - unless it has one input and drives one wall
smaller than that input's signal, which then takes the top left of the
signal, the size on the wall card, as processors do by default. Four
processors each showing the top-left 2880x1536 of a 3840x2160 output, with
2880x1536 wall cards, build an 11520x1536 canvas, one continuous wall, with a
2880x1536 viewport in each output. The build replaces
every viewport of the windows it lays out (not other windows, nor surfaces),
says what it did and anything it could not do, and one undo takes it back.

To try all of this without a controller, run `python scripts/coex-simulator.py`
(add `--edid-rate 59.94` or `--port 18001` as wanted). It answers as one
All-In-One MX40 Pro with four inputs and one 6912x1536 screen, accepts EDID
writes, and is found by Find processors. `python scripts/tessera-simulator.py`
stands in for a Tessera S8 on port 80 (`--port 8080` if 80 is taken, then
type `127.0.0.1:8080` as the card's address); a PUT of `{"data": true}` to
`/api/override/blackout/enabled` shows an override on the map.

Other processor brands, sync generators, storage, recorders and cameras are
not on the map yet; see the roadmap.

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
`scripts/run-output-node.ps1`; simulated samples are marked as such all the
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

Use Library to import media or models, then drag them into Stage. Models can
be glTF/GLB, OBJ or FBX (binary or ASCII). An FBX file is converted from its
own axes and units to the stage's Y-up metres, with node pivots and mirroring
applied, and imports as static geometry: its animation, skinning and blend
shapes are not used. Its texture paths are tried as written and then by file
name beside the model, so a copied asset folder still resolves; a texture that
cannot be found or decoded (PNG, JPEG, TGA or BMP) refuses the import and names
the file. The Inspector
and Timeline author ordinary document commands, so their changes participate in
replication and undo/redo. The conventional **Edit** menu carries Undo, Redo,
Copy, and Paste, with **Ctrl+Z** for undo and **Ctrl+Shift+Z** or **Ctrl+Y** for redo; **Ctrl+S** saves the project. A
new edit after undo starts a new branch and clears redo only for that authoring
context. Selecting a Scene/Stage item or a Compositor node/canvas chooses which
of those two histories the global actions navigate.

The **History** strip along the bottom of the workspace shows that history as
a row of steps, oldest first, each named for what it did ("Change position of
2 objects", "Delete group "Lights"", "Paste clips"). A marker stands after
the current step; steps after it are dimmed and are what Redo would bring
back. Click a step to go to just after it, or **Start** to go back before
every listed step: every step in between is undone or redone together, as one
change. Hovering a step outlines the elements it touched on Stage in amber.
The **Scene** and **Compositor** buttons choose which history it shows. It
follows the one you last edited in; click a button to pin that history, and
click it again to follow your edits again. When the scene you are editing is
the one on air, a jump of more than one step asks for a second click (the step
glows amber and reads "Click again"), because the wall changes all at once.
When the steps run past the strip's ends, a thin bar along its bottom edge
shows where you are; drag it, or click along it, to scroll, or use the mouse
wheel over the steps. The strip keeps the latest 256 steps of each history; older ones show as
"N earlier", and opening a project starts a new history. It is not a docked
panel and does not move: the chevron at its left end puts it away, and the
**History** chip at the left of the status bar brings it back. The editor
remembers which you chose.

A clip dropped into a scene lands *placed* on the
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
over what is behind it, including a card standing in front of several screens
at once. A card that passes through a media screen is hidden where it is
behind the screen. The screen is hidden where it is behind the card's fully
opaque part. Where a see-through part of the card is in front of the screen it
passes through, it can still be drawn under the screen at some camera angles.

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
port is connected and a ring is open. A node's header carries the colour of its Add Node category.

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
output reaches nothing is parked: it is drawn faded, and parking a node to keep
it for later is not an error. A node that needs an input it does not have yet -
a Composite with nothing on LAYERS, a Merge with no BASE, a Matte without both
its images - is disabled until it is wired: its detail line says `DISABLED`
(hover it to see what it is waiting for), it is drawn faded, and its wires
carry nothing, so whatever it feeds waits with it. An Output with nothing wired
is disabled the same way, and Program then shows the scene as authored. A node
with a problem carries a mark
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

**Chroma Key** shows the key colour as a colour well above its Key Red, Green
and Blue values, then Tolerance, Softness, Edge px (whole pixels: positive grows
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
| (menu) | UV checker: each surface's texture mapping as a coloured checker, red rising along U and green along V |
| (menu) | World normals: which way each surface faces, as a colour |

Shading belongs to the view: in four views each can shade differently, and
Program and the outputs always show the lit picture. Every shading but Lit
and Lights only draws no atmosphere (the renderer reports fog idle there), and
nothing is mirrored in a planar reflection while it is on. Picking, the gizmo, Measure and
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
| Framed square with Proxy / Full res | Which file Stage decodes for video above 720p - see [Editor proxies](media-playback.md#editor-proxies). On Stage a bar along its foot fills as the project's editor proxies are made; on Program it fills while some clips still play their source waiting for theirs |

The rate at the right end is how fast Stage is drawing. Program Output's bar
carries the same media and resolution menus, chosen separately from Stage's. Over the image's
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
ordinary group; the vehicle processing workflow itself is built on
[platesets](#vehicle-process-platesets).

**Highlight Color...** in a row's right-click menu marks the selected rows with
one of the eight marker colours - a tint across the row and a bar at its right
edge that stays visible while it is selected - or **None** to clear it. A
highlight is saved with the project, replicated and undoable, and never
changes the picture. Blender-linked rows do not take one.

Hardware Map and Feed Map are setup panels, opened together in one large
floating window. **Build from hardware** starts a Feed Map from the cabling;
see [Hardware Map and LED processors](#hardware-map-and-led-processors).
Feed mappings support flat,
horizontal-arc, and imported-mesh surfaces. For an imported projection mesh,
split the calibrated LED geometry externally, retain a shared model origin, and
import each feed mesh with `+ IMPORT UV MESH` as glTF/GLB, OBJ or FBX. FBX
geometry is converted to Y-up metres first, so pieces exported from the same
file keep their shared origin. Its persistent projection asset
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

Each surface faces the way its **YAW** and **PITCH** turn it in Surface
Calibration. Yaw turns the face about the stage's up axis. Pitch then tilts it
about its own right edge: 90 degrees lays it flat facing down, which is a
ceiling over the viewer, and -90 degrees is a floor. A ceiling at the height
of a front wall's top edge, with its centre half its depth from that edge
towards the viewer, meets the wall along that edge: the bottom row of the
ceiling image continues the top row of the wall. Side walls are walls with a yaw of 90 and -90 degrees.
Every face renders its own view from the one shared eye, so the picture lines
up across each fold as long as the calibrated corners meet.

The **Stage** setting under the surface list says where all the surfaces
stand:

- **Fixed** (the default) keeps them in the world. The camera moves among
  them, as in a virtual-production volume, and tracked-camera plates are drawn
  on them.
- **Follows camera** carries the whole stage with the scene camera, for an
  immersive room or CAVE whose camera move is the show. Surfaces are then
  measured in the room: stage -Z is where the camera looks, +Y its up and +X
  its right. **Viewer eye** is where the viewer stands in the room, in metres;
  that point rides on the scene camera's eye, so a room measured from its
  floor centre with the eye at 1.6 m up flies with the camera at head height.
  On Stage the surfaces are drawn where the authored camera puts them; place
  them from Feed Mapping or Properties, since the Stage gizmo does not drag a
  surface on a carried stage. A tracked camera has no fixed wall to frame a
  plate on, so outputs report that instead of drawing one.

Screen-space effects (bloom, ambient occlusion, depth of field, temporal
anti-aliasing) run per face, so near a fold between two faces they can differ
slightly where they would blend across a flat wall. Compositor effects defined
over the canvas, such as gradients, do not continue around a fold; place
anything that should wrap the room in the scene instead.

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

The editor's file, folder and unsaved-changes dialogs never pause the show:
Program keeps playing and render nodes stay in lockstep while one is open, and
the answer takes effect on the next frame. Only one such dialog is open at a
time; asking for another while one is up shows "Another dialog is already
open" in the status bar instead. If another project is opened while a dialog
is up, its answer is dropped rather than applied to
the new project.
`.mstage` saves are checksummed and atomically replaced. Recovered WAL state
takes precedence at startup; otherwise the last successfully opened/saved
project is offered through recent-project recovery. A damaged or inaccessible
project safely falls back to untitled startup.
