// present_store_geom: default byte-identical vs prerefactor + 720p identity probe.
#include "Vpresent_store_geom_tb_top.h"
#include "verilated.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>

static int g_fail = 0;

static void expect(const char* n, bool ok) {
	if (!ok) {
		std::fprintf(stderr, "FAIL %s\n", n);
		g_fail++;
	}
}

int main(int argc, char** argv) {
	Verilated::commandArgs(argc, argv);
	auto* top = new Vpresent_store_geom_tb_top;

	// ---- Default identity: every (hc,py) in 0..600 x 0..260 ----
	uint64_t checked = 0;
	uint64_t p720_ok = 0;
	bool abort_scan = false;
	for (int py = 0; py <= 260 && !abort_scan; ++py) {
		for (int hc = 0; hc <= 600; ++hc) {
			top->hc = hc & 0x3ff;
			top->py = py & 0x3ff;
			top->hb = 0;
			top->vb = 0;
			top->hc11 = 0;
			top->vc11 = 0;
			top->eval();
			if (top->dut_in != top->ref_in ||
			    top->dut_sx != top->ref_sx ||
			    top->dut_sy != top->ref_sy ||
			    top->dut_plr != top->ref_plr) {
				std::fprintf(stderr,
					"FAIL mismatch hc=%d py=%d dut in=%d sx=%u sy=%u plr=%d "
					"ref in=%d sx=%u sy=%u plr=%d\n",
					hc, py,
					top->dut_in, top->dut_sx, top->dut_sy, top->dut_plr,
					top->ref_in, top->ref_sx, top->ref_sy, top->ref_plr);
				g_fail++;
				if (g_fail > 20) { abort_scan = true; break; }
			}
			checked++;
		}
	}
	std::printf("OK default identity samples=%llu\n",
		(unsigned long long)checked);

	// Spot-check known mapping: hc=0 → sx=0; hc=528 last DE col
	top->hc = 0; top->py = 0; top->hb = 0; top->vb = 0; top->eval();
	expect("hc0_sx0", top->dut_sx == 0 && top->dut_in == 1);
	top->hc = 529; top->eval();
	expect("hc529_out_of_de", top->dut_in == 0);

	// NEGATIVE: wrong TPL would not match prerefactor — covered by dual DUT;
	// if someone hardcodes H_DE=640 in dut defaults, identity fails above.

	// ---- 720p identity addressing ----
	int p720_fail = 0;
	for (int v = 0; v < 720; v += 17) {
		for (int h = 0; h < 1280; h += 19) {
			top->hc11 = h;
			top->vc11 = v;
			top->hb = 0;
			top->vb = 0;
			top->eval();
			if (!top->p720_in || top->p720_sx != (uint16_t)h ||
			    top->p720_sy != (uint16_t)v ||
			    top->p720_sx >= 1280 || top->p720_sy >= 720)
				p720_fail++;
			p720_ok++;
		}
	}
	expect("p720_identity_grid", p720_fail == 0);
	// Edge clamps
	top->hc11 = 1280; top->vc11 = 720; top->eval();
	expect("p720_clamp_x", top->p720_sx == 1279);
	expect("p720_clamp_y", top->p720_sy == 719);
	expect("p720_oob_in0", top->p720_in == 0);
	std::printf("OK 720p identity samples=%llu sx_max_checked=1279 sy_max=719\n",
		(unsigned long long)p720_ok);

	delete top;
	if (g_fail) {
		std::fprintf(stderr, "present_store_geom_tb: %d FAIL(s)\n", g_fail);
		return 1;
	}
	std::printf("present_store_geom_tb: PASS\n");
	return 0;
}
