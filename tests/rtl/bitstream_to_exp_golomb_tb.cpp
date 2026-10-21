// Feeder → h264_exp_golomb_reader integration (w-path).
// A) plain ue sequence
// B) EPB 0x000003 removed; ue sequence still matches (burst gaps)
// C) NEGATIVE: naive keep-0x03 bit string ≠ stripped; DUT matches stripped
// D) mid-symbol stall — resume yields correct ue
// E) skid backpressure: in_ready drops when full and bits not taken

#include "Vbitstream_to_exp_golomb_tb_top.h"
#include "verilated.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

static vluint64_t main_time = 0;
double sc_time_stamp() { return static_cast<double>(main_time); }

static void tick(Vbitstream_to_exp_golomb_tb_top* t) {
	t->clk = 0;
	t->eval();
	main_time++;
	t->clk = 1;
	t->eval();
	main_time++;
}

static void reset_dut(Vbitstream_to_exp_golomb_tb_top* t) {
	t->reset = 1;
	t->clear = 0;
	t->in_valid = 0;
	t->in_byte = 0;
	t->in_last = 0;
	t->use_eg_ready = 0;
	t->raw_bit_ready = 0;
	t->eg_start = 0;
	t->eg_signed_mode = 0;
	for (int i = 0; i < 4; ++i)
		tick(t);
	t->reset = 0;
	tick(t);
}

static std::string ue_bits(uint32_t code) {
	uint32_t x = code + 1;
	int lz = 0;
	for (uint32_t tt = x; tt > 1; tt >>= 1)
		lz++;
	std::string s(static_cast<size_t>(lz), '0');
	for (int i = lz; i >= 0; --i)
		s.push_back(((x >> i) & 1) ? '1' : '0');
	return s;
}

static void pack_msb(const std::string& bits, std::vector<uint8_t>* out) {
	uint8_t cur = 0;
	int n = 0;
	for (char c : bits) {
		cur = static_cast<uint8_t>((cur << 1) | (c == '1' ? 1 : 0));
		if (++n == 8) {
			out->push_back(cur);
			cur = 0;
			n = 0;
		}
	}
	if (n) {
		cur = static_cast<uint8_t>(cur << (8 - n));
		out->push_back(cur);
	}
}

static std::string bits_of(const std::vector<uint8_t>& b) {
	std::string s;
	for (uint8_t x : b)
		for (int i = 7; i >= 0; --i)
			s.push_back(((x >> i) & 1) ? '1' : '0');
	return s;
}

static std::vector<uint8_t> strip_epb(const std::vector<uint8_t>& in) {
	std::vector<uint8_t> o;
	int z = 0;
	for (uint8_t b : in) {
		if (z == 2 && b == 0x03) {
			z = 0;
			continue;
		}
		o.push_back(b);
		z = (b == 0x00) ? z + 1 : 0;
	}
	return o;
}

static int push_byte(Vbitstream_to_exp_golomb_tb_top* t, uint8_t b, bool last, int gap) {
	for (int g = 0; g < gap; ++g) {
		t->in_valid = 0;
		tick(t);
	}
	int w = 0;
	while (!t->in_ready) {
		t->in_valid = 0;
		tick(t);
		if (++w > 20000)
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

static int push_all(Vbitstream_to_exp_golomb_tb_top* t, const std::vector<uint8_t>& bytes, int gap) {
	for (size_t i = 0; i < bytes.size(); ++i) {
		if (push_byte(t, bytes[i], i + 1 == bytes.size(), gap) != 0)
			return -1;
	}
	return 0;
}

static int read_ue(Vbitstream_to_exp_golomb_tb_top* t, uint32_t* ue_out, int max_cyc) {
	t->use_eg_ready = 1;
	t->raw_bit_ready = 0;
	t->eg_signed_mode = 0;
	t->eg_start = 1;
	tick(t);
	t->eg_start = 0;
	for (int i = 0; i < max_cyc; ++i) {
		tick(t);
		if (t->eg_done) {
			if (!t->eg_ok)
				return -2;
			*ue_out = static_cast<uint32_t>(t->eg_ue_value);
			for (int k = 0; k < 2; ++k)
				tick(t);
			return 0;
		}
	}
	return -1;
}

static int expect_ues(Vbitstream_to_exp_golomb_tb_top* t, const std::vector<uint32_t>& want,
                      const char* tag) {
	for (size_t i = 0; i < want.size(); ++i) {
		uint32_t got = 0xFFFFFFFFu;
		int rc = read_ue(t, &got, 8000);
		if (rc != 0) {
			std::printf("FAIL %s: read_ue[%zu] rc=%d\n", tag, i, rc);
			return 1;
		}
		if (got != want[i]) {
			std::printf("FAIL %s: ue[%zu]=%u want=%u\n", tag, i, got, want[i]);
			return 1;
		}
	}
	std::printf("OK %s (%zu ues)\n", tag, want.size());
	return 0;
}

int main(int argc, char** argv) {
	Verilated::commandArgs(argc, argv);
	auto* t = new Vbitstream_to_exp_golomb_tb_top;
	int fails = 0;

	std::printf("CASE EXECUTED bitstream_to_exp_golomb\n");

	// A plain
	{
		reset_dut(t);
		std::vector<uint32_t> want = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
		std::string bits;
		for (uint32_t v : want)
			bits += ue_bits(v);
		std::vector<uint8_t> bytes;
		pack_msb(bits, &bytes);
		t->use_eg_ready = 0;
		t->raw_bit_ready = 0;
		if (push_all(t, bytes, 0) != 0) {
			std::printf("FAIL A push\n");
			fails++;
		} else if (expect_ues(t, want, "A_plain_ue") != 0) {
			fails++;
		}
	}

	// B: gap-push plain ue (fits skid) + B2 EPB fixture
	{
		reset_dut(t);
		std::vector<uint32_t> want = {0, 1, 2, 3, 4, 5};
		std::string bits;
		for (uint32_t v : want)
			bits += ue_bits(v);
		std::vector<uint8_t> rbsp;
		pack_msb(bits, &rbsp);
		t->use_eg_ready = 0;
		t->raw_bit_ready = 0;
		if (push_all(t, rbsp, 2) != 0) {
			std::printf("FAIL B push\n");
			fails++;
		} else if (expect_ues(t, want, "B_gap_plain_ue") != 0) {
			fails++;
		}

		// B2: EPB straddle gaps — strip then bit-compare to rbsp oracle
		reset_dut(t);
		std::vector<uint8_t> annex_fix = {0x00, 0x00, 0x03, 0xA5, 0x5A};
		auto stripped = strip_epb(annex_fix);
		std::string good = bits_of(stripped);
		t->use_eg_ready = 0;
		t->raw_bit_ready = 0;
		if (push_all(t, annex_fix, 3) != 0) {
			std::printf("FAIL B2 push\n");
			fails++;
		} else {
			std::string got;
			int idle = 0;
			while ((int)got.size() < (int)good.size() && idle < 8000) {
				t->raw_bit_ready = 0;
				tick(t);
				if (!t->bit_valid) {
					idle++;
					continue;
				}
				idle = 0;
				got.push_back(t->bit_value ? '1' : '0');
				t->raw_bit_ready = 1;
				tick(t);
				t->raw_bit_ready = 0;
			}
			if (t->epb_removed < 1) {
				std::printf("FAIL B2 epb_removed=%d\n", (int)t->epb_removed);
				fails++;
			} else if (got != good) {
				std::printf("FAIL B2 bits got=%zu want=%zu\n", got.size(), good.size());
				fails++;
			} else {
				std::printf("OK B2 epb_straddle_bits epb=%d rbsp_bytes=%d\n", (int)t->epb_removed,
				            (int)t->rbsp_bytes);
			}
		}
	}

	// C NEGATIVE
	{
		std::vector<uint8_t> annex = {0x00, 0x00, 0x03, 0xA5, 0x5A};
		auto stripped = strip_epb(annex);
		std::string naive = bits_of(annex);
		std::string good = bits_of(stripped);
		if (naive == good) {
			std::printf("FAIL C NEGATIVE fixture: naive==stripped\n");
			fails++;
		} else {
			reset_dut(t);
			t->use_eg_ready = 0;
			t->raw_bit_ready = 0;
			if (push_all(t, annex, 1) != 0) {
				std::printf("FAIL C push\n");
				fails++;
			} else {
				std::string got;
				int idle = 0;
				while (got.size() < good.size() && idle < 8000) {
					t->raw_bit_ready = 0;
					tick(t);
					if (!t->bit_valid) {
						idle++;
						continue;
					}
					idle = 0;
					got.push_back(t->bit_value ? '1' : '0');
					t->raw_bit_ready = 1;
					tick(t);
					t->raw_bit_ready = 0;
				}
				if (got != good) {
					std::printf("FAIL C DUT bits != stripped (got %zu want %zu)\n", got.size(),
					            good.size());
					fails++;
				} else if (got == naive) {
					std::printf("FAIL C DUT matched naive keep-0x03\n");
					fails++;
				} else {
					std::printf("OK C NEGATIVE naive_keep_0x03_rejected epb=%d\n",
					            (int)t->epb_removed);
				}
			}
		}
	}

	// D mid-symbol stall
	{
		reset_dut(t);
		std::vector<uint8_t> bytes;
		pack_msb(ue_bits(3), &bytes);
		t->use_eg_ready = 0;
		t->raw_bit_ready = 0;
		if (push_all(t, bytes, 0) != 0) {
			std::printf("FAIL D push\n");
			fails++;
		} else {
			t->use_eg_ready = 1;
			t->eg_start = 1;
			tick(t);
			t->eg_start = 0;
			int took = 0;
			for (int i = 0; i < 50 && took < 2; ++i) {
				tick(t);
				if (t->bit_valid && t->bit_ready_to_feed)
					took++;
			}
			t->use_eg_ready = 0;
			t->raw_bit_ready = 0;
			int v0 = t->bit_valid;
			int b0 = t->bit_value;
			int bad = 0;
			for (int s = 0; s < 20; ++s) {
				tick(t);
				if (v0 && t->bit_valid && t->bit_value != b0) {
					std::printf("FAIL D bit changed while stalled\n");
					bad = 1;
					fails++;
					break;
				}
			}
			if (!bad) {
				t->use_eg_ready = 1;
				int rc_done = 0;
				uint32_t ue = 0;
				for (int i = 0; i < 5000; ++i) {
					tick(t);
					if (t->eg_done) {
						rc_done = 1;
						ue = static_cast<uint32_t>(t->eg_ue_value);
						break;
					}
				}
				if (!rc_done || !t->eg_ok || ue != 3) {
					std::printf("FAIL D resume ue ok=%d done=%d ue=%u\n", (int)t->eg_ok, rc_done, ue);
					fails++;
				} else {
					std::printf("OK D mid_symbol_stall_resume ue=3\n");
				}
			}
		}
	}

	// E backpressure
	{
		reset_dut(t);
		t->use_eg_ready = 0;
		t->raw_bit_ready = 0;
		int saw_not_ready = 0;
		for (int i = 0; i < 20; ++i) {
			int w = 0;
			while (!t->in_ready && w < 5) {
				saw_not_ready = 1;
				tick(t);
				w++;
			}
			if (!t->in_ready) {
				saw_not_ready = 1;
				break;
			}
			t->in_valid = 1;
			t->in_byte = static_cast<uint8_t>(0xA0 + i);
			t->in_last = 0;
			tick(t);
			t->in_valid = 0;
		}
		if (!saw_not_ready) {
			std::printf("FAIL E expected in_ready low after flood\n");
			fails++;
		} else {
			std::printf("OK E skid_backpressure in_ready_low\n");
		}
	}

	delete t;
	if (fails) {
		std::printf("FAIL bitstream_to_exp_golomb fails=%d\n", fails);
		return 1;
	}
	std::printf("PASS bitstream_to_exp_golomb all cases\n");
	return 0;
}
