// Phase 3.3d/e: Baseline PPS parse — CAVLC only, no FMO; export deblock_ctrl.

module pps_parser (
	input  wire        clk,
	input  wire        reset,

	input  wire        cap_clear,
	input  wire        cap_en,
	input  wire [7:0]  cap_data,
	input  wire        cap_end,

	output reg         valid,
	output reg  [7:0]  pps_id,
	output reg  [7:0]  sps_id,
	output reg         entropy_cabac,
	output reg  [7:0]  num_ref_l0,
	output reg  signed [7:0] pic_init_qp,
	output reg         deblock_ctrl,
	output reg         busy
);

	localparam int MAXB = 24;
	reg [7:0] mem [0:MAXB-1];
	reg [4:0] len;

	reg [4:0] bbyte;
	reg [2:0] bpos;
	wire [7:0] cur = mem[bbyte];
	wire bitv = cur[bpos];
	wire oob = (bbyte >= len);

	reg [4:0] st, cont, ue_cont;
	reg [5:0] zcnt;
	reg [4:0] nleft;
	reg [15:0] acc, ue_val;

	localparam [4:0]
		ST_IDLE    = 5'd0,
		ST_GETBITS = 5'd1,
		ST_UE_Z    = 5'd2,
		ST_UE_V    = 5'd3,
		ST_PPSID   = 5'd4,
		ST_SPSID   = 5'd5,
		ST_ENT     = 5'd6,
		ST_BOT     = 5'd7,
		ST_SG      = 5'd8,
		ST_NRL0    = 5'd9,
		ST_NRL1    = 5'd10,
		ST_WP      = 5'd11,
		ST_WBI     = 5'd12,
		ST_QP      = 5'd13,
		ST_QS      = 5'd14,
		ST_CHR     = 5'd15,
		ST_DB      = 5'd16,
		ST_CI      = 5'd17,
		ST_RED     = 5'd18,
		ST_DONE    = 5'd19,
		ST_FAIL    = 5'd20;

	function automatic signed [7:0] se_of;
		input [15:0] k;
		begin
			if (k[0] == 1'b0)
				se_of = -$signed({1'b0, k[7:1]});
			else
				se_of = $signed({1'b0, k[7:1]}) + 8'sd1;
		end
	endfunction

	always @(posedge clk) begin
		if (reset || cap_clear)
			len <= 0;
		else if (cap_en && len < MAXB[4:0]) begin
			mem[len] <= cap_data;
			len <= len + 1'd1;
		end
	end

	always @(posedge clk) begin
		if (reset) begin
			st <= ST_IDLE;
			valid <= 0;
			busy <= 0;
			pps_id <= 0;
			sps_id <= 0;
			entropy_cabac <= 0;
			num_ref_l0 <= 0;
			pic_init_qp <= 8'sd26;
			deblock_ctrl <= 0;
			bbyte <= 0;
			bpos <= 3'd7;
			zcnt <= 0;
			nleft <= 0;
			acc <= 0;
			ue_val <= 0;
			cont <= ST_IDLE;
			ue_cont <= ST_IDLE;
		end else begin
			case (st)
			ST_IDLE: begin
				busy <= 0;
				if (cap_end && len >= 5'd2) begin
					busy <= 1'b1;
					bbyte <= 0;
					bpos <= 3'd7;
					zcnt <= 0;
					ue_cont <= ST_PPSID;
					st <= ST_UE_Z;
				end
			end

			ST_GETBITS: begin
				if (oob) st <= ST_FAIL;
				else begin
					acc <= {acc[14:0], bitv};
					if (bpos == 3'd0) begin bpos <= 3'd7; bbyte <= bbyte + 1'd1; end
					else bpos <= bpos - 1'd1;
					if (nleft == 5'd1) st <= cont;
					else nleft <= nleft - 1'd1;
				end
			end

			ST_UE_Z: begin
				if (oob) st <= ST_FAIL;
				else if (bitv == 1'b0) begin
					if (zcnt >= 6'd16) st <= ST_FAIL;
					else begin
						zcnt <= zcnt + 1'd1;
						if (bpos == 3'd0) begin bpos <= 3'd7; bbyte <= bbyte + 1'd1; end
						else bpos <= bpos - 1'd1;
					end
				end else begin
					if (bpos == 3'd0) begin bpos <= 3'd7; bbyte <= bbyte + 1'd1; end
					else bpos <= bpos - 1'd1;
					if (zcnt == 0) begin ue_val <= 0; st <= ue_cont; end
					else begin
						nleft <= zcnt[4:0]; acc <= 0; cont <= ST_UE_V; st <= ST_GETBITS;
					end
				end
			end

			ST_UE_V: begin
				ue_val <= ((16'd1 << zcnt) - 16'd1) + acc;
				st <= ue_cont;
			end

			ST_PPSID: begin pps_id <= ue_val[7:0]; zcnt <= 0; ue_cont <= ST_SPSID; st <= ST_UE_Z; end
			ST_SPSID: begin sps_id <= ue_val[7:0]; nleft <= 5'd1; acc <= 0; cont <= ST_ENT; st <= ST_GETBITS; end
			ST_ENT: begin entropy_cabac <= acc[0]; nleft <= 5'd1; acc <= 0; cont <= ST_BOT; st <= ST_GETBITS; end
			ST_BOT: begin zcnt <= 0; ue_cont <= ST_SG; st <= ST_UE_Z; end
			ST_SG: begin
				if (ue_val != 0) st <= ST_FAIL;
				else begin zcnt <= 0; ue_cont <= ST_NRL0; st <= ST_UE_Z; end
			end
			ST_NRL0: begin num_ref_l0 <= ue_val[7:0]; zcnt <= 0; ue_cont <= ST_NRL1; st <= ST_UE_Z; end
			ST_NRL1: begin nleft <= 5'd1; acc <= 0; cont <= ST_WP; st <= ST_GETBITS; end
			ST_WP: begin nleft <= 5'd2; acc <= 0; cont <= ST_WBI; st <= ST_GETBITS; end
			ST_WBI: begin zcnt <= 0; ue_cont <= ST_QP; st <= ST_UE_Z; end
			ST_QP: begin
				pic_init_qp <= 8'sd26 + se_of(ue_val);
				zcnt <= 0; ue_cont <= ST_QS; st <= ST_UE_Z;
			end
			ST_QS: begin zcnt <= 0; ue_cont <= ST_CHR; st <= ST_UE_Z; end
			ST_CHR: begin nleft <= 5'd1; acc <= 0; cont <= ST_DB; st <= ST_GETBITS; end
			ST_DB: begin
				deblock_ctrl <= acc[0];
				nleft <= 5'd1; acc <= 0; cont <= ST_CI; st <= ST_GETBITS;
			end
			ST_CI: begin nleft <= 5'd1; acc <= 0; cont <= ST_RED; st <= ST_GETBITS; end
			ST_RED: begin st <= ST_DONE; end
			ST_DONE: begin
				busy <= 0;
				st <= ST_IDLE;
				if (!entropy_cabac)
					valid <= 1'b1;
			end
			ST_FAIL: begin busy <= 0; st <= ST_IDLE; end
			default: st <= ST_IDLE;
			endcase
		end
	end

endmodule
