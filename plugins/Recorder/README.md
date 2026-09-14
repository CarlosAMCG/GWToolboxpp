# Recorder 1.1.0 Preview

Recorder automatically captures selected Guild Wars explorable areas. Recording starts when the map is ready, stops when the character leaves, and produces an MP4 named:

`Area__Character__YYYY-MM-DD_HH-MM-SS.mp4`

The default areas are:

- Deep (307)
- Urgoz (266)
- UW (72)
- FoW (34)
- DoA (474)

Each area can be enabled independently. Additional map IDs can be entered manually or added from the current map.

## Video and audio

Recorder captures the final Guild Wars DirectX 9 frame, including GWToolbox windows and widgets, and uses Windows Media Foundation to create an H.264 MP4. The default settings are 30 FPS and 8 Mbps. Files are stored in `Videos\GWToolbox Recordings` unless another directory is configured.

The default Windows output device (headphones or speakers) and default microphone can be mixed into one AAC stereo track. Each source can be disabled or given a separate volume.

## Notes

- Use `Start recording` in settings to record manually, independently of the automatic area filter. Manual recording ends when you stop it or leave the map.
- Enable `Show in main window` to add Recorder to the main menu. Its entry opens a compact control window with automatic recording toggle, recording mode, status, and Start/Stop controls.
- A compact `REC` indicator is displayed while recording and can be disabled.
- Drag the `REC` text to move the indicator. Press `Stop` beside it (or `Stop recording` in settings) to finish early without restarting automatically in the same visit to the area.
- When a recording finishes, the plugin asks whether to save or discard it. This confirmation can be disabled to save automatically.
- The plugin warns in chat when a recording exceeds the configured duration.
- The plugin never deletes recordings.
- If a filename already exists, a numeric suffix is appended.
- DirectX 9 readback can affect game performance, so test it before relying on it for long runs.

Third-party GWToolbox plugins are unsupported and may violate the Guild Wars Terms of Service. Use at your own risk.
