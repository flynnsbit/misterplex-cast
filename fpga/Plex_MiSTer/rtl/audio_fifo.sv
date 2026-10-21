// Present-domain stereo PCM FIFO (Phase 3.2) — sized for M10K inference.
// Write: clk_wr (sys). Read: clk_rd (CLK_AUDIO) @ 48 kHz (div 512).
// Keep a single registered read port so Quartus maps to altsyncram.
//
// CDC uses Gray-coded pointers (not binary) to avoid multi-bit glitch
// when sampled across clock domains.  Binary pointers stayed in the
// original design by mistake; fixed 2026-07-27 per async_fifo pattern.

module audio_fifo #(
	parameter int DEPTH = 2048  // ~42 ms @ 48 kHz stereo
)(
	input  wire        clk_wr,
	input  wire        clk_rd,
	input  wire        reset,

	input  wire        wr_en,
	input  wire [31:0] wr_data,  // {R[15:0], L[15:0]}
	input  wire        wr_flush,
	output wire        wr_full,
	output reg  [15:0] wr_level,

	input  wire        rd_enable,
	output reg  [15:0] sample_l,
	output reg  [15:0] sample_r,
	output reg         underrun,
	output wire        has_audio
);

	localparam int AW = $clog2(DEPTH);

	function automatic [AW:0] bin2gray(input [AW:0] b);
		bin2gray = (b >> 1) ^ b;
	endfunction

	function automatic [AW:0] gray2bin(input [AW:0] g);
		integer k;
		begin
			gray2bin = g;
			for (k = AW - 1; k >= 0; k = k - 1)
				gray2bin[k] = gray2bin[k+1] ^ g[k];
		end
	endfunction

	// Force block RAM (avoid register explosion)
	(* ramstyle = "M10K" *)
	reg [31:0] mem [0:DEPTH-1];

	reg [AW:0] wr_ptr;      // binary, clk_wr domain
	reg [AW:0] wr_gray;     // Gray,   clk_wr domain
	reg [AW:0] rd_ptr;      // binary, clk_rd domain
	reg [AW:0] rd_gray;     // Gray,   clk_rd domain
	reg        has_wr;

	// CDC: Gray-coded pointers through 2-FF synchronizers
	reg [AW:0] rd_gray_w1, rd_gray_w2;   // rd Gray → clk_wr
	reg [AW:0] wr_gray_r1, wr_gray_r2;   // wr Gray → clk_rd
	reg        has_wr_s1, has_wr_s2;

	// Convert synchronized Gray rd pointer back to binary for wr_level arithmetic.
	// Only the address bits [AW-1:0] are used; MSB serves full/wrap detection
	// which is handled by the Gray-coded comparison below.
	/* verilator lint_off UNUSEDSIGNAL */
	wire [AW:0] rd_bin_in_wr = gray2bin(rd_gray_w2);
	/* verilator lint_on UNUSEDSIGNAL */

	always @(posedge clk_wr) begin
		if (reset) begin
			rd_gray_w1 <= '0;
			rd_gray_w2 <= '0;
		end else begin
			rd_gray_w1 <= rd_gray;
			rd_gray_w2 <= rd_gray_w1;
		end
	end

	always @(posedge clk_rd) begin
		if (reset) begin
			wr_gray_r1 <= '0;
			wr_gray_r2 <= '0;
			has_wr_s1  <= 1'b0;
			has_wr_s2  <= 1'b0;
		end else begin
			wr_gray_r1 <= wr_gray;
			wr_gray_r2 <= wr_gray_r1;
			has_wr_s1  <= has_wr;
			has_wr_s2  <= has_wr_s1;
		end
	end

	// Full/empty use Gray-coded comparisons (safe across CDC)
	wire [AW:0] wr_gray_full = {~rd_gray_w2[AW:AW-1], rd_gray_w2[AW-2:0]};
	assign wr_full = (wr_gray == wr_gray_full);
	wire rd_empty = (rd_gray == wr_gray_r2);

	// Write port
	always @(posedge clk_wr) begin
		if (reset) begin
			wr_ptr   <= '0;
			wr_gray  <= '0;
			wr_level <= 16'd0;
			has_wr   <= 1'b0;
		end else if (wr_flush) begin
			wr_ptr   <= '0;
			wr_gray  <= '0;
			wr_level <= 16'd0;
		end else begin
			if (wr_en && !wr_full) begin
				mem[wr_ptr[AW-1:0]] <= wr_data;
				wr_ptr  <= wr_ptr + 1'd1;
				wr_gray <= bin2gray(wr_ptr + 1'd1);
				has_wr  <= 1'b1;
			end
			// level ≈ wr - rd (binary, address bits only)
			wr_level <= 16'(wr_ptr[AW-1:0] - rd_bin_in_wr[AW-1:0]);
		end
	end

	assign has_audio = has_wr_s2;

	// Read @ 48 kHz: one registered memory read
	reg [8:0]  sdiv;
	reg [31:0] rd_data;
	reg [AW-1:0] rd_addr_r;

	always @(posedge clk_rd) begin
		if (reset) begin
			rd_ptr    <= '0;
			rd_gray   <= '0;
			sdiv      <= 9'd0;
			sample_l  <= 16'd0;
			sample_r  <= 16'd0;
			underrun  <= 1'b0;
			rd_data   <= 32'd0;
			rd_addr_r <= '0;
		end else if (!rd_enable) begin
			sample_l <= 16'd0;
			sample_r <= 16'd0;
		end else begin
			sdiv <= sdiv + 1'd1;
			// continuous registered read of current address
			rd_addr_r <= rd_ptr[AW-1:0];
			rd_data   <= mem[rd_addr_r];

			if (sdiv == 0) begin
				if (!rd_empty) begin
					sample_l <= rd_data[15:0];
					sample_r <= rd_data[31:16];
					rd_ptr   <= rd_ptr + 1'd1;
					rd_gray  <= bin2gray(rd_ptr + 1'd1);
				end else begin
					sample_l <= 16'd0;
					sample_r <= 16'd0;
					if (has_wr_s2)
						underrun <= 1'b1;
				end
			end
		end
	end

	// ── RED GUARD: binary pointer CDC must never return ──────────
	// If anyone removes Gray coding and reverts to raw binary pointer
	// synchronization, this block will fire during simulation.
	// Flush/reset are exempt — they are domain-local reset events, not
	// incremental pointer advances.
`ifdef SIMULATION
	reg [AW:0] _red_prev_wr_gray = '0;
	reg [AW:0] _red_prev_rd_gray = '0;
	reg        _red_wr_flushed = 1'b0;
	always @(posedge clk_wr) begin
		_red_wr_flushed <= wr_flush;
		_red_prev_wr_gray <= wr_gray;
		if (!reset && !_red_wr_flushed && (_red_prev_wr_gray !== wr_gray)) begin
			// Gray codes must differ in exactly one bit per transition
			if ($countones(_red_prev_wr_gray ^ wr_gray) != 1)
				$error("RED GUARD: wr_gray changed >1 bit — binary CDC regression");
		end
	end
	always @(posedge clk_rd) begin
		_red_prev_rd_gray <= rd_gray;
		if (!reset && (_red_prev_rd_gray !== rd_gray)) begin
			if ($countones(_red_prev_rd_gray ^ rd_gray) != 1)
				$error("RED GUARD: rd_gray changed >1 bit — binary CDC regression");
		end
	end
`endif

endmodule
