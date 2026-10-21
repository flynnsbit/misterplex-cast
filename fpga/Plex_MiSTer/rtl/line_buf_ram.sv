// True dual-clock parameterized WIDTH×16 line buffer RAM.
module line_buf_ram #(
	parameter int WIDTH = 320,
	parameter int AW = 9,
	parameter int DATA_W = 16
)(
	input  wire          wr_clk,
	input  wire          wr_en,
	input  wire [AW-1:0] wr_addr,
	input  wire [DATA_W-1:0] wr_data,

	input  wire          rd_clk,
	input  wire [AW-1:0] rd_addr,
	output reg  [DATA_W-1:0] rd_data
);
	(* ramstyle = "M10K" *) reg [DATA_W-1:0] mem [0:WIDTH-1];

	always @(posedge wr_clk) begin
		if (wr_en)
			mem[wr_addr] <= wr_data;
	end

	always @(posedge rd_clk) begin
		rd_data <= mem[rd_addr];
	end
endmodule
