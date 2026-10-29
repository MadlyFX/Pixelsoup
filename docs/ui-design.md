# Editor UI design

The editor is built for an operator who may be fourteen hours into a show day.
Every panel, widget and new control follows the four principles below. The
code that carries them lives in `engine/gui` (`Theme`, `UiContext`, icons,
the colour picker) and `engine/editor/include/mediaserver/editor/operator_widgets.hpp`.
Reuse those pieces rather than drawing controls by hand.

## Principles

1. **Soft and obvious.** Rounded, friendly shapes with hit targets that are
   easy to find tired. The next step is always visible.
2. **Light RGB accents.** Colour comes from state, like an LED wall: calm
   surfaces, with glow only on what is live or waiting for the operator. The
   wall picture should stay the most saturated thing on screen.
3. **Pictures over words.** Frequent actions are drawn icons with a tooltip,
   not text buttons.
4. **Comfortable and in flow.** Nothing startles, blocks or makes the
   operator hunt. Motion is small and quick. Dangerous actions are hard to hit
   by accident.

## Shape

- Use `ctx.rounded`, `rounded_outline`, `glow` and `shadow`. Do not draw
  square `ctx.panel` rects for controls, cards or rows.
- Use the theme radii, never literals:
  - `radius_small` (5) for fields and chips;
  - `radius` (8) for rows, cards and buttons;
  - `radius_large` (12) for panels and floating frames.
  - A pill is `bounds.height * 0.5`.
- Square content inside a rounded card, such as a thumbnail or a clip body,
  is clipped with a corner mask (`add_corner_mask`). It never pokes past the
  curve.
- Sizes:
  - Keep `Metrics::control` (24) the smallest clickable height.
  - List rows are `list_metrics::row_height` (28), menu rows
    `gui::menu_row_height` (28).
  - Give the most-pressed control in a group the largest target. Play is
    wider than the step keys; Take and GO are the biggest keys in the show
    bar.
- Menus, tooltips and popovers are rounded cards with a shadow.

## Colour

Every colour comes from `gui::Theme`. Do not add inline RGB values in panels.
If a new meaning needs a colour, add it to `Theme`, with a comment saying
what it means, and to `night_theme()`.

| Theme colour | Means | Never use it for |
| --- | --- | --- |
| `good` (green) | On air, healthy, playing | Decoration or selection |
| `preview` / `accent` (cyan) | Preview, focus, the engaged tool, what is being edited | On-air state |
| `cue` (magenta) | The cue stack, GO, the next thing to press | General highlights |
| `caution` (amber) | Degraded, behind, changed on disk, connecting; the reset arrow | Errors |
| `warning` (red) | Faulted, missing, destructive | Anything recoverable at a glance |
| `playhead` (warm white) | The current time | State |

- **Glow is a signal.** Use `ctx.glow` only for something live, armed or
  waiting for input. It follows `Theme::glow`, which the night dim lowers.
  A screen where everything glows has no signal.
- **Selection is quiet.** A selected row uses `Theme::selection`, the accent
  held back, so a long selected list does not light up.
- **State is never colour alone.** A state chip pairs its dot with an icon or
  a word.
- **Text is soft.** Text sits a little below white (`Theme::text`). Secondary
  text uses `text_muted`.

## Icons and words

- **Frequent actions:** use `icon_button`, `tool_key` or `pill_button` with an
  icon. Always give a tooltip that names the action and its shortcut, for
  example "Step back a frame (Left)".
- **Rare, destructive or show-critical actions keep a word beside the icon.**
  Examples are Import, Insert/Overwrite/Append, Delete, Stop all and Take.
- **New icons:**
  - Add an entry to `gui::Icon` and draw it in `engine/gui/src/icons.cpp`
    with `Pen` (round caps, `Pen::rounded`).
  - Keep it within the ±9 unit box; `canvas_tests` checks every icon.
  - Do not use text glyphs such as `|<` or `>` as icons.
- **The same action has the same control everywhere.** The Timeline and the
  Asset Viewer transports are both built from `tool_key`. A new transport or
  tool strip reuses it, with the same icons and engaged colours: Play lit
  `good`, modes lit `accent`.
- **Tones:**
  - `ButtonTone::Primary` is for the one thing a panel most wants pressed.
  - `Danger` is ringed and inked in red, never filled.
- **Wording:**
  - Sentence case everywhere: titles, chips and section headers. Use
    `gui::sentence_case` when a name comes from data. Do not use UPPERCASE
    status words.
  - Name actual state in the operator's terms (on air, preview, held,
    fading), as `AGENTS.md` requires. Keep technical detail for tooltips.

## Values

- **Colours:** every colour a user can set is a `ctx.color_well`, which opens
  the shared colour popover (SV field, hue rail, hex, 0–255 RGB, recents and
  LED primaries). Do not build a separate picker. While the popover is open
  or dragging (`color_open` / `color_dragging`), the panel's undo coalescing
  treats the edit as one gesture.
- **Reset to default:** every editable value shows a `reset_button` while it
  differs from its default, Unreal style.
  - The tooltip names the default.
  - Defaults come from one place:
    - default-constructed settings structs;
    - `scene::initial_effect()` for effects;
    - the pristine element in the Inspector.
  - The Director authors new nodes from the same source, so "new" and "reset"
    agree.
  - A new field adds its reset arrow in the same change.

## Motion

Motion makes the UI feel alive without costing speed.

- Use the `UiContext` motion store: `ease` (no overshoot), `spring` (a small
  overshoot for physical things) and `settle` (start a value away from its
  target, as when a compositor node is planted).
- Keep transitions around a tenth of a second.
  - Never delay input. Never animate what the operator is directly dragging.
  - Never block a frame waiting for motion to finish.
- Everything scales with `Theme::motion`. At 0 it snaps, and it snaps when
  frame dt is 0, so tests stay deterministic.
- **Uses:** hover tints, rows gliding to a new order, a drop planting, a
  progress fill. Do not animate decoration that loops forever.

## Comfort and safety

- **Night dim:** `night_theme()` lowers surface luminance and glow, and is
  persisted with the workspace. New colours must look right in both themes.
- **Focus:** keyboard focus always shows a ring.
- **Destructive actions:**
  - Separate them from everyday ones.
  - Show-wide stops need a second press inside a short window, like Stop all
    and its "Confirm stop".
- **Show state:** keep show state in the show bar: Program/Preview, Take, the
  next cue's GO and node health. Do not bury it in a tab.
- **Popovers and menus:** they block the pointer for what lies under them,
  and Escape always backs out.

## Checking a UI change

- **Build and test:** build and test `area-editor` (and `area-server` if
  authoring changed).
- **Look at it:**
  - Render the affected panels with `mediaserver_editor_ui_snapshot`.
  - Check the result at both themes before and after.
  - A layout refactor should be pixel-identical where it means to be.
- **Write it down:** update `docs/operator-guide.md` when an operator-visible
  behaviour, colour meaning or shortcut changes.
