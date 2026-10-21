// Phase 3.3l-2 prep: H.264 4x4 inverse-quant, IDCT residual, and pred add.
// Combinational first-block building block for the FPGA decode ladder.
// Golden fixture: tests/fixtures/p3_host_recon/mb0_luma_v1.json block 0.
//
// WIDTH: dequant/IDCT/residual are signed [28:0] (29 bits). This covers
// the full H.264 coefficient × QP space:
//   max |dequant| = 192,937,984 at coeff=±32768, QP=51, mi=2 (28 bits)
//   max |IDCT|    =  48,234,496 worst-case butterfly (26 bits)
// The 16-bit coefficient input accommodates I_16x16 DC Hadamard levels
// measured at |level|=14,573 (w-level, QP=1). Do NOT narrow below 29
// bits without re-measuring: the ±2047 spec bound for AC levels does
// not cover DC Hadamard values that flow through this same path.

module h264_dequant4x4 (
	input  wire signed [15:0] coeff [0:15],
	input  wire [5:0]        qp,
	input  wire [4:0]        max_coeff,
	output wire signed [28:0] dequant [0:15]
);
	function automatic [4:0] zigzag;
		input [4:0] k;
		begin
			case (k)
			5'd0:  zigzag = 5'd0;
			5'd1:  zigzag = 5'd1;
			5'd2:  zigzag = 5'd4;
			5'd3:  zigzag = 5'd8;
			5'd4:  zigzag = 5'd5;
			5'd5:  zigzag = 5'd2;
			5'd6:  zigzag = 5'd3;
			5'd7:  zigzag = 5'd6;
			5'd8:  zigzag = 5'd9;
			5'd9:  zigzag = 5'd12;
			5'd10: zigzag = 5'd13;
			5'd11: zigzag = 5'd10;
			5'd12: zigzag = 5'd7;
			5'd13: zigzag = 5'd11;
			5'd14: zigzag = 5'd14;
			default: zigzag = 5'd15;
			endcase
		end
	endfunction

	function automatic [4:0] norm_adjust;
		input [2:0] qmod;
		input [1:0] mi;
		begin
			case ({qmod, mi})
			5'd0:  norm_adjust = 5'd10; 5'd1:  norm_adjust = 5'd13; 5'd2:  norm_adjust = 5'd16;
			5'd4:  norm_adjust = 5'd11; 5'd5:  norm_adjust = 5'd14; 5'd6:  norm_adjust = 5'd18;
			5'd8:  norm_adjust = 5'd13; 5'd9:  norm_adjust = 5'd16; 5'd10: norm_adjust = 5'd20;
			5'd12: norm_adjust = 5'd14; 5'd13: norm_adjust = 5'd18; 5'd14: norm_adjust = 5'd23;
			5'd16: norm_adjust = 5'd16; 5'd17: norm_adjust = 5'd20; 5'd18: norm_adjust = 5'd25;
			5'd20: norm_adjust = 5'd18; 5'd21: norm_adjust = 5'd23; default: norm_adjust = 5'd29;
			endcase
		end
	endfunction

	function automatic signed [28:0] dequant_one;
		input signed [15:0] c;
		input [5:0] q;
		input [4:0] scan;
		input       skip_dc;
		reg [4:0] z;
		reg [1:0] mi;
		reg [2:0] qmod;
		reg [3:0] qdiv;
		reg signed [31:0] qmul;
		reg signed [31:0] v;
		begin
			if (skip_dc)
				z = zigzag(scan + 5'd1);
			else
				z = zigzag(scan);
			if (((z[1:0] & 2'b01) + (z[3:2] & 2'b01)) == 2'd0)
				mi = 2'd0;
			else if (((z[1:0] & 2'b01) + (z[3:2] & 2'b01)) == 2'd1)
				mi = 2'd1;
			else
				mi = 2'd2;
			qmod = q % 6;
			qdiv = q / 6;
			qmul = $signed({1'b0, norm_adjust(qmod, mi)}) * 32'sd16;
			qmul = qmul <<< (qdiv + 4'd2);
			v = ($signed(c) * qmul + 32'sd32) >>> 6;
			dequant_one = v[28:0];
		end
	endfunction

	// row-major output: dequant[zigzag(scan)] receives coeff[scan]
	assign dequant[0]  = (max_coeff > 5'd0)  ? dequant_one(coeff[0],  qp, 5'd0,  1'b0) : 29'sd0;
	assign dequant[1]  = (max_coeff > 5'd1)  ? dequant_one(coeff[1],  qp, 5'd1,  1'b0) : 29'sd0;
	assign dequant[2]  = (max_coeff > 5'd5)  ? dequant_one(coeff[5],  qp, 5'd5,  1'b0) : 29'sd0;
	assign dequant[3]  = (max_coeff > 5'd6)  ? dequant_one(coeff[6],  qp, 5'd6,  1'b0) : 29'sd0;
	assign dequant[4]  = (max_coeff > 5'd2)  ? dequant_one(coeff[2],  qp, 5'd2,  1'b0) : 29'sd0;
	assign dequant[5]  = (max_coeff > 5'd4)  ? dequant_one(coeff[4],  qp, 5'd4,  1'b0) : 29'sd0;
	assign dequant[6]  = (max_coeff > 5'd7)  ? dequant_one(coeff[7],  qp, 5'd7,  1'b0) : 29'sd0;
	assign dequant[7]  = (max_coeff > 5'd12) ? dequant_one(coeff[12], qp, 5'd12, 1'b0) : 29'sd0;
	assign dequant[8]  = (max_coeff > 5'd3)  ? dequant_one(coeff[3],  qp, 5'd3,  1'b0) : 29'sd0;
	assign dequant[9]  = (max_coeff > 5'd8)  ? dequant_one(coeff[8],  qp, 5'd8,  1'b0) : 29'sd0;
	assign dequant[10] = (max_coeff > 5'd11) ? dequant_one(coeff[11], qp, 5'd11, 1'b0) : 29'sd0;
	assign dequant[11] = (max_coeff > 5'd13) ? dequant_one(coeff[13], qp, 5'd13, 1'b0) : 29'sd0;
	assign dequant[12] = (max_coeff > 5'd9)  ? dequant_one(coeff[9],  qp, 5'd9,  1'b0) : 29'sd0;
	assign dequant[13] = (max_coeff > 5'd10) ? dequant_one(coeff[10], qp, 5'd10, 1'b0) : 29'sd0;
	assign dequant[14] = (max_coeff > 5'd14) ? dequant_one(coeff[14], qp, 5'd14, 1'b0) : 29'sd0;
	assign dequant[15] = (max_coeff > 5'd15) ? dequant_one(coeff[15], qp, 5'd15, 1'b0) : 29'sd0;
endmodule

module h264_idct4x4 (
	input  wire signed [28:0] dequant [0:15],
	output wire signed [28:0] residual [0:15]
);
	function automatic signed [28:0] sat29;
		input signed [31:0] v;
		begin
			if (v > 32'sd268435455) sat29 = 29'sd268435455;
			else if (v < -32'sd268435456) sat29 = ~29'sd268435455;
			else sat29 = v[28:0];
		end
	endfunction

	wire signed [31:0] b0  = dequant[0] + 29'sd32;
	wire signed [31:0] b1  = dequant[1];
	wire signed [31:0] b2  = dequant[2];
	wire signed [31:0] b3  = dequant[3];
	wire signed [31:0] b4  = dequant[4];
	wire signed [31:0] b5  = dequant[5];
	wire signed [31:0] b6  = dequant[6];
	wire signed [31:0] b7  = dequant[7];
	wire signed [31:0] b8  = dequant[8];
	wire signed [31:0] b9  = dequant[9];
	wire signed [31:0] b10 = dequant[10];
	wire signed [31:0] b11 = dequant[11];
	wire signed [31:0] b12 = dequant[12];
	wire signed [31:0] b13 = dequant[13];
	wire signed [31:0] b14 = dequant[14];
	wire signed [31:0] b15 = dequant[15];

	wire signed [31:0] r0_z0 = b0 + b2,    r0_z1 = b0 - b2,    r0_z2 = (b1 >>> 1) - b3,    r0_z3 = b1 + (b3 >>> 1);
	wire signed [31:0] r1_z0 = b4 + b6,    r1_z1 = b4 - b6,    r1_z2 = (b5 >>> 1) - b7,    r1_z3 = b5 + (b7 >>> 1);
	wire signed [31:0] r2_z0 = b8 + b10,   r2_z1 = b8 - b10,   r2_z2 = (b9 >>> 1) - b11,   r2_z3 = b9 + (b11 >>> 1);
	wire signed [31:0] r3_z0 = b12 + b14,  r3_z1 = b12 - b14,  r3_z2 = (b13 >>> 1) - b15,  r3_z3 = b13 + (b15 >>> 1);

	wire signed [31:0] t0  = r0_z0 + r0_z3, t1  = r0_z1 + r0_z2, t2  = r0_z1 - r0_z2, t3  = r0_z0 - r0_z3;
	wire signed [31:0] t4  = r1_z0 + r1_z3, t5  = r1_z1 + r1_z2, t6  = r1_z1 - r1_z2, t7  = r1_z0 - r1_z3;
	wire signed [31:0] t8  = r2_z0 + r2_z3, t9  = r2_z1 + r2_z2, t10 = r2_z1 - r2_z2, t11 = r2_z0 - r2_z3;
	wire signed [31:0] t12 = r3_z0 + r3_z3, t13 = r3_z1 + r3_z2, t14 = r3_z1 - r3_z2, t15 = r3_z0 - r3_z3;

	wire signed [31:0] c0_z0 = t0 + t8,   c0_z1 = t0 - t8,   c0_z2 = (t4 >>> 1) - t12,  c0_z3 = t4 + (t12 >>> 1);
	wire signed [31:0] c1_z0 = t1 + t9,   c1_z1 = t1 - t9,   c1_z2 = (t5 >>> 1) - t13,  c1_z3 = t5 + (t13 >>> 1);
	wire signed [31:0] c2_z0 = t2 + t10,  c2_z1 = t2 - t10,  c2_z2 = (t6 >>> 1) - t14,  c2_z3 = t6 + (t14 >>> 1);
	wire signed [31:0] c3_z0 = t3 + t11,  c3_z1 = t3 - t11,  c3_z2 = (t7 >>> 1) - t15,  c3_z3 = t7 + (t15 >>> 1);

	assign residual[0]  = sat29((c0_z0 + c0_z3) >>> 6);
	assign residual[4]  = sat29((c0_z1 + c0_z2) >>> 6);
	assign residual[8]  = sat29((c0_z1 - c0_z2) >>> 6);
	assign residual[12] = sat29((c0_z0 - c0_z3) >>> 6);
	assign residual[1]  = sat29((c1_z0 + c1_z3) >>> 6);
	assign residual[5]  = sat29((c1_z1 + c1_z2) >>> 6);
	assign residual[9]  = sat29((c1_z1 - c1_z2) >>> 6);
	assign residual[13] = sat29((c1_z0 - c1_z3) >>> 6);
	assign residual[2]  = sat29((c2_z0 + c2_z3) >>> 6);
	assign residual[6]  = sat29((c2_z1 + c2_z2) >>> 6);
	assign residual[10] = sat29((c2_z1 - c2_z2) >>> 6);
	assign residual[14] = sat29((c2_z0 - c2_z3) >>> 6);
	assign residual[3]  = sat29((c3_z0 + c3_z3) >>> 6);
	assign residual[7]  = sat29((c3_z1 + c3_z2) >>> 6);
	assign residual[11] = sat29((c3_z1 - c3_z2) >>> 6);
	assign residual[15] = sat29((c3_z0 - c3_z3) >>> 6);
endmodule

module h264_recon4x4 (
	input  wire [7:0]         pred [0:15],
	input  wire signed [28:0] residual [0:15],
	output wire [7:0]         recon [0:15]
);
	function automatic [7:0] clip8;
		input signed [29:0] v;
		begin
			if (v < 30'sd0) clip8 = 8'd0;
			else if (v > 30'sd255) clip8 = 8'd255;
			else clip8 = v[7:0];
		end
	endfunction

	assign recon[0]  = clip8($signed({1'b0, pred[0]})  + residual[0]);
	assign recon[1]  = clip8($signed({1'b0, pred[1]})  + residual[1]);
	assign recon[2]  = clip8($signed({1'b0, pred[2]})  + residual[2]);
	assign recon[3]  = clip8($signed({1'b0, pred[3]})  + residual[3]);
	assign recon[4]  = clip8($signed({1'b0, pred[4]})  + residual[4]);
	assign recon[5]  = clip8($signed({1'b0, pred[5]})  + residual[5]);
	assign recon[6]  = clip8($signed({1'b0, pred[6]})  + residual[6]);
	assign recon[7]  = clip8($signed({1'b0, pred[7]})  + residual[7]);
	assign recon[8]  = clip8($signed({1'b0, pred[8]})  + residual[8]);
	assign recon[9]  = clip8($signed({1'b0, pred[9]})  + residual[9]);
	assign recon[10] = clip8($signed({1'b0, pred[10]}) + residual[10]);
	assign recon[11] = clip8($signed({1'b0, pred[11]}) + residual[11]);
	assign recon[12] = clip8($signed({1'b0, pred[12]}) + residual[12]);
	assign recon[13] = clip8($signed({1'b0, pred[13]}) + residual[13]);
	assign recon[14] = clip8($signed({1'b0, pred[14]}) + residual[14]);
	assign recon[15] = clip8($signed({1'b0, pred[15]}) + residual[15]);
endmodule
