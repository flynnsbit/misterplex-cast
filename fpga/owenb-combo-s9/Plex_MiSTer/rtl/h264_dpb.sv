// Phase 3 motion-compensation decoded picture buffer helpers.
// One short-term reference picture, one current reconstruction picture.
`default_nettype none

module h264_dpb_i420_addr #(
	parameter int FRAME_W = 1280,
	parameter int FRAME_H = 720
)(
	input  wire [31:0] base,
	input  wire [1:0]  plane,
	input  wire [15:0] x,
	input  wire [15:0] y,
	output reg  [31:0] addr
);
	localparam int Y_BYTES = FRAME_W * FRAME_H;
	localparam int C_W     = FRAME_W / 2;
	localparam int C_H     = FRAME_H / 2;
	localparam int C_BYTES = C_W * C_H;

	always @* begin
		case (plane)
		2'd0: addr = base + (y * FRAME_W) + {16'd0, x};
		2'd1: addr = base + Y_BYTES + (y * C_W) + {16'd0, x};
		default: addr = base + Y_BYTES + C_BYTES + (y * C_W) + {16'd0, x};
		endcase
	end
endmodule

module h264_dpb_mb_write_addr #(
	parameter int FRAME_W = 1280,
	parameter int FRAME_H = 720
)(
	input  wire [31:0] bank_base,
	input  wire [7:0]  mb_x,
	input  wire [7:0]  mb_y,
	input  wire [1:0]  plane,
	input  wire [7:0]  sample_idx,
	output wire [31:0] addr
);
	wire [15:0] y_x = {4'd0, mb_x, 4'd0} + {12'd0, sample_idx[3:0]};
	wire [15:0] y_y = {4'd0, mb_y, 4'd0} + {12'd0, sample_idx[7:4]};
	wire [15:0] c_x = {5'd0, mb_x, 3'd0} + {13'd0, sample_idx[2:0]};
	wire [15:0] c_y = {5'd0, mb_y, 3'd0} + {13'd0, sample_idx[5:3]};
	wire [15:0] px = (plane == 2'd0) ? y_x : c_x;
	wire [15:0] py = (plane == 2'd0) ? y_y : c_y;

	h264_dpb_i420_addr #(.FRAME_W(FRAME_W), .FRAME_H(FRAME_H)) u_addr (
		.base(bank_base), .plane(plane), .x(px), .y(py), .addr(addr)
	);
endmodule

module h264_dpb_one_ref #(
	parameter int FRAME_W = 1280,
	parameter int FRAME_H = 720,
	parameter int BANK0_BASE = 0,
	// One I420 frame at default geometry (1280*720*3/2); instances may override.
	parameter int BANK1_BASE = 1280 * 720 * 3 / 2
)(
	input  wire               clk,
	input  wire               reset,

	input  wire               idr_start,
	input  wire               frame_done,
	output reg                ref_ready,
	output reg  [31:0]        current_base,
	output reg  [31:0]        reference_base,

	input  wire               filtered_sample_valid,
	input  wire [7:0]         filtered_mb_x,
	input  wire [7:0]         filtered_mb_y,
	input  wire [1:0]         filtered_plane,
	input  wire [7:0]         filtered_sample_idx,
	input  wire [7:0]         filtered_sample,
	output wire               mem_we,
	output wire [31:0]        mem_waddr,
	output wire [7:0]         mem_wdata,

	input  wire               fetch_start,
	input  wire [7:0]         fetch_mb_x,
	input  wire [7:0]         fetch_mb_y,
	input  wire [2:0]         fetch_part_mode,
	input  wire [1:0]         fetch_part_idx,
	input  wire [4:0]         fetch_part_w,
	input  wire [4:0]         fetch_part_h,
	input  wire signed [15:0] fetch_mv_x_qpel,
	input  wire signed [15:0] fetch_mv_y_qpel,
	output reg                fetch_busy,
	output reg                fetch_done,
	output reg                fetch_error_no_ref,
	output reg  [1:0]         luma_frac_x,
	output reg  [1:0]         luma_frac_y,
	output reg  [2:0]         chroma_frac_x,
	output reg  [2:0]         chroma_frac_y,
	output reg  signed [15:0] luma_origin_x,
	output reg  signed [15:0] luma_origin_y,
	output reg  signed [15:0] chroma_origin_x,
	output reg  signed [15:0] chroma_origin_y,

	output reg                mem_rd,
	output reg  [31:0]        mem_raddr,
	input  wire [7:0]         mem_rdata,
	input  wire               mem_rvalid,
	output reg                luma_window_valid,
	output reg  [8:0]         luma_window_idx,
	output reg  [7:0]         luma_window_sample,
	output reg                chroma_u_window_valid,
	output reg                chroma_v_window_valid,
	output reg  [6:0]         chroma_window_idx,
	output reg  [7:0]         chroma_window_sample
);
	localparam int C_W = FRAME_W / 2;
	localparam int C_H = FRAME_H / 2;
	localparam [2:0] PH_IDLE  = 3'd0;
	localparam [2:0] PH_LUMA  = 3'd1;
	localparam [2:0] PH_U     = 3'd2;
	localparam [2:0] PH_V     = 3'd3;
	localparam [2:0] PH_DRAIN = 3'd4;

	wire [31:0] write_addr;
	h264_dpb_mb_write_addr #(.FRAME_W(FRAME_W), .FRAME_H(FRAME_H)) u_write_addr (
		.bank_base(current_base),
		.mb_x(filtered_mb_x), .mb_y(filtered_mb_y),
		.plane(filtered_plane), .sample_idx(filtered_sample_idx),
		.addr(write_addr)
	);
	assign mem_we    = filtered_sample_valid;
	assign mem_waddr = write_addr;
	assign mem_wdata = filtered_sample;

	function automatic signed [15:0] part_x_off(input [2:0] mode, input [1:0] idx);
		begin
			case (mode)
			3'd2: part_x_off = (idx == 2'd1) ? 16'sd8 : 16'sd0; // P_8x16 right
			3'd3, 3'd4: part_x_off = idx[0] ? 16'sd8 : 16'sd0; // P_8x8/sub
			default: part_x_off = 16'sd0;
			endcase
		end
	endfunction

	function automatic signed [15:0] part_y_off(input [2:0] mode, input [1:0] idx);
		begin
			case (mode)
			3'd1: part_y_off = (idx == 2'd1) ? 16'sd8 : 16'sd0; // P_16x8 bottom
			3'd3, 3'd4: part_y_off = idx[1] ? 16'sd8 : 16'sd0; // P_8x8/sub
			default: part_y_off = 16'sd0;
			endcase
		end
	endfunction

	function automatic [15:0] clamp_coord(input signed [15:0] v, input [15:0] limit);
		begin
			if (v < 0) clamp_coord = 16'd0;
			else if ($signed({1'b0, v}) >= $signed({1'b0, limit})) clamp_coord = limit - 16'd1;
			else clamp_coord = v[15:0];
		end
	endfunction

	function automatic [31:0] i420_addr(
		input [31:0] base,
		input [1:0]  plane,
		input [15:0] x,
		input [15:0] y
	);
		begin
			case (plane)
			2'd0: i420_addr = base + (y * FRAME_W) + {16'd0, x};
			2'd1: i420_addr = base + (FRAME_W * FRAME_H) + (y * C_W) + {16'd0, x};
			default: i420_addr = base + (FRAME_W * FRAME_H) + (C_W * C_H) + (y * C_W) + {16'd0, x};
			endcase
		end
	endfunction

	reg [2:0] phase;
	reg [8:0] issue_idx;
	wire [8:0] issue_mod9 = issue_idx % 9'd9;
	wire [8:0] issue_div9 = issue_idx / 9'd9;
	wire signed [15:0] issue_mod9_s = $signed({7'd0, issue_mod9});
	wire signed [15:0] issue_div9_s = $signed({7'd0, issue_div9});
	reg [1:0] pending_plane;
	reg [8:0] pending_idx;
	reg       pending_valid;
	// External SRAM/BRAM path (decode_stub + TB) presents mem_rvalid/mem_rdata
	// one cycle after mem_rd/mem_raddr. pending_* is itself registered on the
	// issue edge, so on the response edge the pre-NBA value of pending_* is
	// exactly the metadata for the beat arriving on mem_rvalid.
	// Do NOT add a second pending_*_d1 stage: that double-delays metadata and
	// pairs window[n] with mem[addr_{n+1}] (measured: idx=4 got=20 want=17 at
	// origin=(-2,-2) edge clamp).
	reg signed [15:0] lx;
	reg signed [15:0] ly;
	reg signed [15:0] cx;
	reg signed [15:0] cy;
	reg [1:0] lat_part_w_lo;
	reg [1:0] lat_part_h_lo;
	reg signed [15:0] sx;
	reg signed [15:0] sy;
	reg [15:0] clamped_x;
	reg [15:0] clamped_y;

	always @* begin
		lx = luma_origin_x;
		ly = luma_origin_y;
		cx = chroma_origin_x;
		cy = chroma_origin_y;
		sx = 16'sd0;
		sy = 16'sd0;
		clamped_x = 16'd0;
		clamped_y = 16'd0;
		case (phase)
		PH_LUMA: begin
			sx = lx + $signed({7'd0, (issue_idx % 9'd21)}) - 16'sd2;
			sy = ly + $signed({7'd0, (issue_idx / 9'd21)}) - 16'sd2;
			clamped_x = clamp_coord(sx, FRAME_W[15:0]);
			clamped_y = clamp_coord(sy, FRAME_H[15:0]);
		end
		default: begin
			sx = cx + issue_mod9_s;
			sy = cy + issue_div9_s;
			clamped_x = clamp_coord(sx, C_W[15:0]);
			clamped_y = clamp_coord(sy, C_H[15:0]);
		end
		endcase
	end

	always @(posedge clk) begin
		mem_rd                <= 1'b0;
		luma_window_valid     <= 1'b0;
		chroma_u_window_valid <= 1'b0;
		chroma_v_window_valid <= 1'b0;

		if (reset) begin
			current_base        <= BANK0_BASE[31:0];
			reference_base      <= BANK1_BASE[31:0];
			ref_ready           <= 1'b0;
			fetch_busy          <= 1'b0;
			fetch_done          <= 1'b0;
			fetch_error_no_ref  <= 1'b0;
			phase               <= PH_IDLE;
			issue_idx           <= 9'd0;
			pending_valid       <= 1'b0;
			pending_plane       <= 2'd0;
			pending_idx         <= 9'd0;
			mem_raddr           <= 32'd0;
			luma_frac_x         <= 2'd0;
			luma_frac_y         <= 2'd0;
			chroma_frac_x       <= 3'd0;
			chroma_frac_y       <= 3'd0;
			luma_origin_x       <= 16'sd0;
			luma_origin_y       <= 16'sd0;
			chroma_origin_x     <= 16'sd0;
			chroma_origin_y     <= 16'sd0;
			lat_part_w_lo       <= 2'd0;
			lat_part_h_lo       <= 2'd0;
			luma_window_idx     <= 9'd0;
			luma_window_sample  <= 8'd0;
			chroma_window_idx   <= 7'd0;
			chroma_window_sample <= 8'd0;
		end else begin
			if (idr_start) begin
				ref_ready <= 1'b0;
			end
			if (frame_done) begin
				reference_base <= current_base;
				current_base   <= (current_base == BANK0_BASE[31:0]) ? BANK1_BASE[31:0] : BANK0_BASE[31:0];
				ref_ready      <= 1'b1;
			end

			if (mem_rvalid && pending_valid) begin
				case (pending_plane)
				2'd0: begin
					luma_window_valid  <= 1'b1;
					luma_window_idx    <= pending_idx;
					luma_window_sample <= mem_rdata;
				end
				2'd1: begin
					chroma_u_window_valid <= 1'b1;
					chroma_window_idx     <= pending_idx[6:0];
					chroma_window_sample  <= mem_rdata;
				end
				default: begin
					chroma_v_window_valid <= 1'b1;
					chroma_window_idx     <= pending_idx[6:0];
					chroma_window_sample  <= mem_rdata;
				end
				endcase
			end

			if (phase == PH_IDLE) begin
				pending_valid <= 1'b0;
				fetch_busy    <= 1'b0;
				if (fetch_start) begin
					fetch_done         <= 1'b0;
					fetch_error_no_ref <= !ref_ready;
					if (ref_ready) begin
						fetch_busy      <= 1'b1;
						phase           <= PH_LUMA;
						issue_idx       <= 9'd0;
						luma_frac_x     <= fetch_mv_x_qpel[1:0];
						luma_frac_y     <= fetch_mv_y_qpel[1:0];
						chroma_frac_x   <= fetch_mv_x_qpel[2:0];
						chroma_frac_y   <= fetch_mv_y_qpel[2:0];
						luma_origin_x   <= $signed({4'd0, fetch_mb_x, 4'd0}) +
						                   part_x_off(fetch_part_mode, fetch_part_idx) +
						                   (fetch_mv_x_qpel >>> 2);
						luma_origin_y   <= $signed({4'd0, fetch_mb_y, 4'd0}) +
						                   part_y_off(fetch_part_mode, fetch_part_idx) +
						                   (fetch_mv_y_qpel >>> 2);
						chroma_origin_x <= $signed({5'd0, fetch_mb_x, 3'd0}) +
						                   (part_x_off(fetch_part_mode, fetch_part_idx) >>> 1) +
						                   (fetch_mv_x_qpel >>> 3);
						chroma_origin_y <= $signed({5'd0, fetch_mb_y, 3'd0}) +
						                   (part_y_off(fetch_part_mode, fetch_part_idx) >>> 1) +
						                   (fetch_mv_y_qpel >>> 3);
						lat_part_w_lo   <= fetch_part_w[1:0];
						lat_part_h_lo   <= fetch_part_h[1:0];
					end else begin
						fetch_done <= 1'b1;
					end
				end
			end else if (phase == PH_DRAIN) begin
				pending_valid <= 1'b0;
				if (mem_rvalid && pending_valid) begin
					phase      <= PH_IDLE;
					fetch_busy <= 1'b0;
					fetch_done <= 1'b1;
				end
			end else begin
				fetch_error_no_ref <= 1'b0;
				mem_rd        <= 1'b1;
				pending_valid <= 1'b1;
				pending_plane <= (phase == PH_LUMA) ? 2'd0 : ((phase == PH_U) ? 2'd1 : 2'd2);
				pending_idx   <= issue_idx;
				mem_raddr     <= i420_addr(reference_base,
				                           (phase == PH_LUMA) ? 2'd0 : ((phase == PH_U) ? 2'd1 : 2'd2),
				                           clamped_x, clamped_y);
				if (phase == PH_LUMA) begin
					if (issue_idx == 9'd440) begin
						phase     <= PH_U;
						issue_idx <= 9'd0;
					end else begin
						issue_idx <= issue_idx + 9'd1;
					end
				end else if (phase == PH_U) begin
					if (issue_idx == 9'd80) begin
						phase     <= PH_V;
						issue_idx <= 9'd0;
					end else begin
						issue_idx <= issue_idx + 9'd1;
					end
				end else begin
					if (issue_idx == 9'd80) begin
						phase      <= PH_DRAIN;
					end else begin
						issue_idx <= issue_idx + 9'd1;
					end
				end
			end
			if (lat_part_w_lo != 2'd0 || lat_part_h_lo != 2'd0) begin
				// Keep part_w/part_h observed for lint and future narrower fetch windows.
			end
		end
	end
endmodule

module h264_luma_qpel_block_16x16 (
	input  wire [7:0] ref_win [0:440],
	input  wire [1:0] frac_x,
	input  wire [1:0] frac_y,
	output reg  [7:0] pred [0:255]
);
	function automatic integer clip1(input integer v);
		begin
			if (v < 0) clip1 = 0;
			else if (v > 255) clip1 = 255;
			else clip1 = v;
		end
	endfunction

	function automatic integer pix(input integer r, input integer c);
		reg [7:0] sample;
		begin
			sample = ref_win[r * 21 + c];
			pix = {24'd0, sample};
		end
	endfunction

	function automatic [7:0] low8(input integer v);
		low8 = v[7:0];
	endfunction

	function automatic integer avg2(input integer a, input integer b);
		avg2 = (a + b + 1) >>> 1;
	endfunction


	function automatic integer hraw_at(input integer row, input integer col);
		hraw_at = pix(row, col - 2) - 5 * pix(row, col - 1) +
		          20 * pix(row, col) + 20 * pix(row, col + 1) -
		          5 * pix(row, col + 2) + pix(row, col + 3);
	endfunction

	function automatic integer half_h_at(input integer row, input integer col);
		half_h_at = clip1((hraw_at(row, col) + 16) >>> 5);
	endfunction

	function automatic integer half_v_at(input integer row, input integer col);
		half_v_at = clip1((pix(row - 2, col) - 5 * pix(row - 1, col) +
		                    20 * pix(row, col) + 20 * pix(row + 1, col) -
		                    5 * pix(row + 2, col) + pix(row + 3, col) + 16) >>> 5);
	endfunction

	function automatic integer half_c_at(input integer row, input integer col);
		integer sum;
		begin
			sum = hraw_at(row - 2, col) - 5 * hraw_at(row - 1, col) +
			      20 * hraw_at(row, col) + 20 * hraw_at(row + 1, col) -
			      5 * hraw_at(row + 2, col) + hraw_at(row + 3, col);
			half_c_at = clip1((sum + 512) >>> 10);
		end
	endfunction

	function automatic [7:0] qpel_at(input integer x, input integer y);
		integer col;
		integer row;
		begin
			col = x + 2;
			row = y + 2;
			case ({frac_y, frac_x})

			4'b0000: qpel_at = low8(pix(row, col));
			4'b0001: qpel_at = low8(avg2(pix(row, col), half_h_at(row, col)));
			4'b0010: qpel_at = low8(half_h_at(row, col));
			4'b0011: qpel_at = low8(avg2(half_h_at(row, col), pix(row, col + 1)));
			4'b0100: qpel_at = low8(avg2(pix(row, col), half_v_at(row, col)));
			4'b0101: qpel_at = low8(avg2(half_h_at(row, col), half_v_at(row, col)));
			4'b0110: qpel_at = low8(avg2(half_h_at(row, col), half_c_at(row, col)));
			4'b0111: qpel_at = low8(avg2(half_h_at(row, col), half_v_at(row, col + 1)));
			4'b1000: qpel_at = low8(half_v_at(row, col));
			4'b1001: qpel_at = low8(avg2(half_v_at(row, col), half_c_at(row, col)));
			4'b1010: qpel_at = low8(half_c_at(row, col));
			4'b1011: qpel_at = low8(avg2(half_c_at(row, col), half_v_at(row, col + 1)));
			4'b1100: qpel_at = low8(avg2(half_v_at(row, col), pix(row + 1, col)));
			4'b1101: qpel_at = low8(avg2(half_h_at(row + 1, col), half_v_at(row, col)));
			4'b1110: qpel_at = low8(avg2(half_c_at(row, col), half_h_at(row + 1, col)));
			default: qpel_at = low8(avg2(half_h_at(row + 1, col), half_v_at(row, col + 1)));

			endcase
		end
	endfunction

	integer ox;
	integer oy;
	always @* begin
		for (oy = 0; oy < 16; oy = oy + 1)
			for (ox = 0; ox < 16; ox = ox + 1)
				pred[oy * 16 + ox] = qpel_at(ox, oy);
	end
endmodule

module h264_chroma_epel_block_8x8 (
	input  wire [7:0] ref_win [0:80],
	input  wire [2:0] frac_x,
	input  wire [2:0] frac_y,
	output reg  [7:0] pred [0:63]
);
	function automatic integer chroma_pix(input integer idx);
		reg [7:0] sample;
		begin
			sample = ref_win[idx];
			chroma_pix = {24'd0, sample};
		end
	endfunction

	function automatic [7:0] interp(input integer x, input integer y);
		integer p00;
		integer p10;
		integer p01;
		integer p11;
		integer fx;
		integer fy;
		integer sum;
		begin
			fx = {29'd0, frac_x};
			fy = {29'd0, frac_y};
			p00 = chroma_pix(y * 9 + x);
			p10 = chroma_pix(y * 9 + x + 1);
			p01 = chroma_pix((y + 1) * 9 + x);
			p11 = chroma_pix((y + 1) * 9 + x + 1);
			sum = (8 - fx) * (8 - fy) * p00 +
			      fx * (8 - fy) * p10 +
			      (8 - fx) * fy * p01 +
			      fx * fy * p11 + 32;
			interp = sum[13:6];
		end
	endfunction

	integer ox;
	integer oy;
	always @* begin
		for (oy = 0; oy < 8; oy = oy + 1)
			for (ox = 0; ox < 8; ox = ox + 1)
				pred[oy * 8 + ox] = interp(ox, oy);
	end
endmodule

module h264_inter_mc_16x16 (
	input  wire [7:0] luma_ref_win [0:440],
	input  wire [7:0] chroma_u_ref_win [0:80],
	input  wire [7:0] chroma_v_ref_win [0:80],
	input  wire [1:0] luma_frac_x,
	input  wire [1:0] luma_frac_y,
	input  wire [2:0] chroma_frac_x,
	input  wire [2:0] chroma_frac_y,
	output wire [7:0] pred_y [0:255],
	output wire [7:0] pred_u [0:63],
	output wire [7:0] pred_v [0:63]
);
	h264_luma_qpel_block_16x16 u_luma (
		.ref_win(luma_ref_win), .frac_x(luma_frac_x), .frac_y(luma_frac_y),
		.pred(pred_y)
	);

	h264_chroma_epel_block_8x8 u_chroma_u (
		.ref_win(chroma_u_ref_win), .frac_x(chroma_frac_x), .frac_y(chroma_frac_y),
		.pred(pred_u)
	);

	h264_chroma_epel_block_8x8 u_chroma_v (
		.ref_win(chroma_v_ref_win), .frac_x(chroma_frac_x), .frac_y(chroma_frac_y),
		.pred(pred_v)
	);
endmodule

module h264_inter_mc_part (
	input  wire [7:0] luma_ref_win [0:440],
	input  wire [7:0] chroma_u_ref_win [0:80],
	input  wire [7:0] chroma_v_ref_win [0:80],
	input  wire [1:0] luma_frac_x,
	input  wire [1:0] luma_frac_y,
	input  wire [2:0] chroma_frac_x,
	input  wire [2:0] chroma_frac_y,
	input  wire [4:0] part_w,
	input  wire [4:0] part_h,
	output wire [7:0] pred_y [0:255],
	output wire       pred_y_valid [0:255],
	output wire [7:0] pred_u [0:63],
	output wire       pred_u_valid [0:63],
	output wire [7:0] pred_v [0:63],
	output wire       pred_v_valid [0:63]
);
	wire [7:0] full_y [0:255];
	wire [7:0] full_u [0:63];
	wire [7:0] full_v [0:63];
	wire [4:0] chroma_w = {1'b0, part_w[4:1]};
	wire [4:0] chroma_h = {1'b0, part_h[4:1]};

	h264_inter_mc_16x16 u_full (
		.luma_ref_win(luma_ref_win),
		.chroma_u_ref_win(chroma_u_ref_win),
		.chroma_v_ref_win(chroma_v_ref_win),
		.luma_frac_x(luma_frac_x), .luma_frac_y(luma_frac_y),
		.chroma_frac_x(chroma_frac_x), .chroma_frac_y(chroma_frac_y),
		.pred_y(full_y), .pred_u(full_u), .pred_v(full_v)
	);

	genvar py_i;
	generate
		for (py_i = 0; py_i < 256; py_i = py_i + 1) begin : gen_part_y
			localparam int LX = py_i % 16;
			localparam int LY = py_i / 16;
			wire in_part = (LX[4:0] < part_w) && (LY[4:0] < part_h);
			assign pred_y_valid[py_i] = in_part;
			assign pred_y[py_i] = in_part ? full_y[py_i] : 8'd0;
		end
		for (py_i = 0; py_i < 64; py_i = py_i + 1) begin : gen_part_c
			localparam int CX = py_i % 8;
			localparam int CY = py_i / 8;
			wire in_part = (CX[4:0] < chroma_w) && (CY[4:0] < chroma_h);
			assign pred_u_valid[py_i] = in_part;
			assign pred_v_valid[py_i] = in_part;
			assign pred_u[py_i] = in_part ? full_u[py_i] : 8'd0;
			assign pred_v[py_i] = in_part ? full_v[py_i] : 8'd0;
		end
	endgenerate
endmodule

`default_nettype wire
