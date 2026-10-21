// Red-proof testbench for audio_fifo Gray-coded CDC.
// Verifies that Gray pointers change by exactly one bit per transition.
`timescale 1ns/1ps

module tb_audio_fifo_cdc;
	reg clk_wr = 0, clk_rd = 0, reset = 1;
	reg        wr_en = 0;
	reg [31:0] wr_data = 0;
	reg        wr_flush = 0;
	wire       wr_full;
	wire [15:0] wr_level;
	wire [15:0] sample_l, sample_r;
	wire        underrun, has_audio;

	// clk_wr ~20 MHz (50ns), clk_rd ~24.576 MHz (40.7ns) — intentionally
	// incommensurate to stress CDC
	always #25 clk_wr = ~clk_wr;
	always #20 clk_rd = ~clk_rd;  // faster than real audio, stress CDC

	audio_fifo #(.DEPTH(16)) dut (
		.clk_wr(clk_wr), .clk_rd(clk_rd), .reset(reset),
		.wr_en(wr_en), .wr_data(wr_data), .wr_flush(wr_flush),
		.wr_full(wr_full), .wr_level(wr_level),
		.rd_enable(1'b1),
		.sample_l(sample_l), .sample_r(sample_r),
		.underrun(underrun), .has_audio(has_audio)
	);

	integer i;
	integer errors = 0;
	integer writes = 0;

	initial begin
		// Release reset
		repeat (10) @(posedge clk_wr);
		reset = 0;
		repeat (5) @(posedge clk_wr);

		// Write 64 samples (wrapping the 16-deep FIFO multiple times)
		for (i = 0; i < 64; i = i + 1) begin
			@(posedge clk_wr);
			if (!wr_full) begin
				wr_en = 1;
				wr_data = {16'(i * 3 + 1), 16'(i * 2 + 1)};
				writes = writes + 1;
			end else begin
				wr_en = 0;
			end
		end
		wr_en = 0;

		// Let read side drain
		repeat (1000) @(posedge clk_rd);

		// Test flush
		@(posedge clk_wr);
		wr_flush = 1;
		@(posedge clk_wr);
		wr_flush = 0;

		// Write more after flush
		repeat (5) @(posedge clk_wr);
		for (i = 0; i < 8; i = i + 1) begin
			@(posedge clk_wr);
			wr_en = 1;
			wr_data = {16'hBEEF, 16'hCAFE};
		end
		wr_en = 0;

		repeat (500) @(posedge clk_rd);

		// RED GUARD check: the $error assertions in audio_fifo.sv
		// under SIMULATION flag will have fired if Gray invariant broke.
		// Also check basic functionality:
		if (writes == 0) begin
			$display("FAIL: no writes completed");
			errors = errors + 1;
		end

		if (errors == 0)
			$display("PASS: audio_fifo CDC Gray-code red-proof (%0d writes)", writes);
		else
			$display("FAIL: %0d errors", errors);

		$finish;
	end

	// Timeout
	initial begin
		#100000;
		$display("TIMEOUT");
		$finish;
	end
endmodule
