# misterplex-cacert.pem (tools/gcast/ca/)
Mozilla CA root set as published by curl (https://curl.se/ca/cacert.pem), fetched 2026-10-18 01:21 CT, 121 roots.
Install next to the daemon as /media/fat/linux/misterplex-cacert.pem. misterplexd's YouTube receiver (GYT=1) uses it
for www.youtube.com Lounge HTTPS. Override with MPX_CA_BUNDLE=/path. The MiSTer image ships no CA bundle
(/etc/ssl/certs/ca-certificates.crt is absent on a lab MiSTer), and verification is never turned off: no bundle = YouTube off.
License: MPL 2.0 (Mozilla certdata). Refresh when curl publishes a new bundle.
