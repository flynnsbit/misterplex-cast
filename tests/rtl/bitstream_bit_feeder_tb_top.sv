// TB top: bitstream_bit_feeder (EPB strip + bit window + backpressure).
module bitstream_bit_feeder_tb_top (
	input  wire        clk,
	input  wire        reset,
	input  wire        clear,
	input  wire        in_valid,
	input  wire [7:0]  in_byte,
	input  wire        in_last,
	output wire        in_ready,
	output wire        bit_valid,
	output wire        bit_value,
	input  wire        bit_ready,
	output wire        nal_bit_last,
	output wire [15:0] epb_removed,
	output wire [15:0] rbsp_bytes,
	output wire [31:0] bits_out,
	output wire        byte_q_full,
	output wire        byte_q_empty
);
	bitstream_bit_feeder #(.BYTE_Q_DEPTH(8)) dut (
		.clk(clk),
		.reset(reset),
		.clear(clear),
		.in_valid(in_valid),
		.in_byte(in_byte),
		.in_last(in_last),
		.in_ready(in_ready),
		.bit_valid(bit_valid),
		.bit_value(bit_value),
		.bit_ready(bit_ready),
		.nal_bit_last(nal_bit_last),
		.epb_removed(epb_removed),
		.rbsp_bytes(rbsp_bytes),
		.bits_out(bits_out),
		.byte_q_full(byte_q_full),
		.byte_q_empty(byte_q_empty)
	);
endmodule
