# The menu bar

The menu bar is a Dear ImGui main menu bar at the top of the game window. The menus come from the registrations of
the mods and of the extender.

- It hooks `Present`, `Present1`, `ResizeBuffers` and `ResizeBuffers1` of the D3D11 swap chain. The game presents
  with `Present1`.
- The slots of `IDXGISwapChain` are fixed by the COM interface, so the code can name them.
- It finds the game window by the process and the title. The class of the window changes at each start.
- It starts at the late start (tick 120), only with `liveReload` and `overlay.enabled`.
- An error stops only the menu bar. The game continues.

## When it shows

- The extender cannot tell yet when the game menu is open. Thus the bar shows only when the user pins it.
- `Ctrl+Shift+F4` (the action `toggleOverlay`) pins and unpins it.
- `overlay.pinned` set to `true` pins it at the start.
- While it shows, the main thread gives it a new state of the mods each 15 frames.

## The text size

- `overlay.fontScale` sets the size of the text.
- With no value, the bar calculates the size from the window height and the DPI. For example, 1.35 at 5120x1440.

## Threads

- The main thread installs, ticks and stops the bar, and runs the commands of the controls.
- The render thread draws the bar in the hook on `Present`. It calls no function of a mod.
- The thread of the game window gives the input to the bar.
