// ddram_frame_rd geometry: default dual-DUT identity + 720p stride/completion.
#include "Vddram_frame_rd_geom_tb_top.h"
#include "verilated.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int g_fail = 0;
static void expect(const char* n, bool ok) {
	if (!ok) { std::fprintf(stderr, "FAIL %s\n", n); g_fail++; }
	else std::printf("OK   %s\n", n);
}

struct BusModel {
	int burst_left = 0;
	uint32_t addr = 0;
	int delay = 0;
	bool rd_seen = false;

	void tick_pre(bool rd, uint8_t bcnt, uint32_t a) {
		if (rd && burst_left == 0) {
			burst_left = bcnt ? bcnt : 1;
			addr = a;
			delay = 1; // 1-cycle latency
			rd_seen = true;
		}
	}
	void tick_drive(uint8_t& busy, uint64_t& dout, uint8_t& ready) {
		busy = 0;
		ready = 0;
		dout = 0;
		if (burst_left <= 0) return;
		if (delay > 0) { delay--; return; }
		// Pattern: low 32 = addr, high 32 = ~addr (stable, comparable)
		dout = (uint64_t(~addr) << 32) | uint64_t(addr);
		ready = 1;
		addr++;
		burst_left--;
	}
};

int main(int argc, char** argv) {
	Verilated::commandArgs(argc, argv);
	auto* top = new Vddram_frame_rd_geom_tb_top;
	BusModel dut_bus, ref_bus, p720_bus;

	auto eval = [&]() { top->eval(); };

	// Reset
	top->clk = 0;
	top->reset = 1;
	top->start_req = 0;
	top->bank_sel = 0;
	top->swap_pending = 0;
	top->wr_ready = 1;
	top->dut_BUSY = 0; top->ref_BUSY = 0; top->p720_BUSY = 0;
	top->dut_DOUT = 0; top->ref_DOUT = 0; top->p720_DOUT = 0;
	top->dut_DOUT_READY = 0; top->ref_DOUT_READY = 0; top->p720_DOUT_READY = 0;
	for (int i = 0; i < 8; i++) {
		eval();
		top->clk = 1; eval();
		top->clk = 0;
	}
	top->reset = 0;

	// Run a few idle cycles (doorbell polls — return 0 magic)
	for (int i = 0; i < 64; i++) {
		dut_bus.tick_pre(top->dut_RD, top->dut_BURSTCNT, top->dut_ADDR);
		ref_bus.tick_pre(top->ref_RD, top->ref_BURSTCNT, top->ref_ADDR);
		uint8_t b, r; uint64_t d;
		dut_bus.tick_drive(b, d, r); top->dut_BUSY = b; top->dut_DOUT = d; top->dut_DOUT_READY = r;
		ref_bus.tick_drive(b, d, r); top->ref_BUSY = b; top->ref_DOUT = d; top->ref_DOUT_READY = r;
		// p720 idle too
		p720_bus.tick_pre(top->p720_RD, top->p720_BURSTCNT, top->p720_ADDR);
		p720_bus.tick_drive(b, d, r); top->p720_BUSY = b; top->p720_DOUT = d; top->p720_DOUT_READY = r;
		top->clk = 1; eval();
		top->clk = 0; eval();
	}

	// ---- Phase 1: default dual identity (SPI start bank0) ----
	// Only drive dut+ref; hold p720 start low by... they share start_req!
	// Problem: all three share start_req. Run default compare first without
	// caring about p720 (it will also start). That's OK — compare dut vs ref only.

	top->start_req = 1;
	top->clk = 1; eval(); top->clk = 0; eval();
	top->start_req = 0;

	std::vector<uint16_t> dut_pix, ref_pix;
	std::vector<uint32_t> dut_addrs, ref_addrs;
	const int PIXELS = 320 * 240;
	int cycles = 0;
	const int MAX_CYC = 500000;

	while (cycles < MAX_CYC) {
		// Capture RD addresses on issue
		if (top->dut_RD) dut_addrs.push_back(top->dut_ADDR);
		if (top->ref_RD) ref_addrs.push_back(top->ref_ADDR);

		dut_bus.tick_pre(top->dut_RD, top->dut_BURSTCNT, top->dut_ADDR);
		ref_bus.tick_pre(top->ref_RD, top->ref_BURSTCNT, top->ref_ADDR);
		p720_bus.tick_pre(top->p720_RD, top->p720_BURSTCNT, top->p720_ADDR);

		uint8_t b, r; uint64_t d;
		dut_bus.tick_drive(b, d, r); top->dut_BUSY = b; top->dut_DOUT = d; top->dut_DOUT_READY = r;
		ref_bus.tick_drive(b, d, r); top->ref_BUSY = b; top->ref_DOUT = d; top->ref_DOUT_READY = r;
		p720_bus.tick_drive(b, d, r); top->p720_BUSY = b; top->p720_DOUT = d; top->p720_DOUT_READY = r;

		top->clk = 1; eval();
		if (top->dut_wr_en) dut_pix.push_back(top->dut_wr_pixel);
		if (top->ref_wr_en) ref_pix.push_back(top->ref_wr_pixel);
		top->clk = 0; eval();
		cycles++;

		if (!top->dut_busy && !top->ref_busy && dut_pix.size() >= (size_t)PIXELS
		    && ref_pix.size() >= (size_t)PIXELS)
			break;
	}

	expect("default_completed", dut_pix.size() >= (size_t)PIXELS && ref_pix.size() >= (size_t)PIXELS);
	expect("default_frames_done", top->dut_frames_done == 1 && top->ref_frames_done == 1);

	size_t n = std::min(dut_pix.size(), ref_pix.size());
	size_t mism = 0;
	for (size_t i = 0; i < n; i++) {
		if (dut_pix[i] != ref_pix[i]) {
			if (mism < 5)
				std::fprintf(stderr, "FAIL pix[%zu] dut=%04x ref=%04x\n", i, dut_pix[i], ref_pix[i]);
			mism++;
		}
	}
	expect("default_pixel_identical", mism == 0 && dut_pix.size() == ref_pix.size());
	std::printf("default pixels compared=%zu mismatches=%zu\n", n, mism);

	// Address stream identity
	size_t na = std::min(dut_addrs.size(), ref_addrs.size());
	size_t amism = 0;
	for (size_t i = 0; i < na; i++) {
		if (dut_addrs[i] != ref_addrs[i]) amism++;
	}
	expect("default_addr_identical", amism == 0 && dut_addrs.size() == ref_addrs.size());
	// BASE_W0 = 0x30000000>>3 = 0x06000000; first frame issue should be that
	if (!dut_addrs.empty()) {
		expect("default_base_w0", dut_addrs[0] == 0x06000000u);
		// bank stride qwords = 0x8000
		std::printf("default first_addr=0x%x issues=%zu\n", dut_addrs[0], dut_addrs.size());
	}

	// ---- Phase 2: 720p completion + stride ----
	// Reset and run only caring about p720 (dut/ref will also run — ignore).
	top->reset = 1;
	for (int i = 0; i < 8; i++) { top->clk = 1; eval(); top->clk = 0; eval(); }
	top->reset = 0;
	dut_bus = BusModel(); ref_bus = BusModel(); p720_bus = BusModel();
	for (int i = 0; i < 16; i++) {
		p720_bus.tick_pre(top->p720_RD, top->p720_BURSTCNT, top->p720_ADDR);
		uint8_t b, r; uint64_t d;
		p720_bus.tick_drive(b, d, r);
		top->p720_BUSY = b; top->p720_DOUT = d; top->p720_DOUT_READY = r;
		// keep others quiet
		top->dut_BUSY = 0; top->dut_DOUT_READY = 0;
		top->ref_BUSY = 0; top->ref_DOUT_READY = 0;
		// still need to service dut/ref or they hang the shared start — they have own buses
		dut_bus.tick_pre(top->dut_RD, top->dut_BURSTCNT, top->dut_ADDR);
		ref_bus.tick_pre(top->ref_RD, top->ref_BURSTCNT, top->ref_ADDR);
		dut_bus.tick_drive(b, d, r); top->dut_BUSY = b; top->dut_DOUT = d; top->dut_DOUT_READY = r;
		ref_bus.tick_drive(b, d, r); top->ref_BUSY = b; top->ref_DOUT = d; top->ref_DOUT_READY = r;
		top->clk = 1; eval(); top->clk = 0; eval();
	}

	top->bank_sel = 0;
	top->start_req = 1;
	top->clk = 1; eval(); top->clk = 0; eval();
	top->start_req = 0;

	const int P720_PIXELS = 1280 * 720;
	const int P720_QWORDS = P720_PIXELS / 4; // 230400
	const uint32_t P720_BASE = 0x06000000u;
	const uint32_t P720_STRIDE_QW = 0x00200000u / 8; // 0x40000 RGB565 2MiB bank
	uint64_t p720_writes = 0;
	uint32_t p720_max_addr = 0, p720_min_addr = 0xffffffffu;
	std::vector<uint32_t> p720_issue_addrs;
	cycles = 0;
	const int MAX_720 = 5000000;

	while (cycles < MAX_720) {
		if (top->p720_RD) {
			p720_issue_addrs.push_back(top->p720_ADDR);
			if (top->p720_ADDR < p720_min_addr) p720_min_addr = top->p720_ADDR;
			uint32_t end_a = top->p720_ADDR + top->p720_BURSTCNT;
			if (end_a > p720_max_addr) p720_max_addr = end_a;
		}
		p720_bus.tick_pre(top->p720_RD, top->p720_BURSTCNT, top->p720_ADDR);
		dut_bus.tick_pre(top->dut_RD, top->dut_BURSTCNT, top->dut_ADDR);
		ref_bus.tick_pre(top->ref_RD, top->ref_BURSTCNT, top->ref_ADDR);
		uint8_t b, r; uint64_t d;
		p720_bus.tick_drive(b, d, r); top->p720_BUSY = b; top->p720_DOUT = d; top->p720_DOUT_READY = r;
		dut_bus.tick_drive(b, d, r); top->dut_BUSY = b; top->dut_DOUT = d; top->dut_DOUT_READY = r;
		ref_bus.tick_drive(b, d, r); top->ref_BUSY = b; top->ref_DOUT = d; top->ref_DOUT_READY = r;

		top->clk = 1; eval();
		if (top->p720_wr_en) p720_writes++;
		top->clk = 0; eval();
		cycles++;
		if (!top->p720_busy && p720_writes >= (uint64_t)P720_PIXELS) break;
	}

	expect("p720_completed", p720_writes == (uint64_t)P720_PIXELS);
	expect("p720_frames_done", top->p720_frames_done == 1);
	expect("p720_qwords_count", P720_QWORDS == 230400);
	if (!p720_issue_addrs.empty()) {
		expect("p720_base", p720_issue_addrs[0] == P720_BASE);
		// Last issued addr + burst should stay within bank: base + QWORDS
		uint32_t bank_end = P720_BASE + (uint32_t)P720_QWORDS;
		expect("p720_no_overflow_bank", p720_max_addr <= bank_end);
		expect("p720_within_stride",
			(p720_max_addr - P720_BASE) <= P720_STRIDE_QW);
		// clog2-style: qword index fits in 18 bits (230400 < 2^18)
		expect("p720_qw_fits_18b", P720_QWORDS < (1 << 18));
		std::printf("p720 writes=%llu issues=%zu min_addr=0x%x max_end=0x%x bank_end=0x%x stride_qw=0x%x\n",
			(unsigned long long)p720_writes, p720_issue_addrs.size(),
			p720_min_addr, p720_max_addr, bank_end, P720_STRIDE_QW);
	} else {
		expect("p720_had_issues", false);
	}

	// NEGATIVE knowledge: 16-bit qword counter would wrap at 65536; we completed 230400.
	expect("p720_exceeds_16b_counter", P720_QWORDS > 65535);

	delete top;
	if (g_fail) {
		std::fprintf(stderr, "ddram_frame_rd_geom_tb: %d FAIL(s)\n", g_fail);
		return 1;
	}
	std::printf("ddram_frame_rd_geom_tb: PASS\n");
	return 0;
}
