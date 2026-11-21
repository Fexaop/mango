---
title: Overview
description: Configure overview, jump mode, overcircle, and the window switcher.
---

Mango has four related ways to inspect or switch windows: the standard overview,
jump mode, overcircle, and the switcher. They share some appearance settings,
but each mode has its own entry command and interaction model.

| Mode | Layout | Primary command |
| :--- | :--- | :--- |
| Overview | Packed cards in the normal overview grid | `toggleoverview` |
| Jump mode | Overview grid with one-key jump hints | `togglejump` |
| Overcircle | Focused card in the center with other windows on both sides | `overcircle` |
| Switcher | Centered thumbnail panel | `switcher` |

The keybindings below are examples; replace the shortcuts with ones that are
free in your configuration. For the complete dispatcher reference, see
[Keybindings](/docs/bindings/keys).

## Overview

The standard overview packs every window on the current monitor into a grid.
By default it includes windows from every tag. Pass `1` to
`toggleoverview` to include only windows on the current tagset.

### Keybindings

```ini
# Toggle the overview for all tags
bind=SUPER+ALT,o,toggleoverview,

# Toggle the overview for the current tagset only
bind=SUPER+ALT+SHIFT,o,toggleoverview,1

# Enter or leave without toggling
bind=SUPER+ALT,Return,enteroverview,
bind=SUPER+ALT+Escape,leaveoverview,
```

| Command | Parameter | Description |
| :--- | :--- | :--- |
| `toggleoverview` | `[1]` | Toggle the standard overview. Use `1` to restrict it to the current tagset. |
| `enteroverview` | - | Enter the standard overview. |
| `leaveoverview` | - | Leave the overview. |

The default configuration also provides four-finger swipe gestures for entering
and leaving the overview:

```ini
gesturebind=none,up,4,enteroverview
gesturebind=none,down,4,leaveoverview
```

With `gesture_live=1` (the default), zoom follows your fingers and reverses
with them. Releasing a short swipe or cancelling it returns to the starting
state. Three-finger horizontal swipes pan windows in scroller layouts, while
three-finger up/down swipes switch workspaces. These use Mango's existing
gesture driver; see [Gesture Bindings](/docs/bindings/mouse-gestures#continuous-gestures)
for bindings and release behavior.

### Configuration

Set `overview_style=1` to show zoomed workspaces stacked vertically. Each
workspace uses its configured Mango layout, including dwindle, monocle, tile,
grid, and scroller. Window commands keep their normal Mango behavior while the
overview stays open: focus, move, swap, resize, float, maximize, fullscreen,
change layout, and move windows to tags.

```ini
overview_style=1
overview_zoom=0.5
bind=SUPER,Tab,toggleoverview
bind=SUPER,f,togglefullscreen
bind=SUPER,m,togglemaximizescreen
bind=SUPER+SHIFT,Right,move_client,right
bind=SUPER+SHIFT,3,tag,3
```

Drag a window to rearrange it using the workspace's layout, or to move it to
another workspace. Existing mouse bindings also work on window previews,
including `moveresize,curmove` and `moveresize,curresize`. Left click selects a
window while keeping the overview open; applications receive no keyboard or
pointer focus while overview input is active. `leaveoverview` or toggle-close
restores application focus immediately, even while zooming back in. Reopening
overview immediately blocks application input again. Dragging and resizing
update native geometry; zoom changes rendering, not window size or layout.
Window commands such as maximize target the current workspace selection.
In horizontal and vertical scroller layouts, maximized tiled windows keep
separate scrolling positions rather than overlapping, including after a
configuration refresh. Their maximized state and native size are preserved.
Tiled drags begin with Niri's rubber-band pull: the preview moves against
resistance while the native window stays tiled. Pulling 256 layout pixels
(128 screen pixels at half zoom) detaches it immediately; there is no hold
timer. Releasing early animates it back. Detached windows retain their size
and follow the pointer freely across workspace boundaries.

A landing highlight uses the target workspace's native layout. A narrow
highlight between workspaces inserts a workspace there on release: occupied
workspaces shift down to the first empty tag, preserving their layouts and
window IDs. Hovering does not change tags. If no empty tag remains, the gap
does not offer an insertion target. The highlight uses `drop_color`.

Unmodified arrow keys focus windows while overview is open; configured bindings
take precedence. Layout changes, keyboard moves, and resizes animate using
`animation_duration_move`; workspace switches use `animation_duration_tag`.
The window being dragged follows the pointer directly while surrounding windows
animate into their new positions.
Right click closes the window unless a mouse
binding overrides it. Scrolling switches workspaces. A direct workspace jump
animates through the intervening workspaces, including empty ones.

Desktop switching and overview use one per-monitor workspace camera. Zoom is a
separate animation: entering, leaving, or reopening overview during a switch
preserves its position, destination, and original completion time. For example,
opening overview one second into a five-second switch zooms out while the
remaining four seconds of workspace travel continue.

Numbered workspaces retain their native protocol identities and order while
zooming. Panels see the same workspace list, with only the selected workspace
marked active; overview does not replace the list with a synthetic workspace.

The nested test configuration keeps scroller as the default. Alt+V selects
vertical scroller, and Alt+S returns to horizontal scroller. Alt+Ctrl+Up/Down
switch workspaces; `tag_animation_direction=0` makes ordinary workspace
transitions vertical too. `tag_animation_continuous=1` also makes ordinary
direct jumps travel through intermediate workspaces. Both are enabled in the
nested test configuration, along with `tag_gather=2` to compact abandoned empty
workspaces after arrival. The active workspace remains available when empty.
Plain left drag moves windows; Alt+right drag resizes.

With this mode, trailing empty numbers are aliases: if 1–4 are occupied,
workspace keys 5–9 all select the same empty workspace 5, on the desktop and in
overview. Opening a window there makes workspace 6 available.

Overview references the same native workspace groups and layouts as the desktop.
During a switch from 1 to 3, new terminals and window/layout commands operate on
workspace 3 immediately, even while the camera is crossing workspace 2. Initial
size negotiation and tiling use workspace 3's normal layout, independent of zoom.
The same rules apply while entering or leaving overview; native operations keep
working throughout the exit animation.

Moving a preview between workspaces performs a native tag transfer. Closing the
last window keeps the empty workspace and its surrounding previews in place.
With `tag_gather=2`, that slot is removed only after you leave and all workspace
travel, zoom, and window animations finish. The destination's focused preview
stays centered while the other workspaces close the gap around it. Renumbering
never starts another switch or changes native window geometry.

The shared camera state lives in `Monitor.workspace_viewport`. Native rendering
and overview projection consume that state; overview owns only its zoom and
window-preview transforms. Render frames capture their projection before
temporarily entering a native layout context, so drag/drop and scaled previews
use the same coordinates without depending on borrowed monitor state.

Input ownership is independent of projection lifetime: `overview_input_blocked`
controls interactive overview input; `niri_ov` retains the render projection
until animation finishes. Native configure, workspace membership, and layout
selection never use the input-blocking flag as a geometry mode.

Quickshell can draw a separate wallpaper behind the overview cards, just like
Niri's `place-within-backdrop` layer rule:

```ini
layer_rule=layer_name:^caelestia-backdrop$,place_within_backdrop:1
```

Map a full-output Background-layer surface with that namespace and an empty
input region. Mango uses its existing scene directly: it is hidden on the
desktop and excluded from workspace previews. The normal workspace wallpaper
and windows stay sharp. Without that surface, `overview_backdrop_color` remains
visible. Any wallpaper blur belongs to this separate Quickshell surface only.

Set `overview_style=0` to use the packed overview grid described above.

| Setting | Type | Default | Description |
| :--- | :--- | :--- | :--- |
| `overview_style` | integer | `0` | Overview presentation: `0` packed cards, `1` zoomed workspaces using their existing layouts. |
| `overview_zoom` | float | `0.5` | Workspace scale in the zoomed overview. Range: 0.0001–0.75. |
| `overview_gap_inner` | integer | `5` | Gap between cards. Range: 0–1000. |
| `overview_gap_outer` | integer | `30` | Gap between the card grid and the monitor edges. Range: 0–1000. |
| `enable_hotarea` | integer | `0` | Enter the standard overview when the pointer enters the configured corner (`0`: disabled, `1`: enabled). |
| `hotarea_size` | integer | `10` | Width and height of the hot-area trigger in pixels. Range: 1–1000. |
| `hotarea_corner` | integer | `2` | Trigger corner: `0` top-left, `1` top-right, `2` bottom-left, `3` bottom-right. |
| `hotarea_disable_on_fullscreen` | integer | `1` | Do not trigger the hot area while a fullscreen window is focused (`0`: disabled, `1`: enabled). |

```ini
overview_gap_inner=5
overview_gap_outer=30

enable_hotarea=1
hotarea_size=10
hotarea_corner=2
hotarea_disable_on_fullscreen=1
```

## Jump Mode

Jump mode displays the standard overview grid and places a character hint on
each visible window. Press a hint to focus that window and leave the overview.
Press `Escape` to leave without selecting a window.

The shipped default binding is `ALT+Tab`.

### Keybindings

```ini
# Toggle the overview and its jump hints
bind=ALT,Tab,togglejump,
```

| Interaction | Description |
| :--- | :--- |
| Hint key | Focus the window marked with that hint and close the overview. |
| `Escape` | Close jump mode without changing focus. |

| Command | Parameter | Description |
| :--- | :--- | :--- |
| `togglejump` | - | Enter the overview in jump mode; if an overview is already open, close it. |

### Configuration

| Setting | Type | Default | Description |
| :--- | :--- | :--- | :--- |
| `jump_labels` | string | `HJKLASDFGQWERTYUIOPZXCVBNM` | Ordered characters assigned as window hints. Hints are assigned only to visible windows, so the string length limits how many windows receive one. Label matching uses the configured keyboard layouts and falls back to `us`. |

```ini
# Assign hints in this order
jump_labels=HJKLASDFGQWERTYUIOPZXCVBNM
```

Hint colors, border, padding, corner radius, and font use the
`jump_label_decorate_*` settings. See
[Overview Jump Mode](/docs/visuals/theming#overview-jump-mode) for the complete
list.

## Overcircle

Overcircle keeps the focused window in a centered column and distributes the
remaining windows across columns on its left and right. Repeated use of the same
binding cycles focus around the current monitor.

`next` and `prev` show windows from all tags. `current_next` and
`current_prev` restrict the overview to the current tagset. Releasing a modifier
key exits overcircle.

### Keybindings

```ini
# Cycle forward/backward through all tags on the current monitor
bind=SUPER+ALT,j,overcircle,next
bind=SUPER+ALT+SHIFT,j,overcircle,prev

# Cycle forward/backward through the current tagset only
bind=SUPER+CTRL,j,overcircle,current_next
bind=SUPER+CTRL+SHIFT,j,overcircle,current_prev
```

| Command | Parameter | Description |
| :--- | :--- | :--- |
| `overcircle` | `next` | Enter or cycle forward through every window on the current monitor. |
| `overcircle` | `prev` | Enter or cycle backward through every window on the current monitor. |
| `overcircle` | `current_next` | Enter or cycle forward through the current tagset only. |
| `overcircle` | `current_prev` | Enter or cycle backward through the current tagset only. |

### Configuration

Overcircle uses the same inner and outer overview gaps as the standard
overview, plus settings for the centered column and title message bar.

| Setting | Type | Default | Description |
| :--- | :--- | :--- | :--- |
| `overview_gap_inner` | integer | `5` | Gap between cards in the center and side columns. Range: 0–1000. |
| `overview_gap_outer` | integer | `30` | Gap around the overcircle layout. Range: 0–1000. |
| `overcircle_center_ratio` | float | `0.5` | Width of the centered focused window as a fraction of the usable screen width. Range: 0.1–0.9. |
| `title_message_bar_height` | integer | `52` | Height of the focused window's title message bar. Set to `0` to disable it. |
| `title_message_bar_gap` | integer | `20` | Gap between the focused card and its title message bar. |

```ini
overview_gap_inner=5
overview_gap_outer=30
overcircle_center_ratio=0.5
title_message_bar_height=52
title_message_bar_gap=20
```

The title message bar shows the window title on the first line and its app id
on the second. Its decoration is shared with the switcher; see
[Title Message Bar](/docs/visuals/theming#title-message-bar).

## Switcher

The switcher opens a centered panel of live window thumbnails. Use it to cycle
through the current tag, all tags on the current monitor, or all windows on all
monitors. Releasing any modifier key commits the highlighted window.

The switcher cannot open while any monitor is showing an overview, including
jump mode and overcircle.

### Keybindings

```ini
# Current tag
bind=ALT,grave,switcher,next
bind=ALT+SHIFT,grave,switcher,prev

# All tags on the current monitor
bind=SUPER+ALT,grave,switcher,all_tag_next
bind=SUPER+ALT+SHIFT,grave,switcher,all_tag_prev

# All monitors and tags
bind=SUPER+CTRL+ALT,grave,switcher,all_next
bind=SUPER+CTRL+ALT+SHIFT,grave,switcher,all_prev
```

| Command | Parameter | Description |
| :--- | :--- | :--- |
| `switcher` | `next` / `prev` | Open or cycle through windows on the current tag. |
| `switcher` | `all_tag_next` / `all_tag_prev` | Open or cycle through every window on the current monitor. |
| `switcher` | `all_next` / `all_prev` | Open or cycle through every window on every monitor. |

### Configuration

The switcher has no dedicated size or layout options. Its focused thumbnail
frame uses `focus_color`, other thumbnail frames use `border_color`, and it
shares the title message bar with overcircle.

| Setting | Type | Default | Description |
| :--- | :--- | :--- | :--- |
| `title_message_bar_height` | integer | `52` | Height of the highlighted window's title message bar. Set to `0` to disable it. |
| `title_message_bar_gap` | integer | `20` | Gap between the switcher panel and its title message bar. |
| `focus_color` | color | `0xc66b25ff` | Frame color of the highlighted thumbnail. |
| `border_color` | color | `0x444444ff` | Frame color of the other thumbnails. |

```ini
title_message_bar_height=52
title_message_bar_gap=20
focus_color=0xc66b25ff
border_color=0x444444ff
```

## Mouse Interaction

The overview-based modes and the switcher accept direct pointer input:

| Mode | Left click | Right click |
| :--- | :--- | :--- |
| Overview, jump mode, overcircle | Focus the clicked window and leave the overview. | Close the clicked window. |
| Switcher | Commit the clicked thumbnail. | Close the clicked window. |
| Switcher panel background | Close the switcher. | Close the switcher. |
