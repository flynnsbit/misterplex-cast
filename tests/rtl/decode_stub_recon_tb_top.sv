// Testbench-only decode_stub wrapper. NOT part of the Quartus project.
// It instantiates the same product decode_stub.sv and h264_iq_idct_4x4.sv
// sources that files.qip compiles, then exposes a small Verilator-friendly
// drive surface for the first MB0 reconstruction signature.
`default_nettype none

module decode_stub_recon_tb #(
	parameter bit FAULT_PRED_ONLY = 1'b0
)(
	input  wire        clk,
	input  wire        reset,
	input  wire        vcl_pulse,
	input  wire        residual_ok,
	input  wire [5:0]  slice_qp,
	input  wire signed [15:0] coeff [0:15],
	output wire [7:0]  recon_sig,
	output wire [7:0]  recon_dbg,
	output wire        recon_dbg_valid,
	output wire        recon_valid,
	output wire        busy,
	output wire [15:0] frames_out
);
	wire signed [15:0] coeff16 [0:15];
	genvar i;
	generate
		for (i = 0; i < 16; i = i + 1) begin : g_coeff
			assign coeff16[i] = FAULT_PRED_ONLY ? 16'sd0 : coeff[i];
		end
	endgenerate

	wire wr_en;
	wire [15:0] wr_pixel;
	wire wr_reset_ptr;
	wire swap_req;

	decode_stub #(
		.WIDTH(320),
		.HEIGHT(240),
		.ENABLE_DPB_REF_SEAM(1'b0)
	) dut (
		.clk(clk),
		.reset(reset),
		.vcl_pulse(vcl_pulse),
		.last_nal_type(8'h65),
		.nalu_count(16'd1),
		.idr_count(8'd1),
		.has_idr(1'b1),
		.sps_valid(1'b1),
		.mb_w(8'd20),
		.mb_h(8'd15),
		.slice_type(8'd7),
		.slice_is_i(1'b1),
		.slice_valid(1'b0),
		.first_mb_addr(16'd0),
		.has_mb_type(1'b0),
		.first_mb_p_skip(1'b0),
		.first_mb_part_mode(3'd0),
		.first_mb_part_count(3'd0),
		.first_mb_uses_sub_mb(1'b0),
		.first_mb_intra(1'b1),
		.residual_ok(residual_ok),
		.residual_tc(5'd8),
		.residual_dc(coeff[0][7:0]),
		.residual_valid(residual_ok),
		.slice_qp(slice_qp),
		.residual_coeff(coeff16),
		.recon_sig(recon_sig),
		.recon_dbg(recon_dbg),
		.recon_dbg_valid(recon_dbg_valid),
		.recon_valid(recon_valid),
		.wr_en(wr_en),
		.wr_pixel(wr_pixel),
		.wr_reset_ptr(wr_reset_ptr),
		.swap_req(swap_req),
		.busy(busy),
		.frames_out(frames_out)
	);
endmodule

`default_nettype wire
