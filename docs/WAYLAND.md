# Wayland compositor for Windows GUI programs (spec)

Owner decision: Windows programs run on Wine's own stack, natively, through
`winewayland.drv`. MergedOS therefore needs a small Wayland compositor whose
toplevels are windows of the MergedOS window manager. Written from the
protocol XML; no code from other projects, no libwayland. Source: `src/wayland/`.

## Shape

- `wl_wire.c`: wire protocol. 32-bit host-order words, 8-byte header
  (object, size<<16 | opcode), strings and arrays padded to 4, fds out of
  band (SCM_RIGHTS) queued in order. Bounds-checked reader, overflow-checked
  writer, no allocation. Truncated or malformed input never reads out of bounds.
- `wl_comp.c`: the protocol state machine, portable (no syscalls). It is fed
  bytes + fds per client and produces bytes + fds; the platform supplies a
  `struct wl_host` (map/unmap shm, window create/pixels/title/destroy, keymap fd).
- `wl_main.c`: the guest process (Linux personality, static ELF, raw syscalls,
  no libc): listening AF_UNIX socket `$XDG_RUNTIME_DIR/wayland-0`
  (`/tmp/wayland-0`), poll loop, `/dev/mwm` for windows and input.
- Host tests (`tests/test_wayland.c`, ASan): codec round trips, truncation
  and corruption fuzz, a test client over a socketpair against the real state machine.

## Globals

Wine 9.0 (Debian `winewayland.so`, strings/imports inspected offline) requires
`wl_compositor`, `xdg_wm_base`, `wl_shm`, `wl_subcompositor`,
`zwp_pointer_constraints_v1` and `zwp_relative_pointer_manager_v1`; it
optionally binds `wl_seat`, `wl_output`, `wp_viewporter`, `zxdg_output_manager_v1`.
It links libxkbcommon and parses the keymap we send. No xdg_toplevel_icon
or clipboard in 9.0. We serve:

- wl_compositor v5, wl_subcompositor v1, wl_shm v1 (ARGB8888, XRGB8888)
- xdg_wm_base v5 (toplevel, popup, positioner), wl_seat v7 (pointer, keyboard;
  touch absent), wl_output v4 + zxdg_output_manager_v1 v3, wl_data_device_manager v3 (stub:
  objects exist, no selection ever offered), wp_viewporter v1 (destination scaling)
- zwp_pointer_constraints_v1 (lock/confine accepted and reported active),
  zwp_relative_pointer_manager_v1 (deltas from motion)

## Windows and input

- xdg_toplevel -> one WM window; content size = surface/window-geometry size.
  Subsurfaces and xdg_popups are blitted into their toplevel's window
  (popups clipped to it). Buffers are copied on commit, `wl_buffer.release`
  is sent at once. ARGB alpha is ignored (opaque copy). Frame callbacks fire on commit.
- `/dev/mwm` (kernel/include/mergedos/mwm.h): `write` = commands (create,
  destroy, title, pixels, resize), `read` = 32-byte events (pointer motion,
  button, key, focus, close request). Nonblocking read supported. One opener.
- Keys: the kernel console delivers ASCII; the compositor maps characters back
  to evdev keycodes + shift (US layout) and sends press+release.
  The served keymap is a fixed text file (`wl_keymap.h`) sent through a memfd.

## Limits (v1)

No damage tracking beyond the committed buffer, no shm resize of live
buffers beyond pool growth, no cursor images, no axis/touch, no
fullscreen/maximise handling (acked, ignored), single output, single seat.
