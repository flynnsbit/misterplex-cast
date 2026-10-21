// Testbench-only top for integrated stream_path reconstruction checks.
// Instantiates product stream_path.sv and its product dependencies; no synthesized
// RTL is modified for fault injection or fixture plumbing.
`default_nettype none

module stream_path_recon_integration_tb_top #(
	parameter int FRAME_W = 320,
	parameter int FRAME_H = 240,
	parameter bit FAULT_RECON_SIG_ZERO = 1'b0
)(
	input  wire        clk,
	input  wire        reset,
	input  wire        ioctl_download,
	input  wire        ioctl_wr,
	input  wire [7:0]  ioctl_dout,
	input  wire        enable,
	input  wire        flush,

	output wire        has_stream,
	output wire [15:0] nalu_count,
	output wire [7:0]  last_nal_type,
	output wire [31:0] bytes_in,
	output wire [31:0] bytes_seen,
	output wire [15:0] fifo_level,
	output wire        has_idr,
	output wire [7:0]  idr_count,
	output wire [7:0]  sps_count,
	output wire [7:0]  pps_count,
	output wire [7:0]  slice_count,
	output wire [15:0] stub_frames,
	output wire        stub_busy,
	output wire        sps_valid,
	output wire [15:0] sps_width,
	output wire [15:0] sps_height,
	output wire        pps_valid,
	output wire        slice_valid,
	output wire [5:0]  slice_qp,
	output wire [4:0]  residual_tc,
	output wire [1:0]  residual_t1,
	output wire        residual_ok,
	output wire [7:0]  residual_csum,
	output wire        residual_place_pulse,
	output wire [7:0]  recon_sig,
	output wire [7:0]  recon_dbg,
	output wire        recon_dbg_valid,
	output wire        recon_valid
);
	wire [7:0] sps_profile;
	wire [7:0] sps_level;
	wire [7:0] sps_mb_w;
	wire [7:0] sps_mb_h;
	wire [7:0] slice_type;
	wire slice_is_i;
	wire [7:0] first_mb_type;
	wire has_mb_type;
	wire signed [7:0] residual_dc;
	wire signed [15:0] residual_coeff [0:15];
	wire fs_wr_en;
	wire [15:0] fs_wr_pixel;
	wire fs_wr_reset;
	wire fs_swap;
	wire [7:0] recon_sig_dut;
	wire ddr_bus_want;
	wire [7:0] ddr_burstcnt;
	wire [28:0] ddr_addr;
	wire ddr_rd;
	wire [63:0] ddr_din;
	wire [7:0] ddr_be;
	wire ddr_we;
	wire stream_ddr_active;
	wire [31:0] stream_ddr_bytes_out, stream_ddr_host_write, stream_ddr_fpga_read;
	wire [15:0] stream_ddr_underruns, stream_ddr_overruns;

	stream_path #(
		.FRAME_W(FRAME_W),
		.FRAME_H(FRAME_H)
	) dut (
		.clk(clk),
		.reset(reset),
		.ioctl_download(ioctl_download),
		.ioctl_wr(ioctl_wr),
		.ioctl_dout(ioctl_dout),
		.enable(enable),
		.flush(flush),
		.ddr_stream_enable(1'b0),
		.ddr_bus_want(ddr_bus_want),
		.ddr_busy(1'b1),
		.ddr_burstcnt(ddr_burstcnt),
		.ddr_addr(ddr_addr),
		.ddr_dout(64'd0),
		.ddr_dout_ready(1'b0),
		.ddr_rd(ddr_rd),
		.ddr_din(ddr_din),
		.ddr_be(ddr_be),
		.ddr_we(ddr_we),
		.has_stream(has_stream),
		.nalu_count(nalu_count),
		.last_nal_type(last_nal_type),
		.bytes_in(bytes_in),
		.bytes_seen(bytes_seen),
		.fifo_level(fifo_level),
		.stream_ddr_active(stream_ddr_active),
		.stream_ddr_bytes_out(stream_ddr_bytes_out),
		.stream_ddr_underruns(stream_ddr_underruns),
		.stream_ddr_overruns(stream_ddr_overruns),
		.stream_ddr_host_write(stream_ddr_host_write),
		.stream_ddr_fpga_read(stream_ddr_fpga_read),
		.has_idr(has_idr),
		.idr_count(idr_count),
		.sps_count(sps_count),
		.pps_count(pps_count),
		.slice_count(slice_count),
		.stub_frames(stub_frames),
		.stub_busy(stub_busy),
		.sps_valid(sps_valid),
		.sps_profile(sps_profile),
		.sps_level(sps_level),
		.sps_width(sps_width),
		.sps_height(sps_height),
		.sps_mb_w(sps_mb_w),
		.sps_mb_h(sps_mb_h),
		.pps_valid(pps_valid),
		.slice_valid(slice_valid),
		.slice_type(slice_type),
		.slice_is_i(slice_is_i),
		.first_mb_type(first_mb_type),
		.has_mb_type(has_mb_type),
		.slice_qp(slice_qp),
		.residual_tc(residual_tc),
		.residual_t1(residual_t1),
		.residual_ok(residual_ok),
		.residual_dc(residual_dc),
		.residual_csum(residual_csum),
		.residual_coeff(residual_coeff),
		.residual_place_pulse(residual_place_pulse),
		.recon_sig(recon_sig_dut),
		.recon_dbg(recon_dbg),
		.recon_dbg_valid(recon_dbg_valid),
		.recon_valid(recon_valid),
		.fs_wr_en(fs_wr_en),
		.fs_wr_pixel(fs_wr_pixel),
		.fs_wr_reset(fs_wr_reset),
		.fs_swap(fs_swap)
	);

	assign recon_sig = FAULT_RECON_SIG_ZERO ? 8'h00 : recon_sig_dut;
endmodule

`default_nettype wire
