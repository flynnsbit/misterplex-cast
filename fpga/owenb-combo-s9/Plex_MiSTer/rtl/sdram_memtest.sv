//============================================================================
// MiSTerPlex SDRAM bring-up memory test.
// Exercises the single MiSTer SDRAM stick through the standard sdram.sv port.
//============================================================================

module sdram_memtest #(
	parameter int REFRESH_CYCLES = 780
)(
	input  wire        clk,
	input  wire        reset,

	input  wire [15:0] sdram_dout,
	input  wire        sdram_ready,
	output reg         sdram_sel,
	output reg  [26:1] sdram_addr,
	output reg  [15:0] sdram_din,
	output reg         sdram_wr,
	output reg         sdram_rd,
	output wire  [1:0] sdram_bs,
	output reg         sdram_refresh,

	output reg   [3:0] state_code,
	output reg   [3:0] size_code,
	output reg  [15:0] error_count,
	output reg  [15:0] read_sample,
	output reg         first_fail_valid,
	output reg  [25:0] first_fail_addr,
	output reg  [15:0] first_fail_expect,
	output reg         done,
	output reg         pass
);

	localparam [3:0] SIZE_UNKNOWN = 4'd0;
	localparam [3:0] SIZE_16_MB   = 4'd2;
	localparam [3:0] SIZE_32_MB   = 4'd3;
	localparam [3:0] SIZE_64_MB   = 4'd4;
	localparam [3:0] SIZE_128_MB  = 4'd5;

	localparam [26:0] WORDS_16_MB  = 27'd8_388_608;
	localparam [26:0] WORDS_32_MB  = 27'd16_777_216;
	localparam [26:0] WORDS_64_MB  = 27'd33_554_432;
	localparam [26:0] WORDS_128_MB = 27'd67_108_864;
	localparam [25:0] ADDR_16_MB  = WORDS_16_MB;
	localparam [25:0] ADDR_32_MB  = WORDS_32_MB;
	localparam [25:0] ADDR_64_MB  = WORDS_64_MB[25:0];
	localparam [15:0] REFRESH_LIMIT = REFRESH_CYCLES;

	localparam [15:0] PAT0  = 16'h1357;
	localparam [15:0] PAT16 = 16'h5AA5;
	localparam [15:0] PAT32 = 16'hC33C;
	localparam [15:0] PAT64 = 16'h9E81;

	localparam [5:0] ST_RESET        = 6'd0;
	localparam [5:0] ST_DET_W0       = 6'd1;
	localparam [5:0] ST_DET_W16      = 6'd2;
	localparam [5:0] ST_DET_W32      = 6'd3;
	localparam [5:0] ST_DET_R0       = 6'd4;
	localparam [5:0] ST_DET_R16      = 6'd5;
	localparam [5:0] ST_DET_PICK     = 6'd6;
	localparam [5:0] ST_W1_WRITE     = 6'd7;
	localparam [5:0] ST_W1_READ      = 6'd8;
	localparam [5:0] ST_W0_WRITE     = 6'd9;
	localparam [5:0] ST_W0_READ      = 6'd10;
	localparam [5:0] ST_ADDR_WRITE   = 6'd11;
	localparam [5:0] ST_ADDR_READ    = 6'd12;
	localparam [5:0] ST_DONE         = 6'd13;
	localparam [5:0] ST_OP_ISSUE     = 6'd14;
	localparam [5:0] ST_OP_DROP      = 6'd15;
	localparam [5:0] ST_OP_WAIT      = 6'd16;
	localparam [5:0] ST_DET_W64      = 6'd17;
	localparam [5:0] ST_DET_R32      = 6'd18;
	localparam [5:0] ST_DET_R64      = 6'd19;
	localparam [5:0] ST_OP_CAPTURE   = 6'd20;

	assign sdram_bs = 2'b11;

	reg [5:0]  state;
	reg [5:0]  op_return;
	reg [26:0] ptr;
	reg [26:0] limit_words;
	reg [25:0] op_addr;
	reg [15:0] op_din;
	reg [15:0] op_expect;
	reg        op_write;
	reg        op_check;
	reg [15:0] det_r0;
	reg [15:0] det_r16;
	reg [15:0] det_r32;
	reg [15:0] last_read;
	reg [15:0] refresh_ctr;

	function automatic [15:0] walk_one(input [25:0] a);
		walk_one = (16'h0001 << a[3:0]);
	endfunction

	function automatic [15:0] addr_pattern(input [25:0] a);
		addr_pattern = a[15:0] ^ {6'd0, a[25:16]} ^ 16'hA5A5;
	endfunction

	task automatic start_write(input [25:0] a, input [15:0] d, input [5:0] ret);
		begin
			op_addr   <= a;
			op_din    <= d;
			op_expect <= 16'd0;
			op_write  <= 1'b1;
			op_check  <= 1'b0;
			op_return <= ret;
			state     <= ST_OP_ISSUE;
		end
	endtask

	task automatic start_read(input [25:0] a, input [15:0] exp, input check, input [5:0] ret);
		begin
			op_addr   <= a;
			op_din    <= 16'd0;
			op_expect <= exp;
			op_write  <= 1'b0;
			op_check  <= check;
			op_return <= ret;
			state     <= ST_OP_ISSUE;
		end
	endtask

	always @(posedge clk) begin
		if (reset) begin
			refresh_ctr    <= 16'd0;
			sdram_refresh <= 1'b0;
		end else begin
			if (refresh_ctr == REFRESH_LIMIT) begin
				refresh_ctr    <= 16'd0;
				sdram_refresh <= ~sdram_refresh;
			end else begin
				refresh_ctr <= refresh_ctr + 16'd1;
			end
		end
	end

	always @(posedge clk) begin
		if (reset) begin
			state        <= ST_RESET;
			state_code   <= 4'd1;
			size_code    <= SIZE_UNKNOWN;
			error_count  <= 16'd0;
			read_sample  <= 16'd0;
			first_fail_valid  <= 1'b0;
			first_fail_addr   <= 26'd0;
			first_fail_expect <= 16'd0;
			done         <= 1'b0;
			pass         <= 1'b0;
			sdram_sel    <= 1'b0;
			sdram_wr     <= 1'b0;
			sdram_rd     <= 1'b0;
			sdram_addr   <= 26'd0;
			sdram_din    <= 16'd0;
			ptr          <= 27'd0;
			limit_words  <= WORDS_32_MB;
			det_r0       <= 16'd0;
			det_r16      <= 16'd0;
			det_r32      <= 16'd0;
			last_read    <= 16'd0;
			op_addr      <= 26'd0;
			op_din       <= 16'd0;
			op_expect    <= 16'd0;
			op_write     <= 1'b0;
			op_check     <= 1'b0;
			op_return    <= ST_RESET;
		end else begin
			sdram_sel <= 1'b0;
			sdram_wr  <= 1'b0;
			sdram_rd  <= 1'b0;

			case (state)
				ST_RESET: begin
					state_code <= 4'd1;
					if (sdram_ready) begin
						state_code <= 4'd2;
						start_write(26'd0, PAT0, ST_DET_W16);
					end
				end

				ST_DET_W16: start_write(ADDR_16_MB, PAT16, ST_DET_W32);
				ST_DET_W32: start_write(ADDR_32_MB, PAT32, ST_DET_W64);
				ST_DET_W64: start_write(ADDR_64_MB, PAT64, ST_DET_R0);
				ST_DET_R0:  start_read(26'd0, 16'd0, 1'b0, ST_DET_R16);
				ST_DET_R16: begin
					det_r0 <= last_read;
					start_read(ADDR_16_MB, 16'd0, 1'b0, ST_DET_R32);
				end
				ST_DET_R32: begin
					det_r16 <= last_read;
					start_read(ADDR_32_MB, 16'd0, 1'b0, ST_DET_R64);
				end
				ST_DET_R64: begin
					det_r32 <= last_read;
					start_read(ADDR_64_MB, 16'd0, 1'b0, ST_DET_PICK);
				end

				ST_DET_PICK: begin
					if (det_r0 == PAT0 && det_r16 == PAT16 && det_r32 == PAT32 && last_read == PAT64) begin
						size_code   <= SIZE_128_MB;
						limit_words <= WORDS_128_MB;
					end else if (det_r0 == PAT0 && det_r16 == PAT16 && det_r32 == PAT32) begin
						size_code   <= SIZE_64_MB;
						limit_words <= WORDS_64_MB;
					end else if (det_r16 == PAT16) begin
						size_code   <= SIZE_32_MB;
						limit_words <= WORDS_32_MB;
					end else if (det_r0 == PAT32 || det_r16 == PAT32 || det_r32 == PAT32) begin
						size_code   <= SIZE_16_MB;
						limit_words <= WORDS_16_MB;
					end else begin
						size_code   <= SIZE_UNKNOWN;
						limit_words <= WORDS_16_MB;
						if (!first_fail_valid) begin
							first_fail_valid <= 1'b1;
							if (det_r0 != PAT0) begin
								first_fail_addr   <= 26'd0;
								first_fail_expect <= PAT0;
								read_sample       <= det_r0;
							end else if (det_r16 != PAT16) begin
								first_fail_addr   <= ADDR_16_MB;
								first_fail_expect <= PAT16;
								read_sample       <= det_r16;
							end else if (det_r32 != PAT32) begin
								first_fail_addr   <= ADDR_32_MB;
								first_fail_expect <= PAT32;
								read_sample       <= det_r32;
							end else begin
								first_fail_addr   <= ADDR_64_MB;
								first_fail_expect <= PAT64;
								read_sample       <= last_read;
							end
						end
						if (error_count != 16'hFFFF) error_count <= error_count + 16'd1;
					end
					ptr        <= 27'd0;
					state_code <= 4'd3;
					state      <= ST_W1_WRITE;
				end

				ST_W1_WRITE: begin
					if (ptr < limit_words) begin
						start_write(ptr[25:0], walk_one(ptr[25:0]), ST_W1_WRITE);
						ptr <= ptr + 27'd1;
					end else begin
						ptr <= 27'd0;
						state <= ST_W1_READ;
					end
				end

				ST_W1_READ: begin
					if (ptr < limit_words) begin
						start_read(ptr[25:0], walk_one(ptr[25:0]), 1'b1, ST_W1_READ);
						ptr <= ptr + 27'd1;
					end else begin
						ptr <= 27'd0;
						state_code <= 4'd4;
						state <= ST_W0_WRITE;
					end
				end

				ST_W0_WRITE: begin
					if (ptr < limit_words) begin
						start_write(ptr[25:0], ~walk_one(ptr[25:0]), ST_W0_WRITE);
						ptr <= ptr + 27'd1;
					end else begin
						ptr <= 27'd0;
						state <= ST_W0_READ;
					end
				end

				ST_W0_READ: begin
					if (ptr < limit_words) begin
						start_read(ptr[25:0], ~walk_one(ptr[25:0]), 1'b1, ST_W0_READ);
						ptr <= ptr + 27'd1;
					end else begin
						ptr <= 27'd0;
						state_code <= 4'd5;
						state <= ST_ADDR_WRITE;
					end
				end

				ST_ADDR_WRITE: begin
					if (ptr < limit_words) begin
						start_write(ptr[25:0], addr_pattern(ptr[25:0]), ST_ADDR_WRITE);
						ptr <= ptr + 27'd1;
					end else begin
						ptr <= 27'd0;
						state <= ST_ADDR_READ;
					end
				end

				ST_ADDR_READ: begin
					if (ptr < limit_words) begin
						start_read(ptr[25:0], addr_pattern(ptr[25:0]), 1'b1, ST_ADDR_READ);
						ptr <= ptr + 27'd1;
					end else begin
						state <= ST_DONE;
					end
				end

				ST_DONE: begin
					done       <= 1'b1;
					pass       <= (error_count == 16'd0) && (size_code != SIZE_UNKNOWN);
					state_code <= ((error_count == 16'd0) && (size_code != SIZE_UNKNOWN)) ? 4'd6 : 4'd7;
				end

				ST_OP_ISSUE: begin
					if (sdram_ready) begin
						sdram_sel  <= 1'b1;
						sdram_addr <= op_addr;
						sdram_din  <= op_din;
						sdram_wr   <= op_write;
						sdram_rd   <= ~op_write;
						state      <= ST_OP_DROP;
					end
				end

				ST_OP_DROP: begin
					state <= ST_OP_WAIT;
				end

				ST_OP_WAIT: begin
					if (sdram_ready) begin
						last_read <= sdram_dout;
						state <= ST_OP_CAPTURE;
					end
				end

				ST_OP_CAPTURE: begin
					if (!first_fail_valid)
						read_sample <= last_read;
					if (op_check && (last_read != op_expect)) begin
						if (!first_fail_valid) begin
							first_fail_valid  <= 1'b1;
							first_fail_addr   <= op_addr;
							first_fail_expect <= op_expect;
							read_sample       <= last_read;
						end
						if (error_count != 16'hFFFF)
							error_count <= error_count + 16'd1;
					end
					state <= op_return;
				end

				default: state <= ST_RESET;
			endcase
		end
	end

endmodule
