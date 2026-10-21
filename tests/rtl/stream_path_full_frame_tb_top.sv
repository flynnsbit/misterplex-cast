// Testbench-only top for full-frame stream_path presentation comparison.
// The product stream_path RTL is instantiated unchanged; FAULT_PIXEL_XOR only
// perturbs the testbench-visible output so the comparator has a behavior red path.
`default_nettype none

module stream_path_full_frame_tb #(
	parameter int FRAME_W = 320,
	parameter int FRAME_H = 240,
	parameter bit FAULT_PIXEL_XOR = 1'b0,
	parameter bit FAULT_TRACE_COEFF0_PLUS1 = 1'b0
)(
	input  wire        clk,
	input  wire        reset,
	input  wire        ioctl_download,
	input  wire        ioctl_wr,
	input  wire [7:0]  ioctl_dout,
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
	output wire [7:0]  sps_profile,
	output wire [15:0] sps_width,
	output wire [15:0] sps_height,
	output wire [7:0]  sps_mb_w,
	output wire [7:0]  sps_mb_h,
	output wire        pps_valid,
	output wire        slice_valid,
	output wire [7:0]  slice_type,
	output wire        slice_is_i,
	output wire [7:0]  first_mb_type,
	output wire        has_mb_type,
	output wire [7:0]  recon_sig,
	output wire [7:0]  recon_dbg,
	output wire        recon_valid,
	output wire [5:0]  trace_slice_qp,
	output wire [4:0]  trace_residual_tc,
	output wire [1:0]  trace_residual_t1,
	output wire signed [7:0] trace_residual_dc,
	output wire [7:0]  trace_residual_csum,
	output wire signed [15:0] trace_residual_coeff [0:15],
	output wire signed [28:0] trace_idct_dequant [0:15],
	output wire signed [28:0] trace_idct_residual [0:15],
	output wire [7:0]  trace_recon_px [0:15],
	output wire        fs_wr_en,
	output wire [15:0] fs_wr_pixel,
	output wire        fs_wr_reset,
	output wire        fs_swap,

	output wire        native_inter_valid,
	output wire [15:0] native_inter_frame_idx,
	output wire [7:0]  native_inter_mb_x,
	output wire [7:0]  native_inter_mb_y,
	output wire        native_inter_p_skip,
	output wire [2:0]  native_inter_part_mode,
	output wire [7:0]  native_inter_pred_y [0:255],
	output wire [7:0]  native_inter_pred_u [0:63],
	output wire [7:0]  native_inter_pred_v [0:63],

	// Native I420 DPB write tap — per-sample stream from reconstruction path
	output wire        native_i420_wr_en,
	output wire [31:0] native_i420_wr_offset,
	output wire [7:0]  native_i420_wr_data,
	output wire [15:0] native_i420_wr_frame,

	// DPB reference pre-fill — testbench injects real IDR reference data
	input  wire        dpb_prefill_en,
	input  wire [31:0] dpb_prefill_addr,
	input  wire [7:0]  dpb_prefill_data
);
	wire [7:0] sps_level;
	wire [5:0] slice_qp;
	wire [4:0] residual_tc;
	wire [1:0] residual_t1;
	wire residual_ok;
	wire signed [7:0] residual_dc;
	wire [7:0] residual_csum;
	wire signed [15:0] residual_coeff [0:15];
	wire residual_place_pulse;
	wire recon_dbg_valid;
	wire [15:0] fs_wr_pixel_dut;
	wire ddr_bus_want, ddr_rd, ddr_we;
	wire [7:0] ddr_burstcnt, ddr_be;
	wire [28:0] ddr_addr;
	wire [63:0] ddr_din;
	wire stream_ddr_active;
	wire [31:0] stream_ddr_bytes_out, stream_ddr_host_write, stream_ddr_fpga_read;
	wire [15:0] stream_ddr_underruns, stream_ddr_overruns;
	wire [1:0] disable_deblocking_filter_idc;
	wire signed [4:0] slice_alpha_c0_offset_div2;
	wire signed [4:0] slice_beta_offset_div2;
	wire signed [4:0] slice_alpha_c0_offset;
	wire signed [4:0] slice_beta_offset;
	wire first_mb_p_skip_w;
	wire [7:0] p_skip_run_w;
	wire [2:0] first_mb_part_mode_w;
	wire [2:0] first_mb_part_count_w;
	wire first_mb_uses_sub_mb_w;
	wire first_mb_intra_w;

	stream_path #(
		.FRAME_W(FRAME_W),
		.FRAME_H(FRAME_H)
	) dut (
		.clk(clk),
		.reset(reset),
		.ioctl_download(ioctl_download),
		.ioctl_wr(ioctl_wr),
		.ioctl_dout(ioctl_dout),
		.enable(1'b1),
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
		.first_mb_p_skip(first_mb_p_skip_w),
		.p_skip_run(p_skip_run_w),
		.first_mb_part_mode(first_mb_part_mode_w),
		.first_mb_part_count(first_mb_part_count_w),
		.first_mb_uses_sub_mb(first_mb_uses_sub_mb_w),
		.first_mb_intra(first_mb_intra_w),
		.slice_qp(slice_qp),
		.disable_deblocking_filter_idc(disable_deblocking_filter_idc),
		.slice_alpha_c0_offset_div2(slice_alpha_c0_offset_div2),
		.slice_beta_offset_div2(slice_beta_offset_div2),
		.slice_alpha_c0_offset(slice_alpha_c0_offset),
		.slice_beta_offset(slice_beta_offset),
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
		.fs_wr_pixel(fs_wr_pixel_dut),
		.fs_wr_reset(fs_wr_reset),
		.fs_swap(fs_swap)
	);

	assign fs_wr_pixel = FAULT_PIXEL_XOR ? (fs_wr_pixel_dut ^ 16'hffff) : fs_wr_pixel_dut;
	assign trace_slice_qp = dut.stub.lat_qp;
	assign trace_residual_tc = dut.stub.lat_res_tc;
	assign trace_residual_t1 = dut.sl_place_t1;
	assign trace_residual_dc = dut.stub.lat_res_dc;
	assign trace_residual_csum = residual_csum;
	assign native_inter_valid = dut.stub.dpb_fetch_done && dut.stub.dpb_inter_ok;
	assign native_inter_frame_idx = dut.stub.frames_out;
	assign native_inter_mb_x = dut.stub.lat_p_mb_x;
	assign native_inter_mb_y = dut.stub.lat_p_mb_y;
	assign native_inter_p_skip = dut.stub.lat_p_skip;
	assign native_inter_part_mode = dut.stub.lat_p_part_mode;
	genvar trace_i;
	generate
		for (trace_i = 0; trace_i < 16; trace_i = trace_i + 1) begin : gen_trace
			assign trace_residual_coeff[trace_i] =
				(FAULT_TRACE_COEFF0_PLUS1 && trace_i == 0) ?
					(dut.stub.lat_coeff[trace_i] + 9'sd1) : dut.stub.lat_coeff[trace_i];
			assign trace_idct_dequant[trace_i] = dut.stub.idct_dequant[trace_i];
			assign trace_idct_residual[trace_i] = dut.stub.idct_residual[trace_i];
			assign trace_recon_px[trace_i] = dut.stub.recon_px[trace_i];
		end
	endgenerate
	genvar native_y_i;
	generate
		for (native_y_i = 0; native_y_i < 256; native_y_i = native_y_i + 1) begin : gen_native_y
			assign native_inter_pred_y[native_y_i] = dut.stub.dpb_pred_y[native_y_i];
		end
	endgenerate
	genvar native_c_i;
	generate
		for (native_c_i = 0; native_c_i < 64; native_c_i = native_c_i + 1) begin : gen_native_c
			assign native_inter_pred_u[native_c_i] = dut.stub.dpb_pred_u[native_c_i];
			assign native_inter_pred_v[native_c_i] = dut.stub.dpb_pred_v[native_c_i];
		end
	endgenerate

	// Native I420 DPB write tap: captures every sample written to the DPB
	// (IDR intra frames during DPB fill; eventually inter reconstruction too).
	wire [31:0] dpb_wr_addr_raw = dut.stub.dpb_mem_waddr;
	wire [31:0] dpb_cur_base    = dut.stub.dpb_current_base;
	assign native_i420_wr_en     = dut.stub.dpb_mem_we;
	assign native_i420_wr_offset = dpb_wr_addr_raw - dpb_cur_base;
	assign native_i420_wr_data   = dut.stub.dpb_mem_wdata;
	assign native_i420_wr_frame  = dut.stub.frames_out;

	// DPB reference pre-fill: inject real decoded IDR samples into the
	// DPB SRAM. The testbench writes to the REFERENCE bank so that MC
	// fetches see real data instead of synthetic patterns.
	always @(posedge clk) begin
		if (dpb_prefill_en)
			dut.stub.dpb_mem[dpb_prefill_addr[17:0]] <= dpb_prefill_data;
	end
endmodule

`default_nettype wire
