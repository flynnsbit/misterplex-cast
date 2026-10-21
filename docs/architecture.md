# Architecture

MiSTerPlex on this release plays through the PlexOB core. The ARM program fetches the stream, and the FPGA decodes H.264.

![How MiSTerPlex fits together](architecture.png)

The drawing is `docs/architecture.svg`. How each box was checked is `docs/VERIFY.md`. Regenerate the drawing with `python3 tools/gen_arch.py`, then `rsvg-convert -w 2400 docs/architecture.svg -o docs/architecture.png`.

Older notes that describe an ARM RGB framebuffer, a display-resolution matrix, or a CRT/LCD mode table are not the PlexOB path.
