# fpp-plugin-tplink
FPP TP-Link(Kasa) Plugin

Add Plugin URL to FPP
https://raw.githubusercontent.com/computergeek1507/fpp-plugin-tplink/main/pluginInfo.json

## Switching plugs from a sequence

Give a plug a start channel and the sequence switches it on and off by crossing half; it only acts on a change; it never turns off a plug the sequence has not turned on.

In more detail, for TPLink Switch/Plug, Tasmota Switch and Tapo Switch entries:

- A channel value of 127 or more means on. Below 127 means off.
- The plugin sends a command only when the channel crosses half. Holding the channel steady sends nothing more.
- Until the sequence has turned a plug's channel on, a channel below half leaves the plug alone. A plug switched on by a command, a playlist lead-in or Home Assistant stays on while a sequence leaves its channel at 0.
- Once the sequence has turned a plug on, dropping the channel below half turns it off. The plugin remembers this until fppd restarts; starting another sequence does not reset it.
- Each plug has one sender, so the sequence's commands to a plug never overlap and always arrive in order. If a plug does not answer, the plugin retries the latest state for up to 2 minutes, waiting longer each time (up to 8 seconds between tries), then gives up and says so in the log. So a plug that was offline when its channel changed never switches itself on or off when it comes back later; the next change on its channel tries again. A newer state from the sequence replaces the one being retried, is sent straight away, and gets 2 minutes of its own.
- A start channel of 0 means sequences never touch the plug.

The commands (TPLink Set Switch, TPLink Toggle Switch, TPLink All Switches On, Off and Toggle) work as before.

## Tests

The flip rule, the per-plug sender and the Kasa protocol code live in `src/core` and build without FPP. `make -C tests` builds and runs their unit tests on macOS or Linux, against a fake plug on 127.0.0.1.
