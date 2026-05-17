# quickrec

Tiny C X11 screen-region recorder built around a custom selector plus `gpu-screen-recorder`.

## What it does

- uses a custom X11 selector for click-to-select-window or click-drag region selection
- uses a `#fe6b6b` 1px selection outline
- keeps a `#fe6b6b` 1px bounding box on screen while recording
- starts `gpu-screen-recorder` on that region
- stores recorder state in `~/.local/state/quickrec/`
- lets you `start`, `stop`, `toggle`, and `status`

## Dependencies

- `gpu-screen-recorder`
- `dmenu` (for naming the finished recording when you stop)
- X11 (`DISPLAY` must be set)

## Build

```sh
make
```

## Install

```sh
make install
```

## Usage

```sh
quickrec                               # toggle
quickrec start                         # select a region and start recording
quickrec start -g 1280x720+0+0         # non-interactive start
quickrec stop                          # stop the current recording
quickrec status                        # see whether it is recording
quickrec start -f 30
quickrec start -o recordings/test.mp4
quickrec start --no-audio
quickrec start --mic-source default_input --system-source default_output

# while recording, Ctrl+Shift+Esc triggers the normal stop flow
```

By default recordings are saved to:

```sh
~/Desktop/Videos/quickrec-YYYY-MM-DD_HH-MM-SS.mp4
```

You can override the default output directory with:

```sh
export QUICKREC_OUTPUT_DIR=/some/other/dir
```

## Notes

- selected regions are automatically nudged to even dimensions for H.264
- current codec settings are intentionally simple: `h264`, `very_high`, `aac`, `mp4`
- audio defaults to `default_input|default_output` for mic + system audio
- after recording, quickrec sanitizes gpu-screen-recorder's MP4 track-name metadata in-place to avoid FFmpeg `UDTA` warnings
- gpu-screen-recorder logs go to `~/.local/state/quickrec/gpu-screen-recorder.log`

## License

MIT
