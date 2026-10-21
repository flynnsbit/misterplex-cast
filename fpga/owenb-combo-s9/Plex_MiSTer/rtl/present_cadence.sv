// Present cadence: decide whether this display tick should advance a unique content frame.
// Must match host/libmisterplex/cadence.hpp (unit-tested).
//
// content_index = floor(display_index * content_fps / display_hz)
// Advance when content_index increases (tick 0 always advances).

module present_cadence (
	input  wire        clk,
	input  wire        reset,
	input  wire        display_tick,   // 1-cycle pulse at start of active frame
	input  wire [7:0]  content_fps,    // e.g. 24, 30, 60
	input  wire [7:0]  display_hz,     // e.g. 60, 50
	output reg         advance_unique,
	output reg [31:0]  display_index,
	output reg [31:0]  content_index
);

	function automatic [31:0] floor_mul;
		input [31:0] n;
		input [7:0]  cf;
		input [7:0]  dh;
		begin
			if (dh == 0)
				floor_mul = n;
			else
				floor_mul = (n * cf) / dh;
		end
	endfunction

	reg [31:0] next_content;

	always @(posedge clk) begin
		if (reset) begin
			display_index  <= 0;
			content_index  <= 0;
			advance_unique <= 0;
		end else begin
			advance_unique <= 0;
			if (display_tick) begin
				if (content_fps == 0 || display_hz == 0 || content_fps >= display_hz) begin
					advance_unique <= 1'b1;
					content_index  <= content_index + 1'd1;
				end else begin
					next_content = floor_mul(display_index, content_fps, display_hz);
					if (display_index == 0 || next_content != content_index) begin
						advance_unique <= 1'b1;
						content_index  <= next_content;
					end
					// else hold: content_index unchanged
				end
				display_index <= display_index + 1'd1;
			end
		end
	end

endmodule
