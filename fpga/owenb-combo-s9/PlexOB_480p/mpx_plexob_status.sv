// MPX-FORK: MiSTerPlex status words in owenb ring ctrl CORE3 (80-87).
// P0 scaffold — not wired into ring_stat yet; documents the daemon-visible layout.
`default_nettype none

module mpx_plexob_status #(
    parameter logic [31:0] BUILD_ID = 32'h0B59C11A
) (
    input  wire        clk,
    input  wire        rst_n,
    output logic [31:0] word80_magic,   // 0x4F424D50 "OBMP"
    output logic [31:0] word81_build_id,
    output logic [31:0] word82_mode,    // 0 idle 1 play 2 error (daemon via HPS later)
    output logic [31:0] word83_reserved
);
    localparam logic [31:0] kMagic = 32'h4F424D50;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            word80_magic <= '0;
            word81_build_id <= '0;
            word82_mode <= '0;
            word83_reserved <= '0;
        end else begin
            word80_magic <= kMagic;
            word81_build_id <= BUILD_ID;
            word82_mode <= '0;
            word83_reserved <= '0;
        end
    end
endmodule

`default_nettype wire
