# The docs of the mod loader

Read the files in this order. Each file is short and has one topic.

1. [What the mod loader is](overview.md): what it does, and what came from the Masters of Albion loader.
2. [How the game loads it](game-load.md): the proxy `winmm.dll` and the wait for the decryption.
3. [The start](start.md): the start thread, the bootstrap hook and the first tick.
4. [The DLL mods](dll-mods.md): the folder and the checks of each DLL.
5. [The start-up guard](startup-guard.md): what turns off after a start that did not complete.
6. [The settings file](settings.md): the keys of `mods.json` and the live reload.
7. [The hooks](hooks.md): the function callbacks and the owners of the hooks.
8. [The menu bar](menu-bar.md): the Dear ImGui bar on the swap chain of the game.
9. [The threads](threads.md): which thread runs which work.
10. [The quit](quit.md): what stops when the game quits.
12. [Open items](follow-ups.md): the work that is not done.

The research of how the game loads the extender is in [research/loader/load.md](research/loader/load.md). It cites
the addresses of one game version; these docs do not.

The interface between the extender and a DLL mod, with the full sequence of a mod load, is in the
[design of common](https://github.com/sims-4-modded/common/blob/main/docs/design.md#the-interface-between-the-extender-and-a-mod).
