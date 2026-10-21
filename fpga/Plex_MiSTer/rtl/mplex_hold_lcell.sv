// Timing hold pad (identity LUT). Shared by ddr_bus_arbiter / ddr_frame_store.
// VERILATOR: pure assign. Quartus: cyclonev_lcell_comb dont_touch.
module mplex_hold_lcell (
	input  wire din,
	output wire dout
);
`ifdef VERILATOR
	assign dout = din;
`else
	cyclonev_lcell_comb #(
		.lut_mask(64'hAAAAAAAAAAAAAAAA),
		.dont_touch("on")
	) hold_lcell (
		.dataa(din),
		.datab(1'b0),
		.datac(1'b0),
		.datad(1'b0),
		.datae(1'b0),
		.dataf(1'b0),
		.combout(dout)
	);
`endif
endmodule
