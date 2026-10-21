// Verilator TB for bitstream_bit_feeder.
// Cases:
//   A) plain bytes → MSB-first bits
//   B) EPB 0x00 0x00 0x03 removed across burst gaps
//   C) NEGATIVE: naive keep-0x03 stream must NOT match
//   D) NEGATIVE: mid-symbol backpressure must not drop/duplicate bits
//   E) bit_valid held while consumer stalled before first take

#include "Vbitstream_bit_feeder_tb_top.h"
#include "verilated.h"

#include <cstdint>
#include <cstdio>
#include <vector>

static vluint64_t main_time = 0;
double sc_time_stamp() { return static_cast<double>(main_time); }

static void tick(Vbitstream_bit_feeder_tb_top* t) {
	t->clk = 0;
	t->eval();
	main_time++;
	t->clk = 1;
	t->eval();
	main_time++;
}

static void reset_dut(Vbitstream_bit_feeder_tb_top* t) {
	t->reset = 1;
	t->clear = 0;
	t->in_valid = 0;
	t->in_byte = 0;
	t->in_last = 0;
	t->bit_ready = 0;
	for (int i = 0; i < 4; ++i)
		tick(t);
	t->reset = 0;
	tick(t);
}

static int push_byte(Vbitstream_bit_feeder_tb_top* t, uint8_t b, bool last, int gap_cycles) {
	for (int g = 0; g < gap_cycles; ++g) {
		t->in_valid = 0;
		t->bit_ready = 0;
		tick(t);
	}
	int wait = 0;
	while (!t->in_ready) {
		t->in_valid = 0;
		tick(t);
		if (++wait > 10000)
			return -1;
	}
	t->in_valid = 1;
	t->in_byte = b;
	t->in_last = last ? 1 : 0;
	tick(t);
	t->in_valid = 0;
	t->in_last = 0;
	return 0;
}

static std::vector<int> drain_bits(Vbitstream_bit_feeder_tb_top* t, int max_bits, int stall_after,
                                   int stall_cycles, int* nal_last_count, int* bp_fail) {
	std::vector<int> bits;
	*nal_last_count = 0;
	*bp_fail = 0;
	int idle = 0;
	bool need_stall = (stall_after >= 0 && stall_cycles > 0);

	while (static_cast<int>(bits.size()) < max_bits && idle < 8000) {
		t->bit_ready = 0;
		tick(t);
		if (t->nal_bit_last)
			(*nal_last_count)++;

		if (!t->bit_valid) {
			idle++;
			continue;
		}
		idle = 0;

		const int bit = t->bit_value ? 1 : 0;
		bits.push_back(bit);

		t->bit_ready = 1;
		tick(t);
		if (t->nal_bit_last)
			(*nal_last_count)++;
		t->bit_ready = 0;

		if (need_stall && static_cast<int>(bits.size()) == stall_after) {
			need_stall = false;
			for (int s = 0; s < stall_cycles; ++s) {
				const int v0 = t->bit_valid;
				const int b0 = t->bit_value;
				tick(t);
				if (t->nal_bit_last)
					(*nal_last_count)++;
				if (v0) {
					if (!t->bit_valid) {
						std::fprintf(stderr, "FAIL backpressure dropped bit_valid\n");
						*bp_fail = 1;
						return {};
					}
					if (t->bit_value != b0) {
						std::fprintf(stderr, "FAIL backpressure changed bit_value\n");
						*bp_fail = 1;
						return {};
					}
				}
			}
		}
	}
	return bits;
}

static std::vector<int> bytes_to_msb_bits(const std::vector<uint8_t>& bytes) {
	std::vector<int> bits;
	for (uint8_t b : bytes) {
		for (int i = 7; i >= 0; --i)
			bits.push_back((b >> i) & 1);
	}
	return bits;
}

static bool vec_eq(const std::vector<int>& a, const std::vector<int>& b) {
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); ++i) {
		if (a[i] != b[i])
			return false;
	}
	return true;
}

int main(int argc, char** argv) {
	Verilated::commandArgs(argc, argv);
	auto* t = new Vbitstream_bit_feeder_tb_top;

	int fails = 0;
	std::printf("CASE EXECUTED bitstream_bit_feeder\n");

	reset_dut(t);
	if (push_byte(t, 0xA5, true, 0) != 0) {
		std::fprintf(stderr, "FAIL A push\n");
		return 2;
	}
	int nal_last = 0, bp_fail = 0;
	auto bits = drain_bits(t, 8, -1, 0, &nal_last, &bp_fail);
	auto exp = bytes_to_msb_bits({0xA5});
	if (!vec_eq(bits, exp)) {
		std::fprintf(stderr, "FAIL A plain bits size=%zu\n", bits.size());
		fails++;
	} else {
		std::printf("OK A plain_0xA5_msb_bits\n");
	}
	if (nal_last < 1) {
		std::fprintf(stderr, "FAIL A nal_bit_last missing\n");
		fails++;
	} else {
		std::printf("OK A nal_bit_last\n");
	}

	reset_dut(t);
	if (push_byte(t, 0x00, false, 0) != 0 || push_byte(t, 0x00, false, 3) != 0 ||
	    push_byte(t, 0x03, false, 3) != 0 || push_byte(t, 0x01, true, 3) != 0) {
		std::fprintf(stderr, "FAIL B push\n");
		return 2;
	}
	for (int i = 0; i < 16; ++i) {
		t->bit_ready = 0;
		tick(t);
	}
	if (t->epb_removed != 1) {
		std::fprintf(stderr, "FAIL B epb_removed want 1 got %u\n", t->epb_removed);
		fails++;
	} else {
		std::printf("OK B epb_removed_straddle_burst_gap\n");
	}
	if (t->rbsp_bytes != 3) {
		std::fprintf(stderr, "FAIL B rbsp_bytes want 3 got %u\n", t->rbsp_bytes);
		fails++;
	} else {
		std::printf("OK B rbsp_bytes=3\n");
	}
	nal_last = 0;
	bp_fail = 0;
	bits = drain_bits(t, 24, -1, 0, &nal_last, &bp_fail);
	exp = bytes_to_msb_bits({0x00, 0x00, 0x01});
	if (!vec_eq(bits, exp)) {
		std::fprintf(stderr, "FAIL B RBSP bits size=%zu\n", bits.size());
		fails++;
	} else {
		std::printf("OK B rbsp_bits_after_epb\n");
	}
	auto naive = bytes_to_msb_bits({0x00, 0x00, 0x03, 0x01});
	if (vec_eq(bits, naive)) {
		std::fprintf(stderr, "FAIL C NEGATIVE EPB not stripped\n");
		fails++;
	} else {
		std::printf("OK C NEGATIVE naive_keep_0x03_rejected\n");
	}

	reset_dut(t);
	if (push_byte(t, 0xF0, true, 0) != 0) {
		std::fprintf(stderr, "FAIL D push\n");
		return 2;
	}
	nal_last = 0;
	bp_fail = 0;
	bits = drain_bits(t, 8, 3, 20, &nal_last, &bp_fail);
	if (bp_fail) {
		fails++;
	} else if (!vec_eq(bits, bytes_to_msb_bits({0xF0}))) {
		std::fprintf(stderr, "FAIL D bits after stall mismatch size=%zu\n", bits.size());
		fails++;
	} else {
		std::printf("OK D mid_symbol_backpressure_no_drop_dup\n");
	}

	reset_dut(t);
	if (push_byte(t, 0x81, true, 0) != 0) {
		std::fprintf(stderr, "FAIL E push\n");
		return 2;
	}
	t->bit_ready = 0;
	for (int i = 0; i < 40; ++i)
		tick(t);
	if (!t->bit_valid) {
		std::fprintf(stderr, "FAIL E bit_valid not held while stalled\n");
		fails++;
	} else {
		std::printf("OK E bit_valid_while_consumer_stalled\n");
	}
	nal_last = 0;
	bp_fail = 0;
	bits = drain_bits(t, 8, -1, 0, &nal_last, &bp_fail);
	if (!vec_eq(bits, bytes_to_msb_bits({0x81}))) {
		std::fprintf(stderr, "FAIL E delayed drain mismatch\n");
		fails++;
	} else {
		std::printf("OK E delayed_drain_intact\n");
	}

	if (fails) {
		std::printf("FAIL bitstream_bit_feeder %d fails\n", fails);
		delete t;
		return 1;
	}
	std::printf("PASS bitstream_bit_feeder all cases\n");
	delete t;
	return 0;
}
