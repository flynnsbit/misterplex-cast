// Continuous square/sine-ish tone for A/V path validation.
// Sampled in CLK_AUDIO domain (24.576 MHz on MiSTer) with simple divider → ~48 kHz rate.

module audio_tone (
	input  wire        clk_audio,  // 24.576 MHz
	input  wire        reset,
	input  wire        enable,
	input  wire [15:0] freq_div,   // sample ticks per half-period (e.g. 48000/440/2 ≈ 54 at 48k)
	output reg  [15:0] sample_l,
	output reg  [15:0] sample_r,
	output reg         underrun    // sticky if disabled while expected; cleared on enable
);

	// 24.576e6 / 512 = 48000
	reg [8:0] sdiv;
	reg [15:0] half;
	reg        phase;

	always @(posedge clk_audio) begin
		if (reset) begin
			sdiv     <= 0;
			half     <= 0;
			phase    <= 0;
			sample_l <= 0;
			sample_r <= 0;
			underrun <= 0;
		end else begin
			if (!enable) begin
				sample_l <= 0;
				sample_r <= 0;
				// not an underrun when intentionally muted
			end else begin
				sdiv <= sdiv + 1'd1;
				if (sdiv == 0) begin
					// one 48 kHz sample
					if (half == 0) begin
						phase <= ~phase;
						half  <= (freq_div == 0) ? 16'd54 : freq_div;
					end else begin
						half <= half - 1'd1;
					end
					sample_l <= phase ? 16'h3000 : 16'hD000; // signed-ish magnitude
					sample_r <= phase ? 16'h3000 : 16'hD000;
				end
			end
		end
	end

endmodule
