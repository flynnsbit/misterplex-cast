// Multi-pixel beam generator for present path.
//
// Advances glass X by PX_PER_CLK pixels per beam_ce cycle. Blanking compares
// remain in pixel units (H_DE, H_TOTAL-1). Intended replacement/companion for
// colorbars timing when PRESENT_MULTI_PIXEL is enabled — NOT wired into the
// product path by default.
//
// Constraints (see MULTI_PIXEL_PRESENT_DESIGN.md):
//   - PX_PER_CLK in {1,2,4}
//   - Prefer H_DE % PX_PER_CLK == 0 and (H_LAST+1) % PX_PER_CLK == 0
//   - CEA H_TOTAL=1650 is EVEN (1650%2=0). 1650%4=2 → PPC=4 needs short final
//     step of 2 (handled below). Not "odd total".

module present_beam_ppc #(
	parameter int PX_PER_CLK = 1,
	parameter int H_DE       = 1280,
	parameter int H_TOTAL    = 1650,
	parameter int V_ACTIVE   = 720,
	parameter int V_TOTAL    = 750,
	parameter int H_SYNC_S   = 1390,
	parameter int H_SYNC_E   = 1430,
	parameter int V_SYNC_S   = 725,
	parameter int V_SYNC_E   = 730,
	parameter int X_W        = 12,
	parameter int Y_W        = 12
)(
	input  wire            clk,
	input  wire            reset,
	input  wire            enable,       // 0: hold; 1: run (product scandouble ce=1)

	output reg             beam_ce,      // 1 when a group is issued (always 1 when enable)
	output reg  [X_W-1:0]  glass_x0,     // leftmost pixel of this group
	output reg  [Y_W-1:0]  glass_y,
	output reg  [PX_PER_CLK-1:0] lane_de,
	output reg             HBlank,
	output reg             HSync,
	output reg             VBlank,
	output reg             VSync,
	output reg             frame_start
);
	localparam int H_LAST = H_TOTAL - 1;
	localparam int V_LAST = V_TOTAL - 1;

	reg [X_W-1:0] hc;
	reg [Y_W-1:0] vc;

	integer li;
	reg [X_W-1:0] x_lane;
	reg [X_W-1:0] step;
	reg [X_W-1:0] remain;

	// Step size: full PPC until the final partial group on the line.
	always @* begin
		remain = X_W'(H_LAST) - hc + X_W'(1);
		if (remain >= X_W'(PX_PER_CLK))
			step = X_W'(PX_PER_CLK);
		else if (remain == 0)
			step = X_W'(PX_PER_CLK); // unused; wrap path
		else
			step = remain;
	end

	always @(posedge clk) begin
		if (reset) begin
			hc          <= '0;
			vc          <= '0;
			beam_ce     <= 1'b0;
			glass_x0    <= '0;
			glass_y     <= '0;
			lane_de     <= '0;
			HBlank      <= 1'b1;
			HSync       <= 1'b0;
			VBlank      <= 1'b1;
			VSync       <= 1'b0;
			frame_start <= 1'b0;
		end else if (!enable) begin
			beam_ce     <= 1'b0;
			frame_start <= 1'b0;
		end else begin
			beam_ce     <= 1'b1;
			frame_start <= 1'b0;
			glass_x0    <= hc;
			glass_y     <= vc;

			for (li = 0; li < PX_PER_CLK; li = li + 1) begin
				x_lane = hc + X_W'(li);
				lane_de[li] <= (x_lane < X_W'(H_DE)) && (vc < Y_W'(V_ACTIVE));
			end

			HBlank <= (hc >= X_W'(H_DE));
			if (hc >= X_W'(H_SYNC_S) && hc < X_W'(H_SYNC_E))
				HSync <= 1'b1;
			else
				HSync <= 1'b0;

			VBlank <= (vc >= Y_W'(V_ACTIVE));
			if (vc >= Y_W'(V_SYNC_S) && vc < Y_W'(V_SYNC_E))
				VSync <= 1'b1;
			else
				VSync <= 1'b0;

			// Advance beam in pixel units by `step` (handles 1650%4=2 tail).
			if (hc + step > X_W'(H_LAST)) begin
				hc <= '0;
				if (vc >= Y_W'(V_LAST)) begin
					vc <= '0;
					frame_start <= 1'b1;
				end else begin
					vc <= vc + Y_W'(1);
				end
			end else begin
				hc <= hc + step;
			end
		end
	end
endmodule
