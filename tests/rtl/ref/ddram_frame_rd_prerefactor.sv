// Phase 3.1b: HPS → DDR3 bulk YUV420p → RGB565 frame_store.
//
// Physical layout (HPS /dev/mem view):
//   Bank 0: 0x30000000
//   Bank 1: phys_base + bank_stride.
//           The measured 480p contract is coded 624x480/display 618x480/
//           presented 640x480; see ddr_frame_layout_params.svh and the host
//           ddr_frame_layout.hpp guard for its RGB/YUV strides and doorbells.
//   Doorbell: 0x3007F000  (one 64-bit word — product hot path, no SPI kick)
//     [31:0]  magic 0x504C584B ("PLXK")
//     [62:32] seq   (monotonic)
//     [63]    bank  (0/1)
//   Status mailbox: 0x3007F100  (one 64-bit word, core -> HPS, no SPI)
//     [31:0]  magic 0x504C5853 ("PLXS")
//     [47:32] status[15:0]  (the live OSD menu word)
//     [63:48] seq (monotonic; lets the host reject a torn/stale read)
//   Input mailbox: 0x3007F108  (one 64-bit word, core -> HPS, no SPI)
//     [31:0]  magic 0x504C5849 ("PLXI")
//     [39:32] cmd (1=play/pause, 2=stop, 3=skip fwd, 4=skip back)
//     [47:40] cmd_seq (increments for every published command)
//     [63:48] seq (monotonic; lets the host reject a torn/stale read)
//   Reserved fixed mailbox slots (host/FPGA ABI; keep stable):
//   SDRAM bring-up mailbox: 0x3007F110  (one 64-bit word, core -> HPS, no SPI)
//     [31:0]  magic 0x504C584D ("PLXM")
//     [39:32] seq (monotonic; lets the host reject a torn/stale read)
//     [43:40] memtest state (1 init, 2 detect, 3 walk1, 4 walk0,
//                            5 address, 6 pass, 7 fail)
//     [47:44] detected size code (2=16MB, 3=32MB, 4=64MB, 5=128MB; 0 unknown)
//     [63:48] saturated error count
//   SDRAM diagnostic mailbox: 0x3007F120 (one 64-bit word, core -> HPS, no SPI)
//     [4:0]   layout version = 1
//     [20:5]  expected value at first failing address
//     [46:21] first failing 16-bit word address
//     [47]    first-fail valid
//     [63:48] read sample: first failing read value if valid, otherwise latest read
//   SDRAM frame-store mailbox: 0x3007F118 (one 64-bit word, core -> HPS, no SPI)
//     [31:0]  magic 0x504C5846 ("PLXF")
//     [39:32] seq
//     [47:40] frame-store SDRAM debug state
//     [63:48] saturated line-buffer underrun count
//   Continuous H.264 bitstream ring (HPS DDR3, independent of SDRAM stick):
//     Data ring:       0x30100000..0x3013FFFF (256 KiB)
//     HPS->FPGA CTRL:  0x30140000 ("PLXB", write_count[30:0], reset epoch)
//     FPGA->HPS READ:  0x30140008 ("PLXR", read_count[31:0])
//     FPGA->HPS ERR:   0x30140010 ("PLXE", seq, active, underrun/overrun sticky/counts)
//
// Why the mailbox exists: misterplexd used to read the OSD word back over the
// HPS<->FPGA SPI bus (UIO_GET_STATUS). That bus is a single GPO register owned
// by Main_MiSTer, whose fpga_spi() spins on the strobe/ACK handshake with no
// timeout — so an interleaved access from another process can hang Main dead
// (no F12, no OSD, no MiSTer_cmd). Publishing the word into DDR instead means
// the daemon never touches SPI at all during normal operation.
//
// Start paths:
//   A) SPI: rising status[12] + status[13] bank
//   B) Doorbell: idle poll of 0x3007F000; new seq → start
//
// swap_req is a request only; frame_store flips display bank on vsync.
//
// Tear fix: never start a new DMA while frame_store.swap_pending — the completed
// back buffer must not be overwritten before the vsync flip. Queue one held
// start (latest doorbell/SPI) and launch when pending clears.

module ddram_frame_rd_prerefactor #(
	parameter int WIDTH      = 320,
	parameter int HEIGHT     = 240,
	parameter [31:0] PHYS_BASE = 32'h3000_0000,
	parameter [31:0] DOORBELL_PHYS = 32'h3007_F000,
	parameter [31:0] MAILBOX_PHYS  = 32'h3007_F100,
	parameter [31:0] INPUT_MAILBOX_PHYS = 32'h3007_F108,
	parameter [31:0] MEMTEST_MAILBOX_PHYS = 32'h3007_F110,
	parameter [31:0] UNDERRUN_MAILBOX_PHYS = 32'h3007_F118,
	parameter [31:0] MEMTEST_DIAG_MAILBOX_PHYS = 32'h3007_F120,
	parameter int BURST      = 16
)(
	input  wire        clk,
	input  wire        reset,

	input  wire        start_req,
	input  wire        bank_sel,
	// From frame_store: hold new DMA until vsync consumed last swap
	input  wire        swap_pending,

	// Live OSD menu word to publish to the HPS (see mailbox layout above).
	input  wire [15:0] status_osd,
	input  wire        input_cmd_valid,
	input  wire  [7:0] input_cmd,
	// SDRAM bring-up telemetry to publish to the HPS (see mailbox layout above).
	input  wire  [3:0] sdram_test_state,
	input  wire  [3:0] sdram_size_code,
	input  wire [15:0] sdram_error_count,
	input  wire [15:0] sdram_read_sample,
	input  wire        sdram_first_fail_valid,
	input  wire [25:0] sdram_first_fail_addr,
	input  wire [15:0] sdram_first_fail_expect,
	input  wire  [7:0] frame_sdram_state,
	input  wire [15:0] frame_underrun_count,

	output wire        DDRAM_CLK,
	input  wire        DDRAM_BUSY,
	output reg   [7:0] DDRAM_BURSTCNT,
	output reg  [28:0] DDRAM_ADDR,
	input  wire [63:0] DDRAM_DOUT,
	input  wire        DDRAM_DOUT_READY,
	output reg         DDRAM_RD,
	output reg  [63:0] DDRAM_DIN,
	output wire  [7:0] DDRAM_BE,
	output reg         DDRAM_WE,

	output reg         wr_en,
	output reg  [15:0] wr_pixel,
	output reg         wr_reset_ptr,
	output reg         swap_req,
	input  wire        wr_ready,

	output reg         busy,
	output reg  [15:0] frames_done,
	output reg         doorbell_ok
);

`include "ddr_frame_layout_params.svh"

	localparam int PIXELS = WIDTH * HEIGHT;
	localparam int QWORDS = PIXELS / 4;
	localparam [28:0] BASE_W0 = PHYS_BASE[31:3];
	localparam [28:0] BASE_W1 = PHYS_BASE[31:3] + 29'h8000;
	localparam [28:0] DOORBELL_W = DOORBELL_PHYS[31:3];
	localparam [28:0] MAILBOX_W  = MAILBOX_PHYS[31:3];
	localparam [28:0] INPUT_MAILBOX_W = INPUT_MAILBOX_PHYS[31:3];
	localparam [28:0] MEMTEST_MAILBOX_W = MEMTEST_MAILBOX_PHYS[31:3];
	localparam [28:0] UNDERRUN_MAILBOX_W = UNDERRUN_MAILBOX_PHYS[31:3];
	localparam [28:0] MEMTEST_DIAG_MAILBOX_W = MEMTEST_DIAG_MAILBOX_PHYS[31:3];
	localparam [31:0] MAGIC = 32'h504C_584B;
	localparam [31:0] MAGIC_S = 32'h504C_5853;
	localparam [31:0] MAGIC_I = 32'h504C_5849;
	localparam [31:0] MAGIC_M = 32'h504C_584D;
	localparam [31:0] MAGIC_F = 32'h504C_5846;
	localparam [4:0]  MEMTEST_DIAG_VERSION = 5'd1;

	localparam int FIFO_AW = 5;
	localparam int FIFO_N  = 1 << FIFO_AW;
	localparam int CMD_FIFO_AW = 2;
	localparam int CMD_FIFO_N  = 1 << CMD_FIFO_AW;

	assign DDRAM_CLK = clk;
	assign DDRAM_BE  = 8'hFF;

	reg [63:0] fifo_mem [0:FIFO_N-1];
	reg [FIFO_AW:0] fifo_wr, fifo_rd;
	wire [FIFO_AW:0] fifo_level = fifo_wr - fifo_rd;
	wire fifo_empty = (fifo_wr == fifo_rd);
	wire [FIFO_AW-1:0] fifo_wix = fifo_wr[FIFO_AW-1:0];
	wire [FIFO_AW-1:0] fifo_rix = fifo_rd[FIFO_AW-1:0];

	reg [28:0] rd_addr;
	reg [15:0] qwords_issued;
	reg [15:0] qwords_written;
	reg [15:0] inflight;
	reg        start_d;
	reg        active;
	reg [1:0]  pix_i;
	reg [63:0] beat_q;
	reg        have_beat;
	reg        need_reset;
	reg        bank_r;
	reg [30:0] last_seq;
	reg        have_seq;
	reg [15:0] poll_div;
	reg        poll_pending;

	// Status mailbox (core -> HPS). Published on change and on a slow heartbeat
	// so the host always converges even if it starts late or misses a change.
	reg [15:0] mbox_seq;
	reg [15:0] mbox_last;
	reg        mbox_req;
	reg        mbox_valid;
	reg [17:0] mbox_hb;
	reg  [7:0] sdram_mbox_seq;
	reg [23:0] sdram_mbox_last;
	reg        sdram_mbox_req;
	reg        sdram_mbox_valid;
	reg [17:0] sdram_mbox_hb;
	reg [63:0] sdram_diag_last;
	reg        sdram_diag_req;
	reg        sdram_diag_valid;
	reg  [7:0] frame_mbox_seq;
	reg [23:0] frame_mbox_last;
	reg        frame_mbox_req;
	reg        frame_mbox_valid;
	reg [17:0] frame_mbox_hb;

	// Input mailbox FIFO. Human input is sparse, but a tiny queue keeps
	// commands from being lost while a frame DMA or status publish owns DDR.
	reg [7:0]  cmd_fifo [0:CMD_FIFO_N-1];
	reg [CMD_FIFO_AW:0] cmd_fifo_wr, cmd_fifo_rd;
	wire [CMD_FIFO_AW:0] cmd_fifo_level = cmd_fifo_wr - cmd_fifo_rd;
	wire cmd_fifo_empty = (cmd_fifo_wr == cmd_fifo_rd);
	wire cmd_fifo_full  = cmd_fifo_level[CMD_FIFO_AW];
	wire [CMD_FIFO_AW-1:0] cmd_fifo_wix = cmd_fifo_wr[CMD_FIFO_AW-1:0];
	wire [CMD_FIFO_AW-1:0] cmd_fifo_rix = cmd_fifo_rd[CMD_FIFO_AW-1:0];
	wire [7:0] cmd_fifo_head = cmd_fifo[cmd_fifo_rix];
	wire [63:0] sdram_diag_word = {sdram_read_sample, sdram_first_fail_valid,
	                               sdram_first_fail_addr, sdram_first_fail_expect,
	                               MEMTEST_DIAG_VERSION};
	reg [15:0] imbox_seq;
	reg [7:0]  imbox_cmd_seq;

	// Held start while swap_pending (or overlapping edge)
	reg        hold_start;
	reg        hold_bank;

	wire [28:0] bank_base = bank_r ? BASE_W1 : BASE_W0;

	wire [15:0] cur_pix =
		(pix_i == 2'd0) ? beat_q[15:0]  :
		(pix_i == 2'd1) ? beat_q[31:16] :
		(pix_i == 2'd2) ? beat_q[47:32] : beat_q[63:48];

	wire [FIFO_AW:0] space = FIFO_N[FIFO_AW:0] - fifo_level;
	wire can_issue = active && !DDRAM_BUSY && !DDRAM_RD && !DDRAM_WE
		&& (qwords_issued < QWORDS[15:0])
		&& (inflight == 0)
		&& (space >= BURST[FIFO_AW:0])
		&& !need_reset
		&& !poll_pending;

	wire [15:0] remain = QWORDS[15:0] - qwords_issued;
	wire [7:0]  this_burst = (remain >= BURST[15:0]) ? BURST[7:0] : remain[7:0];

	wire        do_issue = can_issue;
	wire [15:0] inf_after_issue = do_issue ? (inflight + {8'd0, this_burst}) : inflight;
	wire        do_ready_frame = DDRAM_DOUT_READY && active && !poll_pending;
	wire [15:0] inf_next =
		do_ready_frame ? (inf_after_issue != 0 ? inf_after_issue - 16'd1 : 16'd0)
		               : inf_after_issue;

	// SPI start edge or doorbell new-seq (detected even if we cannot launch yet)
	wire spi_edge    = start_req && !start_d;
	wire db_magic_ok = poll_pending && DDRAM_DOUT_READY && (DDRAM_DOUT[31:0] == MAGIC);
	wire db_new_seq  = db_magic_ok && (!have_seq || (DDRAM_DOUT[62:32] != last_seq));
	wire db_bank     = DDRAM_DOUT[63];

	// Launch when idle and vsync has consumed prior swap. Doorbell detect runs
	// while poll_pending=1, so free_for_dma must NOT require !poll_pending.
	// Held starts wait until the poll cycle ends so DDRAM_RD is free.
	wire free_for_dma = !active && !swap_pending;
	wire fresh_spi    = spi_edge && free_for_dma && !poll_pending;
	wire fresh_db     = db_new_seq && free_for_dma;
	wire held_go      = hold_start && free_for_dma && !poll_pending;
	wire any_start    = fresh_spi || fresh_db || held_go;
	wire start_bank   = fresh_db  ? db_bank :
	                    fresh_spi ? bank_sel :
	                    hold_bank;
	// can_launch: used for hold-queue decisions (not ready to run yet)
	wire can_launch   = free_for_dma && (!poll_pending || db_new_seq);

	always @(posedge clk) begin
		wr_en        <= 1'b0;
		wr_reset_ptr <= 1'b0;
		swap_req     <= 1'b0;
		start_d      <= start_req;

		if (reset) begin
			busy           <= 1'b0;
			active         <= 1'b0;
			frames_done    <= 16'd0;
			DDRAM_RD       <= 1'b0;
			DDRAM_ADDR     <= 29'd0;
			DDRAM_BURSTCNT <= 8'd1;
			fifo_wr        <= 0;
			fifo_rd        <= 0;
			qwords_issued  <= 0;
			qwords_written <= 0;
			inflight       <= 0;
			have_beat      <= 1'b0;
			pix_i          <= 2'd0;
			need_reset     <= 1'b0;
			bank_r         <= 1'b0;
			last_seq       <= 31'd0;
			have_seq       <= 1'b0;
			poll_div       <= 16'd0;
			poll_pending   <= 1'b0;
			doorbell_ok    <= 1'b0;
			DDRAM_WE       <= 1'b0;
			DDRAM_DIN      <= 64'd0;
			mbox_seq       <= 16'd0;
			mbox_last      <= 16'd0;
			mbox_req       <= 1'b1; // publish once as soon as we go idle
			mbox_valid     <= 1'b0;
			mbox_hb        <= 18'd0;
			sdram_mbox_seq   <= 8'd0;
			sdram_mbox_last  <= 24'd0;
			sdram_mbox_req   <= 1'b1;
			sdram_mbox_valid <= 1'b0;
			sdram_mbox_hb    <= 18'd0;
			sdram_diag_last  <= 64'd0;
			sdram_diag_req   <= 1'b1;
			sdram_diag_valid <= 1'b0;
			frame_mbox_seq   <= 8'd0;
			frame_mbox_last  <= 24'd0;
			frame_mbox_req   <= 1'b1;
			frame_mbox_valid <= 1'b0;
			frame_mbox_hb    <= 18'd0;
			cmd_fifo_wr    <= 0;
			cmd_fifo_rd    <= 0;
			imbox_seq      <= 16'd0;
			imbox_cmd_seq  <= 8'd0;
			hold_start     <= 1'b0;
			hold_bank      <= 1'b0;
		end else begin
			if (!DDRAM_BUSY) begin
				DDRAM_RD <= 1'b0;
				DDRAM_WE <= 1'b0;
			end

			// Request a publish when the OSD word changes, and periodically so a
			// host that attaches later still gets a value without any SPI.
			mbox_hb <= mbox_hb + 18'd1;
			if (!mbox_valid || (status_osd != mbox_last) || (mbox_hb == 18'd0))
				mbox_req <= 1'b1;
			sdram_mbox_hb <= sdram_mbox_hb + 18'd1;
			if (!sdram_mbox_valid
			    || ({sdram_error_count, sdram_size_code, sdram_test_state} != sdram_mbox_last)
			    || (sdram_diag_word != sdram_diag_last)
			    || (sdram_mbox_hb == 18'd0)) begin
				sdram_mbox_req <= 1'b1;
				sdram_diag_req <= 1'b1;
			end
			frame_mbox_hb <= frame_mbox_hb + 18'd1;
			if (!frame_mbox_valid
			    || ({frame_underrun_count, frame_sdram_state} != frame_mbox_last)
			    || (frame_mbox_hb == 18'd0))
				frame_mbox_req <= 1'b1;

			if (input_cmd_valid && (input_cmd != 8'd0) && !cmd_fifo_full) begin
				cmd_fifo[cmd_fifo_wix] <= input_cmd;
				cmd_fifo_wr <= cmd_fifo_wr + 1'd1;
			end

			// Capture doorbell seq whenever we see a new one (even if held)
			if (db_new_seq) begin
				last_seq    <= DDRAM_DOUT[62:32];
				have_seq    <= 1'b1;
				doorbell_ok <= 1'b1;
			end

			// Queue start if we cannot launch yet (swap_pending / active / poll)
			// Latest doorbell or SPI bank wins.
			if (db_new_seq && !can_launch) begin
				hold_start <= 1'b1;
				hold_bank  <= db_bank;
			end else if (spi_edge && !can_launch) begin
				hold_start <= 1'b1;
				hold_bank  <= bank_sel;
			end

			// Complete doorbell poll when not launching from it this cycle
			if (poll_pending && DDRAM_DOUT_READY && !(db_new_seq && can_launch))
				poll_pending <= 1'b0;

			if (any_start) begin
				active         <= 1'b1;
				busy           <= 1'b1;
				bank_r         <= start_bank;
				rd_addr        <= start_bank ? BASE_W1 : BASE_W0;
				qwords_issued  <= 0;
				qwords_written <= 0;
				inflight       <= 0;
				fifo_wr        <= 0;
				fifo_rd        <= 0;
				have_beat      <= 1'b0;
				pix_i          <= 2'd0;
				need_reset     <= 1'b1;
				poll_pending   <= 1'b0;
				hold_start     <= 1'b0;
			end else if (!active) begin
				// Idle doorbell poll ~every 256 cycles
				poll_div <= poll_div + 16'd1;
				if (!poll_pending && poll_div[7:0] == 8'd0 && !DDRAM_BUSY && !DDRAM_RD
				    && !DDRAM_WE) begin
					DDRAM_ADDR     <= DOORBELL_W;
					DDRAM_BURSTCNT <= 8'd1;
					DDRAM_RD       <= 1'b1;
					poll_pending   <= 1'b1;
				end
				// Publish the OSD word in a slot that cannot collide with the
				// poll above: idle only, no read outstanding, bus free.
				else if (!cmd_fifo_empty && !poll_pending && poll_div[7:0] == 8'd64
				         && !DDRAM_BUSY && !DDRAM_RD && !DDRAM_WE) begin
					DDRAM_ADDR     <= INPUT_MAILBOX_W;
					DDRAM_BURSTCNT <= 8'd1;
					DDRAM_DIN      <= {imbox_seq + 16'd1, imbox_cmd_seq + 8'd1,
					                   cmd_fifo_head, MAGIC_I};
					DDRAM_WE       <= 1'b1;
					imbox_seq      <= imbox_seq + 16'd1;
					imbox_cmd_seq  <= imbox_cmd_seq + 8'd1;
					cmd_fifo_rd    <= cmd_fifo_rd + 1'd1;
				end
				else if (mbox_req && !poll_pending && poll_div[7:0] == 8'd128
				         && !DDRAM_BUSY && !DDRAM_RD && !DDRAM_WE) begin
					DDRAM_ADDR     <= MAILBOX_W;
					DDRAM_BURSTCNT <= 8'd1;
					DDRAM_DIN      <= {mbox_seq + 16'd1, status_osd, MAGIC_S};
					DDRAM_WE       <= 1'b1;
					mbox_seq       <= mbox_seq + 16'd1;
					mbox_last      <= status_osd;
					mbox_valid     <= 1'b1;
					mbox_req       <= 1'b0;
				end
				else if (sdram_mbox_req && !poll_pending && poll_div[7:0] == 8'd192
				         && !DDRAM_BUSY && !DDRAM_RD && !DDRAM_WE) begin
					DDRAM_ADDR     <= MEMTEST_MAILBOX_W;
					DDRAM_BURSTCNT <= 8'd1;
					DDRAM_DIN      <= {sdram_error_count, sdram_size_code, sdram_test_state,
					                   sdram_mbox_seq + 8'd1, MAGIC_M};
					DDRAM_WE       <= 1'b1;
					sdram_mbox_seq   <= sdram_mbox_seq + 8'd1;
					sdram_mbox_last  <= {sdram_error_count, sdram_size_code, sdram_test_state};
					sdram_mbox_valid <= 1'b1;
					sdram_mbox_req   <= 1'b0;
				end
				else if (sdram_diag_req && !poll_pending && poll_div[7:0] == 8'd208
				         && !DDRAM_BUSY && !DDRAM_RD && !DDRAM_WE) begin
					DDRAM_ADDR     <= MEMTEST_DIAG_MAILBOX_W;
					DDRAM_BURSTCNT <= 8'd1;
					DDRAM_DIN      <= sdram_diag_word;
					DDRAM_WE       <= 1'b1;
					sdram_diag_last  <= sdram_diag_word;
					sdram_diag_valid <= 1'b1;
					sdram_diag_req   <= 1'b0;
				end
				else if (frame_mbox_req && !poll_pending && poll_div[7:0] == 8'd224
				         && !DDRAM_BUSY && !DDRAM_RD && !DDRAM_WE) begin
					DDRAM_ADDR     <= UNDERRUN_MAILBOX_W;
					DDRAM_BURSTCNT <= 8'd1;
					DDRAM_DIN      <= {frame_underrun_count, frame_sdram_state,
					                   frame_mbox_seq + 8'd1, MAGIC_F};
					DDRAM_WE       <= 1'b1;
					frame_mbox_seq   <= frame_mbox_seq + 8'd1;
					frame_mbox_last  <= {frame_underrun_count, frame_sdram_state};
					frame_mbox_valid <= 1'b1;
					frame_mbox_req   <= 1'b0;
				end
			end else begin
				// Active frame DMA
				if (need_reset) begin
					wr_reset_ptr <= 1'b1;
					need_reset   <= 1'b0;
				end

				if (do_issue) begin
					DDRAM_ADDR     <= rd_addr;
					DDRAM_BURSTCNT <= this_burst;
					DDRAM_RD       <= 1'b1;
					rd_addr        <= rd_addr + {21'd0, this_burst};
					qwords_issued  <= qwords_issued + {8'd0, this_burst};
				end

				if (do_ready_frame) begin
					fifo_mem[fifo_wix] <= DDRAM_DOUT;
					fifo_wr <= fifo_wr + 1'd1;
				end

				inflight <= inf_next;

				if (active && !need_reset && wr_ready) begin
					if (!have_beat) begin
						if (!fifo_empty) begin
							beat_q    <= fifo_mem[fifo_rix];
							fifo_rd   <= fifo_rd + 1'd1;
							have_beat <= 1'b1;
							pix_i     <= 2'd0;
						end
					end else begin
						wr_pixel <= cur_pix;
						wr_en    <= 1'b1;
						if (pix_i == 2'd3) begin
							have_beat <= 1'b0;
							qwords_written <= qwords_written + 16'd1;
							if (qwords_written + 16'd1 == QWORDS[15:0]) begin
								swap_req    <= 1'b1;
								frames_done <= frames_done + 16'd1;
								active      <= 1'b0;
								busy        <= 1'b0;
							end
						end else
							pix_i <= pix_i + 2'd1;
					end
				end
			end
		end
	end

endmodule
