# OBS Recorder 1.0.0

Automatically records selected Guild Wars explorable areas through OBS Studio. OBS Recorder starts when the map is ready, stops when the character leaves, and renames the completed file to:

`Area__Character__YYYY-MM-DD_HH-MM-SS.ext`

The default areas are:

- Deep (307)
- Urgoz (266)
- UW (72)
- FoW (34)
- DoA (474)

Each area can be enabled independently. Additional map IDs can be entered manually or added from the current map.

## OBS setup

1. Create an OBS scene with a Game Capture source for Guild Wars.
2. Configure the recording path, encoder, quality, audio, and format in OBS.
3. Open **Tools > WebSocket Server Settings** and enable the server.
4. Copy `ObsRecorder.dll` into the GWToolbox `plugins` folder and load it.
5. Enter the WebSocket host, port, and password in the plugin settings, then reload the plugin.

The default host and port are `127.0.0.1:4455`. OBS Studio must remain open. A compact `REC` indicator is displayed while recording and can be disabled in the settings.

## Notes

- OBS controls the output directory and file format.
- The plugin never deletes recordings.
- If a renamed file already exists, a numeric suffix is appended.
- If renaming fails, the original OBS recording is preserved and the error appears in the plugin status.
- The WebSocket password is stored locally in the plugin settings file.

Third-party GWToolbox plugins are unsupported and may violate the Guild Wars Terms of Service. Use at your own risk.
