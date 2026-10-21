// Minimal SDR SDRAM read-path model for controller DQ turnaround simulation.
// It decodes the mode register CAS/burst fields, drives DQ only during read
// bursts, and otherwise leaves the bus high-Z so the top-level tri1 bus floats
// to 0xffff just like the observed hardware failure.

module sdram_read_model #(
	parameter bit DEVICE_DRIVES = 1'b1
)(
	input  wire        clk,
	input  wire        cke,
	input  wire        nCS,
	input  wire        nRAS,
	input  wire        nCAS,
	input  wire        nWE,
	input  wire [12:0] A,
	input  wire  [1:0] BA,

	output reg         dq_drive,
	output reg  [15:0] dq_value,
	output reg   [2:0] cas_latency,
	output reg   [3:0] burst_len,
	output reg  [31:0] read_count
);
	localparam [2:0] CMD_ACTIVE       = 3'b011;
	localparam [2:0] CMD_READ         = 3'b101;
	localparam [2:0] CMD_WRITE        = 3'b100;
	localparam [2:0] CMD_LOAD_MODE    = 3'b000;

	reg [2:0] read_delay;
	reg [3:0] burst_remaining;
	reg [3:0] burst_index;
	reg [31:0] pending_read_index;
	reg [12:0] active_row;
	reg [1:0]  active_bank;
	reg [12:0] last_read_col;
	reg        saw_write;

	function automatic [3:0] decode_burst(input [2:0] code);
		case (code)
			3'b000: decode_burst = 4'd1;
			3'b001: decode_burst = 4'd2;
			3'b010: decode_burst = 4'd4;
			3'b011: decode_burst = 4'd8;
			default: decode_burst = 4'd1;
		endcase
	endfunction

	function automatic [15:0] read_pattern(input [31:0] idx, input [3:0] beat);
		case (idx)
			32'd0: read_pattern = (beat == 0) ? 16'h1357 : (16'hA100 | beat);
			32'd1: read_pattern = (beat == 0) ? 16'h5AA5 : (16'hB100 | beat);
			32'd2: read_pattern = (beat == 0) ? 16'hC33C : (16'hC100 | beat);
			32'd3: read_pattern = (beat == 0) ? 16'h9E81 : (16'hD100 | beat);
			default: read_pattern = {idx[7:0], beat[3:0], idx[11:8]};
		endcase
	endfunction

	wire [2:0] cmd = {nRAS, nCAS, nWE};

	always @(posedge clk) begin
		dq_drive <= 1'b0;

		if (!cke) begin
			cas_latency      <= 3'd2;
			burst_len        <= 4'd4;
			read_delay       <= 3'd0;
			burst_remaining  <= 4'd0;
			burst_index      <= 4'd0;
			read_count       <= 32'd0;
			pending_read_index <= 32'd0;
			dq_value         <= 16'd0;
			active_row       <= 13'd0;
			active_bank      <= 2'd0;
			last_read_col    <= 13'd0;
			saw_write        <= 1'b0;
		end else begin
			if (burst_remaining != 0) begin
				if (DEVICE_DRIVES) begin
					dq_drive <= 1'b1;
					dq_value <= read_pattern(pending_read_index, burst_index);
				end
				burst_remaining <= burst_remaining - 4'd1;
				burst_index <= burst_index + 4'd1;
			end

			if (read_delay != 0) begin
				read_delay <= read_delay - 3'd1;
				if (read_delay == 2) begin
					if (DEVICE_DRIVES) begin
						dq_drive <= 1'b1;
						dq_value <= read_pattern(pending_read_index, 4'd0);
					end
					burst_remaining <= burst_len - 4'd1;
					burst_index <= 4'd1;
				end
			end

			if (!nCS) begin
				case (cmd)
					CMD_LOAD_MODE: begin
						cas_latency <= A[6:4];
						burst_len <= decode_burst(A[2:0]);
					end
					CMD_ACTIVE: begin
						active_row <= A;
						active_bank <= BA;
					end
					CMD_READ: begin
						pending_read_index <= read_count;
						read_count <= read_count + 32'd1;
						last_read_col <= A;
						read_delay <= (cas_latency == 0) ? 3'd1 : cas_latency;
					end
					CMD_WRITE: begin
						saw_write <= 1'b1;
					end
					default: begin
					end
				endcase
			end
		end
	end

	wire _unused = &{active_row, active_bank, last_read_col, saw_write};
endmodule
