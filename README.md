# quickrec

Tiny C X11 screen-region recorder built around a custom selector plus `ffmpeg`.

## What it does

- uses a custom X11 selector for click-to-select-window or click-drag region selection
- uses a `#fe6b6b` 1px selection outline
- keeps a `#fe6b6b` 1px bounding box on screen while recording
- starts `ffmpeg` with `x11grab` on that region
- stores recorder state in `~/.local/state/quickrec/`
- lets you `start`, `stop`, `toggle`, and `status`

## Dependencies

- `ffmpeg`
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
```

By default recordings are saved to:

```sh
~/Workspace/quickrec/recordings/quickrec-YYYY-MM-DD_HH-MM-SS.mp4
```

You can override the default output directory with:

```sh
export QUICKREC_OUTPUT_DIR=/some/other/dir
```

## Notes

- selected regions are automatically nudged to even dimensions for H.264/yuv420p
- current codec settings are intentionally simple: `libx264`, `ultrafast`, `yuv420p`
- this is video-only for now
- ffmpeg logs go to `~/.local/state/quickrec/ffmpeg.log`

## License

MIT
