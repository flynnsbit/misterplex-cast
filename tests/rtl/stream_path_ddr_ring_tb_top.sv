module stream_path_ddr_ring_tb_top #(
	parameter bit FAULT_WRAP_DATA = 1'b0,
	parameter bit FAULT_UNDERRUN_TELEM = 1'b0,
	parameter bit FAULT_OVERRUN_TELEM = 1'b0
)(
	input  wire        clk,
	input  wire        reset,
	input  wire        ioctl_download,
	input  wire        ioctl_wr,
	input  wire [7:0]  ioctl_dout,
	input  wire        enable,
	input  wire        flush,
	input  wire        ddr_stream_enable,
	input  wire        ddr_busy,
	input  wire [63:0] ddr_model_dout,
	input  wire        ddr_model_dout_ready,
	output wire        ddr_bus_want,
	output wire  [7:0] ddr_burstcnt,
	output wire [28:0] ddr_addr,
	output wire        ddr_rd,
	output wire [63:0] ddr_din,
	output wire  [7:0] ddr_be,
	output wire        ddr_we,
	output wire [15:0] nalu_count,
	output wire [7:0]  last_nal_type,
	output wire [31:0] bytes_seen,
	output wire [15:0] fifo_level,
	output wire        stream_ddr_active,
	output wire [31:0] stream_ddr_bytes_out,
	output wire [15:0] stream_ddr_underruns,
	output wire [15:0] stream_ddr_overruns,
	output wire [31:0] stream_ddr_host_write,
	output wire [31:0] stream_ddr_fpga_read
);
	wire [15:0] underruns_raw;
	wire [15:0] overruns_raw;
	wire [63:0] ddr_dout_to_dut;
	reg [28:0] rd_addr_q;

	always @(posedge clk) begin
		if (ddr_rd)
			rd_addr_q <= ddr_addr;
	end

	assign ddr_dout_to_dut =
		(FAULT_WRAP_DATA && ddr_model_dout_ready && rd_addr_q == 29'(32'h3010_0000 >> 3))
			? (ddr_model_dout ^ 64'h0000_0000_0000_00FF) : ddr_model_dout;
	assign stream_ddr_underruns = FAULT_UNDERRUN_TELEM ? 16'd0 : underruns_raw;
	assign stream_ddr_overruns  = FAULT_OVERRUN_TELEM  ? 16'd0 : overruns_raw;

	wire        has_stream;
	wire [31:0] bytes_in;
	wire        has_idr;
	wire [7:0]  idr_count;
	wire [7:0]  sps_count;
	wire [7:0]  pps_count;
	wire [7:0]  slice_count;
	wire [15:0] stub_frames;
	wire        stub_busy;
	wire        sps_valid;
	wire [7:0]  sps_profile;
	wire [7:0]  sps_level;
	wire [15:0] sps_width;
	wire [15:0] sps_height;
	wire [7:0]  sps_mb_w;
	wire [7:0]  sps_mb_h;
	wire        pps_valid;
	wire        slice_valid;
	wire [7:0]  slice_type;
	wire        slice_is_i;
	wire [7:0]  first_mb_type;
	wire        has_mb_type;
	wire [5:0]  slice_qp;
	wire [4:0]  residual_tc;
	wire [1:0]  residual_t1;
	wire        residual_ok;
	wire signed [7:0] residual_dc;
	wire [7:0]  residual_csum;
	wire signed [15:0] residual_coeff [0:15];
	wire        residual_place_pulse;
	wire [7:0]  recon_sig;
	wire [7:0]  recon_dbg;
	wire        recon_dbg_valid;
	wire        recon_valid;
	wire        fs_wr_en;
	wire [15:0] fs_wr_pixel;
	wire        fs_wr_reset;
	wire        fs_swap;

	stream_path spath (
		.clk(clk),
		.reset(reset),
		.ioctl_download(ioctl_download),
		.ioctl_wr(ioctl_wr),
		.ioctl_dout(ioctl_dout),
		.enable(enable),
		.flush(flush),
		.ddr_stream_enable(ddr_stream_enable),
		.ddr_bus_want(ddr_bus_want),
		.ddr_busy(ddr_busy),
		.ddr_burstcnt(ddr_burstcnt),
		.ddr_addr(ddr_addr),
		.ddr_dout(ddr_dout_to_dut),
		.ddr_dout_ready(ddr_model_dout_ready),
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
		.stream_ddr_underruns(underruns_raw),
		.stream_ddr_overruns(overruns_raw),
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
		.recon_sig(recon_sig),
		.recon_dbg(recon_dbg),
		.recon_dbg_valid(recon_dbg_valid),
		.recon_valid(recon_valid),
		.fs_wr_en(fs_wr_en),
		.fs_wr_pixel(fs_wr_pixel),
		.fs_wr_reset(fs_wr_reset),
		.fs_swap(fs_swap)
	);
endmodule
