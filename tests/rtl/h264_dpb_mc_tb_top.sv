// Testbench-only top for the product DPB/MC RTL.
`default_nettype none

module h264_dpb_mc_tb #(
	parameter FAULT_BAD_CLAMP = 0,
	parameter FAULT_BAD_MC_ROUND = 0,
	parameter FAULT_EARLY_REF = 0,
	parameter FAULT_BAD_PART_MASK = 0,
	parameter USE_DEBLOCK_WB_CTRL = 0
)(
	input  wire               clk,
	input  wire               reset,
	input  wire               idr_start,
	input  wire               frame_done,
	output wire               ref_ready,
	output wire [31:0]        current_base,
	output wire [31:0]        reference_base,

	input  wire               filtered_sample_valid,
	input  wire [7:0]         filtered_mb_x,
	input  wire [7:0]         filtered_mb_y,
	input  wire [1:0]         filtered_plane,
	input  wire [7:0]         filtered_sample_idx,
	input  wire [7:0]         filtered_sample,
	input  wire               filtered_mb_valid,
	input  wire [10:0]        filtered_mb_addr,
	input  wire               filtered_mb_is_ref,
	input  wire               filtered_frame_done,
	input  wire [1:0]         frame_slot_i,
	input  wire               frame_boundary,
	output wire               deblock_wb_valid,
	output wire [10:0]        deblock_wb_mb_addr,
	output wire               deblock_ref_ready_pulse,
	output wire               deblock_commit_order_error,
	output wire               mem_we,
	output wire [31:0]        mem_waddr,
	output wire [7:0]         mem_wdata,

	input  wire               fetch_start,
	input  wire [7:0]         fetch_mb_x,
	input  wire [7:0]         fetch_mb_y,
	input  wire [2:0]         fetch_part_mode,
	input  wire [1:0]         fetch_part_idx,
	input  wire [4:0]         fetch_part_w,
	input  wire [4:0]         fetch_part_h,
	input  wire signed [15:0] fetch_mv_x_qpel,
	input  wire signed [15:0] fetch_mv_y_qpel,
	output wire               fetch_busy,
	output wire               fetch_done,
	output wire               fetch_error_no_ref,
	output wire [1:0]         luma_frac_x,
	output wire [1:0]         luma_frac_y,
	output wire [2:0]         chroma_frac_x,
	output wire [2:0]         chroma_frac_y,
	output wire signed [15:0] luma_origin_x,
	output wire signed [15:0] luma_origin_y,
	output wire signed [15:0] chroma_origin_x,
	output wire signed [15:0] chroma_origin_y,

	output wire               mem_rd,
	output wire [31:0]        mem_raddr,
	input  wire [7:0]         mem_rdata,
	input  wire               mem_rvalid,
	output wire               luma_window_valid,
	output wire [8:0]         luma_window_idx,
	output wire [7:0]         luma_window_sample,
	output wire               chroma_u_window_valid,
	output wire               chroma_v_window_valid,
	output wire [6:0]         chroma_window_idx,
	output wire [7:0]         chroma_window_sample,

	input  wire [7:0]         luma_ref_win [0:440],
	input  wire [7:0]         chroma_u_ref_win [0:80],
	input  wire [7:0]         chroma_v_ref_win [0:80],
	output wire [7:0]         pred_y [0:255],
	output wire               pred_y_valid [0:255],
	output wire [7:0]         pred_u [0:63],
	output wire               pred_u_valid [0:63],
	output wire [7:0]         pred_v [0:63],
	output wire               pred_v_valid [0:63]
);
	wire ref_ready_good;
	wire luma_window_valid_good;
	wire [8:0] luma_window_idx_good;
	wire [7:0] luma_window_sample_good;
	wire chroma_u_window_valid_good;
	wire chroma_v_window_valid_good;
	wire [6:0] chroma_window_idx_good;
	wire [7:0] chroma_window_sample_good;
	wire [7:0] pred_y_good [0:255];
	wire pred_y_valid_good [0:255];
	wire [7:0] pred_u_good [0:63];
	wire pred_u_valid_good [0:63];
	wire [7:0] pred_v_good [0:63];
	wire pred_v_valid_good [0:63];
	wire dpb_frame_done;

	generate
		if (USE_DEBLOCK_WB_CTRL) begin : gen_deblock_wb
			wire deblock_wb_is_ref;
			wire deblock_dpb_invalidate_refs;
			wire [1:0] deblock_ref_ready_slot;
			h264_deblock_writeback_ctrl #(.MB_COUNT(1170), .FRAME_SLOT_W(2), .SAMPLES_PER_MB(384)) u_deblock_wb (
				.clk(clk), .reset(reset),
				.idr_frame_start(idr_start),
				.filtered_sample_valid(filtered_sample_valid),
				.filtered_mb_valid(filtered_mb_valid),
				.filtered_mb_addr(filtered_mb_addr),
				.filtered_mb_is_ref(filtered_mb_is_ref),
				.filtered_frame_done(filtered_frame_done),
				.frame_slot_i(frame_slot_i),
				.frame_boundary(frame_boundary),
				.wb_valid(deblock_wb_valid),
				.wb_mb_addr(deblock_wb_mb_addr),
				.wb_is_ref(deblock_wb_is_ref),
				.dpb_invalidate_refs(deblock_dpb_invalidate_refs),
				.ref_ready_pulse(deblock_ref_ready_pulse),
				.ref_ready_slot(deblock_ref_ready_slot),
				.commit_order_error(deblock_commit_order_error)
			);
			assign dpb_frame_done = deblock_ref_ready_pulse;
		end else begin : gen_direct_frame_done
			assign deblock_wb_valid = 1'b0;
			assign deblock_wb_mb_addr = 11'd0;
			assign deblock_ref_ready_pulse = 1'b0;
			assign deblock_commit_order_error = 1'b0;
			assign dpb_frame_done = frame_done;
		end
	endgenerate

	// Pin BANK1_BASE to fixture geometry. Product RTL defaults are 1280x720;
	// leaving BANK1 unpinned made cur_base=1382400 and broke promotion checks.
	h264_dpb_one_ref #(
		.FRAME_W(624),
		.FRAME_H(480),
		.BANK1_BASE(624 * 480 * 3 / 2)
	) u_dpb (
		.clk(clk), .reset(reset),
		.idr_start(idr_start), .frame_done(dpb_frame_done),
		.ref_ready(ref_ready_good), .current_base(current_base), .reference_base(reference_base),
		.filtered_sample_valid(filtered_sample_valid),
		.filtered_mb_x(filtered_mb_x), .filtered_mb_y(filtered_mb_y),
		.filtered_plane(filtered_plane), .filtered_sample_idx(filtered_sample_idx),
		.filtered_sample(filtered_sample),
		.mem_we(mem_we), .mem_waddr(mem_waddr), .mem_wdata(mem_wdata),
		.fetch_start(fetch_start),
		.fetch_mb_x(fetch_mb_x), .fetch_mb_y(fetch_mb_y),
		.fetch_part_mode(fetch_part_mode), .fetch_part_idx(fetch_part_idx),
		.fetch_part_w(fetch_part_w), .fetch_part_h(fetch_part_h),
		.fetch_mv_x_qpel(fetch_mv_x_qpel), .fetch_mv_y_qpel(fetch_mv_y_qpel),
		.fetch_busy(fetch_busy), .fetch_done(fetch_done), .fetch_error_no_ref(fetch_error_no_ref),
		.luma_frac_x(luma_frac_x), .luma_frac_y(luma_frac_y),
		.chroma_frac_x(chroma_frac_x), .chroma_frac_y(chroma_frac_y),
		.luma_origin_x(luma_origin_x), .luma_origin_y(luma_origin_y),
		.chroma_origin_x(chroma_origin_x), .chroma_origin_y(chroma_origin_y),
		.mem_rd(mem_rd), .mem_raddr(mem_raddr), .mem_rdata(mem_rdata), .mem_rvalid(mem_rvalid),
		.luma_window_valid(luma_window_valid_good), .luma_window_idx(luma_window_idx_good),
		.luma_window_sample(luma_window_sample_good),
		.chroma_u_window_valid(chroma_u_window_valid_good),
		.chroma_v_window_valid(chroma_v_window_valid_good),
		.chroma_window_idx(chroma_window_idx_good),
		.chroma_window_sample(chroma_window_sample_good)
	);

	h264_inter_mc_part u_mc (
		.luma_ref_win(luma_ref_win),
		.chroma_u_ref_win(chroma_u_ref_win),
		.chroma_v_ref_win(chroma_v_ref_win),
		.luma_frac_x(luma_frac_x), .luma_frac_y(luma_frac_y),
		.chroma_frac_x(chroma_frac_x), .chroma_frac_y(chroma_frac_y),
		.part_w(fetch_part_w), .part_h(fetch_part_h),
		.pred_y(pred_y_good), .pred_y_valid(pred_y_valid_good),
		.pred_u(pred_u_good), .pred_u_valid(pred_u_valid_good),
		.pred_v(pred_v_good), .pred_v_valid(pred_v_valid_good)
	);

	assign ref_ready = FAULT_EARLY_REF ? 1'b1 : ref_ready_good;
	assign luma_window_valid = luma_window_valid_good;
	assign luma_window_idx = luma_window_idx_good;
	// Red twin: corrupt the first window beat. For the mv=(-5,-7) top-left
	// case idx0 is fully edge-clamped (origin-2 → sample (0,0)); a wrong
	// clamp/pipeline that never checks edge samples would miss this.
	assign luma_window_sample = (FAULT_BAD_CLAMP && luma_window_valid_good && luma_window_idx_good == 9'd0) ?
	                            (luma_window_sample_good + 8'd1) : luma_window_sample_good;
	assign chroma_u_window_valid = chroma_u_window_valid_good;
	assign chroma_v_window_valid = chroma_v_window_valid_good;
	assign chroma_window_idx = chroma_window_idx_good;
	assign chroma_window_sample = chroma_window_sample_good;

	genvar i;
	generate
		for (i = 0; i < 256; i = i + 1) begin : gen_pred_y
			assign pred_y[i] = (FAULT_BAD_MC_ROUND && i == 0) ? (pred_y_good[i] + 8'd1) : pred_y_good[i];
			assign pred_y_valid[i] = (FAULT_BAD_PART_MASK && i == 8) ? ~pred_y_valid_good[i] : pred_y_valid_good[i];
		end
		for (i = 0; i < 64; i = i + 1) begin : gen_pred_c
			assign pred_u[i] = pred_u_good[i];
			assign pred_v[i] = pred_v_good[i];
			assign pred_u_valid[i] = pred_u_valid_good[i];
			assign pred_v_valid[i] = pred_v_valid_good[i];
		end
	endgenerate
endmodule

`default_nettype wire
