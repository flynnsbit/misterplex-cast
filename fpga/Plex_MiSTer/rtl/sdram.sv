//============================================================================
// MiSTerPlex SDRAM controller.
//
// Reworked 2026-07 from the known-good MemTest_MiSTer controller command
// schedule after MemTest_MiSTer passed on the project DE10-Nano at 142 MHz.
//
// Original reference:
//   https://github.com/MiSTer-devel/MemTest_MiSTer/blob/86f89561b325d329ab96dfa6097d895e79ded36a/rtl/sdram.v
//   Copyright (c) MiSTer-devel / Sorgelig, GPL-2.0-or-later as distributed in
//   MemTest_MiSTer.
//
// This adaptation preserves the MiSTerPlex random single-word client interface
// while adopting the conservative bus timing shape proven by MemTest: CL3-capable
// mode setup, >=tRCD spacing between ACTIVE and READ/WRITE, explicit
// auto-precharge recovery before ready is reasserted, and fully initialised
// reset/ready state.
//============================================================================

module sdram
#(
	parameter int unsigned SDRAM_CLK_HZ = 100_000_000
)(
	input             init,
	input             clk,

	inout  reg [15:0] SDRAM_DQ,
	output reg [12:0] SDRAM_A,
	output            SDRAM_DQML,
	output            SDRAM_DQMH,
	output reg  [1:0] SDRAM_BA,
	output            SDRAM_nCS,
	output            SDRAM_nWE,
	output            SDRAM_nRAS,
	output            SDRAM_nCAS,
	output            SDRAM_CKE,
	output            SDRAM_CLK,
	input             SDRAM_EN,

	input             sel,
	input      [26:1] addr,
	output reg [15:0] dout,
	input      [15:0] din,
	input             wr,
	input       [1:0] bs,
	input             rd,
	output reg        ready,
	input             refresh,

	input             cpsel,
	input      [26:1] cpaddr,
	input      [15:0] cpdin,
	output reg        cprd,
	input             cpreq,
	output reg        cpbusy
);

assign SDRAM_nCS  = chip;
assign SDRAM_nRAS = command[2];
assign SDRAM_nCAS = command[1];
assign SDRAM_nWE  = command[0];
assign SDRAM_CKE  = 1'b1;
assign {SDRAM_DQMH, SDRAM_DQML} = SDRAM_A[12:11];

localparam int BURST_LENGTH        = 4;
localparam [2:0] BURST_CODE        = 3'b010; // burst length 4, matching MemTest_MiSTer
localparam ACCESS_TYPE             = 1'b0;
`ifdef SDRAM_CL3
localparam [2:0] CAS_LATENCY       = 3'd3;
`else
localparam [2:0] CAS_LATENCY       = 3'd2;
`endif
localparam [1:0] OP_MODE           = 2'b00;
localparam NO_WRITE_BURST          = 1'b1; // random single-word client writes
localparam [12:0] MODE             = {3'b000, NO_WRITE_BURST, OP_MODE, CAS_LATENCY, ACCESS_TYPE, BURST_CODE};

localparam longint unsigned STARTUP_CYCLES_CALC = ((longint'(SDRAM_CLK_HZ) * 121) + 999_999) / 1_000_000;
localparam longint unsigned REFRESH_CYCLES_CALC = ((longint'(SDRAM_CLK_HZ) * 64_000) / (8192 * 1_000_000)) - 1;
localparam longint unsigned T_RCD_CALC          = ((longint'(SDRAM_CLK_HZ) * 20) + 999_999_999) / 1_000_000_000;
localparam longint unsigned T_RP_CALC           = ((longint'(SDRAM_CLK_HZ) * 20) + 999_999_999) / 1_000_000_000;
localparam longint unsigned T_RFC_CALC          = ((longint'(SDRAM_CLK_HZ) * 66) + 999_999_999) / 1_000_000_000;
localparam int unsigned SDRAM_STARTUP_CYCLES    = STARTUP_CYCLES_CALC[31:0];
localparam int unsigned CYCLES_PER_REFRESH      = REFRESH_CYCLES_CALC[31:0];
localparam int unsigned T_RCD_CYCLES            = (T_RCD_CALC < 2) ? 2 : T_RCD_CALC[31:0];
localparam int unsigned T_RP_CYCLES             = (T_RP_CALC  < 2) ? 2 : T_RP_CALC[31:0];
localparam int unsigned T_RFC_CYCLES            = (T_RFC_CALC < 8) ? 8 : T_RFC_CALC[31:0];
localparam int unsigned POST_CAS_CYCLES         = BURST_LENGTH + T_RP_CYCLES;

localparam [2:0] CMD_NOP             = 3'b111;
localparam [2:0] CMD_ACTIVE          = 3'b011;
localparam [2:0] CMD_READ            = 3'b101;
localparam [2:0] CMD_WRITE           = 3'b100;
localparam [2:0] CMD_PRECHARGE       = 3'b010;
localparam [2:0] CMD_AUTO_REFRESH    = 3'b001;
localparam [2:0] CMD_LOAD_MODE       = 3'b000;

localparam [4:0] ST_INIT_WAIT        = 5'd0;
localparam [4:0] ST_INIT_PRECHARGE   = 5'd1;
localparam [4:0] ST_INIT_AR1         = 5'd2;
localparam [4:0] ST_INIT_AR2         = 5'd3;
localparam [4:0] ST_INIT_MRS         = 5'd4;
localparam [4:0] ST_WAIT             = 5'd5;
localparam [4:0] ST_IDLE             = 5'd6;
localparam [4:0] ST_REFRESH          = 5'd7;
localparam [4:0] ST_ACTIVE_WAIT      = 5'd8;
localparam [4:0] ST_CAS              = 5'd9;
localparam [4:0] ST_READ_WAIT        = 5'd10;
localparam [4:0] ST_POST_CAS         = 5'd11;

reg [4:0]  state;
reg [4:0]  wait_return;
reg [31:0] wait_count;
reg [31:0] startup_count;
reg [31:0] refresh_count;
reg        refresh_old;
reg [12:0] cas_addr;
reg [15:0] saved_data;
reg        saved_wr;
reg        chip;
reg [2:0]  command;
reg [15:0] dq_sample;

wire request = sel & (rd | wr);

always @(posedge clk) begin
	SDRAM_DQ <= 16'hZZZZ;
	command  <= CMD_NOP;
	cprd     <= 1'b0;
	dq_sample <= SDRAM_DQ;

	if (init) begin
		state         <= ST_INIT_WAIT;
		wait_return   <= ST_IDLE;
		wait_count    <= 32'd0;
		startup_count <= SDRAM_STARTUP_CYCLES;
		refresh_count <= 32'd0;
		refresh_old   <= refresh;
		ready         <= 1'b0;
		cpbusy        <= 1'b0;
		cprd          <= 1'b0;
		dout          <= 16'd0;
		SDRAM_A       <= 13'd0;
		SDRAM_BA      <= 2'd0;
		chip          <= 1'b1;
		cas_addr      <= 13'd0;
		saved_data    <= 16'd0;
		saved_wr      <= 1'b0;
	end else if (!SDRAM_EN) begin
		state         <= ST_IDLE;
		ready         <= 1'b1;
		cpbusy        <= 1'b0;
		cprd          <= 1'b0;
		dout          <= 16'd0;
		SDRAM_A       <= 13'd0;
		SDRAM_BA      <= 2'd0;
		chip          <= 1'b1;
		command       <= CMD_NOP;
	end else begin
		if (refresh_count != 32'hFFFF_FFFF)
			refresh_count <= refresh_count + 32'd1;

		case (state)
			ST_INIT_WAIT: begin
				ready <= 1'b0;
				chip  <= 1'b1;
				if (startup_count != 0) begin
					startup_count <= startup_count - 32'd1;
				end else begin
					state <= ST_INIT_PRECHARGE;
				end
			end

			ST_INIT_PRECHARGE: begin
				chip        <= 1'b0;
				SDRAM_A     <= 13'd0;
				SDRAM_A[10] <= 1'b1;
				SDRAM_BA    <= 2'b00;
				command     <= CMD_PRECHARGE;
				wait_count  <= T_RP_CYCLES - 1;
				wait_return <= ST_INIT_AR1;
				state       <= ST_WAIT;
			end

			ST_INIT_AR1: begin
				chip        <= 1'b0;
				command     <= CMD_AUTO_REFRESH;
				wait_count  <= T_RFC_CYCLES - 1;
				wait_return <= ST_INIT_AR2;
				state       <= ST_WAIT;
			end

			ST_INIT_AR2: begin
				chip        <= 1'b0;
				command     <= CMD_AUTO_REFRESH;
				wait_count  <= T_RFC_CYCLES - 1;
				wait_return <= ST_INIT_MRS;
				state       <= ST_WAIT;
			end

			ST_INIT_MRS: begin
				chip        <= 1'b0;
				SDRAM_BA    <= 2'b00;
				SDRAM_A     <= MODE;
				command     <= CMD_LOAD_MODE;
				wait_count  <= 32'd2;
				wait_return <= ST_IDLE;
				refresh_count <= 32'd0;
				state       <= ST_WAIT;
			end

			ST_WAIT: begin
				if (wait_count != 0) begin
					wait_count <= wait_count - 32'd1;
				end else begin
					state <= wait_return;
				end
			end

			ST_IDLE: begin
				ready  <= 1'b1;
				cpbusy <= 1'b0;
				chip   <= 1'b1;
				if ((refresh ^ refresh_old) || (refresh_count >= CYCLES_PER_REFRESH)) begin
					ready         <= 1'b0;
					refresh_old   <= refresh;
					refresh_count <= 32'd0;
					state         <= ST_REFRESH;
				end else if (request) begin
					ready      <= 1'b0;
					{cas_addr[12:9], SDRAM_BA, SDRAM_A, cas_addr[8:0]} <= {wr ? ~bs : 2'b00, 1'b1, addr[25:1]};
					chip       <= addr[26];
					saved_data <= din;
					saved_wr   <= wr;
					command    <= CMD_ACTIVE;
					wait_count <= T_RCD_CYCLES - 1;
					state      <= ST_ACTIVE_WAIT;
				end else if (~refresh_old) begin
					refresh_old <= refresh;
				end
			end

			ST_REFRESH: begin
				chip        <= 1'b0;
				command     <= CMD_AUTO_REFRESH;
				wait_count  <= T_RFC_CYCLES - 1;
				wait_return <= ST_IDLE;
				state       <= ST_WAIT;
			end

			ST_ACTIVE_WAIT: begin
				if (wait_count != 0) begin
					wait_count <= wait_count - 32'd1;
				end else begin
					state <= ST_CAS;
				end
			end

			ST_CAS: begin
				SDRAM_A <= cas_addr;
				if (saved_wr) begin
					command     <= CMD_WRITE;
					SDRAM_DQ    <= saved_data;
					wait_count  <= POST_CAS_CYCLES - 1;
					wait_return <= ST_IDLE;
					state       <= ST_POST_CAS;
				end else begin
					command     <= CMD_READ;
					wait_count  <= {29'd0, CAS_LATENCY} + 32'd1;
					state       <= ST_READ_WAIT;
				end
			end

			ST_READ_WAIT: begin
				if (wait_count != 0) begin
					wait_count <= wait_count - 32'd1;
				end else begin
					dout       <= dq_sample;
					wait_count <= POST_CAS_CYCLES - 1;
					state      <= ST_POST_CAS;
				end
			end

			ST_POST_CAS: begin
				if (wait_count != 0) begin
					wait_count <= wait_count - 32'd1;
				end else begin
					state <= ST_IDLE;
				end
			end

			default: state <= ST_INIT_WAIT;
		endcase
	end
end

altddio_out
#(
	.extend_oe_disable("OFF"),
	.intended_device_family("Cyclone V"),
	.invert_output("OFF"),
	.lpm_hint("UNUSED"),
	.lpm_type("altddio_out"),
	.oe_reg("UNREGISTERED"),
	.power_up_high("OFF"),
	.width(1)
)
sdramclk_ddr
(
	.datain_h(1'b0),
	.datain_l(1'b1),
	.outclock(clk),
	.dataout(SDRAM_CLK),
	.aclr(1'b0),
	.aset(1'b0),
	.oe(1'b1),
	.outclocken(1'b1),
	.sclr(1'b0),
	.sset(1'b0)
);

wire _unused = &{cpsel, cpaddr, cpdin, cpreq};

endmodule
