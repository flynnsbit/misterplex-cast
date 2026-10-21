#pragma once
// avformat_open_input options shared by the OB demuxer and the in-process
// decoder. No libav headers: unit tests include this file on the host.
//
// protocol_whitelist keeps a remote open on file/http/https/tls/tcp/crypto.
// tls_verify is on. ca_file is the shipped Mozilla bundle when one is present.
// The FFmpeg 8.1.2 OpenSSL backend also calls SSL_set1_host for a non-numeric
// host when tls_verify is on, so the certificate name is checked with the
// chain. A numeric host skips that name check.
// Plain http stays in the whitelist so a LAN Plex server still plays.

namespace misterplex {

inline constexpr char kAvProtocolWhitelist[] = "file,http,https,tls,tcp,crypto";

}  // namespace misterplex
