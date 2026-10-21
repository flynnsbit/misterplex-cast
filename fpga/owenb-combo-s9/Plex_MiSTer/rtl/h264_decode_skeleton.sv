// h264_decode_skeleton — Resource estimation skeleton for Quartus fitting.
//
// PURPOSE: Instantiate every module the product decode datapath requires,
//          wired so Quartus cannot optimise them away, to measure actual
//          FPGA resource consumption on the DE10-Nano (Cyclone V 5CSEBA6U23I7).
//
// THIS IS NOT A DECODER. It does not process data correctly. It exists
// solely to hold area in the fitter so we get a real resource measurement.
//
// ACCEPTANCE CRITERION: Every module in post-fit hierarchy must report
//                       non-zero ALMs. If any shows 0, the skeleton is
//                       lying and the area number is worthless.
//
// ANTI-OPTIMISATION: All module outputs are XOR-reduced into a registered
//                    observation chain that drives a real output port.
//                    Quartus cannot strip logic whose output is consumed.
//
// INSTANTIATION COUNTS: 1 of each (lower bound). Real datapath may require
//                       parallel instances (e.g. 4× IDCT). Update from
//                       w-arch manifest when received.
//
// OWNER: w-rel. CONSUMER: w-cap (Quartus fit), w-arch (interpret results).

`default_nettype none

module h264_decode_skeleton #(
    parameter int FRAME_W  = 1280,  // 80 MB columns (720p content)
    parameter int FRAME_H  = 720,   // 45 MB rows
    parameter int MB_W     = (FRAME_W + 15) / 16,
    parameter int MB_H     = (FRAME_H + 15) / 16,
    parameter int MB_COUNT = MB_W * MB_H,
    // Line buffer sizing (w-ctl: 2-4 M10K of 146 free)
    parameter int NC_LINE_DEPTH    = MB_W * 4,  // nC: 4 per MB, one row
    parameter int RECON_LINE_DEPTH = MB_W * 16  // recon top row: 16 px per MB
)(
    input  wire        clk,
    input  wire        reset,

    // Stimulus inputs — prevent constant-propagation optimisation
    input  wire [7:0]  stim_byte,
    input  wire [5:0]  stim_qp,
    input  wire [3:0]  stim_mode,
    input  wire [15:0] stim_mv_x,
    input  wire [15:0] stim_mv_y,
    input  wire        stim_valid,

    // Observable output — must drive a real port or mailbox
    output reg  [7:0]  skeleton_observe
);

    // ════════════════════════════════════════════════════════════════════
    // XOR OBSERVATION CHAIN
    // ════════════════════════════════════════════════════════════════════
    function automatic [7:0] xor16x8(input logic [7:0] a [0:15]);
        xor16x8 = a[0] ^ a[1] ^ a[2] ^ a[3] ^ a[4] ^ a[5] ^ a[6] ^ a[7] ^
                  a[8] ^ a[9] ^ a[10] ^ a[11] ^ a[12] ^ a[13] ^ a[14] ^ a[15];
    endfunction

    function automatic [7:0] xor4x8(input logic [7:0] a [0:3]);
        xor4x8 = a[0] ^ a[1] ^ a[2] ^ a[3];
    endfunction

    // ════════════════════════════════════════════════════════════════════
    // 1. CAVLC RESIDUAL BLOCK — sequential bitstream parser (w-level)
    // ════════════════════════════════════════════════════════════════════
    wire [7:0] cavlc_rbsp [0:63];
    genvar ci;
    generate
        for (ci = 0; ci < 64; ci = ci + 1) begin : gen_cavlc_rbsp
            assign cavlc_rbsp[ci] = stim_byte ^ ci[7:0];
        end
    endgenerate

    wire        cavlc_busy, cavlc_done, cavlc_ok;
    wire [9:0]  cavlc_bit_end;
    wire [4:0]  cavlc_tc;
    wire [1:0]  cavlc_t1;
    wire [3:0]  cavlc_tz;
    wire signed [15:0] cavlc_coeff [0:15];
    wire signed [15:0] cavlc_level_dbg [0:15];
    wire [3:0]  cavlc_run_dbg [0:15];

    h264_cavlc_residual_block #(.MAX_BYTES(64)) u_cavlc (
        .clk(clk), .reset(reset),
        .start(stim_valid),
        .coeff_token_table(stim_mode[2:0]),
        .max_coeff(5'd16),
        .bit_offset_start({4'd0, stim_qp}),
        .bit_len(10'd400),
        .rbsp(cavlc_rbsp),
        .busy(cavlc_busy), .done(cavlc_done), .ok(cavlc_ok),
        .bit_offset_end(cavlc_bit_end),
        .total_coeff(cavlc_tc), .trailing_ones(cavlc_t1), .total_zeros(cavlc_tz),
        .coeff(cavlc_coeff), .level_dbg(cavlc_level_dbg), .run_dbg(cavlc_run_dbg)
    );

    wire [7:0] xor_cavlc;
    assign xor_cavlc = cavlc_coeff[0][7:0] ^ cavlc_coeff[7][7:0] ^ cavlc_coeff[15][7:0] ^
                        {3'd0, cavlc_tc} ^ {6'd0, cavlc_t1} ^
                        cavlc_bit_end[7:0] ^ {cavlc_busy, cavlc_done, cavlc_ok, 5'd0};

    // ════════════════════════════════════════════════════════════════════
    // 2. DEQUANT + IDCT + RECON — arithmetic pipeline (w-cabac)
    // ════════════════════════════════════════════════════════════════════
    wire signed [8:0] dq_coeff [0:15];
    genvar di;
    generate
        for (di = 0; di < 16; di = di + 1) begin : gen_dq_input
            assign dq_coeff[di] = cavlc_coeff[di][8:0];
        end
    endgenerate

    wire signed [17:0] dq_out [0:15];
    h264_dequant4x4 u_dequant (
        .coeff(dq_coeff),
        .qp(stim_qp),
        .max_coeff(5'd16),
        .dequant(dq_out)
    );

    wire signed [17:0] idct_out [0:15];
    h264_idct4x4 u_idct (
        .dequant(dq_out),
        .residual(idct_out)
    );

    // Prediction source (muxed from intra predictors below)
    wire [7:0] recon_pred [0:15];
    wire [7:0] recon_out [0:15];
    h264_recon4x4 u_recon (
        .pred(recon_pred),
        .residual(idct_out),
        .recon(recon_out)
    );

    wire [7:0] xor_arith = xor16x8(recon_out) ^ dq_out[0][7:0] ^ idct_out[8][7:0];

    // ════════════════════════════════════════════════════════════════════
    // 3. INTRA PREDICTION — all three predictor modules (w-plane)
    // ════════════════════════════════════════════════════════════════════
    wire [7:0] nb_above_16 [0:15];
    wire [7:0] nb_left_16 [0:15];
    wire [7:0] nb_above_8 [0:7];
    wire [7:0] nb_left_8 [0:7];
    wire [7:0] nb_above_4 [0:7]; // 4 above + 4 above-right
    wire [7:0] nb_left_4 [0:3];
    wire [7:0] nb_top_left;

    // Intra 4×4
    wire [3:0] i4_used_mode;
    wire [7:0] i4_pred [0:15];
    h264_intra4x4_pred u_intra4x4 (
        .mode(stim_mode),
        .above(nb_above_4),
        .left(nb_left_4),
        .top_left(nb_top_left),
        .has_above(stim_valid),
        .has_left(stim_valid),
        .used_mode(i4_used_mode),
        .pred(i4_pred)
    );

    // Intra 16×16
    wire       i16_unsupported;
    wire [7:0] i16_pred [0:255];
    h264_intra16x16_pred u_intra16x16 (
        .mode(stim_mode[1:0]),
        .above(nb_above_16),
        .left(nb_left_16),
        .top_left(nb_top_left),
        .has_above(stim_valid),
        .has_left(stim_valid),
        .unsupported(i16_unsupported),
        .pred(i16_pred)
    );

    // Chroma 8×8
    wire [7:0] chroma_pred [0:63];
    h264_chroma8x8_pred u_chroma_pred (
        .mode(stim_mode[1:0]),
        .above(nb_above_8),
        .left(nb_left_8),
        .top_left(nb_top_left),
        .has_above(stim_valid),
        .has_left(stim_valid),
        .pred(chroma_pred)
    );

    // Intra mode guard (sequential)
    wire guard_unsup_valid, guard_unsup_seen;
    wire [3:0] guard_unsup_code;
    wire [15:0] guard_unsup_mb;
    wire [4:0]  guard_unsup_block;
    h264_intra_mode_guard u_mode_guard (
        .clk(clk), .reset(reset),
        .mb_valid(stim_valid),
        .mb_type(stim_byte),
        .i16_pred_mode(stim_mode[1:0]),
        .mb_index({8'd0, stim_byte}),
        .block_index(stim_qp[4:0]),
        .unsupported_valid(guard_unsup_valid),
        .unsupported_seen(guard_unsup_seen),
        .unsupported_code(guard_unsup_code),
        .unsupported_mb(guard_unsup_mb),
        .unsupported_block(guard_unsup_block)
    );

    // Mux prediction to recon (stim_mode[3] selects I16 vs I4 — prevents opt)
    genvar pi;
    generate
        for (pi = 0; pi < 16; pi = pi + 1) begin : gen_pred_mux
            assign recon_pred[pi] = stim_mode[3] ? i16_pred[pi] : i4_pred[pi];
        end
    endgenerate

    wire [7:0] xor_intra = xor16x8(i4_pred) ^ i16_pred[0] ^ i16_pred[128] ^ i16_pred[255] ^
                            chroma_pred[0] ^ chroma_pred[32] ^ chroma_pred[63] ^
                            {4'd0, i4_used_mode} ^ {7'd0, i16_unsupported} ^
                            {7'd0, guard_unsup_seen} ^ guard_unsup_code[3:0] ^
                            guard_unsup_mb[7:0];

    // ════════════════════════════════════════════════════════════════════
    // 4. MOTION COMPENSATION — per-sample interpolation (w-mc)
    //    h264_luma_qpel_sample: 81-tap 6-tap FIR (THE largest combinational module)
    //    h264_chroma_epel_sample: bilinear 4-tap
    //    Real datapath iterates over a 16×16/8×8 block; here we instantiate
    //    one of each to measure per-sample cost. Scale by iteration count.
    // ════════════════════════════════════════════════════════════════════
    wire [7:0] mc_luma_ref [0:80];
    genvar mi;
    generate
        for (mi = 0; mi < 81; mi = mi + 1) begin : gen_mc_luma_ref
            assign mc_luma_ref[mi] = stim_byte ^ mi[7:0];
        end
    endgenerate

    wire [7:0] mc_luma_sample;
    h264_luma_qpel_sample u_luma_mc (
        .ref_pix(mc_luma_ref),
        .frac_x(stim_mv_x[1:0]),
        .frac_y(stim_mv_y[1:0]),
        .sample(mc_luma_sample)
    );

    wire [7:0] mc_chroma_sample;
    h264_chroma_epel_sample u_chroma_mc (
        .p00(stim_byte),
        .p10(stim_byte ^ 8'h11),
        .p01(stim_byte ^ 8'h22),
        .p11(stim_byte ^ 8'h33),
        .frac_x(stim_mv_x[2:0]),
        .frac_y(stim_mv_y[2:0]),
        .sample(mc_chroma_sample)
    );

    wire [7:0] xor_mc = mc_luma_sample ^ mc_chroma_sample;

    // ════════════════════════════════════════════════════════════════════
    // 5. MV PREDICTION — 16×16 and partition-level (w-mc)
    // ════════════════════════════════════════════════════════════════════
    wire signed [15:0] mvp_pred_x, mvp_pred_y, mvp_mv_x, mvp_mv_y;
    wire               mvp_skip_zero;
    h264_mv_pred_16x16 u_mv_pred_16x16 (
        .avail_a(stim_valid), .avail_b(stim_valid),
        .avail_c(stim_mode[0]), .avail_d(stim_mode[1]),
        .mv_a_x($signed(stim_mv_x)), .mv_a_y($signed(stim_mv_y)),
        .mv_b_x($signed(stim_mv_x) + 16'sd4), .mv_b_y($signed(stim_mv_y) - 16'sd2),
        .mv_c_x($signed(stim_mv_x) - 16'sd8), .mv_c_y($signed(stim_mv_y) + 16'sd6),
        .mv_d_x(16'sd0), .mv_d_y(16'sd0),
        .mvd_x($signed(stim_mv_x[15:0])),
        .mvd_y($signed(stim_mv_y[15:0])),
        .p_skip(stim_mode[2]),
        .pred_x(mvp_pred_x), .pred_y(mvp_pred_y),
        .mv_x(mvp_mv_x), .mv_y(mvp_mv_y),
        .skip_zero(mvp_skip_zero)
    );

    wire signed [15:0] mvp_part_pred_x, mvp_part_pred_y, mvp_part_mv_x, mvp_part_mv_y;
    wire               mvp_part_skip;
    h264_mv_pred_part u_mv_pred_part (
        .part_mode(stim_mode[2:0]),
        .part_idx(stim_mode[1:0]),
        .avail_a(stim_valid), .avail_b(stim_valid),
        .avail_c(stim_mode[0]), .avail_d(stim_mode[1]),
        .mv_a_x($signed(stim_mv_x)), .mv_a_y($signed(stim_mv_y)),
        .mv_b_x($signed(stim_mv_x) + 16'sd2), .mv_b_y($signed(stim_mv_y) - 16'sd1),
        .mv_c_x($signed(stim_mv_x) - 16'sd3), .mv_c_y($signed(stim_mv_y) + 16'sd5),
        .mv_d_x(16'sd0), .mv_d_y(16'sd0),
        .mvd_x($signed(stim_mv_x)),
        .mvd_y($signed(stim_mv_y)),
        .p_skip(stim_mode[3]),
        .pred_x(mvp_part_pred_x), .pred_y(mvp_part_pred_y),
        .mv_x(mvp_part_mv_x), .mv_y(mvp_part_mv_y),
        .skip_zero(mvp_part_skip)
    );

    wire [7:0] xor_mv = mvp_mv_x[7:0] ^ mvp_mv_y[7:0] ^ mvp_pred_x[7:0] ^
                         mvp_part_mv_x[7:0] ^ mvp_part_mv_y[7:0] ^
                         {7'd0, mvp_skip_zero} ^ {7'd0, mvp_part_skip};

    // ════════════════════════════════════════════════════════════════════
    // 6. DPB — reference frame management + MC fetch (w-dpb/w-rel)
    // ════════════════════════════════════════════════════════════════════
    localparam int DPB_Y_SIZE = FRAME_W * FRAME_H;
    localparam int DPB_C_SIZE = (FRAME_W/2) * (FRAME_H/2);
    localparam int DPB_FRAME_BYTES = DPB_Y_SIZE + 2 * DPB_C_SIZE;

    // DPB local memory — infers block RAM (this IS the resource we measure)
    (* ram_style = "block" *) reg [7:0] dpb_mem [0:2*DPB_FRAME_BYTES-1];
    reg [7:0]  dpb_rdata;
    reg        dpb_rvalid;
    wire       dpb_mem_we;
    wire [31:0] dpb_mem_waddr;
    wire [7:0]  dpb_mem_wdata;
    wire       dpb_mem_rd;
    wire [31:0] dpb_mem_raddr;

    always @(posedge clk) begin
        if (dpb_mem_we && dpb_mem_waddr < (2*DPB_FRAME_BYTES))
            dpb_mem[dpb_mem_waddr[19:0]] <= dpb_mem_wdata;
        dpb_rdata  <= dpb_mem[dpb_mem_raddr[19:0]];
        dpb_rvalid <= dpb_mem_rd;
    end

    wire        dpb_ref_ready;
    wire [31:0] dpb_current_base, dpb_reference_base;
    wire        dpb_fetch_busy, dpb_fetch_done, dpb_fetch_error;
    wire [1:0]  dpb_luma_frac_x, dpb_luma_frac_y;
    wire [2:0]  dpb_chroma_frac_x, dpb_chroma_frac_y;
    wire signed [15:0] dpb_luma_ox, dpb_luma_oy, dpb_chroma_ox, dpb_chroma_oy;
    wire        dpb_luma_win_valid;
    wire [8:0]  dpb_luma_win_idx;
    wire [7:0]  dpb_luma_win_sample;
    wire        dpb_chroma_u_win_valid, dpb_chroma_v_win_valid;
    wire [6:0]  dpb_chroma_win_idx;
    wire [7:0]  dpb_chroma_win_sample;

    h264_dpb_one_ref #(
        .FRAME_W(FRAME_W), .FRAME_H(FRAME_H),
        .BANK0_BASE(0), .BANK1_BASE(DPB_FRAME_BYTES)
    ) u_dpb (
        .clk(clk), .reset(reset),
        .idr_start(stim_valid && stim_mode == 4'd15),
        .frame_done(stim_valid && stim_byte[7]),
        .ref_ready(dpb_ref_ready),
        .current_base(dpb_current_base),
        .reference_base(dpb_reference_base),
        .filtered_sample_valid(stim_valid),
        .filtered_mb_x(stim_byte),
        .filtered_mb_y(stim_byte),
        .filtered_plane(stim_mode[1:0]),
        .filtered_sample_idx(stim_byte),
        .filtered_sample(stim_byte),
        .mem_we(dpb_mem_we), .mem_waddr(dpb_mem_waddr), .mem_wdata(dpb_mem_wdata),
        .fetch_start(stim_valid && stim_mode[0]),
        .fetch_mb_x(stim_byte),
        .fetch_mb_y(stim_byte),
        .fetch_part_mode(stim_mode[2:0]),
        .fetch_part_idx(stim_mode[1:0]),
        .fetch_part_w(5'd16),
        .fetch_part_h(5'd16),
        .fetch_mv_x_qpel($signed(stim_mv_x)),
        .fetch_mv_y_qpel($signed(stim_mv_y)),
        .fetch_busy(dpb_fetch_busy),
        .fetch_done(dpb_fetch_done),
        .fetch_error_no_ref(dpb_fetch_error),
        .luma_frac_x(dpb_luma_frac_x),
        .luma_frac_y(dpb_luma_frac_y),
        .chroma_frac_x(dpb_chroma_frac_x),
        .chroma_frac_y(dpb_chroma_frac_y),
        .luma_origin_x(dpb_luma_ox),
        .luma_origin_y(dpb_luma_oy),
        .chroma_origin_x(dpb_chroma_ox),
        .chroma_origin_y(dpb_chroma_oy),
        .mem_rd(dpb_mem_rd), .mem_raddr(dpb_mem_raddr),
        .mem_rdata(dpb_rdata), .mem_rvalid(dpb_rvalid),
        .luma_window_valid(dpb_luma_win_valid),
        .luma_window_idx(dpb_luma_win_idx),
        .luma_window_sample(dpb_luma_win_sample),
        .chroma_u_window_valid(dpb_chroma_u_win_valid),
        .chroma_v_window_valid(dpb_chroma_v_win_valid),
        .chroma_window_idx(dpb_chroma_win_idx),
        .chroma_window_sample(dpb_chroma_win_sample)
    );

    wire [7:0] xor_dpb = dpb_mem_wdata ^ dpb_rdata ^ dpb_luma_win_sample ^
                          dpb_chroma_win_sample ^ dpb_current_base[7:0] ^
                          {dpb_ref_ready, dpb_fetch_busy, dpb_fetch_done, dpb_fetch_error,
                           dpb_luma_frac_x, dpb_luma_frac_y};

    // ════════════════════════════════════════════════════════════════════
    // 7. DEBLOCKING FILTER — BS calculator + pipelined edge filter (w-deblock)
    // ════════════════════════════════════════════════════════════════════
    wire [2:0] deblock_bs_out;
    wire       deblock_unsup_ref;
    h264_deblock_bs u_deblock_bs (
        .disable_all(1'b0),
        .slice_boundary_blocked(1'b0),
        .mb_boundary(stim_byte[1]),
        .p_intra(stim_mode[0]),
        .q_intra(stim_mode[1]),
        .p_nonzero(stim_mode[2]),
        .q_nonzero(stim_mode[3]),
        .p_ref(stim_mode[1:0]),
        .q_ref(stim_mode[3:2]),
        .p_mvx($signed(stim_mv_x[11:0])),
        .p_mvy($signed(stim_mv_y[11:0])),
        .q_mvx($signed(stim_mv_x[11:0]) + 12'sd5),
        .q_mvy($signed(stim_mv_y[11:0]) - 12'sd3),
        .bs(deblock_bs_out),
        .unsupported_ref(deblock_unsup_ref)
    );

    // Deblock edge pipe — the core filtering unit
    wire [7:0] db_p3 [0:3], db_p2 [0:3], db_p1 [0:3], db_p0 [0:3];
    wire [7:0] db_q0 [0:3], db_q1 [0:3], db_q2 [0:3], db_q3 [0:3];
    genvar dbi;
    generate
        for (dbi = 0; dbi < 4; dbi = dbi + 1) begin : gen_db_in
            assign db_p3[dbi] = stim_byte ^ dbi[7:0] ^ 8'h10;
            assign db_p2[dbi] = stim_byte ^ dbi[7:0] ^ 8'h20;
            assign db_p1[dbi] = stim_byte ^ dbi[7:0] ^ 8'h30;
            assign db_p0[dbi] = stim_byte ^ dbi[7:0] ^ 8'h40;
            assign db_q0[dbi] = stim_byte ^ dbi[7:0] ^ 8'h50;
            assign db_q1[dbi] = stim_byte ^ dbi[7:0] ^ 8'h60;
            assign db_q2[dbi] = stim_byte ^ dbi[7:0] ^ 8'h70;
            assign db_q3[dbi] = stim_byte ^ dbi[7:0] ^ 8'h80;
        end
    endgenerate

    wire        db_pipe_valid;
    wire [7:0]  db_p2_out [0:3], db_p1_out [0:3], db_p0_out [0:3];
    wire [7:0]  db_q0_out [0:3], db_q1_out [0:3], db_q2_out [0:3];

    h264_deblock_edge_pipe u_deblock_pipe (
        .clk(clk), .reset(reset),
        .valid_i(stim_valid),
        .is_chroma(stim_mode[0]),
        .bs(deblock_bs_out),
        .qp_avg(stim_qp),
        .slice_alpha_c0_offset(5'sd0),
        .slice_beta_offset(5'sd0),
        .p3_in(db_p3), .p2_in(db_p2), .p1_in(db_p1), .p0_in(db_p0),
        .q0_in(db_q0), .q1_in(db_q1), .q2_in(db_q2), .q3_in(db_q3),
        .valid_o(db_pipe_valid),
        .p2_out(db_p2_out), .p1_out(db_p1_out), .p0_out(db_p0_out),
        .q0_out(db_q0_out), .q1_out(db_q1_out), .q2_out(db_q2_out)
    );

    // Deblock writeback controller
    localparam int MB_AW = (MB_COUNT <= 1) ? 1 : $clog2(MB_COUNT);
    wire                wb_valid;
    wire [MB_AW-1:0]    wb_mb_addr;
    wire                wb_is_ref;
    wire                dpb_invalidate;
    wire                ref_pulse;
    wire [1:0]          ref_slot;
    wire                commit_error;

    h264_deblock_writeback_ctrl #(
        .MB_COUNT(MB_COUNT),
        .FRAME_SLOT_W(2),
        .SAMPLES_PER_MB(384)
    ) u_deblock_wb (
        .clk(clk), .reset(reset),
        .idr_frame_start(stim_valid && stim_mode == 4'd5),
        .filtered_sample_valid(stim_valid),
        .filtered_mb_valid(stim_valid && stim_byte[0]),
        .filtered_mb_addr(stim_byte[MB_AW-1:0]),
        .filtered_mb_is_ref(1'b1),
        .filtered_frame_done(stim_valid && stim_byte[7]),
        .frame_slot_i(2'd0),
        .frame_boundary(stim_valid && stim_byte[6]),
        .wb_valid(wb_valid),
        .wb_mb_addr(wb_mb_addr),
        .wb_is_ref(wb_is_ref),
        .dpb_invalidate_refs(dpb_invalidate),
        .ref_ready_pulse(ref_pulse),
        .ref_ready_slot(ref_slot),
        .commit_order_error(commit_error)
    );

    wire [7:0] xor_deblock = xor4x8(db_p0_out) ^ xor4x8(db_q0_out) ^
                              xor4x8(db_p1_out) ^ xor4x8(db_q1_out) ^
                              {5'd0, deblock_bs_out} ^ {7'd0, db_pipe_valid} ^
                              {7'd0, wb_valid} ^ wb_mb_addr[7:0] ^
                              {6'd0, commit_error, deblock_unsup_ref};

    // ════════════════════════════════════════════════════════════════════
    // 8. P-SLICE MB TYPE DECODE (w-level)
    // ════════════════════════════════════════════════════════════════════
    wire        pmb_is_skip, pmb_is_inter, pmb_is_intra, pmb_uses_sub, pmb_ref0_only, pmb_unsup;
    wire [2:0]  pmb_part_mode, pmb_part_count, pmb_sub_count;
    wire [4:0]  pmb_part_w, pmb_part_h;
    wire [3:0]  pmb_sub_w, pmb_sub_h;

    h264_p_mb_type_decode u_p_mb_type (
        .skipped(stim_mode[3]),
        .mb_type({2'd0, stim_mode}),
        .sub_mb_type(stim_mode[1:0]),
        .sub_mb_valid(stim_valid),
        .is_p_skip(pmb_is_skip),
        .is_inter(pmb_is_inter),
        .is_intra(pmb_is_intra),
        .uses_sub_mb(pmb_uses_sub),
        .ref0_only(pmb_ref0_only),
        .unsupported(pmb_unsup),
        .part_mode(pmb_part_mode),
        .mb_part_count(pmb_part_count),
        .mb_part_w(pmb_part_w),
        .mb_part_h(pmb_part_h),
        .sub_part_count(pmb_sub_count),
        .sub_part_w(pmb_sub_w),
        .sub_part_h(pmb_sub_h)
    );

    wire [7:0] xor_pmb = {pmb_is_skip, pmb_is_inter, pmb_is_intra, pmb_uses_sub,
                           pmb_ref0_only, pmb_part_mode} ^ pmb_part_w[4:0] ^
                          {3'd0, pmb_part_h[4:0]} ^ {4'd0, pmb_sub_w};

    // ════════════════════════════════════════════════════════════════════
    // 9. nC PREDICTOR — combinational context derivation (w-level)
    // ════════════════════════════════════════════════════════════════════
    wire nc_nA_avail, nc_nB_avail;
    wire [4:0] nc_nC;
    wire [2:0] nc_table;

    h264_cavlc_nc_predictor u_nc_pred (
        .mb_x(stim_byte),
        .mb_y(stim_byte),
        .mb_index({8'd0, stim_byte}),
        .mb_width(MB_W[7:0]),
        .first_mb_in_slice(16'd0),
        .block_x(stim_mode[1:0]),
        .block_y(stim_mode[3:2]),
        .left_tc_valid(stim_valid),
        .left_tc(stim_qp[4:0]),
        .up_tc_valid(stim_valid),
        .up_tc(stim_qp[4:0]),
        .nA_available(nc_nA_avail),
        .nB_available(nc_nB_avail),
        .nC(nc_nC),
        .coeff_token_table(nc_table)
    );

    wire [7:0] xor_nc = {nc_nA_avail, nc_nB_avail, nc_table, nc_nC[2:0]};

    // ════════════════════════════════════════════════════════════════════
    // 10. NEIGHBOUR CONTEXT LINE BUFFERS (w-ctl) — M10K usage
    // ════════════════════════════════════════════════════════════════════
    // nC line buffer: 4×5b per MB column
    (* ram_style = "block" *) reg [4:0] nc_line [0:NC_LINE_DEPTH-1];
    reg [4:0] nc_rd_data;
    reg [$clog2(NC_LINE_DEPTH)-1:0] nc_addr;

    always @(posedge clk) begin
        if (stim_valid)
            nc_line[nc_addr] <= cavlc_tc;
        nc_rd_data <= nc_line[nc_addr];
        nc_addr <= stim_byte[$clog2(NC_LINE_DEPTH)-1:0];
    end

    // Reconstructed-sample line buffers: Y/U/V top row
    (* ram_style = "block" *) reg [7:0] recon_line_y [0:RECON_LINE_DEPTH-1];
    (* ram_style = "block" *) reg [7:0] recon_line_u [0:(RECON_LINE_DEPTH/2)-1];
    (* ram_style = "block" *) reg [7:0] recon_line_v [0:(RECON_LINE_DEPTH/2)-1];
    reg [7:0] recon_rd_y, recon_rd_u, recon_rd_v;
    reg [$clog2(RECON_LINE_DEPTH)-1:0] recon_addr;

    always @(posedge clk) begin
        if (stim_valid) begin
            recon_line_y[recon_addr] <= recon_out[0];
            recon_line_u[recon_addr[$clog2(RECON_LINE_DEPTH)-1:1]] <= recon_out[1];
            recon_line_v[recon_addr[$clog2(RECON_LINE_DEPTH)-1:1]] <= recon_out[2];
        end
        recon_rd_y <= recon_line_y[recon_addr];
        recon_rd_u <= recon_line_u[recon_addr[$clog2(RECON_LINE_DEPTH)-1:1]];
        recon_rd_v <= recon_line_v[recon_addr[$clog2(RECON_LINE_DEPTH)-1:1]];
        recon_addr <= {stim_byte, stim_qp[$clog2(RECON_LINE_DEPTH)-9:0]};
    end

    // Wire line buffer outputs to neighbour inputs (prevents dead-code opt)
    genvar ni;
    generate
        for (ni = 0; ni < 16; ni = ni + 1) begin : gen_nb16
            assign nb_above_16[ni] = recon_rd_y ^ ni[7:0];
            assign nb_left_16[ni]  = stim_byte ^ ni[7:0] ^ 8'hCC;
        end
        for (ni = 0; ni < 8; ni = ni + 1) begin : gen_nb8
            assign nb_above_8[ni] = recon_rd_u ^ ni[7:0];
            assign nb_left_8[ni]  = stim_byte ^ ni[7:0] ^ 8'hDD;
        end
        for (ni = 0; ni < 8; ni = ni + 1) begin : gen_nb4a
            assign nb_above_4[ni] = recon_rd_y ^ ni[7:0] ^ 8'hEE;
        end
        for (ni = 0; ni < 4; ni = ni + 1) begin : gen_nb4l
            assign nb_left_4[ni] = stim_byte ^ ni[7:0] ^ 8'hFF;
        end
    endgenerate
    assign nb_top_left = recon_rd_y ^ stim_byte;

    wire [7:0] xor_linebuf = recon_rd_y ^ recon_rd_u ^ recon_rd_v ^ {3'd0, nc_rd_data};

    // ════════════════════════════════════════════════════════════════════
    // 11. CHROMA DC HADAMARD + QP — STUBS (w-plane / w-qp)
    //     Real modules on feat/plane-intra and feat/qp-range. These stubs
    //     use representative computation depth to approximate area cost.
    // ════════════════════════════════════════════════════════════════════
    // Chroma DC 2×2 inverse Hadamard: add/sub + scale. ~6 LUT levels.
    wire signed [17:0] chroma_dc_out [0:3];
    genvar cdi;
    generate
        for (cdi = 0; cdi < 4; cdi = cdi + 1) begin : gen_chroma_dc
            wire signed [15:0] c_in = cavlc_coeff[cdi];
            wire signed [17:0] c_scale = c_in * $signed({1'b0, stim_qp[2:0]});
            assign chroma_dc_out[cdi] = c_scale + $signed({10'd0, stim_byte});
        end
    endgenerate

    // Luma DC 4×4 Hadamard (not yet built — resource stub)
    wire signed [17:0] luma_dc_out [0:15];
    genvar ldi;
    generate
        for (ldi = 0; ldi < 16; ldi = ldi + 1) begin : gen_luma_dc
            wire signed [15:0] l_in = cavlc_coeff[ldi];
            wire signed [17:0] l_sum = l_in + cavlc_coeff[(ldi+1) & 4'hF];
            wire signed [17:0] l_diff = l_in - cavlc_coeff[(ldi+2) & 4'hF];
            assign luma_dc_out[ldi] = (l_sum + l_diff) * $signed({1'b0, stim_qp[1:0]});
        end
    endgenerate

    // Chroma QP mapping: ROM lookup ~54×6b
    reg [5:0] chroma_qp_mapped;
    always @(posedge clk) begin
        if (stim_qp < 6'd30)
            chroma_qp_mapped <= stim_qp;
        else
            chroma_qp_mapped <= 6'd29 + ((stim_qp - 6'd30) >> 1);
    end

    wire [7:0] xor_chroma = chroma_dc_out[0][7:0] ^ chroma_dc_out[1][7:0] ^
                             chroma_dc_out[2][7:0] ^ chroma_dc_out[3][7:0] ^
                             luma_dc_out[0][7:0] ^ luma_dc_out[8][7:0] ^
                             {2'd0, chroma_qp_mapped};

    // ════════════════════════════════════════════════════════════════════
    // 12. REF TAP ADDRESS GENERATION (h264_luma_ref_tap_addr + clamp)
    // ════════════════════════════════════════════════════════════════════
    wire [15:0] tap_x, tap_y;
    h264_luma_ref_tap_addr u_tap_addr (
        .base_x($signed(stim_mv_x)),
        .base_y($signed(stim_mv_y)),
        .tap_idx(stim_byte[6:0]),
        .width(FRAME_W[15:0]),
        .height(FRAME_H[15:0]),
        .tap_x(tap_x),
        .tap_y(tap_y)
    );

    wire [7:0] xor_tap = tap_x[7:0] ^ tap_y[7:0];

    // ════════════════════════════════════════════════════════════════════
    // 13. EXP-GOLOMB READER — ue()/se() syntax element decoding (w-level)
    // ════════════════════════════════════════════════════════════════════
    wire        eg_busy, eg_done, eg_ok, eg_bit_ready;
    wire [31:0] eg_ue;
    wire signed [31:0] eg_se;
    wire [7:0]  eg_bits;

    h264_exp_golomb_reader #(.MAX_LEADING_ZERO(24)) u_exp_golomb (
        .clk(clk), .reset(reset),
        .start(stim_valid),
        .signed_mode(stim_mode[0]),
        .bit_valid(stim_valid),
        .bit_value(stim_byte[0]),
        .bit_ready(eg_bit_ready),
        .busy(eg_busy), .done(eg_done), .ok(eg_ok),
        .ue_value(eg_ue), .se_value(eg_se),
        .bits_consumed(eg_bits)
    );

    wire [7:0] xor_eg = eg_ue[7:0] ^ eg_se[7:0] ^ eg_bits ^
                         {eg_busy, eg_done, eg_ok, eg_bit_ready, 4'd0};

    // ════════════════════════════════════════════════════════════════════
    // 14. BITSTREAM FIFO — single-clock BRAM ring (32KB, M10K)
    //     Real datapath uses async_fifo for CDC (clk_sys → clk_decode).
    //     This measures the BRAM cost; CDC logic is negligible by comparison.
    // ════════════════════════════════════════════════════════════════════
    wire        bs_full, bs_empty, bs_has_data;
    wire [7:0]  bs_rd_data;
    wire [15:0] bs_wr_level;

    bitstream_fifo #(.DEPTH(32768)) u_bitstream_fifo (
        .clk(clk), .reset(reset),
        .wr_en(stim_valid),
        .wr_data(stim_byte),
        .wr_flush(1'b0),
        .wr_full(bs_full),
        .wr_level(bs_wr_level),
        .rd_en(stim_valid & stim_mode[1]),
        .rd_data(bs_rd_data),
        .rd_empty(bs_empty),
        .has_data(bs_has_data)
    );

    wire [7:0] xor_fifo = bs_rd_data ^ bs_wr_level[7:0] ^
                           {bs_full, bs_empty, bs_has_data, 5'd0};

    // ════════════════════════════════════════════════════════════════════
    // 15. REFERENCE WINDOW BUFFER — SRAM for MC reference samples
    //     21×21 luma (441 bytes) + 9×9×2 chroma (162 bytes) = 603 bytes
    //     Infers 1 M10K block (up to 1024×8).
    // ════════════════════════════════════════════════════════════════════
    localparam int REF_WIN_LUMA_SIZE  = 21 * 21;  // 441
    localparam int REF_WIN_CHROMA_SIZE = 9 * 9;   // 81 per plane
    localparam int REF_WIN_TOTAL = REF_WIN_LUMA_SIZE + 2 * REF_WIN_CHROMA_SIZE; // 603

    (* ram_style = "block" *) reg [7:0] ref_win_mem [0:REF_WIN_TOTAL-1];
    reg [7:0]  ref_win_rd;
    reg [9:0]  ref_win_addr;

    always @(posedge clk) begin
        if (stim_valid)
            ref_win_mem[ref_win_addr] <= stim_byte;
        ref_win_rd   <= ref_win_mem[ref_win_addr];
        ref_win_addr <= {stim_mode, stim_qp};
    end

    wire [7:0] xor_refwin = ref_win_rd;

    // ════════════════════════════════════════════════════════════════════
    // 16. NEIGHBOUR CONTEXT LEFT — register bank (w-ctl)
    //     Left-column context: 4 nC values + 16 recon samples + MB flags.
    //     Small enough for registers (no M10K needed).
    // ════════════════════════════════════════════════════════════════════
    reg [4:0]  left_nc [0:3];
    reg [7:0]  left_recon_y [0:15];
    reg [7:0]  left_recon_u [0:7];
    reg [7:0]  left_recon_v [0:7];
    reg        left_intra;
    reg [15:0] left_mv_x, left_mv_y;

    always @(posedge clk) begin
        if (reset) begin
            left_intra <= 1'b0;
            left_mv_x  <= 16'd0;
            left_mv_y  <= 16'd0;
        end else if (stim_valid) begin
            left_nc[stim_mode[1:0]] <= cavlc_tc;
            left_recon_y[stim_mode] <= recon_out[0];
            left_recon_u[stim_mode[2:0]] <= recon_out[1];
            left_recon_v[stim_mode[2:0]] <= recon_out[2];
            left_intra <= stim_mode[3];
            left_mv_x  <= stim_mv_x;
            left_mv_y  <= stim_mv_y;
        end
    end

    wire [7:0] xor_left = left_recon_y[0] ^ left_recon_y[15] ^
                           left_recon_u[0] ^ left_recon_v[0] ^
                           {3'd0, left_nc[0]} ^ left_mv_x[7:0] ^
                           {7'd0, left_intra};

    // ════════════════════════════════════════════════════════════════════
    // 17. MB CONTROLLER FSM — resource stub (w-arch: h264_mb_controller)
    //     Sequential FSM controlling MB-level decode sequencing.
    //     Stub represents ~200-300 ALMs of state machine logic.
    // ════════════════════════════════════════════════════════════════════
    localparam [3:0] MB_ST_IDLE    = 4'd0,
                     MB_ST_PARSE   = 4'd1,
                     MB_ST_DEQUANT = 4'd2,
                     MB_ST_IDCT    = 4'd3,
                     MB_ST_PRED    = 4'd4,
                     MB_ST_RECON   = 4'd5,
                     MB_ST_DEBLOCK = 4'd6,
                     MB_ST_STORE   = 4'd7,
                     MB_ST_NEXT    = 4'd8;

    reg [3:0]  mb_state;
    reg [15:0] mb_addr;
    reg [7:0]  mb_x, mb_y;
    reg [4:0]  block_idx;
    reg [2:0]  sub_state;
    reg [9:0]  cycle_count;

    always @(posedge clk) begin
        if (reset) begin
            mb_state    <= MB_ST_IDLE;
            mb_addr     <= 16'd0;
            mb_x        <= 8'd0;
            mb_y        <= 8'd0;
            block_idx   <= 5'd0;
            sub_state   <= 3'd0;
            cycle_count <= 10'd0;
        end else begin
            cycle_count <= cycle_count + 10'd1;
            case (mb_state)
                MB_ST_IDLE: if (stim_valid) begin
                    mb_state <= MB_ST_PARSE;
                    mb_addr  <= {8'd0, stim_byte};
                end
                MB_ST_PARSE: begin
                    if (cycle_count[5:0] == 6'd63)
                        mb_state <= MB_ST_DEQUANT;
                    block_idx <= block_idx + 5'd1;
                end
                MB_ST_DEQUANT: mb_state <= MB_ST_IDCT;
                MB_ST_IDCT:    mb_state <= MB_ST_PRED;
                MB_ST_PRED:    mb_state <= MB_ST_RECON;
                MB_ST_RECON:   mb_state <= MB_ST_DEBLOCK;
                MB_ST_DEBLOCK: begin
                    if (sub_state == 3'd7)
                        mb_state <= MB_ST_STORE;
                    sub_state <= sub_state + 3'd1;
                end
                MB_ST_STORE: mb_state <= MB_ST_NEXT;
                MB_ST_NEXT: begin
                    mb_state <= MB_ST_IDLE;
                    if (mb_x == MB_W[7:0] - 8'd1) begin
                        mb_x <= 8'd0;
                        mb_y <= mb_y + 8'd1;
                    end else begin
                        mb_x <= mb_x + 8'd1;
                    end
                    mb_addr <= mb_addr + 16'd1;
                end
                default: mb_state <= MB_ST_IDLE;
            endcase
        end
    end

    wire [7:0] xor_mbctrl = {4'd0, mb_state} ^ mb_addr[7:0] ^ mb_x ^ mb_y ^
                             {3'd0, block_idx} ^ {5'd0, sub_state} ^ cycle_count[7:0];

    // ════════════════════════════════════════════════════════════════════
    // 18. PIPELINE HANDSHAKE — valid/ready registered boundaries
    //     One stage per major pipeline boundary (5 stages representative).
    // ════════════════════════════════════════════════════════════════════
    localparam int PIPE_STAGES = 5;
    reg [7:0]  pipe_data [0:PIPE_STAGES-1];
    reg        pipe_valid [0:PIPE_STAGES-1];

    genvar psi;
    generate
        for (psi = 0; psi < PIPE_STAGES; psi = psi + 1) begin : gen_pipe
            always @(posedge clk) begin
                if (reset) begin
                    pipe_data[psi]  <= 8'd0;
                    pipe_valid[psi] <= 1'b0;
                end else begin
                    if (psi == 0) begin
                        pipe_data[0]  <= stim_byte ^ xor_cavlc;
                        pipe_valid[0] <= stim_valid;
                    end else begin
                        pipe_data[psi]  <= pipe_data[psi-1] ^ recon_out[psi[3:0]];
                        pipe_valid[psi] <= pipe_valid[psi-1];
                    end
                end
            end
        end
    endgenerate

    wire [7:0] xor_pipe = pipe_data[PIPE_STAGES-1] ^ {3'd0, pipe_valid[0],
                           pipe_valid[1], pipe_valid[2], pipe_valid[3], pipe_valid[4]};

    // ════════════════════════════════════════════════════════════════════
    // OBSERVATION REGISTER — collects ALL module outputs, drives port
    // ════════════════════════════════════════════════════════════════════
    (* preserve, noprune *) reg [7:0] xor_chain;

    always @(posedge clk) begin
        if (reset)
            xor_chain <= 8'd0;
        else
            xor_chain <= xor_cavlc ^ xor_arith ^ xor_intra ^ xor_mc ^
                         xor_dpb ^ xor_deblock ^ xor_mv ^ xor_linebuf ^
                         xor_nc ^ xor_pmb ^ xor_chroma ^ xor_tap ^
                         xor_eg ^ xor_fifo ^ xor_refwin ^ xor_left ^
                         xor_mbctrl ^ xor_pipe;
    end

    assign skeleton_observe = xor_chain;

endmodule

`default_nettype wire
