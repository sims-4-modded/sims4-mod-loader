# The quit

The extender hooks the first step of the shutdown of the game application (the quit hook). The game calls it one
time, on the main thread, after the main loop stops and before the services of the game stop.

The quit hook:

1. stops the ticks of the extender;
2. calls `Ts4ModShutdown` of each loaded DLL mod, in the reverse order of the loads;
3. stops the menu bar: the window procedure and the swap chain get their original functions back;
4. stops the monitor of `mods.json`;
5. removes the marker of the [start-up guard](startup-guard.md).

The DLLs stay in memory until the process stops, because the hooks of the game can still call them.

## What a run showed

- A quit from live mode ran the quit hook. The log has "the game quits; the extender stops the DLL mods and the menu
  bar", then "the extender stopped".
- A quit from the main menu wrote no quit line. See [follow-ups.md](follow-ups.md).

If the extender does not find the quit function, the log tells it, and the DLL mods do not stop when the game quits.
