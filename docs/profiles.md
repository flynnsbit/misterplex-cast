# Stream profiles

## MiSTerPlex-1 (Phase 3 target)

| Field | Value |
|-------|--------|
| Video codec | H.264 Baseline or Main |
| Resolution | ≤ 720p30 or 480p60 (raise after timing) |
| Scan | Progressive preferred |
| Audio | AAC-LC stereo or PCM s16le |
| Container | MP4 / fMP4 or elementary after ARM demux |
| Bitrate | Soft cap; PMS ladder on direct-play fail |

Exact limits are validated on hardware (CPU idle during play, underrun≈0, sync ≤±40 ms).

## PMS weak-client mapping

When direct play exceeds the profile, request a transcode matching MiSTerPlex-1 (same spirit as mistercast-linux weak-client auto).

## Phase 1

No compressed video — internal color bars only.
