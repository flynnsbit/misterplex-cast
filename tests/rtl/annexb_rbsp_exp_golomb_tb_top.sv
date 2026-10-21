// Product front-end chain (w-path interface proof for w-plxd):
//   annex-B bytes → h264_rbsp_filter (DEAD today) → bit window (STRIP_EPB=0)
//                 → h264_exp_golomb_reader (DEAD today)
// Instantiates only; does not edit h264_*.sv.
module annexb_rbsp_exp_golomb_tb_top (
	input  wire        clk,
	input  wire        reset,
	input  wire        clear,

	input  wire        in_valid,
	input  wire [7:0]  in_byte,
	input  wire        in_last,
	output wire        in_ready,

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
	output wire [15:0] epb_removed,
	output wire [15:0] rbsp_len,
	output wire        filter_done,
	output wire [15:0] feed_epb_removed,
	output wire [15:0] feed_rbsp_bytes
);
	wire        f_out_valid;
	wire [7:0]  f_out_byte;
	wire        f_out_last;
	wire        f_out_ready;
	wire        feed_in_ready;
	wire        eg_bit_ready;

	assign bit_ready_to_feed = use_eg_ready ? eg_bit_ready : raw_bit_ready;
	// rbsp_filter → feeder: ready when feeder can take
	assign f_out_ready = feed_in_ready;

	h264_rbsp_filter u_rbsp (
		.clk(clk),
		.reset(reset),
		.clear(clear),
		.in_valid(in_valid),
		.in_byte(in_byte),
		.in_last(in_last),
		.in_ready(in_ready),
		.out_valid(f_out_valid),
		.out_byte(f_out_byte),
		.out_last(f_out_last),
		.out_index(),
		.out_ready(f_out_ready),
		.rbsp_len(rbsp_len),
		.epb_removed(epb_removed),
		.done(filter_done)
	);

	bitstream_bit_feeder #(
		.BYTE_Q_DEPTH(8),
		.STRIP_EPB(1'b0)
	) u_bits (
		.clk(clk),
		.reset(reset),
		.clear(clear),
		.in_valid(f_out_valid),
		.in_byte(f_out_byte),
		.in_last(f_out_last),
		.in_ready(feed_in_ready),
		.bit_valid(bit_valid),
		.bit_value(bit_value),
		.bit_ready(bit_ready_to_feed),
		.nal_bit_last(),
		.epb_removed(feed_epb_removed),
		.rbsp_bytes(feed_rbsp_bytes),
		.bits_out(),
		.byte_q_full(),
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
