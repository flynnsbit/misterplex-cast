// Phase 3.3c: H.264 SPS RBSP → width/height/profile (Baseline / Main progressive).
// Feeder supplies RBSP bytes (EPB already stripped). On cap_end, sequential bit-walk.
// High profiles (100+) → fail (product path is Baseline without scaling lists).

module sps_parser (
	input  wire        clk,
	input  wire        reset,

	input  wire        cap_clear,
	input  wire        cap_en,
	input  wire [7:0]  cap_data,
	input  wire        cap_end,

	output reg         valid,
	output reg  [7:0]  profile_idc,
	output reg  [7:0]  level_idc,
	output reg  [15:0] width,
	output reg  [15:0] height,
	// Extras for slice header / MB grid (3.3d)
	output reg  [4:0]  log2_max_frame_num, // 4..16
	output reg  [2:0]  poc_type,           // 0..2
	output reg  [7:0]  mb_width,           // pic_width_in_mbs
	output reg  [7:0]  mb_height,          // frame height in MBs
	output reg         busy
);

	localparam int MAXB = 48;

	reg [7:0] mem [0:MAXB-1];
	reg [5:0] len;

	reg [5:0] bbyte;
	reg [2:0] bpos;

	wire [7:0] cur  = mem[bbyte];
	// H.264 bitstream is MSB-first within each byte. bpos counts 7→0.
	wire       bitv = cur[bpos];
	wire       oob  = (bbyte >= len);

	reg [4:0] st;
	reg [4:0] cont;     // return from ST_GETBITS
	reg [4:0] ue_cont;  // return from complete ue()
	reg [5:0] zcnt;
	reg [4:0] nleft;
	reg [15:0] acc;
	reg [15:0] ue_val;
	reg [15:0] w_mbs, h_map;
	reg        frame_mbs_only;
	reg        crop_flag;
	reg [15:0] cl, cr, ct, cb;
	reg [7:0]  prof;
	reg [4:0]  log2_fn;
	reg [2:0]  poc_t;

	localparam [4:0]
		ST_IDLE    = 5'd0,
		ST_GETBITS = 5'd1,
		ST_UE_Z    = 5'd2,
		ST_UE_V    = 5'd3,
		ST_PROF    = 5'd4,
		ST_CONS    = 5'd5,
		ST_LEVEL   = 5'd6,
		ST_SPSID   = 5'd7,
		ST_LOG2    = 5'd8,
		ST_POC     = 5'd9,
		ST_POC0    = 5'd10,
		ST_REFS    = 5'd11,
		ST_GAPS    = 5'd12,
		ST_W       = 5'd13,
		ST_H       = 5'd14,
		ST_FMO     = 5'd15,
		ST_MBAFF   = 5'd16,
		ST_D8      = 5'd17,
		ST_CROP    = 5'd18,
		ST_CL      = 5'd19,
		ST_CR      = 5'd20,
		ST_CT      = 5'd21,
		ST_CB      = 5'd22,
		ST_FINISH  = 5'd23,
		ST_FAIL    = 5'd24;

	// capture buffer (independent of parse FSM)
	always @(posedge clk) begin
		if (reset || cap_clear)
			len <= 0;
		else if (cap_en && len < MAXB[5:0]) begin
			mem[len] <= cap_data;
			len <= len + 1'd1;
		end
	end

	always @(posedge clk) begin
		if (reset) begin
			st <= ST_IDLE;
			valid <= 0;
			busy <= 0;
			profile_idc <= 0;
			level_idc <= 0;
			width <= 0;
			height <= 0;
			log2_max_frame_num <= 5'd4;
			poc_type <= 0;
			mb_width <= 0;
			mb_height <= 0;
			log2_fn <= 5'd4;
			poc_t <= 0;
			bbyte <= 0;
			bpos <= 3'd7;
			zcnt <= 0;
			nleft <= 0;
			acc <= 0;
			ue_val <= 0;
			w_mbs <= 0;
			h_map <= 0;
			frame_mbs_only <= 1;
			crop_flag <= 0;
			cl <= 0; cr <= 0; ct <= 0; cb <= 0;
			prof <= 0;
			cont <= ST_IDLE;
			ue_cont <= ST_IDLE;
		end else begin
			case (st)
			ST_IDLE: begin
				busy <= 0;
				if (cap_end && len >= 6'd4) begin
					busy  <= 1'b1;
					bbyte <= 0;
					bpos  <= 3'd7;
					nleft <= 5'd8;
					acc   <= 0;
					cont  <= ST_PROF;
					st    <= ST_GETBITS;
				end
			end

			ST_GETBITS: begin
				if (oob) begin
					st <= ST_FAIL;
				end else begin
					acc <= {acc[14:0], bitv};
					if (bpos == 3'd0) begin
						bpos  <= 3'd7;
						bbyte <= bbyte + 1'd1;
					end else begin
						bpos <= bpos - 1'd1;
					end
					if (nleft == 5'd1)
						st <= cont;
					else
						nleft <= nleft - 1'd1;
				end
			end

			ST_UE_Z: begin
				if (oob) begin
					st <= ST_FAIL;
				end else if (bitv == 1'b0) begin
					if (zcnt >= 6'd20) begin
						st <= ST_FAIL;
					end else begin
						zcnt <= zcnt + 1'd1;
						if (bpos == 3'd0) begin
							bpos  <= 3'd7;
							bbyte <= bbyte + 1'd1;
						end else begin
							bpos <= bpos - 1'd1;
						end
					end
				end else begin
					// leading 1
					if (bpos == 3'd0) begin
						bpos  <= 3'd7;
						bbyte <= bbyte + 1'd1;
					end else begin
						bpos <= bpos - 1'd1;
					end
					if (zcnt == 0) begin
						ue_val <= 0;
						st <= ue_cont;
					end else begin
						nleft <= zcnt[4:0];
						acc   <= 0;
						cont  <= ST_UE_V;
						st    <= ST_GETBITS;
					end
				end
			end

			ST_UE_V: begin
				ue_val <= ((16'd1 << zcnt) - 16'd1) + acc;
				st <= ue_cont;
			end

			ST_PROF: begin
				prof <= acc[7:0];
				profile_idc <= acc[7:0];
				nleft <= 5'd8;
				acc <= 0;
				cont <= ST_CONS;
				st <= ST_GETBITS;
			end
			ST_CONS: begin
				nleft <= 5'd8;
				acc <= 0;
				cont <= ST_LEVEL;
				st <= ST_GETBITS;
			end
			ST_LEVEL: begin
				level_idc <= acc[7:0];
				zcnt <= 0;
				ue_cont <= ST_SPSID;
				st <= ST_UE_Z;
			end
			ST_SPSID: begin
				if (prof == 8'd100 || prof == 8'd110 || prof == 8'd122 ||
				    prof == 8'd244 || prof == 8'd44  || prof == 8'd83  ||
				    prof == 8'd86  || prof == 8'd118 || prof == 8'd128) begin
					st <= ST_FAIL;
				end else begin
					zcnt <= 0;
					ue_cont <= ST_LOG2;
					st <= ST_UE_Z;
				end
			end
			ST_LOG2: begin
				// ue_val = log2_max_frame_num_minus4
				log2_fn <= ue_val[4:0] + 5'd4;
				zcnt <= 0;
				ue_cont <= ST_POC;
				st <= ST_UE_Z;
			end
			ST_POC: begin
				poc_t <= ue_val[2:0];
				if (ue_val == 16'd0) begin
					zcnt <= 0;
					ue_cont <= ST_POC0;
					st <= ST_UE_Z;
				end else if (ue_val == 16'd1) begin
					st <= ST_FAIL;
				end else begin
					zcnt <= 0;
					ue_cont <= ST_REFS;
					st <= ST_UE_Z;
				end
			end
			ST_POC0: begin
				zcnt <= 0;
				ue_cont <= ST_REFS;
				st <= ST_UE_Z;
			end
			ST_REFS: begin
				nleft <= 5'd1;
				acc <= 0;
				cont <= ST_GAPS;
				st <= ST_GETBITS;
			end
			ST_GAPS: begin
				zcnt <= 0;
				ue_cont <= ST_W;
				st <= ST_UE_Z;
			end
			ST_W: begin
				w_mbs <= ue_val + 16'd1;
				zcnt <= 0;
				ue_cont <= ST_H;
				st <= ST_UE_Z;
			end
			ST_H: begin
				h_map <= ue_val + 16'd1;
				nleft <= 5'd1;
				acc <= 0;
				cont <= ST_FMO;
				st <= ST_GETBITS;
			end
			ST_FMO: begin
				frame_mbs_only <= acc[0];
				if (acc[0] == 1'b0) begin
					nleft <= 5'd1;
					acc <= 0;
					cont <= ST_MBAFF;
					st <= ST_GETBITS;
				end else begin
					nleft <= 5'd1;
					acc <= 0;
					cont <= ST_D8;
					st <= ST_GETBITS;
				end
			end
			ST_MBAFF: begin
				nleft <= 5'd1;
				acc <= 0;
				cont <= ST_D8;
				st <= ST_GETBITS;
			end
			ST_D8: begin
				nleft <= 5'd1;
				acc <= 0;
				cont <= ST_CROP;
				st <= ST_GETBITS;
			end
			ST_CROP: begin
				crop_flag <= acc[0];
				if (acc[0]) begin
					zcnt <= 0;
					ue_cont <= ST_CL;
					st <= ST_UE_Z;
				end else begin
					st <= ST_FINISH;
				end
			end
			ST_CL: begin
				cl <= ue_val;
				zcnt <= 0;
				ue_cont <= ST_CR;
				st <= ST_UE_Z;
			end
			ST_CR: begin
				cr <= ue_val;
				zcnt <= 0;
				ue_cont <= ST_CT;
				st <= ST_UE_Z;
			end
			ST_CT: begin
				ct <= ue_val;
				zcnt <= 0;
				ue_cont <= ST_CB;
				st <= ST_UE_Z;
			end
			ST_CB: begin
				cb <= ue_val;
				st <= ST_FINISH;
			end

			ST_FINISH: begin
				busy <= 0;
				st <= ST_IDLE;
				log2_max_frame_num <= log2_fn;
				poc_type <= poc_t;
				mb_width <= w_mbs[7:0];
				mb_height <= frame_mbs_only ? h_map[7:0] : {h_map[6:0], 1'b0};
				if (crop_flag) begin
					if ((w_mbs * 16'd16) > ((cl + cr) << 1) &&
					    (h_map * 16'd16 * (frame_mbs_only ? 16'd1 : 16'd2)) > ((ct + cb) << 1)) begin
						width  <= (w_mbs * 16'd16) - ((cl + cr) << 1);
						height <= (h_map * 16'd16 * (frame_mbs_only ? 16'd1 : 16'd2)) - ((ct + cb) << 1);
						valid  <= 1'b1;
					end
				end else begin
					width  <= w_mbs * 16'd16;
					height <= h_map * 16'd16 * (frame_mbs_only ? 16'd1 : 16'd2);
					valid  <= 1'b1;
				end
			end

			ST_FAIL: begin
				busy <= 0;
				st <= ST_IDLE;
			end

			default: st <= ST_IDLE;
			endcase
		end
	end

endmodule
