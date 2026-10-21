// MPX-FORK: PlexOB P0 — keep MiSTerPlex shim RTL in the emu netlist.
`default_nettype none

module mpx_plexob_core_hook #(
    parameter logic [31:0] BUILD_ID = 32'h0B59C11A
) (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        probe_valid,
    input  wire [63:0] probe_nonce,
    output logic       led_mpx,
    output logic [31:0] status_build_id,
    output logic [31:0] caps_build_id
);
    logic [31:0] w80, w81, w82, w83;
    logic [63:0] mpc0, mpc1, mpc2, mpc3, mpc4, mpc5, mpc6, mpc7;
    logic        caps_valid;

    mpx_plexob_status #(
        .BUILD_ID(BUILD_ID)
    ) u_status (
        .clk(clk),
        .rst_n(rst_n),
        .word80_magic(w80),
        .word81_build_id(w81),
        .word82_mode(w82),
        .word83_reserved(w83)
    );

    mpx_mister_caps_shim #(
        .BUILD_ID(BUILD_ID)
    ) u_caps (
        .clk(clk),
        .rst_n(rst_n),
        .probe_valid(probe_valid),
        .probe_nonce(probe_nonce),
        .mpc_word0(mpc0),
        .mpc_word1(mpc1),
        .mpc_word2(mpc2),
        .mpc_word3(mpc3),
        .mpc_word4(mpc4),
        .mpc_word5(mpc5),
        .mpc_word6(mpc6),
        .mpc_word7(mpc7),
        .caps_valid(caps_valid)
    );

    assign status_build_id = w81;
    assign caps_build_id = mpc4[63:32];
    assign led_mpx = caps_valid | (w80 == 32'h4F424D50);
endmodule

`default_nettype wire
