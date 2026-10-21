// Dual-DUT ddram_frame_rd: independent DDRAM ports per instance.
`timescale 1ns / 1ps
`default_nettype none

module ddram_frame_rd_geom_tb_top (
	input  wire        clk,
	input  wire        reset,
	input  wire        start_req,
	input  wire        bank_sel,
	input  wire        swap_pending,
	input  wire        wr_ready,
	input  wire        dut_BUSY,
	input  wire [63:0] dut_DOUT,
	input  wire        dut_DOUT_READY,
	output wire        dut_RD,
	output wire        dut_WE,
	output wire [28:0] dut_ADDR,
	output wire  [7:0] dut_BURSTCNT,
	output wire        dut_wr_en,
	output wire [15:0] dut_wr_pixel,
	output wire        dut_swap_req,
	output wire        dut_busy,
	output wire [15:0] dut_frames_done,
	input  wire        ref_BUSY,
	input  wire [63:0] ref_DOUT,
	input  wire        ref_DOUT_READY,
	output wire        ref_RD,
	output wire        ref_WE,
	output wire [28:0] ref_ADDR,
	output wire  [7:0] ref_BURSTCNT,
	output wire        ref_wr_en,
	output wire [15:0] ref_wr_pixel,
	output wire        ref_swap_req,
	output wire        ref_busy,
	output wire [15:0] ref_frames_done,
	input  wire        p720_BUSY,
	input  wire [63:0] p720_DOUT,
	input  wire        p720_DOUT_READY,
	output wire        p720_RD,
	output wire [28:0] p720_ADDR,
	output wire  [7:0] p720_BURSTCNT,
	output wire        p720_wr_en,
	output wire [15:0] p720_wr_pixel,
	output wire        p720_swap_req,
	output wire        p720_busy,
	output wire [15:0] p720_frames_done
);
	wire unused_clk, unused_we, unused_db;
	wire [63:0] unused_din;
	wire [7:0] unused_be;

	ddram_frame_rd #(
		.WIDTH(320), .HEIGHT(240),
		.BANK_STRIDE_BYTES(32'h0004_0000),
		.BURST(16)
	) u_dut (
		.clk(clk), .reset(reset),
		.start_req(start_req), .bank_sel(bank_sel), .swap_pending(swap_pending),
		.status_osd(16'd0), .input_cmd_valid(1'b0), .input_cmd(8'd0),
		.sdram_test_state(4'd0), .sdram_size_code(4'd0), .sdram_error_count(16'd0),
		.sdram_read_sample(16'd0), .sdram_first_fail_valid(1'b0),
		.sdram_first_fail_addr(26'd0), .sdram_first_fail_expect(16'd0),
		.frame_sdram_state(8'd0), .frame_underrun_count(16'd0),
		.DDRAM_CLK(unused_clk), .DDRAM_BUSY(dut_BUSY),
		.DDRAM_BURSTCNT(dut_BURSTCNT), .DDRAM_ADDR(dut_ADDR),
		.DDRAM_DOUT(dut_DOUT), .DDRAM_DOUT_READY(dut_DOUT_READY),
		.DDRAM_RD(dut_RD), .DDRAM_DIN(unused_din), .DDRAM_BE(unused_be),
		.DDRAM_WE(dut_WE),
		.wr_en(dut_wr_en), .wr_pixel(dut_wr_pixel), .wr_reset_ptr(),
		.swap_req(dut_swap_req), .wr_ready(wr_ready),
		.busy(dut_busy), .frames_done(dut_frames_done), .doorbell_ok(unused_db)
	);

	ddram_frame_rd_prerefactor #(
		.WIDTH(320), .HEIGHT(240), .BURST(16)
	) u_ref (
		.clk(clk), .reset(reset),
		.start_req(start_req), .bank_sel(bank_sel), .swap_pending(swap_pending),
		.status_osd(16'd0), .input_cmd_valid(1'b0), .input_cmd(8'd0),
		.sdram_test_state(4'd0), .sdram_size_code(4'd0), .sdram_error_count(16'd0),
		.sdram_read_sample(16'd0), .sdram_first_fail_valid(1'b0),
		.sdram_first_fail_addr(26'd0), .sdram_first_fail_expect(16'd0),
		.frame_sdram_state(8'd0), .frame_underrun_count(16'd0),
		.DDRAM_CLK(), .DDRAM_BUSY(ref_BUSY),
		.DDRAM_BURSTCNT(ref_BURSTCNT), .DDRAM_ADDR(ref_ADDR),
		.DDRAM_DOUT(ref_DOUT), .DDRAM_DOUT_READY(ref_DOUT_READY),
		.DDRAM_RD(ref_RD), .DDRAM_DIN(), .DDRAM_BE(),
		.DDRAM_WE(ref_WE),
		.wr_en(ref_wr_en), .wr_pixel(ref_wr_pixel), .wr_reset_ptr(),
		.swap_req(ref_swap_req), .wr_ready(wr_ready),
		.busy(ref_busy), .frames_done(ref_frames_done), .doorbell_ok()
	);

	ddram_frame_rd #(
		.WIDTH(1280), .HEIGHT(720),
		.BANK_STRIDE_BYTES(32'h0020_0000),
		.PHYS_BASE(32'h3000_0000),
		.BURST(16)
	) u_720 (
		.clk(clk), .reset(reset),
		.start_req(start_req), .bank_sel(bank_sel), .swap_pending(swap_pending),
		.status_osd(16'd0), .input_cmd_valid(1'b0), .input_cmd(8'd0),
		.sdram_test_state(4'd0), .sdram_size_code(4'd0), .sdram_error_count(16'd0),
		.sdram_read_sample(16'd0), .sdram_first_fail_valid(1'b0),
		.sdram_first_fail_addr(26'd0), .sdram_first_fail_expect(16'd0),
		.frame_sdram_state(8'd0), .frame_underrun_count(16'd0),
		.DDRAM_CLK(), .DDRAM_BUSY(p720_BUSY),
		.DDRAM_BURSTCNT(p720_BURSTCNT), .DDRAM_ADDR(p720_ADDR),
		.DDRAM_DOUT(p720_DOUT), .DDRAM_DOUT_READY(p720_DOUT_READY),
		.DDRAM_RD(p720_RD), .DDRAM_DIN(), .DDRAM_BE(),
		.DDRAM_WE(unused_we),
		.wr_en(p720_wr_en), .wr_pixel(p720_wr_pixel), .wr_reset_ptr(),
		.swap_req(p720_swap_req), .wr_ready(wr_ready),
		.busy(p720_busy), .frames_done(p720_frames_done), .doorbell_ok()
	);
endmodule

`default_nettype wire
