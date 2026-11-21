---
title: Animations
description: Configure smooth transitions for windows and layers.
---

## Enabling Animations

mangowm supports animations for both standard windows and layer shell surfaces (like bars and notifications).

```ini
animations=1
layer_animations=1
```

## Animation Types

You can define different animation styles for opening and closing windows and layer surfaces.

Available types: `slide`, `zoom`, `fade`, `none`.

```ini
animation_type_open=zoom
animation_type_close=slide
layer_animation_type_open=slide
layer_animation_type_close=slide
```

## Fade Settings

Control the fade-in and fade-out effects for animations.

```ini
animation_fade_in=1
animation_fade_out=1
fade_in_begin_opacity=0.5
fade_out_begin_opacity=0.5
```

- `animation_fade_in` — Enable fade-in effect (0: disable, 1: enable)
- `animation_fade_out` — Enable fade-out effect (0: disable, 1: enable)
- `fade_in_begin_opacity` — Starting opacity for fade-in animations (0.0–1.0)
- `fade_out_begin_opacity` — Starting opacity for fade-out animations (0.0–1.0)

## Zoom Settings

Adjust the zoom ratios for zoom animations.

```ini
zoom_initial_ratio=0.4
zoom_end_ratio=0.8
```

- `zoom_initial_ratio` — Initial zoom ratio
- `zoom_end_ratio` — End zoom ratio

## Durations

Control the speed of animations (in milliseconds).

| Setting | Type | Default | Description |
| :--- | :--- | :--- | :--- |
| `animation_duration_move` | integer | `500` | Move animation duration (ms) |
| `animation_duration_open` | integer | `400` | Open animation duration (ms) |
| `animation_duration_tag` | integer | `300` | Tag animation duration (ms) |
| `animation_duration_close` | integer | `300` | Close animation duration (ms) |
| `animation_duration_focus` | integer | `0` | Focus change (opacity transition) animation duration (ms) |

```ini
animation_duration_move=500
animation_duration_open=400
animation_duration_tag=300
animation_duration_close=300
animation_duration_focus=0
```

In `scroller` and `vertical_scroller`, ordinary tiled layout changes snap window
width and height to their destination while x/y movement remains animated.
This includes proportion changes and maximize/restore, both normally and inside
Niri-style overview. Overview previews keep their zoom scale without stretching
through intermediate sizes. `animation_duration_move` and `animation_curve_move`
still control the positional transition.
Adjacent windows retain their native content scale as they slide across output
edges.

Opening/closing effects, overview zoom, workspace transitions, floating windows,
and other layouts retain their existing animation behavior. Client repaint timing
remains asynchronous; scrolling reflows do not request intermediate client sizes.

## Custom Bezier Curves

Bezier curves determine the "feel" of an animation (e.g., linear vs. bouncy). The format is `x1,y1,x2,y2`.

You can visualize and generate curve values using online tools like [cssportal.com](https://www.cssportal.com/css-cubic-bezier-generator/) or [easings.net](https://easings.net).

| Setting | Type | Default | Description |
| :--- | :--- | :--- | :--- |
| `animation_curve_open` | string | `0.46,1.0,0.29,0.99` | Open animation bezier curve |
| `animation_curve_move` | string | `0.46,1.0,0.29,0.99` | Move animation bezier curve |
| `animation_curve_tag` | string | `0.46,1.0,0.29,0.99` | Tag animation bezier curve |
| `animation_curve_close` | string | `0.46,1.0,0.29,0.99` | Close animation bezier curve |
| `animation_curve_focus` | string | `0.46,1.0,0.29,0.99` | Focus change (opacity transition) animation bezier curve |
| `animation_curve_opacity_fade_in` | string | `0.46,1.0,0.29,0.99` | Open opacity animation bezier curve |
| `animation_curve_opacity_fade_out` | string | `0.5,0.5,0.5,0.5` | Close opacity animation bezier curve |

```ini
animation_curve_open=0.46,1.0,0.29,0.99
animation_curve_move=0.46,1.0,0.29,0.99
animation_curve_tag=0.46,1.0,0.29,0.99
animation_curve_close=0.46,1.0,0.29,0.99
animation_curve_focus=0.46,1.0,0.29,0.99
animation_curve_opacity_fade_in=0.46,1.0,0.29,0.99
animation_curve_opacity_fade_out=0.5,0.5,0.5,0.5
```

## Tag Animation Direction

Control the direction of tag switch animations.

| Setting | Default | Description |
| :--- | :--- | :--- |
| `tag_animation_direction` | `1` | Tag animation direction (1: horizontal, 0: vertical) |
| `tag_animation_continuous` | `0` | Animate direct jumps through intermediate tags (0: disable, 1: enable) |

Enable continuous workspace travel with:

```ini
tag_animation_continuous=1
```

A jump from workspace 1 to 3 slides through workspace 2, including when it is
empty; the reverse jump travels through 3, 2, 1. The entire transition uses
`animation_duration_tag` and follows `tag_animation_direction`. Focus and the
active tag change directly to the requested destination.

Workspace travel and overview zoom have separate timelines. Opening overview
does not snap to the destination or restart `animation_duration_tag`; closing or
reopening it also preserves the current workspace position and switch deadline.
Closing a window or recalculating its layout does not start another workspace
transition. Operations during travel use the requested destination's native
layout; new windows are never configured to the scaled preview size.

With `tag_gather=2`, active empty workspaces remain in place. Removing an empty
slot after departure waits for workspace travel, overview zoom, window
animations, and interactions to finish, then renumbers around the focused
workspace without moving it on screen.

This applies to ordinary single-tag views with animations enabled. Multi-tag
views, special workspaces, and carousel wraparound keep their existing
transitions. Niri-style overview already travels through intermediate workspaces
independently of this option.