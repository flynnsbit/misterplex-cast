# MiSTerPlex top-level Makefile
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
CXX  ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -I$(ROOT)/host
FFMPEG_CFLAGS := $(shell pkg-config --cflags libavformat libavcodec libavutil 2>/dev/null)
FFMPEG_LIBS   := $(shell pkg-config --libs libavformat libavcodec libavutil 2>/dev/null)

.PHONY: all preflight unit unit-unlocked unit-host unit-host-unlocked core-gate-e2e core-gate-mutants resolve-fps-replay rtl-sim rtl-lint quartus-sv-subset define-parity post-fit-hierarchy post-fit-timing timing-exclusion pms-baseline-check pms-nal-stats arm-plexd arm-plexd-daemon arm-plexd-tools arm-ffmpeg arm-ffmpeg-static-check arm-ddr-bench arm-pl330-bench arm-profile-tools ddr-bench profile-tools present-harness clean help plexd package h264-golden-tools glass-baseline glass-baseline-policy v3-stable-320 v3-stable-320-policy plexui_shot

all: unit

help:
	@echo "Targets:"
	@echo "  make unit       - serialized host unit tests with resource backoff (cadence, resolve, companion HTTP)"
	@echo "  make resolve-fps-replay - offline fake-PMS resolve replay, f6526d96 vs this tree"
	@echo "  make rtl-sim    - run real Verilator RTL simulations if Verilator is installed"
	@echo "  make rtl-lint   - run Verilator parse/lint width/implicit regression gate (not Quartus synthesis)"
	@echo "  make quartus-sv-subset - curated Quartus SV subset guard with toolchain presence probe"
	@echo "  make define-parity - verify Quartus product macros match Verilator/lint macros"
	@echo "  make post-fit-hierarchy FIT_RPT=... [MAP_RPT=...] [COMPILE_LOG=...] - critical fitted-module guard"
	@echo "  make post-fit-timing STA_RPT=... - fail negative Quartus timing slack"
	@echo "  make timing-exclusion [STA_RPT=...] - detect timing closed by exclusion not design"
	@echo "  make pms-baseline-check - live PMS profile, AU, syntax and cadence guard (requires PLEX_BASE/TOKEN/KEY)"
	@echo "  make pms-nal-stats      - live PMS NAL size/jitter probe (requires PLEX_BASE/TOKEN/KEY)"
	@echo "  make glass-baseline-policy - offline glass floor pair.json + artifact md5s"
	@echo "  make glass-baseline - live HDMI floor (pin v0.2.0 pair unless GLASS_CANDIDATE=1)"
	@echo "  make v3-stable-320-policy - offline v0.3 lab-stable pair freeze"
	@echo "  make v3-stable-320 - isolated v0.3@320 deploy + chevron/B6 (not product H5)"
	@echo "  make h264-golden-tools - build shared H.264 golden fixture extractor"
	@echo "  make arm-plexd  - cross-build ARM misterplexd (if toolchain present)"
	@echo "  make arm-plexd-daemon - build only ARM_PLEXD_OUTPUT, without auxiliary binaries"
	@echo "  make arm-ffmpeg - build static-safe ARM libav from existing FFmpeg 8.1.2 source"
	@echo "  make arm-ffmpeg-static-check - reject iconv/module loaders or missing media support"
	@echo "  make build-rbf  - build Plex.rbf via scripts/build_rbf.sh (long)"
	@echo "  make unit-host  - host-only unit gate from a clean checkout, SLOT-FORM only (docs/progress/UNIT-HOST.md)"
	@echo "  make test       - alias for unit"
	@echo "  make package    - dist tarball (ARM + conf + docs + named RBF pairs)"
	@echo "  make arm-ddr-bench - cross-build DDR write microbenchmark"
	@echo "  make arm-pl330-bench - cross-build PL330 ingest bench (lab DMA diag)"
	@echo "  make arm-profile-tools - cross-build ARM decode/profile probes"
	@echo "  make present-harness - build offline present-loop pipe/copy harness"

test: unit

UNIT_ANNEXB := $(ROOT)/build/plex_real_baseline.264

preflight: $(ROOT)/scripts/test_resource_preflight.sh
	@bash $(ROOT)/scripts/test_resource_preflight.sh

unit:
	@bash $(ROOT)/scripts/run_with_resource_preflight.sh -- $(MAKE) unit-unlocked

UNIT_PREREQS = $(ROOT)/build/test_gdm_filter $(ROOT)/build/test_cast_av_480p_policy $(ROOT)/build/test_p720_audio_keepup $(ROOT)/build/test_p720_transcode_vf $(ROOT)/build/test_transcode_offset $(ROOT)/build/test_spi_txn_complete $(ROOT)/build/test_idle_poll_budget $(ROOT)/build/test_p720_e2e_budget $(ROOT)/build/test_pl330_encode $(ROOT)/build/test_cadence $(ROOT)/build/test_pts_cadence $(ROOT)/build/test_avclock $(ROOT)/build/test_mraudio_status $(ROOT)/build/test_audio_buf_prefill $(ROOT)/build/test_osd_menu $(ROOT)/build/test_plexob_yc $(ROOT)/build/test_playback_overlay $(ROOT)/build/test_overlay_onchange $(ROOT)/build/test_input_mailbox $(ROOT)/build/test_plx_page $(ROOT)/build/input_mailbox_probe_host $(ROOT)/build/test_pixel_format $(ROOT)/build/test_main_guard $(ROOT)/build/test_d52_log $(ROOT)/build/test_ddr_frame_wc $(ROOT)/build/test_core_gate $(ROOT)/build/test_fake_core_caps $(ROOT)/build/test_status_telemetry $(ROOT)/build/test_status_osd_v3 $(ROOT)/build/test_fpga_build_ids $(ROOT)/build/test_sps_relax_gate $(ROOT)/build/test_status_heartbeat $(ROOT)/build/test_wedge_read $(ROOT)/build/test_gate_caps $(ROOT)/build/test_gate_owner $(ROOT)/build/test_gate_error_render $(ROOT)/build/test_idle_pairing_error $(ROOT)/build/test_resolve $(ROOT)/build/test_pms_timeline $(ROOT)/build/test_companion_plant_seek $(ROOT)/build/test_companion_480i_field_rate $(ROOT)/build/test_pts_jump_clamp $(ROOT)/build/test_ttfp_stamps $(ROOT)/build/test_480i_active_height $(ROOT)/build/test_core_id_select $(ROOT)/build/test_sps_reject_high $(ROOT)/build/pms_baseline_probe $(ROOT)/build/test_h264_bitstream_source $(ROOT)/build/test_frame_store_math $(ROOT)/build/test_frame_store_sdram_sim $(ROOT)/build/test_frame_store_ddr_prefetch_sim $(ROOT)/build/test_sdram_memtest_sim $(ROOT)/build/test_sdram_mailbox $(ROOT)/build/test_annexb_count $(ROOT)/build/test_sps_parse $(ROOT)/build/test_slice_hdr $(ROOT)/build/test_cavlc_dc $(ROOT)/build/test_idct_quant $(ROOT)/build/test_p3_host_recon_vectors $(ROOT)/build/test_p3_idct_reference_model $(ROOT)/build/test_p3_inter_pred_vectors $(ROOT)/build/extract_h264_golden $(ROOT)/build/test_idle_fb_map $(ROOT)/build/test_idle_fb_map.mutant-check $(ROOT)/build/test_owenb_ring $(ROOT)/build/test_owenb_ring.mutant-check $(ROOT)/build/test_owenb_core_gate $(ROOT)/build/test_owenb_core_gate.mutant-check $(ROOT)/build/test_owenb_session $(ROOT)/build/test_owenb_session.mutant-check $(ROOT)/build/test_owenb_play $(ROOT)/build/test_owenb_timeline_heartbeat $(ROOT)/build/test_queue_preroll $(ROOT)/build/test_queue_preroll.mutant-check $(ROOT)/build/test_companion_queue_version $(ROOT)/build/test_owenb_weak_profile $(ROOT)/build/test_owenb_weak_profile.mutant-check $(ROOT)/build/test_owenb_pms_traits $(ROOT)/build/test_owenb_pms_traits.mutant-check $(ROOT)/build/test_owenb_overlay $(ROOT)/build/test_owenb_aspect $(ROOT)/build/test_owenb_core_select $(ROOT)/build/test_owenb_osd $(ROOT)/build/test_owenb_osd.mutant-check $(ROOT)/build/test_owenb_osd.ovl-ref-check $(ROOT)/build/test_owenb_fbcon $(ROOT)/build/test_owenb_fbcon.mutant-check $(ROOT)/build/test_owenb_input $(ROOT)/build/test_owenb_input.mutant-check

unit-unlocked: preflight $(UNIT_PREREQS)
	python3 $(ROOT)/tests/unit/test_unit_host_coverage.py
	@python3 $(ROOT)/tests/unit/test_script_modes.py
	$(ROOT)/build/test_gdm_filter
	$(ROOT)/tests/unit/test_cast_http_ready_policy.sh
	python3 $(ROOT)/tests/unit/test_candidate_deploy.py
	$(ROOT)/tests/unit/test_deploy_misterplexd_guard.sh
	python3 $(ROOT)/tests/unit/test_arm_ffmpeg_static.py
	$(ROOT)/build/test_spi_txn_complete
	$(ROOT)/build/test_idle_poll_budget
	$(ROOT)/build/test_p720_e2e_budget
	$(ROOT)/build/test_p720_audio_keepup
	bash $(ROOT)/tests/unit/test_p720_av_match_480p_policy.sh
	$(ROOT)/build/test_p720_transcode_vf
	$(ROOT)/build/test_transcode_offset
	bash $(ROOT)/tests/unit/test_pair_conf_policy.sh
	bash $(ROOT)/tests/unit/test_legacy_core_prefix.sh
	$(ROOT)/build/test_pl330_encode
	python3 $(ROOT)/tests/unit/test_p720_shared_bw_contract.py
	bash $(ROOT)/tests/unit/test_idle_thread_budget_gate.sh
	bash $(ROOT)/tests/unit/test_io_ack_follow_rtl_sim.sh
	bash $(ROOT)/tests/unit/test_bitstream_bit_feeder_rtl_sim.sh
	bash $(ROOT)/tests/unit/test_bitstream_to_exp_golomb_rtl_sim.sh
	bash $(ROOT)/tests/unit/test_annexb_rbsp_exp_golomb_rtl_sim.sh
	$(ROOT)/build/test_cadence
	$(ROOT)/build/test_pts_cadence
	$(ROOT)/build/test_avclock
	$(ROOT)/build/test_cast_av_480p_policy
	bash $(ROOT)/tests/unit/test_cast_av_480p_policy.sh
	$(ROOT)/build/test_mraudio_status
	$(ROOT)/build/test_audio_buf_prefill
	bash $(ROOT)/tests/unit/test_audio_session_host.sh
	bash $(ROOT)/tests/unit/test_audio_session.sh
	python3 $(ROOT)/tests/unit/test_fpga_audio_routing.py
	bash $(ROOT)/tests/unit/test_ddr_bitstream_reader_transport.sh
	bash $(ROOT)/tests/unit/test_fpga_video_audio_feedback.sh
	$(ROOT)/build/test_osd_menu
	$(ROOT)/build/test_plexob_yc
	$(ROOT)/build/test_playback_overlay
	$(ROOT)/build/test_overlay_onchange
	bash $(ROOT)/tests/unit/test_fpga_playback_overlay.sh
	$(ROOT)/build/test_input_mailbox
	$(ROOT)/build/test_plx_page
	bash $(ROOT)/tests/unit/test_plx_page_scripts.sh
	python3 $(ROOT)/tests/unit/test_build_table_twins.py
	$(ROOT)/build/test_pixel_format
	$(ROOT)/build/test_main_guard
	$(ROOT)/build/test_ddr_frame_wc
	$(ROOT)/build/test_d52_log
	python3 $(ROOT)/tests/unit/test_live_glass_startup_order.py
	$(ROOT)/build/test_idle_fb_map
	$(ROOT)/build/test_r1_plxh
	python3 $(ROOT)/tests/unit/test_mplex_abi_sync.py $(ROOT)
	$(ROOT)/build/test_core_gate
	$(ROOT)/build/test_fake_core_caps
	$(ROOT)/build/test_mast_reload
	bash $(ROOT)/tests/unit/test_mast_reload_launcher.sh
	bash $(ROOT)/tests/unit/test_core_gate_base_diff.sh
	$(ROOT)/build/test_status_telemetry
	$(ROOT)/build/test_status_osd_v3
	$(ROOT)/build/test_fpga_build_ids
	$(ROOT)/build/test_sps_relax_gate
	$(ROOT)/build/test_status_heartbeat
	$(ROOT)/build/test_wedge_read
	$(ROOT)/build/test_gate_caps
	CXX="$(CXX)" CXXFLAGS="$(CXXFLAGS)" bash $(ROOT)/tests/unit/test_gate_caps_mutants.sh
	$(ROOT)/build/test_gate_owner
	CXX="$(CXX)" CXXFLAGS="$(CXXFLAGS)" bash $(ROOT)/tests/unit/test_gate_owner_mutants.sh
	$(ROOT)/build/test_gate_error_render
	$(ROOT)/build/test_idle_pairing_error
	$(if $(COMPRESSED_SYNTAX_OK),$(ROOT)/build/test_compressed_syntax $(ROOT),@echo "SKIP test_compressed_syntax (no FFmpeg dev libs)")
	$(if $(COMPRESSED_SYNTAX_OK),CXX="$(CXX)" CXXFLAGS="$(CXXFLAGS)" bash $(ROOT)/tests/unit/test_compressed_syntax_mutants.sh,@echo "SKIP test_compressed_syntax_mutants (no FFmpeg dev libs)")
	$(ROOT)/build/test_source_aspect_dar
	$(if $(AVUTIL_LIBS),$(ROOT)/build/test_source_aspect_dar_avutil,@echo "SKIP test_source_aspect_dar_avutil (no libavutil)")
	$(ROOT)/build/test_l2_transcode_dar
	$(if $(L2_DEMUX_OK),bash $(ROOT)/tests/unit/test_l2_transcode_dar_demux.sh,@echo "SKIP test_l2_transcode_dar_demux (no FFmpeg dev libs or ffmpeg)")
	$(ROOT)/build/test_library_browser
	$(ROOT)/build/test_browse_v2 $(ROOT)/tests/unit/fake_browse_server.py
	$(ROOT)/build/test_browse_library_list
	python3 $(ROOT)/tests/unit/test_library_parse_corpus.py
	python3 $(ROOT)/tests/unit/test_core_gate_mutants_audit.py
	python3 $(ROOT)/tools/gcast/test_gcast_xcode.py
	python3 $(ROOT)/tests/unit/test_gcast_hls_level_mutant.py
	$(ROOT)/build/test_resolve
	$(ROOT)/build/test_pms_timeline
	$(ROOT)/build/test_companion_plant_seek
	$(ROOT)/build/test_companion_480i_field_rate
	$(ROOT)/build/test_sps_reject_high
	$(ROOT)/build/test_companion_letterbox_bars
	$(ROOT)/build/test_companion_live_clock
	$(ROOT)/build/test_title_direct_policy
	$(ROOT)/build/test_pts_jump_clamp
	$(ROOT)/build/test_ttfp_stamps
	$(ROOT)/build/test_480i_active_height
	$(ROOT)/build/test_core_id_select
	python3 $(ROOT)/tests/unit/test_shared_letterbox_crop_apply.py
	python3 $(ROOT)/tests/unit/test_audio_pts_bind_480i.py
	python3 $(ROOT)/tests/unit/test_last_frame_hold_static.py
	python3 $(ROOT)/tests/unit/test_ttfp_breakdown_log.py
	python3 $(ROOT)/tests/unit/test_240p_active_height.py
	python3 $(ROOT)/tests/unit/test_480p_active_height.py
	python3 $(ROOT)/tests/unit/test_letterbox_bars.py
	python3 $(ROOT)/tests/unit/test_audio_pts_bind_fpga.py
	python3 $(ROOT)/tests/unit/test_fpga_av_start_gate.py
	python3 $(ROOT)/tests/unit/test_batch_review_pins.py
	python3 $(ROOT)/tests/unit/test_companion_timeline_tick.py
	$(ROOT)/build/test_pts_wrap
	$(ROOT)/build/test_audio_lead_clamp
	$(ROOT)/build/test_audio_queue_bound_240p
	$(ROOT)/build/test_sps_reject
	$(ROOT)/build/test_bitrate_floor_480i
	$(ROOT)/build/test_companion_title_policy
	bash $(ROOT)/tests/unit/test_respawn_resume_static.sh
	$(ROOT)/build/test_companion_natural_eof
	$(ROOT)/build/test_companion_reqlog
	$(ROOT)/build/test_input_close_eof
	$(ROOT)/build/test_fpga_av_trace
	$(ROOT)/build/test_fpga_av_trace_off
	$(ROOT)/tests/unit/test_pms_baseline_gate.sh
	bash $(ROOT)/tests/unit/test_pms_baseline_live_gate.sh
	bash $(ROOT)/tests/unit/test_pms_nal_stats_gate.sh
	$(ROOT)/build/test_h264_bitstream_source
	$(ROOT)/build/test_owenb_ring
	$(ROOT)/build/test_owenb_core_gate
	$(ROOT)/build/test_owenb_session
	$(ROOT)/build/test_owenb_play
	$(ROOT)/build/test_owenb_timeline_heartbeat
	$(ROOT)/build/test_queue_preroll
	$(ROOT)/build/test_companion_queue_version
	$(ROOT)/build/test_owenb_weak_profile
	$(ROOT)/build/test_owenb_overlay
	$(ROOT)/build/test_owenb_aspect
	$(ROOT)/build/test_owenb_core_select
	$(ROOT)/build/test_owenb_osd
	$(ROOT)/build/test_owenb_fbcon
	$(ROOT)/build/test_owenb_input
	$(if $(OWENB_AV_E2E_OK),$(ROOT)/build/test_owenb_av_e2e,@echo "SKIP test_owenb_av_e2e (no FFmpeg dev libs)")
	$(ROOT)/build/test_frame_store_math
	$(ROOT)/build/test_frame_store_sdram_sim
	$(ROOT)/build/test_frame_store_ddr_prefetch_sim
	$(ROOT)/build/test_sdram_memtest_sim
	$(ROOT)/build/test_sdram_mailbox
	$(ROOT)/build/test_annexb_count
	@mkdir -p $(ROOT)/build
	@python3 $(ROOT)/scripts/gen_test_annexb_real.py $(UNIT_ANNEXB)
	$(ROOT)/build/test_status_telemetry $(UNIT_ANNEXB)
	$(ROOT)/build/test_sps_parse $(UNIT_ANNEXB)
	$(ROOT)/build/test_slice_hdr $(UNIT_ANNEXB)
	$(ROOT)/build/test_cavlc_dc $(UNIT_ANNEXB)
	$(ROOT)/build/test_idct_quant $(UNIT_ANNEXB)
	$(ROOT)/build/test_p3_host_recon_vectors
	$(ROOT)/tests/unit/test_h264_golden_extractor.sh
	$(ROOT)/tests/unit/test_h264_frame_plane_goldens.sh
	$(ROOT)/tests/unit/test_i420_candidate_score.sh
	$(ROOT)/tests/unit/test_h264_multinal_stream_path.sh
	$(ROOT)/build/test_p3_idct_reference_model
	$(ROOT)/build/test_p3_inter_pred_vectors
	python3 $(ROOT)/tests/unit/test_no_conflict_markers.py
	python3 $(ROOT)/tests/unit/test_bench_rtl_filelists.py
	python3 $(ROOT)/tests/unit/test_present_720p_land_static.py
	python3 $(ROOT)/tests/unit/test_present_720p_l4_static.py
	python3 $(ROOT)/tests/unit/test_present_geom_params_static.py
	$(ROOT)/tests/unit/test_present_geom_params_rtl_sim.sh
	python3 $(ROOT)/tests/unit/test_present_720p_store_wire_static.py
	$(ROOT)/tests/unit/test_present_720p_store_wire_rtl_sim.sh
	python3 $(ROOT)/tests/unit/test_720p_clk_ddr_arith.py
	python3 $(ROOT)/tests/unit/test_p3_high_cabac_scope.py
	python3 $(ROOT)/tests/unit/test_p3_intra_mb0_verilator.py
	python3 $(ROOT)/tests/unit/test_h264_intra_nb_ctx_verilator.py
	python3 $(ROOT)/tests/unit/test_p3_idct_rtl_model.py
	python3 $(ROOT)/tests/unit/test_p3_intra_frame_verilator.py
	$(ROOT)/tests/unit/test_p3_inter_rtl_sim.sh
	$(ROOT)/tests/unit/test_p3_dpb_mc_rtl_sim.sh
	$(ROOT)/tests/unit/test_h264_p_slice_modes_rtl_sim.sh
	$(ROOT)/tests/unit/test_p3_inter_stream_path_rtl_sim.sh
	$(ROOT)/tests/unit/test_companion_http.sh
	$(ROOT)/tests/unit/test_companion_hold.sh
	$(ROOT)/tests/unit/test_plex_browse.sh
	$(ROOT)/tests/unit/test_play_file_delivery.sh
	$(ROOT)/tests/unit/test_no_private_data.sh
	$(ROOT)/tests/unit/test_capture_rig.sh
	$(ROOT)/tests/unit/test_resource_preflight.sh
	$(ROOT)/scripts/check_define_parity.py
	python3 $(ROOT)/tests/unit/test_hw_visual_compare.py
	$(ROOT)/tests/unit/test_decode_throughput_gate.sh
	$(ROOT)/tests/unit/test_rtl_invariants.sh
	$(ROOT)/tests/unit/test_mister_ini_plex_guard.sh
	python3 $(ROOT)/tests/unit/test_plexob_emu_wiring.py
	$(ROOT)/tests/unit/test_confstr_guard.sh
	$(ROOT)/tests/unit/test_release_rbf_hash.sh
	$(ROOT)/tests/unit/test_glass_baseline_pair_policy.sh
	$(ROOT)/tests/unit/test_v030_lab_stable_policy.sh
	$(ROOT)/tests/unit/test_sdram_startup_verilator.sh
	$(ROOT)/tests/unit/test_sdram_dq_turnaround_verilator.sh
	$(ROOT)/tests/unit/test_h264_cavlc_residual_verilator.sh
	$(ROOT)/tests/unit/test_level_width_verilator.sh
	$(ROOT)/tests/unit/test_stream_path_recon_integration.sh
	$(ROOT)/tests/unit/test_stream_path_full_frame_compare.sh
	python3 $(ROOT)/tests/parse_res_csum_status.py --self-test
	$(ROOT)/tests/unit/test_p3_idct_rtl_sim.sh
	$(ROOT)/tests/unit/test_p3_deblock_rtl_sim.sh
	$(ROOT)/tests/unit/test_p3_stream_path_recon_rtl_sim.sh
	$(ROOT)/tests/unit/test_stream_path_deblock_integration.sh
	bash $(ROOT)/tests/unit/test_stream_path_ddr_ring_integration.sh
	$(ROOT)/tests/unit/test_ddr_frame_store_warm_reset.sh
	python3 $(ROOT)/tests/unit/test_rtl_lint_stubs.py
	$(ROOT)/scripts/rtl_lint.py
	$(ROOT)/tests/unit/test_h264_syntax_primitives_rtl_sim.sh
	$(ROOT)/tests/unit/test_h264_sps_geometry_rtl_sim.sh
	$(ROOT)/tests/unit/test_h264_baseline_syntax_rtl_sim.sh
	python3 $(ROOT)/tests/unit/test_lock_paths_needle.py
	bash $(ROOT)/tests/unit/test_lock_paths.sh

unit-host:
	@self=$$(readlink /proc/self/ns/pid); one=$$(readlink /proc/1/ns/pid 2>/dev/null); \
	if [ "$$self" = "pid:[4026531836]" ] || [ "$$one" != "$$self" ] || [ -e /tmp/misterplexd.lock ] || [ "$$(id -u)" = 0 ]; then \
		echo "unit-host: run it only in the SLOT-FORM nested form (heavy-sim-slot; outer unshare --user --map-root-user" >&2; \
		echo "  --pid --fork --mount-proc [--net]; a private tmpfs /tmp; inner unshare --user --map-user=UID --map-group=GID)." >&2; \
		echo "  Refused: pid ns $$self, /proc/1 ns $$one, host /tmp/misterplexd.lock visible: $$([ -e /tmp/misterplexd.lock ] && echo yes || echo no), uid $$(id -u) (0 = a mapped root: permission tests such as test_lock_paths T1e go RED)." >&2; \
		echo "  See docs/progress/UNIT-HOST.md." >&2; exit 2; \
	fi
	@bash $(ROOT)/scripts/run_with_resource_preflight.sh -- $(MAKE) unit-host-unlocked

unit-host-unlocked: preflight $(UNIT_PREREQS)
	python3 $(ROOT)/tests/unit/test_unit_host_coverage.py
	@python3 $(ROOT)/tests/unit/test_script_modes.py
	$(ROOT)/build/test_gdm_filter
	$(ROOT)/tests/unit/test_cast_http_ready_policy.sh
	python3 $(ROOT)/tests/unit/test_candidate_deploy.py
	$(ROOT)/tests/unit/test_deploy_misterplexd_guard.sh
	python3 $(ROOT)/tests/unit/test_arm_ffmpeg_static.py
	$(ROOT)/build/test_spi_txn_complete
	$(ROOT)/build/test_idle_poll_budget
	$(ROOT)/build/test_p720_e2e_budget
	$(ROOT)/build/test_p720_audio_keepup
	bash $(ROOT)/tests/unit/test_p720_av_match_480p_policy.sh
	$(ROOT)/build/test_p720_transcode_vf
	$(ROOT)/build/test_transcode_offset
	bash $(ROOT)/tests/unit/test_pair_conf_policy.sh
	bash $(ROOT)/tests/unit/test_legacy_core_prefix.sh
	$(ROOT)/build/test_pl330_encode
	python3 $(ROOT)/tests/unit/test_p720_shared_bw_contract.py
	bash $(ROOT)/tests/unit/test_idle_thread_budget_gate.sh
	$(ROOT)/build/test_cadence
	$(ROOT)/build/test_pts_cadence
	$(ROOT)/build/test_avclock
	$(ROOT)/build/test_cast_av_480p_policy
	bash $(ROOT)/tests/unit/test_cast_av_480p_policy.sh
	$(ROOT)/build/test_mraudio_status
	$(ROOT)/build/test_audio_buf_prefill
	bash $(ROOT)/tests/unit/test_audio_session_host.sh
	$(ROOT)/build/test_osd_menu
	$(ROOT)/build/test_plexob_yc
	$(ROOT)/build/test_playback_overlay
	$(ROOT)/build/test_overlay_onchange
	$(ROOT)/build/test_input_mailbox
	$(ROOT)/build/test_plx_page
	bash $(ROOT)/tests/unit/test_plx_page_scripts.sh
	python3 $(ROOT)/tests/unit/test_build_table_twins.py
	$(ROOT)/build/test_pixel_format
	$(ROOT)/build/test_main_guard
	$(ROOT)/build/test_ddr_frame_wc
	$(ROOT)/build/test_d52_log
	python3 $(ROOT)/tests/unit/test_live_glass_startup_order.py
	$(ROOT)/build/test_idle_fb_map
	$(ROOT)/build/test_r1_plxh
	python3 $(ROOT)/tests/unit/test_mplex_abi_sync.py $(ROOT)
	$(ROOT)/build/test_core_gate
	$(ROOT)/build/test_fake_core_caps
	$(ROOT)/build/test_mast_reload
	bash $(ROOT)/tests/unit/test_mast_reload_launcher.sh
	bash $(ROOT)/tests/unit/test_core_gate_base_diff.sh
	$(ROOT)/build/test_status_telemetry
	$(ROOT)/build/test_status_osd_v3
	$(ROOT)/build/test_fpga_build_ids
	$(ROOT)/build/test_sps_relax_gate
	$(ROOT)/build/test_status_heartbeat
	$(ROOT)/build/test_wedge_read
	$(ROOT)/build/test_gate_caps
	CXX="$(CXX)" CXXFLAGS="$(CXXFLAGS)" bash $(ROOT)/tests/unit/test_gate_caps_mutants.sh
	$(ROOT)/build/test_gate_owner
	CXX="$(CXX)" CXXFLAGS="$(CXXFLAGS)" bash $(ROOT)/tests/unit/test_gate_owner_mutants.sh
	$(ROOT)/build/test_gate_error_render
	$(ROOT)/build/test_idle_pairing_error
	$(if $(COMPRESSED_SYNTAX_OK),$(ROOT)/build/test_compressed_syntax $(ROOT),@echo "SKIP test_compressed_syntax (no FFmpeg dev libs)")
	$(if $(COMPRESSED_SYNTAX_OK),CXX="$(CXX)" CXXFLAGS="$(CXXFLAGS)" bash $(ROOT)/tests/unit/test_compressed_syntax_mutants.sh,@echo "SKIP test_compressed_syntax_mutants (no FFmpeg dev libs)")
	$(ROOT)/build/test_source_aspect_dar
	$(if $(AVUTIL_LIBS),$(ROOT)/build/test_source_aspect_dar_avutil,@echo "SKIP test_source_aspect_dar_avutil (no libavutil)")
	$(ROOT)/build/test_l2_transcode_dar
	$(if $(L2_DEMUX_OK),bash $(ROOT)/tests/unit/test_l2_transcode_dar_demux.sh,@echo "SKIP test_l2_transcode_dar_demux (no FFmpeg dev libs or ffmpeg)")
	$(ROOT)/build/test_library_browser
	$(ROOT)/build/test_browse_v2 $(ROOT)/tests/unit/fake_browse_server.py
	$(ROOT)/build/test_browse_library_list
	python3 $(ROOT)/tests/unit/test_library_parse_corpus.py
	python3 $(ROOT)/tests/unit/test_core_gate_mutants_audit.py
	python3 $(ROOT)/tools/gcast/test_gcast_xcode.py
	python3 $(ROOT)/tests/unit/test_gcast_hls_level_mutant.py
	$(ROOT)/build/test_resolve
	$(ROOT)/build/test_pms_timeline
	$(ROOT)/build/test_companion_plant_seek
	$(ROOT)/build/test_companion_480i_field_rate
	$(ROOT)/build/test_sps_reject_high
	$(ROOT)/build/test_companion_letterbox_bars
	$(ROOT)/build/test_companion_live_clock
	$(ROOT)/build/test_title_direct_policy
	$(ROOT)/build/test_pts_jump_clamp
	$(ROOT)/build/test_ttfp_stamps
	$(ROOT)/build/test_480i_active_height
	$(ROOT)/build/test_core_id_select
	python3 $(ROOT)/tests/unit/test_shared_letterbox_crop_apply.py
	python3 $(ROOT)/tests/unit/test_audio_pts_bind_480i.py
	python3 $(ROOT)/tests/unit/test_last_frame_hold_static.py
	python3 $(ROOT)/tests/unit/test_ttfp_breakdown_log.py
	python3 $(ROOT)/tests/unit/test_240p_active_height.py
	python3 $(ROOT)/tests/unit/test_480p_active_height.py
	python3 $(ROOT)/tests/unit/test_letterbox_bars.py
	python3 $(ROOT)/tests/unit/test_audio_pts_bind_fpga.py
	python3 $(ROOT)/tests/unit/test_fpga_av_start_gate.py
	python3 $(ROOT)/tests/unit/test_batch_review_pins.py
	python3 $(ROOT)/tests/unit/test_companion_timeline_tick.py
	$(ROOT)/build/test_pts_wrap
	$(ROOT)/build/test_audio_lead_clamp
	$(ROOT)/build/test_audio_queue_bound_240p
	$(ROOT)/build/test_sps_reject
	$(ROOT)/build/test_bitrate_floor_480i
	$(ROOT)/build/test_companion_title_policy
	bash $(ROOT)/tests/unit/test_respawn_resume_static.sh
	$(ROOT)/build/test_companion_natural_eof
	$(ROOT)/build/test_companion_reqlog
	$(ROOT)/build/test_input_close_eof
	$(ROOT)/build/test_fpga_av_trace
	$(ROOT)/build/test_fpga_av_trace_off
	$(ROOT)/tests/unit/test_pms_baseline_gate.sh
	bash $(ROOT)/tests/unit/test_pms_nal_stats_gate.sh
	$(ROOT)/build/test_h264_bitstream_source
	$(ROOT)/build/test_owenb_ring
	$(ROOT)/build/test_owenb_core_gate
	$(ROOT)/build/test_owenb_session
	$(ROOT)/build/test_owenb_play
	$(ROOT)/build/test_owenb_timeline_heartbeat
	$(ROOT)/build/test_queue_preroll
	$(ROOT)/build/test_companion_queue_version
	$(ROOT)/build/test_owenb_weak_profile
	$(ROOT)/build/test_owenb_overlay
	$(ROOT)/build/test_owenb_aspect
	$(ROOT)/build/test_owenb_core_select
	$(ROOT)/build/test_owenb_osd
	$(ROOT)/build/test_owenb_fbcon
	$(ROOT)/build/test_owenb_input
	$(if $(OWENB_AV_E2E_OK),$(ROOT)/build/test_owenb_av_e2e,@echo "SKIP test_owenb_av_e2e (no FFmpeg dev libs)")
	$(ROOT)/build/test_frame_store_math
	$(ROOT)/build/test_frame_store_sdram_sim
	$(ROOT)/build/test_frame_store_ddr_prefetch_sim
	$(ROOT)/build/test_sdram_memtest_sim
	$(ROOT)/build/test_sdram_mailbox
	$(ROOT)/build/test_annexb_count
	@mkdir -p $(ROOT)/build
	@python3 $(ROOT)/scripts/gen_test_annexb_real.py $(UNIT_ANNEXB)
	$(ROOT)/build/test_status_telemetry $(UNIT_ANNEXB)
	$(ROOT)/build/test_sps_parse $(UNIT_ANNEXB)
	$(ROOT)/build/test_slice_hdr $(UNIT_ANNEXB)
	$(ROOT)/build/test_cavlc_dc $(UNIT_ANNEXB)
	$(ROOT)/build/test_idct_quant $(UNIT_ANNEXB)
	$(ROOT)/build/test_p3_host_recon_vectors
	$(ROOT)/tests/unit/test_h264_golden_extractor.sh
	$(ROOT)/tests/unit/test_h264_frame_plane_goldens.sh
	$(ROOT)/tests/unit/test_i420_candidate_score.sh
	$(ROOT)/build/test_p3_idct_reference_model
	$(ROOT)/build/test_p3_inter_pred_vectors
	python3 $(ROOT)/tests/unit/test_no_conflict_markers.py
	python3 $(ROOT)/tests/unit/test_present_720p_land_static.py
	python3 $(ROOT)/tests/unit/test_present_720p_l4_static.py
	python3 $(ROOT)/tests/unit/test_present_geom_params_static.py
	python3 $(ROOT)/tests/unit/test_present_720p_store_wire_static.py
	python3 $(ROOT)/tests/unit/test_720p_clk_ddr_arith.py
	python3 $(ROOT)/tests/unit/test_p3_high_cabac_scope.py
	python3 $(ROOT)/tests/unit/test_p3_idct_rtl_model.py
	$(ROOT)/tests/unit/test_companion_http.sh
	$(ROOT)/tests/unit/test_companion_hold.sh
	$(ROOT)/tests/unit/test_plex_browse.sh
	$(ROOT)/tests/unit/test_play_file_delivery.sh
	$(ROOT)/tests/unit/test_no_private_data.sh
	$(ROOT)/tests/unit/test_capture_rig.sh
	$(ROOT)/tests/unit/test_resource_preflight.sh
	$(ROOT)/scripts/check_define_parity.py
	python3 $(ROOT)/tests/unit/test_hw_visual_compare.py
	$(ROOT)/tests/unit/test_decode_throughput_gate.sh
	$(ROOT)/tests/unit/test_mister_ini_plex_guard.sh
	python3 $(ROOT)/tests/unit/test_plexob_emu_wiring.py
	$(ROOT)/tests/unit/test_confstr_guard.sh
	$(ROOT)/tests/unit/test_release_rbf_hash.sh
	$(ROOT)/tests/unit/test_glass_baseline_pair_policy.sh
	$(ROOT)/tests/unit/test_v030_lab_stable_policy.sh
	python3 $(ROOT)/tests/parse_res_csum_status.py --self-test
	python3 $(ROOT)/tests/unit/test_lock_paths_needle.py
	bash $(ROOT)/tests/unit/test_lock_paths.sh

rtl-sim:
	$(ROOT)/tests/unit/test_p3_idct_rtl_sim.sh
	$(ROOT)/tests/unit/test_p3_deblock_rtl_sim.sh
	bash $(ROOT)/tests/unit/test_stream_path_ddr_ring_integration.sh
	$(ROOT)/tests/unit/test_h264_cavlc_residual_verilator.sh
	$(ROOT)/tests/unit/test_p3_stream_path_recon_rtl_sim.sh
	$(ROOT)/tests/unit/test_stream_path_deblock_integration.sh
	$(ROOT)/tests/unit/test_ddr_frame_store_warm_reset.sh
	$(ROOT)/tests/unit/test_stream_path_recon_integration.sh
	$(ROOT)/tests/unit/test_stream_path_full_frame_compare.sh
	$(ROOT)/tests/unit/test_h264_syntax_primitives_rtl_sim.sh
	$(ROOT)/tests/unit/test_h264_sps_geometry_rtl_sim.sh
	$(ROOT)/tests/unit/test_h264_baseline_syntax_rtl_sim.sh
	$(ROOT)/tests/unit/test_h264_p_slice_modes_rtl_sim.sh
	$(ROOT)/tests/unit/test_p3_inter_rtl_sim.sh
	$(ROOT)/tests/unit/test_p3_dpb_mc_rtl_sim.sh
	$(ROOT)/tests/unit/test_p3_inter_stream_path_rtl_sim.sh

rtl-lint:
	python3 $(ROOT)/tests/unit/test_rtl_lint_stubs.py
	$(ROOT)/scripts/rtl_lint.py

quartus-sv-subset:
	$(ROOT)/scripts/check_quartus_sv_subset.py $$($(ROOT)/scripts/rtl_lint.py --list-files)

define-parity:
	$(ROOT)/scripts/check_define_parity.py

post-fit-hierarchy:
	@if [ -z "$(FIT_RPT)" ]; then echo "FIT_RPT is required" >&2; exit 2; fi
	$(ROOT)/scripts/check_quartus_fit_hierarchy.py --fit-rpt "$(FIT_RPT)" \
		$(if $(MAP_RPT),--map-rpt "$(MAP_RPT)",) \
		$(if $(COMPILE_LOG),--log "$(COMPILE_LOG)",)

post-fit-timing:
	@if [ -z "$(STA_RPT)" ]; then echo "STA_RPT is required" >&2; exit 2; fi
	$(ROOT)/scripts/check_quartus_timing.py --sta-rpt "$(STA_RPT)"

timing-exclusion:
	$(ROOT)/scripts/check_timing_exclusions.py $(if $(STA_RPT),--sta-rpt "$(STA_RPT)",)

pms-baseline-check: $(ROOT)/build/pms_baseline_probe
	$(ROOT)/tests/hw/test_pms_baseline_profile.sh

pms-nal-stats: $(ROOT)/build/pms_nal_stats
	bash $(ROOT)/tests/hw/test_pms_nal_stats.sh

h264-golden-tools: $(ROOT)/build/extract_h264_golden $(ROOT)/build/score_h264_native_frames $(ROOT)/build/analyze_h264_intra_mbs

BROWSE_RENDER_TESTS := \
	$(ROOT)/build/test_browse_render_rg1 \
	$(ROOT)/build/test_browse_paint_rg2 \
	$(ROOT)/build/test_browse_render_rg3 \
	$(ROOT)/build/test_browse_render_rg4 \
	$(ROOT)/build/test_browse_r1_adapter

unit-unlocked unit-host-unlocked: $(ROOT)/build/font_8x16.check $(BROWSE_RENDER_TESTS)

$(ROOT)/build/font_8x16.check: $(ROOT)/tools/bdf2hpp.py \
		$(ROOT)/third_party/spleen-2.2.0/spleen-8x16.bdf \
		$(ROOT)/third_party/spleen-2.2.0/LICENSE \
		$(ROOT)/arm/misterplexd/font_8x16.hpp
	@mkdir -p $(ROOT)/build
	python3 $(ROOT)/tools/bdf2hpp.py \
		--bdf $(ROOT)/third_party/spleen-2.2.0/spleen-8x16.bdf \
		--license $(ROOT)/third_party/spleen-2.2.0/LICENSE \
		--out $(ROOT)/build/font_8x16.hpp
	diff -u $(ROOT)/arm/misterplexd/font_8x16.hpp $(ROOT)/build/font_8x16.hpp
	@touch $@

$(ROOT)/build/test_browse_render_rg1: $(ROOT)/tests/unit/test_browse_render_rg1.cpp \
		$(ROOT)/tests/unit/browse_render_test_common.hpp \
		$(ROOT)/host/libmisterplex/browse_render.hpp \
		$(ROOT)/arm/misterplexd/font_8x16.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_browse_render_rg1.cpp -lz

$(ROOT)/build/test_browse_paint_rg2: $(ROOT)/tests/unit/test_browse_paint_rg2.cpp \
		$(ROOT)/tests/unit/browse_render_test_common.hpp \
		$(ROOT)/host/libmisterplex/browse_render.hpp \
		$(ROOT)/host/libmisterplex/browse_paint.hpp \
		$(ROOT)/host/libmisterplex/r1_plxh.hpp $(ROOT)/tests/unit/r1_plxh_rtl_model.hpp \
		$(ROOT)/arm/misterplexd/font_8x16.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_browse_paint_rg2.cpp -lz

$(ROOT)/build/test_browse_paint_rg2_mut_copy_dn_only: $(ROOT)/tests/unit/test_browse_paint_rg2.cpp \
		$(ROOT)/tests/unit/browse_render_test_common.hpp \
		$(ROOT)/host/libmisterplex/browse_render.hpp \
		$(ROOT)/host/libmisterplex/browse_paint.hpp \
		$(ROOT)/host/libmisterplex/r1_plxh.hpp $(ROOT)/tests/unit/r1_plxh_rtl_model.hpp \
		$(ROOT)/arm/misterplexd/font_8x16.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -DMUT_COPY_DN_ONLY -o $@ $(ROOT)/tests/unit/test_browse_paint_rg2.cpp -lz

$(ROOT)/build/test_browse_paint_rg2_mut_no_full_after_skip: $(ROOT)/tests/unit/test_browse_paint_rg2.cpp \
		$(ROOT)/tests/unit/browse_render_test_common.hpp \
		$(ROOT)/host/libmisterplex/browse_render.hpp \
		$(ROOT)/host/libmisterplex/browse_paint.hpp \
		$(ROOT)/host/libmisterplex/r1_plxh.hpp $(ROOT)/tests/unit/r1_plxh_rtl_model.hpp \
		$(ROOT)/arm/misterplexd/font_8x16.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -DMUT_NO_FULL_AFTER_SKIP -o $@ $(ROOT)/tests/unit/test_browse_paint_rg2.cpp -lz

$(ROOT)/build/test_browse_paint_rg2_mut_no_repoint_after_timeout: $(ROOT)/tests/unit/test_browse_paint_rg2.cpp \
		$(ROOT)/tests/unit/browse_render_test_common.hpp \
		$(ROOT)/host/libmisterplex/browse_render.hpp \
		$(ROOT)/host/libmisterplex/browse_paint.hpp \
		$(ROOT)/host/libmisterplex/r1_plxh.hpp $(ROOT)/tests/unit/r1_plxh_rtl_model.hpp \
		$(ROOT)/arm/misterplexd/font_8x16.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -DMUT_NO_REPOINT_AFTER_TIMEOUT -o $@ $(ROOT)/tests/unit/test_browse_paint_rg2.cpp -lz

# review 19:27 F5 on RG-2 (w9): the planner over the real PLXH stack must not paint until 2
# no-op latches; MUT_F5_PAINT_AFTER_1_LATCH (r1_plxh.hpp) must FAIL while the base build PASSes.
$(ROOT)/build/test_browse_paint_rg2_mut_f5_paint_after_1_latch: $(ROOT)/tests/unit/test_browse_paint_rg2.cpp \
		$(ROOT)/tests/unit/browse_render_test_common.hpp \
		$(ROOT)/host/libmisterplex/browse_render.hpp \
		$(ROOT)/host/libmisterplex/browse_paint.hpp \
		$(ROOT)/host/libmisterplex/r1_plxh.hpp $(ROOT)/tests/unit/r1_plxh_rtl_model.hpp \
		$(ROOT)/arm/misterplexd/font_8x16.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -DMUT_F5_PAINT_AFTER_1_LATCH -o $@ $(ROOT)/tests/unit/test_browse_paint_rg2.cpp -lz

$(ROOT)/build/test_browse_paint_rg2.f5-mutant-check: $(ROOT)/build/test_browse_paint_rg2 \
		$(ROOT)/build/test_browse_paint_rg2_mut_f5_paint_after_1_latch
	@$(ROOT)/build/test_browse_paint_rg2 > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@if $(ROOT)/build/test_browse_paint_rg2_mut_f5_paint_after_1_latch > $@.log 2>&1; then \
		echo "MUTANT SURVIVED: test_browse_paint_rg2_mut_f5_paint_after_1_latch"; exit 1; fi
	@echo "test_browse_paint_rg2: F5 mutant paint-after-1-latch FAILS as required" | tee $@

unit-unlocked unit-host-unlocked: $(ROOT)/build/test_browse_paint_rg2.f5-mutant-check

$(ROOT)/build/test_browse_render_rg3: $(ROOT)/tests/unit/test_browse_render_rg3.cpp \
		$(ROOT)/tests/unit/browse_render_test_common.hpp \
		$(ROOT)/host/libmisterplex/browse_render.hpp \
		$(ROOT)/arm/misterplexd/font_8x16.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_browse_render_rg3.cpp -lz

$(ROOT)/build/test_browse_render_rg4: $(ROOT)/tests/unit/test_browse_render_rg4.cpp \
		$(ROOT)/tests/unit/browse_render_test_common.hpp \
		$(ROOT)/host/libmisterplex/browse_render.hpp \
		$(ROOT)/arm/misterplexd/font_8x16.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_browse_render_rg4.cpp -lz

# R1 phase 2: the P1 browser -> renderer adapter (Q1/Q2/Q5 defaults, states, viewport, footer errors).
$(ROOT)/build/test_browse_r1_adapter: $(ROOT)/tests/unit/test_browse_r1_adapter.cpp \
		$(ROOT)/host/libmisterplex/browse_r1_adapter.hpp $(ROOT)/host/libmisterplex/browse_render.hpp \
		$(ROOT)/host/libmisterplex/browse_v2.hpp $(ROOT)/host/libmisterplex/library_browser.hpp \
		$(ROOT)/arm/misterplexd/font_8x16.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -o $@ $(ROOT)/tests/unit/test_browse_r1_adapter.cpp

$(ROOT)/build/test_status_telemetry: $(ROOT)/tests/unit/test_status_telemetry.cpp \
		$(ROOT)/arm/misterplexd/fpga_spi.cpp $(ROOT)/arm/misterplexd/fpga_spi.hpp \
		$(ROOT)/host/libmisterplex/audio_session.hpp \
		$(ROOT)/host/libmisterplex/mailbox_abi_spec.hpp \
		$(ROOT)/host/libmisterplex/ddr_bitstream_ring.hpp \
		$(ROOT)/host/libmisterplex/status_telemetry.hpp \
		$(ROOT)/host/libmisterplex/h264_residual_gold.hpp \
		$(ROOT)/host/libmisterplex/pixel_format.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -DMPX_CORE_GATE_START_OPEN -o $@ \
		$(ROOT)/tests/unit/test_status_telemetry.cpp $(ROOT)/arm/misterplexd/fpga_spi.cpp

$(ROOT)/build/test_status_osd_v3: $(ROOT)/tests/unit/test_status_osd_v3.cpp \
		$(ROOT)/arm/misterplexd/fpga_spi.cpp $(ROOT)/arm/misterplexd/fpga_spi.hpp \
		$(ROOT)/host/libmisterplex/status_telemetry.hpp $(ROOT)/host/libmisterplex/fpga_build_ids.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -DMPX_CORE_GATE_START_OPEN -o $@ \
		$(ROOT)/tests/unit/test_status_osd_v3.cpp $(ROOT)/arm/misterplexd/fpga_spi.cpp

$(ROOT)/build/test_fpga_build_ids: $(ROOT)/tests/unit/test_fpga_build_ids.cpp \
		$(ROOT)/host/libmisterplex/fpga_build_ids.hpp $(ROOT)/host/libmisterplex/status_telemetry.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_fpga_build_ids.cpp

$(ROOT)/build/test_sps_relax_gate: $(ROOT)/tests/unit/test_sps_relax_gate.cpp \
		$(ROOT)/host/libmisterplex/sps_relax_gate.hpp $(ROOT)/host/libmisterplex/fpga_build_ids.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_sps_relax_gate.cpp

# T-SPS-GATE-FALLBACK (pre-reg v1.4.3 b57a87fb section 1): CAPS -> gate verdict, T11a-g; mutants (pinned first FAIL,
# same CXXFLAGS) in tests/unit/test_gate_caps_mutants.sh.
$(ROOT)/build/test_gate_caps: $(ROOT)/tests/unit/test_gate_caps.cpp $(ROOT)/host/libmisterplex/gate_caps.hpp \
		$(ROOT)/host/libmisterplex/ddr_bitstream_ring.hpp $(ROOT)/host/libmisterplex/ddr_frame_layout.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_gate_caps.cpp

$(ROOT)/build/test_fake_core_caps: $(ROOT)/tests/unit/test_fake_core_caps.cpp \
		$(ROOT)/tests/unit/fake_plex_core.hpp $(ROOT)/host/libmisterplex/ddr_bitstream_ring.hpp \
		$(ROOT)/host/libmisterplex/mailbox_abi_spec.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/tests/unit -o $@ $(ROOT)/tests/unit/test_fake_core_caps.cpp

# SPS-gate layer 3 (L3 paper v1 c1f55425): the decision owner's rows R1-R10h; mutants (pinned first FAIL, same
# CXXFLAGS) in tests/unit/test_gate_owner_mutants.sh.
$(ROOT)/build/test_gate_owner: $(ROOT)/tests/unit/test_gate_owner.cpp $(ROOT)/host/libmisterplex/gate_owner.hpp \
		$(ROOT)/host/libmisterplex/gate_caps.hpp $(ROOT)/host/libmisterplex/ddr_bitstream_ring.hpp \
		$(ROOT)/host/libmisterplex/ddr_frame_layout.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_gate_owner.cpp

# SPS-gate layer 3: the on-screen gate error renderer (wrap/clip, guards, glyph decode on every raster target).
$(ROOT)/build/test_gate_error_render: $(ROOT)/tests/unit/test_gate_error_render.cpp \
		$(ROOT)/host/libmisterplex/gate_error_render.hpp $(ROOT)/host/libmisterplex/idle_screen.hpp \
		$(ROOT)/arm/misterplexd/font_8x16.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_gate_error_render.cpp

$(ROOT)/build/test_idle_pairing_error: $(ROOT)/tests/unit/test_idle_pairing_error.cpp \
		$(ROOT)/host/libmisterplex/idle_pairing_error.hpp \
		$(ROOT)/host/libmisterplex/core_id_select.hpp \
		$(ROOT)/host/libmisterplex/gate_error_render.hpp \
		$(ROOT)/host/libmisterplex/idle_screen.hpp \
		$(ROOT)/arm/misterplexd/font_8x16.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/host -o $@ $(ROOT)/tests/unit/test_idle_pairing_error.cpp

# SPS-gate layer 3 T1f + bar b1: resolve's forceLadder against the fake PMS (spawns tests/fixtures/fake_pms on
# loopback; run it in a private net ns) with mutants M9/M9b/M9c. Not part of unit-unlocked.
# GCAST host tests (lane/gcast): every gcast unit test, each mutant must fail, a short ASan/UBSan fuzz run.
.PHONY: gcast-tests
gcast-tests:
	CXX="$(CXX)" bash $(ROOT)/tests/gcast/run_gcast_tests.sh $(ROOT) $(ROOT)/build/gcast-tests

.PHONY: gate-l3-resolve
gate-l3-resolve:
	CXX="$(CXX)" CXXFLAGS="$(CXXFLAGS)" bash $(ROOT)/tests/unit/test_resolve_gate_fallback.sh $(ROOT) \
		$(ROOT)/build/gate-l3-resolve

# Status block heartbeat (OSD-MENU-SPEC-1002 section 9; ported from 1cb004e5 onto the canon line).
$(ROOT)/build/test_status_heartbeat: $(ROOT)/tests/unit/test_status_heartbeat.cpp \
		$(ROOT)/arm/misterplexd/fpga_spi.cpp $(ROOT)/arm/misterplexd/fpga_spi.hpp \
		$(ROOT)/host/libmisterplex/status_heartbeat.hpp $(ROOT)/host/libmisterplex/status_telemetry.hpp \
		$(ROOT)/host/libmisterplex/fpga_build_ids.hpp $(ROOT)/host/libmisterplex/core_gate.hpp $(ROOT)/host/libmisterplex/owenb_core_identity.hpp \
		$(ROOT)/host/libmisterplex/idle_pairing_error.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -o $@ \
		$(ROOT)/tests/unit/test_status_heartbeat.cpp $(ROOT)/arm/misterplexd/fpga_spi.cpp

# WEDGE-P1 W-* unit rows (prereg r3 / phase1-plan section 6): the W-* rows
# (W-NOLOCK, W-OFFROW, W-TRIG ...) are here; follow-up rows append to the same binary.
$(ROOT)/build/test_wedge_read: $(ROOT)/tests/unit/test_wedge_read.cpp \
		$(ROOT)/arm/misterplexd/fpga_spi.cpp $(ROOT)/arm/misterplexd/fpga_spi.hpp \
		$(ROOT)/host/libmisterplex/fpga_build_ids.hpp $(ROOT)/host/libmisterplex/core_gate.hpp $(ROOT)/host/libmisterplex/owenb_core_identity.hpp \
		$(ROOT)/host/libmisterplex/ddr_bitstream_ring.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -DMPX_TEST_WEDGE_READ_SEAM -DMPX_CORE_GATE_START_OPEN \
		-DMPX_DEVMEM_PATH='"$(ROOT)/build/test_wedge_read.fakemem"' -pthread -o $@ \
		$(ROOT)/tests/unit/test_wedge_read.cpp $(ROOT)/arm/misterplexd/fpga_spi.cpp

# C549 HIGH-1 rearm. Each -D restores that bug; the base binary passes and
# each mutant binary fails. Same seam and fakemem as test_wedge_read.
#   MUT_WEDGE_NO_REARM     drop gate fallback and closed-gate latch -> W-REARM fails
#   MUT_WEDGE_STALE_LATCH  keep pending on an open non-wedge skip   -> W-STALE fails
WEDGE_READ_MUTANTS := MUT_WEDGE_NO_REARM MUT_WEDGE_STALE_LATCH
$(ROOT)/build/test_wedge_read_%: $(ROOT)/tests/unit/test_wedge_read.cpp \
		$(ROOT)/arm/misterplexd/fpga_spi.cpp $(ROOT)/arm/misterplexd/fpga_spi.hpp \
		$(ROOT)/host/libmisterplex/fpga_build_ids.hpp $(ROOT)/host/libmisterplex/core_gate.hpp $(ROOT)/host/libmisterplex/owenb_core_identity.hpp \
		$(ROOT)/host/libmisterplex/ddr_bitstream_ring.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -DMPX_TEST_WEDGE_READ_SEAM -DMPX_CORE_GATE_START_OPEN \
		-D$* -DMPX_DEVMEM_PATH='"$(ROOT)/build/test_wedge_read.fakemem"' -pthread -o $@ \
		$(ROOT)/tests/unit/test_wedge_read.cpp $(ROOT)/arm/misterplexd/fpga_spi.cpp
$(ROOT)/build/test_wedge_read.mutant-check: $(ROOT)/build/test_wedge_read \
		$(addprefix $(ROOT)/build/test_wedge_read_,$(WEDGE_READ_MUTANTS))
	@$(ROOT)/build/test_wedge_read > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in $(WEDGE_READ_MUTANTS); do \
		if $(ROOT)/build/test_wedge_read_$$m > $@.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
	done
	@echo "test_wedge_read: mutants $(WEDGE_READ_MUTANTS) FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_wedge_read.mutant-check

$(ROOT)/build/test_source_aspect_dar: $(ROOT)/tests/unit/test_source_aspect_dar.cpp \
		$(ROOT)/host/libmisterplex/source_aspect.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_source_aspect_dar.cpp

# Bit-exact fold == FFmpeg av_reduce(..., 4095); only where the host has libavutil.
AVUTIL_LIBS := $(shell pkg-config --libs libavutil 2>/dev/null)
$(ROOT)/build/test_source_aspect_dar_avutil: $(ROOT)/tests/unit/test_source_aspect_dar_avutil.cpp \
		$(ROOT)/host/libmisterplex/source_aspect.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags libavutil 2>/dev/null) -o $@ \
		$(ROOT)/tests/unit/test_source_aspect_dar_avutil.cpp $(AVUTIL_LIBS)

unit-unlocked unit-host-unlocked: $(ROOT)/build/test_source_aspect_dar $(if $(AVUTIL_LIBS),$(ROOT)/build/test_source_aspect_dar_avutil)

# review 19:24 L2: a transcode sends its stream DAR (source_aspect.hpp firstAuSourceAspect).
# The _mut_metadata_wins builds define MUT_L2_METADATA_WINS (the double letterbox) and must fail.
$(ROOT)/build/test_l2_transcode_dar: $(ROOT)/tests/unit/test_l2_transcode_dar.cpp \
		$(ROOT)/host/libmisterplex/source_aspect.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_l2_transcode_dar.cpp
$(ROOT)/build/test_l2_transcode_dar_mut_metadata_wins: $(ROOT)/tests/unit/test_l2_transcode_dar.cpp \
		$(ROOT)/host/libmisterplex/source_aspect.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -DMUT_L2_METADATA_WINS -o $@ $(ROOT)/tests/unit/test_l2_transcode_dar.cpp
$(ROOT)/build/test_l2_transcode_dar.mutant-check: $(ROOT)/build/test_l2_transcode_dar \
		$(ROOT)/build/test_l2_transcode_dar_mut_metadata_wins
	@$(ROOT)/build/test_l2_transcode_dar > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@if $(ROOT)/build/test_l2_transcode_dar_mut_metadata_wins > $@.log 2>&1; then \
		echo "MUTANT SURVIVED: test_l2_transcode_dar_mut_metadata_wins"; exit 1; fi
	@echo "test_l2_transcode_dar: mutant metadata-wins FAILS as required" | tee $@

# L2 rows at the FPGA demux (tests/unit/test_l2_transcode_dar_demux.sh), host build with the system
# FFmpeg.
L2_DEMUX_AV := libavformat libavcodec libavutil libswresample
L2_DEMUX_OK := $(shell pkg-config --exists $(L2_DEMUX_AV) 2>/dev/null && command -v ffmpeg >/dev/null 2>&1 && echo 1)
L2_DEMUX_SRC := $(ROOT)/tests/unit/test_l2_transcode_dar_demux.cpp $(ROOT)/arm/misterplexd/av_inproc_decode.cpp
L2_DEMUX_DEPS := $(L2_DEMUX_SRC) $(ROOT)/host/libmisterplex/av_inproc_decode.hpp \
	$(ROOT)/host/libmisterplex/source_aspect.hpp
$(ROOT)/build/test_l2_transcode_dar_demux: $(L2_DEMUX_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -DMPX_HAVE_LIBAV -I$(ROOT)/arm/misterplexd \
		$(shell pkg-config --cflags $(L2_DEMUX_AV)) -o $@ $(L2_DEMUX_SRC) \
		$(shell pkg-config --libs $(L2_DEMUX_AV)) -pthread
$(ROOT)/build/test_l2_transcode_dar_demux_mut_metadata_wins: $(L2_DEMUX_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -DMPX_HAVE_LIBAV -DMUT_L2_METADATA_WINS -I$(ROOT)/arm/misterplexd \
		$(shell pkg-config --cflags $(L2_DEMUX_AV)) -o $@ $(L2_DEMUX_SRC) \
		$(shell pkg-config --libs $(L2_DEMUX_AV)) -pthread

unit-unlocked unit-host-unlocked: $(ROOT)/build/test_l2_transcode_dar $(ROOT)/build/test_l2_transcode_dar.mutant-check \
	$(if $(L2_DEMUX_OK),$(ROOT)/build/test_l2_transcode_dar_demux \
	$(ROOT)/build/test_l2_transcode_dar_demux_mut_metadata_wins)

# T-SPS-GATE-FALLBACK code layer 2 (pre-reg v1.4.3 b57a87fb sections 2, 3 (a), 8 T10): CompressedSyntax::inspect on
# GateCaps + its typed verdict over the gatefix corpus (tests/fixtures/gatefix_c764a27d). One TU with
# arm/misterplexd/av_inproc_decode.cpp and the system FFmpeg libs, as test_l2_transcode_dar_demux; mutants (pinned
# first FAIL, same CXX/CXXFLAGS) in tests/unit/test_compressed_syntax_mutants.sh.
COMPRESSED_SYNTAX_OK := $(shell pkg-config --exists $(L2_DEMUX_AV) 2>/dev/null && echo 1)
$(ROOT)/build/test_compressed_syntax: $(ROOT)/tests/unit/test_compressed_syntax.cpp \
		$(ROOT)/arm/misterplexd/av_inproc_decode.cpp $(ROOT)/host/libmisterplex/av_inproc_decode.hpp \
		$(ROOT)/host/libmisterplex/gate_caps.hpp $(ROOT)/host/libmisterplex/h264_sps.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -DMPX_HAVE_LIBAV -I$(ROOT)/arm/misterplexd \
		$(shell pkg-config --cflags $(L2_DEMUX_AV)) -o $@ $(ROOT)/tests/unit/test_compressed_syntax.cpp \
		$(shell pkg-config --libs $(L2_DEMUX_AV)) -pthread
unit-unlocked unit-host-unlocked: $(if $(COMPRESSED_SYNTAX_OK),$(ROOT)/build/test_compressed_syntax)
$(ROOT)/build/test_library_browser: $(ROOT)/tests/unit/test_library_browser.cpp \
		$(ROOT)/host/libmisterplex/library_browser.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_library_browser.cpp

# rc3: plexui_data.hpp computeGridLayout() now reads screenGeom() (OB port 1/5), so the image-cache test links the
# render/font/data/api sources; plexui_render.cpp carries the stb_image_resize2 implementation (no link stub).
PLEXUI_IMGCACHE_TEST_EXTRA := $(addprefix $(ROOT)/host/libmisterplex/plexui/plexui_,render.cpp font.cpp data.cpp api_cache.cpp)

PLEXUI_IMG_CACHE_SRC := \
	$(ROOT)/host/libmisterplex/plexui/plexui_images.cpp \
	$(ROOT)/host/libmisterplex/plexui/plexui_disk_cache.cpp \
	$(ROOT)/host/libmisterplex/plexui/plexui_http.cpp

$(ROOT)/build/test_plexui_imgcache: $(ROOT)/tests/unit/test_plexui_imgcache.cpp \
		$(PLEXUI_IMGCACHE_TEST_EXTRA) \
		$(PLEXUI_IMG_CACHE_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui -o $@ \
		$(ROOT)/tests/unit/test_plexui_imgcache.cpp \
		$(PLEXUI_IMGCACHE_TEST_EXTRA) \
		$(PLEXUI_IMG_CACHE_SRC)

$(ROOT)/build/test_plexui_imgcache.check: $(ROOT)/build/test_plexui_imgcache
	$(ROOT)/build/test_plexui_imgcache
	@touch $@

$(ROOT)/build/test_plexui_qoi_disk: $(ROOT)/tests/unit/test_plexui_qoi_disk.cpp \
		$(ROOT)/host/libmisterplex/plexui/plexui_disk_cache.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -std=c++17 -I$(ROOT)/host/libmisterplex/plexui -o $@ \
		$(ROOT)/tests/unit/test_plexui_qoi_disk.cpp \
		$(ROOT)/host/libmisterplex/plexui/plexui_disk_cache.cpp

$(ROOT)/build/test_plexui_qoi_disk.check: $(ROOT)/build/test_plexui_qoi_disk
	$(ROOT)/build/test_plexui_qoi_disk
	@touch $@

unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plexui_imgcache.check
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plexui_qoi_disk.check

PLEXUI_ASAN_CXXFLAGS := $(CXXFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer

$(ROOT)/build/test_plexui_cache_stress: $(ROOT)/tests/unit/test_plexui_cache_stress.cpp \
		$(PLEXUI_IMGCACHE_TEST_EXTRA) \
		$(PLEXUI_IMG_CACHE_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(PLEXUI_ASAN_CXXFLAGS) -pthread -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui -o $@ \
		$(ROOT)/tests/unit/test_plexui_cache_stress.cpp \
		$(PLEXUI_IMGCACHE_TEST_EXTRA) \
		$(PLEXUI_IMG_CACHE_SRC)

$(ROOT)/build/test_plexui_cache_stress.check: $(ROOT)/build/test_plexui_cache_stress
	MPX_PLEXUI_STRESS_SEC=8 $(ROOT)/build/test_plexui_cache_stress
	@touch $@

$(ROOT)/build/test_plexui_imgcache_asan: $(ROOT)/tests/unit/test_plexui_imgcache.cpp \
		$(PLEXUI_IMGCACHE_TEST_EXTRA) \
		$(PLEXUI_IMG_CACHE_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(PLEXUI_ASAN_CXXFLAGS) -pthread -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui -o $@ \
		$(ROOT)/tests/unit/test_plexui_imgcache.cpp \
		$(PLEXUI_IMGCACHE_TEST_EXTRA) \
		$(PLEXUI_IMG_CACHE_SRC)

$(ROOT)/build/test_plexui_imgcache_asan.check: $(ROOT)/build/test_plexui_imgcache_asan
	$(ROOT)/build/test_plexui_imgcache_asan
	@touch $@

$(ROOT)/build/test_plexui_qoi_disk_asan: $(ROOT)/tests/unit/test_plexui_qoi_disk.cpp \
		$(ROOT)/host/libmisterplex/plexui/plexui_disk_cache.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(PLEXUI_ASAN_CXXFLAGS) -pthread -std=c++17 -I$(ROOT)/host/libmisterplex/plexui -o $@ \
		$(ROOT)/tests/unit/test_plexui_qoi_disk.cpp \
		$(ROOT)/host/libmisterplex/plexui/plexui_disk_cache.cpp

$(ROOT)/build/test_plexui_qoi_disk_asan.check: $(ROOT)/build/test_plexui_qoi_disk_asan
	$(ROOT)/build/test_plexui_qoi_disk_asan
	@touch $@

unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plexui_cache_stress.check
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plexui_imgcache_asan.check
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plexui_qoi_disk_asan.check

# 0012 soak OOM: one hard PlexUI byte budget (art LRU + view-layer cache + in-flight + disk queue),
# with a 30-min-equivalent grid-scroll soak (MPX_PLEXUI_SOAK_MOVES, default 9000). Plain + ASan/LSan.
PLEXUI_MEM_BUDGET_SRC := $(ROOT)/tests/unit/test_plexui_mem_budget.cpp $(PLEXUI_IMGCACHE_TEST_EXTRA) \
	$(PLEXUI_IMG_CACHE_SRC)

$(ROOT)/build/test_plexui_mem_budget: $(PLEXUI_MEM_BUDGET_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-o $@ $(PLEXUI_MEM_BUDGET_SRC)

$(ROOT)/build/test_plexui_mem_budget.check: $(ROOT)/build/test_plexui_mem_budget
	$(ROOT)/build/test_plexui_mem_budget
	@touch $@

$(ROOT)/build/test_plexui_mem_budget_asan: $(PLEXUI_MEM_BUDGET_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(PLEXUI_ASAN_CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host \
		-I$(ROOT)/host/libmisterplex/plexui -o $@ $(PLEXUI_MEM_BUDGET_SRC)

$(ROOT)/build/test_plexui_mem_budget_asan.check: $(ROOT)/build/test_plexui_mem_budget_asan
	ASAN_OPTIONS=detect_leaks=1 $(ROOT)/build/test_plexui_mem_budget_asan
	@touch $@

PLEXUI_MEM_BUDGET_MUTANTS := no_trim:MUT_PLEXUI_NO_TRIM_ON_LOWER pins_over:MUT_PLEXUI_PINS_OVER_BUDGET \
		evict_pins_inflight:MUT_PLEXUI_EVICT_PINS_FOR_INFLIGHT
PLEXUI_MEM_BUDGET_MUT_BINS := $(foreach m,$(PLEXUI_MEM_BUDGET_MUTANTS),$(ROOT)/build/test_plexui_mem_budget_mut_$(word 1,$(subst :, ,$(m))))

$(ROOT)/build/test_plexui_mem_budget_mut_%: $(PLEXUI_MEM_BUDGET_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-D$(word 2,$(subst :, ,$(filter $*:%,$(PLEXUI_MEM_BUDGET_MUTANTS)))) -o $@ $(PLEXUI_MEM_BUDGET_SRC)

$(ROOT)/build/test_plexui_mem_budget.mutant-check: $(ROOT)/build/test_plexui_mem_budget $(PLEXUI_MEM_BUDGET_MUT_BINS)
	@$(ROOT)/build/test_plexui_mem_budget > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in $(foreach x,$(PLEXUI_MEM_BUDGET_MUTANTS),$(word 1,$(subst :, ,$(x)))); do \
		if $(ROOT)/build/test_plexui_mem_budget_mut_$$m > $@.log 2>&1; then echo "MUTANT SURVIVED: $$m"; exit 1; fi; done
	@echo "test_plexui_mem_budget: $(words $(PLEXUI_MEM_BUDGET_MUTANTS)) mutants FAIL as required" | tee $@

unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plexui_mem_budget.check
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plexui_mem_budget_asan.check
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plexui_mem_budget.mutant-check

# The MPX_H264_FILTER start-up line (review 17:17 (4)): its format and that it matches the daemon's readers.
# A .check stamp named by both unit-unlocked and unit-host-unlocked (unit-host coverage C4: no rule names only one).
$(ROOT)/build/test_h264_filter_line: $(ROOT)/tests/unit/test_h264_filter_line.cpp \
		$(ROOT)/host/libmisterplex/h264_filter_setting.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_h264_filter_line.cpp

$(ROOT)/build/test_h264_filter_line.check: $(ROOT)/build/test_h264_filter_line
	$(ROOT)/build/test_h264_filter_line
	@touch $@

unit-unlocked unit-host-unlocked: $(ROOT)/build/test_h264_filter_line.check

# Browse P1 (MPX_BROWSE_V2, lab): the pure v2 parser/controller/ticket logic, plus the HTTP
# checks against tests/unit/fake_browse_server.py (python3 + curl; loopback only).
$(ROOT)/build/test_browse_v2: $(ROOT)/tests/unit/test_browse_v2.cpp \
		$(ROOT)/host/libmisterplex/browse_v2.hpp $(ROOT)/host/libmisterplex/browse_controller.hpp \
		$(ROOT)/host/libmisterplex/browse_fetch.hpp $(ROOT)/host/libmisterplex/library_browser.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -o $@ $(ROOT)/tests/unit/test_browse_v2.cpp

$(ROOT)/build/test_browse_library_list: $(ROOT)/tests/unit/test_browse_library_list.cpp \
		$(ROOT)/host/libmisterplex/browse_library_list.hpp \
		$(ROOT)/host/libmisterplex/library_browser.hpp \
		$(ROOT)/arm/misterplexd/font_8x16.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_browse_library_list.cpp

# Browse P1 daemon e2e and flags-off parity: they start host daemons and a fake core, so they
# run ONLY through heavy-sim-slot in a private PID + net namespace (rule B), never
# in a quiet window and never in make unit.
browse-e2e:
	bash $(ROOT)/tests/unit/test_browse_flagoff_parity.sh
	bash $(ROOT)/tests/unit/test_browse_e2e.sh

$(ROOT)/build/test_av_sync: $(ROOT)/tests/unit/test_av_sync.cpp \
		$(ROOT)/host/libmisterplex/av_sync.hpp $(ROOT)/host/libmisterplex/fpga_build_ids.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_av_sync.cpp

$(ROOT)/build/test_transcode_offset: $(ROOT)/tests/unit/test_transcode_offset.cpp \
		$(ROOT)/arm/misterplexd/transcode_offset.hpp $(ROOT)/host/libmisterplex/p720_transcode_vf.hpp \
		$(ROOT)/arm/misterplexd/url_query_codec.hpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/arm/misterplexd/plex_resolve.hpp \
		$(ROOT)/host/libmisterplex/osd_menu.hpp \
		$(ROOT)/host/libmisterplex/video_backend.hpp \
		$(ROOT)/host/libmisterplex/present_bank.hpp \
		$(ROOT)/assets/plex-profiles/fpga_profile.hpp \
		$(ROOT)/host/libmisterplex/ddr_bitstream_ring.hpp \
		$(ROOT)/host/libmisterplex/mailbox_abi_spec.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -o $@ \
		$(ROOT)/tests/unit/test_transcode_offset.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp

$(ROOT)/build/test_idct_quant: $(ROOT)/tests/unit/test_idct_quant.cpp \
		$(ROOT)/host/libmisterplex/h264_cavlc.hpp $(ROOT)/host/libmisterplex/h264_nal.hpp \
		$(ROOT)/host/libmisterplex/h264_sps.hpp $(ROOT)/host/libmisterplex/h264_recon.hpp \
		$(ROOT)/host/libmisterplex/h264_residual_gold.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_idct_quant.cpp

$(ROOT)/build/test_p3_host_recon_vectors: $(ROOT)/tests/unit/test_p3_host_recon_vectors.cpp \
		$(ROOT)/host/libmisterplex/h264_recon.hpp \
		$(ROOT)/host/libmisterplex/h264_slice_walk.hpp \
		$(ROOT)/host/libmisterplex/h264_cavlc.hpp \
		$(ROOT)/tests/fixtures/p3_host_recon/plex_real_baseline_320x240_1f.264 \
		$(ROOT)/tests/fixtures/p3_host_recon/mb0_luma_v1.json \
		$(ROOT)/tests/fixtures/p3_host_recon/frame_mae_v1.csv
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_p3_host_recon_vectors.cpp

$(ROOT)/build/extract_h264_golden: $(ROOT)/tools/extract_h264_golden.cpp \
		$(ROOT)/host/libmisterplex/h264_recon.hpp \
		$(ROOT)/host/libmisterplex/h264_slice_walk.hpp \
		$(ROOT)/host/libmisterplex/h264_cavlc.hpp \
		$(ROOT)/host/libmisterplex/h264_residual_gold.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tools/extract_h264_golden.cpp

$(ROOT)/build/score_h264_native_frames: $(ROOT)/tools/score_h264_native_frames.cpp \
		$(ROOT)/host/libmisterplex/h264_recon.hpp \
		$(ROOT)/host/libmisterplex/h264_slice_walk.hpp \
		$(ROOT)/host/libmisterplex/h264_cavlc.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tools/score_h264_native_frames.cpp

$(ROOT)/build/analyze_h264_intra_mbs: $(ROOT)/tools/analyze_h264_intra_mbs.cpp \
		$(ROOT)/host/libmisterplex/h264_recon.hpp \
		$(ROOT)/host/libmisterplex/h264_slice_walk.hpp \
		$(ROOT)/host/libmisterplex/h264_cavlc.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tools/analyze_h264_intra_mbs.cpp

$(ROOT)/build/test_p3_idct_reference_model: $(ROOT)/tests/unit/test_p3_idct_reference_model.cpp \
		$(ROOT)/host/libmisterplex/h264_cavlc.hpp \
		$(ROOT)/host/libmisterplex/h264_nal.hpp \
		$(ROOT)/host/libmisterplex/h264_sps.hpp \
		$(ROOT)/host/libmisterplex/h264_recon.hpp \
		$(ROOT)/host/libmisterplex/h264_slice_walk.hpp \
		$(ROOT)/host/libmisterplex/h264_residual_gold.hpp \
		$(ROOT)/fpga/Plex_MiSTer/files.qip \
		$(ROOT)/fpga/Plex_MiSTer/rtl/slice_hdr_parser.sv \
		$(ROOT)/fpga/Plex_MiSTer/rtl/h264_iq_idct_4x4.sv \
		$(ROOT)/tests/fixtures/p3_host_recon/plex_real_baseline_320x240_1f.264 \
		$(ROOT)/tests/fixtures/p3_host_recon/mb0_luma_v1.json \
		$(ROOT)/tests/fixtures/p3_host_recon/frame_mae_v1.csv
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_p3_idct_reference_model.cpp

$(ROOT)/build/test_p3_inter_pred_vectors: $(ROOT)/tests/unit/test_p3_inter_pred_vectors.cpp \
		$(ROOT)/host/libmisterplex/h264_nal.hpp \
		$(ROOT)/host/libmisterplex/h264_sps.hpp \
		$(ROOT)/scripts/gen_test_annexb_inter.py \
		$(ROOT)/tests/fixtures/p3_inter_pred/plex_inter_p16_baseline_320x240_12f.264 \
		$(ROOT)/tests/fixtures/p3_inter_pred/pframe1_mb_v1.json \
		$(ROOT)/tests/fixtures/p3_inter_pred/frame_mae_v1.csv
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(FFMPEG_CFLAGS) -o $@ $(ROOT)/tests/unit/test_p3_inter_pred_vectors.cpp $(FFMPEG_LIBS)

$(ROOT)/build/test_cavlc_dc: $(ROOT)/tests/unit/test_cavlc_dc.cpp \
		$(ROOT)/host/libmisterplex/h264_cavlc.hpp $(ROOT)/host/libmisterplex/h264_nal.hpp \
		$(ROOT)/host/libmisterplex/h264_sps.hpp $(ROOT)/host/libmisterplex/h264_slice_walk.hpp \
		$(ROOT)/host/libmisterplex/h264_recon.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_cavlc_dc.cpp

$(ROOT)/build/test_sps_parse: $(ROOT)/tests/unit/test_sps_parse.cpp $(ROOT)/host/libmisterplex/h264_sps.hpp \
		$(ROOT)/host/libmisterplex/sps_crop_letterbox.hpp $(ROOT)/host/libmisterplex/fpga_build_ids.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_sps_parse.cpp

$(ROOT)/build/test_slice_hdr: $(ROOT)/tests/unit/test_slice_hdr.cpp \
		$(ROOT)/host/libmisterplex/h264_nal.hpp $(ROOT)/host/libmisterplex/h264_sps.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_slice_hdr.cpp

$(ROOT)/build/test_frame_store_math: $(ROOT)/tests/unit/test_frame_store_math.cpp \
		$(ROOT)/host/libmisterplex/ddr_frame_layout.hpp \
		$(ROOT)/host/libmisterplex/pixel_format.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_frame_store_math.cpp

$(ROOT)/build/test_frame_store_sdram_sim: $(ROOT)/tests/unit/test_frame_store_sdram_sim.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_frame_store_sdram_sim.cpp

$(ROOT)/build/test_frame_store_ddr_prefetch_sim: $(ROOT)/tests/unit/test_frame_store_ddr_prefetch_sim.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_frame_store_ddr_prefetch_sim.cpp

$(ROOT)/build/test_sdram_memtest_sim: $(ROOT)/tests/unit/test_sdram_memtest_sim.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_sdram_memtest_sim.cpp

$(ROOT)/build/test_sdram_mailbox: $(ROOT)/tests/unit/test_sdram_mailbox.cpp \
		$(ROOT)/host/libmisterplex/sdram_mailbox.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_sdram_mailbox.cpp

# T-DAEMON-MVPS (review 18:34): the core's decode_error named on the AU push-reject session end.
$(ROOT)/build/test_decode_error_note: $(ROOT)/tests/unit/test_decode_error_note.cpp \
	$(ROOT)/host/libmisterplex/ddr_bitstream_ring.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_decode_error_note.cpp
$(ROOT)/build/test_decode_error_note.ok: $(ROOT)/build/test_decode_error_note
	@$(ROOT)/build/test_decode_error_note > $@.log 2>&1 || { cat $@.log; exit 1; }
	@cat $@.log
	@cp $@.log $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_decode_error_note.ok

$(ROOT)/build/test_annexb_count: $(ROOT)/tests/unit/test_annexb_count.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_annexb_count.cpp

$(ROOT)/build/test_cadence: $(ROOT)/tests/unit/test_cadence.cpp $(ROOT)/host/libmisterplex/cadence.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_cadence.cpp

$(ROOT)/build/test_pts_cadence: $(ROOT)/tests/unit/test_pts_cadence.cpp $(ROOT)/host/libmisterplex/pts_cadence.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_pts_cadence.cpp

$(ROOT)/build/test_avclock: $(ROOT)/tests/unit/test_avclock.cpp \
		$(ROOT)/host/libmisterplex/av_clock.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_avclock.cpp

$(ROOT)/build/test_p720_audio_keepup: $(ROOT)/tests/unit/test_p720_audio_keepup.cpp \
		$(ROOT)/host/libmisterplex/p720_audio_keepup.hpp \
		$(ROOT)/host/libmisterplex/mraudio_status.hpp \
		$(ROOT)/host/libmisterplex/ddr_frame_layout.hpp \
		$(ROOT)/host/libmisterplex/present_bank.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_p720_audio_keepup.cpp

$(ROOT)/build/test_p720_transcode_vf: $(ROOT)/tests/unit/test_p720_transcode_vf.cpp \
		$(ROOT)/host/libmisterplex/p720_transcode_vf.hpp $(ROOT)/host/libmisterplex/fabric_direct.hpp \
		$(ROOT)/host/libmisterplex/p720_e2e_budget.hpp $(ROOT)/host/libmisterplex/present_bank.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_p720_transcode_vf.cpp

$(ROOT)/build/test_cast_av_480p_policy: $(ROOT)/tests/unit/test_cast_av_480p_policy.cpp \
		$(ROOT)/host/libmisterplex/av_clock.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_cast_av_480p_policy.cpp

$(ROOT)/build/test_main_guard: $(ROOT)/tests/unit/test_main_guard.cpp \
		$(ROOT)/arm/misterplexd/fpga_spi.cpp $(ROOT)/arm/misterplexd/fpga_spi.hpp \
		$(ROOT)/host/libmisterplex/audio_session.hpp \
		$(ROOT)/host/libmisterplex/ddr_bitstream_ring.hpp \
		$(ROOT)/host/libmisterplex/mailbox_abi_spec.hpp \
		$(ROOT)/host/libmisterplex/pixel_format.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -DMPX_CORE_GATE_START_OPEN -o $@ \
		$(ROOT)/tests/unit/test_main_guard.cpp $(ROOT)/arm/misterplexd/fpga_spi.cpp

# MPX_DDR_FRAME_WC guard (K1-B1): the real fpga_spi.cpp, sparse fake /dev/mem and a fake
# /dev/mplex_ddr WC window file that exists (build dir).
$(ROOT)/build/test_ddr_frame_wc: $(ROOT)/tests/unit/test_ddr_frame_wc.cpp \
		$(ROOT)/host/libmisterplex/ddr_frame_wc.hpp \
		$(ROOT)/arm/misterplexd/fpga_spi.cpp $(ROOT)/arm/misterplexd/fpga_spi.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -DMPX_CORE_GATE_START_OPEN \
		-DMPX_DEVMEM_PATH='"$(ROOT)/build/test_ddr_frame_wc.fakemem"' \
		-DMPX_MPLEX_DDR_PATH='"$(ROOT)/build/test_ddr_frame_wc.fakewc"' -o $@ \
		$(ROOT)/tests/unit/test_ddr_frame_wc.cpp $(ROOT)/arm/misterplexd/fpga_spi.cpp

# D52-LOG (lab, MPX_D52_LOG=1): the logger, the PLX page reader and the poller thread on a sparse fake
# /dev/mem (build dir), and the hooks in the real fpga_spi.cpp.
$(ROOT)/build/test_d52_log: $(ROOT)/tests/unit/test_d52_log.cpp \
		$(ROOT)/host/libmisterplex/d52_log.hpp $(ROOT)/host/libmisterplex/source_aspect.hpp \
		$(ROOT)/host/libmisterplex/fpga_build_ids.hpp $(ROOT)/host/libmisterplex/ddr_frame_layout.hpp \
		$(ROOT)/tests/unit/fake_plex_core.hpp \
		$(ROOT)/arm/misterplexd/fpga_spi.cpp $(ROOT)/arm/misterplexd/fpga_spi.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -DMPX_CORE_GATE_START_OPEN \
		-DMPX_DEVMEM_PATH='"$(ROOT)/build/test_d52_log.fakemem"' -o $@ \
		$(ROOT)/tests/unit/test_d52_log.cpp $(ROOT)/arm/misterplexd/fpga_spi.cpp

# review 07:30 C2 + 08:17 (a) + 08:18: one mapper and one lock for the doorbell (PLX) page, on the
# real fpga_spi.cpp (sparse fake /dev/mem; test seams widen a first map and an access by 20 ms).
# Rows P-a..P-h; each mutant (no lock; the frame window maps the page too; close() skips the page-lock
# wait) must FAIL. T-PAGEMAP-CLOSE-WAIT (review 17:06): the forced-schedule rows P-g/P-h (seam
# MPX_TEST_DDR_PAGE_HOLD) must kill the close-wait mutants ALONE (forced-check), without timing.
DDR_PAGE_MAPPER_DEPS := $(ROOT)/tests/unit/test_ddr_page_mapper.cpp \
	$(ROOT)/arm/misterplexd/fpga_spi.cpp $(ROOT)/arm/misterplexd/fpga_spi.hpp \
	$(ROOT)/host/libmisterplex/r1_plxh.hpp
DDR_PAGE_MAPPER_FLAGS := -I$(ROOT)/arm/misterplexd -pthread -DMPX_CORE_GATE_START_OPEN \
	-DMPX_TEST_DDR_PAGE_MAP_DELAY_US=20000 -DMPX_TEST_DDR_PAGE_ACCESS_DELAY_US=20000 -DMPX_TEST_DDR_PAGE_HOLD
DDR_PAGE_MAPPER_MUTANTS := MUT_DDR_PAGE_UNLOCKED MUT_DDR_WINDOW_COVERS_PAGE MUT_BROWSE_DIRECT_OWN_MAP \
	MUT_R1_PLX_OWN_MAP MUT_CLOSE_SKIPS_PAGE_WAIT
DDR_PAGE_MAPPER_FORCED_MUTANTS := MUT_CLOSE_SKIPS_PAGE_WAIT MUT_DDR_PAGE_UNLOCKED
$(ROOT)/build/test_ddr_page_mapper: $(DDR_PAGE_MAPPER_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(DDR_PAGE_MAPPER_FLAGS) -DMPX_DEVMEM_PATH='"$(ROOT)/build/test_ddr_page_mapper.fakemem"' \
		-o $@ $(ROOT)/tests/unit/test_ddr_page_mapper.cpp $(ROOT)/arm/misterplexd/fpga_spi.cpp
$(ROOT)/build/test_ddr_page_mapper.ok: $(ROOT)/build/test_ddr_page_mapper
	@MPX_DDR_FAKEMEM="$(ROOT)/build/test_ddr_page_mapper.ok.fakemem" \
		$(ROOT)/build/test_ddr_page_mapper > $@.log 2>&1 || { cat $@.log; exit 1; }
	@cat $@.log
	@cp $@.log $@
$(ROOT)/build/test_ddr_page_mapper_%: $(DDR_PAGE_MAPPER_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(DDR_PAGE_MAPPER_FLAGS) -D$* \
		-DMPX_DEVMEM_PATH='"$(ROOT)/build/test_ddr_page_mapper_$*.fakemem"' \
		-o $@ $(ROOT)/tests/unit/test_ddr_page_mapper.cpp $(ROOT)/arm/misterplexd/fpga_spi.cpp
$(ROOT)/build/test_ddr_page_mapper.mutant-check: $(ROOT)/build/test_ddr_page_mapper \
		$(addprefix $(ROOT)/build/test_ddr_page_mapper_,$(DDR_PAGE_MAPPER_MUTANTS))
	@MPX_DDR_FAKEMEM="$(ROOT)/build/test_ddr_page_mapper.mutant-check.fakemem" \
		$(ROOT)/build/test_ddr_page_mapper > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in $(DDR_PAGE_MAPPER_MUTANTS); do \
		if $(ROOT)/build/test_ddr_page_mapper_$$m > $@.$$m.log 2>&1; then echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
	done
	@echo "test_ddr_page_mapper: mutants $(DDR_PAGE_MAPPER_MUTANTS) FAIL as required" | tee $@
# The 04:04 grep bar: each forced-row grep is anchored to its full message prefix (test_ddr_page_mapper.cpp :552/:580
# print the P-g/P-h pass lines; :476 prints the kill line, with rows P-g/P-h at :542/:574).
DDR_FORCED_PASS_RX := ^P-[gh] close\(\) during a HELD (first map|access on the mapped page): blocked in futex\(\) on ddrPageMu_ until the window was released,
DDR_FORCED_KILL_RX := ^FAIL P-[gh]: close\(\) returned while the (first map|access) was held in flight \(it did not wait for the page lock\)$$
$(ROOT)/build/test_ddr_page_mapper.forced-check: $(ROOT)/build/test_ddr_page_mapper \
		$(addprefix $(ROOT)/build/test_ddr_page_mapper_,$(DDR_PAGE_MAPPER_FORCED_MUTANTS))
	@MPX_DDR_FAKEMEM="$(ROOT)/build/test_ddr_page_mapper.forced-check.fakemem" \
		$(ROOT)/build/test_ddr_page_mapper forced > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@[ "$$(grep -c -E '$(DDR_FORCED_PASS_RX)' $@.base.log)" = 2 ] || \
		{ echo "THE BASE'S FORCED ROWS P-g/P-h DID NOT BOTH PRINT THEIR PASS LINE:"; cat $@.base.log; exit 1; }
	@for m in $(DDR_PAGE_MAPPER_FORCED_MUTANTS); do \
		if $(ROOT)/build/test_ddr_page_mapper_$$m forced > $@.$$m.log 2>&1; then echo "MUTANT SURVIVED THE FORCED ROWS: $$m"; exit 1; fi; \
		grep -q -E '$(DDR_FORCED_KILL_RX)' $@.$$m.log || { echo "FORCED ROWS DID NOT CATCH $$m:"; cat $@.$$m.log; exit 1; }; \
		! grep -q -E '$(DDR_FORCED_PASS_RX)' $@.$$m.log || \
			{ echo "A FAILED FORCED ROW PRINTED ITS PASS LINE ($$m):"; cat $@.$$m.log; exit 1; }; \
	done
	@echo "test_ddr_page_mapper: forced rows P-g/P-h alone kill $(DDR_PAGE_MAPPER_FORCED_MUTANTS)" | tee $@

unit-unlocked unit-host-unlocked: $(ROOT)/build/test_ddr_page_mapper.ok $(ROOT)/build/test_ddr_page_mapper.mutant-check \
	$(ROOT)/build/test_ddr_page_mapper.forced-check
# K1 KM-2: the R1 idle-FB map path choice (wc / devmem / off) on a fake syscall layer; the mutant (the fallback
# skips /dev/mem) must FAIL. K1 ABI: the kmod and the host header agree (tests/unit/test_mplex_abi_sync.py).
$(ROOT)/build/test_idle_fb_map: $(ROOT)/tests/unit/test_idle_fb_map.cpp \
		$(ROOT)/host/libmisterplex/idle_fb_map.hpp $(ROOT)/host/libmisterplex/r1_idle_fb.hpp \
		$(ROOT)/host/libmisterplex/mplex_ddr_abi.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_idle_fb_map.cpp

$(ROOT)/build/test_idle_fb_map_mut_skip_devmem: $(ROOT)/tests/unit/test_idle_fb_map.cpp \
		$(ROOT)/host/libmisterplex/idle_fb_map.hpp $(ROOT)/host/libmisterplex/r1_idle_fb.hpp \
		$(ROOT)/host/libmisterplex/mplex_ddr_abi.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -DMUT_IDLEFB_SKIP_DEVMEM -o $@ $(ROOT)/tests/unit/test_idle_fb_map.cpp

$(ROOT)/build/test_idle_fb_map.mutant-check: $(ROOT)/build/test_idle_fb_map \
		$(ROOT)/build/test_idle_fb_map_mut_skip_devmem
	@$(ROOT)/build/test_idle_fb_map > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@if $(ROOT)/build/test_idle_fb_map_mut_skip_devmem > $@.log 2>&1; then \
		echo "MUTANT SURVIVED: test_idle_fb_map_mut_skip_devmem"; exit 1; fi
	@echo "test_idle_fb_map: mutant skip-devmem FAILS as required" | tee $@

# OWENB-P1B: the OwenbRing producer (host/libmisterplex/owenb_ring.*) on top of the vendored MIT owenb h264ring lib
# (third_party/owenb-h264ring at eac1064). The test drives a HeapDdrWindow with a fake core (a port of upstream's
# hps/tests/test_ring.c) and a sparse file standing in for /dev/mem. Mutant MUT_OWENB_SKIP_HEARTBEAT: a probe that
# skips the heartbeat must FAIL row P4 (a frozen heartbeat reads as Stale).
OWENB_RING_DIR := $(ROOT)/third_party/owenb-h264ring
OWENB_RING_CFLAGS := -std=c99 -O2 -D_DEFAULT_SOURCE -Wall -Wextra -I$(OWENB_RING_DIR)/include -I$(OWENB_RING_DIR)/lib
OWENB_RING_HDRS := $(wildcard $(OWENB_RING_DIR)/include/*.h) $(wildcard $(OWENB_RING_DIR)/lib/*.h)
OWENB_RING_OBJS := $(patsubst %,$(ROOT)/build/owenb-h264ring/%.o,h264ring h264ring_au h264ring_cavlc h264ring_dump h264ring_linux)
OWENB_RING_SRCS := $(ROOT)/host/libmisterplex/owenb_ring.cpp
OWENB_RING_DEPS := $(ROOT)/tests/unit/test_owenb_ring.cpp $(OWENB_RING_SRCS) \
		$(ROOT)/host/libmisterplex/owenb_ring.hpp $(ROOT)/host/libmisterplex/owenb_ddr_window.hpp \
		$(ROOT)/host/libmisterplex/h264_bitstream_transport.hpp $(ROOT)/host/libmisterplex/video_backend.hpp \
		$(OWENB_RING_OBJS)

$(ROOT)/build/owenb-h264ring/%.o: $(OWENB_RING_DIR)/lib/%.c $(OWENB_RING_HDRS)
	@mkdir -p $(ROOT)/build/owenb-h264ring
	$(CC) $(OWENB_RING_CFLAGS) -c -o $@ $<

$(ROOT)/build/test_owenb_ring: $(OWENB_RING_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(OWENB_RING_DIR)/include -o $@ $(ROOT)/tests/unit/test_owenb_ring.cpp \
		$(OWENB_RING_SRCS) $(OWENB_RING_OBJS)

$(ROOT)/build/test_owenb_ring_mut_skip_heartbeat: $(OWENB_RING_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -DMUT_OWENB_SKIP_HEARTBEAT -I$(OWENB_RING_DIR)/include -o $@ \
		$(ROOT)/tests/unit/test_owenb_ring.cpp $(OWENB_RING_SRCS) $(OWENB_RING_OBJS)

$(ROOT)/build/test_owenb_ring.mutant-check: $(ROOT)/build/test_owenb_ring \
		$(ROOT)/build/test_owenb_ring_mut_skip_heartbeat
	@$(ROOT)/build/test_owenb_ring > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@if $(ROOT)/build/test_owenb_ring_mut_skip_heartbeat > $@.log 2>&1; then \
		echo "MUTANT SURVIVED: test_owenb_ring_mut_skip_heartbeat"; exit 1; fi
	@echo "test_owenb_ring: mutant skip-heartbeat FAILS as required" | tee $@

# OWENB-P1B T3: the core gate on a PlexOB core: the CONF_STR rename hook (PlexOB accepted, stock H264 refused),
# ring-based identity (magic/version/geometry/advancing heartbeat + MPX BUILD_ID word 0x58), owenbOpen() with
# open() kept false, ringProbe routing in coreGateTickStep, sessionVideoBackend, a read-only ctrl-page map on a
# sparse file, and source rows on media_player.cpp (paintIdle's OB early return before gateRepaintOnce_).
# Each mutant must FAIL; --mutate-paint-order runs the source row on a reordered paintIdle and must FAIL.
OWENB_GATE_DEPS := $(ROOT)/tests/unit/test_owenb_core_gate.cpp $(ROOT)/host/libmisterplex/core_gate.hpp \
		$(ROOT)/host/libmisterplex/owenb_core_identity.hpp $(ROOT)/host/libmisterplex/video_backend.hpp \
		$(ROOT)/host/libmisterplex/fpga_build_ids.hpp $(ROOT)/arm/misterplexd/media_player.cpp $(OWENB_RING_HDRS)
OWENB_GATE_FLAGS := -I$(OWENB_RING_DIR)/include -pthread -DMPX_SOURCE_ROOT='"$(ROOT)"'
OWENB_GATE_MUTANTS := accept_h264:MUT_OWENB_GATE_ACCEPT_H264 skip_heartbeat:MUT_OWENB_ID_SKIP_HEARTBEAT \
		legacy_open:MUT_OWENB_GATE_LEGACY_OPEN backend_ignore_gate:MUT_BACKEND_IGNORE_GATE \
		drop_t10:MUT_OWENB_GATE_DROP_T10
OWENB_GATE_MUT_BINS := $(foreach m,$(OWENB_GATE_MUTANTS),$(ROOT)/build/test_owenb_core_gate_mut_$(word 1,$(subst :, ,$(m))))

$(ROOT)/build/test_owenb_core_gate: $(OWENB_GATE_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(OWENB_GATE_FLAGS) -o $@ $(ROOT)/tests/unit/test_owenb_core_gate.cpp

$(ROOT)/build/test_owenb_core_gate_mut_%: $(OWENB_GATE_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(OWENB_GATE_FLAGS) \
		-D$(word 2,$(subst :, ,$(filter $*:%,$(OWENB_GATE_MUTANTS)))) -o $@ \
		$(ROOT)/tests/unit/test_owenb_core_gate.cpp

$(ROOT)/build/test_owenb_core_gate.mutant-check: $(ROOT)/build/test_owenb_core_gate $(OWENB_GATE_MUT_BINS)
	@$(ROOT)/build/test_owenb_core_gate > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in $(foreach x,$(OWENB_GATE_MUTANTS),$(word 1,$(subst :, ,$(x)))); do \
		if $(ROOT)/build/test_owenb_core_gate_mut_$$m > $@.log 2>&1; then echo "MUTANT SURVIVED: $$m"; exit 1; fi; done
	@if $(ROOT)/build/test_owenb_core_gate --mutate-paint-order > $@.log 2>&1; then \
		echo "MUTANT SURVIVED: test_owenb_core_gate --mutate-paint-order"; exit 1; fi
	@echo "test_owenb_core_gate: 5 mutants + paint-order mutant FAIL as required" | tee $@

# OWENB-P1B T5: the OB play route. OwenbPlaySession (host/libmisterplex/owenb_session.*: AUs with PTS through the
# library's CAVLC->CABAC rewrite, the OB_CAVLC_MAX_KBPS guard, 48 kHz PCM records with PTS, pause / held seek / stop /
# end hold / volume, errors and teardown) and the daemon's worker (owenb_play.*), on the paced fake core
# (tests/unit/fake_owenb_core.hpp) with real H.264 (tests/fixtures/owenb_t5). Each session mutant must FAIL.
# test_owenb_av_e2e adds the real libav input (arm/misterplexd/owenb_av_source.cpp: mp4 demux, AAC decode + 44.1 ->
# 48 kHz resample, a container seek); it builds only where the FFmpeg dev libs are (COMPRESSED_SYNTAX_OK pattern).
# T6: the OB overlay renderer's headers (owenb_play.hpp includes them).
OWENB_OSD_HDRS := $(ROOT)/host/libmisterplex/owenb_osd_layout.hpp $(ROOT)/host/libmisterplex/owenb_osd_draw.hpp \
		$(ROOT)/host/libmisterplex/owenb_osd_renderer.hpp $(ROOT)/host/libmisterplex/owenb_aspect_guard.hpp \
		$(ROOT)/host/libmisterplex/owenb_aspect.hpp $(ROOT)/host/libmisterplex/h264_sps.hpp \
		$(ROOT)/host/libmisterplex/fpga_playback_overlay.hpp \
		$(ROOT)/host/libmisterplex/playback_overlay.hpp
OWENB_PLAY_SRCS := $(ROOT)/host/libmisterplex/owenb_ring.cpp $(ROOT)/host/libmisterplex/owenb_session.cpp \
		$(ROOT)/host/libmisterplex/owenb_play.cpp
OWENB_PLAY_HDRS := $(ROOT)/host/libmisterplex/owenb_ring.hpp $(ROOT)/host/libmisterplex/owenb_session.hpp \
		$(ROOT)/host/libmisterplex/owenb_play.hpp $(ROOT)/host/libmisterplex/owenb_ddr_window.hpp \
		$(ROOT)/host/libmisterplex/h264_bitstream_transport.hpp $(OWENB_OSD_HDRS) $(OWENB_RING_HDRS)
OWENB_SESS_DEPS := $(ROOT)/tests/unit/test_owenb_session.cpp $(ROOT)/tests/unit/fake_owenb_core.hpp \
		$(OWENB_PLAY_SRCS) $(OWENB_PLAY_HDRS) $(OWENB_RING_OBJS) \
		$(ROOT)/tests/fixtures/owenb_t5/cabac_160x96_2s.264 $(ROOT)/tests/fixtures/owenb_t5/cavlc_320x240_2s.264
OWENB_SESS_FLAGS := -I$(OWENB_RING_DIR)/include -pthread -DMPX_SOURCE_ROOT='"$(ROOT)"'
OWENB_SESS_MUTANTS := no_eos:MUT_OWENB_SESS_NO_EOS seek_no_flush:MUT_OWENB_SESS_SEEK_NO_FLUSH \
		pause_reads:MUT_OWENB_SESS_PAUSE_READS no_cavlc_guard:MUT_OWENB_SESS_NO_CAVLC_GUARD \
		pcm_no_pts:MUT_OWENB_SESS_PCM_NO_PTS cavlc_rate_only:MUT_OWENB_SESS_CAVLC_RATE_ONLY \
		no_hard_cap:MUT_OWENB_SESS_NO_HARD_CAP no_entropy_report:MUT_OWENB_SESS_NO_ENTROPY_REPORT \
		unbounded_out:MUT_OWENB_SESS_UNBOUNDED_OUT ignore_sendready:MUT_OWENB_SESS_IGNORE_SENDREADY
OWENB_SESS_MUT_BINS := $(foreach m,$(OWENB_SESS_MUTANTS),$(ROOT)/build/test_owenb_session_mut_$(word 1,$(subst :, ,$(m))))

$(ROOT)/build/test_owenb_session: $(OWENB_SESS_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -o $@ $(ROOT)/tests/unit/test_owenb_session.cpp $(OWENB_PLAY_SRCS) \
		$(OWENB_RING_OBJS) -lm

$(ROOT)/build/test_owenb_session_mut_%: $(OWENB_SESS_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) \
		-D$(word 2,$(subst :, ,$(filter $*:%,$(OWENB_SESS_MUTANTS)))) -o $@ \
		$(ROOT)/tests/unit/test_owenb_session.cpp $(OWENB_PLAY_SRCS) $(OWENB_RING_OBJS) -lm

$(ROOT)/build/test_owenb_session.mutant-check: $(ROOT)/build/test_owenb_session $(OWENB_SESS_MUT_BINS)
	@$(ROOT)/build/test_owenb_session > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in $(foreach x,$(OWENB_SESS_MUTANTS),$(word 1,$(subst :, ,$(x)))); do \
		if timeout 120 $(ROOT)/build/test_owenb_session_mut_$$m > $@.log 2>&1; then echo "MUTANT SURVIVED: $$m"; exit 1; fi; done
	@echo "test_owenb_session: $(words $(OWENB_SESS_MUTANTS)) mutants FAIL as required" | tee $@

$(ROOT)/build/test_owenb_play: $(ROOT)/tests/unit/test_owenb_play.cpp $(ROOT)/tests/unit/fake_owenb_core.hpp \
		$(OWENB_PLAY_SRCS) $(OWENB_PLAY_HDRS) $(OWENB_RING_OBJS) $(ROOT)/arm/misterplexd/media_player.cpp \
		$(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/main.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/arm/misterplexd/playback_controls.hpp $(ROOT)/arm/misterplexd/owenb_av_source.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -o $@ $(ROOT)/tests/unit/test_owenb_play.cpp $(OWENB_PLAY_SRCS) \
		$(OWENB_RING_OBJS) -lm

$(ROOT)/build/test_owenb_timeline_heartbeat: $(ROOT)/tests/unit/test_owenb_timeline_heartbeat.cpp \
		$(ROOT)/tests/unit/fake_owenb_core.hpp $(OWENB_PLAY_SRCS) $(OWENB_PLAY_HDRS) $(OWENB_RING_OBJS) \
		$(ROOT)/arm/misterplexd/pms_timeline.cpp $(ROOT)/arm/misterplexd/pms_timeline.hpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/arm/misterplexd/plex_resolve.hpp \
		$(ROOT)/arm/misterplexd/media_player.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -I$(ROOT)/arm/misterplexd -o $@ \
		$(ROOT)/tests/unit/test_owenb_timeline_heartbeat.cpp $(OWENB_PLAY_SRCS) $(OWENB_RING_OBJS) \
		$(ROOT)/arm/misterplexd/pms_timeline.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp -lm

# BUG2 heartbeat mutants (M1-M7). The script rebuilds the test per mutant and requires a failure.
$(ROOT)/build/test_owenb_timeline_heartbeat_mutants.ok: \
		$(ROOT)/tests/unit/test_owenb_timeline_heartbeat_mutants.sh \
		$(ROOT)/tests/unit/test_owenb_timeline_heartbeat.cpp \
		$(ROOT)/arm/misterplexd/media_player.cpp $(ROOT)/host/libmisterplex/owenb_play.cpp
	bash $(ROOT)/tests/unit/test_owenb_timeline_heartbeat_mutants.sh
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_owenb_timeline_heartbeat_mutants.ok

# BUG2-PREROLL: a pre-roll (type="clip") always advances at natural EOF, even with AUTO_NEXT=0.
# The mutant check rebuilds with each MUT_PREROLL_* and requires the test to FAIL.
$(ROOT)/build/test_queue_preroll: $(ROOT)/tests/unit/test_queue_preroll.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/arm/misterplexd/plex_resolve.hpp $(ROOT)/arm/misterplexd/queue_step.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -o $@ $(ROOT)/tests/unit/test_queue_preroll.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp

$(ROOT)/build/test_queue_preroll.mutant-check: $(ROOT)/build/test_queue_preroll
	@$(ROOT)/build/test_queue_preroll > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@set -e; for m in MUT_PREROLL_ANY_TYPE MUT_PREROLL_IGNORED; do \
		$(CXX) $(CXXFLAGS) -D$$m -I$(ROOT)/arm/misterplexd -o $(ROOT)/build/test_queue_preroll.$$m $(ROOT)/tests/unit/test_queue_preroll.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp; \
		if $(ROOT)/build/test_queue_preroll.$$m >/dev/null 2>&1; then echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
		echo "mutant caught: $$m"; done; touch $@

$(ROOT)/build/test_companion_queue_version: $(ROOT)/tests/unit/test_companion_queue_version.cpp \
		$(ROOT)/host/libmisterplex/play_queue_version.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_companion_queue_version.cpp

# OWENB-P1B T6: the ARM overlay renderer for the OB play (host/libmisterplex/owenb_osd_{layout,draw,renderer}.hpp,
# ARM-RENDERER-SPEC-1017 + its Decisions): layout x fb0 size, the page plan (8888 double-buffered, the single-page
# runtime fallback), straight-alpha pixels composited through a model of the core (PNG + sha256 goldens in
# tests/fixtures/owenb_t6), the flip state machine on a fake core, the Letterbox default + its restore (0006), OwenbPlayer + OwenbRing on
# the paced fake core, source rows. Each mutant must FAIL. test_owenb_osd.ovl-ref-check cross-checks the pages with
# owenb's tools/ovl_ref.py (OVL_REF_TOOLS; SKIP where it is not on the machine).
OWENB_OSD_DEPS := $(ROOT)/tests/unit/test_owenb_osd.cpp $(ROOT)/tests/unit/fake_owenb_core.hpp $(ROOT)/tests/unit/h264_sps_builder.hpp \
		$(ROOT)/host/libmisterplex/owenb_input.hpp $(ROOT)/host/libmisterplex/owenb_menu.hpp \
		$(ROOT)/host/libmisterplex/owenb_library.hpp \
		$(ROOT)/tests/fixtures/owenb_t6/goldens.txt $(OWENB_PLAY_SRCS) $(OWENB_PLAY_HDRS) $(OWENB_RING_OBJS) \
		$(ROOT)/arm/misterplexd/media_player.cpp $(ROOT)/arm/misterplexd/main.cpp \
		$(ROOT)/arm/misterplexd/owenb_av_source.cpp
OWENB_OSD_MUTANTS := premult:MUT_OSD_PREMULT reuse_before_gen:MUT_OSD_REUSE_BEFORE_GEN \
		8mb_not_smem:MUT_OSD_8MB_NOT_SMEM single_no_blank:MUT_OSD_SINGLE_NO_BLANK \
		aspect_no_restore:MUT_ASPECT_NO_RESTORE aspect_clear_unacked:MUT_ASPECT_CLEAR_UNACKED \
		idle_no_restart:MUT_IDLE_NO_RESTART idle_teardown_on_stop:MUT_IDLE_TEARDOWN_ON_STOP \
		aspect43_ignore_user:MUT_ASPECT43_IGNORE_USER aspect43_no_restore:MUT_ASPECT43_NO_RESTORE \
		aspect43_any_vui:MUT_ASPECT43_ANY_VUI card_under_page:MUT_OSD_CARD_UNDER_PAGE \
		card_no_shrink:MUT_OSD_CARD_NO_SHRINK aspect43_native_square:MUT_ASPECT43_NATIVE_SQUARE \
		f3_no_rearm:MUT_F3_NO_REARM f3_bg_not_black:MUT_F3_IDLE_BG_NOT_BLACK \
		f3_card_see_through:MUT_F3_CARD_SEE_THROUGH f5_card_centred:MUT_F5_CARD_CENTRED
OWENB_OSD_MUT_BINS := $(foreach m,$(OWENB_OSD_MUTANTS),$(ROOT)/build/test_owenb_osd_mut_$(word 1,$(subst :, ,$(m))))
OVL_REF_TOOLS ?=

$(ROOT)/build/test_owenb_osd: $(OWENB_OSD_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -o $@ $(ROOT)/tests/unit/test_owenb_osd.cpp $(OWENB_PLAY_SRCS) \
		$(OWENB_RING_OBJS) -lm

# OB port: the PlexUI full-screen surface in the OB overlay renderer (+ a mutant that must FAIL).
$(ROOT)/build/test_owenb_osd_plexui: $(OWENB_OSD_DEPS) $(ROOT)/tests/unit/test_owenb_osd_plexui.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -o $@ $(ROOT)/tests/unit/test_owenb_osd_plexui.cpp $(OWENB_RING_OBJS) -lm

.PHONY: test-owenb-osd-plexui
test-owenb-osd-plexui: $(ROOT)/build/test_owenb_osd_plexui
	$(ROOT)/build/test_owenb_osd_plexui
	$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -DMUT_OSD_SURF_ONE_SHADOW -o $(ROOT)/build/test_owenb_osd_plexui_mut \
		$(ROOT)/tests/unit/test_owenb_osd_plexui.cpp $(OWENB_RING_OBJS) -lm
	! $(ROOT)/build/test_owenb_osd_plexui_mut > /dev/null && echo "test_owenb_osd_plexui: mutant FAILS as required"
unit-unlocked unit-host-unlocked: test-owenb-osd-plexui

# OB port: ring_in nav -> PlexUI ops (header only)
.PHONY: test-owenb-plexui-nav
test-owenb-plexui-nav:
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -I$(ROOT)/host -I$(ROOT)/host/libmisterplex -I$(ROOT)/third_party/owenb-h264ring/include \
		-o $(ROOT)/build/test_owenb_plexui_nav $(ROOT)/tests/unit/test_owenb_plexui_nav.cpp
	$(ROOT)/build/test_owenb_plexui_nav
	$(CXX) $(CXXFLAGS) -I$(ROOT)/host -I$(ROOT)/host/libmisterplex -I$(ROOT)/third_party/owenb-h264ring/include -DMUT_PLEXUI_OB_HOME_BACK \
		-o $(ROOT)/build/test_owenb_plexui_nav_mut $(ROOT)/tests/unit/test_owenb_plexui_nav.cpp
	! $(ROOT)/build/test_owenb_plexui_nav_mut > /dev/null && echo "test_owenb_plexui_nav: mutant FAILS as required"

$(ROOT)/build/test_owenb_osd_mut_%: $(OWENB_OSD_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) \
		-D$(word 2,$(subst :, ,$(filter $*:%,$(OWENB_OSD_MUTANTS)))) -o $@ \
		$(ROOT)/tests/unit/test_owenb_osd.cpp $(OWENB_PLAY_SRCS) $(OWENB_RING_OBJS) -lm

$(ROOT)/build/test_owenb_osd.mutant-check: $(ROOT)/build/test_owenb_osd $(OWENB_OSD_MUT_BINS)
	@$(ROOT)/build/test_owenb_osd > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in $(foreach x,$(OWENB_OSD_MUTANTS),$(word 1,$(subst :, ,$(x)))); do \
		if timeout 120 $(ROOT)/build/test_owenb_osd_mut_$$m > $@.log 2>&1; then echo "MUTANT SURVIVED: $$m"; exit 1; fi; done
	@echo "test_owenb_osd: $(words $(OWENB_OSD_MUT_BINS)) mutants FAIL as required" | tee $@

$(ROOT)/build/test_owenb_osd.ovl-ref-check: $(ROOT)/build/test_owenb_osd $(ROOT)/tests/unit/owenb_osd_ovl_ref.py
	@rm -rf $(ROOT)/build/owenb_osd_dump
	@$(ROOT)/build/test_owenb_osd --dump $(ROOT)/build/owenb_osd_dump > $@.log 2>&1 || { cat $@.log; exit 1; }
	@OVL_REF_TOOLS=$(OVL_REF_TOOLS) python3 $(ROOT)/tests/unit/owenb_osd_ovl_ref.py $(ROOT)/build/owenb_osd_dump | tee -a $@.log | tail -1
	@cp $@.log $@

# P1b T6-T9 on the dev T5 branch. The same host pieces were already on the frozen T3 tip.
$(ROOT)/build/test_owenb_weak_profile: $(ROOT)/tests/unit/test_owenb_weak_profile.cpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/arm/misterplexd/plex_resolve.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -o $@ \
		$(ROOT)/tests/unit/test_owenb_weak_profile.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp

# BUG1 (lab 7AE78D80): the per-PMS memory (owenb_pms_traits.hpp). Each mutant must FAIL at its pinned check.
OWENB_TRAITS_MUTANTS := MUT_OB_TRAITS_CABAC_KEEPS:M8 MUT_OB_TRAITS_NO_TTL:M7 MUT_OB_TRAITS_NO_FLOOR:M5
$(ROOT)/build/test_owenb_pms_traits: $(ROOT)/tests/unit/test_owenb_pms_traits.cpp $(ROOT)/host/libmisterplex/owenb_pms_traits.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $<
$(ROOT)/build/test_owenb_pms_traits.mutant-check: $(ROOT)/build/test_owenb_pms_traits
	@$(ROOT)/build/test_owenb_pms_traits > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for mc in $(OWENB_TRAITS_MUTANTS); do m=$${mc%%:*}; c=$${mc##*:}; \
		$(CXX) $(CXXFLAGS) -D$$m -o $@.$$m $(ROOT)/tests/unit/test_owenb_pms_traits.cpp || exit 1; \
		if $@.$$m > $@.$$m.log 2>&1; then echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
		grep -q ": $$c " $@.$$m.log || { echo "MUTANT $$m KILLED BY THE WRONG CHECK (want $$c)"; cat $@.$$m.log; exit 1; }; \
	done
	@echo "test_owenb_pms_traits: $(words $(OWENB_TRAITS_MUTANTS)) mutants FAIL as required" | tee $@

# BUG1-CAP: the OB CAVLC retry's rate cap (owenbCavlcRetryLadder) and the pre-play check (owenbPrePlayCavlcLadder).
# Each mutant must FAIL at its pinned check.
OWENB_WP_MUTANTS := MUT_OWENB_CAVLC_RETRY_NO_CAP:B1 MUT_OB_PREPLAY_ANY_ENTROPY:Q4 MUT_OB_PREPLAY_NO_RATE_CHECK:Q3 \
		MUT_OB_PREPLAY_PROFILE_IGNORED:Q0 MUT_OB_PREPLAY_LEGACY_TOO:Q6 MUT_OB_TSAFE_LEGACY_TOO:T5 \
		MUT_OB_TSAFE_H264_TOO:T3 MUT_OB_CAP_DROPPED:B5
$(ROOT)/build/test_owenb_weak_profile.mutant-check: $(ROOT)/build/test_owenb_weak_profile
	@$(ROOT)/build/test_owenb_weak_profile > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for mc in $(OWENB_WP_MUTANTS); do m=$${mc%%:*}; c=$${mc##*:}; \
		$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -D$$m -o $@.$$m \
			$(ROOT)/tests/unit/test_owenb_weak_profile.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp || exit 1; \
		if $@.$$m > $@.$$m.log 2>&1; then echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
		grep -q "FAIL: $$c" $@.$$m.log || { echo "MUTANT $$m KILLED BY THE WRONG CHECK (want $$c)"; cat $@.$$m.log; exit 1; }; \
	done
	@echo "test_owenb_weak_profile: $(words $(OWENB_WP_MUTANTS)) mutants FAIL as required" | tee $@

$(ROOT)/build/test_owenb_overlay: $(ROOT)/tests/unit/test_owenb_overlay.cpp \
		$(ROOT)/host/libmisterplex/owenb_overlay.hpp $(ROOT)/host/libmisterplex/owenb_osd_layout.hpp $(OWENB_RING_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(OWENB_RING_DIR)/include -o $@ $(ROOT)/tests/unit/test_owenb_overlay.cpp \
		$(OWENB_RING_SRCS) $(OWENB_RING_OBJS)

$(ROOT)/build/test_owenb_aspect: $(ROOT)/tests/unit/test_owenb_aspect.cpp \
		$(ROOT)/host/libmisterplex/owenb_aspect.hpp $(ROOT)/host/libmisterplex/h264_sps.hpp \
		$(ROOT)/tests/unit/h264_sps_builder.hpp $(OWENB_RING_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(OWENB_RING_DIR)/include -o $@ $(ROOT)/tests/unit/test_owenb_aspect.cpp \
		$(OWENB_RING_SRCS) $(OWENB_RING_OBJS)

# OWENB-P1B 0009 (T6 gap G15): fbcon detached from the console while the OB overlay holds fb0
# (host/libmisterplex/owenb_fbcon_guard.hpp), on a temp vtconsole tree. Mutants: no rebind at the
# release; rebinding a console the user had detached.
OWENB_FBCON_MUTANTS := no_rebind:MUT_FBCON_NO_REBIND rebind_all:MUT_FBCON_REBIND_ALL
OWENB_FBCON_MUT_BINS := $(foreach m,$(OWENB_FBCON_MUTANTS),$(ROOT)/build/test_owenb_fbcon_mut_$(word 1,$(subst :, ,$(m))))
$(ROOT)/build/test_owenb_fbcon: $(ROOT)/tests/unit/test_owenb_fbcon.cpp $(ROOT)/host/libmisterplex/owenb_fbcon_guard.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_owenb_fbcon.cpp

$(ROOT)/build/test_owenb_fbcon_mut_%: $(ROOT)/tests/unit/test_owenb_fbcon.cpp $(ROOT)/host/libmisterplex/owenb_fbcon_guard.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -D$(word 2,$(subst :, ,$(filter $*:%,$(OWENB_FBCON_MUTANTS)))) -o $@ \
		$(ROOT)/tests/unit/test_owenb_fbcon.cpp

$(ROOT)/build/test_owenb_fbcon.mutant-check: $(ROOT)/build/test_owenb_fbcon $(OWENB_FBCON_MUT_BINS)
	@$(ROOT)/build/test_owenb_fbcon > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in $(foreach x,$(OWENB_FBCON_MUTANTS),$(word 1,$(subst :, ,$(x)))); do \
		if $(ROOT)/build/test_owenb_fbcon_mut_$$m > $@.log 2>&1; then echo "MUTANT SURVIVED: $$m"; exit 1; fi; done
	@echo "test_owenb_fbcon: $(words $(OWENB_FBCON_MUT_BINS)) mutants FAIL as required" | tee $@

# OWENB-P1B 0010 (T6 gap G2): the OB input decoder (owenb_input.hpp: the core's input region, h264ring.h:382-421),
# the menu panel fed from the daemon's state and the routing (owenb_menu.hpp), the library page's rows
# (owenb_library.hpp). The drawn menu / library pages are test_owenb_osd's goldens. Each mutant must FAIL.
OWENB_INPUT_HDRS := $(ROOT)/host/libmisterplex/owenb_input.hpp $(ROOT)/host/libmisterplex/owenb_menu.hpp \
		$(ROOT)/host/libmisterplex/owenb_library.hpp $(ROOT)/host/libmisterplex/library_browser.hpp \
		$(ROOT)/host/libmisterplex/input_mailbox.hpp $(OWENB_OSD_HDRS) $(OWENB_RING_HDRS)
OWENB_INPUT_MUTANTS := act_in_osd:MUT_INPUT_ACT_IN_OSD level:MUT_INPUT_LEVEL menu_stale:MUT_MENU_STALE \
		home_stops_play:MUT_ROUTE_HOME_STOPS_PLAY window_from_top:MUT_LIBRARY_WINDOW_FROM_TOP \
		info_selectable:MUT_LIBRARY_INFO_SELECTABLE play_no_transport:MUT_ROUTE_PLAY_NO_TRANSPORT
OWENB_INPUT_MUT_BINS := $(foreach m,$(OWENB_INPUT_MUTANTS),$(ROOT)/build/test_owenb_input_mut_$(word 1,$(subst :, ,$(m))))
$(ROOT)/build/test_owenb_input: $(ROOT)/tests/unit/test_owenb_input.cpp $(OWENB_INPUT_HDRS) $(OWENB_RING_OBJS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(OWENB_RING_DIR)/include -o $@ $(ROOT)/tests/unit/test_owenb_input.cpp $(OWENB_RING_OBJS)

$(ROOT)/build/test_owenb_input_mut_%: $(ROOT)/tests/unit/test_owenb_input.cpp $(OWENB_INPUT_HDRS) $(OWENB_RING_OBJS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(OWENB_RING_DIR)/include -D$(word 2,$(subst :, ,$(filter $*:%,$(OWENB_INPUT_MUTANTS)))) \
		-o $@ $(ROOT)/tests/unit/test_owenb_input.cpp $(OWENB_RING_OBJS)

$(ROOT)/build/test_owenb_input.mutant-check: $(ROOT)/build/test_owenb_input $(OWENB_INPUT_MUT_BINS)
	@$(ROOT)/build/test_owenb_input > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in $(foreach x,$(OWENB_INPUT_MUTANTS),$(word 1,$(subst :, ,$(x)))); do \
		if $(ROOT)/build/test_owenb_input_mut_$$m > $@.log 2>&1; then echo "MUTANT SURVIVED: $$m"; exit 1; fi; done
	@echo "test_owenb_input: $(words $(OWENB_INPUT_MUT_BINS)) mutants FAIL as required" | tee $@

$(ROOT)/build/test_owenb_core_select: $(ROOT)/tests/unit/test_owenb_core_select.cpp \
		$(ROOT)/host/libmisterplex/core_id_select.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -DMPX_SOURCE_ROOT='"$(ROOT)"' -o $@ $(ROOT)/tests/unit/test_owenb_core_select.cpp

OWENB_AV_E2E_OK := $(shell pkg-config --exists $(L2_DEMUX_AV) 2>/dev/null && echo 1)
$(ROOT)/build/test_owenb_av_e2e: $(ROOT)/tests/unit/test_owenb_av_e2e.cpp $(ROOT)/tests/unit/fake_owenb_core.hpp \
		$(ROOT)/arm/misterplexd/owenb_av_source.cpp $(ROOT)/arm/misterplexd/owenb_av_source.hpp \
		$(OWENB_PLAY_SRCS) $(OWENB_PLAY_HDRS) $(OWENB_RING_OBJS) \
		$(ROOT)/tests/fixtures/owenb_t5/cabac_aac44k_2s.mp4 $(ROOT)/tests/fixtures/owenb_t5/cavlc_aac48k_2s.mp4
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -DMPX_HAVE_LIBAV -I$(ROOT)/arm/misterplexd \
		$(shell pkg-config --cflags $(L2_DEMUX_AV)) -o $@ $(ROOT)/tests/unit/test_owenb_av_e2e.cpp \
		$(ROOT)/arm/misterplexd/owenb_av_source.cpp $(OWENB_PLAY_SRCS) $(OWENB_RING_OBJS) \
		$(shell pkg-config --libs $(L2_DEMUX_AV)) -lm

unit-unlocked unit-host-unlocked: $(if $(OWENB_AV_E2E_OK),$(ROOT)/build/test_owenb_av_e2e)

# GATE7-1018: the OB libav open must not hold the MPEG-TS codec-probe backlog (~350 MB per yt.ts on the ARM libav).
# Host run (any libav) + the regression proper: the same test built with ARM_CXX against the ARM static libav and
# run under qemu-arm-static (make owenb-probe-mem-arm ARM_CXX=... ARM_FFMPEG_PREFIX=...).
OWENB_PROBE_MEM_FIX := $(ROOT)/tests/fixtures/owenb_t5/cabac_aac44k_6s.ts
$(ROOT)/build/test_owenb_probe_mem: $(ROOT)/tests/unit/test_owenb_probe_mem.cpp \
		$(ROOT)/arm/misterplexd/owenb_av_source.cpp $(ROOT)/arm/misterplexd/owenb_av_source.hpp \
		$(OWENB_PLAY_SRCS) $(OWENB_PLAY_HDRS) $(OWENB_RING_OBJS) $(OWENB_PROBE_MEM_FIX) \
		$(ROOT)/scripts/build_arm_ffmpeg.sh $(ROOT)/scripts/check_arm_ffmpeg_static.py
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -DMPX_HAVE_LIBAV -I$(ROOT)/arm/misterplexd \
		$(shell pkg-config --cflags $(L2_DEMUX_AV)) -o $@ $(ROOT)/tests/unit/test_owenb_probe_mem.cpp \
		$(ROOT)/arm/misterplexd/owenb_av_source.cpp $(OWENB_PLAY_SRCS) $(OWENB_RING_OBJS) \
		$(shell pkg-config --libs $(L2_DEMUX_AV)) -lm

$(ROOT)/build/test_owenb_probe_mem.check: $(ROOT)/build/test_owenb_probe_mem
	$(ROOT)/build/test_owenb_probe_mem $(OWENB_PROBE_MEM_FIX)

unit-unlocked unit-host-unlocked: $(if $(OWENB_AV_E2E_OK),$(ROOT)/build/test_owenb_probe_mem.check)

# Gate 7 host RSS: repeated fake-core plays of the short CABAC clip plus poster
# churn. ASan+LSan, quarantine off, so a leak or a queue that retains shows up
# as RSS growth after the warm-up play.
$(ROOT)/build/test_yt_ui_rss_asan: $(ROOT)/tests/unit/test_yt_ui_rss.cpp $(OWENB_PLAY_SRCS) $(OWENB_PLAY_HDRS) \
		$(OWENB_RING_OBJS) $(PLEXUI_IMGCACHE_TEST_EXTRA) $(PLEXUI_IMG_CACHE_SRC) \
		$(ROOT)/tests/fixtures/owenb_t5/cabac_160x96_2s.264
	@mkdir -p $(ROOT)/build
	$(CXX) $(PLEXUI_ASAN_CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST $(OWENB_SESS_FLAGS) \
		-I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui -o $@ \
		$(ROOT)/tests/unit/test_yt_ui_rss.cpp $(OWENB_PLAY_SRCS) $(OWENB_RING_OBJS) \
		$(PLEXUI_IMGCACHE_TEST_EXTRA) $(PLEXUI_IMG_CACHE_SRC) -lm

$(ROOT)/build/test_yt_ui_rss_asan.check: $(ROOT)/build/test_yt_ui_rss_asan
	ASAN_OPTIONS=detect_leaks=1:quarantine_size_mb=0 $(ROOT)/build/test_yt_ui_rss_asan
	@touch $@

unit-unlocked unit-host-unlocked: $(ROOT)/build/test_yt_ui_rss_asan.check

.PHONY: owenb-probe-mem-arm
owenb-probe-mem-arm: $(ROOT)/tests/unit/test_owenb_probe_mem.cpp $(OWENB_PROBE_MEM_FIX)
	@test -n "$(ARM_CXX)" && test -d "$(ARM_FFMPEG_PREFIX)/lib" || { echo "owenb-probe-mem-arm: set ARM_CXX and ARM_FFMPEG_PREFIX"; exit 2; }
	@mkdir -p $(ROOT)/build/arm/probe-mem
	for c in $(OWENB_RING_DIR)/lib/*.c; do $(ARM_CXX:g++=gcc) -O1 -I$(OWENB_RING_DIR)/include -c $$c \
		-o $(ROOT)/build/arm/probe-mem/$$(basename $$c .c).o || exit 1; done
	$(ARM_CXX) -std=c++17 -O1 -static $(OWENB_SESS_FLAGS) -DMPX_HAVE_LIBAV -I$(ROOT)/host -I$(ROOT)/host/libmisterplex \
		-I$(ROOT)/arm/misterplexd -I$(ARM_FFMPEG_PREFIX)/include -o $(ROOT)/build/arm/probe-mem/test_owenb_probe_mem \
		$(ROOT)/tests/unit/test_owenb_probe_mem.cpp $(ROOT)/arm/misterplexd/owenb_av_source.cpp $(OWENB_PLAY_SRCS) \
		$(ROOT)/build/arm/probe-mem/*.o $(addprefix $(ARM_FFMPEG_PREFIX)/lib/,libavformat.a libavcodec.a libswresample.a libavutil.a) -lm
	qemu-arm-static $(ROOT)/build/arm/probe-mem/test_owenb_probe_mem $(OWENB_PROBE_MEM_FIX)

# The armhf objects of the vendored lib for arm-plexd-daemon (the daemon links the OB play on ARM).
OWENB_RING_ARM_OBJS := $(patsubst %,$(ROOT)/build/arm/owenb-h264ring/%.o,h264ring h264ring_au h264ring_cavlc h264ring_dump h264ring_linux)
$(ROOT)/build/arm/owenb-h264ring/%.o: $(OWENB_RING_DIR)/lib/%.c $(OWENB_RING_HDRS)
	@if [ -z "$(ARM_CXX)" ]; then echo "No armhf g++ found"; exit 1; fi
	@mkdir -p $(ROOT)/build/arm/owenb-h264ring
	$(patsubst %g++,%gcc,$(ARM_CXX)) $(OWENB_RING_CFLAGS) -c -o $@ $<

# review 08:17 (b) / 08:18: D52-LOG reads the PLX page only through the daemon's one mapper
# (FpgaSpi::plxPageRead32). Rows D-a..D-c on the real fpga_spi.cpp + the fake core; each mutant (d52 maps
# the page itself; close() does not detach the poller's reader) must FAIL.
D52_PLX_MAPPER_DEPS := $(ROOT)/tests/unit/test_d52_plx_mapper.cpp $(ROOT)/tests/unit/fake_plex_core.hpp \
	$(ROOT)/host/libmisterplex/d52_log.hpp $(ROOT)/arm/misterplexd/fpga_spi.cpp $(ROOT)/arm/misterplexd/fpga_spi.hpp
D52_PLX_MAPPER_FLAGS := -I$(ROOT)/arm/misterplexd -pthread -DMPX_CORE_GATE_START_OPEN \
	-DMPX_TEST_DDR_PAGE_ACCESS_DELAY_US=2000
D52_PLX_MAPPER_MUTANTS := MUT_D52_OWN_MMAP MUT_D52_NO_DETACH
$(ROOT)/build/test_d52_plx_mapper: $(D52_PLX_MAPPER_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(D52_PLX_MAPPER_FLAGS) -DMPX_DEVMEM_PATH='"$(ROOT)/build/test_d52_plx_mapper.fakemem"' \
		-o $@ $(ROOT)/tests/unit/test_d52_plx_mapper.cpp $(ROOT)/arm/misterplexd/fpga_spi.cpp
$(ROOT)/build/test_d52_plx_mapper.ok: $(ROOT)/build/test_d52_plx_mapper
	@$(ROOT)/build/test_d52_plx_mapper > $@.log 2>&1 || { cat $@.log; exit 1; }
	@cat $@.log
	@cp $@.log $@
$(ROOT)/build/test_d52_plx_mapper_%: $(D52_PLX_MAPPER_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(D52_PLX_MAPPER_FLAGS) -D$* \
		-DMPX_DEVMEM_PATH='"$(ROOT)/build/test_d52_plx_mapper_$*.fakemem"' \
		-o $@ $(ROOT)/tests/unit/test_d52_plx_mapper.cpp $(ROOT)/arm/misterplexd/fpga_spi.cpp
$(ROOT)/build/test_d52_plx_mapper.mutant-check: $(ROOT)/build/test_d52_plx_mapper \
		$(addprefix $(ROOT)/build/test_d52_plx_mapper_,$(D52_PLX_MAPPER_MUTANTS))
	@$(ROOT)/build/test_d52_plx_mapper > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in $(D52_PLX_MAPPER_MUTANTS); do \
		if $(ROOT)/build/test_d52_plx_mapper_$$m > $@.$$m.log 2>&1; then echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
	done
	@echo "test_d52_plx_mapper: mutants $(D52_PLX_MAPPER_MUTANTS) FAIL as required" | tee $@

unit-unlocked unit-host-unlocked: $(ROOT)/build/test_d52_plx_mapper.ok $(ROOT)/build/test_d52_plx_mapper.mutant-check
# R1 PLXH host side (r1_plxh.hpp; review 19:01 A, 19:16 (1)(c), 19:27 F5; K2) against the RTL
# model of the PLX slots. Each MUT_* build must FAIL.
# R1 PLXH host side (r1_plxh.hpp; review 19:01 A, 19:16 (1)(c), 19:27 F5; K2; 20:08 no-FB_VBL
# fallback; wW 20:05 restart seed; 20:23 prime) against the RTL model of the PLX slots. Each MUT_* build must FAIL.
R1_PLXH_DEPS := $(ROOT)/tests/unit/test_r1_plxh.cpp $(ROOT)/tests/unit/r1_plxh_rtl_model.hpp \
	$(ROOT)/host/libmisterplex/r1_plxh.hpp $(ROOT)/host/libmisterplex/r1_idle_fb.hpp \
	$(ROOT)/host/libmisterplex/mplex_ddr_abi.hpp
R1_PLXH_MUTANTS := MUT_PLXH_MAGIC_FIRST MUT_PLXH_EN1_IN_SESSION MUT_F5_PAINT_AFTER_1_LATCH \
	MUT_PLXH_FALLBACK_ON_CHANGE MUT_PLXH_NO_RECHECK MUT_PLXH_NO_SEED MUT_PLXH_NO_PRIME MUT_PLXH_PRIME_EN0
$(ROOT)/build/test_r1_plxh: $(R1_PLXH_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_r1_plxh.cpp
$(ROOT)/build/test_r1_plxh_%: $(R1_PLXH_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -D$* -o $@ $(ROOT)/tests/unit/test_r1_plxh.cpp
$(ROOT)/build/test_r1_plxh.mutant-check: $(ROOT)/build/test_r1_plxh \
		$(addprefix $(ROOT)/build/test_r1_plxh_,$(R1_PLXH_MUTANTS))
	@$(ROOT)/build/test_r1_plxh > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in $(R1_PLXH_MUTANTS); do \
		if $(ROOT)/build/test_r1_plxh_$$m > $@.$$m.log 2>&1; then echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
	done
	@echo "test_r1_plxh: mutants $(R1_PLXH_MUTANTS) FAIL as required" | tee $@

unit-unlocked unit-host-unlocked: $(ROOT)/build/test_r1_plxh $(ROOT)/build/test_r1_plxh.mutant-check

# R1 idle FB paint target + entry clear (r1_idle_paint.hpp; review 20:18 STATE trigger): rows
# R-a..R-i and S0 against a model of fb480 + the reader's clears; review 06:57 D2/D3/D1 rows R-j
# (session END re-shows the idle FB), R-k (no en=1 from the PLAY request to BEGIN), R-l (a refused
# flip is retried); review 07:30 C1 row R-m (the PLAY-request hold is token-matched: no stale
# end, enter or STOP releases a newer request's hold) with the 08:17 terms (the STOP's own generation,
# an owned hold through a 45 s resolve, a throw, the library double bump). Each MUT_* build must FAIL.
R1_IDLE_ENTRY_DEPS := $(ROOT)/tests/unit/test_r1_idle_entry.cpp $(ROOT)/tests/unit/browse_render_test_common.hpp \
	$(ROOT)/host/libmisterplex/r1_idle_paint.hpp $(ROOT)/host/libmisterplex/browse_paint.hpp \
	$(ROOT)/host/libmisterplex/browse_render.hpp $(ROOT)/host/libmisterplex/r1_plxh.hpp \
	$(ROOT)/host/libmisterplex/r1_idle_fb.hpp $(ROOT)/host/libmisterplex/idle_screen.hpp \
	$(ROOT)/arm/misterplexd/playback_controls.hpp $(ROOT)/arm/misterplexd/companion.hpp
R1_IDLE_ENTRY_MUTANTS := MUT_R1_NO_ENTRY_CLEAR MUT_R1_ENTRY_CLEAR_IN_SESSION MUT_R1_ENTRY_CLEAR_RETRY \
	MUT_R1_ENTRY_CLEAR_ANY_TOKEN MUT_R1_NO_SUPERSEDE MUT_R1_NO_SESSION_REARM MUT_R1_GATE_AT_BEGIN \
	MUT_R1_NO_FLIP_RETRY MUT_R1_HOLD_RELEASE_ANY MUT_R1_HOLD_EXPIRES_OWNED MUT_R1_STOP_LIVE_GEN MUT_F1_R1_ENTRY_CLEAR
$(ROOT)/build/test_r1_idle_entry: $(R1_IDLE_ENTRY_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -o $@ $(ROOT)/tests/unit/test_r1_idle_entry.cpp -lz
$(ROOT)/build/test_r1_idle_entry_%: $(R1_IDLE_ENTRY_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -D$* -o $@ $(ROOT)/tests/unit/test_r1_idle_entry.cpp -lz
$(ROOT)/build/test_r1_idle_entry.mutant-check: $(ROOT)/build/test_r1_idle_entry \
		$(addprefix $(ROOT)/build/test_r1_idle_entry_,$(R1_IDLE_ENTRY_MUTANTS))
	@env -u R1_HOLD_CASE $(ROOT)/build/test_r1_idle_entry > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in $(R1_IDLE_ENTRY_MUTANTS); do \
		if $(ROOT)/build/test_r1_idle_entry_$$m > $@.$$m.log 2>&1; then echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
	done
	@echo "test_r1_idle_entry: mutants $(R1_IDLE_ENTRY_MUTANTS) FAIL as required" | tee $@

unit-unlocked unit-host-unlocked: $(ROOT)/build/test_r1_idle_entry $(ROOT)/build/test_r1_idle_entry.mutant-check

# review 11:36 R2: R-m m8 (a throw releases) and m10 (a started doPlay only disowns) have their own
# mutants, applied to a scratch copy of host/libmisterplex only, so the tree and the ARM never change.
$(ROOT)/build/test_r1_hold_mutants.ok: $(R1_IDLE_ENTRY_DEPS) $(ROOT)/tests/unit/test_r1_hold_mutants.sh
	@mkdir -p $(ROOT)/build
	bash $(ROOT)/tests/unit/test_r1_hold_mutants.sh > $@.log 2>&1 || { cat $@.log; exit 1; }
	@tail -n 1 $@.log > $@ && cat $@

unit-unlocked unit-host-unlocked: $(ROOT)/build/test_r1_hold_mutants.ok

# review 06:57 D1: the R1 PLX slots at daemon start on the real fpga_spi.cpp (unmanaged gate, sparse
# 4 GiB fake /dev/mem, the start-up 320x240 frame map: len 0x7f000) answered by an RTL stand-in on
# the fake PLX page. The mutant MUT_R1_PLX_LAYOUT_PAGE (the pre-D1 layout mailbox page) must FAIL.
R1_PLX_PAGE_DEPS := $(ROOT)/tests/unit/test_r1_plx_page.cpp $(ROOT)/arm/misterplexd/fpga_spi.cpp \
	$(ROOT)/arm/misterplexd/fpga_spi.hpp $(ROOT)/host/libmisterplex/r1_plxh.hpp \
	$(ROOT)/host/libmisterplex/r1_idle_fb.hpp
$(ROOT)/build/test_r1_plx_page: $(R1_PLX_PAGE_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -DMPX_CORE_GATE_START_OPEN \
		-DMPX_DEVMEM_PATH='"$(ROOT)/build/test_r1_plx_page.fakemem"' -o $@ \
		$(ROOT)/tests/unit/test_r1_plx_page.cpp $(ROOT)/arm/misterplexd/fpga_spi.cpp
$(ROOT)/build/test_r1_plx_page_mut_layout_page: $(R1_PLX_PAGE_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -DMPX_CORE_GATE_START_OPEN -DMUT_R1_PLX_LAYOUT_PAGE \
		-DMPX_DEVMEM_PATH='"$(ROOT)/build/test_r1_plx_page_mut.fakemem"' -o $@ \
		$(ROOT)/tests/unit/test_r1_plx_page.cpp $(ROOT)/arm/misterplexd/fpga_spi.cpp
$(ROOT)/build/test_r1_plx_page.mutant-check: $(ROOT)/build/test_r1_plx_page \
		$(ROOT)/build/test_r1_plx_page_mut_layout_page
	@$(ROOT)/build/test_r1_plx_page > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@if $(ROOT)/build/test_r1_plx_page_mut_layout_page > $@.log 2>&1; then \
		echo "MUTANT SURVIVED: MUT_R1_PLX_LAYOUT_PAGE"; exit 1; fi
	@echo "test_r1_plx_page: mutant MUT_R1_PLX_LAYOUT_PAGE FAILS as required" | tee $@

unit-unlocked unit-host-unlocked: $(ROOT)/build/test_r1_plx_page $(ROOT)/build/test_r1_plx_page.mutant-check

# review 07:30 C2: the R1 PLX page under concurrency on the real fpga_spi.cpp: 8 threads' first PLX
# access maps the page once (1 mmap, 1 fd), and close() waits for a first map (nothing left after it).
# MPX_TEST_DDR_PAGE_MAP_DELAY_US widens the first map's window (canon's one doorbell-page mapper);
# mutant MUT_DDR_PAGE_UNLOCKED (that mapper without its lock) must FAIL.
R1_PLX_RACE_DEPS := $(ROOT)/tests/unit/test_r1_plx_page_race.cpp $(ROOT)/arm/misterplexd/fpga_spi.cpp \
	$(ROOT)/arm/misterplexd/fpga_spi.hpp $(ROOT)/host/libmisterplex/r1_plxh.hpp
$(ROOT)/build/test_r1_plx_page_race: $(R1_PLX_RACE_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -DMPX_CORE_GATE_START_OPEN -DMPX_TEST_DDR_PAGE_MAP_DELAY_US=20000 \
		-DMPX_DEVMEM_PATH='"$(ROOT)/build/test_r1_plx_page_race.fakemem"' -o $@ \
		$(ROOT)/tests/unit/test_r1_plx_page_race.cpp $(ROOT)/arm/misterplexd/fpga_spi.cpp
$(ROOT)/build/test_r1_plx_page_race.ok: $(ROOT)/build/test_r1_plx_page_race
	@$(ROOT)/build/test_r1_plx_page_race > $@.log 2>&1 || { cat $@.log; exit 1; }
	@cat $@.log
	@cp $@.log $@
$(ROOT)/build/test_r1_plx_page_race_mut_unlocked: $(R1_PLX_RACE_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -DMPX_CORE_GATE_START_OPEN -DMPX_TEST_DDR_PAGE_MAP_DELAY_US=20000 \
		-DMUT_DDR_PAGE_UNLOCKED -DMPX_DEVMEM_PATH='"$(ROOT)/build/test_r1_plx_page_race_mut.fakemem"' -o $@ \
		$(ROOT)/tests/unit/test_r1_plx_page_race.cpp $(ROOT)/arm/misterplexd/fpga_spi.cpp
$(ROOT)/build/test_r1_plx_page_race.mutant-check: $(ROOT)/build/test_r1_plx_page_race \
		$(ROOT)/build/test_r1_plx_page_race_mut_unlocked
	@$(ROOT)/build/test_r1_plx_page_race > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@if $(ROOT)/build/test_r1_plx_page_race_mut_unlocked > $@.log 2>&1; then \
		echo "MUTANT SURVIVED: MUT_DDR_PAGE_UNLOCKED"; exit 1; fi
	@echo "test_r1_plx_page_race: mutant MUT_DDR_PAGE_UNLOCKED FAILS as required" | tee $@

unit-unlocked unit-host-unlocked: $(ROOT)/build/test_r1_plx_page_race.ok $(ROOT)/build/test_r1_plx_page_race.mutant-check

# Core gate stub: the real fpga_spi.cpp against a sparse fake /dev/mem (build dir).
# MPX_TEST_MAINSAFE_HOOK_ON: the fpga_spi.cpp:304 seam (T-CG-FIXTURE-HOOK); release builds leave it empty.
$(ROOT)/build/test_core_gate: $(ROOT)/tests/unit/test_core_gate.cpp \
		$(ROOT)/tests/unit/fake_plex_core.hpp \
		$(ROOT)/host/libmisterplex/core_gate.hpp $(ROOT)/host/libmisterplex/owenb_core_identity.hpp \
		$(ROOT)/arm/misterplexd/fpga_spi.cpp $(ROOT)/arm/misterplexd/fpga_spi.hpp \
		$(ROOT)/arm/misterplexd/media_player.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -DMPX_TEST_MAINSAFE_HOOK_ON \
		-DMPX_DEVMEM_PATH='"$(ROOT)/build/test_core_gate.fakemem"' \
		-DMPX_SOURCE_ROOT='"$(ROOT)"' -o $@ \
		$(ROOT)/tests/unit/test_core_gate.cpp $(ROOT)/arm/misterplexd/fpga_spi.cpp

# review 09:03 C1: the lock-order row's teeth. MUT_C1_LOCK_ORDER_ABBA (layout changes run holding
# spiMutex, the inverted order) must trip the row's watchdog: rc 2 and 'FAIL C1'. Run it under iso2,
# like test_core_gate.
$(ROOT)/build/test_core_gate_mut_c1_abba: $(ROOT)/tests/unit/test_core_gate.cpp \
		$(ROOT)/tests/unit/fake_plex_core.hpp \
		$(ROOT)/host/libmisterplex/core_gate.hpp $(ROOT)/host/libmisterplex/owenb_core_identity.hpp \
		$(ROOT)/arm/misterplexd/fpga_spi.cpp $(ROOT)/arm/misterplexd/fpga_spi.hpp \
		$(ROOT)/arm/misterplexd/media_player.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -DMUT_C1_LOCK_ORDER_ABBA -DMPX_TEST_MAINSAFE_HOOK_ON \
		-DMPX_DEVMEM_PATH='"$(ROOT)/build/test_core_gate_mut_c1.fakemem"' \
		-DMPX_SOURCE_ROOT='"$(ROOT)"' -o $@ \
		$(ROOT)/tests/unit/test_core_gate.cpp $(ROOT)/arm/misterplexd/fpga_spi.cpp

# Core gate, whole daemon in unshare -rn on a fake /dev/mem: idle and reload cases, then a
# real session, then v2.3's MrAudio session lane (reboot, refused/rejected Begin, killed
# owner, mid-play consumer reset, wedge) and its negative control on 3e5ea234 (host libav
# build; these skip without host libav and ffmpeg).
core-gate-e2e:
	bash $(ROOT)/tests/unit/test_core_gate_daemon_e2e.sh
	bash $(ROOT)/tests/unit/test_core_gate_session_e2e.sh
	bash $(ROOT)/tests/unit/test_audio_recovery_e2e.sh
	bash $(ROOT)/tests/unit/test_audio_recovery_negative.sh

# N2 (dev-480i 10/4; review F1-F5): an FFmpeg http EIO at the server's close of a
# length-less response (the ladder proxy) ends the stream only near the PMS duration. The rule
# alone (unit), through the demux on host libav with mutants, and the whole daemon on the
# coregate fixtures with its 2164c4ad control (these two skip without libav/ffmpeg/unshare).
$(ROOT)/build/test_input_close_eof: $(ROOT)/tests/unit/test_input_close_eof.cpp \
		$(ROOT)/host/libmisterplex/av_inproc_decode.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_input_close_eof.cpp

n2-eof: $(ROOT)/build/test_input_close_eof
	$(ROOT)/build/test_input_close_eof
	N2_EOF_MUTANTS=1 bash $(ROOT)/tests/unit/test_input_close_eof_demux.sh
	N2_EOF_E2E_MUTANTS=1 bash $(ROOT)/tests/unit/test_n2_eof_e2e.sh

# Core gate mutation check: unit, daemon e2e, session e2e, audio e2e and browse e2e mutants (about 90 min).
# The daemon/browse e2e classes start host daemons: run them only through heavy-sim-slot in a private
# PID + net namespace (rule B).
core-gate-mutants:
	python3 $(ROOT)/tests/unit/core_gate_mutants.py all

# MAST-WEDGE (review 21:14): the gate-open probe line, the reload marker and decision, and the
# detached launcher hand-off (host/libmisterplex/mast_reload.hpp). Each _MUT_ build must fail.
MAST_RELOAD_MUTANTS := MUT_MAST_RELOAD_LOOP MUT_MAST_KEEP_FDS MUT_MAST_RECOVER_FROM_REQUESTED MUT_MAST_SILENT_NO_GATE
$(ROOT)/build/test_mast_reload: $(ROOT)/tests/unit/test_mast_reload.cpp \
		$(ROOT)/host/libmisterplex/mast_reload.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_mast_reload.cpp
$(ROOT)/build/test_mast_reload_%: $(ROOT)/tests/unit/test_mast_reload.cpp \
		$(ROOT)/host/libmisterplex/mast_reload.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -D$* -o $@ $(ROOT)/tests/unit/test_mast_reload.cpp
$(ROOT)/build/test_mast_reload.mutant-check: $(ROOT)/build/test_mast_reload \
		$(addprefix $(ROOT)/build/test_mast_reload_,$(MAST_RELOAD_MUTANTS))
	@$(ROOT)/build/test_mast_reload > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in $(MAST_RELOAD_MUTANTS); do \
		if $(ROOT)/build/test_mast_reload_$$m > $@.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
	done
	@echo "test_mast_reload: mutants $(MAST_RELOAD_MUTANTS) FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_mast_reload $(ROOT)/build/test_mast_reload.mutant-check

# GET /misterplex/mast (MAST R5b follow-up; review 01:28/03:52): the read-only status reply
# (host/libmisterplex/mast_status_http.hpp). Each _MUT_ build must fail.
MAST_STATUS_MUTANTS := MUT_MAST_STATUS_ANY_VERB MUT_MAST_STATUS_NO_CAP
MAST_STATUS_DEPS := $(ROOT)/tests/unit/test_mast_status_http.cpp \
	$(ROOT)/host/libmisterplex/mast_status_http.hpp $(ROOT)/host/libmisterplex/mast_reload.hpp
$(ROOT)/build/test_mast_status_http: $(MAST_STATUS_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_mast_status_http.cpp
$(ROOT)/build/test_mast_status_http_%: $(MAST_STATUS_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -D$* -o $@ $(ROOT)/tests/unit/test_mast_status_http.cpp
$(ROOT)/build/test_mast_status_http.mutant-check: $(ROOT)/build/test_mast_status_http \
		$(addprefix $(ROOT)/build/test_mast_status_http_,$(MAST_STATUS_MUTANTS))
	@$(ROOT)/build/test_mast_status_http > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in $(MAST_STATUS_MUTANTS); do \
		if $(ROOT)/build/test_mast_status_http_$$m > $@.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
	done
	@echo "test_mast_status_http: mutants $(MAST_STATUS_MUTANTS) FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_mast_status_http $(ROOT)/build/test_mast_status_http.mutant-check
# The same route through the real Companion::httpLoop on a loopback port (status lines, read-only
# before cast/token handling, the request log covers it). The marker lives under build/.
$(ROOT)/build/test_companion_mast_status: $(ROOT)/tests/unit/test_companion_mast_status.cpp \
		$(MAST_STATUS_DEPS) $(ROOT)/arm/misterplexd/companion.cpp \
		$(ROOT)/arm/misterplexd/companion.hpp $(ROOT)/arm/misterplexd/companion_reqlog.hpp \
		$(ROOT)/arm/misterplexd/pms_timeline.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread \
		-DMPX_MAST_RELOAD_MARKER_PATH='"$(ROOT)/build/test_companion_mast_status.marker"' -o $@ \
		$(ROOT)/tests/unit/test_companion_mast_status.cpp $(ROOT)/arm/misterplexd/companion.cpp \
		$(ROOT)/arm/misterplexd/pms_timeline.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_companion_mast_status
$(ROOT)/build/test_companion_mast_status.mutant-check: $(ROOT)/build/test_companion_mast_status \
		$(ROOT)/tests/unit/test_companion_mast_status_mutants.sh
	@$(ROOT)/build/test_companion_mast_status > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@bash $(ROOT)/tests/unit/test_companion_mast_status_mutants.sh $(ROOT) \
		$(ROOT)/build/companion_mast_status_mut > $@.tmp && mv $@.tmp $@ && cat $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_companion_mast_status.mutant-check

# MAST-WEDGE e2e mutants: each test-only MUT_MAST_* daemon build must fail its audio-recovery
# row (tests/unit/test_mast_wedge_mutants.sh; host libav; one daemon build per mutant).
mast-wedge-mutants:
	bash $(ROOT)/tests/unit/test_mast_wedge_mutants.sh

$(ROOT)/build/test_mraudio_status: $(ROOT)/tests/unit/test_mraudio_status.cpp \
		$(ROOT)/host/libmisterplex/mraudio_status.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_mraudio_status.cpp

$(ROOT)/build/test_audio_buf_prefill: $(ROOT)/tests/unit/test_audio_buf_prefill.cpp \
		$(ROOT)/host/libmisterplex/mraudio_status.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_audio_buf_prefill.cpp

$(ROOT)/build/test_osd_menu: $(ROOT)/tests/unit/test_osd_menu.cpp \
		$(ROOT)/host/libmisterplex/osd_menu.hpp \
		$(ROOT)/host/libmisterplex/idle_screen.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_osd_menu.cpp

$(ROOT)/build/test_plexob_yc: $(ROOT)/tests/unit/test_plexob_yc.cpp \
		$(ROOT)/host/libmisterplex/plexob_yc.hpp \
		$(ROOT)/host/libmisterplex/plexob_ini.hpp \
		$(ROOT)/host/libmisterplex/core_id_select.hpp \
		$(ROOT)/host/libmisterplex/core_gate.hpp \
		$(ROOT)/host/libmisterplex/owenb_core_identity.hpp \
		$(ROOT)/arm/misterplexd/media_player.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -DMPX_SOURCE_ROOT='"$(ROOT)"' -o $@ $(ROOT)/tests/unit/test_plexob_yc.cpp

$(ROOT)/build/test_playback_overlay: $(ROOT)/tests/unit/test_playback_overlay.cpp \
		$(ROOT)/host/libmisterplex/playback_overlay.hpp \
		$(ROOT)/host/libmisterplex/fpga_playback_overlay.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_playback_overlay.cpp

# Overlay publisher lab flags (rel overlay-onchange 1004 + D52 RCA2 suspect 3):
# MPX_OVERLAY_ONCHANGE / MPX_OVERLAY_DECOUPLE / MPX_OVERLAY_SKIP_NO_PLANE, the same test
# on every daemon line (replay, receiver model, no-plane count, quiesce-then-hide).
$(ROOT)/build/test_overlay_onchange: $(ROOT)/tests/unit/test_overlay_onchange.cpp \
		$(ROOT)/host/libmisterplex/overlay_publish.hpp $(ROOT)/host/libmisterplex/fpga_playback_overlay.hpp \
		$(ROOT)/host/libmisterplex/playback_overlay.hpp $(ROOT)/host/libmisterplex/spi_window.hpp \
		$(ROOT)/host/libmisterplex/fpga_terminal.hpp \
		$(ROOT)/arm/misterplexd/fpga_spi.cpp $(ROOT)/arm/misterplexd/fpga_spi.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -o $@ \
		$(ROOT)/tests/unit/test_overlay_onchange.cpp $(ROOT)/arm/misterplexd/fpga_spi.cpp

$(ROOT)/build/test_input_mailbox: $(ROOT)/tests/unit/test_input_mailbox.cpp \
		$(ROOT)/host/libmisterplex/input_mailbox.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_input_mailbox.cpp

# PLX mailbox page per build row and the live-glass probe on it (review 00:55).
$(ROOT)/build/test_plx_page: $(ROOT)/tests/unit/test_plx_page.cpp \
		$(ROOT)/host/libmisterplex/plx_page.hpp $(ROOT)/host/libmisterplex/fpga_build_ids.hpp \
		$(ROOT)/host/libmisterplex/present_bank.hpp $(ROOT)/host/libmisterplex/mailbox_abi_spec.hpp \
		$(ROOT)/host/libmisterplex/ddr_frame_layout.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_plx_page.cpp

# Host build of the validation tool: test_plx_page_scripts.sh runs it on a memory image.
$(ROOT)/build/input_mailbox_probe_host: $(ROOT)/tools/input_mailbox_probe.cpp \
		$(ROOT)/host/libmisterplex/input_mailbox.hpp $(ROOT)/host/libmisterplex/fpga_build_ids.hpp \
		$(ROOT)/host/libmisterplex/plx_page.hpp $(ROOT)/host/libmisterplex/mailbox_abi_spec.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tools/input_mailbox_probe.cpp

$(ROOT)/build/test_pixel_format: $(ROOT)/tests/unit/test_pixel_format.cpp \
		$(ROOT)/host/libmisterplex/pixel_format.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_pixel_format.cpp

TEST_RESOLVE_DEPS := $(ROOT)/tests/unit/test_resolve.cpp \
		$(ROOT)/tests/unit/test_resolve_direct_remote.cpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/arm/misterplexd/plex_resolve.hpp \
		$(ROOT)/arm/misterplexd/plex_keys.hpp \
		$(ROOT)/arm/misterplexd/queue_step.hpp \
		$(ROOT)/arm/misterplexd/companion.hpp \
		$(ROOT)/host/libmisterplex/osd_menu.hpp \
		$(ROOT)/host/libmisterplex/video_backend.hpp \
		$(ROOT)/host/libmisterplex/present_bank.hpp \
		$(ROOT)/assets/plex-profiles/fpga_profile.hpp \
		$(ROOT)/host/libmisterplex/ddr_bitstream_ring.hpp \
		$(ROOT)/host/libmisterplex/mailbox_abi_spec.hpp
$(ROOT)/build/test_resolve: $(TEST_RESOLVE_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -o $@ \
		$(ROOT)/tests/unit/test_resolve.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp
# TASKS-NIGHT-1005 #22: built with the pre-#22 ratingKey fallback in queueStepRequest, test_resolve must FAIL.
$(ROOT)/build/test_resolve_mut_skipnext_rk: $(TEST_RESOLVE_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -DMUT_SKIPNEXT_RK_FALLBACK -I$(ROOT)/arm/misterplexd -o $@ \
		$(ROOT)/tests/unit/test_resolve.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp

# TASKS #25 T-PQ-SUBSTRING-MATCH mutants, in the form T-UNIT-DANGLING-3's checker reads (test_unit_targets_run.py):
# each binary is named test_resolve_MUT_PQ_<NAME> (built with -DMUT_PQ_<NAME>), the base test_resolve runs first (r1),
# and the loop runs the binaries by their literal names. KILLED = exit 1 with the pinned first FAIL line; a crash or any
# other first FAIL is not KILLED. The needles and their self-test (which reads c2aed83b from the git history) follow.
PQ25_MUTANTS := ID_SUB ID_RK KEY_SUB BIND_SUB IDX0 STEP_NEG
$(ROOT)/build/test_resolve_MUT_PQ_%: $(TEST_RESOLVE_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -DMUT_PQ_$* -I$(ROOT)/arm/misterplexd -o $@ \
		$(ROOT)/tests/unit/test_resolve.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp
$(ROOT)/build/test_resolve.pq25-mutant-check: $(ROOT)/build/test_resolve \
		$(addprefix $(ROOT)/build/test_resolve_MUT_PQ_,$(PQ25_MUTANTS)) \
		$(ROOT)/tests/unit/pq25_needles.sh $(wildcard $(ROOT)/arm/misterplexd/*.cpp) \
		$(wildcard $(ROOT)/arm/misterplexd/*.hpp)
	@rm -f $@ $@.tmp
	@$(ROOT)/build/test_resolve > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in $(PQ25_MUTANTS); do \
		case $$m in \
		ID_SUB) pin='test_resolve.cpp:649: selectQueueIndex(f1, "", "15", &by) == 1' ;; \
		ID_RK) pin='test_resolve.cpp:655: selectQueueIndex(f2, "", "5001", &by) == 1' ;; \
		KEY_SUB) pin='test_resolve.cpp:668: selectQueueIndex(f3rk, "/library/metadata/123", "", &by) == 1' ;; \
		BIND_SUB) pin='test_resolve.cpp:696: !firstResolveBinds(f3[0], "/library/metadata/123", "123")' ;; \
		IDX0) pin='test_resolve.cpp:678: selectQueueIndex({}, "/library/metadata/123", "", &by) == -1' ;; \
		STEP_NEG) pin='test_resolve.cpp:702: !queueStepDest(-1, 1, 2)' ;; \
		*) echo "MUTANT INVALID: pq25 $$m has no pinned FAIL"; exit 1 ;; \
		esac; \
		rc=0; $(ROOT)/build/test_resolve_MUT_PQ_$$m > $@.$$m.log 2>&1 || rc=$$?; \
		first=$$(grep -m1 '^FAIL ' $@.$$m.log || true); \
		case "$$first" in *"$$pin"*) pinok=1 ;; *) pinok=0 ;; esac; \
		if [ $$rc -eq 0 ]; then echo "MUTANT SURVIVED: pq25 $$m"; exit 1; fi; \
		if [ $$rc -eq 1 ] && [ $$pinok -eq 1 ]; then \
			echo "test_resolve: mutant pq25 $$m KILLED at $$pin" | tee -a $@.tmp; \
		elif [ $$rc -eq 1 ]; then \
			echo "MUTANT KILLED-OTHER: pq25 $$m first='$$first' want='$$pin'"; exit 1; \
		else \
			echo "MUTANT INVALID: pq25 $$m rc=$$rc first='$$first' want='$$pin'"; exit 1; \
		fi; \
	done
	@bash $(ROOT)/tests/unit/pq25_needles.sh $(ROOT)
	@bash $(ROOT)/tests/unit/pq25_needles.sh --self-test $(ROOT)
	@mv $@.tmp $@
# KILLED = exit 1 with exactly one FAIL line, the pinned one. A crash or any other FAIL is not KILLED.
# The source needles cover the call sites, which the helper tooth cannot see.
TEST_RESOLVE_MUT_PIN := test_resolve.cpp:632: b.playQueueItemId.empty()
$(ROOT)/build/test_resolve.mutant-check: $(ROOT)/build/test_resolve $(ROOT)/build/test_resolve_mut_skipnext_rk \
		$(ROOT)/tests/unit/skipnext_rk_needles.sh $(wildcard $(ROOT)/arm/misterplexd/*.cpp)
	@$(ROOT)/build/test_resolve > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@rc=0; $(ROOT)/build/test_resolve_mut_skipnext_rk > $@.log 2>&1 || rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "MUTANT SURVIVED: test_resolve_mut_skipnext_rk"; exit 1; fi; \
	n=$$(grep -c '^FAIL ' $@.log || true); first=$$(grep -m1 '^FAIL ' $@.log || true); \
	case "$$first" in *'$(TEST_RESOLVE_MUT_PIN)') pin=1 ;; *) pin=0 ;; esac; \
	if [ $$rc -ne 1 ] || [ "$$n" != 1 ] || [ $$pin -ne 1 ]; then \
		echo "MUTANT NOT KILLED: test_resolve_mut_skipnext_rk rc=$$rc FAIL lines=$$n first='$$first'"; \
		echo "  want rc 1 and the one FAIL '$(TEST_RESOLVE_MUT_PIN)'"; exit 1; fi
	@bash $(ROOT)/tests/unit/skipnext_rk_needles.sh $(ROOT)
	@echo "test_resolve: mutant skipnext-rk-fallback KILLED at $(TEST_RESOLVE_MUT_PIN)" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_resolve.mutant-check
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_resolve.pq25-mutant-check

# DAEMON-FPS-SPEC-1002 T2 harness: ResolveResult JSON against tests/fixtures/fake_pms.
$(ROOT)/build/test_resolve_fps_replay: $(ROOT)/tests/unit/test_resolve_fps_replay.cpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/arm/misterplexd/plex_resolve.hpp \
		$(ROOT)/host/libmisterplex/fpga_build_ids.hpp \
		$(ROOT)/host/libmisterplex/video_backend.hpp \
		$(ROOT)/assets/plex-profiles/fpga_profile.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -o $@ \
		$(ROOT)/tests/unit/test_resolve_fps_replay.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp

# Offline fake-PMS replay, f6526d96 (clean worktree) vs this tree, full env matrix.
FPS_REPLAY_BASE ?= f6526d96
# Flags-off SPI-sequence replay of the native-overlay publisher against a base ref
# (default prod dbcc42fa), plus invalid lab flag values, which must fail closed to the
# base publisher: tests/replay/overlay_spi/run_overlay_spi_replay.sh.
OVERLAY_SPI_REPLAY_BASE ?= dbcc42fa
overlay-spi-replay:
	bash $(ROOT)/tests/replay/overlay_spi/run_overlay_spi_replay.sh $(OVERLAY_SPI_REPLAY_BASE) HEAD

resolve-fps-replay: $(ROOT)/build/test_resolve_fps_replay
	python3 $(ROOT)/tests/fixtures/fake_pms/run_fps_replay.py --base-ref $(FPS_REPLAY_BASE)

$(ROOT)/build/pms_baseline_probe: $(ROOT)/tools/pms_baseline_probe.cpp \
		$(ROOT)/assets/plex-profiles/fpga_profile.hpp \
		$(ROOT)/host/libmisterplex/ddr_bitstream_ring.hpp \
		$(ROOT)/host/libmisterplex/mailbox_abi_spec.hpp \
		$(ROOT)/host/libmisterplex/h264_cavlc.hpp \
		$(ROOT)/host/libmisterplex/h264_residual_gold.hpp \
		$(ROOT)/host/libmisterplex/h264_nal.hpp \
		$(ROOT)/host/libmisterplex/h264_sps.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm -o $@ $(ROOT)/tools/pms_baseline_probe.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp

$(ROOT)/build/pms_nal_stats: $(ROOT)/tools/pms_nal_stats.cpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/arm/misterplexd/plex_resolve.hpp \
		$(ROOT)/host/libmisterplex/video_backend.hpp \
		$(ROOT)/assets/plex-profiles/fpga_profile.hpp \
		$(ROOT)/host/libmisterplex/ddr_bitstream_ring.hpp \
		$(ROOT)/host/libmisterplex/mailbox_abi_spec.hpp \
		$(ROOT)/host/libmisterplex/h264_bitstream_transport.hpp \
		$(ROOT)/host/libmisterplex/h264_nal_dispatch.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm -I$(ROOT)/arm/misterplexd -o $@ \
		$(ROOT)/tools/pms_nal_stats.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp

$(ROOT)/build/test_pms_timeline: $(ROOT)/tests/unit/test_pms_timeline.cpp \
		$(ROOT)/arm/misterplexd/pms_timeline.cpp \
		$(ROOT)/arm/misterplexd/pms_timeline.hpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/arm/misterplexd/plex_resolve.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -o $@ \
		$(ROOT)/tests/unit/test_pms_timeline.cpp \
		$(ROOT)/arm/misterplexd/pms_timeline.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp


# WAN read-stall bound (a lab MiSTer run-A AVERROR_EXIT at 3.6 s).
$(ROOT)/build/test_read_stall: $(ROOT)/tests/unit/test_read_stall.cpp $(ROOT)/host/libmisterplex/av_inproc_decode.hpp \
		$(ROOT)/arm/misterplexd/media_player.cpp $(ROOT)/arm/misterplexd/av_inproc_decode.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -DMPX_ROOT='"$(ROOT)"' -o $@ $<
$(ROOT)/build/test_read_stall_MUT: $(ROOT)/tests/unit/test_read_stall.cpp $(ROOT)/host/libmisterplex/av_inproc_decode.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -DMPX_ROOT='"$(ROOT)"' -DMUT_WAN_READ_STALL_5S -o $@ $<
$(ROOT)/build/test_read_stall.mutant-check: $(ROOT)/build/test_read_stall $(ROOT)/build/test_read_stall_MUT
	@$(ROOT)/build/test_read_stall > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@if $(ROOT)/build/test_read_stall_MUT > $@.mut.log 2>&1; then echo "MUTANT SURVIVED: MUT_WAN_READ_STALL_5S"; exit 1; fi
	@grep -m1 'FAIL ' $@.mut.log | grep -q 'R2 WAN' || { echo "MUTANT KILLED BY THE WRONG CHECK"; cat $@.mut.log; exit 1; }
	@echo "test_read_stall: mutant MUT_WAN_READ_STALL_5S FAILs at R2 as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_read_stall $(ROOT)/build/test_read_stall.mutant-check

# Prod line: seekTo keeps the playMedia server token (a lab MiSTer scrub 1556).
SEEK_TOKEN_SRC := $(ROOT)/tests/unit/test_seek_token.cpp $(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp
$(ROOT)/build/test_seek_token: $(SEEK_TOKEN_SRC) $(ROOT)/arm/misterplexd/companion.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -o $@ $(SEEK_TOKEN_SRC)
$(ROOT)/build/test_seek_token_MUT: $(SEEK_TOKEN_SRC) $(ROOT)/arm/misterplexd/companion.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -DMUT_SEEK_TAKES_CONTROLLER_TOKEN -o $@ $(SEEK_TOKEN_SRC)
$(ROOT)/build/test_seek_token.mutant-check: $(ROOT)/build/test_seek_token $(ROOT)/build/test_seek_token_MUT
	@$(ROOT)/build/test_seek_token > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@if $(ROOT)/build/test_seek_token_MUT > $@.mut.log 2>&1; then echo "MUTANT SURVIVED: MUT_SEEK_TAKES_CONTROLLER_TOKEN"; exit 1; fi
	@grep -m1 'FAIL ' $@.mut.log | grep -q 'S1 seekTo' || { echo "MUTANT KILLED BY THE WRONG CHECK"; cat $@.mut.log; exit 1; }
	@echo "test_seek_token: mutant MUT_SEEK_TAKES_CONTROLLER_TOKEN FAILs at S1 as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_seek_token $(ROOT)/build/test_seek_token.mutant-check

$(ROOT)/build/test_companion_plant_seek: $(ROOT)/tests/unit/test_companion_plant_seek.cpp \
		$(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/arm/misterplexd/companion.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -o $@ \
		$(ROOT)/tests/unit/test_companion_plant_seek.cpp $(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp

$(ROOT)/build/test_companion_480i_field_rate: $(ROOT)/tests/unit/test_companion_480i_field_rate.cpp \
		$(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/arm/misterplexd/companion.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -o $@ \
		$(ROOT)/tests/unit/test_companion_480i_field_rate.cpp $(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp

$(ROOT)/build/test_pts_jump_clamp: $(ROOT)/tests/unit/test_pts_jump_clamp.cpp \
		$(ROOT)/host/libmisterplex/pts_jump_clamp.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_pts_jump_clamp.cpp

$(ROOT)/build/test_ttfp_stamps: $(ROOT)/tests/unit/test_ttfp_stamps.cpp \
		$(ROOT)/arm/misterplexd/media_player.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -o $@ \
		$(ROOT)/tests/unit/test_ttfp_stamps.cpp

$(ROOT)/build/test_pts_wrap: $(ROOT)/tests/unit/test_pts_wrap.cpp \
		$(ROOT)/host/libmisterplex/pts_wrap.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/host -o $@ $(ROOT)/tests/unit/test_pts_wrap.cpp

$(ROOT)/build/test_audio_lead_clamp: $(ROOT)/tests/unit/test_audio_lead_clamp.cpp \
		$(ROOT)/host/libmisterplex/audio_lead_clamp.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/host -o $@ $(ROOT)/tests/unit/test_audio_lead_clamp.cpp

$(ROOT)/build/test_audio_queue_bound_240p: $(ROOT)/tests/unit/test_audio_queue_bound_240p.cpp \
		$(ROOT)/host/libmisterplex/mraudio_status.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/host -o $@ $(ROOT)/tests/unit/test_audio_queue_bound_240p.cpp

$(ROOT)/build/test_sps_reject: $(ROOT)/tests/unit/test_sps_reject.cpp \
		$(ROOT)/host/libmisterplex/sps_reject.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/host -o $@ $(ROOT)/tests/unit/test_sps_reject.cpp

$(ROOT)/build/test_bitrate_floor_480i: $(ROOT)/tests/unit/test_bitrate_floor_480i.cpp \
		$(ROOT)/host/libmisterplex/bitrate_floor_480i.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/host -o $@ $(ROOT)/tests/unit/test_bitrate_floor_480i.cpp

$(ROOT)/build/test_companion_title_policy: $(ROOT)/tests/unit/test_companion_title_policy.cpp \
		$(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/arm/misterplexd/companion.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -o $@ \
		$(ROOT)/tests/unit/test_companion_title_policy.cpp $(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp

$(ROOT)/build/test_480i_active_height: $(ROOT)/tests/unit/test_480i_active_height.cpp \
		$(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/arm/misterplexd/companion.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -o $@ \
		$(ROOT)/tests/unit/test_480i_active_height.cpp $(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp

$(ROOT)/build/test_core_id_select: $(ROOT)/tests/unit/test_core_id_select.cpp \
		$(ROOT)/host/libmisterplex/core_id_select.hpp \
		$(ROOT)/host/libmisterplex/fpga_build_ids.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/host -o $@ $(ROOT)/tests/unit/test_core_id_select.cpp

$(ROOT)/build/test_sps_reject_high: $(ROOT)/tests/unit/test_sps_reject_high.cpp \
		$(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/arm/misterplexd/companion.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -o $@ \
		$(ROOT)/tests/unit/test_sps_reject_high.cpp $(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp

$(ROOT)/build/test_companion_letterbox_bars: $(ROOT)/tests/unit/test_companion_letterbox_bars.cpp \
		$(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/arm/misterplexd/companion.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -o $@ \
		$(ROOT)/tests/unit/test_companion_letterbox_bars.cpp $(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp

$(ROOT)/build/test_companion_live_clock: $(ROOT)/tests/unit/test_companion_live_clock.cpp \
		$(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/arm/misterplexd/companion.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -o $@ \
		$(ROOT)/tests/unit/test_companion_live_clock.cpp $(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp

$(ROOT)/build/test_title_direct_policy: $(ROOT)/tests/unit/test_title_direct_policy.cpp \
		$(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/arm/misterplexd/companion.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -o $@ \
		$(ROOT)/tests/unit/test_title_direct_policy.cpp $(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp
$(ROOT)/build/test_title_direct_policy.ok: $(ROOT)/build/test_title_direct_policy
	@$(ROOT)/build/test_title_direct_policy > $@.log 2>&1 || { cat $@.log; exit 1; }
	@cat $@.log
	@cp $@.log $@

unit-unlocked unit-host-unlocked: $(ROOT)/build/test_companion_letterbox_bars
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_companion_live_clock
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_title_direct_policy $(ROOT)/build/test_title_direct_policy.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_companion_480i_field_rate
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_pts_jump_clamp
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_ttfp_stamps
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_pts_wrap
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_audio_lead_clamp
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_audio_queue_bound_240p
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_sps_reject
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_bitrate_floor_480i
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_companion_title_policy
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_480i_active_height
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_core_id_select
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_audio_buf_prefill

unit-unlocked unit-host-unlocked: $(ROOT)/build/test_companion_natural_eof
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_companion_reqlog $(ROOT)/build/test_companion_reqlog.mutant-check
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_input_close_eof
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_fpga_av_trace $(ROOT)/build/test_fpga_av_trace_off
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_library_browser $(ROOT)/build/test_browse_v2 \
		$(ROOT)/build/test_browse_library_list

# Companion request latency log (review 03:24, release 03:25): rows + mutants that must FAIL.
COMPANION_REQLOG_DEPS := $(ROOT)/tests/unit/test_companion_reqlog.cpp \
	$(ROOT)/arm/misterplexd/companion_reqlog.hpp
$(ROOT)/build/test_companion_reqlog: $(COMPANION_REQLOG_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -o $@ $(ROOT)/tests/unit/test_companion_reqlog.cpp
$(ROOT)/build/test_companion_reqlog_mut_%: $(COMPANION_REQLOG_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -DMUT_COMPANION_$* -I$(ROOT)/arm/misterplexd -o $@ \
		$(ROOT)/tests/unit/test_companion_reqlog.cpp
$(ROOT)/build/test_companion_reqlog.mutant-check: $(ROOT)/build/test_companion_reqlog \
		$(ROOT)/build/test_companion_reqlog_mut_SLOW_GT $(ROOT)/build/test_companion_reqlog_mut_POLL_SWALLOWS_SLOW
	@$(ROOT)/build/test_companion_reqlog > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in SLOW_GT POLL_SWALLOWS_SLOW; do \
		if $(ROOT)/build/test_companion_reqlog_mut_$$m > $@.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: MUT_COMPANION_$$m"; exit 1; fi; done
	@echo "test_companion_reqlog: mutants SLOW_GT and POLL_SWALLOWS_SLOW FAIL as required" | tee $@

$(ROOT)/build/test_fpga_av_trace: $(ROOT)/tests/unit/test_fpga_av_trace.cpp \
		$(ROOT)/host/libmisterplex/fpga_av_trace.hpp $(ROOT)/host/libmisterplex/fpga_terminal.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -DMPX_FPGA_AV_TRACE=1 -pthread -o $@ $<

$(ROOT)/build/test_fpga_av_trace_off: $(ROOT)/tests/unit/test_fpga_av_trace.cpp \
		$(ROOT)/host/libmisterplex/fpga_av_trace.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -o $@ $<

$(ROOT)/build/test_companion_natural_eof: $(ROOT)/tests/unit/test_companion_natural_eof.cpp \
		$(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/arm/misterplexd/companion.hpp $(ROOT)/arm/misterplexd/natural_eof.hpp \
		$(ROOT)/arm/misterplexd/playback_controls.hpp $(ROOT)/arm/misterplexd/pms_timeline.cpp \
		$(ROOT)/arm/misterplexd/pms_timeline.hpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/arm/misterplexd/plex_resolve.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -o $@ \
		$(ROOT)/tests/unit/test_companion_natural_eof.cpp $(ROOT)/arm/misterplexd/companion.cpp \
		$(ROOT)/arm/misterplexd/pms_timeline.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		-Wl,--wrap=send -Wl,--wrap=pthread_mutex_unlock

$(ROOT)/build/test_h264_bitstream_source: $(ROOT)/tests/unit/test_h264_bitstream_source.cpp \
		$(ROOT)/host/libmisterplex/ddr_bitstream_ring.hpp \
		$(ROOT)/host/libmisterplex/mailbox_abi_spec.hpp \
		$(ROOT)/host/libmisterplex/h264_bitstream_transport.hpp \
		$(ROOT)/host/libmisterplex/h264_nal_dispatch.hpp \
		$(ROOT)/tests/fixtures/p3_multinal/wcap_residual14_idr_plus_p.264
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_h264_bitstream_source.cpp

# Native host daemon for local smoke
PLEXUI_SRC := \
	$(ROOT)/host/libmisterplex/plexui/plexui_controller.cpp \
	$(ROOT)/host/libmisterplex/plexui/plexui_data.cpp \
	$(ROOT)/host/libmisterplex/plexui/plexui_api_cache.cpp \
	$(ROOT)/host/libmisterplex/plexui/plexui_http.cpp \
	$(ROOT)/host/libmisterplex/plexui/plexui_font.cpp \
	$(ROOT)/host/libmisterplex/plexui/plexui_images.cpp \
	$(ROOT)/host/libmisterplex/plexui/plexui_disk_cache.cpp \
	$(ROOT)/host/libmisterplex/plexui/plexui_render.cpp

MPLEX_SRC := \
	$(ROOT)/arm/misterplexd/main.cpp \
	$(ROOT)/arm/misterplexd/wall_clock.cpp \
	$(ROOT)/arm/misterplexd/companion.cpp \
	$(ROOT)/arm/misterplexd/fb_present.cpp \
	$(ROOT)/arm/misterplexd/media_player.cpp \
	$(ROOT)/arm/misterplexd/pms_timeline.cpp \
	$(ROOT)/arm/misterplexd/plex_resolve.cpp \
	$(ROOT)/arm/misterplexd/fpga_spi.cpp \
	$(ROOT)/arm/misterplexd/gcast/airplay_fe.cpp \
	$(ROOT)/arm/misterplexd/gcast/https_client.cpp \
	$(ROOT)/arm/misterplexd/gcast/yt_fe.cpp \
	$(PLEXUI_SRC)
MPLEX_INC := -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui
# OWENB-P1B T5: the daemon links the vendored owenb ring lib and the OB play (host: no libav, so the OB route
# refuses play; arm-plexd-daemon with libav: the full OB route with owenb_av_source.cpp).
OWENB_DAEMON_FLAGS := -DMPX_HAVE_OWENB_RING -I$(ROOT)/third_party/owenb-h264ring/include
# Host recon headers (Phase 3.3i STREAM path)
MPLEX_HDR := \
	$(ROOT)/arm/misterplexd/natural_eof.hpp \
	$(ROOT)/arm/misterplexd/playback_controls.hpp \
	$(ROOT)/arm/misterplexd/plex_keys.hpp \
	$(ROOT)/arm/misterplexd/queue_step.hpp \
	$(ROOT)/host/libmisterplex/video_backend.hpp \
	$(ROOT)/host/libmisterplex/av_clock.hpp \
	$(ROOT)/host/libmisterplex/core_gate.hpp $(ROOT)/host/libmisterplex/owenb_core_identity.hpp \
	$(ROOT)/host/libmisterplex/av_inproc_decode.hpp \
	$(ROOT)/host/libmisterplex/fpga_av_trace.hpp \
	$(ROOT)/assets/plex-profiles/fpga_profile.hpp \
	$(ROOT)/host/libmisterplex/mailbox_abi_spec.hpp \
	$(ROOT)/host/libmisterplex/audio_session.hpp \
	$(ROOT)/host/libmisterplex/ddr_bitstream_ring.hpp \
	$(ROOT)/host/libmisterplex/h264_bitstream_transport.hpp \
	$(ROOT)/host/libmisterplex/h264_nal_dispatch.hpp \
	$(ROOT)/host/libmisterplex/h264_recon.hpp \
	$(ROOT)/host/libmisterplex/h264_slice_walk.hpp \
	$(ROOT)/host/libmisterplex/h264_cavlc.hpp \
	$(ROOT)/host/libmisterplex/h264_nal.hpp \
	$(ROOT)/host/libmisterplex/h264_sps.hpp \
	$(ROOT)/host/libmisterplex/ddr_frame_layout.hpp \
	$(ROOT)/host/libmisterplex/p720_audio_keepup.hpp \
	$(ROOT)/host/libmisterplex/p720_transcode_vf.hpp \
	$(ROOT)/host/libmisterplex/present_bank.hpp \
	$(ROOT)/host/libmisterplex/fabric_direct.hpp \
	$(ROOT)/host/libmisterplex/idle_screen.hpp \
	$(ROOT)/host/libmisterplex/browse_library_list.hpp \
	$(ROOT)/host/libmisterplex/input_mailbox.hpp \
	$(ROOT)/host/libmisterplex/playback_overlay.hpp \
	$(ROOT)/host/libmisterplex/fpga_playback_overlay.hpp $(ROOT)/host/libmisterplex/overlay_publish.hpp $(ROOT)/host/libmisterplex/spi_window.hpp \
	$(ROOT)/host/libmisterplex/pixel_format.hpp $(ROOT)/arm/misterplexd/lock_paths.hpp

plexui_shot: $(ROOT)/build/plexui_shot

.PHONY: plexui-tests
plexui-tests:
	@mkdir -p $(ROOT)/build/plexui-tests
	$(CXX) -std=c++17 -O1 -pthread $(MPLEX_INC) -DMPX_PLEXUI_UNIT_TEST -o $(ROOT)/build/plexui-tests/test_plexui \
		$(ROOT)/tests/unit/test_plexui.cpp $(PLEXUI_SRC)
	cd $(ROOT) && ./build/plexui-tests/test_plexui

# OB port: render geometry (R1 640x480 byte-identical goldens + OB canvases). ASan: PLEXUI_GEOM_SAN=-fsanitize=address
.PHONY: plexui-geom-test
plexui-geom-test:
	@mkdir -p $(ROOT)/build/plexui-tests
	$(CXX) -std=c++17 -O1 -g $(PLEXUI_GEOM_SAN) -pthread $(MPLEX_INC) -o $(ROOT)/build/plexui-tests/test_plexui_geom \
		$(ROOT)/tests/unit/test_plexui_geom.cpp $(PLEXUI_SRC)
	cd $(ROOT) && ./build/plexui-tests/test_plexui_geom

$(ROOT)/build/plexui_shot: $(ROOT)/tools/plexui_shot.cpp $(PLEXUI_SRC) \
		$(ROOT)/host/libmisterplex/browse_fetch.hpp \
		$(ROOT)/host/libmisterplex/browse_v2.hpp \
		$(ROOT)/host/libmisterplex/browse_controller.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread $(MPLEX_INC) -o $@ \
		$(ROOT)/tools/plexui_shot.cpp $(PLEXUI_SRC)

# PlexUI black-first-paint: paintReady gates idle present until visible art is decoded.
PLEXUI_PAINT_READY_FLAGS := $(MPLEX_INC) -pthread -DMPX_PLEXUI_UNIT_TEST
$(ROOT)/build/test_plexui_paint_ready: $(ROOT)/tests/unit/test_plexui_paint_ready.cpp $(PLEXUI_SRC) \
		$(ROOT)/host/libmisterplex/plexui/plexui_idle_paint.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(PLEXUI_PAINT_READY_FLAGS) -o $@ \
		$(ROOT)/tests/unit/test_plexui_paint_ready.cpp $(PLEXUI_SRC)
$(ROOT)/build/test_plexui_paint_ready_mut_skip_art: $(ROOT)/tests/unit/test_plexui_paint_ready.cpp $(PLEXUI_SRC) \
		$(ROOT)/host/libmisterplex/plexui/plexui_idle_paint.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(PLEXUI_PAINT_READY_FLAGS) -DMUT_PLEXUI_SKIP_ART_READY -o $@ \
		$(ROOT)/tests/unit/test_plexui_paint_ready.cpp $(PLEXUI_SRC)
$(ROOT)/build/test_plexui_paint_ready.mutant-check: $(ROOT)/build/test_plexui_paint_ready \
		$(ROOT)/build/test_plexui_paint_ready_mut_skip_art
	@$(ROOT)/build/test_plexui_paint_ready > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@if $(ROOT)/build/test_plexui_paint_ready_mut_skip_art > $@.log 2>&1; then \
		echo "MUTANT SURVIVED: test_plexui_paint_ready_mut_skip_art"; exit 1; fi
	@echo "test_plexui_paint_ready: mutant MUT_PLEXUI_SKIP_ART_READY FAILS as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plexui_paint_ready.mutant-check

# Nested mu_ in updateImageCacheBudget deadlocks the first Home/grid paint. The mutant
# puts the lock back; alarm(3) in the test turns the hang into a failure.
$(ROOT)/build/test_plexui_cache_lock: $(ROOT)/tests/unit/test_plexui_cache_lock.cpp $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(PLEXUI_PAINT_READY_FLAGS) -o $@ \
		$(ROOT)/tests/unit/test_plexui_cache_lock.cpp $(PLEXUI_SRC)
$(ROOT)/build/test_plexui_cache_lock_mut: $(ROOT)/tests/unit/test_plexui_cache_lock.cpp $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(PLEXUI_PAINT_READY_FLAGS) -DMUT_PLEXUI_CACHE_RELOCK -o $@ \
		$(ROOT)/tests/unit/test_plexui_cache_lock.cpp $(PLEXUI_SRC)
$(ROOT)/build/test_plexui_cache_lock.mutant-check: $(ROOT)/build/test_plexui_cache_lock \
		$(ROOT)/build/test_plexui_cache_lock_mut
	@$(ROOT)/build/test_plexui_cache_lock > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@if $(ROOT)/build/test_plexui_cache_lock_mut > $@.log 2>&1; then \
		echo "MUTANT SURVIVED: MUT_PLEXUI_CACHE_RELOCK"; exit 1; fi
	@echo "test_plexui_cache_lock: mutant MUT_PLEXUI_CACHE_RELOCK FAILS as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plexui_cache_lock.mutant-check

# onR1FlipAck must not call hooks_.repaint while the caller holds the paint mutex.
$(ROOT)/build/test_plexui_flipack: $(ROOT)/tests/unit/test_plexui_flipack.cpp $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(PLEXUI_PAINT_READY_FLAGS) -o $@ \
		$(ROOT)/tests/unit/test_plexui_flipack.cpp $(PLEXUI_SRC)
$(ROOT)/build/test_plexui_flipack_mut: $(ROOT)/tests/unit/test_plexui_flipack.cpp $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(PLEXUI_PAINT_READY_FLAGS) -DMUT_PLEXUI_FLIPACK_REPAINT -o $@ \
		$(ROOT)/tests/unit/test_plexui_flipack.cpp $(PLEXUI_SRC)
$(ROOT)/build/test_plexui_flipack.mutant-check: $(ROOT)/build/test_plexui_flipack \
		$(ROOT)/build/test_plexui_flipack_mut
	@$(ROOT)/build/test_plexui_flipack > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@if $(ROOT)/build/test_plexui_flipack_mut > $@.log 2>&1; then \
		echo "MUTANT SURVIVED: MUT_PLEXUI_FLIPACK_REPAINT"; exit 1; fi
	@echo "test_plexui_flipack: mutant MUT_PLEXUI_FLIPACK_REPAINT FAILS as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plexui_flipack.mutant-check

# Gate 8: 1970 is not a plausible wall clock. The mutant treats every time as sane.
$(ROOT)/build/test_wall_clock: $(ROOT)/tests/unit/test_wall_clock.cpp $(ROOT)/arm/misterplexd/wall_clock.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -DMPX_SOURCE_ROOT='"$(ROOT)"' -o $@ $(ROOT)/tests/unit/test_wall_clock.cpp
$(ROOT)/build/test_wall_clock_mut: $(ROOT)/tests/unit/test_wall_clock.cpp $(ROOT)/arm/misterplexd/wall_clock.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -DMPX_SOURCE_ROOT='"$(ROOT)"' -DMUT_WALL_TRUST_1970 -o $@ \
		$(ROOT)/tests/unit/test_wall_clock.cpp
$(ROOT)/build/test_wall_clock_mut_sntp: $(ROOT)/tests/unit/test_wall_clock.cpp $(ROOT)/arm/misterplexd/wall_clock.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -DMPX_SOURCE_ROOT='"$(ROOT)"' -DMUT_SNTP_SKIP_CHECKS -o $@ \
		$(ROOT)/tests/unit/test_wall_clock.cpp
$(ROOT)/build/test_wall_clock_mut_rescue: $(ROOT)/tests/unit/test_wall_clock.cpp $(ROOT)/arm/misterplexd/wall_clock.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -DMPX_SOURCE_ROOT='"$(ROOT)"' -DMUT_WALL_RESCUE_IGNORE_SANE -o $@ \
		$(ROOT)/tests/unit/test_wall_clock.cpp
$(ROOT)/build/test_wall_clock.mutant-check: $(ROOT)/build/test_wall_clock $(ROOT)/build/test_wall_clock_mut \
		$(ROOT)/build/test_wall_clock_mut_sntp $(ROOT)/build/test_wall_clock_mut_rescue
	@$(ROOT)/build/test_wall_clock > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@if $(ROOT)/build/test_wall_clock_mut > $@.log 2>&1; then \
		echo "MUTANT SURVIVED: MUT_WALL_TRUST_1970"; exit 1; fi
	@if $(ROOT)/build/test_wall_clock_mut_sntp > $@.sntp.log 2>&1; then \
		echo "MUTANT SURVIVED: MUT_SNTP_SKIP_CHECKS"; exit 1; fi
	@if $(ROOT)/build/test_wall_clock_mut_rescue > $@.rescue.log 2>&1; then \
		echo "MUTANT SURVIVED: MUT_WALL_RESCUE_IGNORE_SANE"; exit 1; fi
	@echo "test_wall_clock: mutants MUT_WALL_TRUST_1970, MUT_SNTP_SKIP_CHECKS, and MUT_WALL_RESCUE_IGNORE_SANE FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_wall_clock.mutant-check

# a9857d1 linked a 32-bit time_t. This recipe builds the clock test that way
# (Ubuntu's armhf g++ defaults to 64) and runs it under qemu-arm-static.
$(ROOT)/build/test_wall_clock.qemu32.ok: $(ROOT)/tests/unit/test_wall_clock.cpp $(ROOT)/arm/misterplexd/wall_clock.hpp
	@mkdir -p $(ROOT)/build
	@command -v arm-linux-gnueabihf-g++ >/dev/null || { echo "qemu32 clock: arm-linux-gnueabihf-g++ missing"; exit 1; }
	@command -v qemu-arm-static >/dev/null || { echo "qemu32 clock: qemu-arm-static missing"; exit 1; }
	arm-linux-gnueabihf-g++ -std=c++17 -O2 -Wall -pthread -U_TIME_BITS -U_FILE_OFFSET_BITS \
		-I$(ROOT)/arm/misterplexd -DMPX_SOURCE_ROOT='"$(ROOT)"' -DMPX_REQUIRE_TIME_T_32 \
		-o $(ROOT)/build/test_wall_clock_qemu32 $(ROOT)/tests/unit/test_wall_clock.cpp
	qemu-arm-static -L /usr/arm-linux-gnueabihf $(ROOT)/build/test_wall_clock_qemu32
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_wall_clock.qemu32.ok

# Item 8: the ship toolchain (Arm GNU 13.2), not Ubuntu's armhf g++ (already 64).
$(ROOT)/build/test_time64_stat.qemu.ok: $(ROOT)/tests/unit/test_time64_stat.cpp
	@mkdir -p $(ROOT)/build
	@command -v qemu-arm-static >/dev/null || { echo "time64: qemu-arm-static missing"; exit 1; }
	@test -x /tmp/mpx-repro/arm-gnu-toolchain-13.2.Rel1-x86_64-arm-none-linux-gnueabihf/bin/arm-none-linux-gnueabihf-g++ || { echo "time64: Arm GNU g++ missing"; exit 1; }
	/tmp/mpx-repro/arm-gnu-toolchain-13.2.Rel1-x86_64-arm-none-linux-gnueabihf/bin/arm-none-linux-gnueabihf-g++ \
		-std=c++17 -O2 -Wall -static -D_TIME_BITS=64 -D_FILE_OFFSET_BITS=64 \
		-o $(ROOT)/build/test_time64_stat $(ROOT)/tests/unit/test_time64_stat.cpp
	truncate -s 3G $(ROOT)/build/test_time64_stat.sparse
	qemu-arm-static $(ROOT)/build/test_time64_stat $(ROOT)/build/test_time64_stat.sparse
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_time64_stat.qemu.ok

$(ROOT)/build/test_ob_output_reinit: $(ROOT)/tests/unit/test_ob_output_reinit.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_ob_output_reinit.cpp
$(ROOT)/build/test_ob_output_reinit.ok: $(ROOT)/build/test_ob_output_reinit
	cd $(ROOT) && ./build/test_ob_output_reinit
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_ob_output_reinit.ok

$(ROOT)/build/test_av_open_security: $(ROOT)/tests/unit/test_av_open_security.cpp \
		$(ROOT)/arm/misterplexd/av_open_opts.hpp $(ROOT)/arm/misterplexd/ca_bundle.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -o $@ $(ROOT)/tests/unit/test_av_open_security.cpp
$(ROOT)/build/test_av_open_security.ok: $(ROOT)/build/test_av_open_security
	cd $(ROOT) && ./build/test_av_open_security
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_av_open_security.ok

$(ROOT)/build/test_yt_403: $(ROOT)/tests/unit/test_yt_403.cpp $(ROOT)/arm/misterplexd/gcast/yt_403.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -o $@ $(ROOT)/tests/unit/test_yt_403.cpp
$(ROOT)/build/test_yt_403.ok: $(ROOT)/build/test_yt_403
	cd $(ROOT) && ./build/test_yt_403
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_yt_403.ok

$(ROOT)/build/test_plexui_send_home: $(ROOT)/tests/unit/test_plexui_send_home.cpp $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-o $@ $(ROOT)/tests/unit/test_plexui_send_home.cpp $(PLEXUI_SRC)
$(ROOT)/build/test_plexui_send_home.ok: $(ROOT)/build/test_plexui_send_home
	$(ROOT)/build/test_plexui_send_home
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plexui_send_home.ok

# Date fallback and the fake RTC. The preload holds time() at 1970 so OpenSSL
# actually evaluates notBefore. Certs are minted before the preload.
$(ROOT)/build/wall_clock_faketime.so: $(ROOT)/tests/unit/wall_clock_faketime.c
	@mkdir -p $(ROOT)/build
	$(CC) -shared -fPIC -O2 -Wall -Wextra -o $@ $< -ldl -pthread
$(ROOT)/build/test_wall_clock_https: $(ROOT)/tests/unit/test_wall_clock_https.cpp \
		$(ROOT)/arm/misterplexd/wall_clock.cpp $(ROOT)/arm/misterplexd/wall_clock.hpp \
		$(ROOT)/arm/misterplexd/ca_bundle.hpp \
		$(ROOT)/arm/misterplexd/gcast/https_client.cpp $(ROOT)/arm/misterplexd/gcast/https_client.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -DMPX_HAVE_GCAST_TLS -o $@ \
		$(ROOT)/tests/unit/test_wall_clock_https.cpp \
		$(ROOT)/arm/misterplexd/wall_clock.cpp \
		$(ROOT)/arm/misterplexd/gcast/https_client.cpp \
		-lssl -lcrypto -ldl
WALL_CLOCK_CERTS := $(ROOT)/build/wall-clock-certs
$(ROOT)/build/test_wall_clock_https.ok: $(ROOT)/build/test_wall_clock_https $(ROOT)/build/wall_clock_faketime.so
	@mkdir -p $(WALL_CLOCK_CERTS)
	openssl req -x509 -newkey rsa:2048 -keyout $(WALL_CLOCK_CERTS)/ca.key -out $(WALL_CLOCK_CERTS)/ca.pem -days 3650 -nodes -subj "/CN=Misterplex Clock Test CA" >/dev/null 2>&1
	openssl req -newkey rsa:2048 -keyout $(WALL_CLOCK_CERTS)/srv.key -out $(WALL_CLOCK_CERTS)/srv.csr -nodes -subj "/CN=clock.test" >/dev/null 2>&1
	printf '%s\n' 'subjectAltName=DNS:clock.test' > $(WALL_CLOCK_CERTS)/srv.ext
	openssl x509 -req -in $(WALL_CLOCK_CERTS)/srv.csr -CA $(WALL_CLOCK_CERTS)/ca.pem -CAkey $(WALL_CLOCK_CERTS)/ca.key -CAcreateserial -out $(WALL_CLOCK_CERTS)/srv.pem -days 3650 -extfile $(WALL_CLOCK_CERTS)/srv.ext >/dev/null 2>&1
	openssl req -x509 -newkey rsa:2048 -keyout $(WALL_CLOCK_CERTS)/bad.key -out $(WALL_CLOCK_CERTS)/badca.pem -days 3650 -nodes -subj "/CN=Untrusted CA" >/dev/null 2>&1
	LD_PRELOAD=$(ROOT)/build/wall_clock_faketime.so MPX_CLOCK_CERT_DIR=$(WALL_CLOCK_CERTS) $(ROOT)/build/test_wall_clock_https
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_wall_clock_https.ok

# Standalone yt-dlp: route, single-flight, timeout/kill, checksummed swap on 127.0.0.1.
$(ROOT)/build/test_yt_dlp: $(ROOT)/tests/unit/test_yt_dlp.cpp $(ROOT)/arm/misterplexd/gcast/yt_dlp.hpp \
		$(ROOT)/arm/misterplexd/gcast/dash_seek.hpp \
		$(ROOT)/arm/misterplexd/gcast/cast_resolve.hpp $(ROOT)/arm/misterplexd/mem_stat.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -o $@ $(ROOT)/tests/unit/test_yt_dlp.cpp
$(ROOT)/build/test_yt_dlp.ok: $(ROOT)/build/test_yt_dlp
	$(ROOT)/build/test_yt_dlp
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_yt_dlp.ok

# Header-only CA lookup: env, shipped bundle, distro store, stock MiSTer cacert.pem and cert.pem.
$(ROOT)/build/test_ca_bundle: $(ROOT)/tests/unit/test_ca_bundle.cpp $(ROOT)/arm/misterplexd/ca_bundle.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -DMPX_SOURCE_ROOT='"$(ROOT)"' -o $@ \
		$(ROOT)/tests/unit/test_ca_bundle.cpp
$(ROOT)/build/test_ca_bundle.ok: $(ROOT)/build/test_ca_bundle
	$(ROOT)/build/test_ca_bundle
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_ca_bundle.ok

# The log sink redacts X-Plex-Token and access_token on every line.
$(ROOT)/build/test_log_redact: $(ROOT)/tests/unit/test_log_redact.cpp $(ROOT)/arm/misterplexd/log_redact.hpp \
		$(ROOT)/arm/misterplexd/media_player.cpp $(ROOT)/arm/misterplexd/main.cpp \
		$(ROOT)/host/libmisterplex/owenb_session.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -DMPX_SOURCE_ROOT='"$(ROOT)"' -o $@ \
		$(ROOT)/tests/unit/test_log_redact.cpp
$(ROOT)/build/test_log_redact.ok: $(ROOT)/build/test_log_redact
	$(ROOT)/build/test_log_redact
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_log_redact.ok

# MediaPlayer::log and Companion::log share SyncedLogAssembler. TSan must stay clean.
$(ROOT)/build/test_log_race_tsan: $(ROOT)/tests/unit/test_log_race.cpp $(ROOT)/arm/misterplexd/log_redact.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -O1 -g -fsanitize=thread -I$(ROOT)/arm/misterplexd -o $@ \
		$(ROOT)/tests/unit/test_log_race.cpp
$(ROOT)/build/test_log_race.tsan-check: $(ROOT)/build/test_log_race_tsan
	@TSAN_OPTIONS='halt_on_error=1 exitcode=66' $(ROOT)/build/test_log_race_tsan > $@.log 2>&1 || { cat $@.log; exit 1; }
	@echo "test_log_race: TSan clean" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_log_race.tsan-check

# Empty/5xx helper bodies stay on the same play. DASH inputs seek by time.
$(ROOT)/build/test_gcast_open_retry: $(ROOT)/tests/unit/test_gcast_open_retry.cpp \
		$(ROOT)/arm/misterplexd/gcast/open_retry.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -o $@ $(ROOT)/tests/unit/test_gcast_open_retry.cpp
$(ROOT)/build/test_gcast_open_retry.ok: $(ROOT)/build/test_gcast_open_retry
	$(ROOT)/build/test_gcast_open_retry
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_gcast_open_retry.ok

$(ROOT)/build/test_dash_seek: $(ROOT)/tests/unit/test_dash_seek.cpp \
		$(ROOT)/arm/misterplexd/gcast/dash_seek.hpp $(ROOT)/arm/misterplexd/owenb_av_source.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -DMPX_HAVE_LIBAV \
		-DMPX_SOURCE_ROOT='"$(ROOT)"' $(shell pkg-config --cflags libavformat libavcodec libavutil) \
		-o $@ $(ROOT)/tests/unit/test_dash_seek.cpp \
		$(shell pkg-config --libs libavformat libavcodec libavutil)
$(ROOT)/build/test_dash_seek.ok: $(ROOT)/build/test_dash_seek
	$(ROOT)/build/test_dash_seek
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_dash_seek.ok

$(ROOT)/build/test_mem_stat: $(ROOT)/tests/unit/test_mem_stat.cpp $(ROOT)/arm/misterplexd/mem_stat.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -o $@ $(ROOT)/tests/unit/test_mem_stat.cpp
$(ROOT)/build/test_mem_stat.ok: $(ROOT)/build/test_mem_stat
	$(ROOT)/build/test_mem_stat
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_mem_stat.ok

$(ROOT)/build/test_ubsan_fnbind: $(ROOT)/tests/unit/test_ubsan_fnbind.cpp \
		$(ROOT)/host/libmisterplex/d52_log.hpp $(ROOT)/host/libmisterplex/f1_beat.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -fsanitize=undefined -fno-sanitize-recover=undefined -I$(ROOT)/host \
		-DMPX_SOURCE_ROOT='"$(ROOT)"' -o $@ $(ROOT)/tests/unit/test_ubsan_fnbind.cpp
$(ROOT)/build/test_ubsan_fnbind.ok: $(ROOT)/build/test_ubsan_fnbind
	$(ROOT)/build/test_ubsan_fnbind
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_ubsan_fnbind.ok

unit-unlocked unit-host-unlocked: $(ROOT)/build/test_boot_rc3.ok $(ROOT)/build/test_update_ytdlp.ok \
	$(ROOT)/build/test_package_plexob_rbf.ok $(ROOT)/build/test_package_readme_paths.ok
$(ROOT)/build/test_boot_rc3.ok: $(ROOT)/tests/unit/test_boot_rc3.sh $(ROOT)/scripts/misterplexd-boot-rc3.sh
	bash $(ROOT)/tests/unit/test_boot_rc3.sh
	@mkdir -p $(ROOT)/build
	touch $@
$(ROOT)/build/test_airplay_advertise: $(ROOT)/tests/unit/test_airplay_advertise.cpp \
		$(ROOT)/arm/misterplexd/gcast/airplay_fe.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -DMPX_SOURCE_ROOT='"$(ROOT)"' -o $@ \
		$(ROOT)/tests/unit/test_airplay_advertise.cpp
$(ROOT)/build/test_airplay_advertise.ok: $(ROOT)/build/test_airplay_advertise
	$(ROOT)/build/test_airplay_advertise
	@mkdir -p $(ROOT)/build
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_airplay_advertise.ok
$(ROOT)/build/test_package_plexob_rbf.ok: $(ROOT)/tests/unit/test_package_plexob_rbf.sh \
		$(ROOT)/scripts/stage_plexob_yc_rbf.sh $(ROOT)/scripts/package_release.sh
	bash $(ROOT)/tests/unit/test_package_plexob_rbf.sh
	@mkdir -p $(ROOT)/build
	touch $@
$(ROOT)/build/test_package_readme_paths.ok: $(ROOT)/tests/unit/test_package_readme_paths.sh \
		$(ROOT)/scripts/package_release.sh $(ROOT)/docs/release.md \
		$(ROOT)/packaging/licenses/ffmpeg/COPYING.LGPLv2.1 \
		$(ROOT)/packaging/licenses/openssl/LICENSE.txt
	bash $(ROOT)/tests/unit/test_package_readme_paths.sh
	@mkdir -p $(ROOT)/build
	touch $@
$(ROOT)/build/test_update_ytdlp.ok: $(ROOT)/tests/unit/test_update_ytdlp.sh $(ROOT)/scripts/update_ytdlp.sh
	bash $(ROOT)/tests/unit/test_update_ytdlp.sh
	@mkdir -p $(ROOT)/build
	touch $@

# Repeated MPEG-TS demux must not retain the elementary stream. Needs host FFmpeg.
$(ROOT)/build/test_av_read_rss: $(ROOT)/tests/unit/test_av_read_rss.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags $(L2_DEMUX_AV)) -o $@ $< \
		$(shell pkg-config --libs $(L2_DEMUX_AV))
$(ROOT)/build/test_av_read_rss_mut: $(ROOT)/tests/unit/test_av_read_rss.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -DMUT_AV_KEEP_PACKETS $(shell pkg-config --cflags $(L2_DEMUX_AV)) -o $@ $< \
		$(shell pkg-config --libs $(L2_DEMUX_AV))
$(ROOT)/build/test_av_read_rss.mutant-check: $(ROOT)/build/test_av_read_rss $(ROOT)/build/test_av_read_rss_mut
	@$(ROOT)/build/test_av_read_rss > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@if $(ROOT)/build/test_av_read_rss_mut > $@.log 2>&1; then \
		echo "MUTANT SURVIVED: MUT_AV_KEEP_PACKETS"; exit 1; fi
	@echo "test_av_read_rss: mutant MUT_AV_KEEP_PACKETS FAILS as required" | tee $@
unit-unlocked unit-host-unlocked: $(if $(L2_DEMUX_OK),$(ROOT)/build/test_av_read_rss.mutant-check)

$(ROOT)/build/misterplexd: $(MPLEX_SRC) \
		$(ROOT)/arm/misterplexd/companion.hpp \
		$(ROOT)/arm/misterplexd/media_player.hpp \
		$(ROOT)/arm/misterplexd/pms_timeline.hpp \
		$(ROOT)/arm/misterplexd/plex_resolve.hpp \
		$(ROOT)/arm/misterplexd/fb_present.hpp \
		$(ROOT)/arm/misterplexd/fpga_spi.hpp \
		$(MPLEX_HDR) $(OWENB_PLAY_SRCS) $(OWENB_PLAY_HDRS) $(OWENB_RING_OBJS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(MPLEX_INC) $(OWENB_DAEMON_FLAGS) -DMPX_TEST_BUILD -pthread -o $@ $(MPLEX_SRC) \
		$(OWENB_PLAY_SRCS) $(OWENB_RING_OBJS) -lm

plexd: $(ROOT)/build/misterplexd

# Standalone: push one RGB565 file to Plex frame_store via SPI ioctl
$(ROOT)/build/push_frame: $(ROOT)/arm/misterplexd/fpga_spi.cpp \
		$(ROOT)/tools/push_frame.cpp $(ROOT)/arm/misterplexd/fpga_spi.hpp \
		$(ROOT)/arm/misterplexd/core_gate_tool.hpp \
		$(ROOT)/host/libmisterplex/audio_session.hpp \
		$(ROOT)/host/libmisterplex/ddr_bitstream_ring.hpp \
		$(ROOT)/host/libmisterplex/mailbox_abi_spec.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -o $@ \
		$(ROOT)/tools/push_frame.cpp $(ROOT)/arm/misterplexd/fpga_spi.cpp

push-frame: $(ROOT)/build/push_frame

$(ROOT)/build/ddr_write_bench: $(ROOT)/tools/ddr_write_bench.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tools/ddr_write_bench.cpp

ddr-bench: $(ROOT)/build/ddr_write_bench

$(ROOT)/build/present_loop_harness: $(ROOT)/tools/present_loop_harness.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -o $@ $(ROOT)/tools/present_loop_harness.cpp

present-harness: $(ROOT)/build/present_loop_harness

$(ROOT)/build/ffmpeg_cpu_probe: $(ROOT)/tools/ffmpeg_cpu_probe.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tools/ffmpeg_cpu_probe.cpp

profile-tools: $(ROOT)/build/ffmpeg_cpu_probe $(ROOT)/build/ddr_write_bench $(ROOT)/build/present_loop_harness

# ARM hard-float for MiSTer (try common cross compilers + local mistercast toolchain)
ARM_TOOLCHAIN_BIN ?= /opt/arm-gnu-toolchain/bin
ARM_CXX ?= $(shell command -v arm-none-linux-gnueabihf-g++ 2>/dev/null || command -v arm-linux-gnueabihf-g++ 2>/dev/null || command -v armv7l-linux-gnueabihf-g++ 2>/dev/null || ls $(ARM_TOOLCHAIN_BIN)/arm-none-linux-gnueabihf-g++ 2>/dev/null)

# Fully static: MiSTer glibc is 2.31; modern toolchains need 2.32+ for dynamic.
# whole-archive pthread required for std::thread under -static.
# Static iconv can load incompatible device gconv DSOs even without an ELF interpreter.
# Prefix and build dir are named from the configure-line hash so a shared
# tree (the old ffmpeg-arm-static-safe default) cannot satisfy this link.
# `=` rather than `?=`: a stale ARM_FFMPEG_PREFIX in the environment (the
# 32-bit-time_t tree) must not override the time64 prefix. A command-line
# assignment still wins.
ARM_FFMPEG_CONFIG_HASH := $(shell python3 $(ROOT)/scripts/check_arm_ffmpeg_static.py --print-config-hash)
ARM_FFMPEG_PREFIX = $(ROOT)/build/deps/ffmpeg-arm-$(ARM_FFMPEG_CONFIG_HASH)-time64
FFMPEG_BUILD_DIR = $(ROOT)/build/deps/ffmpeg-8.1.2-$(ARM_FFMPEG_CONFIG_HASH)-time64-build
export FFMPEG_BUILD_DIR
ARM_NM ?= $(patsubst %g++,%nm,$(ARM_CXX))
ARM_PLEXD_OUTPUT ?= $(ROOT)/build/arm/misterplexd
ARM_PLEXD_AV_TRACE ?= 0
# GCAST P2: ARM_PLEXD_AIRPLAY=1 links UxPlay libairplay (scripts/build_arm_airplay.sh) for GAIRPLAY=1.
# Default is on. ALLOW_NO_AIRPLAY=1 is the opt-out: stub TLS client, no AirPlay.
ifeq ($(ALLOW_NO_AIRPLAY),1)
ARM_PLEXD_AIRPLAY := 0
else
ARM_PLEXD_AIRPLAY ?= 1
endif
ifeq ($(ARM_PLEXD_AIRPLAY),1)
else
ifneq ($(ALLOW_NO_AIRPLAY),1)
$(error arm-plexd: ARM_PLEXD_AIRPLAY is not 1, so the daemon would ship a stub TLS client and no AirPlay. Pass ARM_PLEXD_AIRPLAY=1, or ALLOW_NO_AIRPLAY=1 to opt out on purpose.)
endif
endif
ARM_AIRPLAY_PREFIX ?= $(ROOT)/build/deps/airplay-arm
# Same static OpenSSL the HTTPS-enabled libav was configured against.
ARM_OPENSSL_PREFIX ?= $(ARM_AIRPLAY_PREFIX)
ifeq ($(ARM_PLEXD_AIRPLAY),1)
ARM_AIRPLAY_FLAGS := -DMPX_HAVE_AIRPLAY -DMPX_HAVE_GCAST_TLS -I$(ARM_AIRPLAY_PREFIX)/include/uxplay -I$(ARM_AIRPLAY_PREFIX)/include
ARM_AIRPLAY_LIBS := $(ARM_AIRPLAY_PREFIX)/lib/libairplay.a $(ARM_AIRPLAY_PREFIX)/lib/libplist-2.0.a $(ARM_AIRPLAY_PREFIX)/lib/libssl.a $(ARM_AIRPLAY_PREFIX)/lib/libcrypto.a
else
ARM_AIRPLAY_FLAGS :=
ARM_AIRPLAY_LIBS :=
endif
ifneq ($(ARM_PLEXD_AV_TRACE),0)
ifneq ($(ARM_PLEXD_AV_TRACE),1)
$(error ARM_PLEXD_AV_TRACE must be 0 or 1)
endif
endif
ARM_FFMPEG_CFLAGS ?= -I$(ARM_FFMPEG_PREFIX)/include
ARM_FFMPEG_LIBS   ?= -L$(ARM_FFMPEG_PREFIX)/lib -Wl,--start-group -lavformat -lavcodec -lswresample -lavutil -Wl,--end-group -pthread -lm -latomic
MPX_LIBAV_PREFIX ?= $(ARM_FFMPEG_PREFIX)
MPX_LIBAV_HAVE := $(and $(wildcard $(ARM_FFMPEG_PREFIX)/lib/libavcodec.a),$(wildcard $(ARM_FFMPEG_PREFIX)/lib/libavformat.a),$(wildcard $(ARM_FFMPEG_PREFIX)/lib/libswresample.a),$(wildcard $(ARM_FFMPEG_PREFIX)/lib/libavutil.a))

arm-ffmpeg:
	@ARM_CXX="$(ARM_CXX)" ARM_FFMPEG_PREFIX="$(ARM_FFMPEG_PREFIX)" FFMPEG_BUILD_DIR="$(FFMPEG_BUILD_DIR)" \
		ARM_OPENSSL_PREFIX="$(ARM_OPENSSL_PREFIX)" ARM_AIRPLAY_PREFIX="$(ARM_AIRPLAY_PREFIX)" \
		bash $(ROOT)/scripts/build_arm_ffmpeg.sh

arm-ffmpeg-static-check:
	@python3 $(ROOT)/scripts/check_arm_ffmpeg_static.py --prefix "$(ARM_FFMPEG_PREFIX)" --nm "$(ARM_NM)"

# DASH URLs are https. This fails closed when the hashed libav has no https protocol.
$(ROOT)/build/test_ffmpeg_https.ok: $(ROOT)/tests/unit/test_ffmpeg_https.c
	@mkdir -p $(ROOT)/build
	@test -f "$(ARM_FFMPEG_PREFIX)/lib/libavformat.a" || { echo "https protocol test: missing $(ARM_FFMPEG_PREFIX)"; exit 1; }
	@test -f "$(ARM_OPENSSL_PREFIX)/lib/libssl.a" || { echo "https protocol test: missing OpenSSL under $(ARM_OPENSSL_PREFIX)"; exit 1; }
	@command -v arm-linux-gnueabihf-gcc >/dev/null || { echo "https protocol test: arm-linux-gnueabihf-gcc missing"; exit 1; }
	@command -v qemu-arm-static >/dev/null || { echo "https protocol test: qemu-arm-static missing"; exit 1; }
	arm-linux-gnueabihf-gcc -std=c11 -O2 -Wall -D_FILE_OFFSET_BITS=64 -D_TIME_BITS=64 \
		-I$(ARM_FFMPEG_PREFIX)/include \
		-o $(ROOT)/build/test_ffmpeg_https $(ROOT)/tests/unit/test_ffmpeg_https.c \
		-L$(ARM_FFMPEG_PREFIX)/lib -Wl,--start-group -lavformat -lavcodec -lswresample -lavutil -Wl,--end-group \
		-L$(ARM_OPENSSL_PREFIX)/lib -lssl -lcrypto -pthread -lm -latomic -ldl
	qemu-arm-static -L /usr/arm-linux-gnueabihf $(ROOT)/build/test_ffmpeg_https
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_ffmpeg_https.ok

arm-ddr-bench:
	@if [ -z "$(ARM_CXX)" ]; then echo "No armhf g++ found"; exit 1; fi
	@mkdir -p $(ROOT)/build/arm
	$(ARM_CXX) -std=c++17 -O2 -Wall -I$(ROOT)/host \
		-o $(ROOT)/build/arm/ddr_write_bench \
		$(ROOT)/tools/ddr_write_bench.cpp \
		-static
	@file $(ROOT)/build/arm/ddr_write_bench

# Lab-only PL330 ingest bench. Does NOT ring doorbell. Requires saw_executing +
# content match; impossible throughput hard-fails. Do NOT unbind dma-pl330.
arm-pl330-bench:
	@if [ -z "$(ARM_CXX)" ]; then echo "No armhf g++ found"; exit 1; fi
	@mkdir -p $(ROOT)/build/arm
	$(ARM_CXX) -std=c++17 -O2 -Wall -I$(ROOT)/host -pthread \
		-o $(ROOT)/build/arm/ddr_pl330_ingest_bench \
		$(ROOT)/tools/ddr_pl330_ingest_bench.cpp \
		-static -Wl,--whole-archive -lpthread -Wl,--no-whole-archive
	@file $(ROOT)/build/arm/ddr_pl330_ingest_bench

arm-profile-tools: arm-ddr-bench arm-pl330-bench
	@if [ -z "$(ARM_CXX)" ]; then echo "No armhf g++ found"; exit 1; fi
	@mkdir -p $(ROOT)/build/arm
	$(ARM_CXX) -std=c++17 -O2 -Wall -I$(ROOT)/host \
		-o $(ROOT)/build/arm/ffmpeg_cpu_probe \
		$(ROOT)/tools/ffmpeg_cpu_probe.cpp \
		-static
	$(ARM_CXX) -std=c++17 -O2 -Wall -pthread \
		-o $(ROOT)/build/arm/present_loop_harness \
		$(ROOT)/tools/present_loop_harness.cpp \
		-static
	@file $(ROOT)/build/arm/ffmpeg_cpu_probe $(ROOT)/build/arm/present_loop_harness

KDIR_MISTER ?= /data/misterplex/kernel/Linux-Kernel_MiSTer
ARM_CROSS ?= arm-none-linux-gnueabihf-
# K1 KM-0: the module must be built with the compiler that configured KDIR_MISTER (CONFIG_GCC_PLUGINS=y: the
# arm_ssp_per_task plugin only loads in that gcc). The MiSTer 5.15.1 tree wants Arm GNU Toolchain 10.2-2020.11.
KMOD_CROSS ?= arm-none-linux-gnueabihf-
arm-kmod:
	@want="$$(sed -n 's/^CONFIG_CC_VERSION_TEXT="\(.*\)"$$/\1/p' $(KDIR_MISTER)/.config)"; \
	have="$$($(KMOD_CROSS)gcc --version 2>/dev/null | head -n 1)"; \
	[ -n "$$want" ] && [ "$$want" = "$$have" ] || \
	{ echo "arm-kmod: $(KMOD_CROSS)gcc is '$$have', $(KDIR_MISTER) was configured with '$$want'"; exit 1; }
	$(MAKE) -C $(ROOT)/kmod/mplex_ddr KDIR=$(KDIR_MISTER) CROSS_COMPILE=$(KMOD_CROSS) ARCH=arm
	@file $(ROOT)/kmod/mplex_ddr/mplex_ddr.ko

arm-plexd: arm-plexd-daemon arm-ddr-bench arm-plexd-tools

arm-plexd-daemon: $(MPLEX_HDR) $(OWENB_RING_ARM_OBJS)
	@if [ -z "$(ARM_CXX)" ]; then echo "No armhf g++ found"; exit 1; fi
	@mkdir -p "$(dir $(ARM_PLEXD_OUTPUT))"
ifneq ($(MPX_LIBAV_HAVE),)
	@python3 $(ROOT)/scripts/check_arm_ffmpeg_static.py --prefix "$(ARM_FFMPEG_PREFIX)" --nm "$(ARM_NM)"
ifeq ($(ARM_PLEXD_AIRPLAY),1)
	@test -f "$(ARM_AIRPLAY_PREFIX)/lib/libairplay.a" || { \
		echo "arm-plexd-daemon: ARM_PLEXD_AIRPLAY=1 but $(ARM_AIRPLAY_PREFIX)/lib/libairplay.a is missing."; \
		echo "Build it with scripts/build_arm_airplay.sh"; exit 1; }
endif
	$(ARM_CXX) -std=c++17 -O2 -Wall -static -D_TIME_BITS=64 -D_FILE_OFFSET_BITS=64 $(MPLEX_INC) -DMPX_HAVE_LIBAV $(ARM_FFMPEG_CFLAGS) \
		$(OWENB_DAEMON_FLAGS) -DMPX_FPGA_AV_TRACE=$(ARM_PLEXD_AV_TRACE) $(ARM_AIRPLAY_FLAGS) \
		-o "$(ARM_PLEXD_OUTPUT)" $(MPLEX_SRC) \
		$(ROOT)/arm/misterplexd/av_inproc_decode.cpp $(ROOT)/arm/misterplexd/owenb_av_source.cpp \
		$(OWENB_PLAY_SRCS) $(OWENB_RING_ARM_OBJS) \
		$(ARM_FFMPEG_LIBS) $(ARM_AIRPLAY_LIBS) \
		-Wl,--whole-archive -lpthread -Wl,--no-whole-archive
	@echo MPX_HAVE_LIBAV=YES prefix=$(ARM_FFMPEG_PREFIX)
else
	@echo MPX_HAVE_LIBAV=NO prefix=$(ARM_FFMPEG_PREFIX)
ifeq ($(ALLOW_NO_AIRPLAY),1)
	$(ARM_CXX) -std=c++17 -O2 -Wall -D_TIME_BITS=64 -D_FILE_OFFSET_BITS=64 $(MPLEX_INC) $(OWENB_DAEMON_FLAGS) -DMPX_FPGA_AV_TRACE=$(ARM_PLEXD_AV_TRACE) \
		-o "$(ARM_PLEXD_OUTPUT)" $(MPLEX_SRC) $(OWENB_PLAY_SRCS) $(OWENB_RING_ARM_OBJS) \
		-static -Wl,--whole-archive -lpthread -Wl,--no-whole-archive
else
	@echo "arm-plexd-daemon: libav static libs missing under $(ARM_FFMPEG_PREFIX)." >&2
	@echo "Build them with 'make arm-ffmpeg' and scripts/build_arm_airplay.sh." >&2
	@echo "ALLOW_NO_AIRPLAY=1 opts out and builds the stub TLS client with no AirPlay." >&2
	@exit 1
endif
endif
	# Arm GNU's static libc and libstdc++ are built -g. Linking them leaves
	# about 14 MB of DWARF in the ship binary (the c9fb88e daemon was 25 MB
	# with a 6.4 MB .text). 55ead85's 11 MB daemon was linked by Ubuntu's
	# armhf g++ and kept only a little debug. Drop .debug_* after the link.
	# The symbol table stays. The ASan binary is built separately and is not
	# stripped.
	$(patsubst %g++,%strip,$(ARM_CXX)) --strip-debug "$(ARM_PLEXD_OUTPUT)"
	@file "$(ARM_PLEXD_OUTPUT)"
	@echo "Built $(ARM_PLEXD_OUTPUT)"

arm-plexd-tools:
	@if [ -z "$(ARM_CXX)" ]; then echo "No armhf g++ found"; exit 1; fi
	@mkdir -p $(ROOT)/build/arm
	$(ARM_CXX) -std=c++17 -O2 -Wall -Wextra -D_FILE_OFFSET_BITS=64 -I$(ROOT)/host \
		-o $(ROOT)/build/arm/mplex_ddr_probe $(ROOT)/tools/mplex_ddr_probe.cpp -static
	$(ARM_CXX) -std=c++17 -O2 -Wall -I$(ROOT)/arm/misterplexd -I$(ROOT)/host \
		-o $(ROOT)/build/arm/push_frame \
		$(ROOT)/tools/push_frame.cpp $(ROOT)/arm/misterplexd/fpga_spi.cpp \
		-static
	$(ARM_CXX) -std=c++17 -O2 -Wall -I$(ROOT)/arm/misterplexd -I$(ROOT)/host \
		-o $(ROOT)/build/arm/set_status \
		$(ROOT)/tools/set_status.cpp $(ROOT)/arm/misterplexd/fpga_spi.cpp \
		-static
	$(ARM_CXX) -std=c++17 -O2 -Wall -I$(ROOT)/host \
		-o $(ROOT)/build/arm/input_mailbox_probe \
		$(ROOT)/tools/input_mailbox_probe.cpp \
		-static
	@file $(ROOT)/build/arm/push_frame $(ROOT)/build/arm/set_status $(ROOT)/build/arm/input_mailbox_probe
	@echo "Built push_frame + set_status + input_mailbox_probe"

$(ROOT)/build/arm/input_mailbox_probe: $(ROOT)/tools/input_mailbox_probe.cpp \
		$(ROOT)/host/libmisterplex/input_mailbox.hpp \
		$(ROOT)/host/libmisterplex/fpga_build_ids.hpp \
		$(ROOT)/host/libmisterplex/plx_page.hpp \
		$(ROOT)/host/libmisterplex/mailbox_abi_spec.hpp
	@if [ -z "$(ARM_CXX)" ]; then echo "No armhf g++ found"; exit 1; fi
	@mkdir -p $(ROOT)/build/arm
	$(ARM_CXX) -std=c++17 -O2 -Wall -I$(ROOT)/host \
		-o $@ $(ROOT)/tools/input_mailbox_probe.cpp -static

MISTER_DEV ?= $(HOME)/misterfpga-dev
build-rbf:
	$(MISTER_DEV)/scripts/mister-dev build $(ROOT)/fpga/Plex_MiSTer --qpf Plex.qpf

package:
	$(ROOT)/scripts/package_release.sh
	REQUIRE_ARTIFACT=1 $(ROOT)/tests/unit/test_no_private_data.sh
	REQUIRE_ARTIFACT=1 $(ROOT)/tests/unit/test_release_rbf_hash.sh

clean:
	rm -rf $(ROOT)/build

$(ROOT)/build/test_gdm_filter: $(ROOT)/tests/unit/test_gdm_filter.cpp \
		$(ROOT)/host/libmisterplex/gdm_filter.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_gdm_filter.cpp

$(ROOT)/build/test_spi_txn_complete: $(ROOT)/tests/unit/test_spi_txn_complete.cpp \
		$(ROOT)/host/libmisterplex/spi_ack_wait.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -o $@ $(ROOT)/tests/unit/test_spi_txn_complete.cpp

$(ROOT)/build/test_idle_poll_budget: $(ROOT)/tests/unit/test_idle_poll_budget.cpp \
		$(ROOT)/host/libmisterplex/spi_ack_wait.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -o $@ $(ROOT)/tests/unit/test_idle_poll_budget.cpp

$(ROOT)/build/test_p720_e2e_budget: $(ROOT)/tests/unit/test_p720_e2e_budget.cpp \
		$(ROOT)/host/libmisterplex/p720_e2e_budget.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_p720_e2e_budget.cpp

$(ROOT)/build/test_pl330_encode: $(ROOT)/tests/unit/test_pl330_encode.cpp \
		$(ROOT)/host/libmisterplex/pl330_mem2mem.hpp \
		$(ROOT)/host/libmisterplex/ddr_frame_layout.hpp \
		$(ROOT)/host/libmisterplex/ddr_zero_copy_ingest.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_pl330_encode.cpp


# HDMI glass regression floor (v0.2.0 pair) — see docs/glass-baseline-pair.md
glass-baseline-policy:
	bash $(ROOT)/tests/unit/test_glass_baseline_pair_policy.sh

glass-baseline: glass-baseline-policy
	bash $(ROOT)/tests/hw/test_glass_baseline_pair.sh

# v0.3.0 lab-stable @320 — isolated from product H5 (docs/v3-stable-320-loop.md)
v3-stable-320-policy:
	bash $(ROOT)/tests/unit/test_v030_lab_stable_policy.sh

v3-stable-320: v3-stable-320-policy
	bash $(ROOT)/scripts/v3_stable_320_loop.sh

# T-UNIT-DANGLING-3 (rev 2 §3.2): five unit tests that were built but never run, in the _race.ok form.
DANGLING3_OK := $(addprefix $(ROOT)/build/,test_browse_render_rg1.ok test_browse_render_rg3.ok \
	test_browse_render_rg4.ok test_browse_r1_adapter.ok test_av_sync.ok)
$(DANGLING3_OK): $(ROOT)/build/%.ok: $(ROOT)/build/%
	@env -u BROWSE_RENDER_UPDATE_GOLDENS $< > $@.log 2>&1 || { cat $@.log; exit 1; }
	@cat $@.log
	@cp $@.log $@
unit-unlocked unit-host-unlocked: $(DANGLING3_OK)

# T-UNIT-DANGLING-3 (rev 2 §3.3): each RG-2 mutant must fail its own pinned first check (SUPP-2 b4dfb1fa), after the
# base passes in the same recipe (the f5 form). A survivor, another first line, rc 124 or a signal fails the target.
RG2_MUTANTS := copy_dn_only no_full_after_skip no_repoint_after_timeout
RG2_KILL_copy_dn_only := RG-2 FAIL: shown buffer mismatch at walk step 2
RG2_KILL_no_full_after_skip := RG-2 FAIL: shown buffer mismatch at walk step 96
RG2_KILL_no_repoint_after_timeout := RG-2 FAIL: wrote idle buffer with outstanding unlatch flip request
RG2_KILLED = rc=0; timeout 120 $(ROOT)/build/test_browse_paint_rg2_mut_$(1) > $@.$(1).log 2>&1 || rc=$$?; \
	first=$$(grep -m1 '^RG-2 FAIL: ' $@.$(1).log); [ "$$rc:$$first" = "1:$(RG2_KILL_$(1))" ] || \
	{ echo "RG-2 MUTANT $(1) NOT KILLED AS PINNED: rc $$rc, first: $$first"; exit 1; }
$(ROOT)/build/test_browse_paint_rg2.mutant-check: $(ROOT)/build/test_browse_paint_rg2 \
		$(addprefix $(ROOT)/build/test_browse_paint_rg2_mut_,$(RG2_MUTANTS))
	@$(ROOT)/build/test_browse_paint_rg2 > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@$(call RG2_KILLED,copy_dn_only)
	@$(call RG2_KILLED,no_full_after_skip)
	@$(call RG2_KILLED,no_repoint_after_timeout)
	@echo "test_browse_paint_rg2: mutants $(RG2_MUTANTS) FAIL as pinned" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_browse_paint_rg2.mutant-check

# T-UNIT-DANGLING-3 (rev 2 §3.4, F2; review 09:03 C1): MUT_C1_LOCK_ORDER_ABBA must trip the C1 watchdog. It runs
# alone in a private tmpfs /tmp + PID ns (d2: tests/unit/ns_tmp_pid.sh), so no other SPI flock user can fake the bite.
# rc 2 with 'FAIL C1: no progress in 90 s' first = bites. The rule-1 CHECK first + the C1 line + rc 2 = 'C1 TOOTH VOID'
# (d1; record_check rule (1b)). Anything else = 'C1 TOOTH DID NOT BITE'.
C1_RULE1_COND := !ok && ds == 0 && !gate.open()
$(ROOT)/build/test_core_gate.c1-check: $(ROOT)/build/test_core_gate_mut_c1_abba $(ROOT)/tests/unit/ns_tmp_pid.sh
	@hd=$$(stat -c %d /tmp); rc=0; \
	timeout 180 $(ROOT)/tests/unit/ns_tmp_pid.sh $(ROOT)/build/test_core_gate_mut_c1_abba > $@.log 2>&1 || rc=$$?; \
	ns=$$(head -n 1 $@.log); d=$${ns#* dev=}; d=$${d%% *}; \
	case "$$ns" in "NS uid=$$(id -u) gid=$$(id -g) pid=1 fstype=tmpfs empty=yes dev="*" unshare="*) ;; \
	*) echo "C1 TOOTH NOT ISOLATED: $$ns"; exit 1 ;; esac; \
	[ "$$d" != "$$hd" ] || { echo "C1 TOOTH NOT ISOLATED: /tmp dev $$d is the host's ($$ns)"; exit 1; }; \
	first=$$(grep -m1 '^FAIL ' $@.log); \
	case "$$rc:$$first" in \
	"2:FAIL C1: no progress in 90 s"*) ;; \
	"2:FAIL $(ROOT)/tests/unit/test_core_gate.cpp:"*": $(C1_RULE1_COND)") \
		if grep -q '^FAIL C1: no progress in 90 s' $@.log; \
		then echo "C1 TOOTH VOID: rule-1 first FAIL, C1 line present, rc 2"; \
		else echo "C1 TOOTH DID NOT BITE: rc $$rc, first FAIL: $$first"; fi; exit 1 ;; \
	*) echo "C1 TOOTH DID NOT BITE: rc $$rc, first FAIL: $$first"; exit 1 ;; esac
	@echo "test_core_gate: mutant MUT_C1_LOCK_ORDER_ABBA trips the C1 watchdog (rc 2) as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_core_gate.c1-check

# T-UNIT-DANGLING-3 (rev 2 §4): every build/test_* binary is run by a recipe that unit-unlocked reaches, or is listed
# in tests/unit/unit_build_only.txt. Standing bar 2: a fresh PYTHONPYCACHEPREFIX for every run.
$(ROOT)/build/test_unit_targets_run.ok: $(ROOT)/tests/unit/test_unit_targets_run.py \
		$(ROOT)/tests/unit/unit_build_only.txt $(ROOT)/tests/unit/ns_tmp_pid.sh \
		$(ROOT)/tests/unit/test_l2_transcode_dar_demux.sh $(ROOT)/Makefile
	@p=$$(mktemp -d); PYTHONPYCACHEPREFIX=$$p timeout 900 python3 $(ROOT)/tests/unit/test_unit_targets_run.py \
		--root $(ROOT) > $@.log 2>&1; rc=$$?; rm -rf $$p; [ $$rc = 0 ] || { cat $@.log; exit 1; }
	@cat $@.log
	@cp $@.log $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_unit_targets_run.ok

# F1 PLXL beat (F1-PREREG v2.2 ba1ae917; review 01:39 k1-k3, H264 53b1779c x1-x3): f1_beat.hpp's Beat on the
# real FpgaSpi::plxlBeatStore, a sparse fake memory file and a managed core gate. Appended at the end of the
# file so that no existing row moves. .ok runs it, .mutant-check runs every mutant (each must FAIL), and
# .tsan-check runs the TSan stress row (no report).
F1_BEAT_DEPS := $(ROOT)/tests/unit/test_f1_plxl_beat.cpp $(ROOT)/arm/misterplexd/fpga_spi.cpp \
	$(ROOT)/arm/misterplexd/fpga_spi.hpp $(ROOT)/host/libmisterplex/f1_beat.hpp \
	$(ROOT)/host/libmisterplex/core_gate.hpp $(ROOT)/host/libmisterplex/owenb_core_identity.hpp $(ROOT)/host/libmisterplex/fpga_build_ids.hpp
F1_BEAT_FLAGS := -I$(ROOT)/arm/misterplexd -pthread -DMPX_TEST_F1_BEAT_SEAM
F1_BEAT_MUTANT_ROWS := MUT_F1_BEAT_ANYROW:B-b MUT_F1_BEAT_IN_SESSION:B-d MUT_F1_BEAT_MAPS:B-e \
	MUT_F1_BEAT_BLOCKLOCK:B-f MUT_F1_BEAT_CHOKEPOINT:B-g MUT_F1_BEAT_MAGIC_ONCE:B-h MUT_F1_BEAT_NOSEED:B-i \
	MUT_F1_IDLE_LEGACY:B-l
F1_BEAT_MUTANTS := $(foreach mr,$(F1_BEAT_MUTANT_ROWS),$(firstword $(subst :, ,$(mr))))
$(ROOT)/build/test_f1_plxl_beat: $(F1_BEAT_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(F1_BEAT_FLAGS) -DMPX_DEVMEM_PATH='"$(ROOT)/build/test_f1_plxl_beat.fakemem"' \
		-o $@ $(ROOT)/tests/unit/test_f1_plxl_beat.cpp $(ROOT)/arm/misterplexd/fpga_spi.cpp
$(ROOT)/build/test_f1_plxl_beat.ok: $(ROOT)/build/test_f1_plxl_beat
	@$(ROOT)/build/test_f1_plxl_beat > $@.log 2>&1 || { cat $@.log; exit 1; }
	@cat $@.log
	@cp $@.log $@
$(ROOT)/build/test_f1_plxl_beat_%: $(F1_BEAT_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(F1_BEAT_FLAGS) -D$* -DMPX_DEVMEM_PATH='"$(ROOT)/build/test_f1_plxl_beat_$*.fakemem"' \
		-o $@ $(ROOT)/tests/unit/test_f1_plxl_beat.cpp $(ROOT)/arm/misterplexd/fpga_spi.cpp
# Each mutant runs its pre-registered row alone, after the base passed (.ok): KILLED = rc 1 and the
# first FAIL line names that row. A crash, a timeout, rc != 1 or another row's FAIL is NOT a kill.
# The check runs the base again first, and loops over F1_BEAT_MUTANTS by name, looking each row up in
# F1_BEAT_MUTANT_ROWS. That is the shape test_unit_targets_run can expand (T-UNIT-DANGLING-3: every
# mutant counts as RUN, and r1 sees the base first), the same as test_ddr_page_mapper.mutant-check.
$(ROOT)/build/test_f1_plxl_beat.mutant-check: $(ROOT)/build/test_f1_plxl_beat.ok $(addprefix $(ROOT)/build/test_f1_plxl_beat_,$(F1_BEAT_MUTANTS))
	@$(ROOT)/build/test_f1_plxl_beat > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@: > $@.log; for m in $(F1_BEAT_MUTANTS); do r=$$(printf '%s\n' $(F1_BEAT_MUTANT_ROWS) | sed -n "s/^$$m://p"); \
		[ -n "$$r" ] || { echo "MUTANT WITHOUT A ROW: $$m"; exit 1; }; \
		timeout 120 $(ROOT)/build/test_f1_plxl_beat_$$m $$r > $@.$$m.log 2>&1; rc=$$?; \
		first=$$(grep -m1 '^FAIL ' $@.$$m.log || true); \
		if [ $$rc -ne 1 ] || ! printf '%s\n' "$$first" | grep -q ": $$r: "; then \
			echo "MUTANT NOT KILLED ON ITS ROW: $$m ($$r) rc=$$rc first='$$first'"; cat $@.$$m.log; exit 1; fi; \
		echo "KILLED $$m on $$r: $$first" | tee -a $@.log; \
	done
	@echo "test_f1_plxl_beat: mutants $(F1_BEAT_MUTANTS) each FAIL on their own row" | tee -a $@.log
	@cp $@.log $@
$(ROOT)/build/test_f1_plxl_beat_tsan: $(F1_BEAT_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(F1_BEAT_FLAGS) -O1 -g -fsanitize=thread \
		-DMPX_DEVMEM_PATH='"$(ROOT)/build/test_f1_plxl_beat_tsan.fakemem"' \
		-o $@ $(ROOT)/tests/unit/test_f1_plxl_beat.cpp $(ROOT)/arm/misterplexd/fpga_spi.cpp
$(ROOT)/build/test_f1_plxl_beat.tsan-check: $(ROOT)/build/test_f1_plxl_beat_tsan
	@TSAN_OPTIONS='halt_on_error=1 exitcode=66' $(ROOT)/build/test_f1_plxl_beat_tsan tsan > $@.log 2>&1 || { cat $@.log; exit 1; }
	@! grep -q 'WARNING: ThreadSanitizer' $@.log || { cat $@.log; exit 1; }
	@cat $@.log
	@cp $@.log $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_f1_plxl_beat.ok $(ROOT)/build/test_f1_plxl_beat.mutant-check $(ROOT)/build/test_f1_plxl_beat.tsan-check

# Remote transcode buffer params (fix/remote-buffer-playbar-1011).
REMOTE_BUF_SRC := $(ROOT)/tests/unit/test_remote_buffer_params.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp
$(ROOT)/build/test_remote_buffer_params: $(REMOTE_BUF_SRC) $(ROOT)/arm/misterplexd/plex_resolve.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/host -I$(ROOT)/arm/misterplexd -pthread -o $@ $(REMOTE_BUF_SRC)
$(ROOT)/build/test_remote_buffer_params_MUT: $(REMOTE_BUF_SRC) $(ROOT)/arm/misterplexd/plex_resolve.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/host -I$(ROOT)/arm/misterplexd -pthread -DMUT_REMOTE_BUFFER_102400 -o $@ $(REMOTE_BUF_SRC)
$(ROOT)/build/test_remote_buffer_params.mutant-check: $(ROOT)/build/test_remote_buffer_params $(ROOT)/build/test_remote_buffer_params_MUT
	@$(ROOT)/build/test_remote_buffer_params > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@if $(ROOT)/build/test_remote_buffer_params_MUT > $@.mut.log 2>&1; then echo "MUTANT SURVIVED"; exit 1; fi
	@grep -m1 'FAIL ' $@.mut.log | grep -q 'B1 ' || { echo "MUTANT KILLED BY THE WRONG CHECK"; cat $@.mut.log; exit 1; }
	@echo "test_remote_buffer_params: MUT_REMOTE_BUFFER_102400 FAILS at B1 as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_remote_buffer_params.mutant-check

# Fixed PMS client profile name on every request path (a remote PMS logged "Unable to find client profile
# MiSTerPlex-FPGA-IP-480-23976-filter-off"). Mutant MUT_PROFILE_NAME_VARIANT = the old dynamic name.
PROFILE_NAME_FIXED_SRC := $(ROOT)/tests/unit/test_profile_name_fixed.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/arm/misterplexd/pms_timeline.cpp
$(ROOT)/build/test_profile_name_fixed: $(PROFILE_NAME_FIXED_SRC) $(TEST_RESOLVE_DEPS) $(ROOT)/arm/misterplexd/pms_timeline.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -o $@ $(PROFILE_NAME_FIXED_SRC)
$(ROOT)/build/test_profile_name_fixed.mutant-check: $(ROOT)/build/test_profile_name_fixed
	@$(ROOT)/build/test_profile_name_fixed > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -DMUT_PROFILE_NAME_ALWAYS -o $@.MUT_PROFILE_NAME_ALWAYS $(PROFILE_NAME_FIXED_SRC)
	@if $@.MUT_PROFILE_NAME_ALWAYS > $@.MUT_PROFILE_NAME_ALWAYS.log 2>&1; then echo "MUTANT SURVIVED: MUT_PROFILE_NAME_ALWAYS"; exit 1; fi
	@grep -m1 'FAIL ' $@.MUT_PROFILE_NAME_ALWAYS.log | grep -q 'N0 WAN decision' || { echo "MUTANT KILLED BY THE WRONG CHECK: MUT_PROFILE_NAME_ALWAYS"; cat $@.MUT_PROFILE_NAME_ALWAYS.log; exit 1; }
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -DMUT_NO_EXTRA_LAN -o $@.MUT_NO_EXTRA_LAN $(PROFILE_NAME_FIXED_SRC)
	@if $@.MUT_NO_EXTRA_LAN > $@.MUT_NO_EXTRA_LAN.log 2>&1; then echo "MUTANT SURVIVED: MUT_NO_EXTRA_LAN"; exit 1; fi
	@grep -m1 'FAIL ' $@.MUT_NO_EXTRA_LAN.log | grep -q 'N0 WAN decision Extra' || { echo "MUTANT KILLED BY THE WRONG CHECK: MUT_NO_EXTRA_LAN"; cat $@.MUT_NO_EXTRA_LAN.log; exit 1; }
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -DMUT_PROFILE_NAME_VARIANT -o $@.MUT_PROFILE_NAME_VARIANT $(PROFILE_NAME_FIXED_SRC)
	@if $@.MUT_PROFILE_NAME_VARIANT > $@.MUT_PROFILE_NAME_VARIANT.log 2>&1; then echo "MUTANT SURVIVED: MUT_PROFILE_NAME_VARIANT"; exit 1; fi
	@grep -m1 'FAIL ' $@.MUT_PROFILE_NAME_VARIANT.log | grep -q 'N1 WAN decision' || { echo "MUTANT KILLED BY THE WRONG CHECK: MUT_PROFILE_NAME_VARIANT"; cat $@.MUT_PROFILE_NAME_VARIANT.log; exit 1; }
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -DMUT_PROFILE_NAME_LAN_FIXED -o $@.MUT_PROFILE_NAME_LAN_FIXED $(PROFILE_NAME_FIXED_SRC)
	@if $@.MUT_PROFILE_NAME_LAN_FIXED > $@.MUT_PROFILE_NAME_LAN_FIXED.log 2>&1; then echo "MUTANT SURVIVED: MUT_PROFILE_NAME_LAN_FIXED"; exit 1; fi
	@grep -m1 'FAIL ' $@.MUT_PROFILE_NAME_LAN_FIXED.log | grep -q 'N1 LAN decision' || { echo "MUTANT KILLED BY THE WRONG CHECK: MUT_PROFILE_NAME_LAN_FIXED"; cat $@.MUT_PROFILE_NAME_LAN_FIXED.log; exit 1; }
	@echo "test_profile_name_fixed: mutants MUT_PROFILE_NAME_ALWAYS MUT_NO_EXTRA_LAN MUT_PROFILE_NAME_VARIANT MUT_PROFILE_NAME_LAN_FIXED FAIL at their pinned checks as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_profile_name_fixed $(ROOT)/build/test_profile_name_fixed.mutant-check

# MPX_PREWARM probe: off by default, binary-safe status read (a lab MiSTer C1 http=0).
$(ROOT)/build/test_prewarm_probe: $(ROOT)/tests/unit/test_prewarm_probe.cpp $(TEST_RESOLVE_DEPS) $(ROOT)/arm/misterplexd/seek_reuse.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -o $@ $< $(ROOT)/arm/misterplexd/plex_resolve.cpp
$(ROOT)/build/test_prewarm_probe.mutant-check: $(ROOT)/build/test_prewarm_probe
	@$(ROOT)/build/test_prewarm_probe > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -DMUT_PREWARM_FGETS -o $@.MUT_PREWARM_FGETS $(ROOT)/tests/unit/test_prewarm_probe.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp
	@if $@.MUT_PREWARM_FGETS > $@.MUT_PREWARM_FGETS.log 2>&1; then echo "MUTANT SURVIVED: MUT_PREWARM_FGETS"; exit 1; fi
	@grep -m1 'FAIL ' $@.MUT_PREWARM_FGETS.log | grep -q 'W2 TS body' || { echo "MUTANT KILLED BY THE WRONG CHECK: MUT_PREWARM_FGETS"; cat $@.MUT_PREWARM_FGETS.log; exit 1; }
	@echo "test_prewarm_probe: mutants MUT_PREWARM_FGETS FAIL at their pinned checks as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_prewarm_probe $(ROOT)/build/test_prewarm_probe.mutant-check

# Decision calls per seek burst (a lab MiSTer A3 55 s recovery): seek reuses the session; superseded resolves skip the decision.
$(ROOT)/build/test_seek_decisions: $(ROOT)/tests/unit/test_seek_decisions.cpp $(TEST_RESOLVE_DEPS) $(ROOT)/arm/misterplexd/seek_reuse.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -o $@ $< $(ROOT)/arm/misterplexd/plex_resolve.cpp
$(ROOT)/build/test_seek_decisions.mutant-check: $(ROOT)/build/test_seek_decisions
	@$(ROOT)/build/test_seek_decisions > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -DMUT_SEEK_NO_REUSE -o $@.MUT_SEEK_NO_REUSE $(ROOT)/tests/unit/test_seek_decisions.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp
	@if $@.MUT_SEEK_NO_REUSE > $@.MUT_SEEK_NO_REUSE.log 2>&1; then echo "MUTANT SURVIVED: MUT_SEEK_NO_REUSE"; exit 1; fi
	@grep -m1 'FAIL ' $@.MUT_SEEK_NO_REUSE.log | grep -q 'D2 A3 burst' || { echo "MUTANT KILLED BY THE WRONG CHECK: MUT_SEEK_NO_REUSE"; cat $@.MUT_SEEK_NO_REUSE.log; exit 1; }
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -DMUT_RESOLVE_NO_ABORT -o $@.MUT_RESOLVE_NO_ABORT $(ROOT)/tests/unit/test_seek_decisions.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp
	@if $@.MUT_RESOLVE_NO_ABORT > $@.MUT_RESOLVE_NO_ABORT.log 2>&1; then echo "MUTANT SURVIVED: MUT_RESOLVE_NO_ABORT"; exit 1; fi
	@grep -m1 'FAIL ' $@.MUT_RESOLVE_NO_ABORT.log | grep -q 'D3 coalesced' || { echo "MUTANT KILLED BY THE WRONG CHECK: MUT_RESOLVE_NO_ABORT"; cat $@.MUT_RESOLVE_NO_ABORT.log; exit 1; }
	@echo "test_seek_decisions: mutants MUT_SEEK_NO_REUSE MUT_RESOLVE_NO_ABORT FAIL at their pinned checks as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_seek_decisions $(ROOT)/build/test_seek_decisions.mutant-check

# NO-REQUIRED-PROFILE: decision/SPS mismatch -> one lowered PMS retry -> clean fallback (NVENC refs replay).
$(ROOT)/build/test_no_profile_retry: $(ROOT)/tests/unit/test_no_profile_retry.cpp $(TEST_RESOLVE_DEPS) $(ROOT)/host/libmisterplex/gate_owner.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -DMPX_FIXTURES=\"$(ROOT)/tests/fixtures\" -o $@ $< $(ROOT)/arm/misterplexd/plex_resolve.cpp
$(ROOT)/build/test_no_profile_retry.mutant-check: $(ROOT)/build/test_no_profile_retry
	@$(ROOT)/build/test_no_profile_retry > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -DMPX_FIXTURES=\"$(ROOT)/tests/fixtures\" -DMUT_DECISION_CHECK_OFF -o $@.MUT_DECISION_CHECK_OFF $(ROOT)/tests/unit/test_no_profile_retry.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp
	@if $@.MUT_DECISION_CHECK_OFF > $@.MUT_DECISION_CHECK_OFF.log 2>&1; then echo "MUTANT SURVIVED: MUT_DECISION_CHECK_OFF"; exit 1; fi
	@grep -m1 'FAIL ' $@.MUT_DECISION_CHECK_OFF.log | grep -q 'P1 decision' || { echo "MUTANT KILLED BY THE WRONG CHECK: MUT_DECISION_CHECK_OFF"; cat $@.MUT_DECISION_CHECK_OFF.log; exit 1; }
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -DMPX_FIXTURES=\"$(ROOT)/tests/fixtures\" -DMUT_NO_LOWERED_RETRY -o $@.MUT_NO_LOWERED_RETRY $(ROOT)/tests/unit/test_no_profile_retry.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp
	@if $@.MUT_NO_LOWERED_RETRY > $@.MUT_NO_LOWERED_RETRY.log 2>&1; then echo "MUTANT SURVIVED: MUT_NO_LOWERED_RETRY"; exit 1; fi
	@grep -m1 'FAIL ' $@.MUT_NO_LOWERED_RETRY.log | grep -q 'R1 NVENC' || { echo "MUTANT KILLED BY THE WRONG CHECK: MUT_NO_LOWERED_RETRY"; cat $@.MUT_NO_LOWERED_RETRY.log; exit 1; }
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -DMPX_FIXTURES=\"$(ROOT)/tests/fixtures\" -DMUT_DECISION_PROFILE_FALLBACK -o $@.MUT_DECISION_PROFILE_FALLBACK $(ROOT)/tests/unit/test_no_profile_retry.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp
	@if $@.MUT_DECISION_PROFILE_FALLBACK > $@.MUT_DECISION_PROFILE_FALLBACK.log 2>&1; then echo "MUTANT SURVIVED: MUT_DECISION_PROFILE_FALLBACK"; exit 1; fi
	@grep -m1 'FAIL ' $@.MUT_DECISION_PROFILE_FALLBACK.log | grep -q 'P1r real variant decision' || { echo "MUTANT KILLED BY THE WRONG CHECK: MUT_DECISION_PROFILE_FALLBACK"; cat $@.MUT_DECISION_PROFILE_FALLBACK.log; exit 1; }
	@echo "test_no_profile_retry: mutants MUT_DECISION_CHECK_OFF MUT_NO_LOWERED_RETRY MUT_DECISION_PROFILE_FALLBACK FAIL at their pinned checks as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_no_profile_retry $(ROOT)/build/test_no_profile_retry.mutant-check

# NO-REQUIRED-PROFILE: VideoEncodeFlags travel in the Profile-Extra (add-transcode-target-settings).
$(ROOT)/build/test_encode_flags_extra: $(ROOT)/tests/unit/test_encode_flags_extra.cpp $(TEST_RESOLVE_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -o $@ $< $(ROOT)/arm/misterplexd/plex_resolve.cpp

$(ROOT)/build/test_encode_flags_extra.mutant-check: $(ROOT)/build/test_encode_flags_extra
	@$(ROOT)/build/test_encode_flags_extra > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in MUT_NO_VEF_TERM:E1 MUT_VEF_CABAC_ON:E2; do n=$${m%%:*}; pin=$${m##*:}; \
	  $(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -D$$n -o $@.$$n $(ROOT)/tests/unit/test_encode_flags_extra.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp || exit 1; \
	  if $@.$$n > $@.$$n.log 2>&1; then echo "MUTANT SURVIVED: $$n"; exit 1; fi; \
	  grep -m1 '^FAIL ' $@.$$n.log | grep -q "FAIL $$pin " || { echo "MUTANT KILLED BY THE WRONG CHECK: $$n"; cat $@.$$n.log; exit 1; }; done
	@echo "test_encode_flags_extra: mutants MUT_NO_VEF_TERM MUT_VEF_CABAC_ON FAIL at their pinned checks as required"
	@touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_encode_flags_extra.mutant-check

# Round E 1011: named-profile Extra non-destructive, the profile-name log, decision zero fields unknown.
$(ROOT)/build/test_named_profile_extra: $(ROOT)/tests/unit/test_named_profile_extra.cpp $(TEST_RESOLVE_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -o $@ $< $(ROOT)/arm/misterplexd/plex_resolve.cpp

$(ROOT)/build/test_named_profile_extra.mutant-check: $(ROOT)/build/test_named_profile_extra
	@$(ROOT)/build/test_named_profile_extra > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in MUT_NAMED_EXTRA_DESTRUCTIVE:N1 MUT_PROFILE_NAME_LOG_OLD:L1 MUT_DECISION_ZERO_IS_MISMATCH:D1; do n=$${m%%:*}; pin=$${m##*:}; \
	  $(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -D$$n -o $@.$$n $(ROOT)/tests/unit/test_named_profile_extra.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp || exit 1; \
	  if $@.$$n > $@.$$n.log 2>&1; then echo "MUTANT SURVIVED: $$n"; exit 1; fi; \
	  grep -m1 '^FAIL ' $@.$$n.log | grep -q "FAIL $$pin " || { echo "MUTANT KILLED BY THE WRONG CHECK: $$n"; cat $@.$$n.log; exit 1; }; done
	@echo "test_named_profile_extra: mutants MUT_NAMED_EXTRA_DESTRUCTIVE MUT_PROFILE_NAME_LOG_OLD MUT_DECISION_ZERO_IS_MISMATCH FAIL at their pinned checks as required"
	@touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_named_profile_extra $(ROOT)/build/test_named_profile_extra.mutant-check
