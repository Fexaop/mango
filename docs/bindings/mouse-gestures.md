---
title: Mouse & Gestures
description: Configure mouse buttons, scrolling, gestures, and lid switches.
---

## Mouse Bindings

Assign actions to mouse button presses with optional modifier keys.

> **Info:** All of the bindings in this page (`mousebind`, `axisbind`, `gesturebind`, `switchbind`) support key modes via `key_mode=<name>`, using the exact same rules as `bind`. See [Keys: Key Modes](/docs/bindings/keys).

### Syntax

```ini
mousebind=MODIFIERS,BUTTON,COMMAND,PARAMETERS
```

- **Modifiers**: `SUPER`, `CTRL`, `ALT`, `SHIFT`, `NONE`. Combine with `+` (e.g., `SUPER+CTRL`).
- **Buttons**: Can be specified in one of the following ways:
  - **Standard Names**: `btn_left`, `btn_right`, `btn_middle`, `btn_side`, `btn_extra`, `btn_forward`, `btn_back`, `btn_task`
  - **Hardware Codes**: `code:NUMBER` (e.g., `code:272`, `code:273`, useful for binding non-standard or extra mouse buttons)

### Examples

```ini
# Window manipulation
mousebind=SUPER,btn_left,moveresize,curmove
mousebind=SUPER,btn_right,moveresize,curresize
mousebind=SUPER+CTRL,btn_right,killclient

mousebind=NONE,code:273,togglemaximizescreen,0
```

---

## Axis Bindings

Map scroll wheel movements to actions for workspace and window navigation.

### Syntax

```ini
axisbind=MODIFIERS,DIRECTION,COMMAND,PARAMETERS
```

- **Direction**: `UP`, `DOWN`, `LEFT`, `RIGHT`

### Examples

```ini
axisbind=SUPER,UP,viewtoleft_have_client
axisbind=SUPER,DOWN,viewtoright_have_client
```

---

## Gesture Bindings

Enable trackpad swipe gestures for navigation and window management.

### Syntax

```ini
gesturebind=MODIFIERS,DIRECTION,FINGERS,COMMAND,PARAMETERS
```

- **Direction**: `up`, `down`, `left`, `right`
- **Fingers**: `3` or `4`

> **Info:** Gestures require proper trackpad configuration. See [Input Devices](/docs/configuration/input) for trackpad settings like `tap_to_click` and `trackpad_disable_while_typing`.

### Continuous gestures

`gesture_live=1` uses Mango's trackpad driver to follow finger movement. It
drives workspace transitions, overview zoom, and window panning in horizontal
and vertical scroller layouts, including inside the zoomed overview.

```ini
gesture_live=1

# Four fingers: open/close overview
gesturebind=none,up,4,enteroverview
gesturebind=none,down,4,leaveoverview

# Three fingers: pan through windows; up/down switches tags
gesturebind=none,left,3,focusstack,next
gesturebind=none,right,3,focusstack,prev
gesturebind=none,up,3,viewtoright,0
gesturebind=none,down,3,viewtoleft,0
```

Hold your fingers still to hold the transition; reverse them to move back.
Release past `gesture_swipe_cancel_ratio` to finish, or use a quick flick.
Short swipes return to the starting state, and cancelled gestures return even
after crossing the commit threshold. Scroller windows settle using their native
layout when you release. In other layouts, `focusstack` changes the selected
window on release using Mango's normal focus order.

Set `gesture_live=0` to act only on release. See [Input Devices](/docs/configuration/input)
for distance, cancellation, and flick settings.

The default configuration also keeps four-finger horizontal workspace gestures:

```ini
gesturebind=none,right,4,viewprev_have_client
gesturebind=none,left,4,viewnext_have_client
```

---

## Switch Bindings

Trigger actions on hardware events like laptop lid open/close.

### Syntax

```ini
switchbind=FOLD_STATE,COMMAND,PARAMETERS
```

- **Fold State**: `fold` (lid closed), `unfold` (lid opened)

> **Warning:** Disable system lid handling in `/etc/systemd/logind.conf`:
>
> ```ini
> HandleLidSwitch=ignore
> HandleLidSwitchExternalPower=ignore
> HandleLidSwitchDocked=ignore
> ```

### Examples

```ini
switchbind=fold,spawn,swaylock -f -c 000000
switchbind=unfold,spawn,wlr-dpms on
```
