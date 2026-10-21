# Playback input mailbox

MiSTer `Main` grabs `/dev/input/event*`, so `misterplexd` must not read evdev or touch the HPS↔FPGA SPI bus for playback controls. The core receives keyboard/controller events through `hps_io`, edge-decodes them, and publishes commands in DDR.

## Controls

- Keyboard: `Space` = Play/Pause, `Esc` = Stop, `Right Arrow` = Skip Fwd, `Left Arrow` = Skip Back.
- Controller: `J1,Play/Pause,Stop,Skip Fwd,Skip Back;` maps to `joystick_0[4]..[7]`.

## DDR word

Address: the build's PLX page + `0x108` (`mailbox_abi::kPlxiOff`), one 64-bit little-endian word, magic `0x504C5849` (`PLXI`). The page comes from the build's `fpga_build_ids.hpp` row (`plx_page.hpp`): `0x300FF000` on every recognised build, so `0x300FF108`. The old absolute `0x3007F108` is a dead page on all of them. `tools/input_mailbox_probe --build 0xID` reads it on the row's page.

```
[31:0]  magic
[39:32] cmd       1=PlayPause, 2=Stop, 3=SkipForward, 4=SkipBack
[47:40] cmd_seq   increments for every command, including repeated same-key presses
[63:48] seq       increments for every mailbox publish; use for torn/stale rejection
```

Read twice and accept only if both reads match, `magic == PLXI`, and `seq` or `cmd_seq` advanced since the last accepted command.

The ARM daemon treats the first valid word after startup as a baseline when DDR
already contains `PLXI`, so it does not replay a stale value from a previous core
load. If it first sees no valid magic, the next stable valid word is accepted as
the first live command. A later dispatch requires stable reads plus advanced
`seq` and `cmd_seq`; repeated identical commands are valid because `cmd_seq`
changes each time. Defaults: skip forward = 30000 ms, skip back = 10000 ms
(`SKIP_FORWARD_MS` / `SKIP_BACK_MS`; `SKIP_MS` sets both).
