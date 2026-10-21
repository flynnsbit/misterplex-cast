// Phase 3.3: F3 ioctl byte stream → bitstream_fifo (append mode).
// Flush only on core reset or status pulse (not every download start).

module stream_ingest (
	input  wire        clk,
	input  wire        reset,

	input  wire        ioctl_download,
	input  wire        ioctl_wr,
	input  wire [7:0]  ioctl_dout,
	input  wire        enable,   // true for F3 / elementary bitstream

	output reg         wr_en,
	output reg  [7:0]  wr_data,
	output reg         wr_flush,
	output reg         active,
	output reg  [31:0] bytes_in
);

	reg was_dl;

	always @(posedge clk) begin
		wr_en    <= 1'b0;
		wr_flush <= 1'b0;

		if (reset) begin
			active   <= 0;
			was_dl   <= 0;
			bytes_in <= 0;
		end else if (!enable) begin
			active <= 0;
			was_dl <= ioctl_download;
		end else begin
			if (ioctl_download && !was_dl) begin
				active <= 1'b1;
				// append — do not flush
			end
			if (!ioctl_download && was_dl)
				active <= 1'b0;
			was_dl <= ioctl_download;

			if (ioctl_download && ioctl_wr) begin
				wr_data  <= ioctl_dout;
				wr_en    <= 1'b1;
				bytes_in <= bytes_in + 1'd1;
			end
		end
	end

endmodule
