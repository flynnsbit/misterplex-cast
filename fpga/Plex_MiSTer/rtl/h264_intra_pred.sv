// Phase 3.3l-3: H.264 intra prediction helpers.
// Behaviour matches host/libmisterplex/h264_recon.hpp for the measured all-intra
// Plex vector. All intra modes supported: I4x4 (9 modes), I16x16 (4 modes incl.
// Plane per clause 8.3.3.4), Chroma 8x8 (4 modes incl. Plane per clause 8.3.4.4).
// I_PCM (mb_type 25) remains UNSUPPORTED — mode guard fires unsupported_code for it.

module h264_intra4x4_pred (
	input  wire [3:0] mode,
	input  wire [7:0] above [0:7],
	input  wire [7:0] left [0:3],
	input  wire [7:0] top_left,
	input  wire       has_above,
	input  wire       has_left,
	output reg  [3:0] used_mode,
	output reg  [7:0] pred [0:15]
);
	function automatic [7:0] clip8;
		input integer v;
		begin
			if (v < 0) clip8 = 8'd0;
			else if (v > 255) clip8 = 8'd255;
			else clip8 = v[7:0];
		end
	endfunction

	task automatic put;
		input int x;
		input int y;
		input int v;
		begin
			pred[y * 4 + x] = clip8(v);
		end
	endtask

	integer x, y, i;
	reg [3:0] m;
	integer t0, t1, t2, t3, t4, t5, t6, t7;
	integer l0, l1, l2, l3;
	integer dc_sum;
	reg [7:0] dc_v;
	always @* begin
		dc_sum = 0;
		dc_v = 8'd128;
		for (i = 0; i < 16; i = i + 1) pred[i] = 8'd128;
		m = mode;
		if (!has_above && (mode == 4'd0 || mode == 4'd3 || mode == 4'd7)) m = 4'd2;
		if (!has_left  && (mode == 4'd1 || mode == 4'd8)) m = 4'd2;
		if ((!has_above || !has_left) && (mode == 4'd4 || mode == 4'd5 || mode == 4'd6)) m = 4'd2;
		used_mode = m;

		t0 = above[0]; t1 = above[1]; t2 = above[2]; t3 = above[3];
		t4 = above[4]; t5 = above[5]; t6 = above[6]; t7 = above[7];
		l0 = left[0];  l1 = left[1];  l2 = left[2];  l3 = left[3];

		case (m)
		4'd0: begin // Vertical
			for (y = 0; y < 4; y = y + 1)
				for (x = 0; x < 4; x = x + 1) pred[y * 4 + x] = above[x];
		end
		4'd1: begin // Horizontal
			for (y = 0; y < 4; y = y + 1)
				for (x = 0; x < 4; x = x + 1) pred[y * 4 + x] = left[y];
		end
		4'd2: begin // DC
			if (has_above && has_left) begin
				dc_sum = t0 + t1 + t2 + t3 + l0 + l1 + l2 + l3 + 10'd4;
				dc_v = dc_sum >>> 3;
			end else if (has_above) begin
				dc_sum = t0 + t1 + t2 + t3 + 10'd2;
				dc_v = dc_sum >>> 2;
			end else if (has_left) begin
				dc_sum = l0 + l1 + l2 + l3 + 10'd2;
				dc_v = dc_sum >>> 2;
			end else begin
				dc_v = 8'd128;
			end
			for (i = 0; i < 16; i = i + 1) pred[i] = dc_v;
		end
		4'd3: begin // Diagonal Down-Left
			put(0, 0, (t0 + t2 + 2 * t1 + 2) >>> 2);
			put(1, 0, (t1 + t3 + 2 * t2 + 2) >>> 2); put(0, 1, (t1 + t3 + 2 * t2 + 2) >>> 2);
			put(2, 0, (t2 + t4 + 2 * t3 + 2) >>> 2); put(1, 1, (t2 + t4 + 2 * t3 + 2) >>> 2); put(0, 2, (t2 + t4 + 2 * t3 + 2) >>> 2);
			put(3, 0, (t3 + t5 + 2 * t4 + 2) >>> 2); put(2, 1, (t3 + t5 + 2 * t4 + 2) >>> 2); put(1, 2, (t3 + t5 + 2 * t4 + 2) >>> 2); put(0, 3, (t3 + t5 + 2 * t4 + 2) >>> 2);
			put(3, 1, (t4 + t6 + 2 * t5 + 2) >>> 2); put(2, 2, (t4 + t6 + 2 * t5 + 2) >>> 2); put(1, 3, (t4 + t6 + 2 * t5 + 2) >>> 2);
			put(3, 2, (t5 + t7 + 2 * t6 + 2) >>> 2); put(2, 3, (t5 + t7 + 2 * t6 + 2) >>> 2);
			put(3, 3, (t6 + 3 * t7 + 2) >>> 2);
		end
		4'd4: begin // Diagonal Down-Right
			put(0, 3, (l3 + 2 * l2 + l1 + 2) >>> 2);
			put(0, 2, (l2 + 2 * l1 + l0 + 2) >>> 2); put(1, 3, (l2 + 2 * l1 + l0 + 2) >>> 2);
			put(0, 1, (l1 + 2 * l0 + top_left + 2) >>> 2); put(1, 2, (l1 + 2 * l0 + top_left + 2) >>> 2); put(2, 3, (l1 + 2 * l0 + top_left + 2) >>> 2);
			put(0, 0, (l0 + 2 * top_left + t0 + 2) >>> 2); put(1, 1, (l0 + 2 * top_left + t0 + 2) >>> 2); put(2, 2, (l0 + 2 * top_left + t0 + 2) >>> 2); put(3, 3, (l0 + 2 * top_left + t0 + 2) >>> 2);
			put(1, 0, (top_left + 2 * t0 + t1 + 2) >>> 2); put(2, 1, (top_left + 2 * t0 + t1 + 2) >>> 2); put(3, 2, (top_left + 2 * t0 + t1 + 2) >>> 2);
			put(2, 0, (t0 + 2 * t1 + t2 + 2) >>> 2); put(3, 1, (t0 + 2 * t1 + t2 + 2) >>> 2);
			put(3, 0, (t1 + 2 * t2 + t3 + 2) >>> 2);
		end
		4'd5: begin // Vertical-Right
			put(0, 0, (top_left + t0 + 1) >>> 1); put(1, 2, (top_left + t0 + 1) >>> 1);
			put(1, 0, (t0 + t1 + 1) >>> 1); put(2, 2, (t0 + t1 + 1) >>> 1);
			put(2, 0, (t1 + t2 + 1) >>> 1); put(3, 2, (t1 + t2 + 1) >>> 1);
			put(3, 0, (t2 + t3 + 1) >>> 1);
			put(0, 1, (l0 + 2 * top_left + t0 + 2) >>> 2); put(1, 3, (l0 + 2 * top_left + t0 + 2) >>> 2);
			put(1, 1, (top_left + 2 * t0 + t1 + 2) >>> 2); put(2, 3, (top_left + 2 * t0 + t1 + 2) >>> 2);
			put(2, 1, (t0 + 2 * t1 + t2 + 2) >>> 2); put(3, 3, (t0 + 2 * t1 + t2 + 2) >>> 2);
			put(3, 1, (t1 + 2 * t2 + t3 + 2) >>> 2);
			put(0, 2, (top_left + 2 * l0 + l1 + 2) >>> 2); put(0, 3, (l0 + 2 * l1 + l2 + 2) >>> 2);
		end
		4'd6: begin // Horizontal-Down
			put(0, 0, (top_left + l0 + 1) >>> 1); put(2, 1, (top_left + l0 + 1) >>> 1);
			put(1, 0, (l0 + 2 * top_left + t0 + 2) >>> 2); put(3, 1, (l0 + 2 * top_left + t0 + 2) >>> 2);
			put(2, 0, (top_left + 2 * t0 + t1 + 2) >>> 2); put(3, 0, (t0 + 2 * t1 + t2 + 2) >>> 2);
			put(0, 1, (l0 + l1 + 1) >>> 1); put(2, 2, (l0 + l1 + 1) >>> 1);
			put(1, 1, (top_left + 2 * l0 + l1 + 2) >>> 2); put(3, 2, (top_left + 2 * l0 + l1 + 2) >>> 2);
			put(0, 2, (l1 + l2 + 1) >>> 1); put(2, 3, (l1 + l2 + 1) >>> 1);
			put(1, 2, (l0 + 2 * l1 + l2 + 2) >>> 2); put(3, 3, (l0 + 2 * l1 + l2 + 2) >>> 2);
			put(0, 3, (l2 + l3 + 1) >>> 1); put(1, 3, (l1 + 2 * l2 + l3 + 2) >>> 2);
		end
		4'd7: begin // Vertical-Left
			put(0, 0, (t0 + t1 + 1) >>> 1); put(1, 0, (t1 + t2 + 1) >>> 1); put(0, 2, (t1 + t2 + 1) >>> 1);
			put(2, 0, (t2 + t3 + 1) >>> 1); put(1, 2, (t2 + t3 + 1) >>> 1);
			put(3, 0, (t3 + t4 + 1) >>> 1); put(2, 2, (t3 + t4 + 1) >>> 1);
			put(3, 2, (t4 + t5 + 1) >>> 1);
			put(0, 1, (t0 + 2 * t1 + t2 + 2) >>> 2);
			put(1, 1, (t1 + 2 * t2 + t3 + 2) >>> 2); put(0, 3, (t1 + 2 * t2 + t3 + 2) >>> 2);
			put(2, 1, (t2 + 2 * t3 + t4 + 2) >>> 2); put(1, 3, (t2 + 2 * t3 + t4 + 2) >>> 2);
			put(3, 1, (t3 + 2 * t4 + t5 + 2) >>> 2); put(2, 3, (t3 + 2 * t4 + t5 + 2) >>> 2);
			put(3, 3, (t4 + 2 * t5 + t6 + 2) >>> 2);
		end
		4'd8: begin // Horizontal-Up
			put(0, 0, (l0 + l1 + 1) >>> 1); put(1, 0, (l0 + 2 * l1 + l2 + 2) >>> 2);
			put(2, 0, (l1 + l2 + 1) >>> 1); put(0, 1, (l1 + l2 + 1) >>> 1);
			put(3, 0, (l1 + 2 * l2 + l3 + 2) >>> 2); put(1, 1, (l1 + 2 * l2 + l3 + 2) >>> 2);
			put(2, 1, (l2 + l3 + 1) >>> 1); put(0, 2, (l2 + l3 + 1) >>> 1);
			put(3, 1, (l2 + 2 * l3 + l3 + 2) >>> 2); put(1, 2, (l2 + 2 * l3 + l3 + 2) >>> 2);
			put(3, 2, l3); put(1, 3, l3); put(0, 3, l3); put(2, 2, l3); put(2, 3, l3); put(3, 3, l3);
		end
		default: begin
			used_mode = 4'd15;
			for (i = 0; i < 16; i = i + 1) pred[i] = 8'd128;
		end
		endcase
	end
endmodule

module h264_intra16x16_pred (
	input  wire        clk,
	input  wire        start,
	input  wire [1:0]  mode,
	input  wire [7:0]  above [0:15],
	input  wire [7:0]  left [0:15],
	input  wire [7:0]  top_left,
	input  wire        has_above,
	input  wire        has_left,
	output reg         unsupported,
	output reg         valid,
	output reg  [7:0]  pred [0:255]
);
	// 2-cycle pipeline for Plane prediction (ITU-T H.264 clause 8.3.3.4).
	// Cycle 1: gradient accumulation → b, c → pre-compute 32 products bx[]/cy[]
	// Cycle 2: 256 pixels from registered a + bx[x] + cy[y] → clip
	// Modes V/H/DC: 1 cycle (register combinational result on start).

	function automatic [7:0] clip8;
		input integer v;
		begin
			if (v < 0) clip8 = 8'd0;
			else if (v > 255) clip8 = 8'd255;
			else clip8 = v[7:0];
		end
	endfunction

	// Pipeline phase: 0 = idle, 1 = Plane cycle 2 pending
	reg phase = 1'b0;

	// Registered intermediates for Plane pipeline (32 products, not 256)
	reg signed [15:0] a_r;
	reg signed [15:0] bx_r [0:15];
	reg signed [15:0] cy_r [0:15];

	// Combinational gradient computation (feeds cycle 1 registers)
	integer hgrad_c, vgrad_c, a_c, b_c, c_c;
	integer gi;
	// Individual gradient terms (computed independently, then tree-reduced)
	integer ht [0:7];
	integer vt [0:7];
	always @* begin
		// Compute individual gradient terms (clause 8.3.3.4)
		for (gi = 0; gi < 8; gi = gi + 1) begin
			ht[gi] = (gi + 1) * ($signed({1'b0, above[8 + gi]}) - ((gi == 7) ? $signed({1'b0, top_left}) : $signed({1'b0, above[6 - gi]})));
			vt[gi] = (gi + 1) * ($signed({1'b0, left[8 + gi]})  - ((gi == 7) ? $signed({1'b0, top_left}) : $signed({1'b0, left[6 - gi]})));
		end
		// Balanced tree reduction: 3 add levels instead of 7 in linear chain
		hgrad_c = ((ht[0]+ht[1]) + (ht[2]+ht[3])) + ((ht[4]+ht[5]) + (ht[6]+ht[7]));
		vgrad_c = ((vt[0]+vt[1]) + (vt[2]+vt[3])) + ((vt[4]+vt[5]) + (vt[6]+vt[7]));
		a_c = 16 * ($signed({1'b0, above[15]}) + $signed({1'b0, left[15]}));
		b_c = (5 * hgrad_c + 32) >>> 6;
		c_c = (5 * vgrad_c + 32) >>> 6;
	end

	integer x, y, i;
	integer sum, sa_lo, sa_hi, sl_lo, sl_hi;
	reg [7:0] dc_v;
	integer val;

	always @(posedge clk) begin
		valid <= 1'b0;

		if (phase) begin
			// Plane cycle 2: evaluate 256 pixels from 32 registered products
			for (y = 0; y < 16; y = y + 1)
				for (x = 0; x < 16; x = x + 1) begin
					val = ($signed(a_r) + $signed(bx_r[x]) + $signed(cy_r[y]) + 16) >>> 5;
					pred[y * 16 + x] <= clip8(val);
				end
			valid <= 1'b1;
			phase <= 1'b0;
		end else if (start) begin
			unsupported <= 1'b0;
			if (mode == 2'd3) begin
				if (has_above && has_left) begin
					// Plane cycle 1: register a and 32 pre-computed products
					a_r <= a_c[15:0];
					for (i = 0; i < 16; i = i + 1) begin
						bx_r[i] <= b_c * (i - 7);
						cy_r[i] <= c_c * (i - 7);
					end
					phase <= 1'b1;
				end else begin
					for (i = 0; i < 256; i = i + 1) pred[i] <= 8'd128;
					valid <= 1'b1;
				end
			end else if (mode == 2'd0 && has_above) begin
				for (y = 0; y < 16; y = y + 1)
					for (x = 0; x < 16; x = x + 1) pred[y * 16 + x] <= above[x];
				valid <= 1'b1;
			end else if (mode == 2'd1 && has_left) begin
				for (y = 0; y < 16; y = y + 1)
					for (x = 0; x < 16; x = x + 1) pred[y * 16 + x] <= left[y];
				valid <= 1'b1;
			end else begin
				// Balanced tree: 5 add levels guaranteed vs up to 32 in linear chain
				sa_lo = ((above[0]+above[1]) + (above[2]+above[3]))
				      + ((above[4]+above[5]) + (above[6]+above[7]));
				sa_hi = ((above[8]+above[9]) + (above[10]+above[11]))
				      + ((above[12]+above[13]) + (above[14]+above[15]));
				sl_lo = ((left[0]+left[1]) + (left[2]+left[3]))
				      + ((left[4]+left[5]) + (left[6]+left[7]));
				sl_hi = ((left[8]+left[9]) + (left[10]+left[11]))
				      + ((left[12]+left[13]) + (left[14]+left[15]));
				if (has_above && has_left) sum = (sa_lo + sa_hi) + (sl_lo + sl_hi);
				else if (has_above) sum = sa_lo + sa_hi;
				else if (has_left) sum = sl_lo + sl_hi;
				else sum = 0;
				if (has_above && has_left) dc_v = (sum + 16) >>> 5;
				else if (has_above || has_left) dc_v = (sum + 8) >>> 4;
				else dc_v = 8'd128;
				for (i = 0; i < 256; i = i + 1) pred[i] <= dc_v;
				valid <= 1'b1;
			end
		end
	end
endmodule

module h264_chroma8x8_pred (
	input  wire        clk,
	input  wire        start,
	input  wire [1:0]  mode,
	input  wire [7:0]  above [0:7],
	input  wire [7:0]  left [0:7],
	input  wire [7:0]  top_left,
	input  wire        has_above,
	input  wire        has_left,
	output reg         valid,
	output reg  [7:0]  pred [0:63]
);
	// 2-cycle pipeline for Chroma Plane prediction (ITU-T H.264 clause 8.3.4.4).
	// Cycle 1: gradient accumulation → b, c → pre-compute 16 products bx[]/cy[]
	// Cycle 2: 64 pixels from registered a + bx[x] + cy[y] → clip
	// Modes DC/H/V: 1 cycle (register combinational result on start).

	function automatic [7:0] clip8;
		input integer v;
		begin
			if (v < 0) clip8 = 8'd0;
			else if (v > 255) clip8 = 8'd255;
			else clip8 = v[7:0];
		end
	endfunction

	// Pipeline phase: 0 = idle, 1 = Plane cycle 2 pending
	reg phase = 1'b0;

	// Registered intermediates for Plane pipeline (16 products, not 64)
	reg signed [15:0] a_r;
	reg signed [15:0] bx_r [0:7];
	reg signed [15:0] cy_r [0:7];

	// Combinational gradient computation (clause 8.3.4.4)
	integer hgrad_c, vgrad_c, a_c, b_c, c_c;
	integer gi;
	always @* begin
		hgrad_c = 0;
		vgrad_c = 0;
		for (gi = 0; gi < 4; gi = gi + 1) begin
			hgrad_c = hgrad_c + (gi + 1) * ($signed({1'b0, above[4 + gi]}) - ((gi == 3) ? $signed({1'b0, top_left}) : $signed({1'b0, above[2 - gi]})));
			vgrad_c = vgrad_c + (gi + 1) * ($signed({1'b0, left[4 + gi]})  - ((gi == 3) ? $signed({1'b0, top_left}) : $signed({1'b0, left[2 - gi]})));
		end
		a_c = 16 * ($signed({1'b0, above[7]}) + $signed({1'b0, left[7]}));
		b_c = (17 * hgrad_c + 16) >>> 5;
		c_c = (17 * vgrad_c + 16) >>> 5;
	end

	integer x, y, i;
	integer sum_a0, sum_a1, sum_l0, sum_l1;
	integer val;
	reg [7:0] dc_tl, dc_tr, dc_bl, dc_br;

	always @(posedge clk) begin
		valid <= 1'b0;

		if (phase) begin
			// Plane cycle 2: evaluate 64 pixels from 16 registered products
			for (y = 0; y < 8; y = y + 1)
				for (x = 0; x < 8; x = x + 1) begin
					val = ($signed(a_r) + $signed(bx_r[x]) + $signed(cy_r[y]) + 16) >>> 5;
					pred[y * 8 + x] <= clip8(val);
				end
			valid <= 1'b1;
			phase <= 1'b0;
		end else if (start) begin
			if (mode == 2'd3) begin
				if (has_above && has_left) begin
					// Plane cycle 1: register a and 16 pre-computed products
					a_r <= a_c[15:0];
					for (i = 0; i < 8; i = i + 1) begin
						bx_r[i] <= b_c * (i - 3);
						cy_r[i] <= c_c * (i - 3);
					end
					phase <= 1'b1;
				end else begin
					for (i = 0; i < 64; i = i + 1) pred[i] <= 8'd128;
					valid <= 1'b1;
				end
			end else if (mode == 2'd0) begin
				// DC with 4 quadrant sub-averages (clause 8.3.4.1)
				sum_a0 = (above[0]+above[1]) + (above[2]+above[3]);
				sum_a1 = (above[4]+above[5]) + (above[6]+above[7]);
				sum_l0 = (left[0]+left[1]) + (left[2]+left[3]);
				sum_l1 = (left[4]+left[5]) + (left[6]+left[7]);
				if (has_above && has_left) begin
					dc_tl = clip8((sum_a0 + sum_l0 + 4) >>> 3);
					dc_tr = clip8((sum_a1 + 2) >>> 2);
					dc_bl = clip8((sum_l1 + 2) >>> 2);
					dc_br = clip8((sum_a1 + sum_l1 + 4) >>> 3);
				end else if (has_above) begin
					dc_tl = clip8((sum_a0 + 2) >>> 2);
					dc_tr = clip8((sum_a1 + 2) >>> 2);
					dc_bl = dc_tl;
					dc_br = dc_tr;
				end else if (has_left) begin
					dc_tl = clip8((sum_l0 + 2) >>> 2);
					dc_tr = dc_tl;
					dc_bl = clip8((sum_l1 + 2) >>> 2);
					dc_br = dc_bl;
				end else begin
					dc_tl = 8'd128; dc_tr = 8'd128;
					dc_bl = 8'd128; dc_br = 8'd128;
				end
				for (y = 0; y < 4; y = y + 1)
					for (x = 0; x < 4; x = x + 1) pred[y*8+x] <= dc_tl;
				for (y = 0; y < 4; y = y + 1)
					for (x = 4; x < 8; x = x + 1) pred[y*8+x] <= dc_tr;
				for (y = 4; y < 8; y = y + 1)
					for (x = 0; x < 4; x = x + 1) pred[y*8+x] <= dc_bl;
				for (y = 4; y < 8; y = y + 1)
					for (x = 4; x < 8; x = x + 1) pred[y*8+x] <= dc_br;
				valid <= 1'b1;
			end else if (mode == 2'd1) begin
				// Horizontal
				for (y = 0; y < 8; y = y + 1)
					for (x = 0; x < 8; x = x + 1) pred[y*8+x] <= left[y];
				valid <= 1'b1;
			end else begin
				// Vertical (mode 2)
				for (y = 0; y < 8; y = y + 1)
					for (x = 0; x < 8; x = x + 1) pred[y*8+x] <= above[x];
				valid <= 1'b1;
			end
		end
	end
endmodule

module h264_intra_mode_guard (
	input  wire        clk,
	input  wire        reset,
	input  wire        mb_valid,
	input  wire [7:0]  mb_type,
	input  wire [1:0]  i16_pred_mode,
	input  wire [15:0] mb_index,
	input  wire [4:0]  block_index,
	output reg         unsupported_valid,
	output reg         unsupported_seen,
	output reg  [3:0]  unsupported_code,
	output reg  [15:0] unsupported_mb,
	output reg  [4:0]  unsupported_block
);
	localparam [3:0] UNSUP_I16_PLANE = 4'd1;
	localparam [3:0] UNSUP_IPCM      = 4'd2;
	localparam [3:0] UNSUP_MB_TYPE   = 4'd3;

	wire is_i16 = (mb_type >= 8'd1) && (mb_type <= 8'd24);
	wire is_i4  = (mb_type == 8'd0);
	wire is_ipcm = (mb_type == 8'd25);
	wire bad_type = !(is_i4 || is_i16 || is_ipcm);

	always @(posedge clk) begin
		unsupported_valid <= 1'b0;
		if (reset) begin
			unsupported_seen  <= 1'b0;
			unsupported_code  <= 4'd0;
			unsupported_mb    <= 16'd0;
			unsupported_block <= 5'd0;
		end else if (mb_valid && (is_ipcm || bad_type)) begin
			unsupported_valid <= 1'b1;
			unsupported_seen  <= 1'b1;
			unsupported_code  <= is_ipcm ? UNSUP_IPCM : UNSUP_MB_TYPE;
			unsupported_mb    <= mb_index;
			unsupported_block <= block_index;
		end
	end
endmodule
