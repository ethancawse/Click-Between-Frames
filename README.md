# Summary of Fork Changelog

* NOTE - ONLY SUPPORTED ON WINDOWS
* The mod now uses a faster input pipeline that stays consistent during lag, high polling mice, or high system load. This allows for higher input reliability.
* Lower overhead per frame and input, reducing lag spikes, allowing for increased performance.
* Entering/exiting levels, pausing, dying, minimizing, or disabling the mod properly resets everything to prevent leftover inputs. This allows for safter behavior/stability if something goes wrong.
* Keybind checks are now faster and more lightweight, so custom binds should feel snappier overall (NOTE: Some weird binds may not be supported).
* Added extra Windows thread scheduling/priority options that can improve input consistency on some systems. This should help overall stability/consistency.
* Click-between-frames behavior, physics bypass interaction, safe mode/validity rules, and the endscreen indicators all work the same at a user level (no gameplay changes).

# Changelog

* Replaced the old mutex + `std::deque` input queue with a lock-free single-producer/single-consumer ring buffer for lower overhead and more consistent input handling under load.
* Switched internal buffers to `std::vector` + index cursors (instead of pop-front deques) to reduce allocations and improve cache efficiency.
* Added a unified “hard reset” path that fully resets CBF state (queues, indices, nextInput, held keys, etc.) when leaving a valid play state (pause/endscreen/minimize/death/editor/disable).
* Added dropped-input detection: if the ring buffer ever overflows, the mod automatically resets state and logs a warning instead of continuing with potentially desynced timing.
* Reworked step slicing math to use exact per-step `[start,end)` bounds (no `+1` fudge factor / modulo-based timing), improving determinism and clarity of substep timing.
* Updated keybind handling to an atomic, double-buffered `bitset<256>` mask system (no mutexes) for very fast key lookups.
* Added a one-time warning when keybind hashes exceed 255 (those binds are ignored with the current bitmask approach).
* Optimized Windows RAWINPUT handling: stack-buffer fast path, thread-local fallback only when needed, and early-out filtering for non-click mouse events to reduce processing cost on high polling devices.
* Improved key repeat filtering using a simple fixed-size “held key” array and reset-generation syncing to prevent stuck/duplicated states after resets.
* Simplified and strengthened the optional mouse-move queue trimming (WM_MOUSEMOVE / WM_NCMOUSEMOVE removal) to reduce input lag spikes on high polling mice.
* Added optional MMCSS “Games” scheduling for the input thread (with high MMCSS priority) for more stable capture under system load.
* Added an option to control Windows thread priority boost behavior for the input thread (toggleable).
* Reduced overhead while the mod is disabled by throttling the input thread and clearing pending messages/held states during soft-toggle.
* Unified Linux/Wine input ingestion to use the same bind-mask + ring-buffer pipeline as Windows for consistent behavior across platforms.
* Preserved core gameplay behavior and compatibility logic (CBF input injection model, buffered-input fallback behavior, physics bypass handling, watermarking, safe mode / leaderboard validity rules).

# What does it do

This mod allows your inputs to register in between visual frames, which drastically increases input precision on low framerates like 60FPS. \
It's similar to TPS Bypass or Draw Divide, but with much less lag and (hopefully) far fewer physics-related bugs.

If you like the mod, please consider [donating](https://www.paypal.com/donate/?hosted_button_id=U2LWN9H395TF8)!

# How to use

If the icon is automatically jumping when you respawn, disable the "Stop Triggers on Death" hack in Mega Hack.

To edit keybinds, go to the GD options menu and click the "Keys" button in the top right. \
To enable right click input, use the mod options menu. \
You must enable num lock to use numpad keys.

It is recommended to use either Physics Bypass or one of these FPS values: 60, 80, 120, or 240. \
This is because 2.2 has stutters on FPS values that aren't factors or multiples of 240 unless you enable Physics Bypass.

Disable TPS Bypass/Draw Divide when using this mod, because they're pointless.

The mod comes with its own version of Physics Bypass in the mod options (on Windows/Linux). Be warned that not all lists or leaderboards that allow CBF will consider this legit!

If on Linux, and the mod doesn't work, please try running the command <cr>sudo usermod -aG input $USER</c> (this will make your system slightly less secure).

# Known issues

- This mod does not work with bots
- Controller input is not yet supported

# Credits

Icon by alex/sincos.

Android, macOS, & iOS code based off [Click on Steps by zmx](https://github.com/qimiko/click-on-steps), used with permission. \
Android port by [mat](https://github.com/matcool), macOS & iOS ports port by [Jasmine](https://github.com/hiimjasmine00).
