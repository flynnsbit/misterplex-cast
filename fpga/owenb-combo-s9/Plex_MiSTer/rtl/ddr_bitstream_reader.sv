// HPS DDR compressed-bitstream record-ring reader.
//
// The ARM daemon writes fixed 32-byte records into a power-of-two HPS DDR ring
// and publishes an absolute producer byte count in CTRL_PHYS.  NAL records carry
// Annex-B payload bytes; control records (begin/flush/end/pause/resume) are
// consumed at record boundaries so a seek/flush cannot splice mid-NAL.  The FPGA
// publishes read_count plus transport telemetry in DDR, keeping this path wholly
// separate from MiSTer's shared HPS<->FPGA SPI/GPO register.
//
// Fabric-decode feed (w-path) — two product wiring options for w-plxd:
//
//   Source-graph note (rd-duck NACK on 19/17 file-union probe): treat LIVE/DEAD
//   counts as instrument-dependent. Post-fit hierarchy + PRODUCT_NO_STUB are
//   authority for what ships. decode_stub is diagnostic/partial; product frames
//   still come from ARM FFmpeg until a complete fabric path is wired and fit.
//   rbsp_filter + exp_golomb remain uninstantiated on the product path today.
//
//   (1) Front-end chain (w-plxd ENABLE_FABRIC_DECODE):
//         reader.out_valid/out_byte/out_last  →  h264_rbsp_filter.in_*
//         reader.out_full                    := !h264_rbsp_filter.in_ready
//         reader.out_flush                   →  h264_rbsp_filter.clear (pulse)
//         rbsp_filter.out_*                  →  bit window (STRIP_EPB=0) → exp_golomb
//
//   (2) Integrated bit feed (ENABLE_BIT_FEED=1): reader does EPB strip + MSB bit
//       window in-module and presents bit_valid/bit_value/bit_ready directly to
//       exp_golomb / CAVLC. Default ENABLE_BIT_FEED=0 keeps stream_path's
//       legacy byte-only contract (out_valid/out_byte/out_full).

// -----------------------------------------------------------------------------
// bitstream_bit_feeder — byte stream → EPB strip → bit window (MSB-first).
// Lives in this file so files.qip needs no second entry (collision control).
// -----------------------------------------------------------------------------
module bitstream_bit_feeder #(
	parameter int BYTE_Q_DEPTH = 8, // power-of-two skid after optional EPB strip
	// 1: annex-B in, strip 0x000003 (standalone path / ENABLE_BIT_FEED)
	// 0: already-RBSP bytes in (after h264_rbsp_filter) — bit window only
	parameter bit STRIP_EPB = 1'b1
)(
	input  wire        clk,
	input  wire        reset,
	input  wire        clear,

	// Bytes in: annex-B when STRIP_EPB=1, RBSP when STRIP_EPB=0
	input  wire        in_valid,
	input  wire [7:0]  in_byte,
	input  wire        in_last,   // pulses with final byte of a NAL payload
	output wire        in_ready,

	// Continuous bit stream for CAVLC / exp-golomb (H.264 MSB-first within byte)
	output wire        bit_valid,
	output wire        bit_value,
	input  wire        bit_ready,

	output reg         nal_bit_last, // 1-cycle pulse after last RBSP bit of NAL drained
	output reg  [15:0] epb_removed,
	output reg  [15:0] rbsp_bytes,
	output reg  [31:0] bits_out,
	output wire        byte_q_full,
	output wire        byte_q_empty
);
	localparam int QAW = $clog2(BYTE_Q_DEPTH);

	// --- Optional EPB strip (same contract as h264_rbsp_filter when STRIP_EPB=1) ---
	reg [1:0] zero_count;
	reg       inhibit_skip;
	wire      epb_can_accept;
	wire      skip_epb;

	// --- RBSP byte skid (holds bytes while bit consumer backpressures) ---
	reg [7:0] q_data [0:BYTE_Q_DEPTH-1];
	reg       q_last [0:BYTE_Q_DEPTH-1];
	reg [QAW:0] q_wr;
	reg [QAW:0] q_rd;
	wire [QAW:0] q_level = q_wr - q_rd;
	assign byte_q_empty = (q_wr == q_rd);
	assign byte_q_full  = (q_level >= BYTE_Q_DEPTH[QAW:0]);
	assign epb_can_accept = !byte_q_full;
	assign skip_epb = STRIP_EPB && in_valid && epb_can_accept && !inhibit_skip &&
	                  (zero_count == 2'd2) && (in_byte == 8'h03);
	// Accept input when EPB stage can push or skip without growing a full queue.
	assign in_ready = epb_can_accept;

	// --- Current byte under bit extraction ---
	reg        have_cur;
	reg [7:0]  cur_byte;
	reg        cur_last;
	reg [2:0]  bit_idx; // 0 = MSB (bit7)
	reg        pending_nal_last;

	wire bits_in_cur = have_cur;
	assign bit_valid = bits_in_cur;
	assign bit_value = cur_byte[3'd7 - bit_idx];

	wire take_bit = bit_valid && bit_ready;
	wire load_cur = !have_cur && !byte_q_empty;

	integer qi;
	always @(posedge clk) begin
		if (reset || clear) begin
			zero_count <= 2'd0;
			inhibit_skip <= 1'b0;
			epb_removed <= 16'd0;
			rbsp_bytes <= 16'd0;
			bits_out <= 32'd0;
			q_wr <= '0;
			q_rd <= '0;
			have_cur <= 1'b0;
			cur_byte <= 8'd0;
			cur_last <= 1'b0;
			bit_idx <= 3'd0;
			pending_nal_last <= 1'b0;
			nal_bit_last <= 1'b0;
			for (qi = 0; qi < BYTE_Q_DEPTH; qi = qi + 1) begin
				q_data[qi] <= 8'd0;
				q_last[qi] <= 1'b0;
			end
		end else begin
			nal_bit_last <= 1'b0;

			// Optional EPB filter → push RBSP bytes into skid
			if (in_valid && in_ready) begin
				if (skip_epb) begin
					if (epb_removed != 16'hFFFF)
						epb_removed <= epb_removed + 16'd1;
					inhibit_skip <= 1'b1;
					// EPB is not last-of-RBSP content; if in_last, NAL ends after skip
					if (in_last)
						pending_nal_last <= 1'b1;
				end else begin
					q_data[q_wr[QAW-1:0]] <= in_byte;
					q_last[q_wr[QAW-1:0]] <= in_last;
					q_wr <= q_wr + 1'd1;
					if (rbsp_bytes != 16'hFFFF)
						rbsp_bytes <= rbsp_bytes + 16'd1;
					if (STRIP_EPB) begin
						if (in_byte == 8'h00)
							zero_count <= (zero_count == 2'd2) ? 2'd2 : (zero_count + 2'd1);
						else
							zero_count <= 2'd0;
						inhibit_skip <= 1'b0;
					end
				end
			end

			// Load next RBSP byte into bit window when empty
			if (load_cur) begin
				cur_byte <= q_data[q_rd[QAW-1:0]];
				cur_last <= q_last[q_rd[QAW-1:0]];
				q_rd <= q_rd + 1'd1;
				have_cur <= 1'b1;
				bit_idx <= 3'd0;
			end

			// Consume one bit under backpressure
			if (take_bit) begin
				if (bits_out != 32'hFFFF_FFFF)
					bits_out <= bits_out + 32'd1;
				if (bit_idx == 3'd7) begin
					have_cur <= 1'b0;
					bit_idx <= 3'd0;
					if (cur_last || (pending_nal_last && byte_q_empty)) begin
						nal_bit_last <= 1'b1;
						pending_nal_last <= 1'b0;
					end
				end else begin
					bit_idx <= bit_idx + 3'd1;
				end
			end
		end
	end
endmodule

module ddr_bitstream_reader #(
	parameter [31:0] DATA_PHYS  = 32'h3010_0000,
	parameter [31:0] CTRL_PHYS  = 32'h3014_0000,
	parameter [31:0] READ_PHYS  = 32'h3014_0008,
	parameter [31:0] ERR_PHYS   = 32'h3014_0010,
	parameter [31:0] STAT0_PHYS = 32'h3014_0018,
	parameter [31:0] STAT1_PHYS = 32'h3014_0020,
	parameter [31:0] STAT2_PHYS = 32'h3014_0028,
	parameter [31:0] STAT3_PHYS = 32'h3014_0030,
	parameter [31:0] STAT4_PHYS = 32'h3014_0038,
	parameter [31:0] STAT5_PHYS = 32'h3014_0040,
	parameter [31:0] STAT6_PHYS = 32'h3014_0048,
	parameter int RING_BYTES    = 262144,
	parameter int POLL_DIV_BITS = 6,
	// w-path fabric-decode feed (default off: stream_path byte contract unchanged)
	parameter bit ENABLE_BIT_FEED = 1'b0
)(
	input  wire        clk,
	input  wire        reset,
	input  wire        enable,
	input  wire        flush,

	// Annex-B NAL payload bytes (EPB still present). Matches h264_rbsp_filter
	// input contract when out_full = !rbsp_filter.in_ready.
	output reg         out_valid,
	output reg  [7:0]  out_byte,
	output reg         out_last,   // 1 with final payload byte of current NAL record
	output reg         out_flush,  // seek/BEGIN/END/FLUSH → rbsp_filter.clear
	input  wire        out_full,

	// Optional integrated RBSP bit feed (ENABLE_BIT_FEED=1).
	// Quartus forbids default values on inputs — parents must tie bit_ready
	// (stream_path uses 1'b1 when the bit port is unused).
	output wire        bit_valid,
	output wire        bit_value,
	input  wire        bit_ready,
	output wire        bit_nal_last,
	output wire [15:0] bit_epb_removed,
	output wire [15:0] bit_rbsp_bytes,
	output wire [31:0] bit_bits_out,

	output reg         bus_want,
	input  wire        DDRAM_BUSY,
	output reg   [7:0] DDRAM_BURSTCNT,
	output reg  [28:0] DDRAM_ADDR,
	input  wire [63:0] DDRAM_DOUT,
	input  wire        DDRAM_DOUT_READY,
	output reg         DDRAM_RD,
	output reg  [63:0] DDRAM_DIN,
	output wire  [7:0] DDRAM_BE,
	output reg         DDRAM_WE,

	output reg         active,
	output reg  [31:0] bytes_out,
	output reg  [15:0] underrun_count,
	output reg  [15:0] overrun_count,
	output reg  [31:0] host_write_count,
	output reg  [31:0] fpga_read_count
);
	localparam int RING_AW = $clog2(RING_BYTES);
	localparam [28:0] DATA_W  = DATA_PHYS[31:3];
	localparam [28:0] CTRL_W  = CTRL_PHYS[31:3];
	localparam [28:0] READ_W  = READ_PHYS[31:3];
	localparam [28:0] ERR_W   = ERR_PHYS[31:3];
	localparam [28:0] STAT0_W = STAT0_PHYS[31:3];
	localparam [28:0] STAT1_W = STAT1_PHYS[31:3];
	localparam [28:0] STAT2_W = STAT2_PHYS[31:3];
	localparam [28:0] STAT3_W = STAT3_PHYS[31:3];
	localparam [28:0] STAT4_W = STAT4_PHYS[31:3];
	localparam [28:0] STAT5_W = STAT5_PHYS[31:3];
	localparam [28:0] STAT6_W = STAT6_PHYS[31:3];

	localparam [31:0] MAGIC_CTRL = 32'h504C_5842; // PLXB
	localparam [31:0] MAGIC_READ = 32'h504C_5852; // PLXR
	localparam [31:0] MAGIC_ERR  = 32'h504C_5845; // PLXE
	localparam [31:0] MAGIC_REC  = 32'h504C_584E; // PLXN
	localparam [31:0] MAGIC_ST0  = 32'h504C_5854; // PLXT
	localparam [31:0] MAGIC_ST1  = 32'h504C_5855; // PLXU
	localparam [31:0] MAGIC_ST2  = 32'h504C_5856; // PLXV
	localparam [31:0] MAGIC_ST3  = 32'h504C_5857; // PLXW
	localparam [31:0] MAGIC_ST4  = 32'h504C_5859; // PLXY
	localparam [31:0] MAGIC_ST5  = 32'h504C_585A; // PLXZ
	localparam [31:0] MAGIC_ST6  = 32'h504C_5851; // PLXQ

	localparam [7:0] EVENT_BEGIN  = 8'd1;
	localparam [7:0] EVENT_NAL    = 8'd2;
	localparam [7:0] EVENT_FLUSH  = 8'd3;
	localparam [7:0] EVENT_END    = 8'd4;
	localparam [7:0] EVENT_PAUSE  = 8'd5;
	localparam [7:0] EVENT_RESUME = 8'd6;

	localparam [31:0] RING_BYTES_W = 32'(RING_BYTES);
	localparam [31:0] HEADER_BYTES = 32'd32;

	assign DDRAM_BE = 8'hFF;

	localparam [3:0]
		ST_RESET     = 4'd0,
		ST_IDLE      = 4'd1,
		ST_POLL      = 4'd2,
		ST_READ_WAIT = 4'd3,
		ST_CONSUME   = 4'd4;

	localparam [1:0]
		MODE_HEADER  = 2'd0,
		MODE_PAYLOAD = 2'd1,
		MODE_DROP    = 2'd2;

	reg [3:0] state;
	reg [1:0] mode;
	reg [POLL_DIV_BITS-1:0] poll_div;
	reg [63:0] beat_q;
	reg [2:0] byte_idx;
	reg [3:0] beat_left;
	reg [31:0] write_count;
	reg [31:0] read_count;
	reg reset_seen;
	reg overrun_sticky;
	reg underrun_sticky;
	reg desync_sticky;
	reg fatal_sticky;
	reg paused;
	reg [7:0] telem_seq;
	reg publish_pending;
	reg [3:0] publish_step;
	reg have_ctrl;
	reg empty_seen;
	reg seen_payload;

	reg [7:0] hdr [0:31];
	reg [4:0] hdr_idx;
	reg [31:0] payload_left;
	reg [63:0] current_session;
	reg [31:0] expected_seq;
	reg [31:0] consumer_seq;
	reg [15:0] desync_count;
	reg [31:0] last_bad_seq;
	reg [7:0] rx_byte;

	wire ctrl_magic_ok = DDRAM_DOUT[31:0] == MAGIC_CTRL;
	wire ctrl_reset = DDRAM_DOUT[63];
	wire [30:0] ctrl_write_count = DDRAM_DOUT[62:32];
	wire [31:0] avail = write_count - read_count;
	wire [31:0] ring_level = (avail > RING_BYTES_W) ? RING_BYTES_W : avail;
	wire ring_has_data = have_ctrl && (avail != 32'd0) && !overrun_sticky && !fatal_sticky;
	wire [RING_AW-1:0] read_ring_index = read_count[RING_AW-1:0];
	wire [28:0] read_qword_offset = 29'(read_ring_index >> 3);
	wire [2:0] read_byte_index = read_ring_index[2:0];
	wire [31:0] bytes_to_qword_end = 32'd8 - {29'd0, read_byte_index};
	wire [31:0] consume_count_w =
		(avail < bytes_to_qword_end) ? avail : bytes_to_qword_end;
	wire [3:0] consume_count = consume_count_w[3:0];
	wire want_poll = enable && (poll_div == {POLL_DIV_BITS{1'b0}});
	wire want_read = enable && ring_has_data && (beat_left == 4'd0);
	wire want_pub = enable && publish_pending;
	// Bit feeder backpressure (ENABLE_BIT_FEED): hold-reg valid/ready, not a
	// single-cycle pulse. A pulse drops the next byte when the feeder skid is
	// one slot from full (ready sampled cycle N, valid arrives N+1 after full).
	wire bit_feed_in_ready;
	reg         bit_in_valid;
	reg  [7:0]  bit_in_byte;
	reg         bit_in_last;
	wire        bit_fire = bit_in_valid && bit_feed_in_ready;
	wire        bit_slot_free = !bit_in_valid || bit_feed_in_ready;
	wire payload_sink_ok = !out_full && (!ENABLE_BIT_FEED || bit_slot_free);
	wire can_consume = (mode != MODE_PAYLOAD) || payload_sink_ok;
	wire [15:0] state_flags = {4'd0, fatal_sticky, desync_sticky, paused, active,
	                           overrun_sticky, underrun_sticky, mode, state};

	// Payload byte → bit feeder holding register (cleared only on fire / clear)
	reg         bit_feed_soft_clear; // EVENT_FLUSH / BEGIN / END / host flush
	wire        bit_feed_clear = reset | bit_feed_soft_clear;

	generate
		if (ENABLE_BIT_FEED) begin : g_bit_feed
			bitstream_bit_feeder #(.BYTE_Q_DEPTH(8)) u_bit_feed (
				.clk(clk),
				.reset(reset),
				.clear(bit_feed_clear),
				.in_valid(bit_in_valid),
				.in_byte(bit_in_byte),
				.in_last(bit_in_last),
				.in_ready(bit_feed_in_ready),
				.bit_valid(bit_valid),
				.bit_value(bit_value),
				.bit_ready(bit_ready),
				.nal_bit_last(bit_nal_last),
				.epb_removed(bit_epb_removed),
				.rbsp_bytes(bit_rbsp_bytes),
				.bits_out(bit_bits_out),
				.byte_q_full(),
				.byte_q_empty()
			);
		end else begin : g_bit_feed_off
			assign bit_feed_in_ready = 1'b1;
			assign bit_valid = 1'b0;
			assign bit_value = 1'b0;
			assign bit_nal_last = 1'b0;
			assign bit_epb_removed = 16'd0;
			assign bit_rbsp_bytes = 16'd0;
			assign bit_bits_out = 32'd0;
		end
	endgenerate

	function automatic [7:0] beat_byte(input [63:0] beat, input [2:0] idx);
		begin
			case (idx)
				3'd0: beat_byte = beat[7:0];
				3'd1: beat_byte = beat[15:8];
				3'd2: beat_byte = beat[23:16];
				3'd3: beat_byte = beat[31:24];
				3'd4: beat_byte = beat[39:32];
				3'd5: beat_byte = beat[47:40];
				3'd6: beat_byte = beat[55:48];
				default: beat_byte = beat[63:56];
			endcase
		end
	endfunction

	function automatic [31:0] hdr32(input int base);
		begin
			hdr32 = {hdr[base + 3], hdr[base + 2], hdr[base + 1], hdr[base + 0]};
		end
	endfunction

	function automatic [63:0] hdr64(input int base);
		begin
			hdr64 = {hdr[base + 7], hdr[base + 6], hdr[base + 5], hdr[base + 4],
			         hdr[base + 3], hdr[base + 2], hdr[base + 1], hdr[base + 0]};
		end
	endfunction

	task automatic mark_desync(input [31:0] bad_seq);
		begin
			desync_sticky <= 1'b1;
			last_bad_seq <= bad_seq;
			if (desync_count != 16'hFFFF)
				desync_count <= desync_count + 16'd1;
			publish_pending <= 1'b1;
		end
	endtask

	task automatic reset_parser;
		begin
			mode <= MODE_HEADER;
			hdr_idx <= 5'd0;
			payload_left <= 32'd0;
			beat_left <= 4'd0;
		end
	endtask

	// Combo want for local ST_IDLE decisions; registered for CDC into
	// ddr_bus_arbiter m1_want 2FF (keepv22). Pathdump keepv21 Path#1 was
	// read_count/write_count → m1_want_s1 (general[0]→general[2], −0.619)
	// through this combo cone — register breaks that STA path to pure FF-CDC.
	reg bus_want_c;
	always @(*) begin
		bus_want_c = 1'b0;
		case (state)
			ST_IDLE: bus_want_c = want_poll || want_read || want_pub;
			ST_POLL, ST_READ_WAIT: bus_want_c = 1'b1;
			default: bus_want_c = 1'b0;
		endcase
	end

	always @(posedge clk) begin
		out_valid <= 1'b0;
		out_last <= 1'b0;
		out_flush <= 1'b0;
		bit_feed_soft_clear <= 1'b0;
		DDRAM_RD <= 1'b0;
		DDRAM_WE <= 1'b0;
		// Holding-reg handshake: drop valid only when feeder accepts.
		if (bit_fire) begin
			bit_in_valid <= 1'b0;
			bit_in_last <= 1'b0;
		end

		if (reset) begin
			bus_want <= 1'b0;
			state <= ST_RESET;
			mode <= MODE_HEADER;
			poll_div <= '0;
			DDRAM_RD <= 1'b0;
			DDRAM_WE <= 1'b0;
			DDRAM_BURSTCNT <= 8'd1;
			DDRAM_ADDR <= 29'd0;
			DDRAM_DIN <= 64'd0;
			out_last <= 1'b0;
			active <= 1'b0;
			paused <= 1'b0;
			bytes_out <= 32'd0;
			underrun_count <= 16'd0;
			overrun_count <= 16'd0;
			desync_count <= 16'd0;
			last_bad_seq <= 32'd0;
			host_write_count <= 32'd0;
			fpga_read_count <= 32'd0;
			write_count <= 32'd0;
			read_count <= 32'd0;
			current_session <= 64'd0;
			expected_seq <= 32'd0;
			consumer_seq <= 32'd0;
			reset_seen <= 1'b0;
			overrun_sticky <= 1'b0;
			underrun_sticky <= 1'b0;
			desync_sticky <= 1'b0;
			fatal_sticky <= 1'b0;
			telem_seq <= 8'd0;
			publish_pending <= 1'b1;
			publish_step <= 4'd0;
			have_ctrl <= 1'b0;
			empty_seen <= 1'b0;
			seen_payload <= 1'b0;
			beat_left <= 4'd0;
			byte_idx <= 3'd0;
			hdr_idx <= 5'd0;
			payload_left <= 32'd0;
			bit_in_valid <= 1'b0;
			bit_in_byte <= 8'd0;
			bit_in_last <= 1'b0;
			bit_feed_soft_clear <= 1'b1;
		end else if (!enable) begin
			state <= ST_IDLE;
			active <= 1'b0;
			paused <= 1'b0;
			bit_in_valid <= 1'b0;
			bit_in_last <= 1'b0;
			bit_feed_soft_clear <= 1'b1;
			reset_parser();
		end else begin
			bus_want <= bus_want_c;
			poll_div <= poll_div + 1'd1;

			if (flush) begin
				read_count <= write_count;
				fpga_read_count <= write_count;
				active <= 1'b0;
				paused <= 1'b0;
				bit_in_valid <= 1'b0;
				bit_in_last <= 1'b0;
				overrun_sticky <= 1'b0;
				underrun_sticky <= 1'b0;
				desync_sticky <= 1'b0;
				fatal_sticky <= 1'b0;
				seen_payload <= 1'b0;
				current_session <= 64'd0;
				expected_seq <= 32'd0;
				consumer_seq <= 32'd0;
				out_flush <= 1'b1;
				bit_feed_soft_clear <= 1'b1;
				publish_pending <= 1'b1;
				publish_step <= 4'd0;
				state <= ST_IDLE;
				reset_parser();
			end

			if (active && have_ctrl && seen_payload && (avail == 32'd0) &&
			    (beat_left == 4'd0) && !empty_seen && !paused) begin
				empty_seen <= 1'b1;
				underrun_sticky <= 1'b1;
				if (underrun_count != 16'hFFFF)
					underrun_count <= underrun_count + 16'd1;
				publish_pending <= 1'b1;
			end else if (avail != 32'd0) begin
				empty_seen <= 1'b0;
			end

			if (avail > RING_BYTES_W && !overrun_sticky) begin
				overrun_sticky <= 1'b1;
				if (overrun_count != 16'hFFFF)
					overrun_count <= overrun_count + 16'd1;
				publish_pending <= 1'b1;
				publish_step <= 4'd0;
			end

			case (state)
				ST_RESET: begin
					state <= ST_IDLE;
					publish_pending <= 1'b1;
					publish_step <= 4'd0;
				end

				ST_IDLE: begin
					if (publish_pending && !DDRAM_BUSY && !DDRAM_RD && !DDRAM_WE) begin
						DDRAM_BURSTCNT <= 8'd1;
						case (publish_step)
							4'd0: begin
								DDRAM_ADDR <= READ_W;
								DDRAM_DIN <= {read_count, MAGIC_READ};
								DDRAM_WE <= 1'b1;
								publish_step <= 4'd1;
							end
							4'd1: begin
								DDRAM_ADDR <= ERR_W;
								DDRAM_DIN <= {overrun_count[7:0], underrun_count[7:0],
								              active, overrun_sticky, underrun_sticky, 5'd0,
								              telem_seq + 8'd1, MAGIC_ERR};
								DDRAM_WE <= 1'b1;
								telem_seq <= telem_seq + 8'd1;
								publish_step <= 4'd2;
							end
							4'd2: begin
								DDRAM_ADDR <= STAT0_W;
								DDRAM_DIN <= {ring_level, MAGIC_ST0};
								DDRAM_WE <= 1'b1;
								publish_step <= 4'd3;
							end
							4'd3: begin
								DDRAM_ADDR <= STAT1_W;
								DDRAM_DIN <= {consumer_seq, MAGIC_ST1};
								DDRAM_WE <= 1'b1;
								publish_step <= 4'd4;
							end
							4'd4: begin
								DDRAM_ADDR <= STAT2_W;
								DDRAM_DIN <= {last_bad_seq, MAGIC_ST2};
								DDRAM_WE <= 1'b1;
								publish_step <= 4'd5;
							end
							4'd5: begin
								DDRAM_ADDR <= STAT3_W;
								DDRAM_DIN <= {current_session[31:0], MAGIC_ST3};
								DDRAM_WE <= 1'b1;
								publish_step <= 4'd6;
							end
							4'd6: begin
								DDRAM_ADDR <= STAT4_W;
								DDRAM_DIN <= {current_session[63:32], MAGIC_ST4};
								DDRAM_WE <= 1'b1;
								publish_step <= 4'd7;
							end
							4'd7: begin
								DDRAM_ADDR <= STAT5_W;
								DDRAM_DIN <= {underrun_count, overrun_count, MAGIC_ST5};
								DDRAM_WE <= 1'b1;
								publish_step <= 4'd8;
							end
							default: begin
								DDRAM_ADDR <= STAT6_W;
								DDRAM_DIN <= {desync_count, state_flags, MAGIC_ST6};
								DDRAM_WE <= 1'b1;
								publish_step <= 4'd0;
								publish_pending <= 1'b0;
							end
						endcase
					end else if (want_poll && !DDRAM_BUSY && !DDRAM_RD && !DDRAM_WE) begin
						DDRAM_ADDR <= CTRL_W;
						DDRAM_BURSTCNT <= 8'd1;
						DDRAM_RD <= 1'b1;
						state <= ST_POLL;
					end else if (want_read && !DDRAM_BUSY && !DDRAM_RD && !DDRAM_WE) begin
						DDRAM_ADDR <= DATA_W + read_qword_offset;
						DDRAM_BURSTCNT <= 8'd1;
						DDRAM_RD <= 1'b1;
						byte_idx <= read_byte_index;
						state <= ST_READ_WAIT;
					end
				end

				ST_POLL: begin
					if (DDRAM_DOUT_READY) begin
						if (ctrl_magic_ok) begin
							have_ctrl <= 1'b1;
							write_count <= {1'b0, ctrl_write_count};
							host_write_count <= {1'b0, ctrl_write_count};
							if (ctrl_reset != reset_seen) begin
								reset_seen <= ctrl_reset;
								read_count <= {1'b0, ctrl_write_count};
								fpga_read_count <= {1'b0, ctrl_write_count};
								active <= 1'b0;
								paused <= 1'b0;
								overrun_sticky <= 1'b0;
								underrun_sticky <= 1'b0;
								desync_sticky <= 1'b0;
								fatal_sticky <= 1'b0;
								seen_payload <= 1'b0;
								current_session <= 64'd0;
								expected_seq <= 32'd0;
								consumer_seq <= 32'd0;
								out_flush <= 1'b1;
								publish_pending <= 1'b1;
								publish_step <= 4'd0;
								reset_parser();
							end
						end
						state <= ST_IDLE;
					end
				end

				ST_READ_WAIT: begin
					if (DDRAM_DOUT_READY) begin
						beat_q <= DDRAM_DOUT;
						beat_left <= consume_count;
						state <= ST_CONSUME;
					end
				end

				ST_CONSUME: begin
					if (beat_left != 4'd0 && can_consume) begin
						rx_byte = beat_byte(beat_q, byte_idx);
						byte_idx <= byte_idx + 3'd1;
						beat_left <= beat_left - 4'd1;
						read_count <= read_count + 32'd1;
						fpga_read_count <= read_count + 32'd1;
						publish_pending <= 1'b1;

						if (mode == MODE_PAYLOAD) begin
							out_byte <= rx_byte;
							out_valid <= 1'b1;
							// NAL boundary for h264_rbsp_filter.in_last
							out_last <= (payload_left == 32'd1);
							// Fabric bit feed: same payload byte, last marks NAL end
							bit_in_valid <= ENABLE_BIT_FEED;
							bit_in_byte <= rx_byte;
							bit_in_last <= ENABLE_BIT_FEED && (payload_left == 32'd1);
							bytes_out <= bytes_out + 32'd1;
							seen_payload <= 1'b1;
							payload_left <= payload_left - 32'd1;
							if (payload_left == 32'd1) begin
								mode <= MODE_HEADER;
								hdr_idx <= 5'd0;
							end
						end else if (mode == MODE_DROP) begin
							payload_left <= payload_left - 32'd1;
							if (payload_left == 32'd1) begin
								mode <= MODE_HEADER;
								hdr_idx <= 5'd0;
							end
						end else begin
							hdr[hdr_idx] = rx_byte;
							if (hdr_idx == 5'd31) begin
								hdr_idx <= 5'd0;
								if (hdr32(0) != MAGIC_REC || hdr32(24) != 32'd0) begin
									fatal_sticky <= 1'b1;
									active <= 1'b0;
									mark_desync(hdr32(16));
								end else if (hdr[4] == EVENT_BEGIN) begin
									if (active || hdr32(20) != 32'd0) begin
										fatal_sticky <= 1'b1;
										mark_desync(hdr32(16));
									end else begin
										active <= 1'b1;
										paused <= 1'b0;
										current_session <= hdr64(8);
										expected_seq <= 32'd0;
										consumer_seq <= 32'd0;
										seen_payload <= 1'b0;
										out_flush <= 1'b1;
										bit_feed_soft_clear <= 1'b1;
										bit_in_valid <= 1'b0;
										bit_in_last <= 1'b0;
									end
								end else if (hdr[4] == EVENT_NAL) begin
									if (!active || paused || hdr64(8) != current_session ||
									    hdr32(16) != expected_seq) begin
										mark_desync(hdr32(16));
										if (hdr32(20) != 32'd0) begin
											payload_left <= hdr32(20);
											mode <= MODE_DROP;
										end
									end else begin
										consumer_seq <= hdr32(16);
										expected_seq <= hdr32(16) + 32'd1;
										if (hdr32(20) != 32'd0) begin
											payload_left <= hdr32(20);
											mode <= MODE_PAYLOAD;
										end
									end
								end else if (hdr[4] == EVENT_FLUSH) begin
									if (!active || hdr64(8) != current_session || hdr32(20) != 32'd0) begin
										mark_desync(hdr32(16));
									end else begin
										out_flush <= 1'b1;
										bit_feed_soft_clear <= 1'b1;
										bit_in_valid <= 1'b0;
										bit_in_last <= 1'b0;
										seen_payload <= 1'b0;
									end
								end else if (hdr[4] == EVENT_END) begin
									if (!active || hdr64(8) != current_session || hdr32(20) != 32'd0) begin
										mark_desync(hdr32(16));
									end else begin
										active <= 1'b0;
										paused <= 1'b0;
										out_flush <= 1'b1;
										bit_feed_soft_clear <= 1'b1;
										bit_in_valid <= 1'b0;
										bit_in_last <= 1'b0;
									end
								end else if (hdr[4] == EVENT_PAUSE) begin
									if (!active || hdr64(8) != current_session || hdr32(20) != 32'd0)
										mark_desync(hdr32(16));
									else
										paused <= 1'b1;
								end else if (hdr[4] == EVENT_RESUME) begin
									if (!active || hdr64(8) != current_session || hdr32(20) != 32'd0)
										mark_desync(hdr32(16));
									else
										paused <= 1'b0;
								end else begin
									mark_desync(hdr32(16));
								end
							end else begin
								hdr_idx <= hdr_idx + 5'd1;
							end
						end

						if (beat_left == 4'd1)
							state <= ST_IDLE;
					end else if (beat_left == 4'd0) begin
						state <= ST_IDLE;
					end
				end

				default: state <= ST_IDLE;
			endcase
		end
	end
endmodule
