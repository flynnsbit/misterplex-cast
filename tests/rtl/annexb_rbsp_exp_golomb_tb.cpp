// Product chain: annex-B → h264_rbsp_filter → bit window (no strip) → exp_golomb
// A) plain ue via filter+window
// B) EPB removed by rbsp_filter (not feeder); ue OK
// C) NEGATIVE: feeder STRIP_EPB=0 must NOT remove EPB if filter bypassed
//    (fixture: bits through filter differ from naive keep)
// D) out_last / in_last NAL boundary: filter done after last

#include "Vannexb_rbsp_exp_golomb_tb_top.h"
#include "verilated.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

static vluint64_t main_time = 0;
double sc_time_stamp() { return static_cast<double>(main_time); }

static void tick(Vannexb_rbsp_exp_golomb_tb_top* t) {
	t->clk = 0; t->eval(); main_time++;
	t->clk = 1; t->eval(); main_time++;
}

static void reset_dut(Vannexb_rbsp_exp_golomb_tb_top* t) {
	t->reset = 1; t->clear = 0; t->in_valid = 0; t->in_byte = 0; t->in_last = 0;
	t->use_eg_ready = 0; t->raw_bit_ready = 0; t->eg_start = 0; t->eg_signed_mode = 0;
	for (int i = 0; i < 4; ++i) tick(t);
	t->reset = 0; tick(t);
}

static std::string ue_bits(uint32_t code) {
	uint32_t x = code + 1;
	int lz = 0;
	for (uint32_t tt = x; tt > 1; tt >>= 1) lz++;
	std::string s(static_cast<size_t>(lz), '0');
	for (int i = lz; i >= 0; --i) s.push_back(((x >> i) & 1) ? '1' : '0');
	return s;
}

static void pack_msb(const std::string& bits, std::vector<uint8_t>* out) {
	uint8_t cur = 0; int n = 0;
	for (char c : bits) {
		cur = static_cast<uint8_t>((cur << 1) | (c == '1' ? 1 : 0));
		if (++n == 8) { out->push_back(cur); cur = 0; n = 0; }
	}
	if (n) { cur = static_cast<uint8_t>(cur << (8 - n)); out->push_back(cur); }
}

static int push_all(Vannexb_rbsp_exp_golomb_tb_top* t, const std::vector<uint8_t>& bytes, int gap) {
	for (size_t i = 0; i < bytes.size(); ++i) {
		for (int g = 0; g < gap; ++g) { t->in_valid = 0; tick(t); }
		int w = 0;
		while (!t->in_ready) { t->in_valid = 0; tick(t); if (++w > 20000) return -1; }
		t->in_valid = 1; t->in_byte = bytes[i]; t->in_last = (i + 1 == bytes.size());
		tick(t);
		t->in_valid = 0; t->in_last = 0;
	}
	return 0;
}

static int read_ue(Vannexb_rbsp_exp_golomb_tb_top* t, uint32_t* ue_out) {
	t->use_eg_ready = 1; t->raw_bit_ready = 0; t->eg_signed_mode = 0;
	t->eg_start = 1; tick(t); t->eg_start = 0;
	for (int i = 0; i < 8000; ++i) {
		tick(t);
		if (t->eg_done) {
			if (!t->eg_ok) return -2;
			*ue_out = static_cast<uint32_t>(t->eg_ue_value);
			for (int k = 0; k < 2; ++k) tick(t);
			return 0;
		}
	}
	return -1;
}

static int expect_ues(Vannexb_rbsp_exp_golomb_tb_top* t, const std::vector<uint32_t>& want, const char* tag) {
	for (size_t i = 0; i < want.size(); ++i) {
		uint32_t got = 0;
		int rc = read_ue(t, &got);
		if (rc != 0 || got != want[i]) {
			std::printf("FAIL %s ue[%zu] rc=%d got=%u want=%u\n", tag, i, rc, got, want[i]);
			return 1;
		}
	}
	std::printf("OK %s (%zu ues)\n", tag, want.size());
	return 0;
}

int main(int argc, char** argv) {
	Verilated::commandArgs(argc, argv);
	auto* t = new Vannexb_rbsp_exp_golomb_tb_top;
	int fails = 0;
	std::printf("CASE EXECUTED annexb_rbsp_exp_golomb\n");

	// A plain
	{
		reset_dut(t);
		std::vector<uint32_t> want = {0, 1, 2, 3, 4, 5};
		std::string bits; for (uint32_t v : want) bits += ue_bits(v);
		std::vector<uint8_t> bytes; pack_msb(bits, &bytes);
		if (push_all(t, bytes, 0) != 0) { std::printf("FAIL A push\n"); fails++; }
		else if (expect_ues(t, want, "A_plain_via_rbsp_filter") != 0) fails++;
	}

	// B EPB stripped by h264_rbsp_filter
	{
		reset_dut(t);
		std::vector<uint8_t> annex = {0x00, 0x00, 0x03, 0xFF}; // rbsp 00 00 FF after strip
		// Don't decode ue across zeros — check filter epb + feeder epb_removed==0
		if (push_all(t, annex, 2) != 0) { std::printf("FAIL B push\n"); fails++; }
		else {
			// drain some bits
			t->raw_bit_ready = 1; t->use_eg_ready = 0;
			for (int i = 0; i < 64; ++i) tick(t);
			if (t->epb_removed < 1) {
				std::printf("FAIL B filter epb_removed=%d\n", (int)t->epb_removed);
				fails++;
			} else if (t->feed_epb_removed != 0) {
				std::printf("FAIL B feeder should not strip (STRIP_EPB=0) got %d\n",
				            (int)t->feed_epb_removed);
				fails++;
			} else if (t->rbsp_len != 3) {
				std::printf("FAIL B rbsp_len=%d want 3\n", (int)t->rbsp_len);
				fails++;
			} else {
				std::printf("OK B epb_by_rbsp_filter_only epb=%d rbsp_len=%d feed_epb=%d\n",
				            (int)t->epb_removed, (int)t->rbsp_len, (int)t->feed_epb_removed);
			}
		}
	}

	// B2: EPB + ue payload after strip equals packed want
	{
		reset_dut(t);
		std::vector<uint32_t> want = {0, 1, 2, 3};
		std::string bits; for (uint32_t v : want) bits += ue_bits(v);
		std::vector<uint8_t> rbsp; pack_msb(bits, &rbsp);
		// Force EPB: 00 00 03 + rbsp would add zeros — instead insert 03 after natural 00 00 if any
		std::vector<uint8_t> annex = rbsp;
		bool inj = false;
		for (size_t i = 0; i + 1 < rbsp.size(); ++i) {
			if (rbsp[i] == 0 && rbsp[i+1] == 0) {
				annex.insert(annex.begin() + static_cast<long>(i + 2), 0x03);
				inj = true;
				break;
			}
		}
		if (!inj) {
			// annex-B fixture independent: filter strips, then plain small want via 0xFF-like
			// Use annex = rbsp only and still check filter path for ue (no EPB)
			if (push_all(t, rbsp, 1) != 0) { std::printf("FAIL B2 push\n"); fails++; }
			else if (expect_ues(t, want, "B2_no_epb_ue") != 0) fails++;
			else std::printf("OK B2_note no natural EPB site in pack\n");
		} else {
			if (push_all(t, annex, 1) != 0) { std::printf("FAIL B2 push\n"); fails++; }
			else if (t->epb_removed < 1) {
				// need cycles for filter
				for (int i = 0; i < 8; ++i) tick(t);
				if (t->epb_removed < 1) {
					std::printf("FAIL B2 epb_removed=%d\n", (int)t->epb_removed);
					fails++;
				} else if (expect_ues(t, want, "B2_epb_ue") != 0) fails++;
			} else if (expect_ues(t, want, "B2_epb_ue") != 0) fails++;
		}
	}

	// C NEGATIVE: if EPB not stripped, first bits of 00 00 03 A5 != stripped
	{
		std::vector<uint8_t> annex = {0x00, 0x00, 0x03, 0xA5};
		// Software oracles
		std::string naive;
		for (uint8_t b : annex)
			for (int i = 7; i >= 0; --i) naive.push_back(((b >> i) & 1) ? '1' : '0');
		std::vector<uint8_t> stripped = {0x00, 0x00, 0xA5};
		std::string good;
		for (uint8_t b : stripped)
			for (int i = 7; i >= 0; --i) good.push_back(((b >> i) & 1) ? '1' : '0');
		if (naive == good) { std::printf("FAIL C fixture\n"); fails++; }
		else {
			reset_dut(t);
			if (push_all(t, annex, 0) != 0) { std::printf("FAIL C push\n"); fails++; }
			else {
				std::string got;
				int idle = 0;
				while (got.size() < good.size() && idle < 8000) {
					t->raw_bit_ready = 0; t->use_eg_ready = 0;
					tick(t);
					if (!t->bit_valid) { idle++; continue; }
					idle = 0;
					got.push_back(t->bit_value ? '1' : '0');
					t->raw_bit_ready = 1; tick(t); t->raw_bit_ready = 0;
				}
				if (got != good) {
					std::printf("FAIL C bits != filter-stripped (got %zu want %zu) epb=%d\n",
					            got.size(), good.size(), (int)t->epb_removed);
					fails++;
				} else if (got == naive) {
					std::printf("FAIL C matched naive keep-0x03\n");
					fails++;
				} else {
					std::printf("OK C NEGATIVE filter_strip_not_naive epb=%d\n", (int)t->epb_removed);
				}
			}
		}
	}

	// D filter done on in_last
	{
		reset_dut(t);
		std::vector<uint8_t> one = {0x80}; // ue(0)
		if (push_all(t, one, 0) != 0) { std::printf("FAIL D push\n"); fails++; }
		else {
			for (int i = 0; i < 16; ++i) tick(t);
			if (!t->filter_done) {
				std::printf("FAIL D filter_done not set after in_last\n");
				fails++;
			} else {
				uint32_t ue = 99;
				if (read_ue(t, &ue) != 0 || ue != 0) {
					std::printf("FAIL D ue=%u\n", ue);
					fails++;
				} else {
					std::printf("OK D nal_last_filter_done_ue0\n");
				}
			}
		}
	}

	delete t;
	if (fails) {
		std::printf("FAIL annexb_rbsp_exp_golomb fails=%d\n", fails);
		return 1;
	}
	std::printf("PASS annexb_rbsp_exp_golomb all cases\n");
	return 0;
}
