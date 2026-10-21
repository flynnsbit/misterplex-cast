// Integration TB: bitstream_bit_feeder → h264_exp_golomb_reader
// w-path owns feeder; instantiates (does not edit) h264_exp_golomb_reader.
module bitstream_to_exp_golomb_tb_top (
	input  wire        clk,
	input  wire        reset,
	input  wire        clear,

	input  wire        in_valid,
	input  wire [7:0]  in_byte,
	input  wire        in_last,
	output wire        in_ready,

	// 1 = connect exp-golomb bit_ready; 0 = raw_bit_ready drives feeder
	input  wire        use_eg_ready,
	input  wire        raw_bit_ready,

	input  wire        eg_start,
	input  wire        eg_signed_mode,
	output wire        eg_busy,
	output wire        eg_done,
	output wire        eg_ok,
	output wire [31:0] eg_ue_value,
	output wire signed [31:0] eg_se_value,
	output wire [7:0]  eg_bits_consumed,

	output wire        bit_valid,
	output wire        bit_value,
	output wire        bit_ready_to_feed,
	output wire        nal_bit_last,
	output wire [15:0] epb_removed,
	output wire [15:0] rbsp_bytes,
	output wire [31:0] bits_out,
	output wire        byte_q_full
);
	wire eg_bit_ready;

	assign bit_ready_to_feed = use_eg_ready ? eg_bit_ready : raw_bit_ready;

	bitstream_bit_feeder #(.BYTE_Q_DEPTH(8)) u_feed (
		.clk(clk),
		.reset(reset),
		.clear(clear),
		.in_valid(in_valid),
		.in_byte(in_byte),
		.in_last(in_last),
		.in_ready(in_ready),
		.bit_valid(bit_valid),
		.bit_value(bit_value),
		.bit_ready(bit_ready_to_feed),
		.nal_bit_last(nal_bit_last),
		.epb_removed(epb_removed),
		.rbsp_bytes(rbsp_bytes),
		.bits_out(bits_out),
		.byte_q_full(byte_q_full),
		.byte_q_empty()
	);

	h264_exp_golomb_reader u_eg (
		.clk(clk),
		.reset(reset),
		.start(eg_start),
		.signed_mode(eg_signed_mode),
		.bit_valid(bit_valid && use_eg_ready),
		.bit_value(bit_value),
		.bit_ready(eg_bit_ready),
		.busy(eg_busy),
		.done(eg_done),
		.ok(eg_ok),
		.ue_value(eg_ue_value),
		.se_value(eg_se_value),
		.bits_consumed(eg_bits_consumed)
	);
endmodule
