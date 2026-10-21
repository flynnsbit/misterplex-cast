#!/usr/bin/env bash
# Retired: F1 frame presentation is DDR YUV420p-only.
set -euo pipefail
echo "test_fpga_push: retired — non-YUV SPI F1 frame pushes are refused."
echo "Use tests/hw/test_ddr_frame.sh or push_frame --ddr --yuv420p WxH frame.yuv420p."
exit 2
