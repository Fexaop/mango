---
title: Miscellaneous
description: Advanced settings for XWayland, focus behavior, and system integration.
---

## System & Hardware

| Setting | Default | Description |
| :--- | :--- | :--- |
| `xwayland_persistence` | `1` | Keep XWayland running even when no X11 apps are open (reduces startup lag). |
| `xwayland_ignore_scale` | `0` | DIsable global scale for xwayland.|
| `sync_obj_enable` | `1` | Enable `drm_syncobj` timeline support (helps with gaming stutter/lag). **Requires restart.** |
| `allow_lock_transparent` | `0` | Allow the lock screen to be transparent. |
| `allow_shortcuts_inhibit` | `1` | Allow shortcuts to be inhibited by clients. |
| `auto_reload_config` | `1` | Watch the main config file and every `source`/`source_optional` file, then reload immediately after any of them changes. |

## Focus & Input

| Setting | Default | Description |
| :--- | :--- | :--- |
| `focus_on_activate` | `1` | Automatically focus windows when they request activation. |
| `sloppy_focus` | `1` | Focus follows the mouse cursor. |
| `warp_cursor` | `1` | Warp the cursor to the center of the window when focus changes via keyboard. |
| `cursor_hide_timeout` | `0` | Hide the cursor after `N` seconds of inactivity (`0` to disable). |
| `cursor_hide_on_keypress` | `0` | Hide the cursor on keypress. |
| `drag_tile_to_tile` | `0` | Allow dragging a tiled window onto another to swap their positions. |
| `drag_tile_small` | `1` | Allow dragging a tiled window temporarily to small size.|
| `drag_corner` | `3` | Corner for drag-to-tile detection (0: none, 1–3: corners, 4: auto-detect). |
| `drag_warp_cursor` | `1` | Warp cursor when dragging windows to tile. |
| `axis_bind_apply_timeout` | `100` | Timeout (ms) for detecting consecutive scroll events for axis bindings. |
| `disable_middle_paste` | `0` | Disable middle-click paste by turning off the whole primary selection. Only affects Wayland apps; X11 apps are unaffected. Restart apps after re-enabling. |

## Multi-Monitor & Tags

| Setting | Default | Description |
| :--- | :--- | :--- |
| `focus_cross_monitor` | `0` | Allow directional focus to cross monitor boundaries. |
| `focus_direction_only_zone_overlap` | `1` | When enabled, directional focus only selects windows that overlap the current window on the perpendicular axis (y for left/right, x for up/down); returns nothing if none qualify. |
| `exchange_cross_monitor` | `0` | Allow the `exchange_client` and `move_client` dispatchers to reach across monitor boundaries. With `exchange_client` the two windows swap monitors; with `move_client` the window moves onto the monitor holding the neighbor (or lying in the move direction when there is none) and is inserted in front of or behind that neighbor instead of swapping with it. While disabled, both dispatchers keep the windows on the current monitor. |
| `focus_cross_tag` | `0` | Allow directional focus to cross into other tags. |
| `view_current_to_back` | `0` | Toggling the current tag switches back to the previously viewed tag. |
| `scratchpad_cross_monitor` | `0` | Share the scratchpad pool across all monitors. |
| `special_tag_cross_monitor` | `0` | Migrate windows on other monitors' special tags to the monitor where the special tag is activated, copying their special-tag layout. Floating windows are rescaled and centered for the target monitor. |
| `single_scratchpad` | `1` | Only allow one scratchpad (named or standard) to be visible at a time. |
| `tag_num` | `9` | Number of tags/workspaces (1–31). On config reload or runtime changes, clients on tags beyond this count are moved to the last tag. |
| `tag_gather` | `0` | Workspace numbering: `0` keeps fixed tags; `1` gathers tags while retaining an empty active workspace; `2` retains the active workspace and waits for transitions to finish before compacting abandoned empty slots, keeping the focused workspace visually anchored. |

```ini
# Compact abandoned empty workspaces after arrival; keep an active empty slot.
tag_gather=2
```

For example, windows on tags 1, 3, and 9 become workspaces 1, 2, and 3 in
that order. Viewing the old tag 9 keeps that workspace selected as 3, not 1.
Changing focus never rotates or reprioritizes occupied workspaces.

All empty workspace numbers after the last occupied one alias a single trailing
empty workspace. With occupied workspaces 1–4, requests for 5, 6, 7, or 9 all
target workspace 5, before any animation starts. Repeating an alias during travel
does not restart the switch. Once workspace 5 contains a window, workspace 6
becomes the trailing empty destination. Empty workspaces retained in the middle
keep their existing identity until departure and deferred cleanup.

Closing the last window on workspace 2 keeps that empty workspace in place
while you remain there. Opening another window refills the same workspace.
After you switch to workspace 3 and all travel, zoom, and window animations
finish, the abandoned empty slot is removed and the old workspace 3 becomes 2.
The focused workspace stays at the same screen position; in overview the other
workspaces close the gap around it rather than moving the focused card.

Returning to the empty workspace before arrival keeps it. With animations
disabled, cleanup happens after the destination is applied. Occupied workspaces
retain their windows and layouts, independently on each monitor; when no windows
remain, an empty workspace is still available. Set `tag_gather=0` for fixed tags.

## Window Behavior

| Setting | Default | Description |
| :--- | :--- | :--- |
| `enable_floating_snap` | `0` | Snap floating windows to edges or other windows. |
| `snap_distance` | `30` | Max distance (pixels) to trigger floating snap. |
| `float_full_to_top` | `0` | Let fullscreen, floating and layer-shell `top` windows share one layer so they can cover each other; which one ends up on top depends on which was opened or raised last. When `0`, they are split into separate layers instead: floating windows below, fullscreen windows above layer-shell `top` windows. |
| `no_border_when_single` | `0` | Remove window borders when only one window is visible on the tag. |
| `smart_gaps` | `0` | Disable gaps when only one window is present. |
| `idle_inhibit_ignore_visible` | `0` | Allow invisible clients (e.g., background audio players) to inhibit idle. |
| `idle_inhibit_when_fullscreen` | `0` | Keep idle inhibited while a fullscreen window is focused. |
| `tag_carousel` | `0` | Enable tag carousel (cycling through tags). |
| `enable_border_resize` | `1` | Change the cursor when hovering a window border and start an interactive resize with a plain left click, like sway. Floating windows expose every edge; tiled windows only expose edges shared with a neighbor. |
| `border_resize_size` | `10` | Thickness in pixels of the resize grab strip measured inward from a window's edge (`0` uses the window border width). Set it larger for an easier-to-hit grip, which also works for borderless windows. |

## Config errors

When the config file contains errors, mango shows a `mangonag` bar at the top of
the focused output.

- **Close** dismisses the bar.
- **Edit** opens the first reported error in your editor at the reported line.

The editor is taken from `$EDITOR`, then `$VISUAL`, then `vi`; terminal editors
open in `$TERMINAL`.
