# MiSTerPlex rc5 (pre-release)

> This is a release candidate for testing, not a final release. Back up your SD card first. The start-at-boot file is still named `misterplexd-boot-rc3.sh`, so an existing boot line keeps working.

The PlexOB core and the MiSTerPlex program in this release have to be installed together. Follow the step-by-step install in `README.md`. Use the top-level `SHA256SUMS` to check your download. When upgrading, copy the `linux/gcast` folder too (step 4 in `README.md`), because that is where this release's AirPlay change lives.

**Keep your settings file.** If `/media/fat/linux/misterplex.conf` already exists, back it up and keep it. Step 5 in `README.md` only installs the example when there is no settings file yet. Never copy `misterplex.conf.example` over your own file, because that replaces your server address, token and device name. Reboot once after upgrading.

## What's new

- **AirPlay video plays at full speed.** When a web video, a web stream or an mp4 is already in a format the MiSTer can show (H.264 up to 720p and 30 frames per second), the on-card helper now passes it straight through instead of re-encoding it on the card. These play smoothly at about 30 frames per second, where rc4 managed only 2 to 3, and the processor is no longer maxed out.
- **AirPlay picks the right quality from a web stream.** When a stream offers several qualities, the helper picks the best one the MiSTer can pass straight through. Streams that redirect to another address still play.
- **AirPlay audio no longer drops sound.** In testing, no audio was lost (rc4 lost about a quarter), and the music screen uses less of the processor.
- **The last video frame no longer stays up for the whole song.** After an AirPlay video, AirPlay audio now switches to the music screen (see Known issues for the short delay).
- **Stopping YouTube from the phone should work more often.** The YouTube connection reconnects by itself when it goes quiet, so the phone's stop usually gets through. In testing the stop arrived 3 times out of 3, but the first stop after a cold boot can still be lost (see Known issues). If it ever gets stuck, stop from Plex and restart the program.

AirPlay was tested with lab senders only, not a real iPhone.

## Known issues

- **AirPlay was tested with lab senders only, not a real iPhone.** No real iPhone, iPad or Mac has been tried with rc5 yet.
- **The first YouTube stop after a cold boot can be lost.** Right after the MiSTer is switched on, the first stop from the YouTube app may not get through. Press stop again, or stop from Plex.
- **Some web streams can't be played over AirPlay.** Streams that carry a certain kind of broadcast information table make the on-card helper crash, so the cast is refused. Apple's own streams and most web video are fine.
- **After an AirPlay video, AirPlay audio shows the frozen last video frame for about 10 seconds** before the music screen appears. The sound plays normally during that time.
- **Video the MiSTer can't pass straight through is still very slow (about 2 to 3 frames per second).** This covers things like 1080p, 60 frames per second or 10-bit video. These are re-encoded on the card, which uses all of the processor. A helper on a computer (`GCAST_XCODE`) avoids this.
- **Some streams' sound gets re-encoded.** If a stream's sound isn't in the usual AAC stereo format, the helper re-encodes the sound while passing the video straight through. The picture still plays at full speed, but the processor runs flat out.
- **An AirPlay video can keep playing if the phone drops off without stopping it,** and AirPlay audio started after that doesn't take over the screen. Stopping from Plex recovers it.
- **After seeking in YouTube, the ready screen sometimes shows for a few seconds** before the video comes back. This was seen once in two tries.
- **The ready screen may briefly drop YouTube** if the YouTube connection is refused and has to start over.
- **AirPlay appears on the ready screen a few seconds after start-up,** once the on-card helper passes its self-test.
- **If a computer AirPlay helper stops,** the ready screen keeps showing AirPlay until it next redraws.
- **Casting from a shared Plex server addressed by its local network address can fail.** Casting through plex.tv works. Browsing a shared server on the TV is untested.
- **The progress bar in the Plex app** may not move while you cast, even though playback is fine.
- **S-Video and composite** haven't been tested on a real TV yet.
- **240p output** isn't available yet.
- **Subtitles aren't supported** on the PlexOB core yet, and you can't change audio or subtitle tracks from your phone.

## Source

MiSTerPlex is free software under the GNU General Public License, version 3 or (at your option) any later version (SPDX: `GPL-3.0-or-later`). The complete source for this release, including the on-card AirPlay helper's FFmpeg, x264 and OpenSSL source and build scripts, is published at https://github.com/flynnsbit/misterplex-cast/releases/tag/rc5. See `licenses/README.md` and `licenses/gcast-ffmpeg/NOTICE`.

## Thanks

MiSTerPlex stands on the work of MiSTer FPGA, owenb321's MiSTer_h264_decoder, UxPlay and RPiPlay, FFmpeg, x264, OpenSSL, yt-dlp, libplist, llhttp, csrp, mdnsd, stb, QOI, Roboto and Spleen. The full credits and licences are in the README and in `licenses/`.
