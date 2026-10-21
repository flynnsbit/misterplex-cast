// H.264 intra prediction neighbour context store.
// Stores reconstructed (pre-deblock) samples for cross-MB and within-MB
// neighbour derivation. Feeds h264_intra4x4_pred and h264_intra16x16_pred.
//
// Storage:
//   - Above row line buffer: MB_WIDTH_MAX * 16 luma samples in M10K
//     (bottom row of each MB in the row above)
//   - Left column: 16 registers (right column of left MB / within-MB)
//   - Within-MB buffer: 16x16 registers (accumulates as blocks finish)
//   - Top-left corner: 1 register per MB boundary
//
// The line buffer holds PRE-DEBLOCK reconstructed samples.
// Post-deblock samples for DPB are stored separately by the deblock path.
//
// Resource estimate @ MB_WIDTH_MAX=80: 1 M10K (1280 B above row) + ~260 registers.
// Cap is a hard geometry gate for 1280-wide (80 MB/row). Exceeding it used to
// silently wrap above_row_buf addresses (mb_x=64 → alias mb 0). geom_reject
// makes that LOUD and sticky.

module h264_intra_nb_ctx #(
    parameter int MB_WIDTH_MAX = 80   // max MBs per row (1280/16)
)(
    input  wire        clk,
    input  wire        reset,

    // MB-level control
    input  wire [7:0]  mb_x,
    input  wire [7:0]  mb_y,
    input  wire [7:0]  mb_width,
    input  wire        mb_start,      // pulse at start of each MB

    // Block-level control
    input  wire [3:0]  block_idx,     // 0..15 raster within 16x16 MB
    input  wire        block_valid,   // pulse: block reconstructed

    // Reconstructed pixels from recon4x4 (feedback path)
    input  wire [7:0]  recon_pixels [0:15],  // 4x4 block in raster order

    // I4x4 outputs (per-block, depends on block_idx)
    output wire [7:0]  above [0:7],   // 4 above + 4 above-right
    output wire [7:0]  left [0:3],
    output wire [7:0]  top_left,
    output wire        has_above,
    output wire        has_left,

    // Sticky geometry reject: mb_x >= MB_WIDTH_MAX or mb_width > MB_WIDTH_MAX.
    // Does not clear until reset. FAULT_NB_CTX_NO_GEOM_REJECT silences it (red twin).
    output reg         geom_reject
);

    // =========================================================================
    // Block position decode
    // =========================================================================
    wire [3:0] blk_x = {block_idx[1], block_idx[0], 2'b00};  // pixel x: (idx%4)*4
    wire [3:0] blk_y = {block_idx[3], block_idx[2], 2'b00};  // pixel y: (idx/4)*4

    // =========================================================================
    // Geometry cap — loud sticky reject (silent wrap was the defect)
    // =========================================================================
    // mb_x is 0..mb_width-1; legal max index is MB_WIDTH_MAX-1.
    wire geom_over_x     = (mb_x >= MB_WIDTH_MAX[7:0]);
    wire geom_over_width = (mb_width > MB_WIDTH_MAX[7:0]);
    wire geom_fire       = geom_over_x | geom_over_width;

    always @(posedge clk) begin
        if (reset)
            geom_reject <= 1'b0;
`ifdef FAULT_NB_CTX_NO_GEOM_REJECT
        else
            geom_reject <= 1'b0; // red twin: swallow the reject
`else
        else if (geom_fire && (mb_start || block_valid))
            geom_reject <= 1'b1;
`endif
    end

    // Suppress line-buffer writes when over cap (still raise reject).
    wire nb_geom_ok = !geom_over_x && !geom_over_width && !geom_reject;

    // =========================================================================
    // Availability flags (geometric, Baseline single-slice IDR)
    // =========================================================================
    assign has_above = (mb_y != 8'd0) || (blk_y != 4'd0);
    assign has_left  = (mb_x != 8'd0) || (blk_x != 4'd0);

    // Above-right availability for I4x4 (spec 6.4.11.1)
    // Available if: above exists AND there is a valid block to the upper-right
    // Block-level: blocks at blk_x=12 within the MB have no above-right from
    // within the MB when blk_y > 0 (the block to their right is in the next row).
    // Additionally, blocks 3,7,11,13,15 (in zigzag) have above-right unavailable
    // per the spec — but in simple raster, at blk_x=12 the above-right is the
    // next MB's above buffer which requires mb_avail_topright.
    wire has_above_right_mb = (mb_y != 8'd0) && (mb_x < (mb_width - 8'd1));
    wire has_above_right_within = (blk_y != 4'd0) && (blk_x < 4'd12);
    wire has_above_right_top_edge = (blk_y == 4'd0) && (
        (blk_x < 4'd12) || has_above_right_mb
    );
    wire has_above_right = (blk_y == 4'd0) ? has_above_right_top_edge :
                           has_above_right_within;

    // =========================================================================
    // Within-MB reconstruction buffer (16x16 bytes, registers/MLAB)
    // =========================================================================
    // Stores all reconstructed pixels of the current MB as blocks complete.
    // Used for within-MB neighbour derivation.
    reg [7:0] mb_buf [0:15][0:15];  // [row][col], pixel coordinates

    // Store reconstructed block into mb_buf on block_valid
    integer si, sj;
    always @(posedge clk) begin
        if (reset || mb_start) begin
            // Clear on MB start (or use don't-care — blocks write before read)
            for (si = 0; si < 16; si = si + 1)
                for (sj = 0; sj < 16; sj = sj + 1)
                    mb_buf[si][sj] <= 8'd128;
        end else if (block_valid) begin
            // recon_pixels[0:15] is 4x4 in raster: [y*4+x]
            for (si = 0; si < 4; si = si + 1)
                for (sj = 0; sj < 4; sj = sj + 1)
                    mb_buf[blk_y + si[3:0]][blk_x + sj[3:0]] <= recon_pixels[si*4 + sj];
        end
    end

    // =========================================================================
    // Above row line buffer (M10K) — bottom row of each MB from the row above
    // =========================================================================
    // Stores 16 samples per MB column: the bottom row (row 15) of reconstructed
    // luma for each MB in the previous row.
    // Address: mb_x * 16 + pixel_x_within_mb (0..15)
    // Total: MB_WIDTH_MAX * 16 (80 → 1280 B = 1 M10K)
    localparam int ABOVE_BUF_DEPTH = MB_WIDTH_MAX * 16;
    localparam int ABOVE_AW = $clog2(ABOVE_BUF_DEPTH);

    (* ramstyle = "M10K" *) reg [7:0] above_row_buf [0:ABOVE_BUF_DEPTH-1];

    // Read port: combinational read for current block's above context
    // Write port: store bottom row when MB row completes
    reg [ABOVE_AW-1:0] above_wr_addr;
    reg [7:0]          above_wr_data;
    reg                above_wr_en;

    always @(posedge clk) begin
        if (above_wr_en)
            above_row_buf[above_wr_addr] <= above_wr_data;
    end

    // Write the bottom row (row 15) of current MB into above_row_buf
    // at position mb_x*16 + col. Triggered at end of MB (when all blocks done).
    // We write sample-by-sample from mb_buf[15][0..15] using a small FSM.
    reg [4:0] above_wr_cnt;
    reg       above_wr_active;

    always @(posedge clk) begin
        if (reset) begin
            above_wr_active <= 1'b0;
            above_wr_cnt <= 5'd0;
            above_wr_en <= 1'b0;
        end else begin
            above_wr_en <= 1'b0;
            if (block_valid && block_idx == 4'd15 && nb_geom_ok) begin
                // Last block of MB done — start writing bottom row to line buf
                above_wr_active <= 1'b1;
                above_wr_cnt <= 5'd0;
            end
            if (above_wr_active) begin
                above_wr_en <= 1'b1;
                above_wr_addr <= mb_x * 16 + {{(ABOVE_AW-5){1'b0}}, above_wr_cnt};
                above_wr_data <= mb_buf[15][above_wr_cnt[3:0]];
                if (above_wr_cnt == 5'd15) begin
                    above_wr_active <= 1'b0;
                end
                above_wr_cnt <= above_wr_cnt + 5'd1;
            end
        end
    end

    // =========================================================================
    // Left column register — right column (col 15) of the left MB
    // =========================================================================
    reg [7:0] left_col [0:15];  // 16 rows of the right column of left MB

    // Store right column at end of each MB
    integer li;
    always @(posedge clk) begin
        if (reset) begin
            for (li = 0; li < 16; li = li + 1)
                left_col[li] <= 8'd128;
        end else if (block_valid && block_idx == 4'd15) begin
            // Store right column (col 15) of current MB for next MB's left
            for (li = 0; li < 16; li = li + 1)
                left_col[li] <= mb_buf[li][15];
        end
    end

    // =========================================================================
    // Top-left corner register
    // =========================================================================
    reg [7:0] tl_corner;  // bottom-right pixel of above-left MB

    always @(posedge clk) begin
        if (reset)
            tl_corner <= 8'd128;
        else if (mb_start)
            // Latch: the current left_col[15] is the bottom-right of the MB
            // that was to our left in the previous row... but actually we need
            // the above-row buffer's last pixel of the left MB.
            // top_left for MB = above_row_buf[(mb_x-1)*16 + 15]
            // We read it on mb_start. For now, use left_col[0] which is top-left
            // of the row from the left MB... Actually this needs to be the
            // bottom-right of the above-left MB.
            // Simplified: store from above_row_buf at mb_start
            tl_corner <= (mb_x > 0 && mb_y > 0) ?
                         above_row_buf[(mb_x - 8'd1) * 16 + 15] : 8'd128;
    end

    // =========================================================================
    // Output mux: select between within-MB buffer and cross-MB storage
    // =========================================================================

    // --- ABOVE[0:3] ---
    // If blk_y > 0: from mb_buf[blk_y-1][blk_x+0..3] (within MB)
    // If blk_y == 0: from above_row_buf[mb_x*16 + blk_x + 0..3]
    wire [7:0] above_from_mb [0:3];
    wire [7:0] above_from_buf [0:3];
    genvar gi;
    generate
        for (gi = 0; gi < 4; gi = gi + 1) begin : gen_above
            assign above_from_mb[gi] = mb_buf[blk_y - 4'd1][blk_x + gi[3:0]];
            assign above_from_buf[gi] = above_row_buf[mb_x * 16 + {4'd0, blk_x} + gi];
            assign above[gi] = has_above ?
                               ((blk_y != 4'd0) ? above_from_mb[gi] : above_from_buf[gi])
                               : 8'd128;
        end
    endgenerate

    // --- ABOVE[4:7] (above-right) ---
    // If has_above_right: from the 4 pixels to the right of above[0:3]
    // If not: replicate above[3] (spec clause 8.3.1.2.1)
    wire [7:0] above_right_from_mb [0:3];
    wire [7:0] above_right_from_buf [0:3];
    generate
        for (gi = 0; gi < 4; gi = gi + 1) begin : gen_above_right
            assign above_right_from_mb[gi] = mb_buf[blk_y - 4'd1][blk_x + 4'd4 + gi[3:0]];
            assign above_right_from_buf[gi] = above_row_buf[mb_x * 16 + {4'd0, blk_x} + 4 + gi];
            assign above[4 + gi] = has_above_right ?
                                   ((blk_y != 4'd0) ? above_right_from_mb[gi] : above_right_from_buf[gi])
                                   : above[3];  // substitute from above[3]
        end
    endgenerate

    // --- LEFT[0:3] ---
    // If blk_x > 0: from mb_buf[blk_y+0..3][blk_x-1] (within MB)
    // If blk_x == 0: from left_col[blk_y+0..3]
    wire [7:0] left_from_mb [0:3];
    wire [7:0] left_from_col [0:3];
    generate
        for (gi = 0; gi < 4; gi = gi + 1) begin : gen_left
            assign left_from_mb[gi] = mb_buf[blk_y + gi[3:0]][blk_x - 4'd1];
            assign left_from_col[gi] = left_col[blk_y + gi[3:0]];
            assign left[gi] = has_left ?
                              ((blk_x != 4'd0) ? left_from_mb[gi] : left_from_col[gi])
                              : 8'd128;
        end
    endgenerate

    // --- TOP_LEFT ---
    // Corner pixel at (blk_x-1, blk_y-1)
    // Cases:
    //   blk_x>0, blk_y>0: mb_buf[blk_y-1][blk_x-1]
    //   blk_x==0, blk_y>0: left_col[blk_y-1]
    //   blk_x>0, blk_y==0: above_row_buf[mb_x*16 + blk_x - 1]
    //   blk_x==0, blk_y==0: tl_corner (above-left MB's bottom-right)
    assign top_left = (!has_above && !has_left) ? 8'd128 :
                      (blk_x != 4'd0 && blk_y != 4'd0) ? mb_buf[blk_y - 4'd1][blk_x - 4'd1] :
                      (blk_x == 4'd0 && blk_y != 4'd0) ? left_col[blk_y - 4'd1] :
                      (blk_x != 4'd0 && blk_y == 4'd0) ? above_row_buf[mb_x * 16 + {4'd0, blk_x} - 1] :
                      tl_corner;

endmodule
