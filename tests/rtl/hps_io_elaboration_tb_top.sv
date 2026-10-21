// Elaboration top for hps_io under the lint BUILD_DATE define.
// Connects the same ports Plex.sv uses so --lint-only sees that HPS scope.
`timescale 1ns / 1ps

module hps_io_elaboration_tb_top;
	localparam CONF_STR = {
		"Plex;;",
		"V,v", `BUILD_DATE
	};

	logic        clk_sys;
	wire [45:0]  HPS_BUS;
	wire         forced_scandoubler;
	wire [1:0]   buttons;
	wire [127:0] status;
	wire [127:0] status_in;
	logic        status_set;
	wire [10:0]  ps2_key;
	wire [31:0]  joystick_0;
	wire         ioctl_download;
	wire         ioctl_wr;
	wire [26:0]  ioctl_addr;
	wire [7:0]   ioctl_dout;
	wire [15:0]  ioctl_index;
	logic        ioctl_wait;

	hps_io #(.CONF_STR(CONF_STR)) dut (
		.clk_sys(clk_sys),
		.HPS_BUS(HPS_BUS),
		.EXT_BUS(),
		.gamma_bus(),
		.forced_scandoubler(forced_scandoubler),
		.buttons(buttons),
		.status(status),
		.status_in(status_in),
		.status_set(status_set),
		.status_menumask(16'd0),
		.ps2_key(ps2_key),
		.joystick_0(joystick_0),
		.ioctl_download(ioctl_download),
		.ioctl_index(ioctl_index),
		.ioctl_wr(ioctl_wr),
		.ioctl_addr(ioctl_addr),
		.ioctl_dout(ioctl_dout),
		.ioctl_wait(ioctl_wait)
	);
endmodule
