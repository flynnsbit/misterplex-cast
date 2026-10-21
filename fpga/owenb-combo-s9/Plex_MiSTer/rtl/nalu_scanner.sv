// Phase 3.3–3.3d: annex-B scan + typed counts + RBSP capture (SPS/PPS/slice hdr).
// EPB (00 00 03) stripped. Slice capture stores first 32 bytes then ends parse.

module nalu_scanner (
	input  wire        clk,
	input  wire        reset,

	input  wire [7:0]  rd_data,
	input  wire        rd_empty,
	output reg         rd_en,

	output reg  [15:0] nalu_count,
	output reg  [7:0]  last_nal_type,
	output reg         has_stream,
	output reg  [31:0] bytes_seen,

	output reg  [7:0]  idr_count,
	output reg  [7:0]  sps_count,
	output reg  [7:0]  pps_count,
	output reg  [7:0]  slice_count,
	output reg         has_idr,
	output reg         vcl_pulse,

	output reg         sps_cap_clear,
	output reg         sps_cap_en,
	output reg  [7:0]  sps_cap_data,
	output reg         sps_cap_end,

	output reg         pps_cap_clear,
	output reg         pps_cap_en,
	output reg  [7:0]  pps_cap_data,
	output reg         pps_cap_end,

	output reg         sl_cap_clear,
	output reg         sl_cap_en,
	output reg  [7:0]  sl_cap_data,
	output reg         sl_cap_end,
	output reg         sl_is_idr,
	output reg         sl_nal_ref_idc_nonzero
);

	reg [1:0] zrun;
	reg       pend_type;
	reg       data_valid;
	reg [1:0] cap_tgt; // 0 none, 1 sps, 2 pps, 3 slice
	reg [1:0] epb_z;
	reg [5:0] cap_len;
	reg       sl_idr_r;
	reg       sl_ref_r;
	reg       sl_done; // slice header already ended (still draining NAL)

	wire [4:0] nal_t = rd_data[4:0];
	// Slice header + first mb_type fit in ~48 bytes of RBSP
	wire       can_store = (cap_tgt != 2'd0) && !sl_done &&
	                       !(cap_tgt == 2'd3 && cap_len >= 6'd48);

	always @(posedge clk) begin
		if (reset) begin
			rd_en         <= 0;
			nalu_count    <= 0;
			last_nal_type <= 0;
			has_stream    <= 0;
			bytes_seen    <= 0;
			zrun          <= 0;
			pend_type     <= 0;
			data_valid    <= 0;
			idr_count     <= 0;
			sps_count     <= 0;
			pps_count     <= 0;
			slice_count   <= 0;
			has_idr       <= 0;
			vcl_pulse     <= 0;
			cap_tgt       <= 0;
			epb_z         <= 0;
			cap_len       <= 0;
			sl_idr_r      <= 0;
			sl_done       <= 0;
			sps_cap_clear <= 0;
			sps_cap_en    <= 0;
			sps_cap_data  <= 0;
			sps_cap_end   <= 0;
			pps_cap_clear <= 0;
			pps_cap_en    <= 0;
			pps_cap_data  <= 0;
			pps_cap_end   <= 0;
			sl_cap_clear  <= 0;
			sl_cap_en     <= 0;
			sl_cap_data   <= 0;
			sl_cap_end    <= 0;
			sl_is_idr     <= 0;
			sl_nal_ref_idc_nonzero <= 0;
			sl_ref_r      <= 0;
		end else begin
			vcl_pulse     <= 1'b0;
			sps_cap_clear <= 1'b0;
			sps_cap_en    <= 1'b0;
			sps_cap_end   <= 1'b0;
			pps_cap_clear <= 1'b0;
			pps_cap_en    <= 1'b0;
			pps_cap_end   <= 1'b0;
			sl_cap_clear  <= 1'b0;
			sl_cap_en     <= 1'b0;
			sl_cap_end    <= 1'b0;

			rd_en <= !rd_empty;

			if (data_valid) begin
				bytes_seen <= bytes_seen + 1'd1;
				has_stream <= 1'b1;

				if (pend_type) begin
					// Close prior capture
					if (cap_tgt == 2'd1) sps_cap_end <= 1'b1;
					else if (cap_tgt == 2'd2) pps_cap_end <= 1'b1;
					else if (cap_tgt == 2'd3 && !sl_done) begin
						sl_cap_end <= 1'b1;
						sl_is_idr  <= sl_idr_r;
						sl_nal_ref_idc_nonzero <= sl_ref_r;
					end

					last_nal_type <= rd_data;
					nalu_count    <= nalu_count + 1'd1;
					pend_type     <= 1'b0;
					zrun          <= 0;
					epb_z         <= 0;
					cap_len       <= 0;
					sl_done       <= 0;

					case (nal_t)
						5'd7: begin
							sps_count     <= sps_count + 1'd1;
							cap_tgt       <= 2'd1;
							sps_cap_clear <= 1'b1;
						end
						5'd8: begin
							pps_count     <= pps_count + 1'd1;
							cap_tgt       <= 2'd2;
							pps_cap_clear <= 1'b1;
						end
						5'd5: begin
							idr_count    <= idr_count + 1'd1;
							has_idr      <= 1'b1;
							vcl_pulse    <= 1'b1;
							cap_tgt      <= 2'd3;
							sl_idr_r     <= 1'b1;
							sl_ref_r     <= (rd_data[6:5] != 2'd0);
							sl_cap_clear <= 1'b1;
						end
						5'd1: begin
							slice_count  <= slice_count + 1'd1;
							vcl_pulse    <= 1'b1;
							cap_tgt      <= 2'd3;
							sl_idr_r     <= 1'b0;
							sl_ref_r     <= (rd_data[6:5] != 2'd0);
							sl_cap_clear <= 1'b1;
						end
						default: cap_tgt <= 2'd0;
					endcase
				end else if (rd_data == 8'h00) begin
					if (zrun < 2'd3)
						zrun <= zrun + 1'd1;
					if (can_store) begin
						if (epb_z < 2'd2)
							epb_z <= epb_z + 1'd1;
						if (cap_tgt == 2'd1) begin
							sps_cap_en <= 1'b1; sps_cap_data <= 8'h00;
						end else if (cap_tgt == 2'd2) begin
							pps_cap_en <= 1'b1; pps_cap_data <= 8'h00;
						end else begin
							sl_cap_en <= 1'b1; sl_cap_data <= 8'h00;
						end
						cap_len <= cap_len + 1'd1;
						// slice: end after storing 32nd byte
						if (cap_tgt == 2'd3 && cap_len == 6'd47) begin
							sl_cap_end <= 1'b1;
							sl_is_idr  <= sl_idr_r;
							sl_nal_ref_idc_nonzero <= sl_ref_r;
							sl_done    <= 1'b1;
						end
					end
				end else if (rd_data == 8'h01 && zrun >= 2'd2) begin
					if (cap_tgt == 2'd1) sps_cap_end <= 1'b1;
					else if (cap_tgt == 2'd2) pps_cap_end <= 1'b1;
					else if (cap_tgt == 2'd3 && !sl_done) begin
						sl_cap_end <= 1'b1;
						sl_is_idr  <= sl_idr_r;
						sl_nal_ref_idc_nonzero <= sl_ref_r;
					end
					cap_tgt   <= 2'd0;
					sl_done   <= 0;
					epb_z     <= 0;
					cap_len   <= 0;
					pend_type <= 1'b1;
					zrun      <= 0;
				end else begin
					if (can_store) begin
						if (rd_data == 8'h03 && epb_z >= 2'd2) begin
							epb_z <= 0; // skip EPB
						end else begin
							if (cap_tgt == 2'd1) begin
								sps_cap_en <= 1'b1; sps_cap_data <= rd_data;
							end else if (cap_tgt == 2'd2) begin
								pps_cap_en <= 1'b1; pps_cap_data <= rd_data;
							end else begin
								sl_cap_en <= 1'b1; sl_cap_data <= rd_data;
							end
							cap_len <= cap_len + 1'd1;
							epb_z <= 0;
							if (cap_tgt == 2'd3 && cap_len == 6'd47) begin
								sl_cap_end <= 1'b1;
								sl_is_idr  <= sl_idr_r;
								sl_nal_ref_idc_nonzero <= sl_ref_r;
								sl_done    <= 1'b1;
							end
						end
					end
					zrun <= 0;
				end
			end

			data_valid <= rd_en && !rd_empty;
		end
	end

endmodule
