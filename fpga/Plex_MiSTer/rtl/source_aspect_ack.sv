// Collect ioctl index 4 PLXA (9 bytes) and pulse a clk-domain ACK snapshot.
// ddr_frame_store writes PLXJ at doorbell+0x130 (L4: 0x3047F130).
module source_aspect_ack (
	input  wire        clk,
	input  wire        reset,
	input  wire        ioctl_download,
	input  wire        ioctl_wr,
	input  wire [7:0]  ioctl_dout,
	input  wire [15:0] ioctl_index,
	output reg         ack_toggle,
	output reg  [11:0] ack_x,
	output reg  [11:0] ack_y,
	output reg  [7:0]  ack_token
);
	localparam [31:0] MAGIC_A = 32'h4158_4C50; // "PLXA" LE
	reg        was_dl;
	reg [3:0]  idx;
	reg [7:0]  b0, b1, b2, b3, b4, b5, b6, b7, b8;

	wire [31:0] magic = {b3, b2, b1, b0};
	wire        is_plxa = (ioctl_index[5:0] == 6'd4);

	always @(posedge clk) begin
		if (reset) begin
			was_dl <= 1'b0;
			idx <= 4'd0;
			ack_toggle <= 1'b0;
			ack_x <= 12'd0;
			ack_y <= 12'd0;
			ack_token <= 8'd0;
			b0 <= 8'd0; b1 <= 8'd0; b2 <= 8'd0; b3 <= 8'd0;
			b4 <= 8'd0; b5 <= 8'd0; b6 <= 8'd0; b7 <= 8'd0; b8 <= 8'd0;
		end else begin
			if (ioctl_download && !was_dl)
				idx <= 4'd0;
			if (is_plxa && ioctl_download && ioctl_wr && idx < 4'd9) begin
				case (idx)
					4'd0: b0 <= ioctl_dout;
					4'd1: b1 <= ioctl_dout;
					4'd2: b2 <= ioctl_dout;
					4'd3: b3 <= ioctl_dout;
					4'd4: b4 <= ioctl_dout;
					4'd5: b5 <= ioctl_dout;
					4'd6: b6 <= ioctl_dout;
					4'd7: b7 <= ioctl_dout;
					default: b8 <= ioctl_dout;
				endcase
				idx <= idx + 4'd1;
			end
			if (!ioctl_download && was_dl && is_plxa && idx >= 4'd9 && magic == MAGIC_A) begin
				ack_x <= {b5[3:0], b4};
				ack_y <= {b7[3:0], b6};
				ack_token <= b8;
				ack_toggle <= ~ack_toggle;
			end
			was_dl <= ioctl_download;
		end
	end
endmodule
