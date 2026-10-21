// SMPTE-style color bars + optional moving block driven by content frame index.
// Generates progressive 320x240-class content timing (NTSC 15 kHz family when
// not scandoubled) inside Template-class horizontal DE.
//
// Fix-2 (P3-WIDE): Template HBlank@529 + HSync 544/590 (FBAR-proven DE class)
// with paint across the FULL DE (hc 0..528). Fix-1 HBlank@320 / HSync 336/384
// was ineffective on silicon eyes-on (still PILLAR_320_of_529). frame_store
// remains 320×240 left-aligned in present_core.

module colorbars (
	input  wire        clk,
	input  wire        reset,

	input  wire        pal,           // 0 NTSC-like, 1 PAL-like
	input  wire        scandouble,
	input  wire [31:0] content_index, // unique frame number (cadence-advanced)
	input  wire [1:0]  pattern,       // 0=none(black), 1=bars, 2=bars+block, 3=grid

	output reg         ce_pix,
	output reg         HBlank,
	output reg         HSync,
	output reg         VBlank,
	output reg         VSync,
	output reg         frame_start,   // 1-cycle pulse when leaving VBlank (display tick)

	// Expose counters so present can stretch frame_store with the SAME hc/vc
	// colorbars uses for full-DE paint (reconstructed counters can skew).
	output wire [9:0]  hc_out,
	output wire [9:0]  vc_out,

	output reg  [7:0]  r,
	output reg  [7:0]  g,
	output reg  [7:0]  b
);

	// Template A timing (mycore-class). H_CONTENT is product width only.
	localparam H_CONTENT = 10'd320;  // product / frame_store only
	localparam H_DE      = 10'd529;  // Template active DE width
	localparam H_LAST    = 10'd637;  // wrap → 638 clocks/line (FPS)
	localparam H_BLANK_S = H_DE;     // 529
	localparam H_SYNC_S  = 10'd544;  // Template
	localparam H_SYNC_E  = 10'd590;  // Template

	reg [9:0] hc;
	reg [9:0] vc;
	assign hc_out = hc;
	assign vc_out = vc;

	// Pixel enable + counters (Template mycore style)
	always @(posedge clk) begin
		if (scandouble)
			ce_pix <= 1'b1;
		else
			ce_pix <= ~ce_pix;

		frame_start <= 1'b0;

		if (reset) begin
			hc     <= 0;
			vc     <= 0;
			ce_pix <= 0;
		end else if (ce_pix) begin
			if (hc == H_LAST) begin
				hc <= 0;
				if (vc == (pal ? (scandouble ? 10'd623 : 10'd311)
				               : (scandouble ? 10'd523 : 10'd261))) begin
					vc <= 0;
				end else begin
					vc <= vc + 1'd1;
				end
			end else begin
				hc <= hc + 1'd1;
			end

			// Display tick at start of first active line
			if (hc == H_LAST &&
			    vc == (pal ? (scandouble ? 10'd623 : 10'd311)
			               : (scandouble ? 10'd523 : 10'd261)))
				frame_start <= 1'b1;
		end
	end

	// H/V blank & sync (sample hc every clk; hc advances on ce_pix)
	// Level HBlank: DE width = H_DE (529).
	always @(posedge clk) begin
		if (reset) begin
			HBlank <= 1'b1;
			HSync  <= 1'b0;
			VBlank <= 1'b1;
			VSync  <= 1'b0;
		end else begin
			HBlank <= (hc >= H_BLANK_S);

			if (hc == H_SYNC_S)
				HSync <= 1'b1;
			if (hc == H_SYNC_E)
				HSync <= 1'b0;

			// V edges still keyed off HSync start (Template-style)
			if (hc == H_SYNC_S) begin
				if (pal) begin
					if (vc == (scandouble ? 10'd609 : 10'd304))
						VSync <= 1'b1;
					else if (vc == (scandouble ? 10'd617 : 10'd308))
						VSync <= 1'b0;

					if (vc == (scandouble ? 10'd601 : 10'd300))
						VBlank <= 1'b1;
					else if (vc == 10'd0)
						VBlank <= 1'b0;
				end else begin
					if (vc == (scandouble ? 10'd490 : 10'd245))
						VSync <= 1'b1;
					else if (vc == (scandouble ? 10'd496 : 10'd248))
						VSync <= 1'b0;

					if (vc == (scandouble ? 10'd480 : 10'd240))
						VBlank <= 1'b1;
					else if (vc == 10'd0)
						VBlank <= 1'b0;
				end
			end
		end
	end

	// Color bars fill the full DE window (hc 0..H_DE-1). Stretch 7 bars.
	wire [9:0] px = hc;
	wire [9:0] py = scandouble ? (vc >> 1) : vc;
	// Integer scale: 7 equal slices over DE width 529 (hc=528 → 3696/529=6)
	wire [12:0] bar_prod = hc * 4'd7; // 0 .. 3696
	wire [2:0]  bar      = bar_prod / H_DE; // 0..6
	wire in_content = (hc < H_DE) &&
	                  (py < 10'd240) &&
	                  ~HBlank && ~VBlank;

	reg [7:0] br, bg, bb;
	always @(*) begin
		br = 8'd0; bg = 8'd0; bb = 8'd0;
		case (bar)
			3'd0: begin br = 8'hC0; bg = 8'hC0; bb = 8'hC0; end // white
			3'd1: begin br = 8'hC0; bg = 8'hC0; bb = 8'h00; end // yellow
			3'd2: begin br = 8'h00; bg = 8'hC0; bb = 8'hC0; end // cyan
			3'd3: begin br = 8'h00; bg = 8'hC0; bb = 8'h00; end // green
			3'd4: begin br = 8'hC0; bg = 8'h00; bb = 8'hC0; end // magenta
			3'd5: begin br = 8'hC0; bg = 8'h00; bb = 8'h00; end // red
			3'd6: begin br = 8'h00; bg = 8'h00; bb = 8'hC0; end // blue
			default: begin br = 8'h00; bg = 8'h00; bb = 8'h00; end // black
		endcase
	end

	// Moving block position from content_index (unique frames only)
	wire [7:0] bx = content_index[7:0];
	wire [7:0] by = content_index[15:8] ^ content_index[7:0];
	wire in_block = (px >= {2'b0, bx}) && (px < {2'b0, bx} + 10'd32)
	             && (py >= {2'b0, by[6:0], 1'b0}) && (py < {2'b0, by[6:0], 1'b0} + 10'd24);

	wire grid = px[3] ^ py[3];

	always @(posedge clk) begin
		if (ce_pix) begin
			if (!in_content) begin
				r <= 0; g <= 0; b <= 0;
			end else begin
				case (pattern)
					2'd0: begin // None — black (idle / product default)
						r <= 8'd0; g <= 8'd0; b <= 8'd0;
					end
					2'd1: begin // Bars
						r <= br; g <= bg; b <= bb;
					end
					2'd2: begin // Bars + moving block
						if (in_block) begin
							r <= 8'hFF; g <= 8'hFF; b <= 8'h00;
						end else begin
							r <= br; g <= bg; b <= bb;
						end
					end
					default: begin // Grid
						r <= grid ? 8'hFF : 8'h20;
						g <= grid ? 8'hFF : 8'h20;
						b <= grid ? 8'hFF : 8'h20;
					end
				endcase
			end
		end
	end

endmodule
