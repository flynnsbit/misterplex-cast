// Patched ddr_bus_arbiter with m1 response hold.
// This is the PROPOSED FIX for w-a3: m1_dout_ready is held high until
// the consumer issues the next m1_rd, guaranteeing the slow-domain
// consumer (clk_sys, 20 MHz) sees every beat from the fast-domain
// DDR bridge (clk_ddr, 90 MHz).

module ddr_bus_arbiter_patched (
	input  wire        clk,
	input  wire        reset,

	input  wire        m1_want,

	output wire        m0_busy,
	input  wire  [7:0] m0_burstcnt,
	input  wire [28:0] m0_addr,
	output wire [63:0] m0_dout,
	output wire        m0_dout_ready,
	input  wire        m0_rd,
	input  wire [63:0] m0_din,
	input  wire  [7:0] m0_be,
	input  wire        m0_we,

	output wire        m1_busy,
	input  wire  [7:0] m1_burstcnt,
	input  wire [28:0] m1_addr,
	output wire [63:0] m1_dout,
	output wire        m1_dout_ready,
	input  wire        m1_rd,
	input  wire [63:0] m1_din,
	input  wire  [7:0] m1_be,
	input  wire        m1_we,

	input  wire        DDRAM_BUSY,
	output wire  [7:0] DDRAM_BURSTCNT,
	output wire [28:0] DDRAM_ADDR,
	input  wire [63:0] DDRAM_DOUT,
	input  wire        DDRAM_DOUT_READY,
	output wire        DDRAM_RD,
	output wire [63:0] DDRAM_DIN,
	output wire  [7:0] DDRAM_BE,
	output wire        DDRAM_WE
);
	reg grant_m1;
	reg rsp_owner_m1;
	reg [8:0] rsp_left;

	wire rsp_active = rsp_left != 9'd0;
	wire m0_cmd = m0_rd | m0_we;
	wire m1_cmd = m1_rd | m1_we;
	wire use_m1 = grant_m1;
	wire [7:0] selected_burst = use_m1 ? m1_burstcnt : m0_burstcnt;

	assign m0_busy = DDRAM_BUSY | grant_m1 | (rsp_active & rsp_owner_m1);
	assign m1_busy = DDRAM_BUSY | !grant_m1 | (rsp_active & !rsp_owner_m1);

	assign DDRAM_BURSTCNT = use_m1 ? m1_burstcnt : m0_burstcnt;
	assign DDRAM_ADDR     = use_m1 ? m1_addr      : m0_addr;
	assign DDRAM_RD       = use_m1 ? m1_rd        : m0_rd;
	assign DDRAM_DIN      = use_m1 ? m1_din       : m0_din;
	assign DDRAM_BE       = use_m1 ? m1_be        : m0_be;
	assign DDRAM_WE       = use_m1 ? m1_we        : m0_we;

	// m0: combinational pass-through (same domain, no CDC issue)
	assign m0_dout = DDRAM_DOUT;
	assign m0_dout_ready = DDRAM_DOUT_READY & rsp_active & !rsp_owner_m1;

	// m1: HOLD response until consumer issues next m1_rd (rising edge)
	// This converts the 1-clk_ddr-cycle DDRAM_DOUT_READY pulse into a level
	// signal that the slow clk_sys consumer is guaranteed to see.
	reg        m1_dout_ready_held;
	reg [63:0] m1_dout_held;
	reg        m1_rd_d;
	wire       m1_rd_rise = m1_rd && !m1_rd_d;

	assign m1_dout_ready = m1_dout_ready_held;
	assign m1_dout       = m1_dout_held;

	always @(posedge clk) begin
		if (reset) begin
			grant_m1 <= 1'b0;
			rsp_owner_m1 <= 1'b0;
			rsp_left <= 9'd0;
			m1_dout_ready_held <= 1'b0;
			m1_dout_held <= 64'd0;
			m1_rd_d <= 1'b0;
		end else begin
			m1_rd_d <= m1_rd;

			// Clear hold on rising edge of m1_rd (= consumer starting next read)
			if (m1_rd_rise)
				m1_dout_ready_held <= 1'b0;

			// Capture and hold m1 response (last-assignment wins over clear)
			if (DDRAM_DOUT_READY && rsp_active && rsp_owner_m1) begin
				m1_dout_ready_held <= 1'b1;
				m1_dout_held <= DDRAM_DOUT;
			end

			if (DDRAM_DOUT_READY && rsp_active)
				rsp_left <= rsp_left - 9'd1;

			if (!DDRAM_BUSY && !rsp_active) begin
				if (grant_m1) begin
					if (m1_rd) begin
						rsp_owner_m1 <= 1'b1;
						rsp_left <= {1'b0, selected_burst};
						grant_m1 <= 1'b0;
					end else if (m1_we || !m1_want) begin
						grant_m1 <= 1'b0;
					end
				end else begin
					if (m0_rd) begin
						rsp_owner_m1 <= 1'b0;
						rsp_left <= {1'b0, selected_burst};
					end else if (!m0_cmd && m1_want) begin
						grant_m1 <= 1'b1;
					end
				end
			end

			if (grant_m1 && !DDRAM_BUSY && m1_cmd && !m1_rd)
				grant_m1 <= 1'b0;
		end
	end
endmodule
