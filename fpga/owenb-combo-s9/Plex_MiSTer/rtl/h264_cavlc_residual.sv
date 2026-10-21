// H.264 Baseline CAVLC residual block decoder building blocks.
// Sequential bit/table decoder; outputs coefficients in CAVLC scan order for h264_dequant4x4.

module h264_cavlc_nc_predictor (
    input  wire [7:0]  mb_x,
    input  wire [7:0]  mb_y,
    input  wire [15:0] mb_index,
    input  wire [7:0]  mb_width,
    input  wire [15:0] first_mb_in_slice,
    input  wire [1:0]  block_x,
    input  wire [1:0]  block_y,
    input  wire        left_tc_valid,
    input  wire [4:0]  left_tc,
    input  wire        up_tc_valid,
    input  wire [4:0]  up_tc,
    output wire        nA_available,
    output wire        nB_available,
    output wire [4:0]  nC,
    output wire [2:0]  coeff_token_table
);
    wire left_mb_available = (mb_x != 8'd0) && ((mb_index - 16'd1) >= first_mb_in_slice);
    wire up_mb_available   = (mb_y != 8'd0) && ((mb_index - {8'd0, mb_width}) >= first_mb_in_slice);
    assign nA_available = left_tc_valid && ((block_x != 2'd0) || left_mb_available);
    assign nB_available = up_tc_valid   && ((block_y != 2'd0) || up_mb_available);
    assign nC = (nA_available && nB_available) ? ((left_tc + up_tc + 5'd1) >> 1) :
                nA_available ? left_tc :
                nB_available ? up_tc : 5'd0;
    assign coeff_token_table = (nC < 5'd2) ? 3'd0 :
                               (nC < 5'd4) ? 3'd1 :
                               (nC < 5'd8) ? 3'd2 : 3'd3;
endmodule

module h264_cavlc_residual_block #(
    parameter int MAX_BYTES = 64
)(
    input  wire               clk,
    input  wire               reset,
    input  wire               start,
    input  wire [2:0]         coeff_token_table, // 0:nC<2, 1:nC<4, 2:nC<8, 3:nC>=8, 4:chroma_dc
    input  wire [4:0]         max_coeff,         // 16 luma, 15 AC-only, 4 chroma DC
    input  wire [9:0]         bit_offset_start,
    input  wire [9:0]         bit_len,
    input  wire [7:0]         rbsp [0:MAX_BYTES-1],
    output reg                busy,
    output reg                done,
    output reg                ok,
    output reg [9:0]          bit_offset_end,
    output reg [4:0]          total_coeff,
    output reg [1:0]          trailing_ones,
    output reg [3:0]          total_zeros,
    output reg signed [15:0]  coeff [0:15],
    output reg signed [15:0]  level_dbg [0:15],
    output reg [3:0]          run_dbg [0:15]
`ifdef CAVLC_CYCLE_PROBE
    ,
    // Measurement-only stage cycle counters (cleared on start, freeze at done).
    output reg [15:0]         cy_token,      // ST_TOKEN_BIT + ST_TOKEN_CHK
    output reg [15:0]         cy_sign,       // ST_SIGN (trailing ones)
    output reg [15:0]         cy_level,      // ST_LVL_PRE + ST_LVL_SUF + ST_LVL_STORE
    output reg [15:0]         cy_total_zeros,// ST_TZ_BIT + ST_TZ_CHK
    output reg [15:0]         cy_run_before, // ST_RUN_BIT + ST_RUN_CHK
    output reg [15:0]         cy_place,      // ST_PLACE_INIT + ST_PLACE_STEP
    output reg [15:0]         cy_other,      // IDLE sample edge / DONE / FAIL overhead in block
    output reg [15:0]         cy_total       // all cycles while busy (start accept .. done pulse)
`endif
);
    localparam [4:0]
        ST_IDLE       = 5'd0,
        ST_TOKEN_BIT  = 5'd1,
        ST_TOKEN_CHK  = 5'd2,
        ST_SIGN       = 5'd3,
        ST_LVL_PRE    = 5'd4,
        ST_LVL_SUF    = 5'd5,
        ST_LVL_STORE  = 5'd6,
        ST_TZ_BIT     = 5'd7,
        ST_TZ_CHK     = 5'd8,
        ST_RUN_BIT    = 5'd9,
        ST_RUN_CHK    = 5'd10,
        ST_PLACE_INIT = 5'd11,
        ST_PLACE_STEP = 5'd12,
        ST_DONE       = 5'd13,
        ST_FAIL       = 5'd14;

    reg [4:0] st;
    reg [9:0] bit_pos;
    reg [15:0] code;
    reg [4:0] code_len;
    reg [4:0] tc_r;
    reg [1:0] t1_r;
    reg [4:0] idx;
    reg [2:0] suffix_length;
    reg [5:0] prefix;
    reg [4:0] suffix_left;
    reg [31:0] suffix_acc;
    reg first_non_t1;
    reg [31:0] level_code;
    reg [3:0] zeros_left;
    reg [4:0] place_i;
    reg signed [5:0] coeff_num;

    wire cur_bit = (bit_pos < bit_len) ? rbsp[bit_pos[8:3]][3'd7 - bit_pos[2:0]] : 1'b0;
    wire token_too_long = (coeff_token_table == 3'd3) ? (code_len >= 5'd6) :
                          (coeff_token_table == 3'd4) ? (code_len >= 5'd8) : (code_len >= 5'd16);

    // Peek up to 32 stream bits starting at abs bit offset `pos` (MSB = first).
    // Throughput: multi-bit level prefix/suffix + single-cycle VLC attempts.
    function automatic [31:0] peek32(input [9:0] pos);
        integer k;
        reg [9:0] p;
        begin
            peek32 = 32'd0;
            for (k = 0; k < 32; k = k + 1) begin
                p = pos + k[9:0];
                if (p < bit_len)
                    peek32[31 - k] = rbsp[p[8:3]][3'd7 - p[2:0]];
            end
        end
    endfunction

    // Count leading zeros before first 1 in window (stream order = MSB first).
    // Returns 32 if no 1 present in the 32-bit window.
    function automatic [5:0] clz32(input [31:0] w);
        integer k;
        begin
            clz32 = 6'd32;
            for (k = 31; k >= 0; k = k - 1) begin
                if (w[k] && clz32 == 6'd32)
                    clz32 = 6'(31 - k);
            end
        end
    endfunction

    // Extract n MSBs of window as integer (n in 1..32).
    function automatic [31:0] take_msb(input [31:0] w, input [5:0] n);
        begin
            if (n == 6'd0)
                take_msb = 32'd0;
            else if (n >= 6'd32)
                take_msb = w;
            else
                take_msb = w >> (6'd32 - n);
        end
    endfunction
    wire tz_is_chroma = (max_coeff == 5'd4);

    function automatic [7:0] coeff_token_lookup(input [2:0] tab, input [4:0] len, input [15:0] bits);
        begin
            coeff_token_lookup = 8'd0;
            case ({tab, len, bits})
            24'h010001: coeff_token_lookup = {1'b1, 5'd0, 2'd0};
            24'h060005: coeff_token_lookup = {1'b1, 5'd1, 2'd0};
            24'h020001: coeff_token_lookup = {1'b1, 5'd1, 2'd1};
            24'h080007: coeff_token_lookup = {1'b1, 5'd2, 2'd0};
            24'h060004: coeff_token_lookup = {1'b1, 5'd2, 2'd1};
            24'h030001: coeff_token_lookup = {1'b1, 5'd2, 2'd2};
            24'h090007: coeff_token_lookup = {1'b1, 5'd3, 2'd0};
            24'h080006: coeff_token_lookup = {1'b1, 5'd3, 2'd1};
            24'h070005: coeff_token_lookup = {1'b1, 5'd3, 2'd2};
            24'h050003: coeff_token_lookup = {1'b1, 5'd3, 2'd3};
            24'h0a0007: coeff_token_lookup = {1'b1, 5'd4, 2'd0};
            24'h090006: coeff_token_lookup = {1'b1, 5'd4, 2'd1};
            24'h080005: coeff_token_lookup = {1'b1, 5'd4, 2'd2};
            24'h060003: coeff_token_lookup = {1'b1, 5'd4, 2'd3};
            24'h0b0007: coeff_token_lookup = {1'b1, 5'd5, 2'd0};
            24'h0a0006: coeff_token_lookup = {1'b1, 5'd5, 2'd1};
            24'h090005: coeff_token_lookup = {1'b1, 5'd5, 2'd2};
            24'h070004: coeff_token_lookup = {1'b1, 5'd5, 2'd3};
            24'h0d000f: coeff_token_lookup = {1'b1, 5'd6, 2'd0};
            24'h0b0006: coeff_token_lookup = {1'b1, 5'd6, 2'd1};
            24'h0a0005: coeff_token_lookup = {1'b1, 5'd6, 2'd2};
            24'h080004: coeff_token_lookup = {1'b1, 5'd6, 2'd3};
            24'h0d000b: coeff_token_lookup = {1'b1, 5'd7, 2'd0};
            24'h0d000e: coeff_token_lookup = {1'b1, 5'd7, 2'd1};
            24'h0b0005: coeff_token_lookup = {1'b1, 5'd7, 2'd2};
            24'h090004: coeff_token_lookup = {1'b1, 5'd7, 2'd3};
            24'h0d0008: coeff_token_lookup = {1'b1, 5'd8, 2'd0};
            24'h0d000a: coeff_token_lookup = {1'b1, 5'd8, 2'd1};
            24'h0d000d: coeff_token_lookup = {1'b1, 5'd8, 2'd2};
            24'h0a0004: coeff_token_lookup = {1'b1, 5'd8, 2'd3};
            24'h0e000f: coeff_token_lookup = {1'b1, 5'd9, 2'd0};
            24'h0e000e: coeff_token_lookup = {1'b1, 5'd9, 2'd1};
            24'h0d0009: coeff_token_lookup = {1'b1, 5'd9, 2'd2};
            24'h0b0004: coeff_token_lookup = {1'b1, 5'd9, 2'd3};
            24'h0e000b: coeff_token_lookup = {1'b1, 5'd10, 2'd0};
            24'h0e000a: coeff_token_lookup = {1'b1, 5'd10, 2'd1};
            24'h0e000d: coeff_token_lookup = {1'b1, 5'd10, 2'd2};
            24'h0d000c: coeff_token_lookup = {1'b1, 5'd10, 2'd3};
            24'h0f000f: coeff_token_lookup = {1'b1, 5'd11, 2'd0};
            24'h0f000e: coeff_token_lookup = {1'b1, 5'd11, 2'd1};
            24'h0e0009: coeff_token_lookup = {1'b1, 5'd11, 2'd2};
            24'h0e000c: coeff_token_lookup = {1'b1, 5'd11, 2'd3};
            24'h0f000b: coeff_token_lookup = {1'b1, 5'd12, 2'd0};
            24'h0f000a: coeff_token_lookup = {1'b1, 5'd12, 2'd1};
            24'h0f000d: coeff_token_lookup = {1'b1, 5'd12, 2'd2};
            24'h0e0008: coeff_token_lookup = {1'b1, 5'd12, 2'd3};
            24'h10000f: coeff_token_lookup = {1'b1, 5'd13, 2'd0};
            24'h0f0001: coeff_token_lookup = {1'b1, 5'd13, 2'd1};
            24'h0f0009: coeff_token_lookup = {1'b1, 5'd13, 2'd2};
            24'h0f000c: coeff_token_lookup = {1'b1, 5'd13, 2'd3};
            24'h10000b: coeff_token_lookup = {1'b1, 5'd14, 2'd0};
            24'h10000e: coeff_token_lookup = {1'b1, 5'd14, 2'd1};
            24'h10000d: coeff_token_lookup = {1'b1, 5'd14, 2'd2};
            24'h0f0008: coeff_token_lookup = {1'b1, 5'd14, 2'd3};
            24'h100007: coeff_token_lookup = {1'b1, 5'd15, 2'd0};
            24'h10000a: coeff_token_lookup = {1'b1, 5'd15, 2'd1};
            24'h100009: coeff_token_lookup = {1'b1, 5'd15, 2'd2};
            24'h10000c: coeff_token_lookup = {1'b1, 5'd15, 2'd3};
            24'h100004: coeff_token_lookup = {1'b1, 5'd16, 2'd0};
            24'h100006: coeff_token_lookup = {1'b1, 5'd16, 2'd1};
            24'h100005: coeff_token_lookup = {1'b1, 5'd16, 2'd2};
            24'h100008: coeff_token_lookup = {1'b1, 5'd16, 2'd3};
            24'h220003: coeff_token_lookup = {1'b1, 5'd0, 2'd0};
            24'h26000b: coeff_token_lookup = {1'b1, 5'd1, 2'd0};
            24'h220002: coeff_token_lookup = {1'b1, 5'd1, 2'd1};
            24'h260007: coeff_token_lookup = {1'b1, 5'd2, 2'd0};
            24'h250007: coeff_token_lookup = {1'b1, 5'd2, 2'd1};
            24'h230003: coeff_token_lookup = {1'b1, 5'd2, 2'd2};
            24'h270007: coeff_token_lookup = {1'b1, 5'd3, 2'd0};
            24'h26000a: coeff_token_lookup = {1'b1, 5'd3, 2'd1};
            24'h260009: coeff_token_lookup = {1'b1, 5'd3, 2'd2};
            24'h240005: coeff_token_lookup = {1'b1, 5'd3, 2'd3};
            24'h280007: coeff_token_lookup = {1'b1, 5'd4, 2'd0};
            24'h260006: coeff_token_lookup = {1'b1, 5'd4, 2'd1};
            24'h260005: coeff_token_lookup = {1'b1, 5'd4, 2'd2};
            24'h240004: coeff_token_lookup = {1'b1, 5'd4, 2'd3};
            24'h280004: coeff_token_lookup = {1'b1, 5'd5, 2'd0};
            24'h270006: coeff_token_lookup = {1'b1, 5'd5, 2'd1};
            24'h270005: coeff_token_lookup = {1'b1, 5'd5, 2'd2};
            24'h250006: coeff_token_lookup = {1'b1, 5'd5, 2'd3};
            24'h290007: coeff_token_lookup = {1'b1, 5'd6, 2'd0};
            24'h280006: coeff_token_lookup = {1'b1, 5'd6, 2'd1};
            24'h280005: coeff_token_lookup = {1'b1, 5'd6, 2'd2};
            24'h260008: coeff_token_lookup = {1'b1, 5'd6, 2'd3};
            24'h2b000f: coeff_token_lookup = {1'b1, 5'd7, 2'd0};
            24'h290006: coeff_token_lookup = {1'b1, 5'd7, 2'd1};
            24'h290005: coeff_token_lookup = {1'b1, 5'd7, 2'd2};
            24'h260004: coeff_token_lookup = {1'b1, 5'd7, 2'd3};
            24'h2b000b: coeff_token_lookup = {1'b1, 5'd8, 2'd0};
            24'h2b000e: coeff_token_lookup = {1'b1, 5'd8, 2'd1};
            24'h2b000d: coeff_token_lookup = {1'b1, 5'd8, 2'd2};
            24'h270004: coeff_token_lookup = {1'b1, 5'd8, 2'd3};
            24'h2c000f: coeff_token_lookup = {1'b1, 5'd9, 2'd0};
            24'h2b000a: coeff_token_lookup = {1'b1, 5'd9, 2'd1};
            24'h2b0009: coeff_token_lookup = {1'b1, 5'd9, 2'd2};
            24'h290004: coeff_token_lookup = {1'b1, 5'd9, 2'd3};
            24'h2c000b: coeff_token_lookup = {1'b1, 5'd10, 2'd0};
            24'h2c000e: coeff_token_lookup = {1'b1, 5'd10, 2'd1};
            24'h2c000d: coeff_token_lookup = {1'b1, 5'd10, 2'd2};
            24'h2b000c: coeff_token_lookup = {1'b1, 5'd10, 2'd3};
            24'h2c0008: coeff_token_lookup = {1'b1, 5'd11, 2'd0};
            24'h2c000a: coeff_token_lookup = {1'b1, 5'd11, 2'd1};
            24'h2c0009: coeff_token_lookup = {1'b1, 5'd11, 2'd2};
            24'h2b0008: coeff_token_lookup = {1'b1, 5'd11, 2'd3};
            24'h2d000f: coeff_token_lookup = {1'b1, 5'd12, 2'd0};
            24'h2d000e: coeff_token_lookup = {1'b1, 5'd12, 2'd1};
            24'h2d000d: coeff_token_lookup = {1'b1, 5'd12, 2'd2};
            24'h2c000c: coeff_token_lookup = {1'b1, 5'd12, 2'd3};
            24'h2d000b: coeff_token_lookup = {1'b1, 5'd13, 2'd0};
            24'h2d000a: coeff_token_lookup = {1'b1, 5'd13, 2'd1};
            24'h2d0009: coeff_token_lookup = {1'b1, 5'd13, 2'd2};
            24'h2d000c: coeff_token_lookup = {1'b1, 5'd13, 2'd3};
            24'h2d0007: coeff_token_lookup = {1'b1, 5'd14, 2'd0};
            24'h2e000b: coeff_token_lookup = {1'b1, 5'd14, 2'd1};
            24'h2d0006: coeff_token_lookup = {1'b1, 5'd14, 2'd2};
            24'h2d0008: coeff_token_lookup = {1'b1, 5'd14, 2'd3};
            24'h2e0009: coeff_token_lookup = {1'b1, 5'd15, 2'd0};
            24'h2e0008: coeff_token_lookup = {1'b1, 5'd15, 2'd1};
            24'h2e000a: coeff_token_lookup = {1'b1, 5'd15, 2'd2};
            24'h2d0001: coeff_token_lookup = {1'b1, 5'd15, 2'd3};
            24'h2e0007: coeff_token_lookup = {1'b1, 5'd16, 2'd0};
            24'h2e0006: coeff_token_lookup = {1'b1, 5'd16, 2'd1};
            24'h2e0005: coeff_token_lookup = {1'b1, 5'd16, 2'd2};
            24'h2e0004: coeff_token_lookup = {1'b1, 5'd16, 2'd3};
            24'h44000f: coeff_token_lookup = {1'b1, 5'd0, 2'd0};
            24'h46000f: coeff_token_lookup = {1'b1, 5'd1, 2'd0};
            24'h44000e: coeff_token_lookup = {1'b1, 5'd1, 2'd1};
            24'h46000b: coeff_token_lookup = {1'b1, 5'd2, 2'd0};
            24'h45000f: coeff_token_lookup = {1'b1, 5'd2, 2'd1};
            24'h44000d: coeff_token_lookup = {1'b1, 5'd2, 2'd2};
            24'h460008: coeff_token_lookup = {1'b1, 5'd3, 2'd0};
            24'h45000c: coeff_token_lookup = {1'b1, 5'd3, 2'd1};
            24'h45000e: coeff_token_lookup = {1'b1, 5'd3, 2'd2};
            24'h44000c: coeff_token_lookup = {1'b1, 5'd3, 2'd3};
            24'h47000f: coeff_token_lookup = {1'b1, 5'd4, 2'd0};
            24'h45000a: coeff_token_lookup = {1'b1, 5'd4, 2'd1};
            24'h45000b: coeff_token_lookup = {1'b1, 5'd4, 2'd2};
            24'h44000b: coeff_token_lookup = {1'b1, 5'd4, 2'd3};
            24'h47000b: coeff_token_lookup = {1'b1, 5'd5, 2'd0};
            24'h450008: coeff_token_lookup = {1'b1, 5'd5, 2'd1};
            24'h450009: coeff_token_lookup = {1'b1, 5'd5, 2'd2};
            24'h44000a: coeff_token_lookup = {1'b1, 5'd5, 2'd3};
            24'h470009: coeff_token_lookup = {1'b1, 5'd6, 2'd0};
            24'h46000e: coeff_token_lookup = {1'b1, 5'd6, 2'd1};
            24'h46000d: coeff_token_lookup = {1'b1, 5'd6, 2'd2};
            24'h440009: coeff_token_lookup = {1'b1, 5'd6, 2'd3};
            24'h470008: coeff_token_lookup = {1'b1, 5'd7, 2'd0};
            24'h46000a: coeff_token_lookup = {1'b1, 5'd7, 2'd1};
            24'h460009: coeff_token_lookup = {1'b1, 5'd7, 2'd2};
            24'h440008: coeff_token_lookup = {1'b1, 5'd7, 2'd3};
            24'h48000f: coeff_token_lookup = {1'b1, 5'd8, 2'd0};
            24'h47000e: coeff_token_lookup = {1'b1, 5'd8, 2'd1};
            24'h47000d: coeff_token_lookup = {1'b1, 5'd8, 2'd2};
            24'h45000d: coeff_token_lookup = {1'b1, 5'd8, 2'd3};
            24'h48000b: coeff_token_lookup = {1'b1, 5'd9, 2'd0};
            24'h48000e: coeff_token_lookup = {1'b1, 5'd9, 2'd1};
            24'h47000a: coeff_token_lookup = {1'b1, 5'd9, 2'd2};
            24'h46000c: coeff_token_lookup = {1'b1, 5'd9, 2'd3};
            24'h49000f: coeff_token_lookup = {1'b1, 5'd10, 2'd0};
            24'h48000a: coeff_token_lookup = {1'b1, 5'd10, 2'd1};
            24'h48000d: coeff_token_lookup = {1'b1, 5'd10, 2'd2};
            24'h47000c: coeff_token_lookup = {1'b1, 5'd10, 2'd3};
            24'h49000b: coeff_token_lookup = {1'b1, 5'd11, 2'd0};
            24'h49000e: coeff_token_lookup = {1'b1, 5'd11, 2'd1};
            24'h480009: coeff_token_lookup = {1'b1, 5'd11, 2'd2};
            24'h48000c: coeff_token_lookup = {1'b1, 5'd11, 2'd3};
            24'h490008: coeff_token_lookup = {1'b1, 5'd12, 2'd0};
            24'h49000a: coeff_token_lookup = {1'b1, 5'd12, 2'd1};
            24'h49000d: coeff_token_lookup = {1'b1, 5'd12, 2'd2};
            24'h480008: coeff_token_lookup = {1'b1, 5'd12, 2'd3};
            24'h4a000d: coeff_token_lookup = {1'b1, 5'd13, 2'd0};
            24'h490007: coeff_token_lookup = {1'b1, 5'd13, 2'd1};
            24'h490009: coeff_token_lookup = {1'b1, 5'd13, 2'd2};
            24'h49000c: coeff_token_lookup = {1'b1, 5'd13, 2'd3};
            24'h4a0009: coeff_token_lookup = {1'b1, 5'd14, 2'd0};
            24'h4a000c: coeff_token_lookup = {1'b1, 5'd14, 2'd1};
            24'h4a000b: coeff_token_lookup = {1'b1, 5'd14, 2'd2};
            24'h4a000a: coeff_token_lookup = {1'b1, 5'd14, 2'd3};
            24'h4a0005: coeff_token_lookup = {1'b1, 5'd15, 2'd0};
            24'h4a0008: coeff_token_lookup = {1'b1, 5'd15, 2'd1};
            24'h4a0007: coeff_token_lookup = {1'b1, 5'd15, 2'd2};
            24'h4a0006: coeff_token_lookup = {1'b1, 5'd15, 2'd3};
            24'h4a0001: coeff_token_lookup = {1'b1, 5'd16, 2'd0};
            24'h4a0004: coeff_token_lookup = {1'b1, 5'd16, 2'd1};
            24'h4a0003: coeff_token_lookup = {1'b1, 5'd16, 2'd2};
            24'h4a0002: coeff_token_lookup = {1'b1, 5'd16, 2'd3};
            24'h660003: coeff_token_lookup = {1'b1, 5'd0, 2'd0};
            24'h660000: coeff_token_lookup = {1'b1, 5'd1, 2'd0};
            24'h660001: coeff_token_lookup = {1'b1, 5'd1, 2'd1};
            24'h660004: coeff_token_lookup = {1'b1, 5'd2, 2'd0};
            24'h660005: coeff_token_lookup = {1'b1, 5'd2, 2'd1};
            24'h660006: coeff_token_lookup = {1'b1, 5'd2, 2'd2};
            24'h660008: coeff_token_lookup = {1'b1, 5'd3, 2'd0};
            24'h660009: coeff_token_lookup = {1'b1, 5'd3, 2'd1};
            24'h66000a: coeff_token_lookup = {1'b1, 5'd3, 2'd2};
            24'h66000b: coeff_token_lookup = {1'b1, 5'd3, 2'd3};
            24'h66000c: coeff_token_lookup = {1'b1, 5'd4, 2'd0};
            24'h66000d: coeff_token_lookup = {1'b1, 5'd4, 2'd1};
            24'h66000e: coeff_token_lookup = {1'b1, 5'd4, 2'd2};
            24'h66000f: coeff_token_lookup = {1'b1, 5'd4, 2'd3};
            24'h660010: coeff_token_lookup = {1'b1, 5'd5, 2'd0};
            24'h660011: coeff_token_lookup = {1'b1, 5'd5, 2'd1};
            24'h660012: coeff_token_lookup = {1'b1, 5'd5, 2'd2};
            24'h660013: coeff_token_lookup = {1'b1, 5'd5, 2'd3};
            24'h660014: coeff_token_lookup = {1'b1, 5'd6, 2'd0};
            24'h660015: coeff_token_lookup = {1'b1, 5'd6, 2'd1};
            24'h660016: coeff_token_lookup = {1'b1, 5'd6, 2'd2};
            24'h660017: coeff_token_lookup = {1'b1, 5'd6, 2'd3};
            24'h660018: coeff_token_lookup = {1'b1, 5'd7, 2'd0};
            24'h660019: coeff_token_lookup = {1'b1, 5'd7, 2'd1};
            24'h66001a: coeff_token_lookup = {1'b1, 5'd7, 2'd2};
            24'h66001b: coeff_token_lookup = {1'b1, 5'd7, 2'd3};
            24'h66001c: coeff_token_lookup = {1'b1, 5'd8, 2'd0};
            24'h66001d: coeff_token_lookup = {1'b1, 5'd8, 2'd1};
            24'h66001e: coeff_token_lookup = {1'b1, 5'd8, 2'd2};
            24'h66001f: coeff_token_lookup = {1'b1, 5'd8, 2'd3};
            24'h660020: coeff_token_lookup = {1'b1, 5'd9, 2'd0};
            24'h660021: coeff_token_lookup = {1'b1, 5'd9, 2'd1};
            24'h660022: coeff_token_lookup = {1'b1, 5'd9, 2'd2};
            24'h660023: coeff_token_lookup = {1'b1, 5'd9, 2'd3};
            24'h660024: coeff_token_lookup = {1'b1, 5'd10, 2'd0};
            24'h660025: coeff_token_lookup = {1'b1, 5'd10, 2'd1};
            24'h660026: coeff_token_lookup = {1'b1, 5'd10, 2'd2};
            24'h660027: coeff_token_lookup = {1'b1, 5'd10, 2'd3};
            24'h660028: coeff_token_lookup = {1'b1, 5'd11, 2'd0};
            24'h660029: coeff_token_lookup = {1'b1, 5'd11, 2'd1};
            24'h66002a: coeff_token_lookup = {1'b1, 5'd11, 2'd2};
            24'h66002b: coeff_token_lookup = {1'b1, 5'd11, 2'd3};
            24'h66002c: coeff_token_lookup = {1'b1, 5'd12, 2'd0};
            24'h66002d: coeff_token_lookup = {1'b1, 5'd12, 2'd1};
            24'h66002e: coeff_token_lookup = {1'b1, 5'd12, 2'd2};
            24'h66002f: coeff_token_lookup = {1'b1, 5'd12, 2'd3};
            24'h660030: coeff_token_lookup = {1'b1, 5'd13, 2'd0};
            24'h660031: coeff_token_lookup = {1'b1, 5'd13, 2'd1};
            24'h660032: coeff_token_lookup = {1'b1, 5'd13, 2'd2};
            24'h660033: coeff_token_lookup = {1'b1, 5'd13, 2'd3};
            24'h660034: coeff_token_lookup = {1'b1, 5'd14, 2'd0};
            24'h660035: coeff_token_lookup = {1'b1, 5'd14, 2'd1};
            24'h660036: coeff_token_lookup = {1'b1, 5'd14, 2'd2};
            24'h660037: coeff_token_lookup = {1'b1, 5'd14, 2'd3};
            24'h660038: coeff_token_lookup = {1'b1, 5'd15, 2'd0};
            24'h660039: coeff_token_lookup = {1'b1, 5'd15, 2'd1};
            24'h66003a: coeff_token_lookup = {1'b1, 5'd15, 2'd2};
            24'h66003b: coeff_token_lookup = {1'b1, 5'd15, 2'd3};
            24'h66003c: coeff_token_lookup = {1'b1, 5'd16, 2'd0};
            24'h66003d: coeff_token_lookup = {1'b1, 5'd16, 2'd1};
            24'h66003e: coeff_token_lookup = {1'b1, 5'd16, 2'd2};
            24'h66003f: coeff_token_lookup = {1'b1, 5'd16, 2'd3};
            24'h820001: coeff_token_lookup = {1'b1, 5'd0, 2'd0};
            24'h860007: coeff_token_lookup = {1'b1, 5'd1, 2'd0};
            24'h810001: coeff_token_lookup = {1'b1, 5'd1, 2'd1};
            24'h860004: coeff_token_lookup = {1'b1, 5'd2, 2'd0};
            24'h860006: coeff_token_lookup = {1'b1, 5'd2, 2'd1};
            24'h830001: coeff_token_lookup = {1'b1, 5'd2, 2'd2};
            24'h860003: coeff_token_lookup = {1'b1, 5'd3, 2'd0};
            24'h870003: coeff_token_lookup = {1'b1, 5'd3, 2'd1};
            24'h870002: coeff_token_lookup = {1'b1, 5'd3, 2'd2};
            24'h860005: coeff_token_lookup = {1'b1, 5'd3, 2'd3};
            24'h860002: coeff_token_lookup = {1'b1, 5'd4, 2'd0};
            24'h880003: coeff_token_lookup = {1'b1, 5'd4, 2'd1};
            24'h880002: coeff_token_lookup = {1'b1, 5'd4, 2'd2};
            24'h870000: coeff_token_lookup = {1'b1, 5'd4, 2'd3};
            default: coeff_token_lookup = 8'd0;
            endcase
        end
    endfunction

    function automatic [4:0] total_zeros_lookup(input chroma, input [4:0] tc, input [3:0] len, input [8:0] bits);
        begin
            total_zeros_lookup = 5'd0;
            case ({chroma, tc, len, bits})
            19'h02201: total_zeros_lookup = {1'b1, 4'd0};
            19'h02603: total_zeros_lookup = {1'b1, 4'd1};
            19'h02602: total_zeros_lookup = {1'b1, 4'd2};
            19'h02803: total_zeros_lookup = {1'b1, 4'd3};
            19'h02802: total_zeros_lookup = {1'b1, 4'd4};
            19'h02a03: total_zeros_lookup = {1'b1, 4'd5};
            19'h02a02: total_zeros_lookup = {1'b1, 4'd6};
            19'h02c03: total_zeros_lookup = {1'b1, 4'd7};
            19'h02c02: total_zeros_lookup = {1'b1, 4'd8};
            19'h02e03: total_zeros_lookup = {1'b1, 4'd9};
            19'h02e02: total_zeros_lookup = {1'b1, 4'd10};
            19'h03003: total_zeros_lookup = {1'b1, 4'd11};
            19'h03002: total_zeros_lookup = {1'b1, 4'd12};
            19'h03203: total_zeros_lookup = {1'b1, 4'd13};
            19'h03202: total_zeros_lookup = {1'b1, 4'd14};
            19'h03201: total_zeros_lookup = {1'b1, 4'd15};
            19'h04607: total_zeros_lookup = {1'b1, 4'd0};
            19'h04606: total_zeros_lookup = {1'b1, 4'd1};
            19'h04605: total_zeros_lookup = {1'b1, 4'd2};
            19'h04604: total_zeros_lookup = {1'b1, 4'd3};
            19'h04603: total_zeros_lookup = {1'b1, 4'd4};
            19'h04805: total_zeros_lookup = {1'b1, 4'd5};
            19'h04804: total_zeros_lookup = {1'b1, 4'd6};
            19'h04803: total_zeros_lookup = {1'b1, 4'd7};
            19'h04802: total_zeros_lookup = {1'b1, 4'd8};
            19'h04a03: total_zeros_lookup = {1'b1, 4'd9};
            19'h04a02: total_zeros_lookup = {1'b1, 4'd10};
            19'h04c03: total_zeros_lookup = {1'b1, 4'd11};
            19'h04c02: total_zeros_lookup = {1'b1, 4'd12};
            19'h04c01: total_zeros_lookup = {1'b1, 4'd13};
            19'h04c00: total_zeros_lookup = {1'b1, 4'd14};
            19'h06805: total_zeros_lookup = {1'b1, 4'd0};
            19'h06607: total_zeros_lookup = {1'b1, 4'd1};
            19'h06606: total_zeros_lookup = {1'b1, 4'd2};
            19'h06605: total_zeros_lookup = {1'b1, 4'd3};
            19'h06804: total_zeros_lookup = {1'b1, 4'd4};
            19'h06803: total_zeros_lookup = {1'b1, 4'd5};
            19'h06604: total_zeros_lookup = {1'b1, 4'd6};
            19'h06603: total_zeros_lookup = {1'b1, 4'd7};
            19'h06802: total_zeros_lookup = {1'b1, 4'd8};
            19'h06a03: total_zeros_lookup = {1'b1, 4'd9};
            19'h06a02: total_zeros_lookup = {1'b1, 4'd10};
            19'h06c01: total_zeros_lookup = {1'b1, 4'd11};
            19'h06a01: total_zeros_lookup = {1'b1, 4'd12};
            19'h06c00: total_zeros_lookup = {1'b1, 4'd13};
            19'h08a03: total_zeros_lookup = {1'b1, 4'd0};
            19'h08607: total_zeros_lookup = {1'b1, 4'd1};
            19'h08805: total_zeros_lookup = {1'b1, 4'd2};
            19'h08804: total_zeros_lookup = {1'b1, 4'd3};
            19'h08606: total_zeros_lookup = {1'b1, 4'd4};
            19'h08605: total_zeros_lookup = {1'b1, 4'd5};
            19'h08604: total_zeros_lookup = {1'b1, 4'd6};
            19'h08803: total_zeros_lookup = {1'b1, 4'd7};
            19'h08603: total_zeros_lookup = {1'b1, 4'd8};
            19'h08802: total_zeros_lookup = {1'b1, 4'd9};
            19'h08a02: total_zeros_lookup = {1'b1, 4'd10};
            19'h08a01: total_zeros_lookup = {1'b1, 4'd11};
            19'h08a00: total_zeros_lookup = {1'b1, 4'd12};
            19'h0a805: total_zeros_lookup = {1'b1, 4'd0};
            19'h0a804: total_zeros_lookup = {1'b1, 4'd1};
            19'h0a803: total_zeros_lookup = {1'b1, 4'd2};
            19'h0a607: total_zeros_lookup = {1'b1, 4'd3};
            19'h0a606: total_zeros_lookup = {1'b1, 4'd4};
            19'h0a605: total_zeros_lookup = {1'b1, 4'd5};
            19'h0a604: total_zeros_lookup = {1'b1, 4'd6};
            19'h0a603: total_zeros_lookup = {1'b1, 4'd7};
            19'h0a802: total_zeros_lookup = {1'b1, 4'd8};
            19'h0aa01: total_zeros_lookup = {1'b1, 4'd9};
            19'h0a801: total_zeros_lookup = {1'b1, 4'd10};
            19'h0aa00: total_zeros_lookup = {1'b1, 4'd11};
            19'h0cc01: total_zeros_lookup = {1'b1, 4'd0};
            19'h0ca01: total_zeros_lookup = {1'b1, 4'd1};
            19'h0c607: total_zeros_lookup = {1'b1, 4'd2};
            19'h0c606: total_zeros_lookup = {1'b1, 4'd3};
            19'h0c605: total_zeros_lookup = {1'b1, 4'd4};
            19'h0c604: total_zeros_lookup = {1'b1, 4'd5};
            19'h0c603: total_zeros_lookup = {1'b1, 4'd6};
            19'h0c602: total_zeros_lookup = {1'b1, 4'd7};
            19'h0c801: total_zeros_lookup = {1'b1, 4'd8};
            19'h0c601: total_zeros_lookup = {1'b1, 4'd9};
            19'h0cc00: total_zeros_lookup = {1'b1, 4'd10};
            19'h0ec01: total_zeros_lookup = {1'b1, 4'd0};
            19'h0ea01: total_zeros_lookup = {1'b1, 4'd1};
            19'h0e605: total_zeros_lookup = {1'b1, 4'd2};
            19'h0e604: total_zeros_lookup = {1'b1, 4'd3};
            19'h0e603: total_zeros_lookup = {1'b1, 4'd4};
            19'h0e403: total_zeros_lookup = {1'b1, 4'd5};
            19'h0e602: total_zeros_lookup = {1'b1, 4'd6};
            19'h0e801: total_zeros_lookup = {1'b1, 4'd7};
            19'h0e601: total_zeros_lookup = {1'b1, 4'd8};
            19'h0ec00: total_zeros_lookup = {1'b1, 4'd9};
            19'h10c01: total_zeros_lookup = {1'b1, 4'd0};
            19'h10801: total_zeros_lookup = {1'b1, 4'd1};
            19'h10a01: total_zeros_lookup = {1'b1, 4'd2};
            19'h10603: total_zeros_lookup = {1'b1, 4'd3};
            19'h10403: total_zeros_lookup = {1'b1, 4'd4};
            19'h10402: total_zeros_lookup = {1'b1, 4'd5};
            19'h10602: total_zeros_lookup = {1'b1, 4'd6};
            19'h10601: total_zeros_lookup = {1'b1, 4'd7};
            19'h10c00: total_zeros_lookup = {1'b1, 4'd8};
            19'h12c01: total_zeros_lookup = {1'b1, 4'd0};
            19'h12c00: total_zeros_lookup = {1'b1, 4'd1};
            19'h12801: total_zeros_lookup = {1'b1, 4'd2};
            19'h12403: total_zeros_lookup = {1'b1, 4'd3};
            19'h12402: total_zeros_lookup = {1'b1, 4'd4};
            19'h12601: total_zeros_lookup = {1'b1, 4'd5};
            19'h12401: total_zeros_lookup = {1'b1, 4'd6};
            19'h12a01: total_zeros_lookup = {1'b1, 4'd7};
            19'h14a01: total_zeros_lookup = {1'b1, 4'd0};
            19'h14a00: total_zeros_lookup = {1'b1, 4'd1};
            19'h14601: total_zeros_lookup = {1'b1, 4'd2};
            19'h14403: total_zeros_lookup = {1'b1, 4'd3};
            19'h14402: total_zeros_lookup = {1'b1, 4'd4};
            19'h14401: total_zeros_lookup = {1'b1, 4'd5};
            19'h14801: total_zeros_lookup = {1'b1, 4'd6};
            19'h16800: total_zeros_lookup = {1'b1, 4'd0};
            19'h16801: total_zeros_lookup = {1'b1, 4'd1};
            19'h16601: total_zeros_lookup = {1'b1, 4'd2};
            19'h16602: total_zeros_lookup = {1'b1, 4'd3};
            19'h16201: total_zeros_lookup = {1'b1, 4'd4};
            19'h16603: total_zeros_lookup = {1'b1, 4'd5};
            19'h18800: total_zeros_lookup = {1'b1, 4'd0};
            19'h18801: total_zeros_lookup = {1'b1, 4'd1};
            19'h18401: total_zeros_lookup = {1'b1, 4'd2};
            19'h18201: total_zeros_lookup = {1'b1, 4'd3};
            19'h18601: total_zeros_lookup = {1'b1, 4'd4};
            19'h1a600: total_zeros_lookup = {1'b1, 4'd0};
            19'h1a601: total_zeros_lookup = {1'b1, 4'd1};
            19'h1a201: total_zeros_lookup = {1'b1, 4'd2};
            19'h1a401: total_zeros_lookup = {1'b1, 4'd3};
            19'h1c400: total_zeros_lookup = {1'b1, 4'd0};
            19'h1c401: total_zeros_lookup = {1'b1, 4'd1};
            19'h1c201: total_zeros_lookup = {1'b1, 4'd2};
            19'h1e200: total_zeros_lookup = {1'b1, 4'd0};
            19'h1e201: total_zeros_lookup = {1'b1, 4'd1};
            19'h42201: total_zeros_lookup = {1'b1, 4'd0};
            19'h42401: total_zeros_lookup = {1'b1, 4'd1};
            19'h42601: total_zeros_lookup = {1'b1, 4'd2};
            19'h42600: total_zeros_lookup = {1'b1, 4'd3};
            19'h44201: total_zeros_lookup = {1'b1, 4'd0};
            19'h44401: total_zeros_lookup = {1'b1, 4'd1};
            19'h44400: total_zeros_lookup = {1'b1, 4'd2};
            19'h46201: total_zeros_lookup = {1'b1, 4'd0};
            19'h46200: total_zeros_lookup = {1'b1, 4'd1};
            default: total_zeros_lookup = 5'd0;
            endcase
        end
    endfunction

    function automatic [4:0] run_before_lookup(input [3:0] zeros, input [3:0] len, input [4:0] bits);
        reg [3:0] row;
        begin
            row = (zeros < 4'd7) ? zeros : 4'd7;
            run_before_lookup = 5'd0;
            case ({row, len, bits})
            13'h0221: run_before_lookup = {1'b1, 4'd0};
            13'h0220: run_before_lookup = {1'b1, 4'd1};
            13'h0421: run_before_lookup = {1'b1, 4'd0};
            13'h0441: run_before_lookup = {1'b1, 4'd1};
            13'h0440: run_before_lookup = {1'b1, 4'd2};
            13'h0643: run_before_lookup = {1'b1, 4'd0};
            13'h0642: run_before_lookup = {1'b1, 4'd1};
            13'h0641: run_before_lookup = {1'b1, 4'd2};
            13'h0640: run_before_lookup = {1'b1, 4'd3};
            13'h0843: run_before_lookup = {1'b1, 4'd0};
            13'h0842: run_before_lookup = {1'b1, 4'd1};
            13'h0841: run_before_lookup = {1'b1, 4'd2};
            13'h0861: run_before_lookup = {1'b1, 4'd3};
            13'h0860: run_before_lookup = {1'b1, 4'd4};
            13'h0a43: run_before_lookup = {1'b1, 4'd0};
            13'h0a42: run_before_lookup = {1'b1, 4'd1};
            13'h0a63: run_before_lookup = {1'b1, 4'd2};
            13'h0a62: run_before_lookup = {1'b1, 4'd3};
            13'h0a61: run_before_lookup = {1'b1, 4'd4};
            13'h0a60: run_before_lookup = {1'b1, 4'd5};
            13'h0c43: run_before_lookup = {1'b1, 4'd0};
            13'h0c60: run_before_lookup = {1'b1, 4'd1};
            13'h0c61: run_before_lookup = {1'b1, 4'd2};
            13'h0c63: run_before_lookup = {1'b1, 4'd3};
            13'h0c62: run_before_lookup = {1'b1, 4'd4};
            13'h0c65: run_before_lookup = {1'b1, 4'd5};
            13'h0c64: run_before_lookup = {1'b1, 4'd6};
            13'h0e67: run_before_lookup = {1'b1, 4'd0};
            13'h0e66: run_before_lookup = {1'b1, 4'd1};
            13'h0e65: run_before_lookup = {1'b1, 4'd2};
            13'h0e64: run_before_lookup = {1'b1, 4'd3};
            13'h0e63: run_before_lookup = {1'b1, 4'd4};
            13'h0e62: run_before_lookup = {1'b1, 4'd5};
            13'h0e61: run_before_lookup = {1'b1, 4'd6};
            13'h0e81: run_before_lookup = {1'b1, 4'd7};
            13'h0ea1: run_before_lookup = {1'b1, 4'd8};
            13'h0ec1: run_before_lookup = {1'b1, 4'd9};
            13'h0ee1: run_before_lookup = {1'b1, 4'd10};
            13'h0f01: run_before_lookup = {1'b1, 4'd11};
            13'h0f21: run_before_lookup = {1'b1, 4'd12};
            13'h0f41: run_before_lookup = {1'b1, 4'd13};
            13'h0f61: run_before_lookup = {1'b1, 4'd14};
            default: run_before_lookup = 5'd0;
            endcase
        end
    endfunction

    function automatic signed [15:0] level_from_code(input [31:0] code_in);
        reg signed [15:0] mask;
        reg signed [15:0] t;
        reg [31:0] half;
        begin
            mask = code_in[0] ? -16'sd1 : 16'sd0;
            half = (code_in + 32'd2) >> 1;
            t = $signed(half[15:0]);
            level_from_code = (t ^ mask) - mask;
        end
    endfunction

    function automatic [3:0] idx4(input [4:0] v);
        begin
            idx4 = v[3:0];
        end
    endfunction

    // Match host residualBlock after first non-T1 level:
    //   escape → 2; else 1 + (unsigned(level+3) > 6)
    // BUGFIX: prior RTL used signed (lvl+3)>6, which never fires for
    // negative levels (e.g. -7) and desyncs suffixLength on real CB streams.
    function automatic [2:0] suffix_next_first(input [5:0] pfx, input [2:0] cur_suf, input signed [15:0] lvl);
        reg [15:0] lvl_plus_3_u;
        begin
            lvl_plus_3_u = lvl + 16'sd3; // bit pattern; compare unsigned below
            if (pfx > 6'd14 || (pfx == 6'd14 && cur_suf == 3'd0))
                suffix_next_first = 3'd2;
            else
                suffix_next_first = 3'd1 + (lvl_plus_3_u > 16'd6);
        end
    endfunction

    // Subsequent coeffs — match host residualBlock exactly:
    //   kLim[s] + unsigned(level) > 2*kLim[s]
    // (unsigned cast of a negative level is large → always increment; do NOT
    // use |level| here or synthetic encoder + host diverge).
    function automatic [2:0] suffix_next(input [2:0] cur_suf, input signed [15:0] lvl);
        reg [15:0] lim;
        begin
            case (cur_suf)
            3'd0: lim = 16'd0;
            3'd1: lim = 16'd3;
            3'd2: lim = 16'd6;
            3'd3: lim = 16'd12;
            3'd4: lim = 16'd24;
            default: lim = 16'd48;
            endcase
            suffix_next = cur_suf;
            if (cur_suf < 3'd6 && (lim + lvl[15:0]) > (lim << 1))
                suffix_next = cur_suf + 3'd1;
        end
    endfunction

    task automatic clear_arrays;
        integer i;
        begin
            for (i = 0; i < 16; i = i + 1) begin
                coeff[i] <= 16'sd0;
                level_dbg[i] <= 16'sd0;
                run_dbg[i] <= 4'd0;
            end
        end
    endtask

    always @(posedge clk) begin
        reg [7:0] tok;
        reg [4:0] zlk;
        reg [4:0] rlk;
        reg signed [15:0] lvl_tmp;
        reg signed [5:0] next_coeff_num;
        reg level_bad;
        integer ci;

        done <= 1'b0;
        if (reset) begin
            st <= ST_IDLE;
            busy <= 1'b0;
            ok <= 1'b0;
            done <= 1'b0;
            bit_pos <= 10'd0;
            bit_offset_end <= 10'd0;
            total_coeff <= 5'd0;
            trailing_ones <= 2'd0;
            total_zeros <= 4'd0;
            clear_arrays();
`ifdef CAVLC_CYCLE_PROBE
            cy_token <= 16'd0;
            cy_sign <= 16'd0;
            cy_level <= 16'd0;
            cy_total_zeros <= 16'd0;
            cy_run_before <= 16'd0;
            cy_place <= 16'd0;
            cy_other <= 16'd0;
            cy_total <= 16'd0;
`endif
        end else begin
`ifdef CAVLC_CYCLE_PROBE
            // Count this cycle's state before transitions (busy window).
            if (busy || start) begin
                cy_total <= cy_total + 16'd1;
                case (st)
                ST_TOKEN_BIT, ST_TOKEN_CHK: cy_token <= cy_token + 16'd1;
                ST_SIGN:                    cy_sign <= cy_sign + 16'd1;
                ST_LVL_PRE, ST_LVL_SUF, ST_LVL_STORE: cy_level <= cy_level + 16'd1;
                ST_TZ_BIT, ST_TZ_CHK:       cy_total_zeros <= cy_total_zeros + 16'd1;
                ST_RUN_BIT, ST_RUN_CHK:     cy_run_before <= cy_run_before + 16'd1;
                ST_PLACE_INIT, ST_PLACE_STEP: cy_place <= cy_place + 16'd1;
                default:                    cy_other <= cy_other + 16'd1;
                endcase
            end
`endif
            case (st)
            ST_IDLE: begin
                busy <= 1'b0;
                if (start) begin
                    busy <= 1'b1;
                    ok <= 1'b0;
                    bit_pos <= bit_offset_start;
                    bit_offset_end <= bit_offset_start;
                    code <= 16'd0;
                    code_len <= 5'd0;
                    total_coeff <= 5'd0;
                    trailing_ones <= 2'd0;
                    total_zeros <= 4'd0;
                    tc_r <= 5'd0;
                    t1_r <= 2'd0;
                    idx <= 5'd0;
                    clear_arrays();
`ifdef CAVLC_CYCLE_PROBE
                    cy_token <= 16'd0;
                    cy_sign <= 16'd0;
                    cy_level <= 16'd0;
                    cy_total_zeros <= 16'd0;
                    cy_run_before <= 16'd0;
                    cy_place <= 16'd0;
                    cy_other <= 16'd0;
                    cy_total <= 16'd0;
`endif
                    st <= ST_TOKEN_BIT;
                end
            end

            // Parallel prefix-free token: try lengths 1..max in one cycle.
            ST_TOKEN_BIT: begin
                if (bit_pos >= bit_len) st <= ST_FAIL;
                else begin : g_tok_par
                    reg [31:0] twin;
                    reg [31:0] tbits;
                    reg [4:0] tmax, tlen, blen;
                    reg [7:0] tcand, tbest;
                    reg found_t;
                    integer ti;
                    twin = peek32(bit_pos);
                    tmax = (coeff_token_table == 3'd3) ? 5'd6 :
                           (coeff_token_table == 3'd4) ? 5'd8 : 5'd16;
                    found_t = 1'b0;
                    tbest = 8'd0;
                    blen = 5'd0;
                    for (ti = 1; ti <= 16; ti = ti + 1) begin
                        tlen = ti[4:0];
                        if (tlen <= tmax && (bit_pos + {5'd0, tlen}) <= bit_len) begin
                            // Quartus rejects bit-select on function call results.
                            tbits = take_msb(twin, {1'b0, tlen});
                            tcand = coeff_token_lookup(coeff_token_table, tlen, tbits[15:0]);
                            if (tcand[7] && !found_t) begin
                                found_t = 1'b1;
                                tbest = tcand;
                                blen = tlen;
                            end
                        end
                    end
                    if (!found_t) st <= ST_FAIL;
                    else begin
                        bit_pos <= bit_pos + {5'd0, blen};
                        tc_r <= tbest[6:2];
                        t1_r <= tbest[1:0];
                        total_coeff <= tbest[6:2];
                        trailing_ones <= tbest[1:0];
                        if (tbest[6:2] > max_coeff || {3'd0, tbest[1:0]} > tbest[6:2])
                            st <= ST_FAIL;
                        else if (tbest[6:2] == 5'd0) begin
                            ok <= 1'b1;
                            bit_offset_end <= bit_pos + {5'd0, blen};
                            st <= ST_DONE;
                        end else begin
                            idx <= 5'd0;
                            if (tbest[1:0] != 2'd0) st <= ST_SIGN;
                            else begin
                                suffix_length <= (tbest[6:2] > 5'd10) ? 3'd1 : 3'd0;
                                first_non_t1 <= 1'b1;
                                prefix <= 6'd0;
                                st <= ST_LVL_PRE;
                            end
                        end
                    end
                end
            end

            ST_TOKEN_CHK: st <= ST_FAIL;

            // All trailing-one signs in one cycle (t1 ≤ 3).
            ST_SIGN: begin
                if (bit_pos + {8'd0, t1_r} > bit_len) st <= ST_FAIL;
                else begin
                    // peek up to 3 sign bits
                    begin : g_signs
                        reg [31:0] sw;
                        sw = peek32(bit_pos);
                        if (t1_r >= 2'd1)
                            level_dbg[0] <= sw[31] ? -16'sd1 : 16'sd1;
                        if (t1_r >= 2'd2)
                            level_dbg[1] <= sw[30] ? -16'sd1 : 16'sd1;
                        if (t1_r >= 2'd3)
                            level_dbg[2] <= sw[29] ? -16'sd1 : 16'sd1;
                    end
                    bit_pos <= bit_pos + {8'd0, t1_r};
                    idx <= {3'd0, t1_r};
                    if (tc_r == {3'd0, t1_r}) begin
                        code <= 16'd0; code_len <= 5'd0; st <= ST_TZ_BIT;
                    end else begin
                        suffix_length <= (tc_r > 5'd10 && t1_r < 2'd3) ? 3'd1 : 3'd0;
                        first_non_t1 <= 1'b1;
                        prefix <= 6'd0;
                        st <= ST_LVL_PRE;
                    end
                end
            end

            // Up to three non-T1 levels per cycle (suffix_length chained).
            ST_LVL_PRE: begin
                if (bit_pos >= bit_len) st <= ST_FAIL;
                else begin : g_lvl_tri
                    reg [31:0] win_a, win_b, win_c, sfx_a, sfx_b, sfx_c;
                    reg [5:0] pfx_a, pfx_b, pfx_c;
                    reg [9:0] avail_a, cons_a, cons_b, cons_c, bpos_b, bpos_c;
                    reg [4:0] sleft_a, sleft_b, sleft_c;
                    reg [2:0] sl_a, sl_b, sl_c, sl_cur;
                    reg signed [15:0] lvl_a, lvl_b, lvl_c;
                    reg do_b, do_c, bad_a, bad_b, bad_c, first_a;
                    reg [31:0] lc_a, lc_b, lc_c;
                    reg [1:0] n_ok;

                    first_a = first_non_t1;
                    sl_cur = suffix_length;
                    win_a = peek32(bit_pos);
                    pfx_a = clz32(win_a);
                    avail_a = bit_len - bit_pos;
                    do_b = 1'b0;
                    do_c = 1'b0;
                    bad_a = 1'b1;
                    bad_b = 1'b0;
                    bad_c = 1'b0;
                    cons_a = 10'd0;
                    cons_b = 10'd0;
                    cons_c = 10'd0;
                    lvl_a = 16'sd0;
                    lvl_b = 16'sd0;
                    lvl_c = 16'sd0;
                    sl_a = sl_cur;
                    sl_b = sl_cur;
                    sl_c = sl_cur;
                    n_ok = 2'd0;

                    if (pfx_a >= 6'd32 || {6'd0, pfx_a} >= {2'd0, avail_a} || pfx_a > 6'd31) begin
                        st <= ST_FAIL;
                    end else begin
                        if (first_a) begin
                            if (pfx_a < 6'd14) sleft_a = {2'd0, sl_cur};
                            else if (pfx_a == 6'd14) sleft_a = (sl_cur != 0) ? {2'd0, sl_cur} : 5'd4;
                            else sleft_a = pfx_a[4:0] - 5'd3;
                        end else begin
                            if (pfx_a < 6'd15) sleft_a = {2'd0, sl_cur};
                            else sleft_a = pfx_a[4:0] - 5'd3;
                        end
                        cons_a = {4'd0, pfx_a} + 10'd1 + {5'd0, sleft_a};

                        if (bit_pos + cons_a > bit_len) begin
                            st <= ST_FAIL;
                        end else if (({1'b0, pfx_a} + 7'd1 + {1'b0, sleft_a}) > 7'd32) begin
                            prefix <= pfx_a;
                            bit_pos <= bit_pos + {4'd0, pfx_a} + 10'd1;
                            suffix_left <= sleft_a;
                            suffix_acc <= 32'd0;
                            st <= ST_LVL_SUF;
                        end else begin
                            sfx_a = take_msb(win_a << ({4'd0, pfx_a} + 10'd1), {1'b0, sleft_a});
                            if (first_a) begin
                                if (pfx_a < 6'd14)
                                    lc_a = ({26'd0, pfx_a} << sl_cur) + sfx_a;
                                else if (pfx_a == 6'd14) begin
                                    if (sl_cur != 0)
                                        lc_a = (32'd14 << sl_cur) + sfx_a;
                                    else
                                        lc_a = 32'd14 + sfx_a;
                                end else begin
                                    lc_a = 32'd30;
                                    if (pfx_a >= 6'd16)
                                        lc_a = lc_a + (32'd1 << (pfx_a - 6'd3)) - 32'd4096;
                                    lc_a = lc_a + sfx_a;
                                end
                                if (t1_r < 2'd3) lc_a = lc_a + 32'd2;
                            end else begin
                                if (pfx_a < 6'd15)
                                    lc_a = ({26'd0, pfx_a} << sl_cur) + sfx_a;
                                else begin
                                    lc_a = (32'd15 << sl_cur);
                                    if (pfx_a >= 6'd16)
                                        lc_a = lc_a + (32'd1 << (pfx_a - 6'd3)) - 32'd4096;
                                    lc_a = lc_a + sfx_a;
                                end
                            end
                            bad_a = (idx >= 5'd16 || lc_a > 32'd65535);
                            if (!bad_a) begin
                                lvl_a = level_from_code(lc_a);
                                sl_a = first_a ? suffix_next_first(pfx_a, sl_cur, lvl_a)
                                               : suffix_next(sl_cur, lvl_a);
                                n_ok = 2'd1;
                            end

                            // Level B
                            if (!bad_a && (idx + 5'd1 < tc_r) &&
                                (bit_pos + cons_a < bit_len)) begin
                                bpos_b = bit_pos + cons_a;
                                win_b = peek32(bpos_b);
                                pfx_b = clz32(win_b);
                                if (!(pfx_b >= 6'd32 ||
                                      {6'd0, pfx_b} >= {2'd0, (bit_len - bpos_b)} ||
                                      pfx_b > 6'd31)) begin
                                    if (pfx_b < 6'd15) sleft_b = {2'd0, sl_a};
                                    else sleft_b = pfx_b[4:0] - 5'd3;
                                    cons_b = {4'd0, pfx_b} + 10'd1 + {5'd0, sleft_b};
                                    if ((bpos_b + cons_b <= bit_len) &&
                                        (({1'b0, pfx_b} + 7'd1 + {1'b0, sleft_b}) <= 7'd32)) begin
                                        sfx_b = take_msb(win_b << ({4'd0, pfx_b} + 10'd1),
                                                         {1'b0, sleft_b});
                                        if (pfx_b < 6'd15)
                                            lc_b = ({26'd0, pfx_b} << sl_a) + sfx_b;
                                        else begin
                                            lc_b = (32'd15 << sl_a);
                                            if (pfx_b >= 6'd16)
                                                lc_b = lc_b + (32'd1 << (pfx_b - 6'd3)) - 32'd4096;
                                            lc_b = lc_b + sfx_b;
                                        end
                                        bad_b = ((idx + 5'd1) >= 5'd16 || lc_b > 32'd65535);
                                        if (!bad_b) begin
                                            lvl_b = level_from_code(lc_b);
                                            sl_b = suffix_next(sl_a, lvl_b);
                                            do_b = 1'b1;
                                            n_ok = 2'd2;
                                        end
                                    end
                                end
                            end

                            // Level C
                            if (do_b && !bad_b && (idx + 5'd2 < tc_r)) begin
                                bpos_c = bit_pos + cons_a + cons_b;
                                if (bpos_c < bit_len) begin
                                    win_c = peek32(bpos_c);
                                    pfx_c = clz32(win_c);
                                    if (!(pfx_c >= 6'd32 ||
                                          {6'd0, pfx_c} >= {2'd0, (bit_len - bpos_c)} ||
                                          pfx_c > 6'd31)) begin
                                        if (pfx_c < 6'd15) sleft_c = {2'd0, sl_b};
                                        else sleft_c = pfx_c[4:0] - 5'd3;
                                        cons_c = {4'd0, pfx_c} + 10'd1 + {5'd0, sleft_c};
                                        if ((bpos_c + cons_c <= bit_len) &&
                                            (({1'b0, pfx_c} + 7'd1 + {1'b0, sleft_c}) <= 7'd32)) begin
                                            sfx_c = take_msb(win_c << ({4'd0, pfx_c} + 10'd1),
                                                             {1'b0, sleft_c});
                                            if (pfx_c < 6'd15)
                                                lc_c = ({26'd0, pfx_c} << sl_b) + sfx_c;
                                            else begin
                                                lc_c = (32'd15 << sl_b);
                                                if (pfx_c >= 6'd16)
                                                    lc_c = lc_c + (32'd1 << (pfx_c - 6'd3)) - 32'd4096;
                                                lc_c = lc_c + sfx_c;
                                            end
                                            bad_c = ((idx + 5'd2) >= 5'd16 || lc_c > 32'd65535);
                                            if (!bad_c) begin
                                                lvl_c = level_from_code(lc_c);
                                                sl_c = suffix_next(sl_b, lvl_c);
                                                do_c = 1'b1;
                                                n_ok = 2'd3;
                                            end
                                        end
                                    end
                                end
                            end

                            if (bad_a || bad_b || bad_c) begin
                                st <= ST_FAIL;
                            end else begin
                                first_non_t1 <= 1'b0;
                                if (n_ok == 2'd3) begin
                                    level_dbg[idx[3:0]] <= lvl_a;
                                    level_dbg[idx[3:0] + 4'd1] <= lvl_b;
                                    level_dbg[idx[3:0] + 4'd2] <= lvl_c;
                                    prefix <= pfx_c;
                                    suffix_acc <= sfx_c;
                                    suffix_length <= sl_c;
                                    bit_pos <= bit_pos + cons_a + cons_b + cons_c;
                                    if (idx + 5'd3 >= tc_r) begin
                                        code <= 16'd0; code_len <= 5'd0; st <= ST_TZ_BIT;
                                    end else begin
                                        idx <= idx + 5'd3;
                                        st <= ST_LVL_PRE;
                                    end
                                end else if (n_ok == 2'd2) begin
                                    level_dbg[idx[3:0]] <= lvl_a;
                                    level_dbg[idx[3:0] + 4'd1] <= lvl_b;
                                    prefix <= pfx_b;
                                    suffix_acc <= sfx_b;
                                    suffix_length <= sl_b;
                                    bit_pos <= bit_pos + cons_a + cons_b;
                                    if (idx + 5'd2 >= tc_r) begin
                                        code <= 16'd0; code_len <= 5'd0; st <= ST_TZ_BIT;
                                    end else begin
                                        idx <= idx + 5'd2;
                                        st <= ST_LVL_PRE;
                                    end
                                end else begin
                                    level_dbg[idx[3:0]] <= lvl_a;
                                    prefix <= pfx_a;
                                    suffix_acc <= sfx_a;
                                    suffix_length <= sl_a;
                                    bit_pos <= bit_pos + cons_a;
                                    if (idx + 5'd1 >= tc_r) begin
                                        code <= 16'd0; code_len <= 5'd0; st <= ST_TZ_BIT;
                                    end else begin
                                        idx <= idx + 5'd1;
                                        st <= ST_LVL_PRE;
                                    end
                                end
                            end
                        end
                    end
                end
            end

            // Long-escape suffix tail (prefix already consumed).
            ST_LVL_SUF: begin
                if (suffix_left == 5'd0) begin
                    st <= ST_LVL_STORE;
                end else if (bit_pos + {5'd0, suffix_left} > bit_len) st <= ST_FAIL;
                else begin : g_long_suf
                    reg [31:0] win2;
                    win2 = peek32(bit_pos);
                    suffix_acc <= take_msb(win2, {1'b0, suffix_left});
                    bit_pos <= bit_pos + {5'd0, suffix_left};
                    suffix_left <= 5'd0;
                    st <= ST_LVL_STORE;
                end
            end

            // Complete level after long-escape suffix path only.
            ST_LVL_STORE: begin
                level_bad = 1'b0;
                if (first_non_t1) begin
                    if (prefix < 6'd14)
                        level_code = ({26'd0, prefix} << suffix_length) + suffix_acc;
                    else if (prefix == 6'd14) begin
                        if (suffix_length != 0)
                            level_code = (32'd14 << suffix_length) + suffix_acc;
                        else
                            level_code = 32'd14 + suffix_acc;
                    end else begin
                        level_code = 32'd30;
                        if (prefix >= 6'd16)
                            level_code = level_code + (32'd1 << (prefix - 6'd3)) - 32'd4096;
                        level_code = level_code + suffix_acc;
                    end
                    if (t1_r < 2'd3) level_code = level_code + 32'd2;
                    level_bad = (idx >= 5'd16 || level_code > 32'd65535);
                    if (!level_bad) begin
                        lvl_tmp = level_from_code(level_code);
                        level_dbg[idx[3:0]] <= lvl_tmp;
                        suffix_length <= suffix_next_first(prefix, suffix_length, lvl_tmp);
                        first_non_t1 <= 1'b0;
                    end
                end else begin
                    if (prefix < 6'd15)
                        level_code = ({26'd0, prefix} << suffix_length) + suffix_acc;
                    else begin
                        level_code = (32'd15 << suffix_length);
                        if (prefix >= 6'd16)
                            level_code = level_code + (32'd1 << (prefix - 6'd3)) - 32'd4096;
                        level_code = level_code + suffix_acc;
                    end
                    level_bad = (idx >= 5'd16 || level_code > 32'd65535);
                    if (!level_bad) begin
                        lvl_tmp = level_from_code(level_code);
                        level_dbg[idx[3:0]] <= lvl_tmp;
                        suffix_length <= suffix_next(suffix_length, lvl_tmp);
                    end
                end
                if (level_bad) st <= ST_FAIL;
                else if (idx + 5'd1 >= tc_r) begin
                    code <= 16'd0; code_len <= 5'd0; st <= ST_TZ_BIT;
                end else begin
                    idx <= idx + 5'd1;
                    st <= ST_LVL_PRE;
                end
            end

            ST_TZ_BIT: begin
                if (tc_r >= max_coeff) begin
                    total_zeros <= 4'd0;
                    zeros_left <= 4'd0;
                    idx <= 5'd0;
                    st <= ST_PLACE_INIT;
                end else if (bit_pos >= bit_len) st <= ST_FAIL;
                else begin : g_tz_par
                    reg [31:0] zwin;
                    reg [31:0] zbits;
                    reg [4:0] zmax, zlen, zbest_len;
                    reg [4:0] zcand, zbest;
                    reg found_z;
                    integer zi;
                    zwin = peek32(bit_pos);
                    zmax = tz_is_chroma ? 5'd3 : 5'd9;
                    found_z = 1'b0;
                    zbest = 5'd0;
                    zbest_len = 5'd0;
                    for (zi = 1; zi <= 9; zi = zi + 1) begin
                        zlen = zi[4:0];
                        if (zlen <= zmax && (bit_pos + {5'd0, zlen}) <= bit_len) begin
                            zbits = take_msb(zwin, {1'b0, zlen});
                            zcand = total_zeros_lookup(tz_is_chroma, tc_r, zlen[3:0], zbits[8:0]);
                            if (zcand[4] && !found_z) begin
                                found_z = 1'b1;
                                zbest = zcand;
                                zbest_len = zlen;
                            end
                        end
                    end
                    if (!found_z) st <= ST_FAIL;
                    else begin
                        bit_pos <= bit_pos + {5'd0, zbest_len};
                        total_zeros <= zbest[3:0];
                        zeros_left <= zbest[3:0];
                        idx <= 5'd0;
                        code <= 16'd0; code_len <= 5'd0;
                        if (tc_r <= 5'd1 || zbest[3:0] == 4'd0) begin
                            run_dbg[idx4(tc_r - 5'd1)] <= zbest[3:0];
                            st <= ST_PLACE_INIT;
                        end else st <= ST_RUN_BIT;
                    end
                end
            end

            ST_TZ_CHK: st <= ST_FAIL;

            // Up to two run_before symbols/cycle (zeros_left chained).
            ST_RUN_BIT: begin
                if (idx >= tc_r - 5'd1 || zeros_left == 4'd0) begin
                    run_dbg[idx4(tc_r - 5'd1)] <= zeros_left;
                    st <= ST_PLACE_INIT;
                end else if (bit_pos >= bit_len) st <= ST_FAIL;
                else begin : g_run_pair
                    reg [31:0] rwin_a, rwin_b;
                    reg [31:0] rbits_a, rbits_b;
                    reg [4:0] rmax_a, rmax_b, rlen, rblen_a, rblen_b;
                    reg [4:0] rcand, rbest_a, rbest_b;
                    reg found_a, found_b, do_b;
                    reg [3:0] zl_a, zl_b, run_a, run_b;
                    reg [9:0] bpos_a;
                    integer ri;

                    rwin_a = peek32(bit_pos);
                    rmax_a = (zeros_left < 4'd7) ? 5'd3 : 5'd11;
                    found_a = 1'b0;
                    rbest_a = 5'd0;
                    rblen_a = 5'd0;
                    for (ri = 1; ri <= 11; ri = ri + 1) begin
                        rlen = ri[4:0];
                        if (rlen <= rmax_a && (bit_pos + {5'd0, rlen}) <= bit_len) begin
                            rbits_a = take_msb(rwin_a, {1'b0, rlen});
                            rcand = run_before_lookup(zeros_left, rlen[3:0], rbits_a[4:0]);
                            if (rcand[4] && !found_a) begin
                                found_a = 1'b1;
                                rbest_a = rcand;
                                rblen_a = rlen;
                            end
                        end
                    end

                    do_b = 1'b0;
                    found_b = 1'b0;
                    rbest_b = 5'd0;
                    rblen_b = 5'd0;
                    run_a = 4'd0;
                    run_b = 4'd0;
                    zl_a = zeros_left;
                    zl_b = zeros_left;
                    bpos_a = bit_pos;

                    if (!found_a) st <= ST_FAIL;
                    else if (rbest_a[3:0] > zeros_left) st <= ST_FAIL;
                    else begin
                        run_a = rbest_a[3:0];
                        zl_a = zeros_left - run_a;
                        bpos_a = bit_pos + {5'd0, rblen_a};

                        // Second run if more coeffs remain and zeros remain.
                        if (idx + 5'd1 < tc_r - 5'd1 && zl_a != 4'd0 && bpos_a < bit_len) begin
                            rwin_b = peek32(bpos_a);
                            rmax_b = (zl_a < 4'd7) ? 5'd3 : 5'd11;
                            for (ri = 1; ri <= 11; ri = ri + 1) begin
                                rlen = ri[4:0];
                                if (rlen <= rmax_b && (bpos_a + {5'd0, rlen}) <= bit_len) begin
                                    rbits_b = take_msb(rwin_b, {1'b0, rlen});
                                    rcand = run_before_lookup(zl_a, rlen[3:0], rbits_b[4:0]);
                                    if (rcand[4] && !found_b) begin
                                        found_b = 1'b1;
                                        rbest_b = rcand;
                                        rblen_b = rlen;
                                    end
                                end
                            end
                            if (found_b && rbest_b[3:0] <= zl_a) begin
                                run_b = rbest_b[3:0];
                                zl_b = zl_a - run_b;
                                do_b = 1'b1;
                            end
                        end

                        code <= 16'd0; code_len <= 5'd0;
                        if (do_b) begin
                            run_dbg[idx[3:0]] <= run_a;
                            run_dbg[idx[3:0] + 4'd1] <= run_b;
                            zeros_left <= zl_b;
                            bit_pos <= bpos_a + {5'd0, rblen_b};
                            if (idx + 5'd2 >= tc_r - 5'd1 || zl_b == 4'd0) begin
                                run_dbg[idx4(tc_r - 5'd1)] <= zl_b;
                                st <= ST_PLACE_INIT;
                            end else begin
                                idx <= idx + 5'd2;
                                st <= ST_RUN_BIT;
                            end
                        end else begin
                            run_dbg[idx[3:0]] <= run_a;
                            zeros_left <= zl_a;
                            bit_pos <= bpos_a;
                            if (idx + 5'd1 >= tc_r - 5'd1 || zl_a == 4'd0) begin
                                run_dbg[idx4(tc_r - 5'd1)] <= zl_a;
                                st <= ST_PLACE_INIT;
                            end else begin
                                idx <= idx + 5'd1;
                                st <= ST_RUN_BIT;
                            end
                        end
                    end
                end
            end

            ST_RUN_CHK: st <= ST_FAIL;

            // Place all coeffs in one cycle. Order matches legacy ST_PLACE_STEP:
            // walk idx = tc-1 .. 0 (reverse of decode order).
            ST_PLACE_INIT: begin : g_place_all
                reg signed [5:0] cnum;
                reg fail_p;
                integer pi;
                for (ci = 0; ci < 16; ci = ci + 1)
                    coeff[ci] <= 16'sd0;
                cnum = -6'sd1;
                fail_p = 1'b0;
                for (pi = 16; pi >= 1; pi = pi - 1) begin
                    if (pi <= tc_r && !fail_p) begin
                        cnum = cnum + {2'd0, run_dbg[idx4(5'(pi - 1))]} + 6'sd1;
                        if (cnum < 6'sd16)
                            coeff[cnum[3:0]] <= level_dbg[idx4(5'(pi - 1))];
                        else
                            fail_p = 1'b1;
                    end
                end
                if (fail_p) st <= ST_FAIL;
                else begin
                    ok <= 1'b1;
                    bit_offset_end <= bit_pos;
                    st <= ST_DONE;
                end
            end

            ST_PLACE_STEP: st <= ST_FAIL; // unused (bulk place)

            ST_DONE: begin
                busy <= 1'b0;
                done <= 1'b1;
                st <= ST_IDLE;
            end

            default: begin
                busy <= 1'b0;
                ok <= 1'b0;
                bit_offset_end <= bit_pos;
                done <= 1'b1;
                st <= ST_IDLE;
            end
            endcase
        end
    end
endmodule
