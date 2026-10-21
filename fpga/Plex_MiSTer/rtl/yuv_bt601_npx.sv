// N-lane BT.601 YUV420 → RGB extract from packed 64-bit line-buffer qwords.
//
// Matches ddr_frame_store single-pixel math (integer form):
//   R = sat(Y + (359*V')>>8)
//   G = sat(Y - (88*U' + 183*V')>>8)
//   B = sat(Y + (454*U')>>8)
// with U'=U-128, V'=V-128.
//
// Default product path does NOT instantiate this module unless
// PRESENT_MULTI_PIXEL is defined (see MULTI_PIXEL_PRESENT_DESIGN.md).
//
// PX_PER_CLK in {1,2,4}. Luma qword holds 8 samples; chroma qword holds 8
// samples covering 16 luma X (4:2:0 horizontal). Lane i uses src_x0+i.

module yuv_bt601_npx #(
	parameter int PX_PER_CLK = 1,
	parameter int X_W = 11
)(
	input  wire                 clk,
	input  wire                 reset,
	input  wire                 in_valid,
	input  wire [X_W-1:0]       src_x0,
	input  wire [63:0]          y_qword,
	input  wire [63:0]          u_qword,
	input  wire [63:0]          v_qword,
	// Optional second Y qword when a lane group straddles an 8-byte boundary.
	input  wire [63:0]          y_qword_hi,
	input  wire                 y_hi_valid,

	output reg                  out_valid,
	output reg  [PX_PER_CLK*8-1:0] out_r,
	output reg  [PX_PER_CLK*8-1:0] out_g,
	output reg  [PX_PER_CLK*8-1:0] out_b,
	output reg  [PX_PER_CLK-1:0]   out_lane_valid
);
	function automatic [7:0] pick8(input [63:0] q, input [2:0] idx);
		case (idx)
			3'd0: pick8 = q[7:0];
			3'd1: pick8 = q[15:8];
			3'd2: pick8 = q[23:16];
			3'd3: pick8 = q[31:24];
			3'd4: pick8 = q[39:32];
			3'd5: pick8 = q[47:40];
			3'd6: pick8 = q[55:48];
			default: pick8 = q[63:56];
		endcase
	endfunction

	function automatic [7:0] sat8(input signed [11:0] v);
		if (v < 0)
			sat8 = 8'd0;
		else if (v > 12'sd255)
			sat8 = 8'd255;
		else
			sat8 = v[7:0];
	endfunction

	function automatic [7:0] y_at(input [X_W-1:0] x);
		reg [2:0] idx;
		reg        use_hi;
		begin
			idx = x[2:0];
			use_hi = (x[X_W-1:3] != src_x0[X_W-1:3]) && y_hi_valid;
			y_at = pick8(use_hi ? y_qword_hi : y_qword, idx);
		end
	endfunction

	// Chroma 4:2:0: one U/V sample per 2 luma X; qword index is src_x>>4,
	// byte index within qword is (src_x>>1)[2:0].
	function automatic [7:0] c_at(input [63:0] q, input [X_W-1:0] x);
		reg [2:0] cidx;
		begin
			cidx = x[3:1];
			c_at = pick8(q, cidx);
		end
	endfunction

	integer li;
	reg [X_W-1:0] x_i;
	reg [7:0] y_i, u_i, v_i;
	reg signed [11:0] y_s, u_s, v_s;
	reg signed [20:0] r_w, g_w, b_w;
	reg signed [11:0] r_c, g_c, b_c;
	reg [7:0] r_i, g_i, b_i;

	always @(posedge clk) begin
		if (reset) begin
			out_valid <= 1'b0;
			out_r <= '0;
			out_g <= '0;
			out_b <= '0;
			out_lane_valid <= '0;
		end else begin
			out_valid <= in_valid;
			out_lane_valid <= in_valid ? {PX_PER_CLK{1'b1}} : '0;
			for (li = 0; li < PX_PER_CLK; li = li + 1) begin
				x_i = src_x0 + X_W'(li);
				y_i = y_at(x_i);
				u_i = c_at(u_qword, x_i);
				v_i = c_at(v_qword, x_i);
				y_s = {4'd0, y_i};
				u_s = {4'd0, u_i} - 12'sd128;
				v_s = {4'd0, v_i} - 12'sd128;
				r_w = ({{9{y_s[11]}}, y_s} <<< 8) + (21'sd359 * v_s);
				g_w = ({{9{y_s[11]}}, y_s} <<< 8) - (21'sd88 * u_s) - (21'sd183 * v_s);
				b_w = ({{9{y_s[11]}}, y_s} <<< 8) + (21'sd454 * u_s);
				r_c = r_w[19:8];
				g_c = g_w[19:8];
				b_c = b_w[19:8];
				r_i = sat8(r_c);
				g_i = sat8(g_c);
				b_i = sat8(b_c);
				out_r[li*8 +: 8] <= r_i;
				out_g[li*8 +: 8] <= g_i;
				out_b[li*8 +: 8] <= b_i;
			end
		end
	end
endmodule
