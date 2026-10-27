# The hooks

A DLL mod never changes the code of the game. It asks the extender for a callback before or after a native function.

## Function callbacks

- A callback names a function by its module (`TS4_x64.exe` or `Simulation_x64.dll`) and the signature of its first
  bytes.
- The key of a function is the module and the normal form of the signature. Two texts of one signature give one key.
- A callback gets 0 to 4 integer or pointer parameters and an integer result.
- All callbacks on one function must have the same `parameterCount`, because the function has one hook.
- A signature must match exactly one place in its module. Else the function gets no hook, and the log tells which
  callbacks do not run.
- Two signatures can find one address. The extender joins their callbacks into one hook. It refuses the join if the
  `parameterCount` values are different.
- A hook in `Simulation_x64.dll` waits until the game loads that module.

## The owners of the hooks

The hook manager of common owns the hooks of the mods. Two other parts have their own MinHook hooks:

- **The bootstrap hook** on the tick, until the first ticks end. Then the hook manager takes the tick. See
  [start.md](start.md).
- **The menu bar** on the swap chain. The game can present on a thread that is not the main thread, and these hooks
  must work with the EA overlay and the Discord overlay. See [menu-bar.md](menu-bar.md).

## Where the callbacks run

- The hook manager makes the callbacks run only on the main thread. See [threads.md](threads.md).
- A call of the function on another thread, for example the simulation thread, runs only the original function.
- The tick and the quit function of the game are found by signature, as the functions of the mods are.
