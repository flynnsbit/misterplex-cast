// MPX-FORK: MiSTerPlex CAPS publisher for PlexOB_* (absolute 0x30140050..88).
// Mirrors ddr_bitstream_reader.sv TX_CAPS / tests/unit/fake_plex_core.hpp answerProbe().
// P0: standalone module; wire to HPS lightweight writer + owenb probe hook in a later fit.
`default_nettype none

module mpx_mister_caps_shim #(
    parameter int unsigned ABI_VERSION = 2,
    parameter int unsigned LAYOUT_ID = 1,
    parameter logic [31:0] FEATURES = 32'h0000287F, // host owenb_caps_plan.hpp kPlexOb480pCapsFeatures
    parameter int unsigned MAX_WIDTH = 640,
    parameter int unsigned MAX_HEIGHT = 480,
    parameter logic [31:0] MAX_AU_BYTES = 32'd131072,
    parameter logic [31:0] BUILD_ID = 32'h0B59C11A
) (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        probe_valid,
    input  wire [63:0] probe_nonce,
    output logic [63:0] mpc_word0,
    output logic [63:0] mpc_word1,
    output logic [63:0] mpc_word2,
    output logic [63:0] mpc_word3,
    output logic [63:0] mpc_word4,
    output logic [63:0] mpc_word5,
    output logic [63:0] mpc_word6,
    output logic [63:0] mpc_word7,
    output logic        caps_valid
);
    localparam logic [31:0] kMpc0 = 32'h4D504330;
    localparam logic [31:0] kMpc1 = 32'h4D504331;
    localparam logic [31:0] kMpc2 = 32'h4D504332;
    localparam logic [31:0] kMpc3 = 32'h4D504333;
    localparam logic [31:0] kMpc4 = 32'h4D504334;
    localparam logic [31:0] kMpc5 = 32'h4D504335;
    localparam logic [31:0] kMpc6 = 32'h4D504336;
    localparam logic [31:0] kMpc7 = 32'h4D504337;

    logic [31:0] publication;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            publication <= '0;
            caps_valid <= 1'b0;
            mpc_word0 <= '0;
            mpc_word1 <= '0;
            mpc_word2 <= '0;
            mpc_word3 <= '0;
            mpc_word4 <= '0;
            mpc_word5 <= '0;
            mpc_word6 <= '0;
            mpc_word7 <= '0;
        end else if (probe_valid) begin
            publication <= publication + 1'b1;
            caps_valid <= 1'b1;
            mpc_word7 <= {32'b0, kMpc7}; // invalidate first
            mpc_word0 <= {LAYOUT_ID[15:0], ABI_VERSION[15:0], kMpc0};
            mpc_word1 <= {FEATURES, kMpc1};
            mpc_word2 <= {MAX_HEIGHT[15:0], MAX_WIDTH[15:0], kMpc2};
            mpc_word3 <= {MAX_AU_BYTES, kMpc3};
            mpc_word4 <= {BUILD_ID, kMpc4};
            mpc_word5 <= {probe_nonce[31:0], kMpc5};
            mpc_word6 <= {probe_nonce[63:32], kMpc6};
            mpc_word7 <= {publication + 1'b1, kMpc7};
        end
    end
endmodule

`default_nettype wire
