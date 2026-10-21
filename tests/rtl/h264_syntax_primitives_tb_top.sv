module h264_syntax_primitives_tb_top #(
	parameter bit FAULT_LEAK_EPB = 1'b0,
	parameter bit FAULT_SE_SIGN = 1'b0
) (
	input  wire        clk,
	input  wire        reset,
	input  wire        clear,

	input  wire        in_valid,
	input  wire [7:0]  in_byte,
	input  wire        in_last,
	output wire        in_ready,
	output wire        out_valid,
	output wire [7:0]  out_byte,
	output wire        out_last,
	output wire [15:0] out_index,
	input  wire        out_ready,
	output wire [15:0] rbsp_len,
	output wire [15:0] epb_removed,
	output wire        filter_done,

	input  wire        eg_start,
	input  wire        eg_signed_mode,
	input  wire        eg_bit_valid,
	input  wire        eg_bit_value,
	output wire        eg_bit_ready,
	output wire        eg_busy,
	output wire        eg_done,
	output wire        eg_ok,
	output wire [31:0] eg_ue_value,
	output wire signed [31:0] eg_se_value,
	output wire [7:0]  eg_bits_consumed
);
	wire [7:0] rbsp_byte_raw;
	wire signed [31:0] se_value_raw;

	h264_rbsp_filter rbsp_filter (
		.clk(clk), .reset(reset), .clear(clear),
		.in_valid(in_valid), .in_byte(in_byte), .in_last(in_last), .in_ready(in_ready),
		.out_valid(out_valid), .out_byte(rbsp_byte_raw), .out_last(out_last),
		.out_index(out_index), .out_ready(out_ready),
		.rbsp_len(rbsp_len), .epb_removed(epb_removed), .done(filter_done)
	);

	assign out_byte = (FAULT_LEAK_EPB && epb_removed != 16'd0 && out_valid) ? 8'h03 : rbsp_byte_raw;

	h264_exp_golomb_reader eg_reader (
		.clk(clk), .reset(reset), .start(eg_start), .signed_mode(eg_signed_mode),
		.bit_valid(eg_bit_valid), .bit_value(eg_bit_value), .bit_ready(eg_bit_ready),
		.busy(eg_busy), .done(eg_done), .ok(eg_ok), .ue_value(eg_ue_value),
		.se_value(se_value_raw), .bits_consumed(eg_bits_consumed)
	);

	assign eg_se_value = (FAULT_SE_SIGN && eg_signed_mode && eg_ok) ? -se_value_raw : se_value_raw;
endmodule
