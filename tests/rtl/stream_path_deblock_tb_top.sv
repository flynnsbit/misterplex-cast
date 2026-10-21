// Testbench-only integration top. NOT part of Quartus project.
// It drives the real product stream_path.sv from Annex-B bytes and connects the
// product deblock edge pipeline to stream_path-derived slice QP / intra state.
`default_nettype none

module stream_path_deblock_tb (
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
	output wire [7:0]  idr_count,
	output wire [7:0]  sps_count,
	output wire [7:0]  pps_count,
	output wire [7:0]  slice_count,
	output wire [15:0] stub_frames,
	output wire        stub_busy,
	output wire        sps_valid,
	output wire [7:0]  sps_mb_w,
	output wire [7:0]  sps_mb_h,
	output wire        pps_valid,
	output wire        slice_valid,
	output wire        slice_is_i,
	output wire [5:0]  slice_qp,
	output wire [1:0]  disable_deblocking_filter_idc,
	output wire signed [4:0] slice_alpha_c0_offset_div2,
	output wire signed [4:0] slice_beta_offset_div2,
	output wire signed [4:0] slice_alpha_c0_offset,
	output wire signed [4:0] slice_beta_offset,
	output wire        residual_ok,
	output wire [7:0]  residual_csum,
	output wire        residual_place_pulse,
	output wire [7:0]  recon_sig,
	output wire [7:0]  recon_dbg,
	output wire        recon_dbg_valid,
	output wire        recon_valid,

	input  wire        use_stream_qp,
	input  wire        use_stream_intra,
	input  wire        db_valid_i,
	input  wire        db_is_chroma,
	input  wire [2:0]  db_bs_in,
	input  wire [5:0]  db_qp_avg,
	input  wire signed [4:0] db_alpha_off,
	input  wire signed [4:0] db_beta_off,
	input  wire [7:0]  db_p3_in [0:3],
	input  wire [7:0]  db_p2_in [0:3],
	input  wire [7:0]  db_p1_in [0:3],
	input  wire [7:0]  db_p0_in [0:3],
	input  wire [7:0]  db_q0_in [0:3],
	input  wire [7:0]  db_q1_in [0:3],
	input  wire [7:0]  db_q2_in [0:3],
	input  wire [7:0]  db_q3_in [0:3],
	output wire        db_valid_o,
	output wire [7:0]  db_p2_out [0:3],
	output wire [7:0]  db_p1_out [0:3],
	output wire [7:0]  db_p0_out [0:3],
	output wire [7:0]  db_q0_out [0:3],
	output wire [7:0]  db_q1_out [0:3],
	output wire [7:0]  db_q2_out [0:3],
	output wire [7:0]  db_alpha,
	output wire [7:0]  db_beta,
	output wire [5:0]  db_tc0,

	input  wire        bs_disable_all,
	input  wire        bs_slice_boundary_blocked,
	input  wire        bs_mb_boundary,
	input  wire        bs_p_intra,
	input  wire        bs_q_intra,
	input  wire        bs_p_nonzero,
	input  wire        bs_q_nonzero,
	input  wire [1:0]  bs_p_ref,
	input  wire [1:0]  bs_q_ref,
	input  wire signed [11:0] bs_p_mvx,
	input  wire signed [11:0] bs_p_mvy,
	input  wire signed [11:0] bs_q_mvx,
	input  wire signed [11:0] bs_q_mvy,
	output wire [2:0]  bs_derived,
	output wire        bs_unsupported_ref
);
	wire [15:0] fifo_level;
	wire has_idr;
	wire [7:0] sps_profile, sps_level, slice_type, first_mb_type;
	wire [15:0] sps_width, sps_height;
	wire has_mb_type;
	wire first_mb_p_skip;
	wire [7:0] p_skip_run;
	wire [2:0] first_mb_part_mode;
	wire [2:0] first_mb_part_count;
	wire first_mb_uses_sub_mb;
	wire first_mb_intra;
	wire [4:0] residual_tc;
	wire [1:0] residual_t1;
	wire signed [7:0] residual_dc;
	wire signed [15:0] residual_coeff [0:15];
	wire fs_wr_en, fs_wr_reset, fs_swap;
	wire [15:0] fs_wr_pixel;
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

	stream_path #(.FRAME_W(16), .FRAME_H(16)) u_stream (
		.clk(clk), .reset(reset),
		.ioctl_download(ioctl_download), .ioctl_wr(ioctl_wr), .ioctl_dout(ioctl_dout),
		.enable(enable), .flush(flush),
		.ddr_stream_enable(1'b0), .ddr_bus_want(ddr_bus_want), .ddr_busy(1'b1),
		.ddr_burstcnt(ddr_burstcnt), .ddr_addr(ddr_addr), .ddr_dout(64'd0),
		.ddr_dout_ready(1'b0), .ddr_rd(ddr_rd), .ddr_din(ddr_din),
		.ddr_be(ddr_be), .ddr_we(ddr_we),
		.has_stream(has_stream), .nalu_count(nalu_count), .last_nal_type(last_nal_type),
		.bytes_in(bytes_in), .bytes_seen(bytes_seen), .fifo_level(fifo_level),
		.stream_ddr_active(stream_ddr_active), .stream_ddr_bytes_out(stream_ddr_bytes_out),
		.stream_ddr_underruns(stream_ddr_underruns), .stream_ddr_overruns(stream_ddr_overruns),
		.stream_ddr_host_write(stream_ddr_host_write), .stream_ddr_fpga_read(stream_ddr_fpga_read),
		.has_idr(has_idr), .idr_count(idr_count), .sps_count(sps_count), .pps_count(pps_count),
		.slice_count(slice_count), .stub_frames(stub_frames), .stub_busy(stub_busy),
		.sps_valid(sps_valid), .sps_profile(sps_profile), .sps_level(sps_level),
		.sps_width(sps_width), .sps_height(sps_height), .sps_mb_w(sps_mb_w), .sps_mb_h(sps_mb_h),
		.pps_valid(pps_valid), .slice_valid(slice_valid), .slice_type(slice_type),
		.slice_is_i(slice_is_i), .first_mb_type(first_mb_type), .has_mb_type(has_mb_type),
		.first_mb_p_skip(first_mb_p_skip), .p_skip_run(p_skip_run),
		.first_mb_part_mode(first_mb_part_mode), .first_mb_part_count(first_mb_part_count),
		.first_mb_uses_sub_mb(first_mb_uses_sub_mb), .first_mb_intra(first_mb_intra),
		.slice_qp(slice_qp),
		.disable_deblocking_filter_idc(disable_deblocking_filter_idc),
		.slice_alpha_c0_offset_div2(slice_alpha_c0_offset_div2),
		.slice_beta_offset_div2(slice_beta_offset_div2),
		.slice_alpha_c0_offset(slice_alpha_c0_offset),
		.slice_beta_offset(slice_beta_offset),
		.residual_tc(residual_tc), .residual_t1(residual_t1),
		.residual_ok(residual_ok), .residual_dc(residual_dc), .residual_csum(residual_csum),
		.residual_coeff(residual_coeff), .residual_place_pulse(residual_place_pulse),
		.recon_sig(recon_sig), .recon_dbg(recon_dbg), .recon_dbg_valid(recon_dbg_valid),
		.recon_valid(recon_valid),
		.fs_wr_en(fs_wr_en), .fs_wr_pixel(fs_wr_pixel), .fs_wr_reset(fs_wr_reset), .fs_swap(fs_swap)
	);

	wire [5:0] db_qp = use_stream_qp ? slice_qp : db_qp_avg;
	wire p_intra = use_stream_intra ? slice_is_i : bs_p_intra;
	wire q_intra = use_stream_intra ? slice_is_i : bs_q_intra;

	h264_deblock_bs u_bs (
		.disable_all(bs_disable_all), .slice_boundary_blocked(bs_slice_boundary_blocked),
		.mb_boundary(bs_mb_boundary), .p_intra(p_intra), .q_intra(q_intra),
		.p_nonzero(bs_p_nonzero), .q_nonzero(bs_q_nonzero),
		.p_ref(bs_p_ref), .q_ref(bs_q_ref),
		.p_mvx(bs_p_mvx), .p_mvy(bs_p_mvy), .q_mvx(bs_q_mvx), .q_mvy(bs_q_mvy),
		.bs(bs_derived), .unsupported_ref(bs_unsupported_ref)
	);

	h264_deblock_thresholds u_thr (
		.qp_avg(db_qp), .slice_alpha_c0_offset(db_alpha_off), .slice_beta_offset(db_beta_off),
		.bs(db_bs_in), .alpha(db_alpha), .beta(db_beta), .index_a(), .index_b(), .tc0(db_tc0)
	);

	h264_deblock_edge_pipe u_db (
		.clk(clk), .reset(reset), .valid_i(db_valid_i), .is_chroma(db_is_chroma),
		.bs(db_bs_in), .qp_avg(db_qp), .slice_alpha_c0_offset(db_alpha_off),
		.slice_beta_offset(db_beta_off),
		.p3_in(db_p3_in), .p2_in(db_p2_in), .p1_in(db_p1_in), .p0_in(db_p0_in),
		.q0_in(db_q0_in), .q1_in(db_q1_in), .q2_in(db_q2_in), .q3_in(db_q3_in),
		.valid_o(db_valid_o),
		.p2_out(db_p2_out), .p1_out(db_p1_out), .p0_out(db_p0_out),
		.q0_out(db_q0_out), .q1_out(db_q1_out), .q2_out(db_q2_out)
	);
endmodule

`default_nettype wire
