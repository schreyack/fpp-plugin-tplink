# fpp-plugin-tplink
FPP TP-Link(Kasa) Plugin

Add Plugin URL to FPP
https://raw.githubusercontent.com/computergeek1507/fpp-plugin-tplink/main/pluginInfo.json

## Switching plugs from a sequence

A start channel means the sequence owns the plug. When a playlist starts, the plug is sent the channel's state on the very first frame (so a channel at 0 turns it off right away, not 30 seconds in). After that the plugin sends a command only when the on/off state changes.

In more detail, for TPLink Switch/Plug, Tasmota Switch and Tapo Switch entries:

- A channel value of 127 or more means on. Below 127 means off.
- The plugin sends a command only when the on/off state changes. Holding the channel steady sends nothing more.
- When a playlist starts, the first frame always sends, so a channel at 0 turns the plug off immediately. After that first send, only changes are acted on.
- Each plug has one sender, so the sequence's commands to a plug never overlap and always arrive in order. If a plug does not answer, the plugin retries the latest state for up to 2 minutes, waiting longer each time (up to 8 seconds between tries), then gives up and says so in the log. A newer state from the sequence replaces the one being retried, is sent straight away, and gets 2 minutes of its own.
- Leave the start channel at 0 for plugs you control with commands (playlist lead-ins, Home Assistant).

## Devices go by name

Every device in the config has a name ("LED Roof"). Commands and playlists pick a device by that name, never by its IP: TPLink Set Switch, Toggle Switch, Set Light RGB, HSV and Off each take a device, chosen from a list of the configured switches or lights (a comma list of names also works). Names ignore case but must be unique. The IP, plug number and device type live only in the config. A name the config does not have, or an IP, does nothing and says so in the log.

The lists behind the dropdowns are `GET /api/plugin-apis/TPLink/switches` and `/lights`. The All Switches and All Lights commands, and the effects hold (`/api/plugin-apis/TPLink/effects-hold`), are unchanged.

DMXLive Studio writes `plugin.tplink.json`, names included. The setup page keeps every field Studio writes when it saves.

`tools/migrate_playlists_to_names.py --host <fpp>` rewrites playlists saved with the old IP form to names. It changes nothing without `--apply --backup-dir <dir>`.

## Tests

The flip rule, the per-plug sender, the device names and the Kasa protocol code live in `src/core` and build without FPP. `make -C tests` builds and runs their unit tests on macOS or Linux, against a fake plug on 127.0.0.1.
