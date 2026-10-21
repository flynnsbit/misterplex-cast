// Boundary coverage for ddr_frame_present_geom (480p product + 720p L4 + 240p).
// Negative cases: naive hardcode leaving 618x480 window on a 720p FRAME fails.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include "Vddr_frame_present_geom_tb_top.h"
#include "verilated.h"

static int g_fails = 0;

static void expect(const char *name, bool ok) {
	if (!ok) {
		std::printf("FAIL %s\n", name);
		g_fails++;
	}
}

static void set_xy(Vddr_frame_present_geom_tb_top *top, unsigned x, unsigned y) {
	top->rd_x = x;
	top->rd_y = y;
	top->eval();
}

int main(int argc, char **argv) {
	Verilated::commandArgs(argc, argv);
	auto *top = new Vddr_frame_present_geom_tb_top;

	// ---- Static ends / strides (no xy) ----
	set_xy(top, 0, 0);
	expect("480_present_end_x_629", top->pe480_x == 629); // 11+618
	expect("480_present_end_y_480", top->pe480_y == 480);
	expect("480_y_line_qwords_78", top->yq480 == 78);     // 624/8
	expect("480_c_line_qwords_39", top->cq480 == 39);     // 624/16
	expect("480_frame_bytes", top->f480 == 449280);
	expect("480_bank0", top->b480_0 == 0x30000000u);
	expect("480_bank1", top->b480_1 == 0x30080000u);      // +0x80000
	expect("480_bank0_end", top->b480_end == 0x30000000u + 449280u);
	expect("480_doorbell", top->d480 == 0x300FF000u);
	// bank boundary ±1 relative to frame payload end
	const uint32_t b0_last = top->b480_end - 1;
	const uint32_t b0_overflow = top->b480_end;
	expect("480_bank_last_lt_bank1", b0_last < top->b480_1);
	expect("480_bank_end_le_bank1", b0_overflow <= top->b480_1);

	expect("720_present_end_x_1280", top->pe720_x == 1280);
	expect("720_present_end_y_720", top->pe720_y == 720);
	expect("720_y_line_qwords_160", top->yq720 == 160);   // 1280/8
	expect("720_c_line_qwords_80", top->cq720 == 80);     // 1280/16
	expect("720_frame_bytes_1382400", top->f720 == 1382400u);
	expect("720_bank0", top->b720_0 == 0x30180000u);
	expect("720_bank1", top->b720_1 == 0x30180000u + 0x00180000u);
	expect("720_bank0_end", top->b720_end == 0x30180000u + 1382400u);
	expect("720_doorbell", top->d720 == 0x3047F000u);
	expect("720_bank_last_lt_bank1", (top->b720_end - 1) < top->b720_1);
	expect("720_bank_end_le_bank1", top->b720_end <= top->b720_1);
	// exact bank boundary ±1 on stride (not just frame_bytes)
	const uint32_t stride_end = top->b720_0 + 0x00180000u;
	expect("720_stride_end_eq_bank1", stride_end == top->b720_1);
	expect("720_frame_lt_stride", top->f720 < 0x00180000u);
	expect("720_frame_plus1_still_in_bank",
	       (top->b720_0 + top->f720) < top->b720_1); // end == base+frame; last byte in bank
	expect("720_byte_frame_minus1_in",
	       (0x30180000u + 1382400u - 1u) < top->b720_1);
	expect("720_byte_at_frame_end_is_bank_boundary_ok",
	       (0x30180000u + 1382400u) <= top->b720_1);
	// first byte of bank1 is bank boundary
	expect("720_bank1_is_boundary", top->b720_1 == 0x30300000u);

	expect("240_present_end_320x240", top->pe240_x == 320 && top->pe240_y == 240);
	expect("240_frame_bytes", top->f240 == 320u * 240u * 3u / 2u);

	// ---- 480p visibility window (pillar 11, display 618 → x in [11,629) ) ----
	set_xy(top, 10, 0);
	expect("480_x10_outside_left", top->v480 == 0);
	set_xy(top, 11, 0);
	expect("480_x11_inside", top->v480 == 1);
	expect("480_x11_src0", top->s480_x == 0);
	set_xy(top, 628, 0);
	expect("480_x628_inside_last", top->v480 == 1);
	expect("480_x628_src617", top->s480_x == 617);
	set_xy(top, 629, 0);
	expect("480_x629_outside_right", top->v480 == 0);
	set_xy(top, 11, 479);
	expect("480_y479_inside", top->v480 == 1);
	set_xy(top, 11, 480);
	expect("480_y480_outside", top->v480 == 0);

	// ---- 720p full-frame visibility ----
	set_xy(top, 0, 0);
	expect("720_0_0_inside", top->v720 == 1);
	expect("720_0_0_src", top->s720_x == 0 && top->s720_y == 0);
	set_xy(top, 1279, 719);
	expect("720_1279_719_inside", top->v720 == 1);
	expect("720_1279_719_src", top->s720_x == 1279 && top->s720_y == 719);
	set_xy(top, 1280, 719);
	// rd_x is 11 bits; 1280 fits. Visibility must be outside.
	expect("720_1280_outside", top->v720 == 0);
	set_xy(top, 1279, 720);
	expect("720_y720_outside", top->v720 == 0);

	// NEGATIVE: if L4 still used 480p window, x=1279 would be outside (629 end).
	// We require 720p INSIDE at 1279 — a leftover 618-wide window fails here.
	set_xy(top, 1279, 100);
	expect("720_NEG_not_618_window", top->v720 == 1);
	set_xy(top, 700, 500);
	expect("720_NEG_y500_not_480_clip", top->v720 == 1);

	// 480p must NOT suddenly open to 1280 (regression guard)
	set_xy(top, 700, 100);
	expect("480_x700_still_outside", top->v480 == 0);
	set_xy(top, 100, 500);
	expect("480_y500_still_outside", top->v480 == 0);

	// 240p corners
	set_xy(top, 319, 239);
	expect("240_last_inside", top->v240 == 1);
	set_xy(top, 320, 239);
	expect("240_x320_outside", top->v240 == 0);
	set_xy(top, 319, 240);
	expect("240_y240_outside", top->v240 == 0);

	if (g_fails) {
		std::printf("ddr_frame_present_geom_tb FAIL count=%d\n", g_fails);
		delete top;
		return 1;
	}
	std::printf("PASS ddr_frame_present_geom_tb "
	            "480p_end=629x480 720p_end=1280x720 "
	            "720p_1279_719_inside bank720_stride=0x180000\n");
	delete top;
	return 0;
}
