// h264_decode_top testbench — drives golden MB data through the real decode
// datapath and verifies full-macroblock reconstruction.
//
// This is the INTEGRATED pipeline test — not injection into isolated modules.
// The number it produces is INTEGRATED_PIPELINE_COVERAGE.

`default_nettype none

module h264_decode_top_tb (
    input wire clk,
    input wire reset,

    // Golden injection interface (from C++ driver)
    input  wire        tb_mb_start,
    input  wire [7:0]  tb_mb_type,
    input  wire [5:0]  tb_mb_qp_y,
    input  wire [7:0]  tb_mb_x,
    input  wire [7:0]  tb_mb_y,
    input  wire [1:0]  tb_i16_pred_mode,

    input  wire        tb_block_valid,
    input  wire [3:0]  tb_block_index,
    input  wire signed [8:0] tb_block_coeff [0:15],

    input  wire        tb_i16_dc_valid,
    input  wire signed [17:0] tb_i16_dc [0:15],

    input  wire [3:0]  tb_i4_modes [0:15],

    input  wire        tb_avail_left,
    input  wire        tb_avail_top,
    input  wire        tb_avail_topright,
    input  wire        tb_avail_topleft,
    input  wire [7:0]  tb_nb_top [0:15],
    input  wire [7:0]  tb_nb_left [0:15],
    input  wire [7:0]  tb_nb_topleft,
    input  wire [7:0]  tb_nb_topright [0:3],

    // Outputs to C++ driver
    output wire        tb_mb_recon_valid,
    output wire [7:0]  tb_recon_y [0:255],
    output wire [4:0]  tb_blocks_done
);

    h264_decode_top u_dut (
        .clk(clk),
        .reset(reset),
        .mb_start(tb_mb_start),
        .mb_type(tb_mb_type),
        .mb_qp_y(tb_mb_qp_y),
        .mb_x(tb_mb_x),
        .mb_y(tb_mb_y),
        .i16_pred_mode(tb_i16_pred_mode),
        .block_valid(tb_block_valid),
        .block_index(tb_block_index),
        .block_coeff(tb_block_coeff),
        .i16_dc_valid(tb_i16_dc_valid),
        .i16_dc(tb_i16_dc),
        .i4_modes(tb_i4_modes),
        .mb_avail_left(tb_avail_left),
        .mb_avail_top(tb_avail_top),
        .mb_avail_topright(tb_avail_topright),
        .mb_avail_topleft(tb_avail_topleft),
        .nb_top(tb_nb_top),
        .nb_left(tb_nb_left),
        .nb_topleft(tb_nb_topleft),
        .nb_topright(tb_nb_topright),
        .mb_recon_valid(tb_mb_recon_valid),
        .recon_y(tb_recon_y),
        .blocks_done(tb_blocks_done)
    );

endmodule

`default_nettype wire
