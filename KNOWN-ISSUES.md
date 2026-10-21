# Known issues in rc5

rc5 is a release candidate. These problems are known and still open.

## AirPlay

- **Some web streams can't be played over AirPlay.** Streams that carry a certain kind of broadcast information table make the on-card helper crash, so the cast is refused. Apple's own streams and most web video are fine.
- **After an AirPlay video, AirPlay audio shows the frozen last video frame for about 10 seconds** before the music screen appears. The sound plays normally during that time.
- **Video the MiSTer can't pass straight through is still very slow** (about 2 to 3 frames per second). This covers things like 1080p, 60 frames per second or 10-bit video. These are re-encoded on the card, which uses all of the processor. A helper on a computer (`GCAST_XCODE`) avoids this.
- **Some streams' sound gets re-encoded.** If a stream's sound isn't in the usual AAC stereo format, the helper re-encodes the sound while passing the video straight through. The picture still plays at full speed, but the processor runs flat out.
- **An AirPlay video can keep playing if the phone drops off without stopping it,** and AirPlay audio started after that doesn't take over the screen. Stopping from Plex recovers it.
- **AirPlay appears on the ready screen a few seconds after start-up,** once the on-card helper passes its self-test.
- **If a computer AirPlay helper stops,** the ready screen keeps showing AirPlay until it next redraws.
- **AirPlay was tested with lab senders only, not a real iPhone.** The lab senders behave like an iPhone, but no real iPhone, iPad or Mac has been tried with rc5 yet.

## YouTube

- **The first YouTube stop after a cold boot can be lost.** Right after the MiSTer is switched on, the first time you press stop in the YouTube app the video may keep playing. Press stop again, or stop from Plex. Later stops got through in testing (3 times out of 3).
- **After seeking in YouTube, the ready screen sometimes shows for a few seconds** before the video comes back. This was seen once in two tries.
- **The ready screen may briefly drop YouTube** if the YouTube connection is refused and has to start over.

## Plex

- **Casting from a shared Plex server addressed by its local network address can fail.** Casting through plex.tv works. Browsing a shared server on the TV is untested.
- **The progress bar in the Plex app** may not move while you cast, even though playback is fine.

## Video output

- **S-Video and composite** haven't been tested on a real TV yet.
- **240p output** isn't available yet.
- **Subtitles aren't supported** on the PlexOB core yet, and you can't change audio or subtitle tracks from your phone.

## For developers

- **A test fixture is missing from this repository:** `tests/fixtures/p3_frame_planes/plex_inter_p16_624x480_12f_i420.yuv` (about 5.4 MB). `tests/unit/test_h264_frame_plane_goldens.sh` and `tests/unit/test_stream_path_full_frame_compare.sh` fail on a fresh clone until it is added. Its expected planes are in `plex_inter_p16_624x480_12f_frame_planes_v1.json` next to it.
- A few unit tests fail on this tree for reasons that predate rc5 (a timing-sensitive test, an RTL invariant check, and a lock-path test that can hang). They are tracked for the next release.

## Security notes (for developers)

None of these is visible in normal use, but you should know about them:

- **Plex requests don't check certificates.** The program fetches from Plex with `curl -k` (`arm/misterplexd/plex_resolve.cpp`, `httpGet`) and the on-screen library does the same (`host/libmisterplex/browse_fetch.hpp`). This was done for `plex.direct` addresses. Someone on your network could pretend to be your Plex server.
- **Numeric addresses get no TLS name check.** When a secure connection goes to an IP address instead of a name, the certificate chain is checked but the name isn't (`arm/misterplexd/wall_clock.cpp`, `arm/misterplexd/gcast/https_client.cpp`).
- **The certificate list lacks the newest Let's Encrypt roots.** The bundled Mozilla CA file (`linux/misterplex-cacert.pem`) doesn't include ISRG Root YE or ISRG Root YR yet. Sites that move to those roots will fail until the bundle is updated.
- **The clock fix trusts the network.** If the MiSTer boots with a wrong date, the program asks `pool.ntp.org` or `time.google.com` over plain SNTP, then reads the Date header of an HTTPS reply. SNTP isn't authenticated, so a device on your network could set a wrong time, which can make certificate checks fail.
