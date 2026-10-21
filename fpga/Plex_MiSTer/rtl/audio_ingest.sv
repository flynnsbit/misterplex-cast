// Assemble ioctl byte stream into stereo s16le samples for audio_fifo.
// Stream: L_lo, L_hi, R_lo, R_hi (little-endian PCM).
// index select: only when enabled (F2 download).

module audio_ingest (
	input  wire        clk,
	input  wire        reset,

	input  wire        ioctl_download,
	input  wire        ioctl_wr,
	input  wire [7:0]  ioctl_dout,
	input  wire [15:0] ioctl_index,
	input  wire        enable,       // true when this download is audio (F2)

	output reg         wr_en,
	output reg  [31:0] wr_data,
	output reg         wr_flush,
	output reg         active
);

	reg [1:0] phase;
	reg [7:0] b0, b1, b2;
	reg       was_dl;

	always @(posedge clk) begin
		wr_en    <= 1'b0;
		wr_flush <= 1'b0;

		if (reset) begin
			phase   <= 0;
			active  <= 0;
			was_dl  <= 0;
		end else if (!enable) begin
			phase  <= 0;
			active <= 0;
			was_dl <= ioctl_download;
		end else begin
			// Continuous streaming: do NOT flush on every download start (appends).
			// Flush only via core reset or external af_wr_flush from status.
			if (ioctl_download && !was_dl) begin
				phase  <= 0;
				active <= 1'b1;
			end
			if (!ioctl_download && was_dl)
				active <= 1'b0;
			was_dl <= ioctl_download;

			if (ioctl_download && ioctl_wr) begin
				case (phase)
					2'd0: begin b0 <= ioctl_dout; phase <= 2'd1; end
					2'd1: begin b1 <= ioctl_dout; phase <= 2'd2; end
					2'd2: begin b2 <= ioctl_dout; phase <= 2'd3; end
					default: begin
						// {R, L} as little-endian pairs
						wr_data <= {ioctl_dout, b2, b1, b0};
						wr_en   <= 1'b1;
						phase   <= 2'd0;
					end
				endcase
			end
		end
	end

endmodule
