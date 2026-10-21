// Phase 3.3: single-clock BRAM ring for elementary bitstream (H.264 annex-B).
// Quartus 17 template: separate write always, registered read always, noprune mem.

module bitstream_fifo #(
	parameter int DEPTH = 32768
)(
	input  wire        clk,
	input  wire        reset,

	input  wire        wr_en,
	input  wire [7:0]  wr_data,
	input  wire        wr_flush,
	output wire        wr_full,
	output reg  [15:0] wr_level,

	input  wire        rd_en,
	output reg  [7:0]  rd_data,
	output wire        rd_empty,
	output wire        has_data
);

	localparam int AW = $clog2(DEPTH);

	// Prevent wholesale removal when scan path is simplified
	(* ramstyle = "M10K" *)
	(* noprune *)
	reg [7:0] mem [0:DEPTH-1];

	(* preserve *) reg [AW:0] wr_ptr;
	(* preserve *) reg [AW:0] rd_ptr;
	(* preserve *) reg        ever_wr;

	wire clr = reset | wr_flush;

	assign wr_full =
		(wr_ptr[AW-1:0] == rd_ptr[AW-1:0]) && (wr_ptr[AW] != rd_ptr[AW]);
	assign rd_empty = (wr_ptr == rd_ptr);
	assign has_data = ever_wr;

	// Pure write port (mem never under reset)
	always @(posedge clk) begin
		if (wr_en && !wr_full)
			mem[wr_ptr[AW-1:0]] <= wr_data;
	end

	always @(posedge clk) begin
		if (clr) begin
			wr_ptr  <= 0;
			ever_wr <= 0;
		end else if (wr_en && !wr_full) begin
			wr_ptr  <= wr_ptr + 1'd1;
			ever_wr <= 1'b1;
		end
	end

	// Pure registered read port
	always @(posedge clk) begin
		rd_data <= mem[rd_ptr[AW-1:0]];
	end

	always @(posedge clk) begin
		if (clr)
			rd_ptr <= 0;
		else if (rd_en && !rd_empty)
			rd_ptr <= rd_ptr + 1'd1;
	end

	always @(posedge clk) begin
		if (clr)
			wr_level <= 0;
		else
			wr_level <= wr_ptr[AW-1:0] - rd_ptr[AW-1:0];
	end

endmodule
