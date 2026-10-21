// Small dual-clock FIFO for clock-domain crossings.
module async_fifo #(
	parameter int WIDTH = 8,
	parameter int AW    = 4,
	// Assert wr_almost_full when free slots <= AF_HEADROOM (sticky band, not a single count).
	parameter int AF_HEADROOM = 4
)(
	input  wire             wr_clk,
	input  wire             wr_reset,
	input  wire             wr_en,
	input  wire [WIDTH-1:0] wr_data,
	output wire             wr_full,
	output wire             wr_almost_full,

	input  wire             rd_clk,
	input  wire             rd_reset,
	input  wire             rd_en,
	output wire [WIDTH-1:0] rd_data,
	output wire             rd_empty
);
	localparam int DEPTH = 1 << AW;

	(* ramstyle = "MLAB" *) reg [WIDTH-1:0] mem [0:DEPTH-1];

	reg [AW:0] wr_bin, wr_gray;
	reg [AW:0] rd_bin, rd_gray;
	reg [AW:0] rd_gray_w1, rd_gray_w2;
	reg [AW:0] wr_gray_r1, wr_gray_r2;
	reg [WIDTH-1:0] rd_data_r;
	reg             rd_valid;
	localparam [AW:0] PTR_ONE = {{AW{1'b0}}, 1'b1};

	function automatic [AW:0] bin2gray(input [AW:0] b);
		bin2gray = (b >> 1) ^ b;
	endfunction

	function automatic [AW:0] gray2bin(input [AW:0] g);
		integer gi;
		begin
			gray2bin[AW] = g[AW];
			for (gi = AW - 1; gi >= 0; gi = gi - 1)
				gray2bin[gi] = gray2bin[gi + 1] ^ g[gi];
		end
	endfunction

	wire [AW:0] wr_gray_full = {~rd_gray_w2[AW:AW-1], rd_gray_w2[AW-2:0]};
	wire        wr_full_now  = (wr_gray == wr_gray_full);
	wire        wr_accept    = wr_en && !wr_full_now;
	wire [AW:0] wr_bin_next  = wr_bin + (wr_accept ? PTR_ONE : '0);
	wire [AW:0] wr_gray_next = bin2gray(wr_bin_next);
	wire        rd_has_entry = (rd_gray != wr_gray_r2);
	wire        rd_consume   = rd_en && rd_valid;
	wire        rd_prefetch  = rd_has_entry && (!rd_valid || rd_consume);
	wire [AW:0] rd_bin_next  = rd_bin + (rd_prefetch ? PTR_ONE : '0);
	wire [AW:0] rd_gray_next = bin2gray(rd_bin_next);

	// Occupancy from wr_bin vs CDC-synced rd gray→bin. Conservative under lag.
	// almost_full for free <= AF_HEADROOM (occ >= DEPTH - AF_HEADROOM), and when full.
	// Old bug: only (wr_bin+4) gray == full → true at exactly 4 free, false at 3/2/1.
	wire [AW:0] rd_bin_wsync = gray2bin(rd_gray_w2);
	// Occupancy from binary pointers (rd gray synced). FWFT may hold one extra
	// word already removed from rd_bin — bias AF by +1 so free<=AF still sticky
	// at 3/2/1 free (rd-duck: old wr_bin+4 gray only hit exactly 4 free).
	wire [AW:0] wr_occ = wr_bin - rd_bin_wsync;
	// almost_full sticky band. Disable level compare when DEPTH is too small for headroom.
	localparam bit AF_LEVEL_EN = (DEPTH > (AF_HEADROOM + 1));
	localparam int OCC_AF_INT = AF_LEVEL_EN ? (DEPTH - AF_HEADROOM - 1) : 0;
	wire [AW:0] occ_af_w = (AW+1)'(OCC_AF_INT);
	wire wr_af_level = AF_LEVEL_EN && (wr_occ >= occ_af_w);

	assign wr_full = wr_full_now;
	assign wr_almost_full = wr_full_now || wr_af_level;
	assign rd_empty = !rd_valid;
	assign rd_data = rd_data_r;

	always @(posedge wr_clk) begin
		if (wr_reset) begin
			wr_bin <= '0;
			wr_gray <= '0;
			rd_gray_w1 <= '0;
			rd_gray_w2 <= '0;
		end else begin
			rd_gray_w1 <= rd_gray;
			rd_gray_w2 <= rd_gray_w1;
			if (wr_accept) begin
				mem[wr_bin[AW-1:0]] <= wr_data;
				wr_bin <= wr_bin_next;
				wr_gray <= wr_gray_next;
			end
		end
	end

	always @(posedge rd_clk) begin
		if (rd_reset) begin
			rd_bin <= '0;
			rd_gray <= '0;
			wr_gray_r1 <= '0;
			wr_gray_r2 <= '0;
			rd_data_r <= '0;
			rd_valid <= 1'b0;
		end else begin
			wr_gray_r1 <= wr_gray;
			wr_gray_r2 <= wr_gray_r1;
			if (rd_prefetch) begin
				rd_data_r <= mem[rd_bin[AW-1:0]];
				rd_bin <= rd_bin_next;
				rd_gray <= rd_gray_next;
				rd_valid <= 1'b1;
			end else if (rd_consume)
				rd_valid <= 1'b0;
		end
	end
endmodule
