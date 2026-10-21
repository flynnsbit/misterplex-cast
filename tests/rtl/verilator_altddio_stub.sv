module altddio_out #(
	parameter extend_oe_disable = "OFF",
	parameter intended_device_family = "Cyclone V",
	parameter invert_output = "OFF",
	parameter lpm_hint = "UNUSED",
	parameter lpm_type = "altddio_out",
	parameter oe_reg = "UNREGISTERED",
	parameter power_up_high = "OFF",
	parameter width = 1
)(
	input  wire [width-1:0] datain_h,
	input  wire [width-1:0] datain_l,
	input  wire outclock,
	output wire [width-1:0] dataout,
	input  wire aclr,
	input  wire aset,
	input  wire oe,
	input  wire outclocken,
	input  wire sclr,
	input  wire sset
);
	wire _unused = &{aclr, aset, oe, outclocken, sclr, sset};
	assign dataout = outclock ? datain_h : datain_l;
endmodule
