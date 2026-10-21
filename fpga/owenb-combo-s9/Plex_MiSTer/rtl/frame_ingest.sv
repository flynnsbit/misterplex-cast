// Assemble ioctl byte stream into RGB565 pixels and drive frame_store writes.
// ioctl_dout is 8-bit (hps_io default). Little-endian: lo then hi → {hi,lo}.

module frame_ingest (
	input  wire        clk,
	input  wire        reset,

	input  wire        ioctl_download,
	input  wire        ioctl_wr,
	input  wire [7:0]  ioctl_dout,
	input  wire [26:0] ioctl_addr,
	input  wire [15:0] ioctl_index, // menu file type

	// accept any download index for now (F1 raw); tighten later
	input  wire        enable,

	output reg         wr_en,
	output reg  [15:0] wr_pixel,
	output reg         wr_reset_ptr,
	output reg         swap_req,
	output reg  [31:0] pixels_written,
	output reg         downloading
);

	reg        have_lo;
	reg [7:0]  lo_byte;
	reg        was_dl;

	always @(posedge clk) begin
		wr_en        <= 1'b0;
		wr_reset_ptr <= 1'b0;
		swap_req     <= 1'b0;

		if (reset) begin
			have_lo         <= 1'b0;
			pixels_written  <= 0;
			downloading     <= 1'b0;
			was_dl          <= 1'b0;
		end else if (!enable) begin
			have_lo     <= 1'b0;
			downloading <= 1'b0;
			was_dl      <= ioctl_download;
		end else begin
			// Rising edge of download → new frame
			if (ioctl_download && !was_dl) begin
				wr_reset_ptr   <= 1'b1;
				have_lo        <= 1'b0;
				pixels_written <= 0;
				downloading    <= 1'b1;
			end
			// Falling edge → complete frame, swap banks
			if (!ioctl_download && was_dl && downloading) begin
				swap_req    <= 1'b1;
				downloading <= 1'b0;
				have_lo     <= 1'b0;
			end
			was_dl <= ioctl_download;

			if (ioctl_download && ioctl_wr) begin
				if (!have_lo) begin
					lo_byte <= ioctl_dout;
					have_lo <= 1'b1;
				end else begin
					wr_pixel <= {ioctl_dout, lo_byte};
					wr_en    <= 1'b1;
					have_lo  <= 1'b0;
					pixels_written <= pixels_written + 1'd1;
				end
			end
		end
	end

endmodule
