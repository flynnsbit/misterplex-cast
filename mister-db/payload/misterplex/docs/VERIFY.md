# Diagram notes (private RC 1024)

The picture is `docs/misterplex-architecture.svg` (and the PNG next to it). The install and the open issues are in `README.md`. This file is not a second install guide.

- AirPlay works on the MiSTer by itself. The on-card helper listens on 127.0.0.1:18781. A helper on a computer is optional.
- PlexOB plays 480i, 480p, and native video up to 720p. 240p is not in this release.
- Send Home from the on-screen program returns to the Plex home screen. That, and the on-card AirPlay helper, have not been confirmed on a television yet.
- No SDRAM board is required. PlexOB uses DDRAM only.

Older notes in this `docs/` folder describe earlier packages. They are not this install.
