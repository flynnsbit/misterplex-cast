#!/usr/bin/env bash
# GCAST P6 host leak check of YtFrontend on lab-pc: ASan/LSan harness + N real-Lounge sender cycles.
set -u
cd ~/gcast && rm -rf ytsrc2 && mkdir ytsrc2 && tar xzf ytsrc2.tgz -C ytsrc2 && cd ytsrc2
g++ -std=c++17 -O1 -g -fsanitize=address -fno-omit-frame-pointer -Iarm/misterplexd -Ihost tests/gcast/yt_fe_harness.cpp \
  arm/misterplexd/gcast/yt_fe.cpp arm/misterplexd/gcast/https_client.cpp arm/misterplexd/wall_clock.cpp -lssl -lcrypto -pthread -o yt_fe_harness_asan || exit 1
g++ -std=c++17 -O2 -Iarm/misterplexd -Ihost tests/gcast/yt_fe_harness.cpp \
  arm/misterplexd/gcast/yt_fe.cpp arm/misterplexd/gcast/https_client.cpp arm/misterplexd/wall_clock.cpp -lssl -lcrypto -pthread -o yt_fe_harness_plain || exit 1
for B in asan plain; do
P=8013; OUT=~/gcast/proof/ytleak-$B; mkdir -p $OUT; : > $OUT/rss.txt
ASAN_OPTIONS=detect_leaks=1:log_path=$OUT/asan ./yt_fe_harness_$B LeakCheck $P ~/gcast/ytsrc/tools/gcast/ca/misterplex-cacert.pem /tmp/ytleak_screen_id > $OUT/harness.log 2>&1 &
H=$!; sleep 6
for c in $(seq 1 ${N:-10}); do
  DIAL_PORT=$P NO_SSDP=1 ~/gcast/venv312/bin/python tools/gcast/yt_lounge_sender.py 127.0.0.1 aqz-KE-bpKQ wait:6 pause wait:2 play wait:2 seek:30 wait:3 stop > $OUT/s$c.log 2>&1
  echo "c$c rss_kb=$(awk '/VmRSS/{print $2}' /proc/$H/status) fds=$(command ls /proc/$H/fd | wc -l) thr=$(command ls /proc/$H/task | wc -l)" >> $OUT/rss.txt
done
kill -INT $H; wait $H; echo "harness exit $?" >> $OUT/rss.txt
command ls $OUT
done
