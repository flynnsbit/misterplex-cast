// ddr_frame_base_mux — present-bank base select for option-b direct reader.
//
// Product equation today (ddr_frame_store.sv:737):
//   fill_bank_base = fill_bank ? BASE_W1 : BASE_W0
// With DYN_BASE_EN=0 this module is bit-identical to that select:
//   fill_bank_base = bank ? base_w1 : base_w0
//   using_dyn      = 0
//
// With DYN_BASE_EN=1 (research / future w-mem doorbell base ABI):
//   if dyn_valid[bank] -> dyn_base[bank], else fixed BASE_W*
// Fabric READs the published buffer in place (present R only, ~0 M10K).
// Doorbell/PLXF ABI that carries the base is owned by w-mem — not redefined here.
//
// Contrast source→bank DMA (ddr_frame_dma.sv): full R+W traffic; only retires
// uncached publication memcpy after pin/SG/coherency; decode still writes pixels.
//
// PREREG area (pre-fit): ALM 0..4, M10K 0 (no RAM; layout N/A), REG 0, DSP 0.
// No `timescale — matches ddr_frame_store / present_core (avoids TIMESCALEMOD).

module ddr_frame_base_mux #(
	parameter bit DYN_BASE_EN = 1'b0
)(
	input  wire        bank,        // 0/1 — same sense as fill_bank / disp_bank
	input  wire [28:0] base_w0,     // fixed bank0 qword base (= BASE_W0)
	input  wire [28:0] base_w1,     // fixed bank1 qword base (= BASE_W1)
	input  wire [28:0] dyn_base0,   // dynamic bank0 qword base (w-mem ABI later)
	input  wire [28:0] dyn_base1,
	input  wire        dyn_valid0,
	input  wire        dyn_valid1,
	output wire [28:0] fill_bank_base,
	output wire        using_dyn
);
	// Golden fixed select — must match ddr_frame_store.sv:737 shape.
	wire [28:0] fixed_base = bank ? base_w1 : base_w0;
	wire        dyn_ok     = bank ? dyn_valid1 : dyn_valid0;
	wire [28:0] dyn_sel    = bank ? dyn_base1  : dyn_base0;

	generate
		if (DYN_BASE_EN) begin : g_dyn
			assign using_dyn      = dyn_ok;
			assign fill_bank_base = dyn_ok ? dyn_sel : fixed_base;
		end else begin : g_fixed
			// Product default: dyn_* ports must not affect the base.
			assign using_dyn      = 1'b0;
			assign fill_bank_base = fixed_base;
		end
	endgenerate
endmodule
