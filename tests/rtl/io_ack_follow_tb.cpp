// Verilator TB: product ACK latency is clk_sys-scale (≤2), not milliseconds.
// RED twin: FAULT_STUCK_WAIT never ACKs.
#include "Vio_ack_follow_tb_top.h"
#include "verilated.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>

static void tick(Vio_ack_follow_tb_top* top) {
  top->clk_sys = 0;
  top->eval();
  top->clk_sys = 1;
  top->eval();
}

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  auto* top = new Vio_ack_follow_tb_top;

  // Product contract bound: ≤2 clk_sys with wait=0 (sys_top rack→ack pipeline).
  // At 50 MHz, 2 cycles = 40 ns. 2 ms wall = 100_000 cycles @50 MHz — forbidden.
  constexpr int kMaxProductAckCycles = 2;
  constexpr int kMsWallCyclesAt50MHz = 100000; // 2 ms * 50e6

  top->reset = 1;
  top->io_clk = 0;
  top->io_wait = 0;
  top->vs_wait = 0;
  for (int i = 0; i < 4; i++) tick(top);
  top->reset = 0;
  for (int i = 0; i < 2; i++) tick(top);

  // Raise HPS io_clk and hold (typical SPI bit phase).
  top->io_clk = 1;
  int cycles = 0;
  int ack_at = -1;
  for (; cycles < 32; cycles++) {
    tick(top);
    if (top->io_ack_good && ack_at < 0)
      ack_at = cycles + 1; // after this tick
  }

  const int seen = top->seen_good;
  const int lat = top->lat_good;
  const int fault_seen = top->seen_fault;
  const int fault_ack = top->io_ack_fault;

  std::printf("CASE EXECUTED io_ack_follow\n");
  std::printf("measured_ack_at_cycle=%d\n", ack_at);
  std::printf("measured_lat_good=%d\n", lat);
  std::printf("measured_seen_good=%d\n", seen);
  std::printf("measured_seen_fault=%d\n", fault_seen);
  std::printf("measured_ack_fault=%d\n", fault_ack);
  std::printf("bound_max_product_ack_cycles=%d\n", kMaxProductAckCycles);
  std::printf("bound_2ms_wall_cycles_at_50mhz=%d\n", kMsWallCyclesAt50MHz);
  std::printf("ratio_2ms_over_product=%d\n",
              kMsWallCyclesAt50MHz / (kMaxProductAckCycles > 0 ? kMaxProductAckCycles : 1));

  int fail = 0;
  if (!seen || lat <= 0) {
    std::printf("FAIL product path never ACKed seen=%d lat=%d\n", seen, lat);
    fail = 1;
  }
  if (lat > kMaxProductAckCycles) {
    std::printf("FAIL product ack_latency_cycles=%d > %d (not clk-scale)\n",
                lat, kMaxProductAckCycles);
    fail = 1;
  } else {
    std::printf("OK product_ack_clk_scale lat=%d\n", lat);
  }
  // Explicit L30 negative: product latency must be orders below 2 ms wall.
  if (lat > 0 && (kMsWallCyclesAt50MHz / lat) < 1000) {
    std::printf("FAIL 2ms wall not >> product latency (ratio too small)\n");
    fail = 1;
  } else {
    std::printf("OK 2ms_wall_is_not_product_ack_scale ratio>=1000\n");
  }
  // RED twin must not ACK
  if (fault_seen || fault_ack) {
    std::printf("FAIL FAULT_STUCK_WAIT twin ACKed (seen=%d ack=%d)\n",
                fault_seen, fault_ack);
    fail = 1;
  } else {
    std::printf("OK fault_twin_never_acks\n");
  }

  if (fail) {
    std::printf("FAIL io_ack_follow_tb\n");
    std::printf("true rc=1\n");
    delete top;
    return 1;
  }
  std::printf("PASS io_ack_follow_tb\n");
  std::printf("true rc=0\n");
  delete top;
  return 0;
}
