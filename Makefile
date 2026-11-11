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

UNIT_PREREQS = $(ROOT)/build/test_gdm_filter $(ROOT)/build/test_cast_av_480p_policy $(ROOT)/build/test_p720_audio_keepup $(ROOT)/build/test_p720_transcode_vf $(ROOT)/build/test_transcode_offset $(ROOT)/build/test_spi_txn_complete $(ROOT)/build/test_idle_poll_budget $(ROOT)/build/test_p720_e2e_budget $(ROOT)/build/test_pl330_encode $(ROOT)/build/test_cadence $(ROOT)/build/test_pts_cadence $(ROOT)/build/test_avclock $(ROOT)/build/test_mraudio_status $(ROOT)/build/test_mraudio_oneshot $(ROOT)/build/test_audio_buf_prefill $(ROOT)/build/test_osd_menu $(ROOT)/build/test_plexob_yc $(ROOT)/build/test_playback_overlay $(ROOT)/build/test_overlay_onchange $(ROOT)/build/test_input_mailbox $(ROOT)/build/test_plx_page $(ROOT)/build/input_mailbox_probe_host $(ROOT)/build/test_pixel_format $(ROOT)/build/test_main_guard $(ROOT)/build/test_d52_log $(ROOT)/build/test_ddr_frame_wc $(ROOT)/build/test_core_gate $(ROOT)/build/test_fake_core_caps $(ROOT)/build/test_status_telemetry $(ROOT)/build/test_status_osd_v3 $(ROOT)/build/test_fpga_build_ids $(ROOT)/build/test_sps_relax_gate $(ROOT)/build/test_status_heartbeat $(ROOT)/build/test_wedge_read $(ROOT)/build/test_gate_caps $(ROOT)/build/test_gate_owner $(ROOT)/build/test_gate_error_render $(ROOT)/build/test_idle_pairing_error $(ROOT)/build/test_resolve $(ROOT)/build/test_pms_timeline $(ROOT)/build/test_companion_plant_seek $(ROOT)/build/test_companion_480i_field_rate $(ROOT)/build/test_pts_jump_clamp $(ROOT)/build/test_ttfp_stamps $(ROOT)/build/test_480i_active_height $(ROOT)/build/test_core_id_select $(ROOT)/build/test_sps_reject_high $(ROOT)/build/pms_baseline_probe $(ROOT)/build/test_h264_bitstream_source $(ROOT)/build/test_frame_store_math $(ROOT)/build/test_frame_store_sdram_sim $(ROOT)/build/test_frame_store_ddr_prefetch_sim $(ROOT)/build/test_sdram_memtest_sim $(ROOT)/build/test_sdram_mailbox $(ROOT)/build/test_annexb_count $(ROOT)/build/test_sps_parse $(ROOT)/build/test_slice_hdr $(ROOT)/build/test_cavlc_dc $(ROOT)/build/test_idct_quant $(ROOT)/build/test_p3_host_recon_vectors $(ROOT)/build/test_p3_idct_reference_model $(ROOT)/build/test_p3_inter_pred_vectors $(ROOT)/build/extract_h264_golden $(ROOT)/build/test_idle_fb_map $(ROOT)/build/test_idle_fb_map.mutant-check $(ROOT)/build/test_owenb_ring $(ROOT)/build/test_owenb_ring.mutant-check $(ROOT)/build/test_owenb_core_gate $(ROOT)/build/test_owenb_core_gate.mutant-check $(ROOT)/build/test_owenb_session $(ROOT)/build/test_owenb_session.mutant-check $(ROOT)/build/test_net_resume $(ROOT)/build/test_net_resume.mutant-check $(ROOT)/build/test_owenb_play $(ROOT)/build/test_owenb_timeline_heartbeat $(ROOT)/build/test_queue_preroll $(ROOT)/build/test_queue_preroll.mutant-check $(ROOT)/build/test_markers $(ROOT)/build/test_markers.mutant-check $(ROOT)/build/test_companion_queue_version $(ROOT)/build/test_owenb_weak_profile $(ROOT)/build/test_owenb_weak_profile.mutant-check $(ROOT)/build/test_allow_480p60 $(ROOT)/build/test_allow_480p60.mutant-check $(ROOT)/build/test_pause_dim $(ROOT)/build/test_pause_dim.mutant-check $(ROOT)/build/test_inplay_osd $(ROOT)/build/test_owenb_pms_traits $(ROOT)/build/test_owenb_pms_traits.mutant-check $(ROOT)/build/test_owenb_overlay $(ROOT)/build/test_owenb_aspect $(ROOT)/build/test_owenb_core_select $(ROOT)/build/test_owenb_osd $(ROOT)/build/test_owenb_osd.mutant-check $(ROOT)/build/test_owenb_osd.ovl-ref-check $(ROOT)/build/test_owenb_fbcon $(ROOT)/build/test_owenb_fbcon.mutant-check $(ROOT)/build/test_owenb_input $(ROOT)/build/test_owenb_input.mutant-check

unit-unlocked: preflight $(UNIT_PREREQS)
	python3 $(ROOT)/tests/unit/test_unit_host_coverage.py
	@python3 $(ROOT)/tests/unit/test_script_modes.py
	sh $(ROOT)/tests/unit/test_spelunkr_operator_switch.sh
	$(ROOT)/build/test_gdm_filter
	$(ROOT)/tests/unit/test_cast_http_ready_policy.sh
	python3 $(ROOT)/tests/unit/test_candidate_deploy.py
	$(ROOT)/tests/unit/test_deploy_misterplexd_guard.sh
	python3 $(ROOT)/tests/unit/test_arm_ffmpeg_static.py
	python3 $(ROOT)/tests/unit/test_redact_elf_build_paths.py
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
	$(ROOT)/build/test_mraudio_oneshot
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
	$(ROOT)/build/test_ob_glass
	python3 $(ROOT)/tests/unit/test_gcast_hls_level_mutant.py
	python3 $(ROOT)/tests/unit/test_gcast_videofallback_mutant.py
	python3 $(ROOT)/tests/unit/test_airplay_info_plist.py
	python3 $(ROOT)/tests/unit/test_arm_plexd_airplay_default.py
	python3 $(ROOT)/tests/unit/test_gcast_uxplay_ptth.py
	python3 $(ROOT)/tests/unit/test_gcast_uxplay_handoff.py
	$(ROOT)/build/test_resolve
	$(ROOT)/build/test_pms_timeline
	$(ROOT)/build/test_companion_plant_seek
	$(ROOT)/build/test_companion_480i_field_rate
	$(ROOT)/build/test_sps_reject_high
	$(ROOT)/build/test_companion_letterbox_bars
	$(ROOT)/build/test_companion_live_clock
	$(ROOT)/build/test_track_select
	$(if $(OWENB_AV_E2E_OK),$(ROOT)/build/test_track_select_av,@echo "SKIP test_track_select_av (no FFmpeg dev libs)")
	$(ROOT)/build/test_srt_cues
	$(ROOT)/build/test_ass_to_text
	$(ROOT)/build/test_subtitle_sink_osd
	$(ROOT)/build/test_caption_layout
	$(ROOT)/build/test_caption_clock
	$(ROOT)/build/test_plexui_subtitle_picker
	$(ROOT)/build/test_plexui_audio_picker
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
	$(ROOT)/build/test_no_synthetic_source $(ROOT)
	$(ROOT)/build/test_yt_av_source_policy
	$(ROOT)/build/test_plex_tv_link
	$(ROOT)/build/test_link_qr
	python3 $(ROOT)/tests/unit/test_link_discover.py
	bash $(ROOT)/tests/unit/test_respawn_resume_static.sh
	$(ROOT)/build/test_companion_natural_eof
	$(ROOT)/build/test_companion_reqlog
	$(ROOT)/build/test_input_close_eof
	$(ROOT)/build/test_http_range_resume
	$(ROOT)/build/test_fpga_av_trace
	$(ROOT)/build/test_fpga_av_trace_off
	$(ROOT)/tests/unit/test_pms_baseline_gate.sh
	bash $(ROOT)/tests/unit/test_pms_baseline_live_gate.sh
	bash $(ROOT)/tests/unit/test_pms_nal_stats_gate.sh
	$(ROOT)/build/test_h264_bitstream_source
	$(ROOT)/build/test_owenb_ring
	$(ROOT)/build/test_owenb_core_gate
	$(ROOT)/build/test_owenb_session
	$(ROOT)/build/test_net_resume
	$(ROOT)/build/test_owenb_play
	$(ROOT)/build/test_owenb_timeline_heartbeat
	$(ROOT)/build/test_queue_preroll
	$(ROOT)/build/test_markers
	$(ROOT)/build/test_companion_queue_version
	$(ROOT)/build/test_owenb_weak_profile
	$(ROOT)/build/test_allow_480p60
	$(ROOT)/build/test_pause_dim
	$(ROOT)/build/test_inplay_osd
	$(ROOT)/build/test_owenb_overlay
	$(ROOT)/build/test_owenb_aspect
	$(ROOT)/build/test_owenb_core_select
	$(ROOT)/build/test_owenb_osd
	$(ROOT)/build/test_owenb_osd_plexui
	$(ROOT)/build/test_owenb_browse_hint
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
	$(ROOT)/tests/unit/test_export_scrub_rules.sh
	$(ROOT)/tests/unit/test_export_public_ip_scan.sh
	$(ROOT)/tests/unit/test_export_lab_id_scan.sh
	$(ROOT)/tests/unit/test_export_vendor_source.sh
	$(ROOT)/tests/unit/test_report_names.sh
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
	sh $(ROOT)/tests/unit/test_spelunkr_operator_switch.sh
	$(ROOT)/build/test_gdm_filter
	$(ROOT)/tests/unit/test_cast_http_ready_policy.sh
	python3 $(ROOT)/tests/unit/test_candidate_deploy.py
	$(ROOT)/tests/unit/test_deploy_misterplexd_guard.sh
	python3 $(ROOT)/tests/unit/test_arm_ffmpeg_static.py
	python3 $(ROOT)/tests/unit/test_redact_elf_build_paths.py
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
	$(ROOT)/build/test_mraudio_oneshot
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
	$(ROOT)/build/test_ob_glass
	python3 $(ROOT)/tests/unit/test_gcast_hls_level_mutant.py
	python3 $(ROOT)/tests/unit/test_gcast_videofallback_mutant.py
	python3 $(ROOT)/tests/unit/test_airplay_info_plist.py
	python3 $(ROOT)/tests/unit/test_arm_plexd_airplay_default.py
	python3 $(ROOT)/tests/unit/test_gcast_uxplay_ptth.py
	python3 $(ROOT)/tests/unit/test_gcast_uxplay_handoff.py
	$(ROOT)/build/test_resolve
	$(ROOT)/build/test_pms_timeline
	$(ROOT)/build/test_companion_plant_seek
	$(ROOT)/build/test_companion_480i_field_rate
	$(ROOT)/build/test_sps_reject_high
	$(ROOT)/build/test_companion_letterbox_bars
	$(ROOT)/build/test_companion_live_clock
	$(ROOT)/build/test_track_select
	$(if $(OWENB_AV_E2E_OK),$(ROOT)/build/test_track_select_av,@echo "SKIP test_track_select_av (no FFmpeg dev libs)")
	$(ROOT)/build/test_srt_cues
	$(ROOT)/build/test_ass_to_text
	$(ROOT)/build/test_subtitle_sink_osd
	$(ROOT)/build/test_caption_layout
	$(ROOT)/build/test_caption_clock
	$(ROOT)/build/test_plexui_subtitle_picker
	$(ROOT)/build/test_plexui_audio_picker
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
	$(ROOT)/build/test_no_synthetic_source $(ROOT)
	$(ROOT)/build/test_yt_av_source_policy
	$(ROOT)/build/test_plex_tv_link
	$(ROOT)/build/test_link_qr
	python3 $(ROOT)/tests/unit/test_link_discover.py
	bash $(ROOT)/tests/unit/test_respawn_resume_static.sh
	$(ROOT)/build/test_companion_natural_eof
	$(ROOT)/build/test_companion_reqlog
	$(ROOT)/build/test_input_close_eof
	$(ROOT)/build/test_http_range_resume
	$(ROOT)/build/test_fpga_av_trace
	$(ROOT)/build/test_fpga_av_trace_off
	$(ROOT)/tests/unit/test_pms_baseline_gate.sh
	bash $(ROOT)/tests/unit/test_pms_baseline_live_gate.sh
	bash $(ROOT)/tests/unit/test_pms_nal_stats_gate.sh
	$(ROOT)/build/test_h264_bitstream_source
	$(ROOT)/build/test_owenb_ring
	$(ROOT)/build/test_owenb_core_gate
	$(ROOT)/build/test_owenb_session
	$(ROOT)/build/test_net_resume
	$(ROOT)/build/test_owenb_play
	$(ROOT)/build/test_owenb_timeline_heartbeat
	$(ROOT)/build/test_queue_preroll
	$(ROOT)/build/test_markers
	$(ROOT)/build/test_companion_queue_version
	$(ROOT)/build/test_owenb_weak_profile
	$(ROOT)/build/test_allow_480p60
	$(ROOT)/build/test_pause_dim
	$(ROOT)/build/test_inplay_osd
	$(ROOT)/build/test_owenb_overlay
	$(ROOT)/build/test_owenb_aspect
	$(ROOT)/build/test_owenb_core_select
	$(ROOT)/build/test_owenb_osd
	$(ROOT)/build/test_owenb_osd_plexui
	$(ROOT)/build/test_owenb_browse_hint
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
	$(ROOT)/tests/unit/test_export_scrub_rules.sh
	$(ROOT)/tests/unit/test_export_public_ip_scan.sh
	$(ROOT)/tests/unit/test_export_lab_id_scan.sh
	$(ROOT)/tests/unit/test_export_vendor_source.sh
	$(ROOT)/tests/unit/test_report_names.sh
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

# FPGA Bug 19:27 F5 on RG-2 (w9): the planner over the real PLXH stack must not paint until 2
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
		$(ROOT)/host/libmisterplex/browse_nav.hpp \
		$(ROOT)/host/libmisterplex/browse_v2.hpp $(ROOT)/host/libmisterplex/library_browser.hpp \
		$(ROOT)/arm/misterplexd/font_8x16.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -o $@ $(ROOT)/tests/unit/test_browse_r1_adapter.cpp

$(ROOT)/build/test_browse_r1_adapter.mutant-check: $(ROOT)/build/test_browse_r1_adapter
	@$(ROOT)/build/test_browse_r1_adapter > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -DMUT_RC1037_NO_HINT \
		-o $@.nohint $(ROOT)/tests/unit/test_browse_r1_adapter.cpp
	@if $@.nohint > $@.nohint.log 2>&1; then echo "MUTANT SURVIVED: MUT_RC1037_NO_HINT"; exit 1; fi
	@echo "test_browse_r1_adapter: MUT_RC1037_NO_HINT FAILS as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_browse_r1_adapter.mutant-check

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
$(ROOT)/build/test_ob_glass: $(ROOT)/tests/unit/test_ob_glass.cpp \
		$(ROOT)/arm/misterplexd/gcast/cast_resolve.hpp \
		$(ROOT)/arm/misterplexd/gcast/protected_src.hpp \
		$(ROOT)/host/libmisterplex/ob_glass.hpp \
		$(ROOT)/host/libmisterplex/owenb_play.cpp \
		$(ROOT)/arm/misterplexd/media_player.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -o $@ $(ROOT)/tests/unit/test_ob_glass.cpp

unit-unlocked unit-host-unlocked: $(ROOT)/build/test_ob_glass

# GH#8: AirPlay stays advertised when /probe 404s (health or loopback helper).
$(ROOT)/build/test_gcast_core: $(ROOT)/tests/unit/test_gcast_core.cpp \
		$(ROOT)/arm/misterplexd/gcast/cast_resolve.hpp $(ROOT)/arm/misterplexd/main.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -o $@ \
		$(ROOT)/tests/unit/test_gcast_core.cpp
$(ROOT)/build/test_gcast_core.ok: $(ROOT)/build/test_gcast_core
	cd $(ROOT) && $(ROOT)/build/test_gcast_core
	touch $@
$(ROOT)/build/test_gcast_core.mutant-check: $(ROOT)/build/test_gcast_core
	@cd $(ROOT) && $(ROOT)/build/test_gcast_core > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -DMUT_GH8_PROBE_HIDES \
		-o $(ROOT)/build/test_gcast_core.mut $(ROOT)/tests/unit/test_gcast_core.cpp
	@if cd $(ROOT) && $(ROOT)/build/test_gcast_core.mut > $@.mut.log 2>&1; then \
		echo "test_gcast_core: MUT_GH8_PROBE_HIDES SURVIVED"; exit 1; fi
	@$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -DMUT_YT_LOOPBACK_AS_REMOTE \
		-o $(ROOT)/build/test_gcast_core.ytlb $(ROOT)/tests/unit/test_gcast_core.cpp
	@if cd $(ROOT) && $(ROOT)/build/test_gcast_core.ytlb > $@.ytlb.log 2>&1; then \
		echo "test_gcast_core: MUT_YT_LOOPBACK_AS_REMOTE SURVIVED"; exit 1; fi
	@echo "test_gcast_core: MUT_GH8_PROBE_HIDES MUT_YT_LOOPBACK_AS_REMOTE FAIL as required" | tee $@
	@rm -f $(ROOT)/build/test_gcast_core.mut $(ROOT)/build/test_gcast_core.ytlb
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_gcast_core.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_gcast_core.mutant-check

.PHONY: gcast-tests airplay-tests
gcast-tests airplay-tests:
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

# FPGA Bug 19:24 L2: a transcode sends its stream DAR (source_aspect.hpp firstAuSourceAspect).
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
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui -I$(ROOT)/arm/misterplexd -o $@ \
		$(ROOT)/tests/unit/test_plexui_imgcache.cpp \
		$(PLEXUI_IMGCACHE_TEST_EXTRA) \
		$(PLEXUI_IMG_CACHE_SRC)

$(ROOT)/build/test_plexui_imgcache.check: $(ROOT)/build/test_plexui_imgcache
	$(ROOT)/build/test_plexui_imgcache
	@touch $@
$(ROOT)/build/test_plexui_imgcache.mutant-check: $(ROOT)/build/test_plexui_imgcache.check
	@$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -DMUT_PLEXUI_RAM_96MB -o $(ROOT)/build/test_plexui_imgcache.mut \
		$(ROOT)/tests/unit/test_plexui_imgcache.cpp $(PLEXUI_IMGCACHE_TEST_EXTRA) $(PLEXUI_IMG_CACHE_SRC)
	@if $(ROOT)/build/test_plexui_imgcache.mut > $@.mut.log 2>&1; then \
		echo "test_plexui_imgcache: MUT_PLEXUI_RAM_96MB SURVIVED"; exit 1; fi
	@echo "test_plexui_imgcache: MUT_PLEXUI_RAM_96MB FAIL as required" | tee $@
	@rm -f $(ROOT)/build/test_plexui_imgcache.mut
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plexui_imgcache.mutant-check

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
	$(CXX) $(PLEXUI_ASAN_CXXFLAGS) -pthread -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui -I$(ROOT)/arm/misterplexd -o $@ \
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
	$(CXX) $(PLEXUI_ASAN_CXXFLAGS) -pthread -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui -I$(ROOT)/arm/misterplexd -o $@ \
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
	$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui -I$(ROOT)/arm/misterplexd \
		-o $@ $(PLEXUI_MEM_BUDGET_SRC)

$(ROOT)/build/test_plexui_mem_budget.check: $(ROOT)/build/test_plexui_mem_budget
	$(ROOT)/build/test_plexui_mem_budget
	@touch $@

$(ROOT)/build/test_plexui_mem_budget_asan: $(PLEXUI_MEM_BUDGET_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(PLEXUI_ASAN_CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host \
		-I$(ROOT)/host/libmisterplex/plexui -I$(ROOT)/arm/misterplexd -o $@ $(PLEXUI_MEM_BUDGET_SRC)

$(ROOT)/build/test_plexui_mem_budget_asan.check: $(ROOT)/build/test_plexui_mem_budget_asan
	ASAN_OPTIONS=detect_leaks=1 $(ROOT)/build/test_plexui_mem_budget_asan
	@touch $@

PLEXUI_MEM_BUDGET_MUTANTS := no_trim:MUT_PLEXUI_NO_TRIM_ON_LOWER pins_over:MUT_PLEXUI_PINS_OVER_BUDGET \
		evict_pins_inflight:MUT_PLEXUI_EVICT_PINS_FOR_INFLIGHT short_srt:MUT_254_SHORT_SRT_FIXTURE
PLEXUI_MEM_BUDGET_MUT_BINS := $(foreach m,$(PLEXUI_MEM_BUDGET_MUTANTS),$(ROOT)/build/test_plexui_mem_budget_mut_$(word 1,$(subst :, ,$(m))))

$(ROOT)/build/test_plexui_mem_budget_mut_%: $(PLEXUI_MEM_BUDGET_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui -I$(ROOT)/arm/misterplexd \
		-D$(word 2,$(subst :, ,$(filter $*:%,$(PLEXUI_MEM_BUDGET_MUTANTS)))) -o $@ $(PLEXUI_MEM_BUDGET_SRC)

$(ROOT)/build/test_plexui_mem_budget.mutant-check: $(ROOT)/build/test_plexui_mem_budget $(PLEXUI_MEM_BUDGET_MUT_BINS)
	@$(ROOT)/build/test_plexui_mem_budget > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in $(foreach x,$(PLEXUI_MEM_BUDGET_MUTANTS),$(word 1,$(subst :, ,$(x)))); do \
		if $(ROOT)/build/test_plexui_mem_budget_mut_$$m > $@.log 2>&1; then echo "MUTANT SURVIVED: $$m"; exit 1; fi; done
	@echo "test_plexui_mem_budget: $(words $(PLEXUI_MEM_BUDGET_MUTANTS)) mutants FAIL as required" | tee $@

unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plexui_mem_budget.check
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plexui_mem_budget_asan.check
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plexui_mem_budget.mutant-check

# The MPX_H264_FILTER start-up line (FPGA Bug 17:17 (4)): its format and that it matches the daemon's readers.
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
# run ONLY through ~/bin/heavy-sim-slot in a private PID + net namespace (crew rule B), never
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

$(ROOT)/build/test_ddr_frame_geometry_coded_source: $(ROOT)/tests/unit/test_ddr_frame_geometry_coded_source.cpp \
		$(ROOT)/host/libmisterplex/ddr_frame_layout.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_ddr_frame_geometry_coded_source.cpp

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

# T-DAEMON-MVPS (FPGA Bug 18:34): the core's decode_error named on the AU push-reject session end.
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

# FPGA Bug 07:30 C2 + 08:17 (a) + 08:18: one mapper and one lock for the doorbell (PLX) page, on the
# real fpga_spi.cpp (sparse fake /dev/mem; test seams widen a first map and an access by 20 ms).
# Rows P-a..P-h; each mutant (no lock; the frame window maps the page too; close() skips the page-lock
# wait) must FAIL. T-PAGEMAP-CLOSE-WAIT (FPGA Bug 17:06): the forced-schedule rows P-g/P-h (seam
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
		$(ROOT)/host/libmisterplex/playback_overlay.hpp \
		$(ROOT)/host/libmisterplex/ui_safe_layout.hpp
OWENB_PLAY_SRCS := $(ROOT)/host/libmisterplex/owenb_ring.cpp $(ROOT)/host/libmisterplex/owenb_session.cpp \
		$(ROOT)/host/libmisterplex/owenb_play.cpp $(ROOT)/host/libmisterplex/theme_decode.cpp
OWENB_PLAY_HDRS := $(ROOT)/host/libmisterplex/owenb_ring.hpp $(ROOT)/host/libmisterplex/owenb_session.hpp \
		$(ROOT)/host/libmisterplex/owenb_prefetch.hpp \
		$(ROOT)/host/libmisterplex/remote_h264_transcode.hpp \
		$(ROOT)/host/libmisterplex/owenb_play.hpp $(ROOT)/host/libmisterplex/net_resume.hpp \
		$(ROOT)/host/libmisterplex/owenb_ddr_window.hpp \
		$(ROOT)/host/libmisterplex/h264_bitstream_transport.hpp $(OWENB_OSD_HDRS) $(OWENB_RING_HDRS)
OWENB_SESS_DEPS := $(ROOT)/tests/unit/test_owenb_session.cpp $(ROOT)/tests/unit/fake_owenb_core.hpp \
		$(OWENB_PLAY_SRCS) $(OWENB_PLAY_HDRS) $(OWENB_RING_OBJS) \
		$(ROOT)/tests/fixtures/owenb_t5/cabac_160x96_2s.264 $(ROOT)/tests/fixtures/owenb_t5/cavlc_320x240_2s.264
OWENB_SESS_FLAGS := -I$(OWENB_RING_DIR)/include -I$(ROOT)/arm/misterplexd -pthread -DMPX_SOURCE_ROOT='"$(ROOT)"'
OWENB_SESS_MUTANTS := no_eos:MUT_OWENB_SESS_NO_EOS seek_no_flush:MUT_OWENB_SESS_SEEK_NO_FLUSH \
		pause_reads:MUT_OWENB_SESS_PAUSE_READS no_cavlc_guard:MUT_OWENB_SESS_NO_CAVLC_GUARD \
		pcm_no_pts:MUT_OWENB_SESS_PCM_NO_PTS cavlc_rate_only:MUT_OWENB_SESS_CAVLC_RATE_ONLY \
		no_hard_cap:MUT_OWENB_SESS_NO_HARD_CAP no_entropy_report:MUT_OWENB_SESS_NO_ENTROPY_REPORT \
		unbounded_out:MUT_OWENB_SESS_UNBOUNDED_OUT ignore_sendready:MUT_OWENB_SESS_IGNORE_SENDREADY \
		anchor_open:MUT_OWENB_SESS_ANCHOR_OPEN \
		drain_fixed:MUT_OWENB_DRAIN_FIXED drain_no_stall:MUT_OWENB_DRAIN_NO_STALL drain_no_cap:MUT_OWENB_DRAIN_NO_CAP \
		anchor_no_drop:MUT_OWENB_SESS_ANCHOR_NO_DROP anchor_no_cap:MUT_OWENB_SESS_ANCHOR_NO_CAP \
		reckon_pause:MUT_OWENB_SESS_RECKON_PAUSE reckon_hold:MUT_OWENB_SESS_RECKON_HOLD \
		rungrow:MUT_OWENB_SESS_ANCHOR_RUNGROW \
		halfspeed:MUT_OWENB_SESS_ANCHOR_HALFSPEED \
		nostorepause:MUT_OWENB_SESS_ANCHOR_NOSTOREPAUSE \
		premature_off:MUT_OWENB_PREMATURE_OFF premature_no_cap:MUT_OWENB_PREMATURE_NO_CAP \
		premature_no_near_end:MUT_OWENB_PREMATURE_NO_NEAR_END \
		err_no_resume:MUT_OWENB_ERR_NO_RESUME err_no_backoff:MUT_OWENB_ERR_NO_BACKOFF \
		err_nearend_off:MUT_OWENB_ERR_NEAREND_OFF err_nearend_any:MUT_OWENB_ERR_NEAREND_ANY
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

# tipA-net-drop-resume: classify / backoff / resume / give-up / flap, and the session
# gate. NET_RESUME stays off in the product. Mutants are not in OWENB_SESS_MUTANTS:
# ignore-off resumes a flag-off play, fatal-ok retries a 401, no-giveup never stops.
NET_RESUME_DEPS := $(ROOT)/tests/unit/test_net_resume.cpp $(ROOT)/tests/unit/fake_owenb_core.hpp \
		$(OWENB_PLAY_SRCS) $(OWENB_PLAY_HDRS) $(OWENB_RING_OBJS) \
		$(ROOT)/tests/fixtures/owenb_t5/cabac_160x96_2s.264
NET_RESUME_MUTANTS := ignore_off:MUT_NET_RESUME_IGNORE_OFF fatal_ok:MUT_NET_RESUME_FATAL_OK \
		no_giveup:MUT_NET_RESUME_NO_GIVEUP
NET_RESUME_MUT_BINS := $(foreach m,$(NET_RESUME_MUTANTS),$(ROOT)/build/test_net_resume_mut_$(word 1,$(subst :, ,$(m))))

$(ROOT)/build/test_net_resume: $(NET_RESUME_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -I$(ROOT)/arm/misterplexd -o $@ $(ROOT)/tests/unit/test_net_resume.cpp $(OWENB_PLAY_SRCS) \
		$(OWENB_RING_OBJS) -lm

$(ROOT)/build/test_net_resume_mut_%: $(NET_RESUME_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -I$(ROOT)/arm/misterplexd \
		-D$(word 2,$(subst :, ,$(filter $*:%,$(NET_RESUME_MUTANTS)))) -o $@ \
		$(ROOT)/tests/unit/test_net_resume.cpp $(OWENB_PLAY_SRCS) $(OWENB_RING_OBJS) -lm

$(ROOT)/build/test_net_resume.mutant-check: $(ROOT)/build/test_net_resume $(NET_RESUME_MUT_BINS)
	@$(ROOT)/build/test_net_resume > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in $(foreach x,$(NET_RESUME_MUTANTS),$(word 1,$(subst :, ,$(x)))); do \
		if timeout 120 $(ROOT)/build/test_net_resume_mut_$$m > $@.log 2>&1; then echo "MUTANT SURVIVED: $$m"; exit 1; fi; done
	@echo "test_net_resume: $(words $(NET_RESUME_MUTANTS)) mutants FAIL as required" | tee $@

$(ROOT)/build/test_owenb_play: $(ROOT)/tests/unit/test_owenb_play.cpp $(ROOT)/tests/unit/fake_owenb_core.hpp \
		$(OWENB_PLAY_SRCS) $(OWENB_PLAY_HDRS) $(OWENB_RING_OBJS) $(ROOT)/arm/misterplexd/media_player.cpp \
		$(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/main.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/arm/misterplexd/playback_controls.hpp $(ROOT)/arm/misterplexd/owenb_av_source.cpp \
		$(ROOT)/arm/misterplexd/spelunkr/fe.hpp $(ROOT)/arm/misterplexd/spelunkr/schedule.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -o $@ $(ROOT)/tests/unit/test_owenb_play.cpp $(OWENB_PLAY_SRCS) \
		$(OWENB_RING_OBJS) -lm

$(ROOT)/build/test_owenb_play_mut_timeout: $(ROOT)/build/test_owenb_play
	$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -DMUT_OWENB_PREFETCH_HARD_TIMEOUT -o $@ \
		$(ROOT)/tests/unit/test_owenb_play.cpp $(OWENB_PLAY_SRCS) $(OWENB_RING_OBJS) -lm

$(ROOT)/build/test_owenb_play.timeout-mutant-check: $(ROOT)/build/test_owenb_play_mut_timeout
	@$(ROOT)/build/test_owenb_play --mbb-only > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@if $(ROOT)/build/test_owenb_play_mut_timeout --mbb-only > $@.mut.log 2>&1; then \
		echo "MUTANT SURVIVED: MUT_OWENB_PREFETCH_HARD_TIMEOUT"; exit 1; fi
	@grep -q 'FAIL .*MBB slow IDR beyond 5 s' $@.mut.log
	@grep -q 'FAIL .*timeout requests normal start, not hard failure' $@.mut.log
	@echo "test_owenb_play: 5 s hard timeout without fallback FAILs as required" | tee $@

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

# SEC-M1 / #253: plexHttpGet URL is one argv item; secret headers go via curl -K stdin (never argv).
# Mutants MUT_PLEXGET_NO_DASHDASH / HDR_UNCHECKED / TOKEN_REDIRECT / MUT_HDR_ON_ARGV must FAIL.
PLEXHTTPGET_INJ_SRC := $(ROOT)/tests/unit/test_plexhttpget_injection.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp
PLEXHTTPGET_INJ_CXXFLAGS := -std=c++17 -O1 -Wall -Wextra -I$(ROOT)/host -I$(ROOT)/arm/misterplexd -pthread
$(ROOT)/build/test_plexhttpget_injection: $(PLEXHTTPGET_INJ_SRC) $(ROOT)/arm/misterplexd/plex_resolve.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(PLEXHTTPGET_INJ_CXXFLAGS) -o $@ $(PLEXHTTPGET_INJ_SRC)
$(ROOT)/build/test_plexhttpget_injection_asan: $(PLEXHTTPGET_INJ_SRC) $(ROOT)/arm/misterplexd/plex_resolve.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(PLEXHTTPGET_INJ_CXXFLAGS) -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -o $@ \
		$(PLEXHTTPGET_INJ_SRC)
$(ROOT)/build/test_plexhttpget_injection.mutant-check: $(ROOT)/build/test_plexhttpget_injection \
		$(PLEXHTTPGET_INJ_SRC) $(ROOT)/arm/misterplexd/plex_resolve.hpp
	@mkdir -p $(ROOT)/build
	@$(ROOT)/build/test_plexhttpget_injection > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for M in MUT_PLEXGET_NO_DASHDASH MUT_PLEXGET_HDR_UNCHECKED MUT_PLEXGET_TOKEN_REDIRECT MUT_341_QUERY_HDR_FU10 MUT_341_PROBE_ONCE MUT_HDR_ON_ARGV MUT_FU8_TOKEN_ARGV MUT_FU9_FD_LEAK MUT_FU9_SCRUB_ANY_ARGV MUT_FU10_NO_FOLLOW MUT_PLEX_TV_NO_CLIENT_ID MUT_LINKED_RESOURCES_SHARED_CLIENT MUT_PLEX_TV_HOST_SUBSTRING MUT_PLEX_VERSION_NOT_CONSTANT MUT_KCONFIG_ON_STDIN; do \
		$(CXX) $(PLEXHTTPGET_INJ_CXXFLAGS) -D$$M -o $(ROOT)/build/test_plexhttpget_injection.$$M \
			$(PLEXHTTPGET_INJ_SRC) || { echo "MUTANT DID NOT COMPILE: $$M"; exit 1; }; \
		if $(ROOT)/build/test_plexhttpget_injection.$$M > $(ROOT)/build/test_plexhttpget_injection.$$M.log 2>&1; then \
			echo "MUTANT SURVIVED: $$M"; exit 1; fi; \
	done
	@echo "test_plexhttpget_injection: mutants NO_DASHDASH, HDR_UNCHECKED, TOKEN_REDIRECT, 341_QUERY_HDR_FU10, 341_PROBE_ONCE, HDR_ON_ARGV, FU8_TOKEN_ARGV, FU9_FD_LEAK, FU9_SCRUB_ANY_ARGV, FU10_NO_FOLLOW, PLEX_TV_NO_CLIENT_ID, LINKED_RESOURCES_SHARED_CLIENT, PLEX_TV_HOST_SUBSTRING, PLEX_VERSION_NOT_CONSTANT, KCONFIG_ON_STDIN FAIL as required" | tee $@
$(ROOT)/build/test_plexhttpget_injection.ok: \
		$(ROOT)/build/test_plexhttpget_injection $(ROOT)/build/test_plexhttpget_injection_asan \
		$(ROOT)/build/test_plexhttpget_injection.mutant-check \
		$(ROOT)/tests/unit/test_plexhttpget_injection.sh
	@mkdir -p $(ROOT)/build
	$(ROOT)/build/test_plexhttpget_injection
	ASAN_OPTIONS=detect_leaks=1 $(ROOT)/build/test_plexhttpget_injection_asan
	ROOT=$(ROOT) SAN=1 bash $(ROOT)/tests/unit/test_plexhttpget_injection.sh
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plexhttpget_injection.ok

# D2: no shell on the plex_resolve paths (argv + posix_spawnp); mutants MUT_PLEXGET_NO_DASHDASH / TOKEN_REDIRECT / MUT_PREWARM_FGETS must FAIL.
$(ROOT)/build/test_plex_shell_paths_removed.ok: \
		$(ROOT)/tests/unit/test_plex_shell_paths_removed.sh $(ROOT)/tests/unit/test_plex_shell_paths_removed.cpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/arm/misterplexd/plex_resolve.hpp
	@mkdir -p $(ROOT)/build
	ROOT=$(ROOT) SAN=1 bash $(ROOT)/tests/unit/test_plex_shell_paths_removed.sh
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plex_shell_paths_removed.ok

# D2: every browse/plexui curl gets -q first and "--" before the URL; the 4 MUT_*_NO_Q / NO_DASHDASH mutants must FAIL.
$(ROOT)/build/test_curl_dash_q.ok: \
		$(ROOT)/tests/unit/test_curl_dash_q.sh $(ROOT)/tests/unit/test_curl_dash_q.cpp \
		$(ROOT)/host/libmisterplex/browse_fetch.hpp $(ROOT)/host/libmisterplex/plexui/plexui_http.cpp \
		$(ROOT)/host/libmisterplex/plexui/plexui_http.hpp
	@mkdir -p $(ROOT)/build
	ROOT=$(ROOT) SAN=1 bash $(ROOT)/tests/unit/test_curl_dash_q.sh
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_curl_dash_q.ok

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

# Skip Intro / Up Next. Each MUT_MARKERS_* rebuild must FAIL.
$(ROOT)/build/test_markers: $(ROOT)/tests/unit/test_markers.cpp \
		$(ROOT)/host/libmisterplex/plex_markers.hpp $(ROOT)/host/libmisterplex/marker_tracker.hpp \
		$(ROOT)/host/libmisterplex/marker_flags.hpp $(ROOT)/host/libmisterplex/owenb_osd_draw.hpp \
		$(ROOT)/arm/misterplexd/next_item.hpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/arm/misterplexd/pms_timeline.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -I$(ROOT)/arm/misterplexd -o $@ \
		$(ROOT)/tests/unit/test_markers.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/arm/misterplexd/pms_timeline.cpp -pthread

$(ROOT)/build/test_markers.mutant-check: $(ROOT)/build/test_markers
	@$(ROOT)/build/test_markers > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@set -e; for m in MUT_MARKERS_FLAG_IGNORED MUT_MARKERS_NO_MARKER_SHOWS MUT_MARKERS_INTRO_WINDOW MUT_MARKERS_UPNEXT_CANCEL_IGNORED; do \
		$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -D$$m -I$(ROOT)/arm/misterplexd -o $(ROOT)/build/test_markers.$$m \
			$(ROOT)/tests/unit/test_markers.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
			$(ROOT)/arm/misterplexd/pms_timeline.cpp -pthread; \
		if $(ROOT)/build/test_markers.$$m >/dev/null 2>&1; then echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
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
		f3_card_see_through:MUT_F3_CARD_SEE_THROUGH f5_card_centred:MUT_F5_CARD_CENTRED \
		rearm_req_ignored:MUT_IDLE_REARM_REQ_IGNORED \
		pending_keeps_card:MUT_PENDING_KEEPS_CARD \
		pending_no_label:MUT_PENDING_NO_LABEL
OWENB_OSD_MUT_BINS := $(foreach m,$(OWENB_OSD_MUTANTS),$(ROOT)/build/test_owenb_osd_mut_$(word 1,$(subst :, ,$(m))))
OVL_REF_TOOLS ?= $(ROOT)/../umd-review/src-h264dec/tools

$(ROOT)/build/test_owenb_osd: $(OWENB_OSD_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -I$(ROOT)/arm/misterplexd -o $@ $(ROOT)/tests/unit/test_owenb_osd.cpp $(OWENB_PLAY_SRCS) \
		$(OWENB_RING_OBJS) -lm

# Colorbar removal 10-23: ErrorScreen::DaemonDown + the idle-card painter's plumbing (+ ASan variant).
DAEMON_DOWN_DEPS := $(ROOT)/tests/unit/test_daemon_down_card.cpp $(ROOT)/host/libmisterplex/owenb_osd_draw.hpp \
	$(ROOT)/host/libmisterplex/owenb_idle_card_ctl.hpp $(ROOT)/host/libmisterplex/idle_pairing_error.hpp \
	$(ROOT)/tools/owenb_idle_card.cpp $(ROOT)/scripts/misterplexd-boot-rc3.sh $(ROOT)/scripts/misterplexd_supervise.sh \
	$(ROOT)/arm/misterplexd/main.cpp $(ROOT)/scripts/package_release.sh
$(ROOT)/build/test_daemon_down_card: $(DAEMON_DOWN_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -DMPX_SOURCE_ROOT='"$(ROOT)"' -I$(ROOT)/host -I$(OWENB_RING_DIR)/include -o $@ \
		$(ROOT)/tests/unit/test_daemon_down_card.cpp
$(ROOT)/build/test_daemon_down_card_asan: $(DAEMON_DOWN_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(PLEXUI_ASAN_CXXFLAGS) -pthread -DMPX_SOURCE_ROOT='"$(ROOT)"' -I$(ROOT)/host -I$(OWENB_RING_DIR)/include -o $@ \
		$(ROOT)/tests/unit/test_daemon_down_card.cpp
$(ROOT)/build/test_daemon_down_card_asan.check: $(ROOT)/build/test_daemon_down_card_asan
	ASAN_OPTIONS=detect_leaks=1 $<
	@touch $@
# The painter itself (host build: proves it compiles and links against the ring/session/overlay objects).
$(ROOT)/build/owenb_idle_card: $(ROOT)/tools/owenb_idle_card.cpp $(DAEMON_DOWN_DEPS) $(OWENB_PLAY_SRCS) $(OWENB_RING_OBJS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -DMPX_HAVE_OWENB_RING -I$(ROOT)/host -o $@ $(ROOT)/tools/owenb_idle_card.cpp \
		$(OWENB_PLAY_SRCS) $(OWENB_RING_OBJS) -lm
$(ROOT)/build/test_daemon_down_card.ok: $(ROOT)/build/test_daemon_down_card
	$<
	@touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_daemon_down_card.ok $(ROOT)/build/test_daemon_down_card_asan.check $(ROOT)/build/owenb_idle_card
# PR #30 follow-up 10-23: the card reaches the screen (render + the painter's arming on the fake core + the bug) and its mutant.
CARD_PAINTER_DEPS := $(ROOT)/tests/unit/test_idle_card_painter.cpp $(ROOT)/tests/unit/fake_owenb_core.hpp \
	$(ROOT)/host/libmisterplex/owenb_idle_card_arm.hpp $(ROOT)/host/libmisterplex/owenb_idle_card_fb.hpp \
	$(ROOT)/tools/owenb_idle_card.cpp $(OWENB_PLAY_SRCS) $(OWENB_PLAY_HDRS) $(OWENB_RING_OBJS)
$(ROOT)/build/test_idle_card_painter: $(CARD_PAINTER_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -I$(ROOT)/tests/unit -o $@ $(ROOT)/tests/unit/test_idle_card_painter.cpp \
		$(OWENB_PLAY_SRCS) $(OWENB_RING_OBJS) -lm
$(ROOT)/build/test_idle_card_painter_asan: $(CARD_PAINTER_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(PLEXUI_ASAN_CXXFLAGS) $(OWENB_SESS_FLAGS) -I$(ROOT)/tests/unit -o $@ $(ROOT)/tests/unit/test_idle_card_painter.cpp \
		$(OWENB_PLAY_SRCS) $(OWENB_RING_OBJS) -lm
$(ROOT)/build/test_idle_card_painter_mut_no_osd_check: $(CARD_PAINTER_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -DMUT_CARD_NO_OSD_CHECK -I$(ROOT)/tests/unit -o $@ \
		$(ROOT)/tests/unit/test_idle_card_painter.cpp $(OWENB_PLAY_SRCS) $(OWENB_RING_OBJS) -lm
$(ROOT)/build/test_idle_card_painter.check: $(ROOT)/build/test_idle_card_painter $(ROOT)/build/test_idle_card_painter_asan $(ROOT)/build/test_idle_card_painter_mut_no_osd_check
	$(ROOT)/build/test_idle_card_painter
	ASAN_OPTIONS=detect_leaks=1 $(ROOT)/build/test_idle_card_painter_asan
	@if $(ROOT)/build/test_idle_card_painter_mut_no_osd_check >/dev/null 2>&1; then \
		echo "MUTANT no_osd_check SURVIVED"; exit 1; fi
	@echo "test_idle_card_painter: mutant no_osd_check FAILS as required"
	@touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_idle_card_painter.check

# PR #30 follow-up 10-23 (glass ovn-2): the daemon-down painter yields at the ring open, not in main() (no ~11 s colorbars).
BOOT_HANDOVER_DEPS := $(ROOT)/tests/unit/test_boot_handover.cpp $(ROOT)/tests/unit/fake_owenb_core.hpp $(OWENB_PLAY_SRCS) \
	$(OWENB_PLAY_HDRS) $(ROOT)/host/libmisterplex/owenb_play.cpp $(ROOT)/arm/misterplexd/main.cpp \
	$(ROOT)/arm/misterplexd/media_player.cpp
$(ROOT)/build/test_boot_handover: $(BOOT_HANDOVER_DEPS) $(OWENB_RING_OBJS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -I$(ROOT)/tests/unit -o $@ $(ROOT)/tests/unit/test_boot_handover.cpp \
		$(OWENB_PLAY_SRCS) $(OWENB_RING_OBJS) -lm
$(ROOT)/build/test_boot_handover_asan: $(BOOT_HANDOVER_DEPS) $(OWENB_RING_OBJS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(PLEXUI_ASAN_CXXFLAGS) $(OWENB_SESS_FLAGS) -I$(ROOT)/tests/unit -o $@ $(ROOT)/tests/unit/test_boot_handover.cpp \
		$(OWENB_PLAY_SRCS) $(OWENB_RING_OBJS) -lm
$(ROOT)/build/test_boot_handover.check: $(ROOT)/build/test_boot_handover $(ROOT)/build/test_boot_handover_asan
	$(ROOT)/build/test_boot_handover
	ASAN_OPTIONS=detect_leaks=1 $(ROOT)/build/test_boot_handover_asan
	@touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_boot_handover.check

# OB port: the PlexUI full-screen surface in the OB overlay renderer (+ a mutant that must FAIL).
$(ROOT)/build/test_owenb_osd_plexui: $(OWENB_OSD_DEPS) $(ROOT)/tests/unit/test_owenb_osd_plexui.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -o $@ $(ROOT)/tests/unit/test_owenb_osd_plexui.cpp $(OWENB_RING_OBJS) -lm

.PHONY: test-owenb-osd-plexui
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_owenb_osd_plexui

test-owenb-osd-plexui: $(ROOT)/build/test_owenb_osd_plexui
	$(ROOT)/build/test_owenb_osd_plexui
	$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -DMUT_OSD_SURF_ONE_SHADOW -o $(ROOT)/build/test_owenb_osd_plexui_mut \
		$(ROOT)/tests/unit/test_owenb_osd_plexui.cpp $(OWENB_RING_OBJS) -lm
	! $(ROOT)/build/test_owenb_osd_plexui_mut > /dev/null && echo "test_owenb_osd_plexui: mutant FAILS as required"
	$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -DMUT_OSD_SURF_FLIP_IDLE -o $(ROOT)/build/test_owenb_osd_plexui_mut_flip \
		$(ROOT)/tests/unit/test_owenb_osd_plexui.cpp $(OWENB_RING_OBJS) -lm
	! $(ROOT)/build/test_owenb_osd_plexui_mut_flip > /dev/null && echo "test_owenb_osd_plexui: FLIP_IDLE mutant FAILS as required"

# Idle ready card: no browse-key row, uncut, inside the 5% frame at 480p / 480i / 240p.
# Each mutant must FAIL; -DBH_OLD_HINT (the pre-fix one-line hint) must FAIL too.
BH_MUTANTS := no_wrap:MUT_BH_NO_WRAP one_row:MUT_BH_PAINT_ONE_ROW \
		card_no_grow:MUT_BH_CARD_NO_GROW old_hint:BH_OLD_HINT
BH_MUT_BINS := $(foreach m,$(BH_MUTANTS),$(ROOT)/build/test_owenb_browse_hint_mut_$(word 1,$(subst :, ,$(m))))
BH_DEPS := $(ROOT)/tests/unit/test_owenb_browse_hint.cpp $(ROOT)/host/libmisterplex/owenb_menu.hpp $(OWENB_OSD_HDRS) \
		$(ROOT)/arm/misterplexd/media_player.cpp $(OWENB_RING_OBJS)
$(ROOT)/build/test_owenb_browse_hint: $(BH_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -o $@ $(ROOT)/tests/unit/test_owenb_browse_hint.cpp $(OWENB_RING_OBJS) -lm

$(ROOT)/build/test_owenb_browse_hint_mut_%: $(BH_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -D$(word 2,$(subst :, ,$(filter $*:%,$(BH_MUTANTS)))) \
		-o $@ $(ROOT)/tests/unit/test_owenb_browse_hint.cpp $(OWENB_RING_OBJS) -lm

$(ROOT)/build/test_owenb_browse_hint.mutant-check: $(ROOT)/build/test_owenb_browse_hint $(BH_MUT_BINS)
	@$(ROOT)/build/test_owenb_browse_hint > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in $(foreach x,$(BH_MUTANTS),$(word 1,$(subst :, ,$(x)))); do \
		if $(ROOT)/build/test_owenb_browse_hint_mut_$$m > $@.log 2>&1; then echo "MUTANT SURVIVED: $$m"; exit 1; fi; done
	@echo "test_owenb_browse_hint: $(words $(BH_MUT_BINS)) mutants FAIL as required" | tee $@

# ASan+UBSan build of the same test (host-only): make ROOT=<wt> <wt>/build/test_owenb_browse_hint.san
$(ROOT)/build/test_owenb_browse_hint.san: $(BH_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined \
		-o $@ $(ROOT)/tests/unit/test_owenb_browse_hint.cpp $(OWENB_RING_OBJS) -lm

unit-unlocked unit-host-unlocked: $(ROOT)/build/test_owenb_browse_hint $(ROOT)/build/test_owenb_browse_hint.mutant-check
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
	$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -I$(ROOT)/arm/misterplexd \
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

# P1b T6-T9 on the Engineer T5 branch. The same host pieces were already on the frozen T3 tip.
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

# ALLOW_480P60. Each mutant must FAIL at its pinned check.
ALLOW_480P60_MUTANTS := MUT_ALLOW_480P60_FORCED:F0 MUT_ALLOW_480P60_IGNORED:F1 \
		MUT_480P60_NO_BOX:F2 MUT_480P60_NO_WIDTH:F3 MUT_480P60_NO_HEIGHT:F2h \
		MUT_480P60_NO_BITDEPTH:F4 MUT_480P60_NO_BITRATE:F5 MUT_480P60_NO_CODEC:F6 \
		MUT_480P60_PROFILE_STALE:F7 MUT_480P60_ANY_FPS:F8 MUT_480P60_NO_FPS_MAX:F9 \
		MUT_480P60_LEGACY_TOO:F10 MUT_480P60_ON_LOWERED:F11 MUT_480P60_LOWER_KEEPS60:F11b \
		MUT_480P60_RESOLVE_SKIP:R1
$(ROOT)/build/test_allow_480p60: $(ROOT)/tests/unit/test_allow_480p60.cpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/arm/misterplexd/plex_resolve.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -o $@ \
		$(ROOT)/tests/unit/test_allow_480p60.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp

$(ROOT)/build/test_allow_480p60.mutant-check: $(ROOT)/build/test_allow_480p60
	@$(ROOT)/build/test_allow_480p60 > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for mc in $(ALLOW_480P60_MUTANTS); do m=$${mc%%:*}; c=$${mc##*:}; \
		$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -D$$m -o $@.$$m \
			$(ROOT)/tests/unit/test_allow_480p60.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp || exit 1; \
		if $@.$$m > $@.$$m.log 2>&1; then echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
		grep -q "FAIL: $$c" $@.$$m.log || { echo "MUTANT $$m KILLED BY THE WRONG CHECK (want $$c)"; cat $@.$$m.log; exit 1; }; \
	done
	@echo "test_allow_480p60: $(words $(ALLOW_480P60_MUTANTS)) mutants FAIL as required" | tee $@

# PAUSE_DIM. Each mutant must FAIL at its pinned check.
PAUSE_DIM_MUTANTS := MUT_PAUSE_DIM_FORCED:F0 MUT_PAUSE_DIM_IGNORED:F1 \
		MUT_PAUSE_DIM_NO_SCALE:F1 MUT_PAUSE_DIM_NO_PAUSE:F1 \
		MUT_PAUSE_DIM_SCALES_ALPHA:F1a MUT_PAUSE_DIM_PLAYING_TOO:F2 \
		MUT_PAUSE_DIM_NO_RESTORE:F3 MUT_PAUSE_DIM_IDLE_NOW:F4b \
		MUT_PAUSE_DIM_IDLE_OFF:F4 MUT_PAUSE_DIM_NO_RESET:F4c \
		MUT_PAUSE_DIM_NO_STATIC:F4s MUT_PAUSE_DIM_IDLE_CARD:F5
$(ROOT)/build/test_pause_dim: $(ROOT)/tests/unit/test_pause_dim.cpp \
		$(ROOT)/host/libmisterplex/pause_dim.hpp $(ROOT)/host/libmisterplex/owenb_osd_draw.hpp \
		$(ROOT)/host/libmisterplex/owenb_osd_renderer.hpp $(ROOT)/arm/misterplexd/main.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -o $@ $(ROOT)/tests/unit/test_pause_dim.cpp

$(ROOT)/build/test_pause_dim.mutant-check: $(ROOT)/build/test_pause_dim
	@$(ROOT)/build/test_pause_dim > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for mc in $(PAUSE_DIM_MUTANTS); do m=$${mc%%:*}; c=$${mc##*:}; \
		$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -D$$m -o $@.$$m \
			$(ROOT)/tests/unit/test_pause_dim.cpp || exit 1; \
		if $@.$$m > $@.$$m.log 2>&1; then echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
		grep -q "FAIL: $$c " $@.$$m.log || { echo "MUTANT $$m KILLED BY THE WRONG CHECK (want $$c)"; cat $@.$$m.log; exit 1; }; \
	done
	@echo "test_pause_dim: $(words $(PAUSE_DIM_MUTANTS)) mutants FAIL as required" | tee $@

$(ROOT)/build/test_inplay_osd: $(ROOT)/tests/unit/test_inplay_osd.cpp \
		$(ROOT)/host/libmisterplex/owenb_osd_draw.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -o $@ $(ROOT)/tests/unit/test_inplay_osd.cpp
$(ROOT)/build/test_inplay_osd.ok: $(ROOT)/build/test_inplay_osd
	$(ROOT)/build/test_inplay_osd
	touch $@
$(ROOT)/build/test_inplay_osd.mutant-check: $(ROOT)/build/test_inplay_osd
	@$(ROOT)/build/test_inplay_osd > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -DMUT_CRT_INSET_0 \
		-o $(ROOT)/build/test_inplay_osd.mut $(ROOT)/tests/unit/test_inplay_osd.cpp
	@if $(ROOT)/build/test_inplay_osd.mut > $@.mut.log 2>&1; then \
		echo "test_inplay_osd: MUT_CRT_INSET_0 SURVIVED"; exit 1; fi
	@grep -q "CRT inset is 24 lines" $@.mut.log || { echo "MUTANT MUT_CRT_INSET_0 KILLED BY THE WRONG CHECK"; cat $@.mut.log; exit 1; }
	@echo "test_inplay_osd: MUT_CRT_INSET_0 FAIL as required" | tee $@
	@rm -f $(ROOT)/build/test_inplay_osd.mut
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_inplay_osd.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_inplay_osd.mutant-check

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
		info_selectable:MUT_LIBRARY_INFO_SELECTABLE play_no_transport:MUT_ROUTE_PLAY_NO_TRANSPORT \
		info_browse_always:MUT_ROUTE_INFO_BROWSE_ALWAYS info_stops_play:MUT_ROUTE_INFO_STOPS_PLAY \
		dpad_activate:MUT_RC1037_DPAD_ACTIVATE menu_mark_off:MUT_MW_MENU_WHEN_OFF
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
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_owenb_input.mutant-check

# Host compile of the ARM daemon TU. Host tests listed media_player.cpp as a
# make dep but never compiled it, so a wrong qualifier only failed on arm-plexd.
# RING is enough: setIdleReadyLine is outside the LIBAV ifdef.
$(ROOT)/build/media_player.host-syntax.ok: $(ROOT)/arm/misterplexd/media_player.cpp \
		$(ROOT)/host/libmisterplex/owenb_menu.hpp $(ROOT)/host/libmisterplex/remember_tracks.hpp \
		$(ROOT)/host/libmisterplex/scrub_thumbs.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -I$(ROOT)/arm/misterplexd \
		-I$(ROOT)/host/libmisterplex/plexui -DMPX_HAVE_OWENB_RING \
		-fsyntax-only $(ROOT)/arm/misterplexd/media_player.cpp
	@touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/media_player.host-syntax.ok

# Same TU with the armhf compiler when one is on PATH (make arm-plexd-daemon's ARM_CXX).
$(ROOT)/build/media_player.arm-syntax.ok: $(ROOT)/arm/misterplexd/media_player.cpp \
		$(ROOT)/host/libmisterplex/owenb_menu.hpp
	@mkdir -p $(ROOT)/build
	@if [ -z "$(ARM_CXX)" ]; then echo "media_player.arm-syntax: no ARM_CXX"; exit 1; fi
	$(ARM_CXX) -std=c++17 -Wall -I$(ROOT)/host $(OWENB_SESS_FLAGS) -I$(ROOT)/arm/misterplexd \
		-I$(ROOT)/host/libmisterplex/plexui -DMPX_HAVE_OWENB_RING \
		-fsyntax-only $(ROOT)/arm/misterplexd/media_player.cpp
	@touch $@

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

# P2: the Spelunkr header (moov) cache end-to-end through the real AvSource::open() wiring
# (OpenRequest::headerCacheSize/headerCacheEtag), not just HeaderCacheAvio in isolation.
$(ROOT)/build/test_spelunkr_header_cache_av: $(ROOT)/tests/unit/test_spelunkr_header_cache_integration.cpp \
		$(ROOT)/arm/misterplexd/owenb_av_source.cpp $(ROOT)/arm/misterplexd/owenb_av_source.hpp \
		$(ROOT)/arm/misterplexd/spelunkr/header_cache.hpp $(ROOT)/arm/misterplexd/spelunkr/header_cache_avio.hpp \
		$(OWENB_PLAY_SRCS) $(OWENB_PLAY_HDRS) $(OWENB_RING_OBJS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -DMPX_HAVE_LIBAV -I$(ROOT)/arm/misterplexd \
		$(shell pkg-config --cflags $(L2_DEMUX_AV)) -o $@ $(ROOT)/tests/unit/test_spelunkr_header_cache_integration.cpp \
		$(ROOT)/arm/misterplexd/owenb_av_source.cpp $(OWENB_PLAY_SRCS) $(OWENB_RING_OBJS) \
		$(shell pkg-config --libs $(L2_DEMUX_AV)) -lm

$(ROOT)/build/test_spelunkr_header_cache_av.ok: $(ROOT)/build/test_spelunkr_header_cache_av
	$(ROOT)/build/test_spelunkr_header_cache_av
	touch $@

# #231 Glass G: the premature-EOF reopen retries a refused/reset/unreachable open (real libav input).
PREMATURE_REOPEN_MUTANTS := MUT_REOPEN_REFUSED_FATAL MUT_REOPEN_NO_NET_RETRY MUT_REOPEN_RETRY_IGNORE_OFF MUT_REOPEN_NO_BUDGET MUT_OPEN_NO_STOP_CANCEL MUT_REOPEN_NO_ATTEMPT_CAP MUT_REOPEN_CAP_IGNORE_OFF
PREMATURE_REOPEN_DEPS := $(ROOT)/tests/unit/test_premature_reopen.cpp $(ROOT)/tests/unit/fake_owenb_core.hpp \
		$(ROOT)/arm/misterplexd/owenb_av_source.cpp $(ROOT)/arm/misterplexd/owenb_av_source.hpp \
		$(OWENB_PLAY_SRCS) $(OWENB_PLAY_HDRS) $(OWENB_RING_OBJS) $(ROOT)/tests/fixtures/owenb_t5/cabac_aac44k_6s.ts
PREMATURE_REOPEN_CMD = $(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -DMPX_HAVE_LIBAV -I$(ROOT)/arm/misterplexd \
		$(shell pkg-config --cflags $(L2_DEMUX_AV)) $(1) -o $(2) $(ROOT)/tests/unit/test_premature_reopen.cpp \
		$(ROOT)/arm/misterplexd/owenb_av_source.cpp $(OWENB_PLAY_SRCS) $(OWENB_RING_OBJS) \
		$(shell pkg-config --libs $(L2_DEMUX_AV)) -lm -pthread
$(ROOT)/build/test_premature_reopen: $(PREMATURE_REOPEN_DEPS)
	@mkdir -p $(ROOT)/build
	$(call PREMATURE_REOPEN_CMD,,$@)
$(ROOT)/build/test_premature_reopen.ok: $(ROOT)/build/test_premature_reopen
	$< && touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_premature_reopen.ok
$(ROOT)/build/test_premature_reopen.mutant-check: $(PREMATURE_REOPEN_DEPS)
	@mkdir -p $(ROOT)/build
	@set -e; for m in $(PREMATURE_REOPEN_MUTANTS); do \
	  $(call PREMATURE_REOPEN_CMD,-D$$m,$(ROOT)/build/test_premature_reopen.$$m); \
	  if $(ROOT)/build/test_premature_reopen.$$m >/dev/null 2>&1; then echo "SURVIVED: $$m"; exit 1; fi; \
	done; echo "test_premature_reopen: $(words $(PREMATURE_REOPEN_MUTANTS)) mutants FAIL as required"; touch $@

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
		-I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui -I$(ROOT)/arm/misterplexd -o $@ \
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

# FPGA Bug 08:17 (b) / 08:18: D52-LOG reads the PLX page only through the daemon's one mapper
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
# R1 PLXH host side (r1_plxh.hpp; FPGA Bug 19:01 A, 19:16 (1)(c), 19:27 F5; K2) against the RTL
# model of the PLX slots. Each MUT_* build must FAIL.
# R1 PLXH host side (r1_plxh.hpp; FPGA Bug 19:01 A, 19:16 (1)(c), 19:27 F5; K2; 20:08 no-FB_VBL
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

# R1 idle FB paint target + entry clear (r1_idle_paint.hpp; FPGA Bug 20:18 STATE trigger): rows
# R-a..R-i and S0 against a model of fb480 + the reader's clears; FPGA Bug 06:57 D2/D3/D1 rows R-j
# (session END re-shows the idle FB), R-k (no en=1 from the PLAY request to BEGIN), R-l (a refused
# flip is retried); FPGA Bug 07:30 C1 row R-m (the PLAY-request hold is token-matched: no stale
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

# FPGA Bug 11:36 R2: R-m m8 (a throw releases) and m10 (a started doPlay only disowns) have their own
# mutants, applied to a scratch copy of host/libmisterplex only, so the tree and the ARM never change.
$(ROOT)/build/test_r1_hold_mutants.ok: $(R1_IDLE_ENTRY_DEPS) $(ROOT)/tests/unit/test_r1_hold_mutants.sh
	@mkdir -p $(ROOT)/build
	bash $(ROOT)/tests/unit/test_r1_hold_mutants.sh > $@.log 2>&1 || { cat $@.log; exit 1; }
	@tail -n 1 $@.log > $@ && cat $@

unit-unlocked unit-host-unlocked: $(ROOT)/build/test_r1_hold_mutants.ok

# FPGA Bug 06:57 D1: the R1 PLX slots at daemon start on the real fpga_spi.cpp (unmanaged gate, sparse
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

# FPGA Bug 07:30 C2: the R1 PLX page under concurrency on the real fpga_spi.cpp: 8 threads' first PLX
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

# FPGA Bug 09:03 C1: the lock-order row's teeth. MUT_C1_LOCK_ORDER_ABBA (layout changes run holding
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

# N2 (copilot-480i 10/4; FPGA Bug F1-F5): an FFmpeg http EIO at the server's close of a
# length-less response (the ladder proxy) ends the stream only near the PMS duration. The rule
# alone (unit), through the demux on host libav with mutants, and the whole daemon on the
# coregate fixtures with its 2164c4ad control (these two skip without libav/ffmpeg/unshare).
$(ROOT)/build/test_input_close_eof: $(ROOT)/tests/unit/test_input_close_eof.cpp \
		$(ROOT)/host/libmisterplex/av_inproc_decode.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_input_close_eof.cpp

HTTP_RANGE_DEPS := $(ROOT)/tests/unit/test_http_range_resume.cpp \
		$(ROOT)/host/libmisterplex/http_range_resume.hpp \
		$(ROOT)/host/libmisterplex/http_range_reader.hpp \
		$(ROOT)/arm/misterplexd/owenb_av_source.cpp \
		$(ROOT)/arm/misterplexd/av_inproc_decode.cpp \
		$(ROOT)/arm/misterplexd/main.cpp \
		$(ROOT)/assets/misterplex.conf.example
HTTP_RANGE_MUTANTS := no_range:MUT_HTTP_RANGE_NO_RANGE no_resume:MUT_HTTP_RANGE_NO_RESUME \
		no_200_skip:MUT_HTTP_RANGE_NO_200_SKIP no_budget:MUT_HTTP_RANGE_NO_BUDGET \
		ignore_off:MUT_HTTP_RANGE_IGNORE_OFF \
		no_connect_retry:MUT_HTTP_RANGE_NO_CONNECT_RETRY retry_no_budget:MUT_HTTP_RANGE_RETRY_NO_BUDGET \
		retry_ignore_off:MUT_HTTP_RANGE_RETRY_IGNORE_OFF long_connect:MUT_HTTP_RANGE_LONG_CONNECT \
		connect_no_cancel:MUT_HTTP_RANGE_CONNECT_NO_CANCEL
HTTP_RANGE_MUT_BINS := $(foreach m,$(HTTP_RANGE_MUTANTS),$(ROOT)/build/test_http_range_resume_mut_$(word 1,$(subst :, ,$(m))))

$(ROOT)/build/test_http_range_resume: $(HTTP_RANGE_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -o $@ $(ROOT)/tests/unit/test_http_range_resume.cpp

$(ROOT)/build/test_http_range_resume_mut_%: $(HTTP_RANGE_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread \
		-D$(word 2,$(subst :, ,$(filter $*:%,$(HTTP_RANGE_MUTANTS)))) -o $@ \
		$(ROOT)/tests/unit/test_http_range_resume.cpp

$(ROOT)/build/test_http_range_resume.mutant-check: $(ROOT)/build/test_http_range_resume \
		$(HTTP_RANGE_MUT_BINS)
	@$(ROOT)/build/test_http_range_resume > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in $(foreach x,$(HTTP_RANGE_MUTANTS),$(word 1,$(subst :, ,$(x)))); do \
		if timeout 60 $(ROOT)/build/test_http_range_resume_mut_$$m > $@.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m"; exit 1; fi; done
	@echo "test_http_range_resume: $(words $(HTTP_RANGE_MUTANTS)) mutants FAIL as required" | tee $@

$(ROOT)/build/test_http_range_resume_asan: $(HTTP_RANGE_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(PLEXUI_ASAN_CXXFLAGS) -pthread -o $@ $(ROOT)/tests/unit/test_http_range_resume.cpp

$(ROOT)/build/test_http_range_resume_asan.check: $(ROOT)/build/test_http_range_resume_asan
	ASAN_OPTIONS=detect_leaks=1:quarantine_size_mb=0 $(ROOT)/build/test_http_range_resume_asan
	@touch $@

n2-eof: $(ROOT)/build/test_input_close_eof
	$(ROOT)/build/test_input_close_eof
	N2_EOF_MUTANTS=1 bash $(ROOT)/tests/unit/test_input_close_eof_demux.sh
	N2_EOF_E2E_MUTANTS=1 bash $(ROOT)/tests/unit/test_n2_eof_e2e.sh

# Core gate mutation check: unit, daemon e2e, session e2e, audio e2e and browse e2e mutants (about 90 min).
# The daemon/browse e2e classes start host daemons: run them only through ~/bin/heavy-sim-slot in a private
# PID + net namespace (crew rule B).
core-gate-mutants:
	python3 $(ROOT)/tests/unit/core_gate_mutants.py all

# MAST-WEDGE (FPGA Bug 21:14): the gate-open probe line, the reload marker and decision, and the
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

# GET /misterplex/mast (MAST R5b follow-up; FPGA Bug 01:28/03:52): the read-only status reply
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
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread \
		-DMPX_MAST_RELOAD_MARKER_PATH='"$(ROOT)/build/test_companion_mast_status.marker"' -o $@ \
		$(ROOT)/tests/unit/test_companion_mast_status.cpp $(ROOT)/arm/misterplexd/companion.cpp \
		$(ROOT)/arm/misterplexd/pms_timeline.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/host/libmisterplex/plexui/plexui_http.cpp
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

$(ROOT)/build/test_mraudio_oneshot: $(ROOT)/tests/unit/test_mraudio_oneshot.cpp \
		$(ROOT)/host/libmisterplex/mraudio_oneshot.hpp \
		$(ROOT)/host/libmisterplex/theme_music.hpp \
		$(ROOT)/host/libmisterplex/mraudio_status.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_mraudio_oneshot.cpp
$(ROOT)/build/test_mraudio_oneshot.ok: $(ROOT)/build/test_mraudio_oneshot
	$(ROOT)/build/test_mraudio_oneshot
	touch $@
$(ROOT)/build/test_mraudio_oneshot.mutant-check: $(ROOT)/tests/unit/test_mraudio_oneshot.cpp \
		$(ROOT)/host/libmisterplex/mraudio_oneshot.hpp \
		$(ROOT)/host/libmisterplex/theme_music.hpp
	@mkdir -p $(ROOT)/build
	@for M in MUT_ONESHOT_WRAP MUT_ONESHOT_PUMP_SKIP MUT_ONESHOT_IDLE_SKIP MUT_ONESHOT_CLICK_SILENT \
		MUT_THEME_DEFAULT_ON MUT_THEME_SILENT MUT_THEME_ALWAYS MUT_THEME_TOKEN_IN_URL \
		MUT_THEME_SHOW_SKIP MUT_THEME_SHOW_TOKEN_QUERY; do \
		$(CXX) $(CXXFLAGS) -D$$M -o $(ROOT)/build/test_mraudio_oneshot.$$M \
			$(ROOT)/tests/unit/test_mraudio_oneshot.cpp || exit 1; \
		if $(ROOT)/build/test_mraudio_oneshot.$$M >/dev/null 2>&1; then \
			echo "test_mraudio_oneshot: $$M SURVIVED"; exit 1; fi; \
		echo "test_mraudio_oneshot: $$M FAIL as required"; \
		rm -f $(ROOT)/build/test_mraudio_oneshot.$$M; \
	done
	@touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_mraudio_oneshot.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_mraudio_oneshot.mutant-check

$(ROOT)/build/test_theme_decode: $(ROOT)/tests/unit/test_theme_decode.cpp \
		$(ROOT)/host/libmisterplex/theme_decode.cpp \
		$(ROOT)/host/libmisterplex/theme_decode.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -DMPX_HAVE_LIBAV -I$(ROOT)/host \
		$(shell pkg-config --cflags $(L2_DEMUX_AV)) -o $@ \
		$(ROOT)/tests/unit/test_theme_decode.cpp $(ROOT)/host/libmisterplex/theme_decode.cpp \
		$(shell pkg-config --libs $(L2_DEMUX_AV)) -lm
$(ROOT)/build/test_theme_decode.ok: $(ROOT)/build/test_theme_decode
	cd $(ROOT) && $(ROOT)/build/test_theme_decode
	touch $@
$(ROOT)/build/test_theme_decode.mutant-check: $(ROOT)/tests/unit/test_theme_decode.cpp \
		$(ROOT)/host/libmisterplex/theme_decode.cpp \
		$(ROOT)/host/libmisterplex/theme_decode.hpp
	@mkdir -p $(ROOT)/build
	@for M in MUT_THEME_DECODE_PASSTHROUGH MUT_THEME_DECODE_NO_CAP; do \
		$(CXX) $(CXXFLAGS) -DMPX_HAVE_LIBAV -D$$M -I$(ROOT)/host \
			$(shell pkg-config --cflags $(L2_DEMUX_AV)) -o $(ROOT)/build/test_theme_decode.$$M \
			$(ROOT)/tests/unit/test_theme_decode.cpp $(ROOT)/host/libmisterplex/theme_decode.cpp \
			$(shell pkg-config --libs $(L2_DEMUX_AV)) -lm || exit 1; \
		if cd $(ROOT) && $(ROOT)/build/test_theme_decode.$$M >/dev/null 2>&1; then \
			echo "test_theme_decode: $$M SURVIVED"; exit 1; fi; \
		echo "test_theme_decode: $$M FAIL as required"; \
		rm -f $(ROOT)/build/test_theme_decode.$$M; \
	done
	@touch $@
unit-unlocked unit-host-unlocked: $(if $(L2_DEMUX_OK),$(ROOT)/build/test_theme_decode.ok)
unit-unlocked unit-host-unlocked: $(if $(L2_DEMUX_OK),$(ROOT)/build/test_theme_decode.mutant-check)

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
$(ROOT)/build/test_plexob_yc.mutant-check: $(ROOT)/build/test_plexob_yc
	@$(ROOT)/build/test_plexob_yc > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@$(CXX) $(CXXFLAGS) -pthread -DMPX_SOURCE_ROOT='"$(ROOT)"' -DMUT_YC_WORD_82 \
		-o $(ROOT)/build/test_plexob_yc.mut $(ROOT)/tests/unit/test_plexob_yc.cpp
	@if $(ROOT)/build/test_plexob_yc.mut > $@.mut.log 2>&1; then \
		echo "test_plexob_yc: MUT_YC_WORD_82 SURVIVED"; exit 1; fi
	@grep -q "Y/C ring word is 87" $@.mut.log || { echo "MUTANT MUT_YC_WORD_82 KILLED BY THE WRONG CHECK"; cat $@.mut.log; exit 1; }
	@$(CXX) $(CXXFLAGS) -pthread -DMPX_SOURCE_ROOT='"$(ROOT)"' -DMUT_YC_NO_1101 \
		-o $(ROOT)/build/test_plexob_yc.mut1101 $(ROOT)/tests/unit/test_plexob_yc.cpp
	@if $(ROOT)/build/test_plexob_yc.mut1101 > $@.mut1101.log 2>&1; then \
		echo "test_plexob_yc: MUT_YC_NO_1101 SURVIVED"; exit 1; fi
	@grep -q "the release id, the #278 id, and the older Y/C ids are accepted" $@.mut1101.log || \
		{ echo "MUTANT MUT_YC_NO_1101 KILLED BY THE WRONG CHECK"; cat $@.mut1101.log; exit 1; }
	@echo "test_plexob_yc: MUT_YC_WORD_82 MUT_YC_NO_1101 FAIL as required" | tee $@
	@rm -f $(ROOT)/build/test_plexob_yc.mut $(ROOT)/build/test_plexob_yc.mut1101
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plexob_yc.mutant-check

$(ROOT)/build/test_playback_overlay: $(ROOT)/tests/unit/test_playback_overlay.cpp \
		$(ROOT)/host/libmisterplex/playback_overlay.hpp \
		$(ROOT)/host/libmisterplex/fpga_playback_overlay.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_playback_overlay.cpp

# Overlay publisher lab flags (reviewer overlay-onchange 1004 + D52 RCA2 suspect 3):
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

# PLX mailbox page per build row and the live-glass probe on it (FPGA Bug 00:55).
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
		ID_SUB) pin='test_resolve.cpp:722: selectQueueIndex(f1, "", "15", &by) == 1' ;; \
		ID_RK) pin='test_resolve.cpp:728: selectQueueIndex(f2, "", "5001", &by) == 1' ;; \
		KEY_SUB) pin='test_resolve.cpp:741: selectQueueIndex(f3rk, "/library/metadata/123", "", &by) == 1' ;; \
		BIND_SUB) pin='test_resolve.cpp:769: !firstResolveBinds(f3[0], "/library/metadata/123", "123")' ;; \
		IDX0) pin='test_resolve.cpp:751: selectQueueIndex({}, "/library/metadata/123", "", &by) == -1' ;; \
		STEP_NEG) pin='test_resolve.cpp:775: !queueStepDest(-1, 1, 2)' ;; \
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
TEST_RESOLVE_MUT_PIN := test_resolve.cpp:705: b.playQueueItemId.empty()
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


# WAN read-stall bound ( lab run-A AVERROR_EXIT at 3.6 s).
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

# Prod line: seekTo keeps the playMedia server token ( lab scrub 1556).
SEEK_TOKEN_SRC := $(ROOT)/tests/unit/test_seek_token.cpp $(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/host/libmisterplex/plexui/plexui_http.cpp
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
$(ROOT)/build/test_seek_token_MUT_F1: $(SEEK_TOKEN_SRC) $(ROOT)/arm/misterplexd/companion.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -DMUT_F1_PLAYMEDIA_REFRESHES -o $@ $(SEEK_TOKEN_SRC)
$(ROOT)/build/test_seek_token.f1-mutant: $(ROOT)/build/test_seek_token $(ROOT)/build/test_seek_token_MUT_F1
	@$(ROOT)/build/test_seek_token > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@if $(ROOT)/build/test_seek_token_MUT_F1 > $@.mut.log 2>&1; then echo "MUTANT SURVIVED: MUT_F1_PLAYMEDIA_REFRESHES"; exit 1; fi
	@grep -q 'FAIL seek token: F1' $@.mut.log || { echo "MUTANT KILLED BY THE WRONG CHECK"; cat $@.mut.log; exit 1; }
	@echo "test_seek_token: mutant MUT_F1_PLAYMEDIA_REFRESHES FAILs at F1 as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_seek_token.f1-mutant
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_seek_token $(ROOT)/build/test_seek_token.mutant-check

$(ROOT)/build/test_companion_plant_seek: $(ROOT)/tests/unit/test_companion_plant_seek.cpp \
		$(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/arm/misterplexd/companion.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -o $@ \
		$(ROOT)/tests/unit/test_companion_plant_seek.cpp $(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/host/libmisterplex/plexui/plexui_http.cpp

$(ROOT)/build/test_companion_480i_field_rate: $(ROOT)/tests/unit/test_companion_480i_field_rate.cpp \
		$(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/arm/misterplexd/companion.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -o $@ \
		$(ROOT)/tests/unit/test_companion_480i_field_rate.cpp $(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/host/libmisterplex/plexui/plexui_http.cpp

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

# Shawn 10-23: no built-in test clip / synthetic source in the daemon (source scan + play-source allowlist).
$(ROOT)/build/test_no_synthetic_source: $(ROOT)/tests/unit/test_no_synthetic_source.cpp \
		$(ROOT)/arm/misterplexd/media_source_policy.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -std=c++17 -I$(ROOT)/arm/misterplexd -o $@ $(ROOT)/tests/unit/test_no_synthetic_source.cpp
# The same test under ASan+UBSan.
$(ROOT)/build/test_no_synthetic_source_asan: $(ROOT)/tests/unit/test_no_synthetic_source.cpp \
		$(ROOT)/arm/misterplexd/media_source_policy.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -std=c++17 -fsanitize=address,undefined -fno-omit-frame-pointer -I$(ROOT)/arm/misterplexd -o $@ $(ROOT)/tests/unit/test_no_synthetic_source.cpp
$(ROOT)/build/test_no_synthetic_source_asan.check: $(ROOT)/build/test_no_synthetic_source_asan
	$(ROOT)/build/test_no_synthetic_source_asan $(ROOT)
	@touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_no_synthetic_source_asan.check
# YouTube C3 (yt-av two-URL source) must pass the play allowlist; the prefix must not become a bypass.
YTAV_MUTANTS := no_ytav:MUT_YT_NO_YTAV any_prefix:MUT_YT_ANY_PREFIX video_only:MUT_YT_VIDEO_ONLY \
		any_parts:MUT_YT_ANY_PARTS loose_url:MUT_YT_LOOSE_URL
YTAV_MUT_BINS := $(foreach m,$(YTAV_MUTANTS),$(ROOT)/build/test_yt_av_source_policy_mut_$(word 1,$(subst :, ,$(m))))
YTAV_DEPS := $(ROOT)/tests/unit/test_yt_av_source_policy.cpp $(ROOT)/arm/misterplexd/media_source_policy.hpp \
		$(ROOT)/arm/misterplexd/gcast/dash_seek.hpp
$(ROOT)/build/test_yt_av_source_policy: $(YTAV_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -std=c++17 -I$(ROOT)/arm/misterplexd -o $@ $(ROOT)/tests/unit/test_yt_av_source_policy.cpp
$(ROOT)/build/test_yt_av_source_policy_mut_%: $(YTAV_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -std=c++17 -I$(ROOT)/arm/misterplexd -D$(word 2,$(subst :, ,$(filter $*:%,$(YTAV_MUTANTS)))) \
		-o $@ $(ROOT)/tests/unit/test_yt_av_source_policy.cpp
$(ROOT)/build/test_yt_av_source_policy.mutant-check: $(ROOT)/build/test_yt_av_source_policy $(YTAV_MUT_BINS)
	@$(ROOT)/build/test_yt_av_source_policy > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in $(foreach x,$(YTAV_MUTANTS),$(word 1,$(subst :, ,$(x)))); do \
		if $(ROOT)/build/test_yt_av_source_policy_mut_$$m > $@.log 2>&1; then echo "MUTANT SURVIVED: $$m"; exit 1; fi; done
	@echo "test_yt_av_source_policy: $(words $(YTAV_MUT_BINS)) mutants FAIL as required" | tee $@
$(ROOT)/build/test_yt_av_source_policy.san: $(YTAV_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -std=c++17 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined \
		-I$(ROOT)/arm/misterplexd -o $@ $(ROOT)/tests/unit/test_yt_av_source_policy.cpp
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_yt_av_source_policy $(ROOT)/build/test_yt_av_source_policy.mutant-check

$(ROOT)/build/test_companion_title_policy: $(ROOT)/tests/unit/test_companion_title_policy.cpp \
		$(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/arm/misterplexd/companion.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -o $@ \
		$(ROOT)/tests/unit/test_companion_title_policy.cpp $(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/host/libmisterplex/plexui/plexui_http.cpp

$(ROOT)/build/test_plex_tv_link: $(ROOT)/tests/unit/test_plex_tv_link.cpp \
		$(ROOT)/arm/misterplexd/plex_tv_link.hpp $(ROOT)/host/libmisterplex/header_int.hpp $(ROOT)/host/libmisterplex/plex_link_label.hpp $(ROOT)/host/libmisterplex/gcast_osd.hpp $(ROOT)/arm/misterplexd/companion.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT) -I$(ROOT)/host -pthread -o $@ \
		$(ROOT)/tests/unit/test_plex_tv_link.cpp

# #228: 4-char PIN endpoint and link retry. Both mutants must FAIL with their own check.
$(ROOT)/build/test_plex_tv_link_asan: $(ROOT)/tests/unit/test_plex_tv_link.cpp \
		$(ROOT)/arm/misterplexd/plex_tv_link.hpp $(ROOT)/host/libmisterplex/header_int.hpp $(ROOT)/host/libmisterplex/plex_link_label.hpp $(ROOT)/host/libmisterplex/gcast_osd.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer \
		-I$(ROOT) -I$(ROOT)/host -pthread -o $@ $(ROOT)/tests/unit/test_plex_tv_link.cpp
$(ROOT)/build/test_plex_tv_link_asan.ok: $(ROOT)/build/test_plex_tv_link_asan
	cd $(ROOT) && ASAN_OPTIONS=detect_leaks=1 $(ROOT)/build/test_plex_tv_link_asan
	touch $@
$(ROOT)/build/test_plex_tv_link.mutant-check: $(ROOT)/build/test_plex_tv_link
	@cd $(ROOT) && $(ROOT)/build/test_plex_tv_link > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in MUT_PLEXLINK_STRONG MUT_PLEXLINK_NO_RETRY MUT_PLEXLINK_NO_IDLE_BACKOFF \
		MUT_PLEXLINK_FLAT_RETRY MUT_PLEXLINK_TOKEN_LOOP MUT_PLEXLINK_RA_REPLACES \
		MUT_PLEXLINK_TOKEN_GIVEUP MUT_HDRINT_ATOI MUT_PLEXLINK_LOG_MARKER_AS_CODE \
		MUT_PLEXLINK_FAIL_CLEARS; do \
		$(CXX) $(CXXFLAGS) -D$$m -I$(ROOT) -I$(ROOT)/host -pthread -o $(ROOT)/build/test_plex_tv_link.$$m \
			$(ROOT)/tests/unit/test_plex_tv_link.cpp; \
		if (cd $(ROOT) && $(ROOT)/build/test_plex_tv_link.$$m > $@.$$m.log 2>&1); then \
			echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
		case $$m in MUT_PLEXLINK_STRONG) c=L3 ;; MUT_PLEXLINK_NO_RETRY) c=L2 ;; \
			MUT_PLEXLINK_NO_IDLE_BACKOFF) c=L4 ;; MUT_PLEXLINK_FLAT_RETRY) c=L5 ;; MUT_PLEXLINK_TOKEN_LOOP) c=L8 ;; \
			MUT_PLEXLINK_RA_REPLACES) c=L7 ;; MUT_PLEXLINK_TOKEN_GIVEUP) c=L6 ;; MUT_HDRINT_ATOI) c=L9 ;; \
			MUT_PLEXLINK_LOG_MARKER_AS_CODE) c=L10 ;; MUT_PLEXLINK_FAIL_CLEARS) c=L11 ;; esac; \
		grep -q "FAIL $$c " $@.$$m.log || { echo "MUTANT $$m KILLED BY THE WRONG CHECK (want $$c)"; cat $@.$$m.log; exit 1; }; \
	done
	@echo "test_plex_tv_link: 10 mutants FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plex_tv_link_asan.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plex_tv_link.mutant-check

# GH#23: plex.tv/link QR on the plexui Sign-in card. Size and finder mutants must FAIL.
$(ROOT)/build/test_link_qr: $(ROOT)/tests/unit/test_link_qr.cpp \
		$(ROOT)/host/libmisterplex/plexui/plexui_link.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/host -o $@ $(ROOT)/tests/unit/test_link_qr.cpp
$(ROOT)/build/test_link_qr.ok: $(ROOT)/build/test_link_qr
	$(ROOT)/build/test_link_qr
	touch $@
$(ROOT)/build/test_link_qr.mutant-check: $(ROOT)/build/test_link_qr
	@$(ROOT)/build/test_link_qr > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in MUT_QR_MODULES_1 MUT_QR_BLANK; do \
		$(CXX) $(CXXFLAGS) -I$(ROOT)/host -D$$m -o $(ROOT)/build/test_link_qr.$$m \
			$(ROOT)/tests/unit/test_link_qr.cpp; \
		if $(ROOT)/build/test_link_qr.$$m > $@.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
		case $$m in MUT_QR_MODULES_1) c=QR-SIZE ;; MUT_QR_BLANK) c=QR-FINDER ;; esac; \
		grep -q "FAIL $$c" $@.$$m.log || { echo "MUTANT $$m KILLED BY THE WRONG CHECK (want $$c)"; cat $@.$$m.log; exit 1; }; \
		rm -f $(ROOT)/build/test_link_qr.$$m; \
	done
	@echo "test_link_qr: MUT_QR_MODULES_1 MUT_QR_BLANK FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_link_qr.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_link_qr.mutant-check

RZ_SRC = $(ROOT)/tests/unit/test_companion_resume_zone.cpp $(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/arm/misterplexd/pms_timeline.cpp $(ROOT)/host/libmisterplex/plexui/plexui_http.cpp
RZ_FLAGS = -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread
$(ROOT)/build/test_companion_resume_zone.ok: $(RZ_SRC) $(ROOT)/arm/misterplexd/companion.hpp \
		$(ROOT)/host/libmisterplex/resume_zone.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(RZ_FLAGS) -o $(ROOT)/build/test_companion_resume_zone $(RZ_SRC)
	$(ROOT)/build/test_companion_resume_zone
	@for m in MUT_RZ_NO_FINISHED_ZONE MUT_RZ_NO_TAIL_5S; do \
		$(CXX) $(CXXFLAGS) -D$$m $(RZ_FLAGS) -o $(ROOT)/build/test_companion_resume_zone.mut $(RZ_SRC) 2>/dev/null \
			|| { echo "MUTANT DID NOT COMPILE: $$m"; exit 1; }; \
		if $(ROOT)/build/test_companion_resume_zone.mut >/dev/null 2>&1; then echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
		echo "test_companion_resume_zone: mutant $$m FAILs as required"; \
	done
	@touch $@

unit-unlocked unit-host-unlocked: $(ROOT)/build/test_companion_resume_zone.ok

$(ROOT)/build/test_480i_active_height: $(ROOT)/tests/unit/test_480i_active_height.cpp \
		$(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/arm/misterplexd/companion.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -o $@ \
		$(ROOT)/tests/unit/test_480i_active_height.cpp $(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/host/libmisterplex/plexui/plexui_http.cpp
$(ROOT)/build/test_480i_active_height.ok: $(ROOT)/build/test_480i_active_height
	$(ROOT)/build/test_480i_active_height
	touch $@
$(ROOT)/build/test_480i_active_height.mutant-check: $(ROOT)/build/test_480i_active_height
	@$(ROOT)/build/test_480i_active_height > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -DMUT_480I_FRAME_IS_FIELD \
		-o $(ROOT)/build/test_480i_active_height.mut \
		$(ROOT)/tests/unit/test_480i_active_height.cpp $(ROOT)/arm/misterplexd/companion.cpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/host/libmisterplex/plexui/plexui_http.cpp
	@if $(ROOT)/build/test_480i_active_height.mut > $@.mut.log 2>&1; then \
		echo "test_480i_active_height: MUT_480I_FRAME_IS_FIELD SURVIVED"; exit 1; fi
	@grep -q "k480iActiveHeight" $@.mut.log || { echo "MUTANT MUT_480I_FRAME_IS_FIELD KILLED BY THE WRONG CHECK"; cat $@.mut.log; exit 1; }
	@echo "test_480i_active_height: MUT_480I_FRAME_IS_FIELD FAIL as required" | tee $@
	@rm -f $(ROOT)/build/test_480i_active_height.mut
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_480i_active_height.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_480i_active_height.mutant-check

$(ROOT)/build/test_core_id_select: $(ROOT)/tests/unit/test_core_id_select.cpp \
		$(ROOT)/host/libmisterplex/core_id_select.hpp \
		$(ROOT)/host/libmisterplex/fpga_build_ids.hpp \
		$(ROOT)/host/libmisterplex/display_raster.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/host -o $@ $(ROOT)/tests/unit/test_core_id_select.cpp
$(ROOT)/build/test_core_id_select.mutant-check: $(ROOT)/tests/unit/test_core_id_select.cpp \
		$(ROOT)/host/libmisterplex/display_raster.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -DMUT_GH12_NO_IDLE_VIS0 -I$(ROOT)/host -o $(ROOT)/build/test_core_id_select.gh12 \
		$(ROOT)/tests/unit/test_core_id_select.cpp
	@if $(ROOT)/build/test_core_id_select.gh12 >/dev/null 2>&1; then \
		echo "test_core_id_select: MUT_GH12_NO_IDLE_VIS0 SURVIVED"; exit 1; fi
	@echo "test_core_id_select: MUT_GH12_NO_IDLE_VIS0 FAIL as required"
	@rm -f $(ROOT)/build/test_core_id_select.gh12
	$(CXX) $(CXXFLAGS) -DMUT_GH10_NO_240P -I$(ROOT)/host -o $(ROOT)/build/test_core_id_select.gh10 \
		$(ROOT)/tests/unit/test_core_id_select.cpp
	@if $(ROOT)/build/test_core_id_select.gh10 >/dev/null 2>&1; then \
		echo "test_core_id_select: MUT_GH10_NO_240P SURVIVED"; exit 1; fi
	@echo "test_core_id_select: MUT_GH10_NO_240P FAIL as required"
	@rm -f $(ROOT)/build/test_core_id_select.gh10
	$(CXX) $(CXXFLAGS) -DMUT_GH15_NO_480P -I$(ROOT)/host -o $(ROOT)/build/test_core_id_select.gh15 \
		$(ROOT)/tests/unit/test_core_id_select.cpp
	@if $(ROOT)/build/test_core_id_select.gh15 >/dev/null 2>&1; then \
		echo "test_core_id_select: MUT_GH15_NO_480P SURVIVED"; exit 1; fi
	@echo "test_core_id_select: MUT_GH15_NO_480P FAIL as required"
	@rm -f $(ROOT)/build/test_core_id_select.gh15
	$(CXX) $(CXXFLAGS) -DMUT_FORCE_RASTER_ON_PLAY -I$(ROOT)/host -o $(ROOT)/build/test_core_id_select.playpoke \
		$(ROOT)/tests/unit/test_core_id_select.cpp
	@if $(ROOT)/build/test_core_id_select.playpoke >/dev/null 2>&1; then \
		echo "test_core_id_select: MUT_FORCE_RASTER_ON_PLAY SURVIVED"; exit 1; fi
	@echo "test_core_id_select: MUT_FORCE_RASTER_ON_PLAY FAIL as required"
	@rm -f $(ROOT)/build/test_core_id_select.playpoke
	$(CXX) $(CXXFLAGS) -DMUT_GH10_ALLOW_LCD -I$(ROOT)/host -o $(ROOT)/build/test_core_id_select.lcd \
		$(ROOT)/tests/unit/test_core_id_select.cpp
	@if $(ROOT)/build/test_core_id_select.lcd >/dev/null 2>&1; then \
		echo "test_core_id_select: MUT_GH10_ALLOW_LCD SURVIVED"; exit 1; fi
	@echo "test_core_id_select: MUT_GH10_ALLOW_LCD FAIL as required"
	@rm -f $(ROOT)/build/test_core_id_select.lcd
	@touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_core_id_select.mutant-check

$(ROOT)/build/test_sps_reject_high: $(ROOT)/tests/unit/test_sps_reject_high.cpp \
		$(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/arm/misterplexd/companion.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -o $@ \
		$(ROOT)/tests/unit/test_sps_reject_high.cpp $(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/host/libmisterplex/plexui/plexui_http.cpp

$(ROOT)/build/test_companion_letterbox_bars: $(ROOT)/tests/unit/test_companion_letterbox_bars.cpp \
		$(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/arm/misterplexd/companion.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -o $@ \
		$(ROOT)/tests/unit/test_companion_letterbox_bars.cpp $(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/host/libmisterplex/plexui/plexui_http.cpp

$(ROOT)/build/test_companion_live_clock: $(ROOT)/tests/unit/test_companion_live_clock.cpp \
		$(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/arm/misterplexd/companion.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -o $@ \
		$(ROOT)/tests/unit/test_companion_live_clock.cpp $(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/host/libmisterplex/plexui/plexui_http.cpp

# GOAL #23 / #122: Timeline viewOffset tracks playback; a local stop releases the session key.
# MUT_RC1036_FREEZE_OFFSET keeps viewOffset at 0. MUT_RC1036_KEEP_KEY never starts the idle clock.
RC1036_SRC := $(ROOT)/tests/unit/test_rc1036_scrubber_idle.cpp \
	$(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
	$(ROOT)/arm/misterplexd/pms_timeline.cpp \
	$(ROOT)/host/libmisterplex/plexui/plexui_http.cpp
RC1036_HDR := $(ROOT)/arm/misterplexd/companion.hpp $(ROOT)/arm/misterplexd/pms_timeline.hpp \
	$(ROOT)/arm/misterplexd/plex_resolve.hpp
$(ROOT)/build/test_rc1036_scrubber_idle: $(RC1036_SRC) $(RC1036_HDR)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -o $@ $(RC1036_SRC)
$(ROOT)/build/test_rc1036_scrubber_idle_asan: $(RC1036_SRC) $(RC1036_HDR)
	@mkdir -p $(ROOT)/build
	$(CXX) $(PLEXUI_ASAN_CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -o $@ $(RC1036_SRC)
$(ROOT)/build/test_rc1036_scrubber_idle_FREEZE: $(RC1036_SRC) $(RC1036_HDR)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -DMUT_RC1036_FREEZE_OFFSET -o $@ $(RC1036_SRC)
$(ROOT)/build/test_rc1036_scrubber_idle_KEEP: $(RC1036_SRC) $(RC1036_HDR)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -DMUT_RC1036_KEEP_KEY -o $@ $(RC1036_SRC)
$(ROOT)/build/test_rc1036_scrubber_idle.mutant-check: \
		$(ROOT)/build/test_rc1036_scrubber_idle \
		$(ROOT)/build/test_rc1036_scrubber_idle_asan \
		$(ROOT)/build/test_rc1036_scrubber_idle_FREEZE \
		$(ROOT)/build/test_rc1036_scrubber_idle_KEEP
	@$(ROOT)/build/test_rc1036_scrubber_idle > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@ASAN_OPTIONS=detect_leaks=1 $(ROOT)/build/test_rc1036_scrubber_idle_asan > $@.asan.log 2>&1 || { cat $@.asan.log; exit 1; }
	@if $(ROOT)/build/test_rc1036_scrubber_idle_FREEZE > $@.freeze.log 2>&1; then echo "MUTANT SURVIVED: MUT_RC1036_FREEZE_OFFSET"; exit 1; fi
	@grep -m1 'FAIL' $@.freeze.log | grep -q 'RC1036 viewOffset' || { echo "MUTANT KILLED BY THE WRONG CHECK"; cat $@.freeze.log; exit 1; }
	@if $(ROOT)/build/test_rc1036_scrubber_idle_KEEP > $@.keep.log 2>&1; then echo "MUTANT SURVIVED: MUT_RC1036_KEEP_KEY"; exit 1; fi
	@grep -m1 'FAIL' $@.keep.log | grep -q 'RC1036 idle key' || { echo "MUTANT KILLED BY THE WRONG CHECK"; cat $@.keep.log; exit 1; }
	@echo "test_rc1036_scrubber_idle: mutants MUT_RC1036_FREEZE_OFFSET and MUT_RC1036_KEEP_KEY FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_rc1036_scrubber_idle.mutant-check

$(ROOT)/build/test_track_select: $(ROOT)/tests/unit/test_track_select.cpp \
		$(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/arm/misterplexd/companion.hpp $(ROOT)/arm/misterplexd/plex_resolve.hpp \
		$(ROOT)/host/libmisterplex/track_select.hpp $(ROOT)/host/libmisterplex/forced_subs.hpp \
		$(ROOT)/host/libmisterplex/subtitle_sink.hpp \
		$(ROOT)/host/libmisterplex/owenb_osd_draw.hpp $(ROOT)/host/libmisterplex/owenb_osd_renderer.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -o $@ \
		$(ROOT)/tests/unit/test_track_select.cpp $(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp

$(ROOT)/build/test_track_select_asan: $(ROOT)/tests/unit/test_track_select.cpp \
		$(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/arm/misterplexd/companion.hpp $(ROOT)/arm/misterplexd/plex_resolve.hpp \
		$(ROOT)/host/libmisterplex/track_select.hpp $(ROOT)/host/libmisterplex/forced_subs.hpp \
		$(ROOT)/host/libmisterplex/subtitle_sink.hpp \
		$(ROOT)/host/libmisterplex/owenb_osd_draw.hpp $(ROOT)/host/libmisterplex/owenb_osd_renderer.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(PLEXUI_ASAN_CXXFLAGS) $(OWENB_SESS_FLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -o $@ \
		$(ROOT)/tests/unit/test_track_select.cpp $(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp

$(ROOT)/build/test_srt_cues: $(ROOT)/tests/unit/test_srt_cues.cpp $(ROOT)/host/libmisterplex/subtitle_cues.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/host -o $@ $(ROOT)/tests/unit/test_srt_cues.cpp

$(ROOT)/build/test_ass_to_text: $(ROOT)/tests/unit/test_ass_to_text.cpp $(ROOT)/host/libmisterplex/subtitle_cues.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/host -o $@ $(ROOT)/tests/unit/test_ass_to_text.cpp

$(ROOT)/build/test_subtitle_sink_osd: $(ROOT)/tests/unit/test_subtitle_sink_osd.cpp \
		$(ROOT)/host/libmisterplex/osd_subtitle_sink.hpp $(ROOT)/host/libmisterplex/subtitle_cues.hpp \
		$(ROOT)/host/libmisterplex/subtitle_sink.hpp $(ROOT)/host/libmisterplex/owenb_osd_renderer.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/host -I$(ROOT)/third_party/owenb-h264ring/include -pthread -o $@ $(ROOT)/tests/unit/test_subtitle_sink_osd.cpp

$(ROOT)/build/test_caption_clock: $(ROOT)/tests/unit/test_caption_clock.cpp $(ROOT)/host/libmisterplex/subtitle_cues.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/host -o $@ $(ROOT)/tests/unit/test_caption_clock.cpp

$(ROOT)/build/test_caption_layout: $(ROOT)/tests/unit/test_caption_layout.cpp \
		$(ROOT)/host/libmisterplex/owenb_osd_draw.hpp $(ROOT)/host/libmisterplex/owenb_osd_layout.hpp \
		$(OWENB_RING_OBJS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -I$(ROOT)/host -o $@ \
		$(ROOT)/tests/unit/test_caption_layout.cpp $(OWENB_RING_OBJS) -lm
$(ROOT)/build/test_caption_layout.ok: $(ROOT)/build/test_caption_layout
	$(ROOT)/build/test_caption_layout
	touch $@
$(ROOT)/build/test_caption_layout.mutant-check: $(ROOT)/build/test_caption_layout
	@$(ROOT)/build/test_caption_layout > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -I$(ROOT)/host -DMUT_CAPTION_ODD_LINES \
		-o $(ROOT)/build/test_caption_layout.mut \
		$(ROOT)/tests/unit/test_caption_layout.cpp $(OWENB_RING_OBJS) -lm
	@if $(ROOT)/build/test_caption_layout.mut > $@.mut.log 2>&1; then \
		echo "test_caption_layout: MUT_CAPTION_ODD_LINES SURVIVED"; exit 1; fi
	@grep -q "480i even lines" $@.mut.log || { echo "MUTANT MUT_CAPTION_ODD_LINES KILLED BY THE WRONG CHECK"; cat $@.mut.log; exit 1; }
	@echo "test_caption_layout: MUT_CAPTION_ODD_LINES FAIL as required" | tee $@
	@rm -f $(ROOT)/build/test_caption_layout.mut
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_caption_layout.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_caption_layout.mutant-check

# Soft-sub letterbox parking. ASan/UBSan plus one binary per MUT_SUBPOS_* (each must fail).
SUBPOS_SRC := $(ROOT)/tests/unit/test_subtitle_position.cpp \
		$(ROOT)/host/libmisterplex/subtitle_position.hpp \
		$(ROOT)/host/libmisterplex/owenb_osd_draw.hpp \
		$(ROOT)/host/libmisterplex/owenb_osd_layout.hpp \
		$(ROOT)/arm/misterplexd/main.cpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/assets/misterplex.conf.example
SUBPOS_FLAGS := $(CXXFLAGS) $(OWENB_SESS_FLAGS) -I$(ROOT)/host
SUBPOS_MUTANTS := MUT_SUBPOS_SAFE_OVER_PICTURE MUT_SUBPOS_PICTURE_IN_BAR MUT_SUBPOS_PARSE_SWAP \
		MUT_SUBPOS_NOLB_IN_BAR MUT_SUBPOS_DEFAULT_PICTURE

$(ROOT)/build/test_subtitle_position: $(SUBPOS_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(SUBPOS_FLAGS) -o $@ $(ROOT)/tests/unit/test_subtitle_position.cpp

$(ROOT)/build/test_subtitle_position_asan: $(SUBPOS_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(SUBPOS_FLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -o $@ \
		$(ROOT)/tests/unit/test_subtitle_position.cpp

$(ROOT)/build/test_subtitle_position.ok: $(ROOT)/build/test_subtitle_position $(ROOT)/build/test_subtitle_position_asan
	$(ROOT)/build/test_subtitle_position
	ASAN_OPTIONS=detect_leaks=1 $(ROOT)/build/test_subtitle_position_asan
	@for m in $(SUBPOS_MUTANTS); do \
		$(CXX) $(SUBPOS_FLAGS) -D$$m -o $(ROOT)/build/test_subtitle_position.$$m \
			$(ROOT)/tests/unit/test_subtitle_position.cpp || { echo "MUTANT BUILD $$m"; exit 1; }; \
		if $(ROOT)/build/test_subtitle_position.$$m > $(ROOT)/build/test_subtitle_position.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m"; cat $(ROOT)/build/test_subtitle_position.$$m.log; exit 1; fi; \
	done
	@echo "test_subtitle_position: $(words $(SUBPOS_MUTANTS)) mutants FAIL as required"
	@touch $@

unit-unlocked unit-host-unlocked: $(ROOT)/build/test_subtitle_position.ok

# E9 per-tube overscan. Unset stays the fixed title-safe. ASan plus one binary per mutant.
OVERSCAN_SRC := $(ROOT)/tests/unit/test_overscan_safe.cpp \
		$(ROOT)/host/libmisterplex/overscan_safe.hpp \
		$(ROOT)/host/libmisterplex/owenb_osd_draw.hpp \
		$(ROOT)/host/libmisterplex/owenb_osd_layout.hpp \
		$(ROOT)/host/libmisterplex/owenb_menu.hpp \
		$(ROOT)/host/libmisterplex/gcast_osd.hpp \
		$(ROOT)/host/libmisterplex/plexui/plexui_render.hpp \
		$(ROOT)/arm/misterplexd/main.cpp \
		$(ROOT)/arm/misterplexd/media_player.cpp \
		$(ROOT)/assets/misterplex.conf.example \
		$(OWENB_RING_OBJS)
OVERSCAN_FLAGS := $(CXXFLAGS) $(OWENB_SESS_FLAGS) -I$(ROOT)/host
OVERSCAN_MUTANTS := MUT_OVERSCAN_IGNORED MUT_OVERSCAN_UNSET_SHIFTS MUT_OVERSCAN_NO_SAVE \
		MUT_OVERSCAN_SAVE_ON_CANCEL MUT_OVERSCAN_PLEX_IGNORES

$(ROOT)/build/test_overscan_safe: $(OVERSCAN_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(OVERSCAN_FLAGS) -o $@ $(ROOT)/tests/unit/test_overscan_safe.cpp $(OWENB_RING_OBJS) -lm

$(ROOT)/build/test_overscan_safe_asan: $(OVERSCAN_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(OVERSCAN_FLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -o $@ \
		$(ROOT)/tests/unit/test_overscan_safe.cpp $(OWENB_RING_OBJS) -lm

$(ROOT)/build/test_overscan_safe.ok: $(ROOT)/build/test_overscan_safe $(ROOT)/build/test_overscan_safe_asan
	$(ROOT)/build/test_overscan_safe
	ASAN_OPTIONS=detect_leaks=1 $(ROOT)/build/test_overscan_safe_asan
	@for m in $(OVERSCAN_MUTANTS); do \
		$(CXX) $(OVERSCAN_FLAGS) -D$$m -o $(ROOT)/build/test_overscan_safe.$$m \
			$(ROOT)/tests/unit/test_overscan_safe.cpp $(OWENB_RING_OBJS) -lm || { echo "MUTANT BUILD $$m"; exit 1; }; \
		if $(ROOT)/build/test_overscan_safe.$$m > $(ROOT)/build/test_overscan_safe.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m"; cat $(ROOT)/build/test_overscan_safe.$$m.log; exit 1; fi; \
	done
	@echo "test_overscan_safe: $(words $(OVERSCAN_MUTANTS)) mutants FAIL as required"
	@touch $@

unit-unlocked unit-host-unlocked: $(ROOT)/build/test_overscan_safe.ok

# SOFT_SUBS default off. Host routing plus, when libav is present, a generated text fixture.
SOFTSUBS_SRC := $(ROOT)/tests/unit/test_soft_subs.cpp \
		$(ROOT)/host/libmisterplex/soft_subs.hpp \
		$(ROOT)/host/libmisterplex/subtitle_cues.hpp \
		$(ROOT)/host/libmisterplex/subtitle_sink.hpp \
		$(ROOT)/host/libmisterplex/track_select.hpp \
		$(ROOT)/host/libmisterplex/forced_subs.hpp \
		$(ROOT)/arm/misterplexd/main.cpp \
		$(ROOT)/arm/misterplexd/media_player.cpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/arm/misterplexd/owenb_av_source.cpp \
		$(ROOT)/assets/misterplex.conf.example
SOFTSUBS_FLAGS := $(CXXFLAGS) -I$(ROOT)/host -pthread
SOFTSUBS_MUTANTS := MUT_SOFTSUBS_DEFAULT_ON MUT_SOFTSUBS_PARSE_SWAP MUT_SOFTSUBS_OFF_OVERLAY MUT_LOCAL_SUB_STREAMID_OFF MUT_LIVE_SUB_OFF_IGNORED

$(ROOT)/build/test_soft_subs: $(SOFTSUBS_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(SOFTSUBS_FLAGS) -o $@ $(ROOT)/tests/unit/test_soft_subs.cpp

$(ROOT)/build/test_soft_subs_asan: $(SOFTSUBS_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(SOFTSUBS_FLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -o $@ \
		$(ROOT)/tests/unit/test_soft_subs.cpp

$(ROOT)/build/test_soft_subs.ok: $(ROOT)/build/test_soft_subs $(ROOT)/build/test_soft_subs_asan
	$(ROOT)/build/test_soft_subs
	ASAN_OPTIONS=detect_leaks=1 $(ROOT)/build/test_soft_subs_asan
	@for m in $(SOFTSUBS_MUTANTS); do \
		$(CXX) $(SOFTSUBS_FLAGS) -D$$m -o $(ROOT)/build/test_soft_subs.$$m \
			$(ROOT)/tests/unit/test_soft_subs.cpp || { echo "MUTANT BUILD $$m"; exit 1; }; \
		if $(ROOT)/build/test_soft_subs.$$m > $(ROOT)/build/test_soft_subs.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m"; cat $(ROOT)/build/test_soft_subs.$$m.log; exit 1; fi; \
	done
	@echo "test_soft_subs: $(words $(SOFTSUBS_MUTANTS)) mutants FAIL as required"
	@touch $@

# #258: embedded SRT overlay fallback when the PMS stream fetch fails (501). Never burns.
EMBSRT_SRC := $(ROOT)/tests/unit/test_embedded_srt_fallback.cpp \
		$(ROOT)/host/libmisterplex/embedded_srt_fallback.hpp \
		$(ROOT)/host/libmisterplex/soft_subs.hpp \
		$(ROOT)/host/libmisterplex/subtitle_sink.hpp \
		$(ROOT)/host/libmisterplex/osd_subtitle_sink.hpp \
		$(ROOT)/host/libmisterplex/track_select.hpp
EMBSRT_FLAGS := $(CXXFLAGS) -I$(ROOT)/host -I$(ROOT)/third_party/owenb-h264ring/include -pthread
EMBSRT_MUTANTS := MUT_258_NO_FALLBACK MUT_258_BURN_SRT MUT_258_DOUBLE_PAINT MUT_258_DECODE_GATE MUT_258_EXTERNAL_ELIGIBLE MUT_258_OPTIN_IGNORED MUT_FU8_TOKEN_ARGV MUT_FU8_KEEP_RANGE

$(ROOT)/build/test_embedded_srt_fallback: $(EMBSRT_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(EMBSRT_FLAGS) -o $@ $(ROOT)/tests/unit/test_embedded_srt_fallback.cpp

$(ROOT)/build/test_embedded_srt_fallback_asan: $(EMBSRT_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(EMBSRT_FLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -o $@ \
		$(ROOT)/tests/unit/test_embedded_srt_fallback.cpp

$(ROOT)/build/test_embedded_srt_fallback.ok: $(ROOT)/build/test_embedded_srt_fallback $(ROOT)/build/test_embedded_srt_fallback_asan
	$(ROOT)/build/test_embedded_srt_fallback
	ASAN_OPTIONS=detect_leaks=1 $(ROOT)/build/test_embedded_srt_fallback_asan
	@for m in $(EMBSRT_MUTANTS); do \
		$(CXX) $(EMBSRT_FLAGS) -D$$m -o $(ROOT)/build/test_embedded_srt_fallback.$$m \
			$(ROOT)/tests/unit/test_embedded_srt_fallback.cpp || { echo "MUTANT BUILD $$m"; exit 1; }; \
		if $(ROOT)/build/test_embedded_srt_fallback.$$m > $(ROOT)/build/test_embedded_srt_fallback.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m"; cat $(ROOT)/build/test_embedded_srt_fallback.$$m.log; exit 1; fi; \
	done
	@echo "test_embedded_srt_fallback: $(words $(EMBSRT_MUTANTS)) mutants FAIL as required"
	@touch $@

unit-unlocked unit-host-unlocked: $(ROOT)/build/test_embedded_srt_fallback.ok

# #260: SOFT_SUBS off, embedded mov_text overlays via the packet decoder. Never burns.
MOV260_SRC := $(ROOT)/tests/unit/test_movtext_overlay_260.cpp \
		$(ROOT)/host/libmisterplex/soft_subs.hpp \
		$(ROOT)/host/libmisterplex/subtitle_sink.hpp \
		$(ROOT)/host/libmisterplex/track_select.hpp \
		$(ROOT)/host/libmisterplex/remember_tracks.hpp \
		$(ROOT)/arm/misterplexd/media_player.cpp \
		$(ROOT)/arm/misterplexd/owenb_av_source.cpp
MOV260_FLAGS := $(CXXFLAGS) -I$(ROOT)/host -I$(ROOT)/third_party/owenb-h264ring/include
MOV260_MUTANTS := MUT_260_MOV_STILL_BURN

$(ROOT)/build/test_movtext_overlay_260: $(MOV260_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(MOV260_FLAGS) -o $@ $(ROOT)/tests/unit/test_movtext_overlay_260.cpp

$(ROOT)/build/test_movtext_overlay_260_asan: $(MOV260_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(MOV260_FLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -o $@ \
		$(ROOT)/tests/unit/test_movtext_overlay_260.cpp

$(ROOT)/build/test_movtext_overlay_260.ok: $(ROOT)/build/test_movtext_overlay_260 $(ROOT)/build/test_movtext_overlay_260_asan
	$(ROOT)/build/test_movtext_overlay_260
	ASAN_OPTIONS=detect_leaks=1 $(ROOT)/build/test_movtext_overlay_260_asan
	@for m in $(MOV260_MUTANTS); do \
		$(CXX) $(MOV260_FLAGS) -D$$m -o $(ROOT)/build/test_movtext_overlay_260.$$m \
			$(ROOT)/tests/unit/test_movtext_overlay_260.cpp || { echo "MUTANT BUILD $$m"; exit 1; }; \
		if $(ROOT)/build/test_movtext_overlay_260.$$m > $(ROOT)/build/test_movtext_overlay_260.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m"; cat $(ROOT)/build/test_movtext_overlay_260.$$m.log; exit 1; fi; \
	done
	@echo "test_movtext_overlay_260: $(words $(MOV260_MUTANTS)) mutants FAIL as required"
	@touch $@

unit-unlocked unit-host-unlocked: $(ROOT)/build/test_movtext_overlay_260.ok

SOFTSUBS_AV_SRC := $(ROOT)/tests/unit/test_soft_subs_av.cpp \
		$(ROOT)/arm/misterplexd/owenb_av_source.cpp $(ROOT)/arm/misterplexd/owenb_av_source.hpp \
		$(ROOT)/host/libmisterplex/soft_subs.hpp $(ROOT)/host/libmisterplex/subtitle_cues.hpp \
		$(ROOT)/host/libmisterplex/owenb_ring.cpp $(OWENB_RING_OBJS)
SOFTSUBS_AV_FLAGS := $(CXXFLAGS) $(OWENB_SESS_FLAGS) -DMPX_HAVE_LIBAV -DMPX_SOURCE_ROOT='"$(ROOT)"' \
		-I$(ROOT)/arm/misterplexd -I$(ROOT)/host $(shell pkg-config --cflags $(L2_DEMUX_AV))
SOFTSUBS_AV_LIBS := $(ROOT)/arm/misterplexd/owenb_av_source.cpp $(ROOT)/host/libmisterplex/owenb_ring.cpp \
		$(OWENB_RING_OBJS) $(shell pkg-config --libs $(L2_DEMUX_AV)) -lm

$(ROOT)/build/test_soft_subs_av: $(SOFTSUBS_AV_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(SOFTSUBS_AV_FLAGS) -o $@ $(ROOT)/tests/unit/test_soft_subs_av.cpp $(SOFTSUBS_AV_LIBS)

$(ROOT)/build/test_soft_subs_av_asan: $(SOFTSUBS_AV_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(PLEXUI_ASAN_CXXFLAGS) $(OWENB_SESS_FLAGS) -DMPX_HAVE_LIBAV -DMPX_SOURCE_ROOT='"$(ROOT)"' \
		-I$(ROOT)/arm/misterplexd -I$(ROOT)/host $(shell pkg-config --cflags $(L2_DEMUX_AV)) -o $@ \
		$(ROOT)/tests/unit/test_soft_subs_av.cpp $(SOFTSUBS_AV_LIBS)

$(ROOT)/build/test_soft_subs_av.ok: $(ROOT)/build/test_soft_subs_av $(ROOT)/build/test_soft_subs_av_asan
	$(ROOT)/build/test_soft_subs_av
	ASAN_OPTIONS=detect_leaks=1 $(ROOT)/build/test_soft_subs_av_asan
	$(CXX) $(SOFTSUBS_AV_FLAGS) -DMUT_SOFTSUBS_DISCARD -o $(ROOT)/build/test_soft_subs_av.MUT_SOFTSUBS_DISCARD \
		$(ROOT)/tests/unit/test_soft_subs_av.cpp $(SOFTSUBS_AV_LIBS)
	@if $(ROOT)/build/test_soft_subs_av.MUT_SOFTSUBS_DISCARD > $(ROOT)/build/test_soft_subs_av.MUT_SOFTSUBS_DISCARD.log 2>&1; then \
		echo "MUTANT SURVIVED: MUT_SOFTSUBS_DISCARD"; cat $(ROOT)/build/test_soft_subs_av.MUT_SOFTSUBS_DISCARD.log; exit 1; fi
	@echo "test_soft_subs_av: mutant MUT_SOFTSUBS_DISCARD FAILS as required"
	@touch $@

unit-unlocked unit-host-unlocked: $(ROOT)/build/test_soft_subs.ok \
	$(if $(OWENB_AV_E2E_OK),$(ROOT)/build/test_soft_subs_av.ok)

# #334: embedded srt picks read the original Part (token header only), async, not
# /library/streams/<id> (PMS 501). Pure test + libav e2e against an in-test fake PMS.
PT334_SRC := $(ROOT)/tests/unit/test_334_part_text.cpp $(ROOT)/host/libmisterplex/embedded_text_part.hpp $(ROOT)/host/libmisterplex/subtitle_sink.hpp \
		$(ROOT)/host/libmisterplex/track_select.hpp
PT334_FLAGS := $(CXXFLAGS) -I$(ROOT)/host -pthread
PT334_MUTANTS := MUT_334_STREAMS_KEY_FOR_EMBEDDED MUT_334_NO_TOKEN_HEADER MUT_334_NO_SIZE_CAP MUT_334_PAINT_REWRITES_PART

$(ROOT)/build/test_334_part_text: $(PT334_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(PT334_FLAGS) -o $@ $(ROOT)/tests/unit/test_334_part_text.cpp

$(ROOT)/build/test_334_part_text_asan: $(PT334_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(PT334_FLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -o $@ \
		$(ROOT)/tests/unit/test_334_part_text.cpp

$(ROOT)/build/test_334_part_text.ok: $(ROOT)/build/test_334_part_text $(ROOT)/build/test_334_part_text_asan
	$(ROOT)/build/test_334_part_text
	ASAN_OPTIONS=detect_leaks=1 $(ROOT)/build/test_334_part_text_asan
	@for m in $(PT334_MUTANTS); do \
		$(CXX) $(PT334_FLAGS) -D$$m -o $(ROOT)/build/test_334_part_text.$$m \
			$(ROOT)/tests/unit/test_334_part_text.cpp || { echo "MUTANT BUILD $$m"; exit 1; }; \
		if $(ROOT)/build/test_334_part_text.$$m > $(ROOT)/build/test_334_part_text.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m"; cat $(ROOT)/build/test_334_part_text.$$m.log; exit 1; fi; \
	done
	@echo "test_334_part_text: $(words $(PT334_MUTANTS)) mutants FAIL as required"
	@touch $@

unit-unlocked unit-host-unlocked: $(ROOT)/build/test_334_part_text.ok

# #334 glass cbdbfd2 crit 4: URL/argv log lines redact the token (logUrl).
LU334_FLAGS := $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host
LU334_MUTANTS := MUT_334_LOG_URL_RAW MUT_334_REDACT_CASE MUT_334_TOKEN_KEY_CASE MUT_352_LOG_PART_RAW
$(ROOT)/build/test_334_log_url.ok: $(ROOT)/tests/unit/test_334_log_url.cpp $(ROOT)/arm/misterplexd/log_redact.hpp $(ROOT)/tests/unit/test_log_token_pins.py
	@mkdir -p $(ROOT)/build
	$(CXX) $(LU334_FLAGS) -o $(ROOT)/build/test_334_log_url $<
	$(CXX) $(LU334_FLAGS) -g -fsanitize=address,undefined -fno-omit-frame-pointer -o $(ROOT)/build/test_334_log_url_asan $<
	$(ROOT)/build/test_334_log_url
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 $(ROOT)/build/test_334_log_url_asan
	@for m in $(LU334_MUTANTS); do \
		$(CXX) $(LU334_FLAGS) -D$$m -o $(ROOT)/build/test_334_log_url.$$m $< || { echo "MUTANT BUILD $$m"; exit 1; }; \
		if $(ROOT)/build/test_334_log_url.$$m > $(ROOT)/build/test_334_log_url.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
	done
	python3 $(ROOT)/tests/unit/test_log_token_pins.py
	@for m in 1 2 3 4 5; do \
		if MPX_LOG_PIN_MUTANT=$$m python3 $(ROOT)/tests/unit/test_log_token_pins.py >/dev/null; then \
			echo "MUTANT SURVIVED: MPX_LOG_PIN_MUTANT=$$m"; exit 1; fi; \
	done
	@echo "test_334_log_url: $(LU334_MUTANTS) + MPX_LOG_PIN_MUTANT=1..5 FAIL as required"
	@touch $@

unit-unlocked unit-host-unlocked: $(ROOT)/build/test_334_log_url.ok

PT334AV_SRC := $(ROOT)/tests/unit/test_334_part_text_av.cpp $(ROOT)/host/libmisterplex/embedded_text_part.hpp \
		$(ROOT)/arm/misterplexd/owenb_av_source.cpp $(ROOT)/arm/misterplexd/owenb_av_source.hpp \
		$(ROOT)/host/libmisterplex/owenb_ring.cpp $(OWENB_RING_OBJS)
PT334AV_CF := $(OWENB_SESS_FLAGS) -DMPX_HAVE_LIBAV -I$(ROOT)/arm/misterplexd -I$(ROOT)/host \
		$(shell pkg-config --cflags $(L2_DEMUX_AV))
PT334AV_MUTANTS := MUT_334_NO_TOKEN_HEADER MUT_334_FOLLOW_REDIRECTS MUT_334_NO_DEADLINE
PT334AV_LIBS := $(ROOT)/arm/misterplexd/owenb_av_source.cpp $(ROOT)/host/libmisterplex/owenb_ring.cpp \
		$(OWENB_RING_OBJS) $(shell pkg-config --libs $(L2_DEMUX_AV)) -lm

$(ROOT)/build/test_334_part_text_av: $(PT334AV_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(PT334AV_CF) -o $@ $(ROOT)/tests/unit/test_334_part_text_av.cpp $(PT334AV_LIBS)

$(ROOT)/build/test_334_part_text_av_asan: $(PT334AV_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(PLEXUI_ASAN_CXXFLAGS) $(PT334AV_CF) -o $@ $(ROOT)/tests/unit/test_334_part_text_av.cpp $(PT334AV_LIBS)

$(ROOT)/build/test_334_part_text_av.ok: $(ROOT)/build/test_334_part_text_av $(ROOT)/build/test_334_part_text_av_asan
	$(ROOT)/build/test_334_part_text_av
	ASAN_OPTIONS=detect_leaks=1 $(ROOT)/build/test_334_part_text_av_asan
	@for m in $(PT334AV_MUTANTS); do \
		$(CXX) $(CXXFLAGS) $(PT334AV_CF) -D$$m -o $(ROOT)/build/test_334_part_text_av.$$m \
			$(ROOT)/tests/unit/test_334_part_text_av.cpp $(PT334AV_LIBS) || { echo "MUTANT BUILD $$m"; exit 1; }; \
		if $(ROOT)/build/test_334_part_text_av.$$m > $(ROOT)/build/test_334_part_text_av.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m (av)"; cat $(ROOT)/build/test_334_part_text_av.$$m.log; exit 1; fi; \
	done
	@echo "test_334_part_text_av: $(words $(PT334AV_MUTANTS)) mutants FAIL as required"
	@touch $@

unit-unlocked unit-host-unlocked: $(if $(OWENB_AV_E2E_OK),$(ROOT)/build/test_334_part_text_av.ok)

# Embedded SRT/ASS fallback: PMS index 3 may not be the demuxer's text stream.
EMBED_TEXT_MAP_SRC := $(ROOT)/tests/unit/test_embedded_text_map.cpp \
		$(ROOT)/arm/misterplexd/owenb_av_source.cpp $(ROOT)/host/libmisterplex/embedded_text_map.hpp \
		$(ROOT)/host/libmisterplex/owenb_ring.cpp $(OWENB_RING_OBJS)
EMBED_TEXT_MAP_FLAGS := $(CXXFLAGS) $(OWENB_SESS_FLAGS) -DMPX_HAVE_LIBAV -DMPX_SOURCE_ROOT='"$(ROOT)"' \
		-I$(ROOT)/arm/misterplexd -I$(ROOT)/host $(shell pkg-config --cflags $(L2_DEMUX_AV))
EMBED_TEXT_MAP_LIBS := $(ROOT)/arm/misterplexd/owenb_av_source.cpp $(ROOT)/host/libmisterplex/owenb_ring.cpp \
		$(OWENB_RING_OBJS) $(shell pkg-config --libs $(L2_DEMUX_AV)) -lm

$(ROOT)/build/test_embedded_text_map: $(EMBED_TEXT_MAP_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(EMBED_TEXT_MAP_FLAGS) -o $@ $(ROOT)/tests/unit/test_embedded_text_map.cpp $(EMBED_TEXT_MAP_LIBS)

$(ROOT)/build/test_embedded_text_map_asan: $(EMBED_TEXT_MAP_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(PLEXUI_ASAN_CXXFLAGS) $(OWENB_SESS_FLAGS) -DMPX_HAVE_LIBAV -DMPX_SOURCE_ROOT='"$(ROOT)"' \
		-I$(ROOT)/arm/misterplexd -I$(ROOT)/host $(shell pkg-config --cflags $(L2_DEMUX_AV)) -o $@ \
		$(ROOT)/tests/unit/test_embedded_text_map.cpp $(EMBED_TEXT_MAP_LIBS)

$(ROOT)/build/test_embedded_text_map.ok: $(ROOT)/build/test_embedded_text_map $(ROOT)/build/test_embedded_text_map_asan
	$(ROOT)/build/test_embedded_text_map
	ASAN_OPTIONS=detect_leaks=1 $(ROOT)/build/test_embedded_text_map_asan
	$(CXX) $(EMBED_TEXT_MAP_FLAGS) -DMUT_SOFT_SUB_RAW_INDEX -o $(ROOT)/build/test_embedded_text_map.MUT_SOFT_SUB_RAW_INDEX \
		$(ROOT)/tests/unit/test_embedded_text_map.cpp $(EMBED_TEXT_MAP_LIBS)
	@if $(ROOT)/build/test_embedded_text_map.MUT_SOFT_SUB_RAW_INDEX > $(ROOT)/build/test_embedded_text_map.MUT_SOFT_SUB_RAW_INDEX.log 2>&1; then \
		echo "MUTANT SURVIVED: MUT_SOFT_SUB_RAW_INDEX"; cat $(ROOT)/build/test_embedded_text_map.MUT_SOFT_SUB_RAW_INDEX.log; exit 1; fi
	@grep -q "index mismatch applies the demuxed text track" $(ROOT)/build/test_embedded_text_map.MUT_SOFT_SUB_RAW_INDEX.log
	@echo "test_embedded_text_map: MUT_SOFT_SUB_RAW_INDEX FAIL as required"
	$(CXX) $(EMBED_TEXT_MAP_FLAGS) -DMUT_SOFT_SUB_NO_LANG_GATE -o $(ROOT)/build/test_embedded_text_map.MUT_SOFT_SUB_NO_LANG_GATE \
		$(ROOT)/tests/unit/test_embedded_text_map.cpp $(EMBED_TEXT_MAP_LIBS)
	@if $(ROOT)/build/test_embedded_text_map.MUT_SOFT_SUB_NO_LANG_GATE > $(ROOT)/build/test_embedded_text_map.MUT_SOFT_SUB_NO_LANG_GATE.log 2>&1; then \
		echo "MUTANT SURVIVED: MUT_SOFT_SUB_NO_LANG_GATE"; cat $(ROOT)/build/test_embedded_text_map.MUT_SOFT_SUB_NO_LANG_GATE.log; exit 1; fi
	@grep -q "language gate does not apply a different language" $(ROOT)/build/test_embedded_text_map.MUT_SOFT_SUB_NO_LANG_GATE.log
	@echo "test_embedded_text_map: MUT_SOFT_SUB_NO_LANG_GATE FAIL as required"
	@touch $@

unit-unlocked unit-host-unlocked: $(if $(OWENB_AV_E2E_OK),$(ROOT)/build/test_embedded_text_map.ok)

# OpenSubtitles.com on-device fallback (parse / cache / flag). No network.
OPENSUBS_SRC := $(ROOT)/tests/unit/test_opensubs_fallback.cpp \
		$(ROOT)/host/libmisterplex/opensubs_fallback.hpp \
		$(ROOT)/host/libmisterplex/track_select.hpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp
$(ROOT)/build/test_opensubs_fallback: $(OPENSUBS_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/host -o $@ $(ROOT)/tests/unit/test_opensubs_fallback.cpp
$(ROOT)/build/test_opensubs_fallback_asan: $(OPENSUBS_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -I$(ROOT)/host -o $@ \
		$(ROOT)/tests/unit/test_opensubs_fallback.cpp
$(ROOT)/build/test_opensubs_fallback.ok: $(ROOT)/build/test_opensubs_fallback $(ROOT)/build/test_opensubs_fallback_asan
	$(ROOT)/build/test_opensubs_fallback
	ASAN_OPTIONS=detect_leaks=1 $(ROOT)/build/test_opensubs_fallback_asan
	$(CXX) $(CXXFLAGS) -I$(ROOT)/host -DMUT_OPENSUBS_FALLBACK_OFF -o $(ROOT)/build/test_opensubs_fallback.MUT \
		$(ROOT)/tests/unit/test_opensubs_fallback.cpp
	@if $(ROOT)/build/test_opensubs_fallback.MUT > $(ROOT)/build/test_opensubs_fallback.MUT.log 2>&1; then \
		echo "MUTANT SURVIVED: MUT_OPENSUBS_FALLBACK_OFF"; cat $(ROOT)/build/test_opensubs_fallback.MUT.log; exit 1; fi
	@echo "test_opensubs_fallback: mutant MUT_OPENSUBS_FALLBACK_OFF FAILS as required"
	$(CXX) $(CXXFLAGS) -I$(ROOT)/host -DMUT_OPENSUBS_IGNORE_TEXT_SUB -o $(ROOT)/build/test_opensubs_fallback.MUT2 \
		$(ROOT)/tests/unit/test_opensubs_fallback.cpp
	@if $(ROOT)/build/test_opensubs_fallback.MUT2 > $(ROOT)/build/test_opensubs_fallback.MUT2.log 2>&1; then \
		echo "MUTANT SURVIVED: MUT_OPENSUBS_IGNORE_TEXT_SUB"; cat $(ROOT)/build/test_opensubs_fallback.MUT2.log; exit 1; fi
	@echo "test_opensubs_fallback: mutant MUT_OPENSUBS_IGNORE_TEXT_SUB FAILS as required"
	$(CXX) $(CXXFLAGS) -I$(ROOT)/host -DMUT_OPENSUBS_KEY_ON_ARGV -o $(ROOT)/build/test_opensubs_fallback.MUT3 \
		$(ROOT)/tests/unit/test_opensubs_fallback.cpp
	@if $(ROOT)/build/test_opensubs_fallback.MUT3 > $(ROOT)/build/test_opensubs_fallback.MUT3.log 2>&1; then \
		echo "MUTANT SURVIVED: MUT_OPENSUBS_KEY_ON_ARGV"; cat $(ROOT)/build/test_opensubs_fallback.MUT3.log; exit 1; fi
	@echo "test_opensubs_fallback: mutant MUT_OPENSUBS_KEY_ON_ARGV FAILS as required"
	$(CXX) $(CXXFLAGS) -I$(ROOT)/host -DMUT_OPENSUBS_EVICT_NEWEST -o $(ROOT)/build/test_opensubs_fallback.MUT4 \
		$(ROOT)/tests/unit/test_opensubs_fallback.cpp
	@if $(ROOT)/build/test_opensubs_fallback.MUT4 > $(ROOT)/build/test_opensubs_fallback.MUT4.log 2>&1; then \
		echo "MUTANT SURVIVED: MUT_OPENSUBS_EVICT_NEWEST"; cat $(ROOT)/build/test_opensubs_fallback.MUT4.log; exit 1; fi
	@echo "test_opensubs_fallback: mutant MUT_OPENSUBS_EVICT_NEWEST FAILS as required"
	@awk '/^std::string opensubsHttp\(/{on=1} on{print; if(/^}/){exit}}' $(ROOT)/arm/misterplexd/plex_resolve.cpp | \
		grep -q mkstemp && { echo "opensubsHttp must not use mkstemp temp -K files"; exit 1; }; \
		awk '/^std::string opensubsHttp\(/{on=1} on{print; if(/^}/){exit}}' $(ROOT)/arm/misterplexd/plex_resolve.cpp | \
		grep -qE 'appendHeaderArgv|"-K"' || { echo "opensubsHttp must pass headers via -K (stdin/fd)"; exit 1; }
	@echo "test_opensubs_fallback: opensubsHttp headers via -K stdin/fd (no secrets on argv, no temp file)"
	@awk '/^std::string tryOpenSubsFallback/{f=1} f&&/shouldQueryOpenSubs/{q=NR} f&&/opensubsHttp|cacheReadable/{if(!q){print "opensubs wiring: query before shouldQueryOpenSubs"; exit 1} exit 0}' \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp
	@echo "test_opensubs_fallback: tryOpenSubsFallback gates cache+network on shouldQueryOpenSubs"
	@awk '/^std::string tryOpenSubsFallback\(/ { on=1; d=0 } on { print; for (i=1; i<=length($$0); i++) { c=substr($$0,i,1); if (c=="{") d++; if (c=="}") d-- } if (d==0) exit }' \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp | grep -q '/library/' && \
		{ echo "opensubs: tryOpenSubsFallback must not reference /library/"; exit 1; }; \
		awk '/^std::string tryOpenSubsFallback\(/ { on=1; d=0 } on { print; for (i=1; i<=length($$0); i++) { c=substr($$0,i,1); if (c=="{") d++; if (c=="}") d-- } if (d==0) exit }' \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp | grep -qE 'setStreams|plexHttp.*PUT|"PUT".*/library/' && \
		{ echo "opensubs: tryOpenSubsFallback must not PMS PUT/setStreams"; exit 1; }; \
		grep -qE '/library/|setStreams|plexHttp.*PUT|"PUT".*/library/' $(ROOT)/host/libmisterplex/opensubs_fallback.hpp && \
		{ echo "opensubs_fallback.hpp: must not PMS write paths"; exit 1; }; \
		echo "test_opensubs_fallback: no Plex write paths in opensubs sources"
	@touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_opensubs_fallback.ok

$(ROOT)/build/test_track_select_asan.check: $(ROOT)/build/test_track_select_asan
	ASAN_OPTIONS=detect_leaks=1 $(ROOT)/build/test_track_select_asan
	@touch $@

$(ROOT)/build/test_track_select_av: $(ROOT)/tests/unit/test_track_select_av.cpp \
		$(ROOT)/arm/misterplexd/owenb_av_source.cpp $(ROOT)/arm/misterplexd/owenb_av_source.hpp \
		$(ROOT)/host/libmisterplex/track_select.hpp $(ROOT)/host/libmisterplex/owenb_ring.cpp $(OWENB_RING_OBJS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -DMPX_HAVE_LIBAV -I$(ROOT)/arm/misterplexd -I$(ROOT)/host \
		$(shell pkg-config --cflags $(L2_DEMUX_AV)) -o $@ $(ROOT)/tests/unit/test_track_select_av.cpp \
		$(ROOT)/arm/misterplexd/owenb_av_source.cpp $(ROOT)/host/libmisterplex/owenb_ring.cpp $(OWENB_RING_OBJS) \
		$(shell pkg-config --libs $(L2_DEMUX_AV)) -lm

$(ROOT)/build/test_track_select_av_asan: $(ROOT)/tests/unit/test_track_select_av.cpp \
		$(ROOT)/arm/misterplexd/owenb_av_source.cpp $(ROOT)/arm/misterplexd/owenb_av_source.hpp \
		$(ROOT)/host/libmisterplex/track_select.hpp $(ROOT)/host/libmisterplex/owenb_ring.cpp $(OWENB_RING_OBJS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(PLEXUI_ASAN_CXXFLAGS) $(OWENB_SESS_FLAGS) -DMPX_HAVE_LIBAV -I$(ROOT)/arm/misterplexd -I$(ROOT)/host \
		$(shell pkg-config --cflags $(L2_DEMUX_AV)) -o $@ $(ROOT)/tests/unit/test_track_select_av.cpp \
		$(ROOT)/arm/misterplexd/owenb_av_source.cpp $(ROOT)/host/libmisterplex/owenb_ring.cpp $(OWENB_RING_OBJS) \
		$(shell pkg-config --libs $(L2_DEMUX_AV)) -lm

$(ROOT)/build/test_track_select_av_asan.check: $(ROOT)/build/test_track_select_av_asan \
		$(ROOT)/tests/unit/test_track_select_av.cpp $(ROOT)/arm/misterplexd/owenb_av_source.cpp
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		$(ROOT)/build/test_track_select_av_asan
	$(CXX) $(PLEXUI_ASAN_CXXFLAGS) $(OWENB_SESS_FLAGS) -DMPX_HAVE_LIBAV -DMUT_SUB_OFF_DROPS_AUDIO \
		-I$(ROOT)/arm/misterplexd -I$(ROOT)/host \
		$(shell pkg-config --cflags $(L2_DEMUX_AV)) -o $@.mut $(ROOT)/tests/unit/test_track_select_av.cpp \
		$(ROOT)/arm/misterplexd/owenb_av_source.cpp $(ROOT)/host/libmisterplex/owenb_ring.cpp $(OWENB_RING_OBJS) \
		$(shell pkg-config --libs $(L2_DEMUX_AV)) -lm
	@if ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		$@.mut > $@.mut.log 2>&1; then echo "MUTANT SURVIVED: MUT_SUB_OFF_DROPS_AUDIO"; exit 1; fi
	@grep -q "subtitle off does not drop the audio decoder" $@.mut.log || { \
		echo "MUTANT MUT_SUB_OFF_DROPS_AUDIO killed by the wrong check"; cat $@.mut.log; exit 1; }
	@echo "test_track_select_av: MUT_SUB_OFF_DROPS_AUDIO FAIL as required"
	@touch $@

$(ROOT)/build/test_title_direct_policy: $(ROOT)/tests/unit/test_title_direct_policy.cpp \
		$(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/arm/misterplexd/companion.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -o $@ \
		$(ROOT)/tests/unit/test_title_direct_policy.cpp $(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/host/libmisterplex/plexui/plexui_http.cpp
$(ROOT)/build/test_title_direct_policy.ok: $(ROOT)/build/test_title_direct_policy
	@$(ROOT)/build/test_title_direct_policy > $@.log 2>&1 || { cat $@.log; exit 1; }
	@cat $@.log
	@cp $@.log $@

unit-unlocked unit-host-unlocked: $(ROOT)/build/test_companion_letterbox_bars
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_companion_live_clock
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_track_select $(ROOT)/build/test_track_select_asan.check
# #340: each mutant puts X-Plex-Token= back into an outgoing URL; the suite must FAIL.
$(ROOT)/build/test_track_select_340_mutants.ok: $(ROOT)/build/test_track_select
	@for M in MUT_340_PUT_TOKEN_QUERY MUT_340_META_QUERY; do \
		$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -D$$M -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -o $(ROOT)/build/test_track_select.$$M \
			$(ROOT)/tests/unit/test_track_select.cpp $(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp || exit 1; \
		if $(ROOT)/build/test_track_select.$$M >/dev/null 2>&1; then echo "test_track_select: $$M SURVIVED"; exit 1; fi; \
		echo "test_track_select: $$M FAIL as required"; rm -f $(ROOT)/build/test_track_select.$$M; \
	done
	@touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_track_select_340_mutants.ok
$(ROOT)/build/test_track_select_g7_mutants.ok: $(ROOT)/build/test_track_select
	@for M in MUT_G7_IGNORE_LANG; do \
		$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -D$$M -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -o $(ROOT)/build/test_track_select.$$M \
			$(ROOT)/tests/unit/test_track_select.cpp $(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp || exit 1; \
		if $(ROOT)/build/test_track_select.$$M >/dev/null 2>&1; then echo "test_track_select: $$M SURVIVED"; exit 1; fi; \
		echo "test_track_select: $$M FAIL as required"; rm -f $(ROOT)/build/test_track_select.$$M; \
	done
	@touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_track_select_g7_mutants.ok
unit-unlocked unit-host-unlocked: $(if $(OWENB_AV_E2E_OK),$(ROOT)/build/test_track_select_av $(ROOT)/build/test_track_select_av_asan.check)
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_srt_cues $(ROOT)/build/test_ass_to_text \
	$(ROOT)/build/test_subtitle_sink_osd $(ROOT)/build/test_caption_layout \
	$(ROOT)/build/test_caption_clock $(ROOT)/build/test_plexui_subtitle_picker \
	$(ROOT)/build/test_plexui_audio_picker
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_reviewer_present_gate.ok $(ROOT)/build/test_present_retry.ok $(ROOT)/build/test_plexui_home.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_title_direct_policy $(ROOT)/build/test_title_direct_policy.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_companion_480i_field_rate
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_pts_jump_clamp
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_ttfp_stamps
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_pts_wrap
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_audio_lead_clamp
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_audio_queue_bound_240p
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_sps_reject
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_bitrate_floor_480i
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_companion_title_policy $(ROOT)/build/test_no_synthetic_source
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plex_tv_link
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_480i_active_height
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_core_id_select
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_audio_buf_prefill

unit-unlocked unit-host-unlocked: $(ROOT)/build/test_companion_natural_eof
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_companion_reqlog $(ROOT)/build/test_companion_reqlog.mutant-check
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_input_close_eof \
	$(ROOT)/build/test_http_range_resume $(ROOT)/build/test_http_range_resume.mutant-check \
	$(ROOT)/build/test_http_range_resume_asan.check
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_fpga_av_trace $(ROOT)/build/test_fpga_av_trace_off
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_library_browser $(ROOT)/build/test_browse_v2 \
		$(ROOT)/build/test_browse_library_list

# Companion request latency log (FPGA Bug 03:24, reviewer 03:25): rows + mutants that must FAIL.
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
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -o $@ \
		$(ROOT)/tests/unit/test_companion_natural_eof.cpp $(ROOT)/arm/misterplexd/companion.cpp \
		$(ROOT)/arm/misterplexd/pms_timeline.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/host/libmisterplex/plexui/plexui_http.cpp \
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
	$(ROOT)/arm/misterplexd/tls_http.cpp \
	$(ROOT)/arm/misterplexd/companion.cpp \
	$(ROOT)/arm/misterplexd/fb_present.cpp \
	$(ROOT)/arm/misterplexd/media_player.cpp \
	$(ROOT)/host/libmisterplex/theme_decode.cpp \
	$(ROOT)/arm/misterplexd/pms_timeline.cpp \
	$(ROOT)/arm/misterplexd/plex_resolve.cpp \
	$(ROOT)/arm/misterplexd/fpga_spi.cpp \
	$(ROOT)/arm/misterplexd/gcast/airplay_fe.cpp \
	$(ROOT)/arm/misterplexd/gcast/https_client.cpp \
	$(ROOT)/arm/misterplexd/gcast/yt_fe.cpp \
	$(PLEXUI_SRC)
MPLEX_INC := -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui

$(ROOT)/build/test_server_picker: $(ROOT)/tests/unit/test_server_picker.cpp \
		$(ROOT)/host/libmisterplex/plex_server_catalog.hpp \
		$(ROOT)/host/libmisterplex/browse_controller.hpp \
		$(ROOT)/tests/fixtures/plex_v2_resources_lab.json $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -I$(ROOT)/host/libmisterplex/plexui \
		-DMPX_PLEXUI_UNIT_TEST -o $@ $(ROOT)/tests/unit/test_server_picker.cpp $(PLEXUI_SRC)

$(ROOT)/build/test_server_picker.check: $(ROOT)/build/test_server_picker
	cd $(ROOT) && $(ROOT)/build/test_server_picker
	@touch $@

unit-unlocked unit-host-unlocked: $(ROOT)/build/test_server_picker.check

$(ROOT)/build/test_plex_meta_text: $(ROOT)/tests/unit/test_plex_meta_text.cpp \
		$(ROOT)/host/libmisterplex/plex_text.hpp $(ROOT)/host/libmisterplex/plex_markers.hpp \
		$(ROOT)/host/libmisterplex/chapters.hpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/arm/misterplexd/plex_resolve.hpp $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -I$(ROOT)/host/libmisterplex/plexui \
		-o $@ $(ROOT)/tests/unit/test_plex_meta_text.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp $(PLEXUI_SRC)

$(ROOT)/build/test_plex_meta_text.check: $(ROOT)/build/test_plex_meta_text
	$(ROOT)/build/test_plex_meta_text
	@touch $@

unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plex_meta_text.check
# Stamped into misterplexd so GET /misterplex/status can show the commit. Empty when git is absent.
MPX_GIT_COMMIT := $(shell git -C $(ROOT) rev-parse HEAD 2>/dev/null)
MPX_GIT_DEFINE := $(if $(MPX_GIT_COMMIT),-DMPX_GIT_COMMIT=\"$(MPX_GIT_COMMIT)\",)
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

.PHONY: test_plexui_sidebar_after_play
.PHONY: plexui-tests test_plexui_sidebar_home_focus test_plexui_libgrid_sidebar test_plexui_libgrid_drawer_title test_plexui_chip_focus test_plexui_240p_layout
test_plexui_sidebar_home_focus: $(ROOT)/build/plexui-tests/test_plexui_sidebar_home_focus
	cd $(ROOT) && ./build/plexui-tests/test_plexui_sidebar_home_focus

test_plexui_libgrid_sidebar: $(ROOT)/build/plexui-tests/test_plexui_libgrid_sidebar
	cd $(ROOT) && ./build/plexui-tests/test_plexui_libgrid_sidebar

test_plexui_libgrid_drawer_title: $(ROOT)/build/plexui-tests/test_plexui_libgrid_drawer_title
	cd $(ROOT) && ./build/plexui-tests/test_plexui_libgrid_drawer_title

$(ROOT)/build/plexui-tests/test_plexui_libgrid_drawer_title: \
		$(ROOT)/tests/unit/test_plexui_libgrid_drawer_title.cpp $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build/plexui-tests
	$(CXX) -std=c++17 -O1 -pthread $(MPLEX_INC) -DMPX_PLEXUI_UNIT_TEST -o $@ \
		$(ROOT)/tests/unit/test_plexui_libgrid_drawer_title.cpp $(PLEXUI_SRC)

$(ROOT)/build/plexui-tests/test_plexui_libgrid_sidebar: \
		$(ROOT)/tests/unit/test_plexui_libgrid_sidebar.cpp $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build/plexui-tests
	$(CXX) -std=c++17 -O1 -pthread $(MPLEX_INC) -DMPX_PLEXUI_UNIT_TEST -o $@ \
		$(ROOT)/tests/unit/test_plexui_libgrid_sidebar.cpp $(PLEXUI_SRC)

test_plexui_240p_layout: $(ROOT)/build/plexui-tests/test_plexui_240p_layout
	cd $(ROOT) && ./build/plexui-tests/test_plexui_240p_layout

$(ROOT)/build/plexui-tests/test_plexui_240p_layout: \
		$(ROOT)/tests/unit/test_plexui_240p_layout.cpp $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build/plexui-tests
	$(CXX) -std=c++17 -O1 -pthread $(MPLEX_INC) -DMPX_PLEXUI_UNIT_TEST -o $@ \
		$(ROOT)/tests/unit/test_plexui_240p_layout.cpp $(PLEXUI_SRC)

test_plexui_chip_focus: $(ROOT)/build/plexui-tests/test_plexui_chip_focus
	cd $(ROOT) && ./build/plexui-tests/test_plexui_chip_focus

$(ROOT)/build/plexui-tests/test_plexui_chip_focus: \
		$(ROOT)/tests/unit/test_plexui_chip_focus.cpp $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build/plexui-tests
	$(CXX) -std=c++17 -O1 -pthread $(MPLEX_INC) -DMPX_PLEXUI_UNIT_TEST -o $@ \
		$(ROOT)/tests/unit/test_plexui_chip_focus.cpp $(PLEXUI_SRC)

test_plexui_sidebar_after_play: $(ROOT)/build/plexui-tests/test_plexui_sidebar_after_play
	cd $(ROOT) && ./build/plexui-tests/test_plexui_sidebar_after_play

$(ROOT)/build/plexui-tests/test_plexui_sidebar_after_play: \
		$(ROOT)/tests/unit/test_plexui_sidebar_after_play.cpp $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build/plexui-tests
	$(CXX) -std=c++17 -O1 -pthread $(MPLEX_INC) -DMPX_PLEXUI_UNIT_TEST -o $@ \
		$(ROOT)/tests/unit/test_plexui_sidebar_after_play.cpp $(PLEXUI_SRC)

$(ROOT)/build/plexui-tests/test_plexui_sidebar_home_focus: \
		$(ROOT)/tests/unit/test_plexui_sidebar_home_focus.cpp $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build/plexui-tests
	$(CXX) -std=c++17 -O1 -pthread $(MPLEX_INC) -DMPX_PLEXUI_UNIT_TEST -o $@ \
		$(ROOT)/tests/unit/test_plexui_sidebar_home_focus.cpp $(PLEXUI_SRC)

plexui-tests:
	@mkdir -p $(ROOT)/build/plexui-tests
	$(CXX) -std=c++17 -O1 -pthread $(MPLEX_INC) -DMPX_PLEXUI_UNIT_TEST -o $(ROOT)/build/plexui-tests/test_plexui \
		$(ROOT)/tests/unit/test_plexui.cpp $(PLEXUI_SRC)
	cd $(ROOT) && ./build/plexui-tests/test_plexui
	$(CXX) -std=c++17 -O1 -pthread $(MPLEX_INC) -DMPX_PLEXUI_UNIT_TEST \
		-o $(ROOT)/build/plexui-tests/test_plexui_continue_watching \
		$(ROOT)/tests/unit/test_plexui_continue_watching.cpp $(PLEXUI_SRC)
	cd $(ROOT) && ./build/plexui-tests/test_plexui_continue_watching
	$(CXX) -std=c++17 -O1 -pthread $(MPLEX_INC) -DMPX_PLEXUI_UNIT_TEST \
		-I$(OWENB_RING_DIR)/include \
		-o $(ROOT)/build/plexui-tests/test_plexui_ui_safe \
		$(ROOT)/tests/unit/test_plexui_ui_safe.cpp $(PLEXUI_SRC)
	cd $(ROOT) && ./build/plexui-tests/test_plexui_ui_safe
	$(CXX) -std=c++17 -O1 -pthread $(MPLEX_INC) -DMPX_PLEXUI_UNIT_TEST \
		-I$(OWENB_RING_DIR)/include \
		-o $(ROOT)/build/plexui-tests/test_plexui_sidebar_safe \
		$(ROOT)/tests/unit/test_plexui_sidebar_safe.cpp $(PLEXUI_SRC)
	cd $(ROOT) && ./build/plexui-tests/test_plexui_sidebar_safe
	$(MAKE) test_plexui_sidebar_after_play
	$(MAKE) test_plexui_sidebar_home_focus
	$(MAKE) test_plexui_libgrid_sidebar
	$(MAKE) test_plexui_libgrid_drawer_title
	$(MAKE) test_plexui_chip_focus
	$(MAKE) test_plexui_240p_layout
	$(MAKE) $(ROOT)/build/test_plexui_seed_merge.ok
	$(MAKE) $(ROOT)/build/test_plexui_home_row_clamp.ok $(ROOT)/build/test_plexui_home_row_clamp_asan.ok $(ROOT)/build/test_plexui_home_row_clamp.mutant-check
	$(MAKE) $(ROOT)/build/test_plexui_hide_empty_hubs.ok $(ROOT)/build/test_plexui_hide_empty_hubs_asan.ok $(ROOT)/build/test_plexui_hide_empty_hubs.mutant-check
	$(MAKE) $(ROOT)/build/test_plexui_image_ready_race.ok $(ROOT)/build/test_plexui_image_ready_race_asan.ok $(ROOT)/build/test_plexui_image_ready_race.mutant-check
	$(MAKE) $(ROOT)/build/test_plexui_home_empty_row.ok $(ROOT)/build/test_plexui_home_empty_row_asan.ok $(ROOT)/build/test_plexui_home_empty_row.mutant-check
	$(MAKE) $(ROOT)/build/test_plexui_dup_studio.ok $(ROOT)/build/test_plexui_dup_studio_asan.ok $(ROOT)/build/test_plexui_dup_studio.mutant-check
	$(MAKE) $(ROOT)/build/test_plexui_remote_browse_focus.ok $(ROOT)/build/test_plexui_remote_browse_focus_asan.ok $(ROOT)/build/test_plexui_remote_browse_focus.mutant-check
	$(MAKE) $(ROOT)/build/test_plexui_home_vscroll.ok $(ROOT)/build/test_plexui_home_vscroll_asan.ok $(ROOT)/build/test_plexui_home_vscroll.mutant-check

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
	@$(CXX) $(CXXFLAGS) $(PLEXUI_PAINT_READY_FLAGS) -DMUT_PLEXUI_IDLE_OVERLAY \
		-o $(ROOT)/build/test_plexui_paint_ready.idleov \
		$(ROOT)/tests/unit/test_plexui_paint_ready.cpp $(PLEXUI_SRC)
	@if $(ROOT)/build/test_plexui_paint_ready.idleov > $@.idleov.log 2>&1; then \
		echo "MUTANT SURVIVED: MUT_PLEXUI_IDLE_OVERLAY"; exit 1; fi
	@grep -q "GH#14 skip Ready chevron over plexui" $@.idleov.log || { echo "MUTANT MUT_PLEXUI_IDLE_OVERLAY KILLED BY THE WRONG CHECK"; cat $@.idleov.log; exit 1; }
	@echo "test_plexui_paint_ready: MUT_PLEXUI_SKIP_ART_READY MUT_PLEXUI_IDLE_OVERLAY FAIL as required" | tee $@
	@rm -f $(ROOT)/build/test_plexui_paint_ready.idleov
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
$(ROOT)/build/test_wall_clock_mut_netdown: $(ROOT)/tests/unit/test_wall_clock.cpp $(ROOT)/arm/misterplexd/wall_clock.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -DMPX_SOURCE_ROOT='"$(ROOT)"' -DMUT_WALL_RESCUE_LONG_NETDOWN_SLEEP -o $@ \
		$(ROOT)/tests/unit/test_wall_clock.cpp
$(ROOT)/build/test_wall_clock.mutant-check: $(ROOT)/build/test_wall_clock $(ROOT)/build/test_wall_clock_mut \
		$(ROOT)/build/test_wall_clock_mut_sntp $(ROOT)/build/test_wall_clock_mut_rescue \
		$(ROOT)/build/test_wall_clock_mut_netdown
	@$(ROOT)/build/test_wall_clock > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@if $(ROOT)/build/test_wall_clock_mut > $@.log 2>&1; then \
		echo "MUTANT SURVIVED: MUT_WALL_TRUST_1970"; exit 1; fi
	@if $(ROOT)/build/test_wall_clock_mut_sntp > $@.sntp.log 2>&1; then \
		echo "MUTANT SURVIVED: MUT_SNTP_SKIP_CHECKS"; exit 1; fi
	@if $(ROOT)/build/test_wall_clock_mut_rescue > $@.rescue.log 2>&1; then \
		echo "MUTANT SURVIVED: MUT_WALL_RESCUE_IGNORE_SANE"; exit 1; fi
	@if $(ROOT)/build/test_wall_clock_mut_netdown > $@.netdown.log 2>&1; then \
		echo "MUTANT SURVIVED: MUT_WALL_RESCUE_LONG_NETDOWN_SLEEP"; exit 1; fi
	@echo "test_wall_clock: mutants MUT_WALL_TRUST_1970, MUT_SNTP_SKIP_CHECKS, MUT_WALL_RESCUE_IGNORE_SANE, and MUT_WALL_RESCUE_LONG_NETDOWN_SLEEP FAIL as required" | tee $@
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
	cd $(ROOT) && $(ROOT)/build/test_ob_output_reinit
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_ob_output_reinit.ok

$(ROOT)/build/test_av_open_security: $(ROOT)/tests/unit/test_av_open_security.cpp \
		$(ROOT)/arm/misterplexd/av_open_opts.hpp $(ROOT)/arm/misterplexd/ca_bundle.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -o $@ $(ROOT)/tests/unit/test_av_open_security.cpp
$(ROOT)/build/test_av_open_security.ok: $(ROOT)/build/test_av_open_security
	cd $(ROOT) && $(ROOT)/build/test_av_open_security
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_av_open_security.ok

$(ROOT)/build/test_yt_403: $(ROOT)/tests/unit/test_yt_403.cpp $(ROOT)/arm/misterplexd/gcast/yt_403.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -o $@ $(ROOT)/tests/unit/test_yt_403.cpp
$(ROOT)/build/test_yt_403.ok: $(ROOT)/build/test_yt_403
	cd $(ROOT) && $(ROOT)/build/test_yt_403
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_yt_403.ok

$(ROOT)/build/test_plexui_send_home: $(ROOT)/tests/unit/test_plexui_send_home.cpp $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui -I$(ROOT)/arm/misterplexd \
		-o $@ $(ROOT)/tests/unit/test_plexui_send_home.cpp $(PLEXUI_SRC)
$(ROOT)/build/test_plexui_send_home.ok: $(ROOT)/build/test_plexui_send_home
	$(ROOT)/build/test_plexui_send_home
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plexui_send_home.ok

$(ROOT)/build/test_plexui_subtitle_picker: $(ROOT)/tests/unit/test_plexui_subtitle_picker.cpp $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread $(MPLEX_INC) -DMPX_PLEXUI_UNIT_TEST -o $@ \
		$(ROOT)/tests/unit/test_plexui_subtitle_picker.cpp $(PLEXUI_SRC)

$(ROOT)/build/test_plexui_audio_picker: $(ROOT)/tests/unit/test_plexui_audio_picker.cpp $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread $(MPLEX_INC) -DMPX_PLEXUI_UNIT_TEST -o $@ \
		$(ROOT)/tests/unit/test_plexui_audio_picker.cpp $(PLEXUI_SRC)

# #328 reviewer: browse/PlexUI setup is not gated on initPresent (glass conf, PLEXUI commented out).
$(ROOT)/build/test_reviewer_present_gate: $(ROOT)/tests/unit/test_reviewer_present_gate.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(MPLEX_INC) -o $@ $(ROOT)/tests/unit/test_reviewer_present_gate.cpp

$(ROOT)/build/test_reviewer_present_gate_asan: $(ROOT)/tests/unit/test_reviewer_present_gate.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined \
		$(MPLEX_INC) -o $@ $(ROOT)/tests/unit/test_reviewer_present_gate.cpp

$(ROOT)/build/test_reviewer_present_gate.ok: $(ROOT)/build/test_reviewer_present_gate \
		$(ROOT)/build/test_reviewer_present_gate_asan $(ROOT)/arm/misterplexd/main.cpp
	cd $(ROOT) && $(ROOT)/build/test_reviewer_present_gate
	cd $(ROOT) && $(ROOT)/build/test_reviewer_present_gate_asan
	@touch $@

# #342: present failed at boot -> bounded backoff retry starts the workers (present_retry.hpp).
$(ROOT)/build/test_present_retry: $(ROOT)/tests/unit/test_present_retry.cpp $(ROOT)/arm/misterplexd/present_retry.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(MPLEX_INC) -o $@ $(ROOT)/tests/unit/test_present_retry.cpp

$(ROOT)/build/test_present_retry_asan: $(ROOT)/tests/unit/test_present_retry.cpp $(ROOT)/arm/misterplexd/present_retry.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined \
		$(MPLEX_INC) -o $@ $(ROOT)/tests/unit/test_present_retry.cpp

$(ROOT)/build/test_present_retry.ok: $(ROOT)/build/test_present_retry \
		$(ROOT)/build/test_present_retry_asan $(ROOT)/arm/misterplexd/main.cpp \
		$(ROOT)/arm/misterplexd/media_player.cpp
	cd $(ROOT) && $(ROOT)/build/test_present_retry
	cd $(ROOT) && $(ROOT)/build/test_present_retry_asan
	@touch $@

$(ROOT)/build/test_plexui_home: $(ROOT)/tests/unit/test_plexui_home.cpp $(PLEXUI_SRC) \
		$(ROOT)/host/libmisterplex/plexui/plexui_home.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread $(MPLEX_INC) -DMPX_PLEXUI_UNIT_TEST -o $@ \
		$(ROOT)/tests/unit/test_plexui_home.cpp $(PLEXUI_SRC)

$(ROOT)/build/test_plexui_home_asan: $(ROOT)/tests/unit/test_plexui_home.cpp $(PLEXUI_SRC) \
		$(ROOT)/host/libmisterplex/plexui/plexui_home.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined \
		-pthread $(MPLEX_INC) -DMPX_PLEXUI_UNIT_TEST -o $@ \
		$(ROOT)/tests/unit/test_plexui_home.cpp $(PLEXUI_SRC)

$(ROOT)/build/test_plexui_home.ok: $(ROOT)/build/test_plexui_home $(ROOT)/build/test_plexui_home_asan \
		$(ROOT)/tests/unit/test_plexui_home.cpp $(PLEXUI_SRC) $(ROOT)/host/libmisterplex/plexui/plexui_home.hpp
	cd $(ROOT) && $(ROOT)/build/test_plexui_home
	cd $(ROOT) && $(ROOT)/build/test_plexui_home_asan
	@for m in MUT_HOME_SKIP_PIN MUT_HOME_CLOBBER_ACCOUNT MUT_HOME_ACCEPT_SHORT_PIN; do \
		$(CXX) $(CXXFLAGS) -O1 -g -pthread $(MPLEX_INC) -DMPX_PLEXUI_UNIT_TEST -D$$m \
			-o $(ROOT)/build/test_plexui_home.mut \
			$(ROOT)/tests/unit/test_plexui_home.cpp $(PLEXUI_SRC) \
			|| { echo "MUTANT DID NOT COMPILE: $$m"; exit 1; }; \
		if (cd $(ROOT) && $(ROOT)/build/test_plexui_home.mut >/dev/null 2>&1); then \
			echo "MUTANT SURVIVED: $$m"; exit 1; \
		fi; \
		echo "test_plexui_home: mutant $$m FAILs as required"; \
	done
	@touch $@

$(ROOT)/build/test_plexui_browse_polish: $(ROOT)/tests/unit/test_plexui_browse_polish.cpp $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include -o $@ \
		$(ROOT)/tests/unit/test_plexui_browse_polish.cpp $(PLEXUI_SRC)
$(ROOT)/build/test_plexui_browse_polish.ok: $(ROOT)/build/test_plexui_browse_polish
	$(ROOT)/build/test_plexui_browse_polish
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plexui_browse_polish.ok

$(ROOT)/build/test_plexui_seed_merge: $(ROOT)/tests/unit/test_plexui_seed_merge.cpp $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include -o $@ \
		$(ROOT)/tests/unit/test_plexui_seed_merge.cpp $(PLEXUI_SRC)
$(ROOT)/build/test_plexui_seed_merge.ok: $(ROOT)/build/test_plexui_seed_merge
	$(ROOT)/build/test_plexui_seed_merge
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plexui_seed_merge.ok

GWY_MUTANTS := MUT_GWY_DROP_TOKENLESS MUT_GWY_NO_URI_HOST MUT_GWY_ID_MACHINE_ONLY MUT_GWY_AUTH_GENERIC MUT_reviewer_ADDR_MERGE MUT_reviewer_TOKEN_ON_ADDR MUT_331_TOKEN_ADDR_MERGE
$(ROOT)/build/test_plexui_reviewer: $(ROOT)/tests/unit/test_plexui_reviewer.cpp $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include -o $@ \
		$(ROOT)/tests/unit/test_plexui_reviewer.cpp $(PLEXUI_SRC)
$(ROOT)/build/test_plexui_reviewer.ok: $(ROOT)/build/test_plexui_reviewer
	$(ROOT)/build/test_plexui_reviewer
	touch $@
# each reviewer mutant must build and FAIL a named check
$(ROOT)/build/test_plexui_reviewer.mutant-check: $(ROOT)/tests/unit/test_plexui_reviewer.cpp $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build
	@set -e; for m in $(GWY_MUTANTS); do \
		$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -D$$m -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
			-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include -o $(ROOT)/build/test_plexui_reviewer.$$m \
			$(ROOT)/tests/unit/test_plexui_reviewer.cpp $(PLEXUI_SRC); \
		if $(ROOT)/build/test_plexui_reviewer.$$m > $(ROOT)/build/test_plexui_reviewer.$$m.log 2>&1; then \
			echo "mutant $$m survived"; exit 1; fi; \
		grep -q '^FAIL: ' $(ROOT)/build/test_plexui_reviewer.$$m.log || { echo "mutant $$m: no FAIL line"; exit 1; }; \
		echo "mutant $$m killed: $$(grep -m1 '^FAIL: ' $(ROOT)/build/test_plexui_reviewer.$$m.log)"; \
	done
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plexui_reviewer.ok $(ROOT)/build/test_plexui_reviewer.mutant-check

$(ROOT)/build/test_plexui_dup_studio: $(ROOT)/tests/unit/test_plexui_dup_studio.cpp $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include -o $@ \
		$(ROOT)/tests/unit/test_plexui_dup_studio.cpp $(PLEXUI_SRC)
$(ROOT)/build/test_plexui_dup_studio.ok: $(ROOT)/build/test_plexui_dup_studio
	$(ROOT)/build/test_plexui_dup_studio
	touch $@
$(ROOT)/build/test_plexui_dup_studio_asan: $(ROOT)/tests/unit/test_plexui_dup_studio.cpp $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include -o $@ \
		$(ROOT)/tests/unit/test_plexui_dup_studio.cpp $(PLEXUI_SRC)
$(ROOT)/build/test_plexui_dup_studio_asan.ok: $(ROOT)/build/test_plexui_dup_studio_asan
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 $(ROOT)/build/test_plexui_dup_studio_asan
	touch $@
$(ROOT)/build/test_plexui_dup_studio.mutant-check: $(ROOT)/build/test_plexui_dup_studio
	@$(ROOT)/build/test_plexui_dup_studio > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include -DMUT_MERGE_DEAD_WINS -o $(ROOT)/build/test_plexui_dup_studio.MUT_MERGE_DEAD_WINS \
		$(ROOT)/tests/unit/test_plexui_dup_studio.cpp $(PLEXUI_SRC)
	@if $(ROOT)/build/test_plexui_dup_studio.MUT_MERGE_DEAD_WINS > $@.m.log 2>&1; then echo "MUT_MERGE_DEAD_WINS SURVIVED"; exit 1; fi
	@echo "test_plexui_dup_studio: 1 mutant FAILS as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plexui_dup_studio.ok $(ROOT)/build/test_plexui_dup_studio_asan.ok $(ROOT)/build/test_plexui_dup_studio.mutant-check

$(ROOT)/build/test_plexui_home_row_clamp: $(ROOT)/tests/unit/test_plexui_home_row_clamp.cpp $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include -o $@ \
		$(ROOT)/tests/unit/test_plexui_home_row_clamp.cpp $(PLEXUI_SRC)
$(ROOT)/build/test_plexui_home_row_clamp.ok: $(ROOT)/build/test_plexui_home_row_clamp
	$(ROOT)/build/test_plexui_home_row_clamp
	touch $@
$(ROOT)/build/test_plexui_home_row_clamp_asan: $(ROOT)/tests/unit/test_plexui_home_row_clamp.cpp $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include -o $@ \
		$(ROOT)/tests/unit/test_plexui_home_row_clamp.cpp $(PLEXUI_SRC)
$(ROOT)/build/test_plexui_home_row_clamp_asan.ok: $(ROOT)/build/test_plexui_home_row_clamp_asan
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 $(ROOT)/build/test_plexui_home_row_clamp_asan
	touch $@
$(ROOT)/build/test_plexui_home_row_clamp.mutant-check: $(ROOT)/build/test_plexui_home_row_clamp
	@$(ROOT)/build/test_plexui_home_row_clamp > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include -DMUT_HOME_ROW_UNCLAMPED -o $(ROOT)/build/test_plexui_home_row_clamp.MUT_HOME_ROW_UNCLAMPED \
		$(ROOT)/tests/unit/test_plexui_home_row_clamp.cpp $(PLEXUI_SRC)
	@if $(ROOT)/build/test_plexui_home_row_clamp.MUT_HOME_ROW_UNCLAMPED > $@.m.log 2>&1; then echo "MUT_HOME_ROW_UNCLAMPED SURVIVED"; exit 1; fi
	@echo "test_plexui_home_row_clamp: 1 mutant FAILS as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plexui_home_row_clamp.ok $(ROOT)/build/test_plexui_home_row_clamp_asan.ok $(ROOT)/build/test_plexui_home_row_clamp.mutant-check

# #220: HIDE_EMPTY_HUBS (default off) drops Home hubs with no tiles.
$(ROOT)/build/test_plexui_hide_empty_hubs: $(ROOT)/tests/unit/test_plexui_hide_empty_hubs.cpp $(ROOT)/host/libmisterplex/plexui/hide_empty_hubs.hpp $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include -o $@ \
		$(ROOT)/tests/unit/test_plexui_hide_empty_hubs.cpp $(PLEXUI_SRC)
$(ROOT)/build/test_plexui_hide_empty_hubs.ok: $(ROOT)/build/test_plexui_hide_empty_hubs
	$(ROOT)/build/test_plexui_hide_empty_hubs
	touch $@
$(ROOT)/build/test_plexui_hide_empty_hubs_asan: $(ROOT)/tests/unit/test_plexui_hide_empty_hubs.cpp $(ROOT)/host/libmisterplex/plexui/hide_empty_hubs.hpp $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include -o $@ \
		$(ROOT)/tests/unit/test_plexui_hide_empty_hubs.cpp $(PLEXUI_SRC)
$(ROOT)/build/test_plexui_hide_empty_hubs_asan.ok: $(ROOT)/build/test_plexui_hide_empty_hubs_asan
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 $(ROOT)/build/test_plexui_hide_empty_hubs_asan
	touch $@
$(ROOT)/build/test_plexui_hide_empty_hubs.mutant-check: $(ROOT)/build/test_plexui_hide_empty_hubs
	@$(ROOT)/build/test_plexui_hide_empty_hubs > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include -DMUT_HEH_DEFAULT_ON -o $(ROOT)/build/test_plexui_hide_empty_hubs.MUT_HEH_DEFAULT_ON \
		$(ROOT)/tests/unit/test_plexui_hide_empty_hubs.cpp $(PLEXUI_SRC)
	@if $(ROOT)/build/test_plexui_hide_empty_hubs.MUT_HEH_DEFAULT_ON > $@.MUT_HEH_DEFAULT_ON.log 2>&1; then echo "MUT_HEH_DEFAULT_ON SURVIVED"; exit 1; fi
	@$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include -DMUT_HEH_OFF_STILL_BINDS -o $(ROOT)/build/test_plexui_hide_empty_hubs.MUT_HEH_OFF_STILL_BINDS \
		$(ROOT)/tests/unit/test_plexui_hide_empty_hubs.cpp $(PLEXUI_SRC)
	@if $(ROOT)/build/test_plexui_hide_empty_hubs.MUT_HEH_OFF_STILL_BINDS > $@.MUT_HEH_OFF_STILL_BINDS.log 2>&1; then echo "MUT_HEH_OFF_STILL_BINDS SURVIVED"; exit 1; fi
	@$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include -DMUT_HEH_ON_IGNORED -o $(ROOT)/build/test_plexui_hide_empty_hubs.MUT_HEH_ON_IGNORED \
		$(ROOT)/tests/unit/test_plexui_hide_empty_hubs.cpp $(PLEXUI_SRC)
	@if $(ROOT)/build/test_plexui_hide_empty_hubs.MUT_HEH_ON_IGNORED > $@.MUT_HEH_ON_IGNORED.log 2>&1; then echo "MUT_HEH_ON_IGNORED SURVIVED"; exit 1; fi
	@$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include -DMUT_HEH_WRONG_CONTEXT -o $(ROOT)/build/test_plexui_hide_empty_hubs.MUT_HEH_WRONG_CONTEXT \
		$(ROOT)/tests/unit/test_plexui_hide_empty_hubs.cpp $(PLEXUI_SRC)
	@if $(ROOT)/build/test_plexui_hide_empty_hubs.MUT_HEH_WRONG_CONTEXT > $@.MUT_HEH_WRONG_CONTEXT.log 2>&1; then echo "MUT_HEH_WRONG_CONTEXT SURVIVED"; exit 1; fi
	@echo "test_plexui_hide_empty_hubs: 4 mutants FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plexui_hide_empty_hubs.ok $(ROOT)/build/test_plexui_hide_empty_hubs_asan.ok $(ROOT)/build/test_plexui_hide_empty_hubs.mutant-check

# Lab hostnames are not a reason to hide a Plex server. Fixture names still are.
# MUT_NODE_WORKER_FILTER puts the lab hostname back and must fail the default pick.
$(ROOT)/build/test_fixture_plex_server_name: $(ROOT)/tests/unit/test_fixture_plex_server_name.cpp \
		$(ROOT)/host/libmisterplex/plexui/plexui_types.hpp $(ROOT)/host/libmisterplex/plexui/plexui_controller.hpp \
		$(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include -o $@ \
		$(ROOT)/tests/unit/test_fixture_plex_server_name.cpp $(PLEXUI_SRC)
$(ROOT)/build/test_fixture_plex_server_name.ok: $(ROOT)/build/test_fixture_plex_server_name
	$(ROOT)/build/test_fixture_plex_server_name
	touch $@
$(ROOT)/build/test_fixture_plex_server_name_asan: $(ROOT)/tests/unit/test_fixture_plex_server_name.cpp \
		$(ROOT)/host/libmisterplex/plexui/plexui_types.hpp $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		-pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include -o $@ \
		$(ROOT)/tests/unit/test_fixture_plex_server_name.cpp $(PLEXUI_SRC)
$(ROOT)/build/test_fixture_plex_server_name_asan.ok: $(ROOT)/build/test_fixture_plex_server_name_asan
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 $(ROOT)/build/test_fixture_plex_server_name_asan
	touch $@
$(ROOT)/build/test_fixture_plex_server_name.mutant-check: $(ROOT)/build/test_fixture_plex_server_name
	@$(ROOT)/build/test_fixture_plex_server_name > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -DMUT_NODE_WORKER_FILTER \
		-I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include -o $(ROOT)/build/test_fixture_plex_server_name.MUT_NODE_WORKER_FILTER \
		$(ROOT)/tests/unit/test_fixture_plex_server_name.cpp $(PLEXUI_SRC)
	@if $(ROOT)/build/test_fixture_plex_server_name.MUT_NODE_WORKER_FILTER > $@.m.log 2>&1; then \
		echo "MUT_NODE_WORKER_FILTER SURVIVED"; cat $@.m.log; exit 1; fi
	@echo "test_fixture_plex_server_name: MUT_NODE_WORKER_FILTER fails as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_fixture_plex_server_name.ok \
	$(ROOT)/build/test_fixture_plex_server_name_asan.ok \
	$(ROOT)/build/test_fixture_plex_server_name.mutant-check
# #219: image-worker markImageReady vs paint-thread render() race (UAF).
$(ROOT)/build/test_plexui_image_ready_race: $(ROOT)/tests/unit/test_plexui_image_ready_race.cpp $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include -o $@ \
		$(ROOT)/tests/unit/test_plexui_image_ready_race.cpp $(PLEXUI_SRC)
$(ROOT)/build/test_plexui_image_ready_race.ok: $(ROOT)/build/test_plexui_image_ready_race
	$(ROOT)/build/test_plexui_image_ready_race
	touch $@
$(ROOT)/build/test_plexui_image_ready_race_asan: $(ROOT)/tests/unit/test_plexui_image_ready_race.cpp $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include -o $@ \
		$(ROOT)/tests/unit/test_plexui_image_ready_race.cpp $(PLEXUI_SRC)
$(ROOT)/build/test_plexui_image_ready_race_asan.ok: $(ROOT)/build/test_plexui_image_ready_race_asan
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 $(ROOT)/build/test_plexui_image_ready_race_asan
	touch $@
$(ROOT)/build/test_plexui_image_ready_race.mutant-check: $(ROOT)/build/test_plexui_image_ready_race $(ROOT)/build/test_plexui_image_ready_race_asan
	@$(ROOT)/build/test_plexui_image_ready_race > $@.plain.log 2>&1 || { cat $@.plain.log; exit 1; }
	@$(ROOT)/build/test_plexui_image_ready_race_asan > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include -DMUT_IMG_READY_UNLOCKED -o $(ROOT)/build/test_plexui_image_ready_race.MUT_IMG_READY_UNLOCKED \
		$(ROOT)/tests/unit/test_plexui_image_ready_race.cpp $(PLEXUI_SRC)
	@if $(ROOT)/build/test_plexui_image_ready_race.MUT_IMG_READY_UNLOCKED > $@.MUT_IMG_READY_UNLOCKED.log 2>&1; then echo "MUT_IMG_READY_UNLOCKED SURVIVED"; exit 1; fi
	@echo "test_plexui_image_ready_race: unlocked mutant FAILS (ASan UAF) as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plexui_image_ready_race.ok $(ROOT)/build/test_plexui_image_ready_race_asan.ok $(ROOT)/build/test_plexui_image_ready_race.mutant-check
# #257: RIGHT on an empty Home hub row ran col past the row; off-screen rect memcpy SIGSEGV.
$(ROOT)/build/test_plexui_home_empty_row: $(ROOT)/tests/unit/test_plexui_home_empty_row.cpp $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include -o $@ \
		$(ROOT)/tests/unit/test_plexui_home_empty_row.cpp $(PLEXUI_SRC)
$(ROOT)/build/test_plexui_home_empty_row.ok: $(ROOT)/build/test_plexui_home_empty_row
	$(ROOT)/build/test_plexui_home_empty_row
	touch $@
$(ROOT)/build/test_plexui_home_empty_row_asan: $(ROOT)/tests/unit/test_plexui_home_empty_row.cpp $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include -o $@ \
		$(ROOT)/tests/unit/test_plexui_home_empty_row.cpp $(PLEXUI_SRC)
$(ROOT)/build/test_plexui_home_empty_row_asan.ok: $(ROOT)/build/test_plexui_home_empty_row_asan
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 $(ROOT)/build/test_plexui_home_empty_row_asan
	touch $@
$(ROOT)/build/test_plexui_home_empty_row.mutant-check: $(ROOT)/build/test_plexui_home_empty_row
	@$(ROOT)/build/test_plexui_home_empty_row > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include -DMUT_HOME_EMPTY_ROW_COL -o $(ROOT)/build/test_plexui_home_empty_row.MUT_HOME_EMPTY_ROW_COL \
		$(ROOT)/tests/unit/test_plexui_home_empty_row.cpp $(PLEXUI_SRC)
	@if $(ROOT)/build/test_plexui_home_empty_row.MUT_HOME_EMPTY_ROW_COL > $@.m.log 2>&1; then echo "MUT_HOME_EMPTY_ROW_COL SURVIVED"; exit 1; fi
	@$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include -DMUT_COPYRECT_NO_EMPTY_GUARD -o $(ROOT)/build/test_plexui_home_empty_row.MUT_COPYRECT_NO_EMPTY_GUARD \
		$(ROOT)/tests/unit/test_plexui_home_empty_row.cpp $(PLEXUI_SRC)
	@if ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 $(ROOT)/build/test_plexui_home_empty_row.MUT_COPYRECT_NO_EMPTY_GUARD > $@.m2.log 2>&1; then echo "MUT_COPYRECT_NO_EMPTY_GUARD SURVIVED"; exit 1; fi
	@grep -q "negative-size-param" $@.m2.log || { cat $@.m2.log; echo "MUT_COPYRECT_NO_EMPTY_GUARD: expected ASan negative-size-param"; exit 1; }
	@echo "test_plexui_home_empty_row: 2 mutants FAIL as required (MUT_HOME_EMPTY_ROW_COL, ASan MUT_COPYRECT_NO_EMPTY_GUARD)" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plexui_home_empty_row.ok $(ROOT)/build/test_plexui_home_empty_row_asan.ok $(ROOT)/build/test_plexui_home_empty_row.mutant-check

# #120/#121 (#206): remote-server Home focus stays on hub rows (DOWN clamp), A plays.
$(ROOT)/build/test_plexui_remote_browse_focus: $(ROOT)/tests/unit/test_plexui_remote_browse_focus.cpp $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include -o $@ \
		$(ROOT)/tests/unit/test_plexui_remote_browse_focus.cpp $(PLEXUI_SRC)
$(ROOT)/build/test_plexui_remote_browse_focus.ok: $(ROOT)/build/test_plexui_remote_browse_focus
	$(ROOT)/build/test_plexui_remote_browse_focus
	touch $@
$(ROOT)/build/test_plexui_remote_browse_focus_asan: $(ROOT)/tests/unit/test_plexui_remote_browse_focus.cpp $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include -o $@ \
		$(ROOT)/tests/unit/test_plexui_remote_browse_focus.cpp $(PLEXUI_SRC)
$(ROOT)/build/test_plexui_remote_browse_focus_asan.ok: $(ROOT)/build/test_plexui_remote_browse_focus_asan
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 $(ROOT)/build/test_plexui_remote_browse_focus_asan
	touch $@
$(ROOT)/build/test_plexui_remote_browse_focus.mutant-check: $(ROOT)/build/test_plexui_remote_browse_focus
	@$(ROOT)/build/test_plexui_remote_browse_focus > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include -DMUT_HOME_ROW_UNCLAMPED -o $(ROOT)/build/test_plexui_remote_browse_focus.MUT_HOME_ROW_UNCLAMPED \
		$(ROOT)/tests/unit/test_plexui_remote_browse_focus.cpp $(PLEXUI_SRC)
	@if $(ROOT)/build/test_plexui_remote_browse_focus.MUT_HOME_ROW_UNCLAMPED > $@.m.log 2>&1; then echo "MUT_HOME_ROW_UNCLAMPED SURVIVED"; exit 1; fi
	@echo "test_plexui_remote_browse_focus: 1 mutant FAILS as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plexui_remote_browse_focus.ok $(ROOT)/build/test_plexui_remote_browse_focus_asan.ok $(ROOT)/build/test_plexui_remote_browse_focus.mutant-check

# #206 GLASS-211: Home vertical scroll + column clamp on DOWN (A plays on row 6).
$(ROOT)/build/test_plexui_home_vscroll: $(ROOT)/tests/unit/test_plexui_home_vscroll.cpp $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include -o $@ \
		$(ROOT)/tests/unit/test_plexui_home_vscroll.cpp $(PLEXUI_SRC)
$(ROOT)/build/test_plexui_home_vscroll.ok: $(ROOT)/build/test_plexui_home_vscroll
	$(ROOT)/build/test_plexui_home_vscroll
	touch $@
$(ROOT)/build/test_plexui_home_vscroll_asan: $(ROOT)/tests/unit/test_plexui_home_vscroll.cpp $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include -o $@ \
		$(ROOT)/tests/unit/test_plexui_home_vscroll.cpp $(PLEXUI_SRC)
$(ROOT)/build/test_plexui_home_vscroll_asan.ok: $(ROOT)/build/test_plexui_home_vscroll_asan
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 $(ROOT)/build/test_plexui_home_vscroll_asan
	touch $@
$(ROOT)/build/test_plexui_home_vscroll.mutant-check: $(ROOT)/build/test_plexui_home_vscroll
	@$(ROOT)/build/test_plexui_home_vscroll > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include -DMUT_HOME_ROW_UNCLAMPED -o $(ROOT)/build/test_plexui_home_vscroll.MUT_HOME_ROW_UNCLAMPED \
		$(ROOT)/tests/unit/test_plexui_home_vscroll.cpp $(PLEXUI_SRC)
	@if $(ROOT)/build/test_plexui_home_vscroll.MUT_HOME_ROW_UNCLAMPED > $@.m.log 2>&1; then echo "MUT_HOME_ROW_UNCLAMPED SURVIVED"; exit 1; fi
	@$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include -DMUT_HOME_COL_STALE -o $(ROOT)/build/test_plexui_home_vscroll.MUT_HOME_COL_STALE \
		$(ROOT)/tests/unit/test_plexui_home_vscroll.cpp $(PLEXUI_SRC)
	@if $(ROOT)/build/test_plexui_home_vscroll.MUT_HOME_COL_STALE > $@.MUT_HOME_COL_STALE.log 2>&1; then echo "MUT_HOME_COL_STALE SURVIVED"; exit 1; fi
	@$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include -DMUT_HOME_NO_VSCROLL -o $(ROOT)/build/test_plexui_home_vscroll.MUT_HOME_NO_VSCROLL \
		$(ROOT)/tests/unit/test_plexui_home_vscroll.cpp $(PLEXUI_SRC)
	@if $(ROOT)/build/test_plexui_home_vscroll.MUT_HOME_NO_VSCROLL > $@.MUT_HOME_NO_VSCROLL.log 2>&1; then echo "MUT_HOME_NO_VSCROLL SURVIVED"; exit 1; fi
	@$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include -DMUT_HOME_PATCH_NO_SKIP -o $(ROOT)/build/test_plexui_home_vscroll.MUT_HOME_PATCH_NO_SKIP \
		$(ROOT)/tests/unit/test_plexui_home_vscroll.cpp $(PLEXUI_SRC)
	@if $(ROOT)/build/test_plexui_home_vscroll.MUT_HOME_PATCH_NO_SKIP > $@.MUT_HOME_PATCH_NO_SKIP.log 2>&1; then echo "MUT_HOME_PATCH_NO_SKIP SURVIVED"; exit 1; fi
	@echo "test_plexui_home_vscroll: 4 mutants FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plexui_home_vscroll.ok $(ROOT)/build/test_plexui_home_vscroll_asan.ok $(ROOT)/build/test_plexui_home_vscroll.mutant-check

$(ROOT)/build/test_plexui_continue_watching: $(ROOT)/tests/unit/test_plexui_continue_watching.cpp $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include -o $@ \
		$(ROOT)/tests/unit/test_plexui_continue_watching.cpp $(PLEXUI_SRC)
$(ROOT)/build/test_plexui_continue_watching.ok: $(ROOT)/build/test_plexui_continue_watching
	$(ROOT)/build/test_plexui_continue_watching
	touch $@
$(ROOT)/build/test_plexui_continue_watching.mutant-check: $(ROOT)/build/test_plexui_continue_watching
	@$(ROOT)/build/test_plexui_continue_watching > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -DMUT_CW_KEEP_FINISHED \
		-I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include \
		-o $(ROOT)/build/test_plexui_continue_watching.mut \
		$(ROOT)/tests/unit/test_plexui_continue_watching.cpp $(PLEXUI_SRC)
	@if $(ROOT)/build/test_plexui_continue_watching.mut > $@.mut.log 2>&1; then \
		echo "test_plexui_continue_watching: MUT_CW_KEEP_FINISHED SURVIVED"; exit 1; fi
	@echo "test_plexui_continue_watching: MUT_CW_KEEP_FINISHED FAIL as required" | tee $@
	@rm -f $(ROOT)/build/test_plexui_continue_watching.mut
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plexui_continue_watching.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plexui_continue_watching.mutant-check

$(ROOT)/build/test_plexui_ui_safe: $(ROOT)/tests/unit/test_plexui_ui_safe.cpp $(PLEXUI_SRC) \
		$(ROOT)/host/libmisterplex/ui_safe_layout.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include -o $@ \
		$(ROOT)/tests/unit/test_plexui_ui_safe.cpp $(PLEXUI_SRC)
$(ROOT)/build/test_plexui_ui_safe.ok: $(ROOT)/build/test_plexui_ui_safe
	$(ROOT)/build/test_plexui_ui_safe
	touch $@
$(ROOT)/build/test_plexui_ui_safe.mutant-check: $(ROOT)/build/test_plexui_ui_safe
	@$(ROOT)/build/test_plexui_ui_safe > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in MUT_GH46_NO_LARGE MUT_GH46_NO_TOGGLE; do \
		$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -D$$m \
			-I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
			-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include \
			-o $(ROOT)/build/test_plexui_ui_safe.$$m \
			$(ROOT)/tests/unit/test_plexui_ui_safe.cpp $(PLEXUI_SRC); \
		if $(ROOT)/build/test_plexui_ui_safe.$$m > $@.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
	done
	@echo "test_plexui_ui_safe: MUT_GH46_NO_LARGE MUT_GH46_NO_TOGGLE FAIL as required" | tee $@
	@rm -f $(ROOT)/build/test_plexui_ui_safe.MUT_GH46_NO_LARGE $(ROOT)/build/test_plexui_ui_safe.MUT_GH46_NO_TOGGLE
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plexui_ui_safe.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plexui_ui_safe.mutant-check

# Browse sidebar content must sit inside the UI_SAFE_MARGIN frame (glass: 1.1% at 480p/480i). SAN + mutant via the wrapper.
$(ROOT)/build/test_plexui_sidebar_safe.ok: $(ROOT)/tests/unit/test_plexui_sidebar_safe.cpp \
		$(ROOT)/tests/unit/test_plexui_sidebar_safe.sh $(PLEXUI_SRC) $(ROOT)/host/libmisterplex/ui_safe_layout.hpp
	@mkdir -p $(ROOT)/build
	SAN=1 ROOT=$(ROOT) bash $(ROOT)/tests/unit/test_plexui_sidebar_safe.sh
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plexui_sidebar_safe.ok

$(ROOT)/build/test_plexui_browse_polish.mutant-check: $(ROOT)/build/test_plexui_browse_polish $(PLEXUI_SRC)
	@$(ROOT)/build/test_plexui_browse_polish > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -DMUT_RC1037_REMOTE_NOLR \
		-I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -I$(OWENB_RING_DIR)/include -o $@.nolr \
		$(ROOT)/tests/unit/test_plexui_browse_polish.cpp $(PLEXUI_SRC)
	@if $@.nolr > $@.nolr.log 2>&1; then echo "MUTANT SURVIVED: MUT_RC1037_REMOTE_NOLR"; exit 1; fi
	@echo "test_plexui_browse_polish: MUT_RC1037_REMOTE_NOLR FAILS as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plexui_browse_polish.mutant-check

$(ROOT)/build/test_play_queue_write: $(ROOT)/tests/unit/test_play_queue_write.cpp \
		$(ROOT)/host/libmisterplex/play_queue_write.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/host -o $@ $(ROOT)/tests/unit/test_play_queue_write.cpp
$(ROOT)/build/test_play_queue_write.ok: $(ROOT)/build/test_play_queue_write
	$(ROOT)/build/test_play_queue_write
	touch $@
$(ROOT)/build/test_play_queue_write_asan: $(ROOT)/tests/unit/test_play_queue_write.cpp \
		$(ROOT)/host/libmisterplex/play_queue_write.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -I$(ROOT)/host \
		-o $@ $(ROOT)/tests/unit/test_play_queue_write.cpp
$(ROOT)/build/test_play_queue_write_asan.ok: $(ROOT)/build/test_play_queue_write_asan
	$(ROOT)/build/test_play_queue_write_asan
	touch $@
$(ROOT)/build/test_play_queue_write.mutant-check: $(ROOT)/build/test_play_queue_write
	@$(ROOT)/build/test_play_queue_write > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in MUT_PQ_WRITE_IGNORED MUT_PQ_PUT_NO_NEXT MUT_PQ_ACCEPT_REFUSED MUT_PQ_POST_BODY; do \
		$(CXX) $(CXXFLAGS) -D$$m -I$(ROOT)/host -o $(ROOT)/build/test_play_queue_write.$$m \
			$(ROOT)/tests/unit/test_play_queue_write.cpp; \
		if $(ROOT)/build/test_play_queue_write.$$m > $@.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
	done
	@echo "test_play_queue_write: 4 mutants FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_play_queue_write.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_play_queue_write_asan.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_play_queue_write.mutant-check

$(ROOT)/build/test_mark_watched: $(ROOT)/tests/unit/test_mark_watched.cpp \
		$(ROOT)/host/libmisterplex/mark_watched.hpp $(ROOT)/host/libmisterplex/input_mailbox.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/host -o $@ $(ROOT)/tests/unit/test_mark_watched.cpp
$(ROOT)/build/test_mark_watched.ok: $(ROOT)/build/test_mark_watched
	$(ROOT)/build/test_mark_watched
	touch $@
$(ROOT)/build/test_mark_watched_asan: $(ROOT)/tests/unit/test_mark_watched.cpp \
		$(ROOT)/host/libmisterplex/mark_watched.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -I$(ROOT)/host \
		-o $@ $(ROOT)/tests/unit/test_mark_watched.cpp
$(ROOT)/build/test_mark_watched_asan.ok: $(ROOT)/build/test_mark_watched_asan
	$(ROOT)/build/test_mark_watched_asan
	touch $@
$(ROOT)/build/test_mark_watched.mutant-check: $(ROOT)/build/test_mark_watched
	@$(ROOT)/build/test_mark_watched > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in MUT_MW_FLAG_DEFAULT_ON MUT_MW_SEND_WHEN_OFF MUT_MW_WATCHED_IS_TIMELINE \
		MUT_MW_UNWATCHED_IS_SCROBBLE MUT_MW_DROP_IS_SCROBBLE MUT_MW_TOKEN_IN_URL \
		MUT_MW_PLAYING_IGNORED; do \
		$(CXX) $(CXXFLAGS) -D$$m -I$(ROOT)/host -o $(ROOT)/build/test_mark_watched.$$m \
			$(ROOT)/tests/unit/test_mark_watched.cpp; \
		if $(ROOT)/build/test_mark_watched.$$m > $@.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
	done
	@echo "test_mark_watched: 7 mutants FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_mark_watched.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_mark_watched_asan.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_mark_watched.mutant-check

$(ROOT)/build/test_mark_watched_ui: $(ROOT)/tests/unit/test_mark_watched_ui.cpp $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -o $@ $(ROOT)/tests/unit/test_mark_watched_ui.cpp $(PLEXUI_SRC)
$(ROOT)/build/test_mark_watched_ui.ok: $(ROOT)/build/test_mark_watched_ui
	$(ROOT)/build/test_mark_watched_ui
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_mark_watched_ui.ok

# GRID_PAGE default off: library grid stays on the first 200. On: Down past the
# loaded rows requests X-Plex-Container-Start past 0. Four mutants must FAIL.
$(ROOT)/build/test_grid_page: $(ROOT)/tests/unit/test_grid_page.cpp $(PLEXUI_SRC) \
		$(ROOT)/host/libmisterplex/plexui/grid_page.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST $(MPLEX_INC) -o $@ \
		$(ROOT)/tests/unit/test_grid_page.cpp $(PLEXUI_SRC)
$(ROOT)/build/test_grid_page.ok: $(ROOT)/build/test_grid_page
	$(ROOT)/build/test_grid_page
	touch $@
$(ROOT)/build/test_grid_page_asan: $(ROOT)/tests/unit/test_grid_page.cpp $(PLEXUI_SRC) \
		$(ROOT)/host/libmisterplex/plexui/grid_page.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
		-DMPX_PLEXUI_UNIT_TEST $(MPLEX_INC) -o $@ \
		$(ROOT)/tests/unit/test_grid_page.cpp $(PLEXUI_SRC)
$(ROOT)/build/test_grid_page_asan.ok: $(ROOT)/build/test_grid_page_asan
	$(ROOT)/build/test_grid_page_asan
	touch $@
$(ROOT)/build/test_grid_page.mutant-check: $(ROOT)/build/test_grid_page
	@$(ROOT)/build/test_grid_page > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in MUT_GRID_PAGE_FLAG_DEFAULT_ON MUT_GRID_PAGE_IGNORE_START \
		MUT_GRID_PAGE_OFF_STILL_PAGES MUT_GRID_PAGE_NO_APPEND; do \
		$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -D$$m $(MPLEX_INC) \
			-o $(ROOT)/build/test_grid_page.$$m \
			$(ROOT)/tests/unit/test_grid_page.cpp $(PLEXUI_SRC); \
		if $(ROOT)/build/test_grid_page.$$m > $@.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
	done
	@echo "test_grid_page: 4 mutants FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_grid_page.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_grid_page_asan.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_grid_page.mutant-check

# AZ_JUMP default off: library grid has no letter bar and no firstCharacter.
# On: Select on the bar requests that letter; GRID_PAGE pages it. Four mutants
# must FAIL.
$(ROOT)/build/test_az_jump: $(ROOT)/tests/unit/test_az_jump.cpp $(PLEXUI_SRC) \
		$(ROOT)/host/libmisterplex/plexui/az_jump.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST $(MPLEX_INC) -o $@ \
		$(ROOT)/tests/unit/test_az_jump.cpp $(PLEXUI_SRC)
$(ROOT)/build/test_az_jump.ok: $(ROOT)/build/test_az_jump
	$(ROOT)/build/test_az_jump
	touch $@
$(ROOT)/build/test_az_jump_asan: $(ROOT)/tests/unit/test_az_jump.cpp $(PLEXUI_SRC) \
		$(ROOT)/host/libmisterplex/plexui/az_jump.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
		-DMPX_PLEXUI_UNIT_TEST $(MPLEX_INC) -o $@ \
		$(ROOT)/tests/unit/test_az_jump.cpp $(PLEXUI_SRC)
$(ROOT)/build/test_az_jump_asan.ok: $(ROOT)/build/test_az_jump_asan
	$(ROOT)/build/test_az_jump_asan
	touch $@
$(ROOT)/build/test_az_jump.mutant-check: $(ROOT)/build/test_az_jump
	@$(ROOT)/build/test_az_jump > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in MUT_AZ_JUMP_FLAG_DEFAULT_ON MUT_AZ_JUMP_OFF_STILL_FILTERS \
		MUT_AZ_JUMP_IGNORE_LETTER MUT_AZ_JUMP_NO_ENTER; do \
		$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -D$$m $(MPLEX_INC) \
			-o $(ROOT)/build/test_az_jump.$$m \
			$(ROOT)/tests/unit/test_az_jump.cpp $(PLEXUI_SRC); \
		if $(ROOT)/build/test_az_jump.$$m > $@.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
	done
	@echo "test_az_jump: 4 mutants FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_az_jump.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_az_jump_asan.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_az_jump.mutant-check

# SETSTREAMS default off: Program menu has no Subtitles/Audio rows.
# On: a pick is setStreams (off, image/PGS burn, audio) at the current offset.
# Four mutants must FAIL.
$(ROOT)/build/test_setstreams: $(ROOT)/tests/unit/test_setstreams.cpp \
		$(ROOT)/host/libmisterplex/setstreams.hpp $(ROOT)/host/libmisterplex/owenb_menu.hpp \
		$(ROOT)/host/libmisterplex/track_select.hpp $(OWENB_RING_OBJS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(OWENB_RING_DIR)/include -o $@ $(ROOT)/tests/unit/test_setstreams.cpp $(OWENB_RING_OBJS)
$(ROOT)/build/test_setstreams.ok: $(ROOT)/build/test_setstreams
	$(ROOT)/build/test_setstreams
	touch $@
$(ROOT)/build/test_setstreams_asan: $(ROOT)/tests/unit/test_setstreams.cpp \
		$(ROOT)/host/libmisterplex/setstreams.hpp $(ROOT)/host/libmisterplex/owenb_menu.hpp \
		$(OWENB_RING_OBJS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		-I$(OWENB_RING_DIR)/include -o $@ $(ROOT)/tests/unit/test_setstreams.cpp $(OWENB_RING_OBJS)
$(ROOT)/build/test_setstreams_asan.ok: $(ROOT)/build/test_setstreams_asan
	$(ROOT)/build/test_setstreams_asan
	touch $@
$(ROOT)/build/test_setstreams.mutant-check: $(ROOT)/build/test_setstreams
	@$(ROOT)/build/test_setstreams > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in MUT_SETSTREAMS_FLAG_DEFAULT_ON MUT_SETSTREAMS_OFF_STILL_PICKS \
		MUT_SETSTREAMS_IGNORE_PICK MUT_SETSTREAMS_PGS_HIDDEN; do \
		$(CXX) $(CXXFLAGS) -I$(OWENB_RING_DIR)/include -D$$m -o $(ROOT)/build/test_setstreams.$$m \
			$(ROOT)/tests/unit/test_setstreams.cpp $(OWENB_RING_OBJS); \
		if $(ROOT)/build/test_setstreams.$$m > $@.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
	done
	@echo "test_setstreams: 4 mutants FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_setstreams.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_setstreams_asan.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_setstreams.mutant-check

# CHAPTERS default off: Program menu has no chapter rows and seek is a no-op.
# On: PMS Chapter startTimeOffset is a menu list and previous/next seek.
# Five mutants must FAIL.
$(ROOT)/build/test_chapters: $(ROOT)/tests/unit/test_chapters.cpp \
		$(ROOT)/host/libmisterplex/chapters.hpp $(ROOT)/host/libmisterplex/owenb_menu.hpp \
		$(OWENB_RING_OBJS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(OWENB_RING_DIR)/include -o $@ $(ROOT)/tests/unit/test_chapters.cpp $(OWENB_RING_OBJS)
$(ROOT)/build/test_chapters.ok: $(ROOT)/build/test_chapters
	$(ROOT)/build/test_chapters
	touch $@
$(ROOT)/build/test_chapters_asan: $(ROOT)/tests/unit/test_chapters.cpp \
		$(ROOT)/host/libmisterplex/chapters.hpp $(ROOT)/host/libmisterplex/owenb_menu.hpp \
		$(OWENB_RING_OBJS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		-I$(OWENB_RING_DIR)/include -o $@ $(ROOT)/tests/unit/test_chapters.cpp $(OWENB_RING_OBJS)
$(ROOT)/build/test_chapters_asan.ok: $(ROOT)/build/test_chapters_asan
	$(ROOT)/build/test_chapters_asan
	touch $@
$(ROOT)/build/test_chapters.mutant-check: $(ROOT)/build/test_chapters
	@$(ROOT)/build/test_chapters > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in MUT_CHAPTERS_FLAG_DEFAULT_ON MUT_CHAPTERS_MENU_WHEN_OFF \
		MUT_CHAPTERS_IGNORE_PICK MUT_CHAPTERS_DROP_PARSE MUT_CHAPTERS_SEEK_WHEN_OFF; do \
		$(CXX) $(CXXFLAGS) -I$(OWENB_RING_DIR)/include -D$$m -o $(ROOT)/build/test_chapters.$$m \
			$(ROOT)/tests/unit/test_chapters.cpp $(OWENB_RING_OBJS); \
		if $(ROOT)/build/test_chapters.$$m > $@.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
	done
	@echo "test_chapters: 5 mutants FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_chapters.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_chapters_asan.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_chapters.mutant-check

# SCRUB_THUMBS default off: seek bar unchanged, no BIF fetch.
# On: a part index paints the nearest preview above the bar. Five mutants must FAIL.
$(ROOT)/build/test_scrub_thumbs: $(ROOT)/tests/unit/test_scrub_thumbs.cpp \
		$(ROOT)/host/libmisterplex/scrub_thumbs.hpp \
		$(ROOT)/host/libmisterplex/owenb_osd_draw.hpp \
		$(ROOT)/host/libmisterplex/owenb_osd_renderer.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -I$(ROOT)/arm/misterplexd -o $@ \
		$(ROOT)/tests/unit/test_scrub_thumbs.cpp -lm
$(ROOT)/build/test_scrub_thumbs.ok: $(ROOT)/build/test_scrub_thumbs
	$(ROOT)/build/test_scrub_thumbs
	touch $@
$(ROOT)/build/test_scrub_thumbs_asan: $(ROOT)/tests/unit/test_scrub_thumbs.cpp \
		$(ROOT)/host/libmisterplex/scrub_thumbs.hpp \
		$(ROOT)/host/libmisterplex/owenb_osd_draw.hpp \
		$(ROOT)/host/libmisterplex/owenb_osd_renderer.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		$(OWENB_SESS_FLAGS) -I$(ROOT)/arm/misterplexd -o $@ \
		$(ROOT)/tests/unit/test_scrub_thumbs.cpp -lm
$(ROOT)/build/test_scrub_thumbs_asan.ok: $(ROOT)/build/test_scrub_thumbs_asan
	$(ROOT)/build/test_scrub_thumbs_asan
	touch $@
$(ROOT)/build/test_scrub_thumbs.mutant-check: $(ROOT)/build/test_scrub_thumbs
	@$(ROOT)/build/test_scrub_thumbs > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in MUT_SCRUB_THUMBS_FLAG_DEFAULT_ON MUT_SCRUB_THUMBS_IGNORE_INDEX \
		MUT_SCRUB_THUMBS_DROP_FRAME MUT_SCRUB_THUMBS_WHEN_OFF \
		MUT_SCRUB_THUMBS_BOX_WITHOUT_FRAME; do \
		$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -I$(ROOT)/arm/misterplexd -D$$m \
			-o $(ROOT)/build/test_scrub_thumbs.$$m $(ROOT)/tests/unit/test_scrub_thumbs.cpp -lm; \
		if $(ROOT)/build/test_scrub_thumbs.$$m > $@.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
	done
	@echo "test_scrub_thumbs: 5 mutants FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_scrub_thumbs.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_scrub_thumbs_asan.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_scrub_thumbs.mutant-check

# FORCED_SUBS default off. On: a default play selects a forced=1 subtitle
# (soft overlay, or the burn path already on this tip). Five mutants must FAIL.
$(ROOT)/build/test_forced_subs: $(ROOT)/tests/unit/test_forced_subs.cpp \
		$(ROOT)/host/libmisterplex/forced_subs.hpp $(ROOT)/host/libmisterplex/track_select.hpp \
		$(ROOT)/host/libmisterplex/soft_subs.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_forced_subs.cpp
$(ROOT)/build/test_forced_subs.ok: $(ROOT)/build/test_forced_subs
	$(ROOT)/build/test_forced_subs
	touch $@
$(ROOT)/build/test_forced_subs_asan: $(ROOT)/tests/unit/test_forced_subs.cpp \
		$(ROOT)/host/libmisterplex/forced_subs.hpp $(ROOT)/host/libmisterplex/track_select.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		-o $@ $(ROOT)/tests/unit/test_forced_subs.cpp
$(ROOT)/build/test_forced_subs_asan.ok: $(ROOT)/build/test_forced_subs_asan
	$(ROOT)/build/test_forced_subs_asan
	touch $@
$(ROOT)/build/test_forced_subs.mutant-check: $(ROOT)/build/test_forced_subs
	@$(ROOT)/build/test_forced_subs > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in MUT_FORCED_SUBS_FLAG_DEFAULT_ON MUT_FORCED_SUBS_OFF_STILL_PICKS \
		MUT_FORCED_SUBS_IGNORED MUT_FORCED_SUBS_OVERRIDE_PICK MUT_FORCED_SUBS_SKIP_IMAGE; do \
		$(CXX) $(CXXFLAGS) -D$$m -o $(ROOT)/build/test_forced_subs.$$m \
			$(ROOT)/tests/unit/test_forced_subs.cpp; \
		if $(ROOT)/build/test_forced_subs.$$m > $@.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
	done
	@echo "test_forced_subs: 5 mutants FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_forced_subs.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_forced_subs_asan.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_forced_subs.mutant-check

# REMEMBER_TRACKS default off. On: a setStreams pick is restored on the next
# episode (exact stream id, else language / codec / title, including image burn).
# The matched subtitle is named on the PLAY URL. Eight mutants must FAIL.
$(ROOT)/build/test_remember_tracks: $(ROOT)/tests/unit/test_remember_tracks.cpp \
		$(ROOT)/host/libmisterplex/remember_tracks.hpp $(ROOT)/host/libmisterplex/track_select.hpp \
		$(ROOT)/host/libmisterplex/forced_subs.hpp $(ROOT)/host/libmisterplex/soft_subs.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -o $@ $(ROOT)/tests/unit/test_remember_tracks.cpp
$(ROOT)/build/test_remember_tracks.ok: $(ROOT)/build/test_remember_tracks
	$(ROOT)/build/test_remember_tracks
	touch $@
$(ROOT)/build/test_remember_tracks_asan: $(ROOT)/tests/unit/test_remember_tracks.cpp \
		$(ROOT)/host/libmisterplex/remember_tracks.hpp $(ROOT)/host/libmisterplex/track_select.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
		-o $@ $(ROOT)/tests/unit/test_remember_tracks.cpp
$(ROOT)/build/test_remember_tracks_asan.ok: $(ROOT)/build/test_remember_tracks_asan
	$(ROOT)/build/test_remember_tracks_asan
	touch $@
$(ROOT)/build/test_remember_tracks.mutant-check: $(ROOT)/build/test_remember_tracks
	@$(ROOT)/build/test_remember_tracks > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in MUT_REMEMBER_TRACKS_FLAG_DEFAULT_ON MUT_REMEMBER_TRACKS_OFF_STILL_SAVES \
		MUT_REMEMBER_TRACKS_OFF_STILL_APPLIES MUT_REMEMBER_TRACKS_IGNORED \
		MUT_REMEMBER_TRACKS_SAME_ID_ONLY MUT_REMEMBER_TRACKS_DROP_OFF \
		MUT_REMEMBER_TRACKS_SKIP_IMAGE MUT_REMEMBER_TRACKS_OMIT_SUB_URL; do \
		$(CXX) $(CXXFLAGS) -pthread -D$$m -o $(ROOT)/build/test_remember_tracks.$$m \
			$(ROOT)/tests/unit/test_remember_tracks.cpp; \
		if $(ROOT)/build/test_remember_tracks.$$m > $@.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
	done
	@echo "test_remember_tracks: 8 mutants FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_remember_tracks.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_remember_tracks_asan.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_remember_tracks.mutant-check

# REMEMBER_PICTURE default off. On: the Program-menu picture mode is restored
# on the next episode of that show. No row is a no-op. Six mutants must FAIL.
$(ROOT)/build/test_remember_picture: $(ROOT)/tests/unit/test_remember_picture.cpp \
		$(ROOT)/host/libmisterplex/remember_picture.hpp $(ROOT)/host/libmisterplex/owenb_menu.hpp \
		$(ROOT)/host/libmisterplex/picture_14_9.hpp $(OWENB_RING_OBJS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -DMPX_SOURCE_ROOT='"$(ROOT)"' -I$(OWENB_RING_DIR)/include -o $@ \
		$(ROOT)/tests/unit/test_remember_picture.cpp $(OWENB_RING_OBJS)
$(ROOT)/build/test_remember_picture.ok: $(ROOT)/build/test_remember_picture
	$(ROOT)/build/test_remember_picture
	touch $@
$(ROOT)/build/test_remember_picture_asan: $(ROOT)/tests/unit/test_remember_picture.cpp \
		$(ROOT)/host/libmisterplex/remember_picture.hpp $(ROOT)/host/libmisterplex/owenb_menu.hpp \
		$(ROOT)/host/libmisterplex/picture_14_9.hpp $(OWENB_RING_OBJS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -pthread -DMPX_SOURCE_ROOT='"$(ROOT)"' \
		-I$(OWENB_RING_DIR)/include -o $@ $(ROOT)/tests/unit/test_remember_picture.cpp $(OWENB_RING_OBJS)
$(ROOT)/build/test_remember_picture_asan.ok: $(ROOT)/build/test_remember_picture_asan
	ASAN_OPTIONS=detect_leaks=1 $(ROOT)/build/test_remember_picture_asan
	touch $@
$(ROOT)/build/test_remember_picture.mutant-check: $(ROOT)/build/test_remember_picture
	@$(ROOT)/build/test_remember_picture > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in MUT_REMEMBER_PICTURE_FLAG_DEFAULT_ON MUT_REMEMBER_PICTURE_OFF_STILL_SAVES \
		MUT_REMEMBER_PICTURE_OFF_STILL_APPLIES MUT_REMEMBER_PICTURE_IGNORED \
		MUT_REMEMBER_PICTURE_MENU_WHEN_OFF MUT_REMEMBER_PICTURE_DROP_149 \
		MUT_REMEMBER_PICTURE_NO_DEBOUNCE MUT_REMEMBER_PICTURE_NO_STOP_FLUSH MUT_REMEMBER_PICTURE_NO_WRITE_LOCK; do \
		$(CXX) $(CXXFLAGS) -pthread -DMPX_SOURCE_ROOT='"$(ROOT)"' -I$(OWENB_RING_DIR)/include -D$$m \
			-o $(ROOT)/build/test_remember_picture.$$m \
			$(ROOT)/tests/unit/test_remember_picture.cpp $(OWENB_RING_OBJS); \
		if $(ROOT)/build/test_remember_picture.$$m > $@.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
	done
	@echo "test_remember_picture: 9 mutants FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_remember_picture.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_remember_picture_asan.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_remember_picture.mutant-check

# PICTURE_14_9 default off. On: SPS side crop to DAR 14:9, Letterbox fit unchanged
# when the flag is unset. Seven mutants must FAIL.
$(ROOT)/build/test_picture_14_9: $(ROOT)/tests/unit/test_picture_14_9.cpp \
		$(ROOT)/host/libmisterplex/picture_14_9.hpp $(ROOT)/host/libmisterplex/owenb_menu.hpp \
		$(ROOT)/host/libmisterplex/h264_sps.hpp $(OWENB_RING_OBJS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(OWENB_RING_DIR)/include -o $@ $(ROOT)/tests/unit/test_picture_14_9.cpp $(OWENB_RING_OBJS)
$(ROOT)/build/test_picture_14_9.ok: $(ROOT)/build/test_picture_14_9
	$(ROOT)/build/test_picture_14_9
	touch $@
$(ROOT)/build/test_picture_14_9_asan: $(ROOT)/tests/unit/test_picture_14_9.cpp \
		$(ROOT)/host/libmisterplex/picture_14_9.hpp $(OWENB_RING_OBJS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		-I$(OWENB_RING_DIR)/include -o $@ $(ROOT)/tests/unit/test_picture_14_9.cpp $(OWENB_RING_OBJS)
$(ROOT)/build/test_picture_14_9_asan.ok: $(ROOT)/build/test_picture_14_9_asan
	ASAN_OPTIONS=detect_leaks=1 $(ROOT)/build/test_picture_14_9_asan
	touch $@
$(ROOT)/build/test_picture_14_9.mutant-check: $(ROOT)/build/test_picture_14_9
	@$(ROOT)/build/test_picture_14_9 > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in MUT_PICTURE149_DEFAULT_ON MUT_PICTURE149_NO_CROP MUT_PICTURE149_IGNORE_SAR \
		MUT_PICTURE149_CROP_ALL MUT_PICTURE149_MENU_WHEN_OFF MUT_PICTURE149_SPS_KEEP \
		MUT_PICTURE149_FORCE_LB_OFF; do \
		$(CXX) $(CXXFLAGS) -I$(OWENB_RING_DIR)/include -D$$m -o $(ROOT)/build/test_picture_14_9.$$m \
			$(ROOT)/tests/unit/test_picture_14_9.cpp $(OWENB_RING_OBJS); \
		if $(ROOT)/build/test_picture_14_9.$$m > $@.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
	done
	@echo "test_picture_14_9: 7 mutants FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_picture_14_9.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_picture_14_9_asan.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_picture_14_9.mutant-check

# REPORT_CODE default off. On: Ready-card short code plus a redacted log pack.
# Six mutants must FAIL. Unset stays off (no REPORT_CODE=1 in the example).
$(ROOT)/build/test_report_code: $(ROOT)/tests/unit/test_report_code.cpp \
		$(ROOT)/host/libmisterplex/report_code.hpp $(ROOT)/host/libmisterplex/phone_status_http.hpp \
		$(ROOT)/host/libmisterplex/owenb_osd_draw.hpp $(ROOT)/arm/misterplexd/main.cpp \
		$(ROOT)/arm/misterplexd/media_player.cpp $(ROOT)/arm/misterplexd/companion.cpp \
		$(ROOT)/assets/misterplex.conf.example
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -o $@ $(ROOT)/tests/unit/test_report_code.cpp
$(ROOT)/build/test_report_code.ok: $(ROOT)/build/test_report_code
	$(ROOT)/build/test_report_code
	touch $@
$(ROOT)/build/test_report_code_asan: $(ROOT)/tests/unit/test_report_code.cpp \
		$(ROOT)/host/libmisterplex/report_code.hpp $(ROOT)/host/libmisterplex/phone_status_http.hpp \
		$(ROOT)/host/libmisterplex/owenb_osd_draw.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		$(OWENB_SESS_FLAGS) -o $@ $(ROOT)/tests/unit/test_report_code.cpp
$(ROOT)/build/test_report_code_asan.ok: $(ROOT)/build/test_report_code_asan
	ASAN_OPTIONS=detect_leaks=1 $(ROOT)/build/test_report_code_asan
	touch $@
$(ROOT)/build/test_report_code.mutant-check: $(ROOT)/build/test_report_code
	@$(ROOT)/build/test_report_code > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in MUT_REPORT_CODE_DEFAULT_ON MUT_REPORT_CODE_EMPTY MUT_REPORT_CODE_NO_PACK \
		MUT_REPORT_CODE_SKIP_REDACT MUT_REPORT_CODE_NO_HINT MUT_REPORT_CODE_NO_PHONE \
		MUT_PLEXLINK_REPORT_OVER_LINK MUT_PLEXLINK_REPORT_OVER_ERROR; do \
		$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -D$$m -o $(ROOT)/build/test_report_code.$$m \
			$(ROOT)/tests/unit/test_report_code.cpp; \
		if $(ROOT)/build/test_report_code.$$m > $@.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
		case $$m in \
			MUT_REPORT_CODE_DEFAULT_ON) c=F0 ;; \
			MUT_REPORT_CODE_EMPTY) c=F4 ;; \
			MUT_REPORT_CODE_NO_PACK) c=F2 ;; \
			MUT_REPORT_CODE_SKIP_REDACT) c=F3 ;; \
			MUT_REPORT_CODE_NO_HINT) c=F1 ;; \
			MUT_REPORT_CODE_NO_PHONE) c=F5 ;; \
			MUT_PLEXLINK_REPORT_OVER_LINK) c=F6 ;; MUT_PLEXLINK_REPORT_OVER_ERROR) c=F7 ;; \
		esac; \
		grep -q "FAIL: $$c " $@.$$m.log || { echo "MUTANT $$m KILLED BY THE WRONG CHECK (want $$c)"; cat $@.$$m.log; exit 1; }; \
	done
	@echo "test_report_code: 8 mutants FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_report_code.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_report_code_asan.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_report_code.mutant-check

# AMBIENT default off. On: idle field cycles library art. Six mutants must FAIL.
# Unset stays off (no AMBIENT=1 in the example).
$(ROOT)/build/test_ambient: $(ROOT)/tests/unit/test_ambient.cpp \
		$(ROOT)/host/libmisterplex/ambient.hpp \
		$(ROOT)/host/libmisterplex/owenb_osd_draw.hpp \
		$(ROOT)/host/libmisterplex/owenb_osd_renderer.hpp \
		$(ROOT)/arm/misterplexd/main.cpp \
		$(ROOT)/arm/misterplexd/media_player.cpp \
		$(ROOT)/assets/misterplex.conf.example
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -I$(ROOT)/arm/misterplexd -o $@ \
		$(ROOT)/tests/unit/test_ambient.cpp -lm
$(ROOT)/build/test_ambient.ok: $(ROOT)/build/test_ambient
	$(ROOT)/build/test_ambient
	touch $@
$(ROOT)/build/test_ambient_asan: $(ROOT)/tests/unit/test_ambient.cpp \
		$(ROOT)/host/libmisterplex/ambient.hpp \
		$(ROOT)/host/libmisterplex/owenb_osd_draw.hpp \
		$(ROOT)/host/libmisterplex/owenb_osd_renderer.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		$(OWENB_SESS_FLAGS) -I$(ROOT)/arm/misterplexd -o $@ \
		$(ROOT)/tests/unit/test_ambient.cpp -lm
$(ROOT)/build/test_ambient_asan.ok: $(ROOT)/build/test_ambient_asan
	ASAN_OPTIONS=detect_leaks=1 $(ROOT)/build/test_ambient_asan
	touch $@
$(ROOT)/build/test_ambient.mutant-check: $(ROOT)/build/test_ambient
	@$(ROOT)/build/test_ambient > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in MUT_AMBIENT_DEFAULT_ON MUT_AMBIENT_NO_PAINT MUT_AMBIENT_NO_CYCLE \
		MUT_AMBIENT_PAINT_WHEN_OFF MUT_AMBIENT_DROP_ART MUT_AMBIENT_TOKEN_LEAK; do \
		$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -I$(ROOT)/arm/misterplexd -D$$m \
			-o $(ROOT)/build/test_ambient.$$m $(ROOT)/tests/unit/test_ambient.cpp -lm; \
		if $(ROOT)/build/test_ambient.$$m > $@.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
		case $$m in \
			MUT_AMBIENT_DEFAULT_ON) c=F0 ;; \
			MUT_AMBIENT_NO_PAINT) c=F1 ;; \
			MUT_AMBIENT_NO_CYCLE) c=F2 ;; \
			MUT_AMBIENT_PAINT_WHEN_OFF) c=F3 ;; \
			MUT_AMBIENT_DROP_ART) c=F4 ;; \
			MUT_AMBIENT_TOKEN_LEAK) c=F5 ;; \
		esac; \
		grep -q "FAIL: $$c " $@.$$m.log || { echo "MUTANT $$m KILLED BY THE WRONG CHECK (want $$c)"; cat $@.$$m.log; exit 1; }; \
	done
	@echo "test_ambient: 6 mutants FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_ambient.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_ambient_asan.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_ambient.mutant-check

# ZAPAROO default off. On: a card text becomes playMedia for one library item.
# Six mutants must FAIL. Unset stays off (no ZAPAROO=1 in the example).
$(ROOT)/build/test_zaparoo: $(ROOT)/tests/unit/test_zaparoo.cpp \
		$(ROOT)/host/libmisterplex/zaparoo.hpp \
		$(ROOT)/arm/misterplexd/main.cpp \
		$(ROOT)/arm/misterplexd/companion.cpp \
		$(ROOT)/assets/misterplex.conf.example
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -DMPX_SOURCE_ROOT='"$(ROOT)"' -I$(ROOT)/arm/misterplexd -o $@ \
		$(ROOT)/tests/unit/test_zaparoo.cpp
$(ROOT)/build/test_zaparoo.ok: $(ROOT)/build/test_zaparoo
	$(ROOT)/build/test_zaparoo
	touch $@
$(ROOT)/build/test_zaparoo_asan: $(ROOT)/tests/unit/test_zaparoo.cpp \
		$(ROOT)/host/libmisterplex/zaparoo.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		-DMPX_SOURCE_ROOT='"$(ROOT)"' -I$(ROOT)/arm/misterplexd -o $@ \
		$(ROOT)/tests/unit/test_zaparoo.cpp
$(ROOT)/build/test_zaparoo_asan.ok: $(ROOT)/build/test_zaparoo_asan
	ASAN_OPTIONS=detect_leaks=1 $(ROOT)/build/test_zaparoo_asan
	touch $@
$(ROOT)/build/test_zaparoo.mutant-check: $(ROOT)/build/test_zaparoo
	@$(ROOT)/build/test_zaparoo > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in MUT_ZAPAROO_DEFAULT_ON MUT_ZAPAROO_PLAY_WHEN_OFF MUT_ZAPAROO_IGNORE_CARD \
		MUT_ZAPAROO_OPEN_URL MUT_ZAPAROO_TOKEN_LEAK MUT_ZAPAROO_FOREIGN_SYSTEM; do \
		$(CXX) $(CXXFLAGS) -DMPX_SOURCE_ROOT='"$(ROOT)"' -I$(ROOT)/arm/misterplexd -D$$m \
			-o $(ROOT)/build/test_zaparoo.$$m $(ROOT)/tests/unit/test_zaparoo.cpp; \
		if $(ROOT)/build/test_zaparoo.$$m > $@.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
		case $$m in \
			MUT_ZAPAROO_DEFAULT_ON) c=F0 ;; \
			MUT_ZAPAROO_PLAY_WHEN_OFF) c=F1 ;; \
			MUT_ZAPAROO_IGNORE_CARD) c=F2 ;; \
			MUT_ZAPAROO_OPEN_URL) c=F3 ;; \
			MUT_ZAPAROO_TOKEN_LEAK) c=F4 ;; \
			MUT_ZAPAROO_FOREIGN_SYSTEM) c=F5 ;; \
		esac; \
		grep -q "FAIL: $$c " $@.$$m.log || { echo "MUTANT $$m KILLED BY THE WRONG CHECK (want $$c)"; cat $@.$$m.log; exit 1; }; \
	done
	@echo "test_zaparoo: 6 mutants FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_zaparoo.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_zaparoo_asan.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_zaparoo.mutant-check

# FILTER_4_3 default off. On: the library 4:3 chip hides widescreen titles.
# Six mutants must FAIL. Unset stays off (no FILTER_4_3=1 in the example).
$(ROOT)/build/test_filter_4_3: $(ROOT)/tests/unit/test_filter_4_3.cpp $(PLEXUI_SRC) \
		$(ROOT)/host/libmisterplex/plexui/filter_4_3.hpp \
		$(ROOT)/arm/misterplexd/main.cpp \
		$(ROOT)/assets/misterplex.conf.example
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -DMPX_SOURCE_ROOT='"$(ROOT)"' \
		$(MPLEX_INC) -o $@ $(ROOT)/tests/unit/test_filter_4_3.cpp $(PLEXUI_SRC)
$(ROOT)/build/test_filter_4_3.ok: $(ROOT)/build/test_filter_4_3
	$(ROOT)/build/test_filter_4_3
	touch $@
$(ROOT)/build/test_filter_4_3_asan: $(ROOT)/tests/unit/test_filter_4_3.cpp $(PLEXUI_SRC) \
		$(ROOT)/host/libmisterplex/plexui/filter_4_3.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
		-DMPX_PLEXUI_UNIT_TEST -DMPX_SOURCE_ROOT='"$(ROOT)"' $(MPLEX_INC) -o $@ \
		$(ROOT)/tests/unit/test_filter_4_3.cpp $(PLEXUI_SRC)
$(ROOT)/build/test_filter_4_3_asan.ok: $(ROOT)/build/test_filter_4_3_asan
	ASAN_OPTIONS=detect_leaks=1 $(ROOT)/build/test_filter_4_3_asan
	touch $@
$(ROOT)/build/test_filter_4_3.mutant-check: $(ROOT)/build/test_filter_4_3
	@$(ROOT)/build/test_filter_4_3 > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in MUT_FILTER43_DEFAULT_ON MUT_FILTER43_FILTER_WHEN_OFF MUT_FILTER43_KEEP_WIDE \
		MUT_FILTER43_IGNORE_TOGGLE MUT_FILTER43_DROP_ASPECT MUT_FILTER43_CHIP_WHEN_OFF; do \
		$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -DMPX_SOURCE_ROOT='"$(ROOT)"' \
			$(MPLEX_INC) -D$$m -o $(ROOT)/build/test_filter_4_3.$$m \
			$(ROOT)/tests/unit/test_filter_4_3.cpp $(PLEXUI_SRC); \
		if $(ROOT)/build/test_filter_4_3.$$m > $@.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
		case $$m in \
			MUT_FILTER43_DEFAULT_ON) c=F0 ;; \
			MUT_FILTER43_FILTER_WHEN_OFF) c=F1 ;; \
			MUT_FILTER43_KEEP_WIDE) c=F5 ;; \
			MUT_FILTER43_IGNORE_TOGGLE) c=F3 ;; \
			MUT_FILTER43_DROP_ASPECT) c=F6 ;; \
			MUT_FILTER43_CHIP_WHEN_OFF) c=F7 ;; \
		esac; \
		grep -q "FAIL: $$c " $@.$$m.log || { echo "MUTANT $$m KILLED BY THE WRONG CHECK (want $$c)"; cat $@.$$m.log; exit 1; }; \
	done
	@echo "test_filter_4_3: 6 mutants FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_filter_4_3.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_filter_4_3_asan.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_filter_4_3.mutant-check

# IN_PLAY_OPTIONS default off. On: Program menu Options and Play from start.
# Seven mutants must FAIL. Unset stays off (no IN_PLAY_OPTIONS=1 in the example).
$(ROOT)/build/test_in_play_options: $(ROOT)/tests/unit/test_in_play_options.cpp \
		$(ROOT)/host/libmisterplex/in_play_options.hpp $(ROOT)/host/libmisterplex/owenb_menu.hpp \
		$(ROOT)/arm/misterplexd/main.cpp $(ROOT)/arm/misterplexd/media_player.cpp \
		$(ROOT)/assets/misterplex.conf.example $(OWENB_RING_OBJS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(OWENB_RING_DIR)/include -DMPX_SOURCE_ROOT='"$(ROOT)"' -o $@ \
		$(ROOT)/tests/unit/test_in_play_options.cpp $(OWENB_RING_OBJS)
$(ROOT)/build/test_in_play_options.ok: $(ROOT)/build/test_in_play_options
	$(ROOT)/build/test_in_play_options
	touch $@
$(ROOT)/build/test_in_play_options_asan: $(ROOT)/tests/unit/test_in_play_options.cpp \
		$(ROOT)/host/libmisterplex/in_play_options.hpp $(ROOT)/host/libmisterplex/owenb_menu.hpp \
		$(OWENB_RING_OBJS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		-I$(OWENB_RING_DIR)/include -DMPX_SOURCE_ROOT='"$(ROOT)"' -o $@ \
		$(ROOT)/tests/unit/test_in_play_options.cpp $(OWENB_RING_OBJS)
$(ROOT)/build/test_in_play_options_asan.ok: $(ROOT)/build/test_in_play_options_asan
	ASAN_OPTIONS=detect_leaks=1 $(ROOT)/build/test_in_play_options_asan
	touch $@
$(ROOT)/build/test_in_play_options.mutant-check: $(ROOT)/build/test_in_play_options
	@$(ROOT)/build/test_in_play_options > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in MUT_INPLAY_DEFAULT_ON MUT_INPLAY_INFO_WHEN_OFF MUT_INPLAY_MENU_WHEN_OFF \
		MUT_INPLAY_SEEK_WHEN_OFF MUT_INPLAY_NO_PAGE MUT_INPLAY_IGNORE_START \
		MUT_INPLAY_DROP_INFO; do \
		$(CXX) $(CXXFLAGS) -I$(OWENB_RING_DIR)/include -DMPX_SOURCE_ROOT='"$(ROOT)"' -D$$m \
			-o $(ROOT)/build/test_in_play_options.$$m \
			$(ROOT)/tests/unit/test_in_play_options.cpp $(OWENB_RING_OBJS); \
		if $(ROOT)/build/test_in_play_options.$$m > $@.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
		case $$m in \
			MUT_INPLAY_DEFAULT_ON) c=F0 ;; \
			MUT_INPLAY_INFO_WHEN_OFF) c=F1 ;; \
			MUT_INPLAY_MENU_WHEN_OFF) c=F2 ;; \
			MUT_INPLAY_SEEK_WHEN_OFF) c=F3 ;; \
			MUT_INPLAY_NO_PAGE) c=F4 ;; \
			MUT_INPLAY_IGNORE_START) c=F5 ;; \
			MUT_INPLAY_DROP_INFO) c=F6 ;; \
		esac; \
		grep -q "FAIL: $$c " $@.$$m.log || { echo "MUTANT $$m KILLED BY THE WRONG CHECK (want $$c)"; cat $@.$$m.log; exit 1; }; \
	done
	@echo "test_in_play_options: 7 mutants FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_in_play_options.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_in_play_options_asan.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_in_play_options.mutant-check

# WATCHED_BADGES default off. On: a finished title gets a check on its poster.
# Six mutants must FAIL. Unset stays off (no WATCHED_BADGES=1 in the example).
$(ROOT)/build/test_watched_badges: $(ROOT)/tests/unit/test_watched_badges.cpp \
		$(ROOT)/host/libmisterplex/watched_badges.hpp \
		$(ROOT)/arm/misterplexd/main.cpp \
		$(ROOT)/host/libmisterplex/plexui/plexui_render.cpp \
		$(ROOT)/assets/misterplex.conf.example
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -DMPX_SOURCE_ROOT='"$(ROOT)"' -o $@ \
		$(ROOT)/tests/unit/test_watched_badges.cpp
$(ROOT)/build/test_watched_badges.ok: $(ROOT)/build/test_watched_badges
	$(ROOT)/build/test_watched_badges
	touch $@
$(ROOT)/build/test_watched_badges_asan: $(ROOT)/tests/unit/test_watched_badges.cpp \
		$(ROOT)/host/libmisterplex/watched_badges.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		-DMPX_SOURCE_ROOT='"$(ROOT)"' -o $@ \
		$(ROOT)/tests/unit/test_watched_badges.cpp
$(ROOT)/build/test_watched_badges_asan.ok: $(ROOT)/build/test_watched_badges_asan
	ASAN_OPTIONS=detect_leaks=1 $(ROOT)/build/test_watched_badges_asan
	touch $@
$(ROOT)/build/test_watched_badges.mutant-check: $(ROOT)/build/test_watched_badges
	@$(ROOT)/build/test_watched_badges > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in MUT_WBADGE_DEFAULT_ON MUT_WBADGE_SHOW_WHEN_OFF MUT_WBADGE_PAINT_WHEN_OFF \
		MUT_WBADGE_NEVER MUT_WBADGE_UNWATCHED MUT_WBADGE_PROGRESS; do \
		$(CXX) $(CXXFLAGS) -DMPX_SOURCE_ROOT='"$(ROOT)"' -D$$m \
			-o $(ROOT)/build/test_watched_badges.$$m \
			$(ROOT)/tests/unit/test_watched_badges.cpp; \
		if $(ROOT)/build/test_watched_badges.$$m > $@.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
		case $$m in \
			MUT_WBADGE_DEFAULT_ON) c=F0 ;; \
			MUT_WBADGE_SHOW_WHEN_OFF) c=F1 ;; \
			MUT_WBADGE_PAINT_WHEN_OFF) c=F2 ;; \
			MUT_WBADGE_NEVER) c=F3 ;; \
			MUT_WBADGE_UNWATCHED) c=F4 ;; \
			MUT_WBADGE_PROGRESS) c=F5 ;; \
		esac; \
		grep -q "FAIL: $$c " $@.$$m.log || { echo "MUTANT $$m KILLED BY THE WRONG CHECK (want $$c)"; cat $@.$$m.log; exit 1; }; \
	done
	@echo "test_watched_badges: 6 mutants FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_watched_badges.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_watched_badges_asan.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_watched_badges.mutant-check

# CRT_LOOK default off. On: the on-device UI canvas uses an HDMI CRT-look preset.
# Six mutants must FAIL. Unset stays off (no CRT_LOOK=1 in the example).
$(ROOT)/build/test_crt_look: $(ROOT)/tests/unit/test_crt_look.cpp \
		$(ROOT)/host/libmisterplex/crt_look.hpp \
		$(ROOT)/arm/misterplexd/main.cpp \
		$(ROOT)/host/libmisterplex/plexui/plexui_render.cpp \
		$(ROOT)/assets/misterplex.conf.example \
		$(ROOT)/docs/crt-look-hdmi.md
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -DMPX_SOURCE_ROOT='"$(ROOT)"' -o $@ \
		$(ROOT)/tests/unit/test_crt_look.cpp
$(ROOT)/build/test_crt_look.ok: $(ROOT)/build/test_crt_look
	$(ROOT)/build/test_crt_look
	touch $@
$(ROOT)/build/test_crt_look_asan: $(ROOT)/tests/unit/test_crt_look.cpp \
		$(ROOT)/host/libmisterplex/crt_look.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		-DMPX_SOURCE_ROOT='"$(ROOT)"' -o $@ \
		$(ROOT)/tests/unit/test_crt_look.cpp
$(ROOT)/build/test_crt_look_asan.ok: $(ROOT)/build/test_crt_look_asan
	ASAN_OPTIONS=detect_leaks=1 $(ROOT)/build/test_crt_look_asan
	touch $@
$(ROOT)/build/test_crt_look.mutant-check: $(ROOT)/build/test_crt_look
	@$(ROOT)/build/test_crt_look > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in MUT_CRTLOOK_DEFAULT_ON MUT_CRTLOOK_APPLY_WHEN_OFF MUT_CRTLOOK_PAINT_FIRST \
		MUT_CRTLOOK_SKIP_WHEN_ON MUT_CRTLOOK_EVEN_LINES MUT_CRTLOOK_IGNORE_PRESET; do \
		$(CXX) $(CXXFLAGS) -DMPX_SOURCE_ROOT='"$(ROOT)"' -D$$m \
			-o $(ROOT)/build/test_crt_look.$$m \
			$(ROOT)/tests/unit/test_crt_look.cpp; \
		if $(ROOT)/build/test_crt_look.$$m > $@.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
		case $$m in \
			MUT_CRTLOOK_DEFAULT_ON) c=F0 ;; \
			MUT_CRTLOOK_APPLY_WHEN_OFF) c=F1 ;; \
			MUT_CRTLOOK_PAINT_FIRST) c=F2 ;; \
			MUT_CRTLOOK_SKIP_WHEN_ON) c=F3 ;; \
			MUT_CRTLOOK_EVEN_LINES) c=F4 ;; \
			MUT_CRTLOOK_IGNORE_PRESET) c=F5 ;; \
		esac; \
		grep -q "FAIL: $$c " $@.$$m.log || { echo "MUTANT $$m KILLED BY THE WRONG CHECK (want $$c)"; cat $@.$$m.log; exit 1; }; \
	done
	@echo "test_crt_look: 6 mutants FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_crt_look.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_crt_look_asan.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_crt_look.mutant-check

# IDLE_MENU_KEY default off. On: keyboard P is Aux on Ready idle only.
# Four mutants must FAIL. Unset stays off (no IDLE_MENU_KEY=1 in the example).
$(ROOT)/build/test_idle_menu_key: $(ROOT)/tests/unit/test_idle_menu_key.cpp \
		$(ROOT)/host/libmisterplex/idle_menu_key.hpp $(ROOT)/host/libmisterplex/owenb_input.hpp \
		$(ROOT)/host/libmisterplex/owenb_menu.hpp $(ROOT)/host/libmisterplex/owenb_plexui_nav.hpp \
		$(ROOT)/arm/misterplexd/main.cpp $(ROOT)/arm/misterplexd/media_player.cpp \
		$(ROOT)/assets/misterplex.conf.example \
		$(ROOT)/fpga/owenb-combo-s9/PlexOB_480p/vendor/h264/emu.sv $(OWENB_RING_OBJS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(OWENB_RING_DIR)/include -DMPX_SOURCE_ROOT='"$(ROOT)"' -o $@ \
		$(ROOT)/tests/unit/test_idle_menu_key.cpp $(OWENB_RING_OBJS)
$(ROOT)/build/test_idle_menu_key.ok: $(ROOT)/build/test_idle_menu_key
	$(ROOT)/build/test_idle_menu_key
	touch $@
$(ROOT)/build/test_idle_menu_key_asan: $(ROOT)/tests/unit/test_idle_menu_key.cpp \
		$(ROOT)/host/libmisterplex/idle_menu_key.hpp $(ROOT)/host/libmisterplex/owenb_input.hpp \
		$(ROOT)/host/libmisterplex/owenb_menu.hpp $(ROOT)/host/libmisterplex/owenb_plexui_nav.hpp \
		$(ROOT)/fpga/owenb-combo-s9/PlexOB_480p/vendor/h264/emu.sv \
		$(OWENB_RING_OBJS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		-I$(OWENB_RING_DIR)/include -DMPX_SOURCE_ROOT='"$(ROOT)"' -o $@ \
		$(ROOT)/tests/unit/test_idle_menu_key.cpp $(OWENB_RING_OBJS)
$(ROOT)/build/test_idle_menu_key_asan.ok: $(ROOT)/build/test_idle_menu_key_asan
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 $(ROOT)/build/test_idle_menu_key_asan
	touch $@
$(ROOT)/build/test_idle_menu_key.mutant-check: $(ROOT)/build/test_idle_menu_key
	@$(ROOT)/build/test_idle_menu_key > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in MUT_IDLE_MENU_KEY_DEFAULT_ON MUT_IDLE_MENU_KEY_OFF_BINDS \
		MUT_IDLE_MENU_KEY_IGNORED MUT_IDLE_MENU_KEY_NOT_IDLE; do \
		$(CXX) $(CXXFLAGS) -I$(OWENB_RING_DIR)/include -DMPX_SOURCE_ROOT='"$(ROOT)"' -D$$m \
			-o $(ROOT)/build/test_idle_menu_key.$$m \
			$(ROOT)/tests/unit/test_idle_menu_key.cpp $(OWENB_RING_OBJS); \
		if $(ROOT)/build/test_idle_menu_key.$$m > $@.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
		case $$m in \
			MUT_IDLE_MENU_KEY_DEFAULT_ON) c=F0 ;; \
			MUT_IDLE_MENU_KEY_OFF_BINDS) c=F1 ;; \
			MUT_IDLE_MENU_KEY_IGNORED) c=F2 ;; \
			MUT_IDLE_MENU_KEY_NOT_IDLE) c=F3 ;; \
		esac; \
		grep -q "FAIL: $$c " $@.$$m.log || { echo "MUTANT $$m KILLED BY THE WRONG CHECK (want $$c)"; cat $@.$$m.log; exit 1; }; \
	done
	@echo "test_idle_menu_key: 4 mutants FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_idle_menu_key.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_idle_menu_key_asan.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_idle_menu_key.mutant-check

# GH#9: --version exits before ProcessSingleton / FPGA. Hung probe = color bars, no :3005.
DVER_DEPS := $(ROOT)/tests/unit/test_daemon_version.cpp $(ROOT)/host/libmisterplex/daemon_version.hpp \
	$(ROOT)/arm/misterplexd/main.cpp
$(ROOT)/build/test_daemon_version: $(DVER_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -DMPX_SOURCE_ROOT='"$(ROOT)"' -o $@ $(ROOT)/tests/unit/test_daemon_version.cpp
$(ROOT)/build/test_daemon_version.ok: $(ROOT)/build/test_daemon_version
	$(ROOT)/build/test_daemon_version
	touch $@
$(ROOT)/build/test_daemon_version_asan: $(DVER_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		-DMPX_SOURCE_ROOT='"$(ROOT)"' -o $@ $(ROOT)/tests/unit/test_daemon_version.cpp
$(ROOT)/build/test_daemon_version_asan.ok: $(ROOT)/build/test_daemon_version_asan
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 $(ROOT)/build/test_daemon_version_asan
	touch $@
$(ROOT)/build/test_daemon_version.mutant-check: $(ROOT)/build/test_daemon_version
	@$(ROOT)/build/test_daemon_version > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@$(CXX) $(CXXFLAGS) -DMPX_SOURCE_ROOT='"$(ROOT)"' -DMUT_VERSION_FALL_THROUGH \
		-o $(ROOT)/build/test_daemon_version.mut $(ROOT)/tests/unit/test_daemon_version.cpp
	@if $(ROOT)/build/test_daemon_version.mut > $@.mut.log 2>&1; then \
		echo "test_daemon_version: MUT_VERSION_FALL_THROUGH SURVIVED"; exit 1; fi
	@echo "test_daemon_version: MUT_VERSION_FALL_THROUGH FAIL as required" | tee $@
	@rm -f $(ROOT)/build/test_daemon_version.mut
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_daemon_version.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_daemon_version_asan.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_daemon_version.mutant-check

# GH#13: 1280x960 native-hd overlay must clamp to fb0 3686400.
FB13_DEPS := $(ROOT)/tests/unit/test_overlay_fb_fit.cpp $(ROOT)/host/libmisterplex/owenb_osd_layout.hpp \
	$(ROOT)/host/libmisterplex/owenb_overlay.hpp $(ROOT)/host/libmisterplex/owenb_osd_renderer.hpp \
	$(OWENB_RING_OBJS)
$(ROOT)/build/test_overlay_fb_fit: $(FB13_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -o $@ $(ROOT)/tests/unit/test_overlay_fb_fit.cpp $(OWENB_RING_OBJS) -lm
$(ROOT)/build/test_overlay_fb_fit.ok: $(ROOT)/build/test_overlay_fb_fit
	$(ROOT)/build/test_overlay_fb_fit
	touch $@
$(ROOT)/build/test_overlay_fb_fit_asan: $(FB13_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		$(OWENB_SESS_FLAGS) -o $@ $(ROOT)/tests/unit/test_overlay_fb_fit.cpp $(OWENB_RING_OBJS) -lm
$(ROOT)/build/test_overlay_fb_fit_asan.ok: $(ROOT)/build/test_overlay_fb_fit_asan
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 $(ROOT)/build/test_overlay_fb_fit_asan
	touch $@
$(ROOT)/build/test_overlay_fb_fit.mutant-check: $(ROOT)/build/test_overlay_fb_fit
	@$(ROOT)/build/test_overlay_fb_fit > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -DMUT_GH13_NO_FB_CLAMP \
		-o $(ROOT)/build/test_overlay_fb_fit.mut $(ROOT)/tests/unit/test_overlay_fb_fit.cpp $(OWENB_RING_OBJS) -lm
	@if $(ROOT)/build/test_overlay_fb_fit.mut > $@.mut.log 2>&1; then \
		echo "test_overlay_fb_fit: MUT_GH13_NO_FB_CLAMP SURVIVED"; exit 1; fi
	@$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -DMUT_GH46_HD_SKIP_PCT \
		-o $(ROOT)/build/test_overlay_fb_fit.mut46 $(ROOT)/tests/unit/test_overlay_fb_fit.cpp $(OWENB_RING_OBJS) -lm
	@if $(ROOT)/build/test_overlay_fb_fit.mut46 > $@.mut46.log 2>&1; then \
		echo "test_overlay_fb_fit: MUT_GH46_HD_SKIP_PCT SURVIVED"; exit 1; fi
	@echo "test_overlay_fb_fit: MUT_GH13_NO_FB_CLAMP MUT_GH46_HD_SKIP_PCT FAIL as required" | tee $@
	@rm -f $(ROOT)/build/test_overlay_fb_fit.mut $(ROOT)/build/test_overlay_fb_fit.mut46
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_overlay_fb_fit.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_overlay_fb_fit_asan.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_overlay_fb_fit.mutant-check

# REMOTE_H264_TRANSCODE (#207) default off. Four mutants must FAIL with their own check.
RH264_DEPS := $(ROOT)/tests/unit/test_remote_h264_transcode.cpp $(ROOT)/host/libmisterplex/remote_h264_transcode.hpp \
	$(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/arm/misterplexd/main.cpp $(ROOT)/assets/misterplex.conf.example
$(ROOT)/build/test_remote_h264_transcode: $(RH264_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -DMPX_SOURCE_ROOT='"$(ROOT)"' -o $@ $(ROOT)/tests/unit/test_remote_h264_transcode.cpp
$(ROOT)/build/test_remote_h264_transcode.ok: $(ROOT)/build/test_remote_h264_transcode
	$(ROOT)/build/test_remote_h264_transcode
	touch $@
$(ROOT)/build/test_remote_h264_transcode_asan: $(RH264_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		-DMPX_SOURCE_ROOT='"$(ROOT)"' -o $@ $(ROOT)/tests/unit/test_remote_h264_transcode.cpp
$(ROOT)/build/test_remote_h264_transcode_asan.ok: $(ROOT)/build/test_remote_h264_transcode_asan
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 $(ROOT)/build/test_remote_h264_transcode_asan
	touch $@
$(ROOT)/build/test_remote_h264_transcode.mutant-check: $(ROOT)/build/test_remote_h264_transcode
	@$(ROOT)/build/test_remote_h264_transcode > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in MUT_RH264_DEFAULT_ON MUT_RH264_OFF_BINDS MUT_RH264_IGNORED MUT_RH264_WRONG_CONTEXT MUT_RSTART_DIRECT MUT_RFPGA_OFF MUT_ROSD_MISS MUT_RHDR_TRUNC MUT_RDIRECT_10BIT MUT_RDIRECT_HD MUT_RDIRECT_OFF_TAKES MUT_RDIRECT_FPS MUT_RDIRECT_FPS0 MUT_RDIRECT_BD_UNKNOWN MUT_RDIRECT_FB_LOOP MUT_RDIRECT_FB_NOPIC MUT_RDIRECT_LEVEL_CAP MUT_RDIRECT_REFS_UNKNOWN MUT_RDIRECT_REFS_CAP MUT_RDIRECT_SEEK_NOARM MUT_RDIRECT_GEN_RACE MUT_RMISS_AS_XCODE; do \
		$(CXX) $(CXXFLAGS) -DMPX_SOURCE_ROOT='"$(ROOT)"' -D$$m -o $(ROOT)/build/test_remote_h264_transcode.$$m \
			$(ROOT)/tests/unit/test_remote_h264_transcode.cpp; \
		if $(ROOT)/build/test_remote_h264_transcode.$$m > $@.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
		case $$m in MUT_RH264_DEFAULT_ON) c=F0 ;; MUT_RH264_OFF_BINDS) c=F1 ;; \
			MUT_RH264_IGNORED) c=F2 ;; MUT_RH264_WRONG_CONTEXT) c=F3 ;; MUT_RSTART_DIRECT) c=F6 ;; MUT_RFPGA_OFF) c=F7 ;; MUT_ROSD_MISS) c=F8 ;; MUT_RHDR_TRUNC) c=F10 ;; MUT_RDIRECT_10BIT|MUT_RDIRECT_HD|MUT_RDIRECT_OFF_TAKES|MUT_RDIRECT_FPS) c=F11 ;; MUT_RDIRECT_FPS0) c=F12 ;; MUT_RDIRECT_BD_UNKNOWN) c=F13 ;; MUT_RDIRECT_FB_LOOP|MUT_RDIRECT_FB_NOPIC) c=F14 ;; MUT_RDIRECT_LEVEL_CAP) c=F15 ;; MUT_RDIRECT_REFS_UNKNOWN|MUT_RDIRECT_REFS_CAP) c=F16 ;; MUT_RDIRECT_SEEK_NOARM) c=F17 ;; MUT_RDIRECT_GEN_RACE) c=F18 ;; MUT_RMISS_AS_XCODE) c=F19 ;; esac; \
		grep -q "FAIL: $$c " $@.$$m.log || { echo "MUTANT $$m KILLED BY THE WRONG CHECK (want $$c)"; cat $@.$$m.log; exit 1; }; \
	done
	@echo "test_remote_h264_transcode: 22 mutants FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_remote_h264_transcode.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_remote_h264_transcode_asan.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_remote_h264_transcode.mutant-check

OBENV_DEPS := $(ROOT)/tests/unit/test_ob_decode_envelope.cpp $(ROOT)/host/libmisterplex/ob_decode_envelope.hpp \
	$(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/arm/misterplexd/main.cpp $(ROOT)/assets/misterplex.conf.example
$(ROOT)/build/test_ob_decode_envelope: $(OBENV_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -DMPX_SOURCE_ROOT='"$(ROOT)"' -o $@ $(ROOT)/tests/unit/test_ob_decode_envelope.cpp
$(ROOT)/build/test_ob_decode_envelope.ok: $(ROOT)/build/test_ob_decode_envelope
	$(ROOT)/build/test_ob_decode_envelope
	touch $@
$(ROOT)/build/test_ob_decode_envelope_asan: $(OBENV_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		-DMPX_SOURCE_ROOT='"$(ROOT)"' -o $@ $(ROOT)/tests/unit/test_ob_decode_envelope.cpp
$(ROOT)/build/test_ob_decode_envelope_asan.ok: $(ROOT)/build/test_ob_decode_envelope_asan
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 $(ROOT)/build/test_ob_decode_envelope_asan
	touch $@
$(ROOT)/build/test_ob_decode_envelope.mutant-check: $(ROOT)/build/test_ob_decode_envelope
	@$(ROOT)/build/test_ob_decode_envelope > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in MUT_ENV_SIZE_IGNORED MUT_ENV_CODEC_IGNORED MUT_ENV_OFF_BLOCKS MUT_ENV_ON_IGNORED MUT_FIRSTPIC_STC_ONLY MUT_REDACT_OFF MUT_REDACT_NO_PCT MUT_ENV_LOCAL_GATED; do \
		$(CXX) $(CXXFLAGS) -DMPX_SOURCE_ROOT='"$(ROOT)"' -D$$m -o $(ROOT)/build/test_ob_decode_envelope.$$m \
			$(ROOT)/tests/unit/test_ob_decode_envelope.cpp; \
		if $(ROOT)/build/test_ob_decode_envelope.$$m > $@.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
		case $$m in MUT_ENV_SIZE_IGNORED) c=E1 ;; MUT_ENV_CODEC_IGNORED) c=E2 ;; MUT_ENV_OFF_BLOCKS) c=E3 ;; \
			MUT_ENV_ON_IGNORED) c=E4 ;; MUT_FIRSTPIC_STC_ONLY) c=E5 ;; MUT_REDACT_OFF) c=E6 ;; MUT_REDACT_NO_PCT) c=E8 ;; MUT_ENV_LOCAL_GATED) c=E9 ;; esac; \
		grep -q "FAIL: $$c " $@.$$m.log || { echo "MUTANT $$m KILLED BY THE WRONG CHECK (want $$c)"; cat $@.$$m.log; exit 1; }; \
	done
	@echo "test_ob_decode_envelope: 8 mutants FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_ob_decode_envelope.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_ob_decode_envelope_asan.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_ob_decode_envelope.mutant-check
# BURN_SUBS_FIX default off. On: a picked subtitle the overlay will not paint
# (PGS/image; mov_text when SOFT_SUBS off) is subtitles=burn on the start and
# decision URLs, and directPlay=0. Soft SRT/ASS stay on the overlay.
# Four mutants must FAIL. Unset stays off (no BURN_SUBS_FIX=1 in the example).
$(ROOT)/build/test_burn_subs_fix: $(ROOT)/tests/unit/test_burn_subs_fix.cpp \
		$(ROOT)/host/libmisterplex/burn_subs_fix.hpp $(ROOT)/host/libmisterplex/track_select.hpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/arm/misterplexd/plex_resolve.hpp \
		$(ROOT)/arm/misterplexd/main.cpp $(ROOT)/assets/misterplex.conf.example
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -o $@ \
		$(ROOT)/tests/unit/test_burn_subs_fix.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp
$(ROOT)/build/test_burn_subs_fix.ok: $(ROOT)/build/test_burn_subs_fix
	$(ROOT)/build/test_burn_subs_fix
	touch $@
$(ROOT)/build/test_burn_subs_fix_asan: $(ROOT)/tests/unit/test_burn_subs_fix.cpp \
		$(ROOT)/host/libmisterplex/burn_subs_fix.hpp $(ROOT)/arm/misterplexd/plex_resolve.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		-I$(ROOT)/arm/misterplexd -pthread -o $@ \
		$(ROOT)/tests/unit/test_burn_subs_fix.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp
$(ROOT)/build/test_burn_subs_fix_asan.ok: $(ROOT)/build/test_burn_subs_fix_asan
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 $(ROOT)/build/test_burn_subs_fix_asan
	touch $@
$(ROOT)/build/test_burn_subs_fix.mutant-check: $(ROOT)/build/test_burn_subs_fix
	@$(ROOT)/build/test_burn_subs_fix > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in MUT_BURN_SUBS_FIX_FLAG_DEFAULT_ON MUT_BURN_SUBS_FIX_KEEP_DIRECT \
		MUT_BURN_SUBS_FIX_DROP_TEXT MUT_BURN_SUBS_FIX_DECISION_OMIT; do \
		$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -D$$m \
			-o $(ROOT)/build/test_burn_subs_fix.$$m \
			$(ROOT)/tests/unit/test_burn_subs_fix.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp || exit 1; \
		if $(ROOT)/build/test_burn_subs_fix.$$m > $@.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m"; cat $@.$$m.log; exit 1; fi; \
	done
	@echo "test_burn_subs_fix: 4 mutants FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_burn_subs_fix.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_burn_subs_fix_asan.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_burn_subs_fix.mutant-check

# FU14: burn PUT is opt-in. Flag off is no PUT and the main start URL.
$(ROOT)/build/test_fu14_burn_optin: $(ROOT)/tests/unit/test_fu14_burn_optin.cpp \
		$(ROOT)/host/libmisterplex/burn_subs_fix.hpp $(ROOT)/host/libmisterplex/track_select.hpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -o $@ \
		$(ROOT)/tests/unit/test_fu14_burn_optin.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp
$(ROOT)/build/test_fu14_burn_optin.ok: $(ROOT)/build/test_fu14_burn_optin
	$(ROOT)/build/test_fu14_burn_optin
	touch $@
$(ROOT)/build/test_fu14_burn_optin_asan: $(ROOT)/tests/unit/test_fu14_burn_optin.cpp \
		$(ROOT)/host/libmisterplex/track_select.hpp $(ROOT)/arm/misterplexd/plex_resolve.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		-I$(ROOT)/arm/misterplexd -pthread -o $@ \
		$(ROOT)/tests/unit/test_fu14_burn_optin.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp
$(ROOT)/build/test_fu14_burn_optin_asan.ok: $(ROOT)/build/test_fu14_burn_optin_asan
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 $(ROOT)/build/test_fu14_burn_optin_asan
	touch $@
$(ROOT)/build/test_fu14_burn_optin.mutant-check: $(ROOT)/build/test_fu14_burn_optin
	@$(ROOT)/build/test_fu14_burn_optin > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in MUT_BURN_SUBS_SELECT_IGNORE_FLAG MUT_BURN_SUBS_SELECT_NO_PUT \
		MUT_BURN_SUBS_SELECT_NO_RESTORE MUT_BURN_SUBS_SELECT_KEEP_ON_FAIL \
		MUT_FU14_LEGACY_CONF_IGNORED MUT_LEGACY_IGNORED_WHEN_AUTO_UNSET \
		MUT_AUTO0_NOT_WINNING MUT_NEITHER_MEANS_ON MUT_AUTO1_IGNORED; do \
		$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -D$$m \
			-o $(ROOT)/build/test_fu14_burn_optin.$$m \
			$(ROOT)/tests/unit/test_fu14_burn_optin.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp || exit 1; \
		if $(ROOT)/build/test_fu14_burn_optin.$$m > $@.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m"; cat $@.$$m.log; exit 1; fi; \
	done
	@echo "test_fu14_burn_optin: 9 mutants FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_fu14_burn_optin.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_fu14_burn_optin_asan.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_fu14_burn_optin.mutant-check

# #324: burn subtitle part PUT only with BURN_SUBS_FIX on AND an owned server.
T324_DEPS := $(ROOT)/tests/unit/test_324_burn_put_owned.cpp $(ROOT)/host/libmisterplex/burn_subs_fix.hpp \
	$(ROOT)/host/libmisterplex/track_select.hpp $(ROOT)/arm/misterplexd/plex_resolve.cpp
$(ROOT)/build/test_324_burn_put_owned: $(T324_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -o $@ \
		$(ROOT)/tests/unit/test_324_burn_put_owned.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp
$(ROOT)/build/test_324_burn_put_owned.ok: $(ROOT)/build/test_324_burn_put_owned
	$(ROOT)/build/test_324_burn_put_owned
	touch $@
$(ROOT)/build/test_324_burn_put_owned_asan: $(T324_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		-I$(ROOT)/arm/misterplexd -pthread -o $@ \
		$(ROOT)/tests/unit/test_324_burn_put_owned.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp
$(ROOT)/build/test_324_burn_put_owned_asan.ok: $(ROOT)/build/test_324_burn_put_owned_asan
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 $(ROOT)/build/test_324_burn_put_owned_asan
	touch $@
$(ROOT)/build/test_324_burn_put_owned.mutant-check: $(ROOT)/build/test_324_burn_put_owned
	@for m in MUT_324_NO_OWNER_CHECK MUT_324_DEFAULT_ON MUT_329_HOST_KEY; do \
		$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -D$$m -o $(ROOT)/build/test_324_burn_put_owned.$$m \
			$(ROOT)/tests/unit/test_324_burn_put_owned.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp || exit 1; \
		if $(ROOT)/build/test_324_burn_put_owned.$$m > $@.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m"; cat $@.$$m.log; exit 1; fi; \
		grep -q '^FAIL: ' $@.$$m.log || { echo "mutant $$m: no FAIL line"; exit 1; }; \
		echo "mutant $$m killed: $$(grep -m1 '^FAIL: ' $@.$$m.log)"; \
	done
	@touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_324_burn_put_owned.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_324_burn_put_owned_asan.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_324_burn_put_owned.mutant-check

# #201-r3 final spec: SRT/ASS never burn (flag on/off, SOFT_SUBS on/off, live and
# remembered); BURN_SUBS_FIX forces the burn transcode for image subs only; the
# resolve paint answer equals the player's kind (no double paint). 4 mutants FAIL.
$(ROOT)/build/test_201r3_subs_spec: $(ROOT)/tests/unit/test_201r3_subs_spec.cpp \
		$(ROOT)/host/libmisterplex/burn_subs_fix.hpp $(ROOT)/host/libmisterplex/track_select.hpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/arm/misterplexd/plex_resolve.hpp\
		$(ROOT)/host/libmisterplex/remember_tracks.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -o $@ \
		$(ROOT)/tests/unit/test_201r3_subs_spec.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp
$(ROOT)/build/test_201r3_subs_spec.ok: $(ROOT)/build/test_201r3_subs_spec
	$(ROOT)/build/test_201r3_subs_spec
	touch $@
$(ROOT)/build/test_201r3_subs_spec_asan: $(ROOT)/tests/unit/test_201r3_subs_spec.cpp \
		$(ROOT)/host/libmisterplex/burn_subs_fix.hpp $(ROOT)/arm/misterplexd/plex_resolve.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		-I$(ROOT)/arm/misterplexd -pthread -o $@ \
		$(ROOT)/tests/unit/test_201r3_subs_spec.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp
$(ROOT)/build/test_201r3_subs_spec_asan.ok: $(ROOT)/build/test_201r3_subs_spec_asan
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 $(ROOT)/build/test_201r3_subs_spec_asan
	touch $@
$(ROOT)/build/test_201r3_subs_spec.mutant-check: $(ROOT)/build/test_201r3_subs_spec
	@$(ROOT)/build/test_201r3_subs_spec > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in MUT_R3_MIRROR_CODEC_ONLY MUT_R3_SRT_BURNS_FLAG_ON \
		MUT_R3_FORCE_ANY_BURN MUT_R3_REMEMBER_CODEC_ONLY MUT_R3_MOV_TEXT_AS_IMAGE; do \
		$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -D$$m \
			-o $(ROOT)/build/test_201r3_subs_spec.$$m \
			$(ROOT)/tests/unit/test_201r3_subs_spec.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp || exit 1; \
		if $(ROOT)/build/test_201r3_subs_spec.$$m > $@.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m"; cat $@.$$m.log; exit 1; fi; \
	done
	@echo "test_201r3_subs_spec: 5 mutants FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_201r3_subs_spec.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_201r3_subs_spec_asan.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_201r3_subs_spec.mutant-check

$(ROOT)/build/test_play_queue_browse: $(ROOT)/tests/unit/test_play_queue_browse.cpp $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -o $@ $(ROOT)/tests/unit/test_play_queue_browse.cpp $(PLEXUI_SRC)
$(ROOT)/build/test_play_queue_browse.ok: $(ROOT)/build/test_play_queue_browse
	$(ROOT)/build/test_play_queue_browse
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_play_queue_browse.ok

$(ROOT)/build/test_theme_show_fetch: $(ROOT)/tests/unit/test_theme_show_fetch.cpp $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex/plexui \
		-I$(ROOT)/arm/misterplexd -o $@ $(ROOT)/tests/unit/test_theme_show_fetch.cpp $(PLEXUI_SRC)
$(ROOT)/build/test_theme_show_fetch.ok: $(ROOT)/build/test_theme_show_fetch
	$(ROOT)/build/test_theme_show_fetch
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_theme_show_fetch.ok

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

# Chain + hostname on. Time check skipped only while the clock is implausible.
TLS_VERIFY_CERTS := $(ROOT)/build/tls-verify-certs
$(TLS_VERIFY_CERTS)/ca.pem:
	@mkdir -p $(TLS_VERIFY_CERTS)/newcerts
	openssl req -x509 -newkey rsa:2048 -keyout $(TLS_VERIFY_CERTS)/ca.key -out $(TLS_VERIFY_CERTS)/ca.pem -days 3650 -nodes -subj "/CN=Misterplex TLS Test CA" >/dev/null 2>&1
	openssl req -newkey rsa:2048 -keyout $(TLS_VERIFY_CERTS)/good.key -out $(TLS_VERIFY_CERTS)/good.csr -nodes -subj "/CN=127.0.0.1" >/dev/null 2>&1
	printf '%s\n' 'subjectAltName=DNS:127.0.0.1,IP:127.0.0.1' > $(TLS_VERIFY_CERTS)/good.ext
	openssl x509 -req -in $(TLS_VERIFY_CERTS)/good.csr -CA $(TLS_VERIFY_CERTS)/ca.pem -CAkey $(TLS_VERIFY_CERTS)/ca.key -CAcreateserial -out $(TLS_VERIFY_CERTS)/good.pem -days 3650 -extfile $(TLS_VERIFY_CERTS)/good.ext >/dev/null 2>&1
	openssl req -newkey rsa:2048 -keyout $(TLS_VERIFY_CERTS)/wrong.key -out $(TLS_VERIFY_CERTS)/wrong.csr -nodes -subj "/CN=wrong.test" >/dev/null 2>&1
	printf '%s\n' 'subjectAltName=DNS:wrong.test' > $(TLS_VERIFY_CERTS)/wrong.ext
	openssl x509 -req -in $(TLS_VERIFY_CERTS)/wrong.csr -CA $(TLS_VERIFY_CERTS)/ca.pem -CAkey $(TLS_VERIFY_CERTS)/ca.key -CAcreateserial -out $(TLS_VERIFY_CERTS)/wrong.pem -days 3650 -extfile $(TLS_VERIFY_CERTS)/wrong.ext >/dev/null 2>&1
	openssl req -x509 -newkey rsa:2048 -keyout $(TLS_VERIFY_CERTS)/bad.key -out $(TLS_VERIFY_CERTS)/bad.pem -days 3650 -nodes -subj "/CN=127.0.0.1" -addext "subjectAltName=DNS:127.0.0.1,IP:127.0.0.1" >/dev/null 2>&1
	openssl req -newkey rsa:2048 -keyout $(TLS_VERIFY_CERTS)/direct.key -out $(TLS_VERIFY_CERTS)/direct.csr -nodes -subj "/CN=plex.direct" >/dev/null 2>&1
	printf '%s\n' 'subjectAltName=DNS:*.abcd.plex.direct' > $(TLS_VERIFY_CERTS)/direct.ext
	openssl x509 -req -in $(TLS_VERIFY_CERTS)/direct.csr -CA $(TLS_VERIFY_CERTS)/ca.pem -CAkey $(TLS_VERIFY_CERTS)/ca.key -CAcreateserial -out $(TLS_VERIFY_CERTS)/direct.pem -days 3650 -extfile $(TLS_VERIFY_CERTS)/direct.ext >/dev/null 2>&1
	openssl req -x509 -newkey rsa:2048 -keyout $(TLS_VERIFY_CERTS)/self.key -out $(TLS_VERIFY_CERTS)/self.pem -days 3650 -nodes -subj "/CN=127.0.0.1" -addext "subjectAltName=IP:127.0.0.1" >/dev/null 2>&1
	openssl req -newkey rsa:2048 -keyout $(TLS_VERIFY_CERTS)/expired.key -out $(TLS_VERIFY_CERTS)/expired.csr -nodes -subj "/CN=127.0.0.1" >/dev/null 2>&1
	printf '%s\n' '[ca]' 'default_ca = CA_default' '[CA_default]' 'dir = $(TLS_VERIFY_CERTS)' 'database = $$dir/index.txt' 'new_certs_dir = $$dir/newcerts' 'certificate = $$dir/ca.pem' 'serial = $$dir/serial' 'private_key = $$dir/ca.key' 'default_md = sha256' 'policy = policy_any' 'copy_extensions = copy' 'x509_extensions = v3' '[policy_any]' 'commonName = supplied' '[v3]' 'subjectAltName = DNS:127.0.0.1,IP:127.0.0.1' > $(TLS_VERIFY_CERTS)/ca.cnf
	touch $(TLS_VERIFY_CERTS)/index.txt
	echo 01 > $(TLS_VERIFY_CERTS)/serial
	openssl ca -config $(TLS_VERIFY_CERTS)/ca.cnf -in $(TLS_VERIFY_CERTS)/expired.csr -out $(TLS_VERIFY_CERTS)/expired.pem -startdate 20200101000000Z -enddate 20200102000000Z -batch -notext >/dev/null
TLS_VERIFY_SRC := $(ROOT)/tests/unit/test_tls_verify.cpp \
		$(ROOT)/arm/misterplexd/tls_http.cpp $(ROOT)/arm/misterplexd/tls_http.hpp \
		$(ROOT)/arm/misterplexd/wall_clock.cpp $(ROOT)/arm/misterplexd/ca_bundle.hpp \
		$(ROOT)/host/libmisterplex/tls_policy.hpp
$(ROOT)/build/test_tls_verify: $(TLS_VERIFY_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -DMPX_HAVE_GCAST_TLS -o $@ \
		$(ROOT)/tests/unit/test_tls_verify.cpp \
		$(ROOT)/arm/misterplexd/tls_http.cpp \
		$(ROOT)/arm/misterplexd/wall_clock.cpp \
		-lssl -lcrypto -ldl
$(ROOT)/build/test_tls_verify_asan: $(TLS_VERIFY_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(PLEXUI_ASAN_CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -DMPX_HAVE_GCAST_TLS -o $@ \
		$(ROOT)/tests/unit/test_tls_verify.cpp \
		$(ROOT)/arm/misterplexd/tls_http.cpp \
		$(ROOT)/arm/misterplexd/wall_clock.cpp \
		-lssl -lcrypto -ldl
$(ROOT)/build/test_tls_verify.ok: $(ROOT)/build/test_tls_verify $(ROOT)/build/wall_clock_faketime.so $(TLS_VERIFY_CERTS)/ca.pem
	LD_PRELOAD=$(ROOT)/build/wall_clock_faketime.so MPX_TLS_CERT_DIR=$(TLS_VERIFY_CERTS) MPX_SRC_ROOT=$(ROOT) $(ROOT)/build/test_tls_verify
	touch $@
TLS_ASAN_PRELOAD := $(shell $(CXX) -print-file-name=libasan.so):$(shell $(CXX) -print-file-name=libubsan.so):$(ROOT)/build/wall_clock_faketime.so
$(ROOT)/build/test_tls_verify_asan.ok: $(ROOT)/build/test_tls_verify_asan $(ROOT)/build/wall_clock_faketime.so $(TLS_VERIFY_CERTS)/ca.pem
	ASAN_OPTIONS=detect_leaks=1:quarantine_size_mb=0 LD_PRELOAD=$(TLS_ASAN_PRELOAD) MPX_TLS_CERT_DIR=$(TLS_VERIFY_CERTS) MPX_SRC_ROOT=$(ROOT) $(ROOT)/build/test_tls_verify_asan
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_tls_verify.ok $(ROOT)/build/test_tls_verify_asan.ok
# #340: the TLS GET must move a URL query token into the header; the mutant keeps it in the query.
$(ROOT)/build/test_tls_verify_340_mutant.ok: $(ROOT)/build/test_tls_verify $(ROOT)/build/wall_clock_faketime.so $(TLS_VERIFY_CERTS)/ca.pem
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -DMPX_HAVE_GCAST_TLS -DMUT_340_TLS_QUERY_TOKEN -o $(ROOT)/build/test_tls_verify.mut340 \
		$(ROOT)/tests/unit/test_tls_verify.cpp $(ROOT)/arm/misterplexd/tls_http.cpp $(ROOT)/arm/misterplexd/wall_clock.cpp -lssl -lcrypto -ldl
	@if LD_PRELOAD=$(ROOT)/build/wall_clock_faketime.so MPX_TLS_CERT_DIR=$(TLS_VERIFY_CERTS) MPX_SRC_ROOT=$(ROOT) $(ROOT)/build/test_tls_verify.mut340 >/dev/null 2>&1; then \
		echo "test_tls_verify: MUT_340_TLS_QUERY_TOKEN SURVIVED"; exit 1; fi
	@echo "test_tls_verify: MUT_340_TLS_QUERY_TOKEN FAIL as required"
	@touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_tls_verify_340_mutant.ok

# Standalone yt-dlp: route, single-flight, timeout/kill, checksummed swap on 127.0.0.1.
$(ROOT)/build/test_yt_dlp: $(ROOT)/tests/unit/test_yt_dlp.cpp $(ROOT)/arm/misterplexd/gcast/yt_dlp.hpp \
		$(ROOT)/arm/misterplexd/gcast/yt_cache.hpp $(ROOT)/arm/misterplexd/gcast/yt_captions.hpp \
		$(ROOT)/arm/misterplexd/gcast/dash_seek.hpp \
		$(ROOT)/arm/misterplexd/gcast/cast_resolve.hpp $(ROOT)/arm/misterplexd/mem_stat.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -o $@ $(ROOT)/tests/unit/test_yt_dlp.cpp
$(ROOT)/build/test_yt_dlp.ok: $(ROOT)/build/test_yt_dlp
	$(ROOT)/build/test_yt_dlp
	touch $@
# Dropping the ladder (MUT_YT_NO_CLIENT_RETRY) leaves the bot-check client failed.
# Walking after a URL (MUT_YT_CLIENT_RETRY_WALK) runs clients the first success did not need.
$(ROOT)/build/test_yt_dlp.mutant-check: $(ROOT)/build/test_yt_dlp $(ROOT)/tests/unit/test_yt_dlp.cpp \
		$(ROOT)/arm/misterplexd/gcast/yt_dlp.hpp
	@$(ROOT)/build/test_yt_dlp > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -DMUT_YT_NO_CLIENT_RETRY \
		-o $@.noretry $(ROOT)/tests/unit/test_yt_dlp.cpp
	@if $@.noretry > $@.noretry.log 2>&1; then echo "MUTANT SURVIVED: MUT_YT_NO_CLIENT_RETRY"; exit 1; fi
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -DMUT_YT_CLIENT_RETRY_WALK \
		-o $@.walk $(ROOT)/tests/unit/test_yt_dlp.cpp
	@if $@.walk > $@.walk.log 2>&1; then echo "MUTANT SURVIVED: MUT_YT_CLIENT_RETRY_WALK"; exit 1; fi
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -DMUT_GH8_NO_COOKIES \
		-o $@.nocookie $(ROOT)/tests/unit/test_yt_dlp.cpp
	@if $@.nocookie > $@.nocookie.log 2>&1; then echo "MUTANT SURVIVED: MUT_GH8_NO_COOKIES"; exit 1; fi
	@echo "test_yt_dlp: MUT_YT_NO_CLIENT_RETRY MUT_YT_CLIENT_RETRY_WALK MUT_GH8_NO_COOKIES FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_yt_dlp.ok $(ROOT)/build/test_yt_dlp.mutant-check
# Resolve timeout 90 s total default, YT_RESOLVE_TIMEOUT override, clamp.
$(ROOT)/build/test_yt_resolve_timeout: $(ROOT)/tests/unit/test_yt_resolve_timeout.cpp $(ROOT)/arm/misterplexd/gcast/yt_dlp.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -o $@ $<
$(ROOT)/build/test_yt_resolve_timeout.ok: $(ROOT)/build/test_yt_resolve_timeout
	$(ROOT)/build/test_yt_resolve_timeout
	touch $@
$(ROOT)/build/test_yt_resolve_timeout.mutant-check: $(ROOT)/build/test_yt_resolve_timeout
	@set -e; for m in MUT_335_OLD_TIMEOUT MUT_335_NO_CLAMP MUT_335_IGNORE_CONF; do \
		$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -D$$m -o $@.$$m $(ROOT)/tests/unit/test_yt_resolve_timeout.cpp; \
		if $@.$$m > $@.$$m.log 2>&1; then echo "MUTANT SURVIVED: $$m"; exit 1; fi; done
	@echo "test_yt_resolve_timeout: MUT_335_OLD_TIMEOUT MUT_335_NO_CLAMP MUT_335_IGNORE_CONF FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_yt_resolve_timeout.ok $(ROOT)/build/test_yt_resolve_timeout.mutant-check
# #339: resolve child at nice -5 (clamped), --use-extractors youtube.
$(ROOT)/build/test_yt_resolve_speed: $(ROOT)/tests/unit/test_yt_resolve_speed.cpp $(ROOT)/arm/misterplexd/gcast/yt_dlp.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -o $@ $<
$(ROOT)/build/test_yt_resolve_speed.ok: $(ROOT)/build/test_yt_resolve_speed
	$(ROOT)/build/test_yt_resolve_speed
	touch $@
$(ROOT)/build/test_yt_resolve_speed.mutant-check: $(ROOT)/build/test_yt_resolve_speed
	@set -e; for m in MUT_339_NO_PRIO MUT_339_ALL_EXTRACTORS MUT_339_NO_NICE_CLAMP; do \
		$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -D$$m -o $@.$$m $(ROOT)/tests/unit/test_yt_resolve_speed.cpp; \
		if $@.$$m > $@.$$m.log 2>&1; then echo "MUTANT SURVIVED: $$m"; exit 1; fi; done
	@echo "test_yt_resolve_speed: MUT_339_NO_PRIO MUT_339_ALL_EXTRACTORS MUT_339_NO_NICE_CLAMP FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_yt_resolve_speed.ok $(ROOT)/build/test_yt_resolve_speed.mutant-check

# YT_LIVE: livestream HLS is remuxed to loopback MPEG-TS. Unset keeps the VOD selector.
YTLIVE_SRC := $(ROOT)/tests/unit/test_yt_live.cpp $(ROOT)/arm/misterplexd/gcast/yt_live.hpp \
	$(ROOT)/arm/misterplexd/gcast/yt_dlp.hpp $(ROOT)/arm/misterplexd/gcast/yt_captions.hpp \
	$(ROOT)/arm/misterplexd/main.cpp
$(ROOT)/build/test_yt_live: $(YTLIVE_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -DMPX_SOURCE_ROOT='"$(ROOT)"' -o $@ \
		$(ROOT)/tests/unit/test_yt_live.cpp
$(ROOT)/build/test_yt_live.ok: $(ROOT)/build/test_yt_live
	$(ROOT)/build/test_yt_live
	touch $@
$(ROOT)/build/test_yt_live_asan: $(YTLIVE_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -pthread \
		-I$(ROOT)/host -I$(ROOT)/arm/misterplexd -DMPX_SOURCE_ROOT='"$(ROOT)"' -o $@ $(ROOT)/tests/unit/test_yt_live.cpp
$(ROOT)/build/test_yt_live_asan.ok: $(ROOT)/build/test_yt_live_asan
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 $(ROOT)/build/test_yt_live_asan
	touch $@
$(ROOT)/build/test_yt_live.mutant-check: $(ROOT)/build/test_yt_live $(YTLIVE_SRC)
	@$(ROOT)/build/test_yt_live > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -DMPX_SOURCE_ROOT='"$(ROOT)"' -DMUT_YT_LIVE_IGNORED \
		-o $@.ignored $(ROOT)/tests/unit/test_yt_live.cpp
	@if $@.ignored > $@.ignored.log 2>&1; then echo "MUTANT SURVIVED: MUT_YT_LIVE_IGNORED"; exit 1; fi
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -DMPX_SOURCE_ROOT='"$(ROOT)"' -DMUT_YT_LIVE_KEEP_OFFSET \
		-o $@.offset $(ROOT)/tests/unit/test_yt_live.cpp
	@if $@.offset > $@.offset.log 2>&1; then echo "MUTANT SURVIVED: MUT_YT_LIVE_KEEP_OFFSET"; exit 1; fi
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -DMPX_SOURCE_ROOT='"$(ROOT)"' -DMUT_YT_LIVE_DROP_FORMAT \
		-o $@.format $(ROOT)/tests/unit/test_yt_live.cpp
	@if $@.format > $@.format.log 2>&1; then echo "MUTANT SURVIVED: MUT_YT_LIVE_DROP_FORMAT"; exit 1; fi
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -DMPX_SOURCE_ROOT='"$(ROOT)"' -DMUT_YT_LIVE_PLAY_DIRECT \
		-o $@.direct $(ROOT)/tests/unit/test_yt_live.cpp
	@if $@.direct > $@.direct.log 2>&1; then echo "MUTANT SURVIVED: MUT_YT_LIVE_PLAY_DIRECT"; exit 1; fi
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -DMPX_SOURCE_ROOT='"$(ROOT)"' -DMUT_YT_LIVE_ZERO_LOUNGE_DUR \
		-o $@.lounge $(ROOT)/tests/unit/test_yt_live.cpp
	@if $@.lounge > $@.lounge.log 2>&1; then echo "MUTANT SURVIVED: MUT_YT_LIVE_ZERO_LOUNGE_DUR"; exit 1; fi
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -DMPX_SOURCE_ROOT='"$(ROOT)"' -DMUT_YT_LIVE_REQUIRE_MP4A \
		-o $@.mp4a $(ROOT)/tests/unit/test_yt_live.cpp
	@if $@.mp4a > $@.mp4a.log 2>&1; then echo "MUTANT SURVIVED: MUT_YT_LIVE_REQUIRE_MP4A"; exit 1; fi
	@echo "test_yt_live: six YT_LIVE mutants FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_yt_live.ok $(ROOT)/build/test_yt_live_asan.ok \
	$(ROOT)/build/test_yt_live.mutant-check

# YT_CAST_TITLE: OSD / now-playing uses the extract title already on the yt-dlp child. Unset stays the tip path.
# #214 glass 10/31: caption yt-dlp child timeout + real VTT fixtures (fake yt-dlp sleeps 22 s).
YTCAPFETCH_SRC := $(ROOT)/tests/unit/test_yt_captions_fetch.cpp $(ROOT)/arm/misterplexd/gcast/yt_dlp.hpp \
	$(ROOT)/arm/misterplexd/gcast/yt_cache.hpp \
	$(ROOT)/arm/misterplexd/gcast/yt_captions.hpp $(wildcard $(ROOT)/tests/fixtures/yt_captions/*)
$(ROOT)/build/test_yt_captions_fetch: $(YTCAPFETCH_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -DMPX_SOURCE_ROOT='"$(ROOT)"' -o $@ $(ROOT)/tests/unit/test_yt_captions_fetch.cpp
$(ROOT)/build/test_yt_captions_fetch.ok: $(ROOT)/build/test_yt_captions_fetch
	$(ROOT)/build/test_yt_captions_fetch
	touch $@
$(ROOT)/build/test_yt_captions_fetch_asan: $(YTCAPFETCH_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
		-I$(ROOT)/host -I$(ROOT)/arm/misterplexd -DMPX_SOURCE_ROOT='"$(ROOT)"' -o $@ $(ROOT)/tests/unit/test_yt_captions_fetch.cpp
$(ROOT)/build/test_yt_captions_fetch_asan.ok: $(ROOT)/build/test_yt_captions_fetch_asan
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 $(ROOT)/build/test_yt_captions_fetch_asan
	touch $@
$(ROOT)/build/test_yt_captions_fetch.mutant-check: $(ROOT)/build/test_yt_captions_fetch $(YTCAPFETCH_SRC)
	@$(ROOT)/build/test_yt_captions_fetch > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -DMPX_SOURCE_ROOT='"$(ROOT)"' \
		-DMUT_LP_CAPTIONS_SHORT_TIMEOUT -o $@.short $(ROOT)/tests/unit/test_yt_captions_fetch.cpp
	@if $@.short > $@.short.log 2>&1; then echo "MUTANT SURVIVED: MUT_LP_CAPTIONS_SHORT_TIMEOUT"; exit 1; fi
	@for m in MUT_LP_CAPTIONS_IGNORE_PLAY_WAITER MUT_LP_CAPTIONS_NO_BUDGET MUT_LP_CAPTIONS_CAP_HIDE_ALL; do \
		$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -DMPX_SOURCE_ROOT='"$(ROOT)"' \
			-D$$m -o $@.$$m $(ROOT)/tests/unit/test_yt_captions_fetch.cpp || exit 1; \
		if $@.$$m > $@.$$m.log 2>&1; then echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
		echo "test_yt_captions_fetch: $$m FAIL as required"; done
	@echo "test_yt_captions_fetch: MUT_LP_CAPTIONS_SHORT_TIMEOUT FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_yt_captions_fetch.ok $(ROOT)/build/test_yt_captions_fetch_asan.ok \
	$(ROOT)/build/test_yt_captions_fetch.mutant-check

YTCASTTITLE_SRC := $(ROOT)/tests/unit/test_yt_cast_title.cpp $(ROOT)/arm/misterplexd/gcast/yt_dlp.hpp \
	$(ROOT)/host/libmisterplex/gcast_osd.hpp $(ROOT)/arm/misterplexd/main.cpp \
	$(ROOT)/assets/misterplex.conf.example
$(ROOT)/build/test_yt_cast_title: $(YTCASTTITLE_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -o $@ $(ROOT)/tests/unit/test_yt_cast_title.cpp
$(ROOT)/build/test_yt_cast_title.ok: $(ROOT)/build/test_yt_cast_title
	$(ROOT)/build/test_yt_cast_title
	touch $@
$(ROOT)/build/test_yt_cast_title_asan: $(YTCASTTITLE_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -pthread \
		-I$(ROOT)/host -I$(ROOT)/arm/misterplexd -o $@ $(ROOT)/tests/unit/test_yt_cast_title.cpp
$(ROOT)/build/test_yt_cast_title_asan.ok: $(ROOT)/build/test_yt_cast_title_asan
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 $(ROOT)/build/test_yt_cast_title_asan
	touch $@
$(ROOT)/build/test_yt_cast_title.mutant-check: $(ROOT)/build/test_yt_cast_title $(YTCASTTITLE_SRC)
	@$(ROOT)/build/test_yt_cast_title > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -DMUT_YT_CAST_TITLE_IGNORED \
		-o $@.ignored $(ROOT)/tests/unit/test_yt_cast_title.cpp
	@if $@.ignored > $@.ignored.log 2>&1; then echo "MUTANT SURVIVED: MUT_YT_CAST_TITLE_IGNORED"; exit 1; fi
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -DMUT_YT_CAST_TITLE_FORCED \
		-o $@.forced $(ROOT)/tests/unit/test_yt_cast_title.cpp
	@if $@.forced > $@.forced.log 2>&1; then echo "MUTANT SURVIVED: MUT_YT_CAST_TITLE_FORCED"; exit 1; fi
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -DMUT_YT_CAST_TITLE_DROP_PRINT \
		-o $@.dropprint $(ROOT)/tests/unit/test_yt_cast_title.cpp
	@if $@.dropprint > $@.dropprint.log 2>&1; then echo "MUTANT SURVIVED: MUT_YT_CAST_TITLE_DROP_PRINT"; exit 1; fi
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -DMUT_YT_CAST_TITLE_DROP_PARSE \
		-o $@.dropparse $(ROOT)/tests/unit/test_yt_cast_title.cpp
	@if $@.dropparse > $@.dropparse.log 2>&1; then echo "MUTANT SURVIVED: MUT_YT_CAST_TITLE_DROP_PARSE"; exit 1; fi
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -DMUT_YT_CAST_TITLE_ALWAYS \
		-o $@.always $(ROOT)/tests/unit/test_yt_cast_title.cpp
	@if $@.always > $@.always.log 2>&1; then echo "MUTANT SURVIVED: MUT_YT_CAST_TITLE_ALWAYS"; exit 1; fi
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -DMUT_YT_CAST_TITLE_ANY_OWNER \
		-o $@.anyowner $(ROOT)/tests/unit/test_yt_cast_title.cpp
	@if $@.anyowner > $@.anyowner.log 2>&1; then echo "MUTANT SURVIVED: MUT_YT_CAST_TITLE_ANY_OWNER"; exit 1; fi
	@echo "test_yt_cast_title: six YT_CAST_TITLE mutants FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_yt_cast_title.ok $(ROOT)/build/test_yt_cast_title_asan.ok \
	$(ROOT)/build/test_yt_cast_title.mutant-check

# YouTube seek: reuse the resolved yt-av URLs instead of a 19-22 s yt-dlp run per seek (plus ASan/UBSan).
YTRC_DEPS := $(ROOT)/tests/unit/test_yt_resolve_cache.cpp $(ROOT)/arm/misterplexd/gcast/yt_dlp.hpp \
	$(ROOT)/arm/misterplexd/gcast/dash_seek.hpp $(ROOT)/arm/misterplexd/media_source_policy.hpp \
	$(ROOT)/arm/misterplexd/main.cpp
$(ROOT)/build/test_yt_resolve_cache: $(YTRC_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -DMAIN_CPP='"$(ROOT)/arm/misterplexd/main.cpp"' -o $@ $(ROOT)/tests/unit/test_yt_resolve_cache.cpp
$(ROOT)/build/test_yt_resolve_cache.ok: $(ROOT)/build/test_yt_resolve_cache
	$(ROOT)/build/test_yt_resolve_cache
	touch $@
$(ROOT)/build/test_yt_resolve_cache_asan: $(YTRC_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -pthread \
		-I$(ROOT)/host -I$(ROOT)/arm/misterplexd -DMAIN_CPP='"$(ROOT)/arm/misterplexd/main.cpp"' -o $@ $(ROOT)/tests/unit/test_yt_resolve_cache.cpp
$(ROOT)/build/test_yt_resolve_cache_asan.ok: $(ROOT)/build/test_yt_resolve_cache_asan
	ASAN_OPTIONS=detect_leaks=1 $(ROOT)/build/test_yt_resolve_cache_asan
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_yt_resolve_cache.ok $(ROOT)/build/test_yt_resolve_cache_asan.ok

YTWARM_DEPS := $(ROOT)/tests/unit/test_yt_warm.cpp \
	$(ROOT)/arm/misterplexd/airplay/yt_warm.hpp $(ROOT)/arm/misterplexd/airplay/yt_dlp.hpp \
	$(ROOT)/arm/misterplexd/airplay/yt_cache.hpp $(ROOT)/arm/misterplexd/airplay/mini_json.hpp \
	$(ROOT)/assets/linux/misterplex/yt_warm_helper.py $(ROOT)/tests/fixtures/yt_warm/fake_worker.py
$(ROOT)/build/test_yt_warm: $(YTWARM_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -DMPX_SOURCE_ROOT='"$(ROOT)"' \
		-o $@ $(ROOT)/tests/unit/test_yt_warm.cpp
$(ROOT)/build/test_yt_warm.ok: $(ROOT)/build/test_yt_warm $(ROOT)/tests/unit/test_yt_warm_helper.py
	$(ROOT)/build/test_yt_warm
	PYTHONDONTWRITEBYTECODE=1 python3 $(ROOT)/tests/unit/test_yt_warm_helper.py
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_yt_warm.ok

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
# airplay_xcode.py access logs: X-Plex-Token query/%3D/%253D never keep the live value.
$(ROOT)/build/test_airplay_xcode_log_redact.ok: $(ROOT)/tests/unit/test_airplay_xcode_log_redact.py \
		$(ROOT)/tools/airplay/airplay_xcode.py
	python3 $(ROOT)/tests/unit/test_airplay_xcode_log_redact.py
	@if AIRPLAY_MUT=noredact python3 $(ROOT)/tests/unit/test_airplay_xcode_log_redact.py >/dev/null 2>&1; then \
		echo "MUTANT SURVIVED: AIRPLAY_MUT=noredact"; exit 1; fi
	@echo "test_airplay_xcode_log_redact: AIRPLAY_MUT=noredact FAIL as required"
	@mkdir -p $(ROOT)/build
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_airplay_xcode_log_redact.ok

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

$(ROOT)/build/test_yt_screen_id: $(ROOT)/tests/unit/test_yt_screen_id.cpp \
		$(ROOT)/arm/misterplexd/gcast/yt_screen_id.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -o $@ $(ROOT)/tests/unit/test_yt_screen_id.cpp
$(ROOT)/build/test_yt_screen_id.ok: $(ROOT)/build/test_yt_screen_id
	$(ROOT)/build/test_yt_screen_id
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_yt_screen_id.ok

.PHONY: test-yt-lounge
test-yt-lounge: $(ROOT)/build/test_yt_lounge_token.ok
$(ROOT)/build/test_yt_lounge_token.ok: $(ROOT)/tests/unit/test_yt_lounge_token.cpp \
		$(ROOT)/arm/misterplexd/gcast/yt_lounge_token.hpp $(ROOT)/arm/misterplexd/gcast/yt_screen_id.hpp \
		$(ROOT)/arm/misterplexd/gcast/https_client.hpp $(ROOT)/arm/misterplexd/gcast/lounge_proto.hpp \
		$(ROOT)/tests/unit/hpp_mutant_runner.sh
	@mkdir -p $(ROOT)/build
	SAN=1 ROOT=$(ROOT) bash $(ROOT)/tests/unit/hpp_mutant_runner.sh tests/unit/test_yt_lounge_token.cpp MUT_YT_TOKEN_KEEP_REJECTED_SCREEN
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_yt_lounge_token.ok

$(ROOT)/build/test_ffmpeg_header_relay: $(ROOT)/tests/unit/test_ffmpeg_header_relay.cpp \
		$(ROOT)/host/libmisterplex/ffmpeg_header_relay.hpp $(ROOT)/arm/misterplexd/media_player.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/host -I$(ROOT)/arm/misterplexd \
		-DMPX_SOURCE_ROOT='"$(ROOT)"' -o $@ $(ROOT)/tests/unit/test_ffmpeg_header_relay.cpp
$(ROOT)/build/test_ffmpeg_header_relay.ok: $(ROOT)/build/test_ffmpeg_header_relay
	$(ROOT)/build/test_ffmpeg_header_relay
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_ffmpeg_header_relay.ok

$(ROOT)/build/test_idle_screensaver: $(ROOT)/tests/unit/test_idle_screensaver.cpp \
		$(ROOT)/host/libmisterplex/idle_screen.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/host -o $@ $(ROOT)/tests/unit/test_idle_screensaver.cpp
$(ROOT)/build/test_idle_screensaver.ok: $(ROOT)/build/test_idle_screensaver
	$(ROOT)/build/test_idle_screensaver
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_idle_screensaver.ok

$(ROOT)/build/test_browse_token_332: $(ROOT)/tests/unit/test_browse_token_332.cpp \
		$(ROOT)/host/libmisterplex/browse_fetch.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/host -I$(ROOT)/arm/misterplexd -o $@ $(ROOT)/tests/unit/test_browse_token_332.cpp
$(ROOT)/build/test_browse_token_332.ok: $(ROOT)/build/test_browse_token_332
	$(ROOT)/build/test_browse_token_332
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_browse_token_332.ok

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
$(ROOT)/build/test_mem_stat.mutant-check: $(ROOT)/build/test_mem_stat
	@$(ROOT)/build/test_mem_stat > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -DMUT_SOAK_RSS_UNLIMITED \
		-o $(ROOT)/build/test_mem_stat.mut $(ROOT)/tests/unit/test_mem_stat.cpp
	@if $(ROOT)/build/test_mem_stat.mut > $@.mut.log 2>&1; then \
		echo "test_mem_stat: MUT_SOAK_RSS_UNLIMITED SURVIVED"; exit 1; fi
	@echo "test_mem_stat: MUT_SOAK_RSS_UNLIMITED FAIL as required" | tee $@
	@rm -f $(ROOT)/build/test_mem_stat.mut
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_mem_stat.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_mem_stat.mutant-check

$(ROOT)/build/test_ubsan_fnbind: $(ROOT)/tests/unit/test_ubsan_fnbind.cpp \
		$(ROOT)/host/libmisterplex/d52_log.hpp $(ROOT)/host/libmisterplex/f1_beat.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -fsanitize=undefined -fno-sanitize-recover=undefined -I$(ROOT)/host \
		-DMPX_SOURCE_ROOT='"$(ROOT)"' -o $@ $(ROOT)/tests/unit/test_ubsan_fnbind.cpp
$(ROOT)/build/test_ubsan_fnbind.ok: $(ROOT)/build/test_ubsan_fnbind
	$(ROOT)/build/test_ubsan_fnbind
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_ubsan_fnbind.ok

unit-unlocked unit-host-unlocked: $(ROOT)/build/test_boot_supervise.ok
$(ROOT)/build/test_boot_supervise.ok: $(ROOT)/tests/unit/test_boot_supervise.sh $(ROOT)/scripts/misterplexd-boot-rc3.sh $(ROOT)/scripts/misterplexd_supervise.sh $(ROOT)/scripts/misterplex_cast_ready.sh $(ROOT)/scripts/misterplexd_update_watch.sh
	sh $(ROOT)/tests/unit/test_boot_supervise.sh
	@mkdir -p $(ROOT)/build
	touch $@

# GH#59: supervisor T+60 restores misterplexd.prev when the health marker never appears.
$(ROOT)/build/test_update_watch.ok: $(ROOT)/tests/unit/test_update_watch.sh $(ROOT)/scripts/misterplexd_update_watch.sh $(ROOT)/scripts/misterplexd_supervise.sh $(ROOT)/scripts/package_release.sh
	sh $(ROOT)/tests/unit/test_update_watch.sh
	@mkdir -p $(ROOT)/build
	touch $@
$(ROOT)/build/test_update_watch.mutant-check: $(ROOT)/build/test_update_watch.ok
	@if MUT_NO_T60_ROLLBACK=1 sh $(ROOT)/tests/unit/test_update_watch.sh > $@.mut.log 2>&1; then \
		echo "test_update_watch: MUT_NO_T60_ROLLBACK SURVIVED"; exit 1; fi
	@echo "test_update_watch: MUT_NO_T60_ROLLBACK FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_update_watch.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_update_watch.mutant-check

unit-unlocked unit-host-unlocked: $(ROOT)/build/test_boot_rc3.ok $(ROOT)/build/test_update_ytdlp.ok \
	$(ROOT)/build/test_package_plexob_rbf.ok $(ROOT)/build/test_package_readme_paths.ok \
	$(ROOT)/build/test_readme_conf_guard.ok
$(ROOT)/build/test_readme_conf_guard.ok: $(ROOT)/tests/unit/test_readme_conf_guard.sh $(ROOT)/packaging/rc3/README.md \
		$(ROOT)/docs/release.md $(ROOT)/scripts/package_release.sh
	sh $(ROOT)/tests/unit/test_readme_conf_guard.sh
	@mkdir -p $(ROOT)/build
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_lab_ip_scan.ok
$(ROOT)/build/test_lab_ip_scan.ok: $(ROOT)/tests/unit/test_lab_ip_scan.sh
	sh $(ROOT)/tests/unit/test_lab_ip_scan.sh
	@if MPX_LAB_SCAN_MUTANT=1 sh $(ROOT)/tests/unit/test_lab_ip_scan.sh >/dev/null 2>&1; then echo "MUTANT SURVIVED: MPX_LAB_SCAN_MUTANT"; exit 1; fi
	@mkdir -p $(ROOT)/build
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_release_tar_no_hardlinks.ok
$(ROOT)/build/test_release_tar_no_hardlinks.ok: $(ROOT)/tests/unit/test_release_tar_no_hardlinks.sh \
		$(ROOT)/scripts/verify_release_tarball.sh $(ROOT)/scripts/package_release.sh
	sh $(ROOT)/tests/unit/test_release_tar_no_hardlinks.sh
	@mkdir -p $(ROOT)/build
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_package_spelunkr_distinct.ok
$(ROOT)/build/test_package_spelunkr_distinct.ok: $(ROOT)/tests/unit/test_package_spelunkr_distinct.sh \
		$(ROOT)/scripts/package_release.sh
	bash $(ROOT)/tests/unit/test_package_spelunkr_distinct.sh
	@mkdir -p $(ROOT)/build
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_uninstall_misterplex.ok
$(ROOT)/build/test_uninstall_misterplex.ok: $(ROOT)/tests/unit/test_uninstall_misterplex.sh $(ROOT)/scripts/uninstall-misterplex.sh
	sh $(ROOT)/tests/unit/test_uninstall_misterplex.sh
	@mkdir -p $(ROOT)/build
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_no_utilities_path.ok
.PHONY: $(ROOT)/build/test_no_utilities_path.ok
$(ROOT)/build/test_no_utilities_path.ok: $(ROOT)/tests/unit/test_no_utilities_path.sh
	sh $(ROOT)/tests/unit/test_no_utilities_path.sh
	@mkdir -p $(ROOT)/build
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_install_release_card.ok
$(ROOT)/build/test_install_release_card.ok: $(ROOT)/tests/unit/test_install_release_card.sh $(ROOT)/scripts/install-misterplex.sh \
		$(ROOT)/scripts/uninstall-misterplex.sh $(ROOT)/scripts/plexob_pin_identity.sh \
		$(ROOT)/assets/linux/misterplex/yt_warm_helper.py
	sh $(ROOT)/tests/unit/test_install_release_card.sh
	@mkdir -p $(ROOT)/build
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_export_vendor_source.ok
$(ROOT)/build/test_export_vendor_source.ok: $(ROOT)/tests/unit/test_export_vendor_source.sh \
		$(ROOT)/tools/export/make-export.sh $(ROOT)/tools/export/EXPORT-ALLOWLIST.md
	sh $(ROOT)/tests/unit/test_export_vendor_source.sh
	@mkdir -p $(ROOT)/build
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_clean_install.ok
$(ROOT)/build/test_clean_install.ok: $(ROOT)/tests/unit/test_clean_install.sh $(ROOT)/tests/unit/clean_install_lib.sh \
		$(ROOT)/scripts/package_release.sh $(ROOT)/scripts/misterplexd-boot-rc3.sh
	bash $(ROOT)/tests/unit/test_clean_install.sh
	@mkdir -p $(ROOT)/build
	touch $@

unit-unlocked unit-host-unlocked: $(ROOT)/build/test_notice_source_offer.ok
$(ROOT)/build/test_boot_rc3.ok: $(ROOT)/tests/unit/test_boot_rc3.sh $(ROOT)/scripts/misterplexd-boot-rc3.sh
	bash $(ROOT)/tests/unit/test_boot_rc3.sh
	@mkdir -p $(ROOT)/build
	touch $@
# GH#27: boot Ready chevron after restart. No F015 soak.
$(ROOT)/build/test_boot_ready_chevron: $(ROOT)/tests/unit/test_boot_ready_chevron.cpp \
		$(ROOT)/host/libmisterplex/display_raster.hpp $(ROOT)/host/libmisterplex/gcast_osd.hpp \
		$(ROOT)/arm/misterplexd/media_player.cpp $(ROOT)/scripts/misterplexd-boot-rc3.sh
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/host -I$(ROOT)/arm/misterplexd -DMPX_SOURCE_ROOT='"$(ROOT)"' -o $@ \
		$(ROOT)/tests/unit/test_boot_ready_chevron.cpp
$(ROOT)/build/test_boot_ready_chevron.ok: $(ROOT)/build/test_boot_ready_chevron
	$(ROOT)/build/test_boot_ready_chevron
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_boot_ready_chevron.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_companion_guard.ok
$(ROOT)/build/test_companion_guard.ok: $(ROOT)/tests/unit/test_companion_guard.cpp $(ROOT)/tests/unit/hpp_mutant_runner.sh $(ROOT)/host/libmisterplex/companion_guard.hpp
	@mkdir -p $(ROOT)/build
	SAN=1 ROOT=$(ROOT) bash $(ROOT)/tests/unit/hpp_mutant_runner.sh tests/unit/test_companion_guard.cpp MUT_GUARD_ALL_PEERS MUT_GUARD_ORIGIN_ANY MUT_GUARD_LINKLOCAL_OK MUT_GUARD_NULL_OK MUT_GUARD_HOST_ANY MUT_GUARD_FETCH_SITE_OK MUT_GUARD_NO_ORIGIN_READS_OPEN MUT_GUARD_NO_CGNAT MUT_GUARD_MISTERPLEX_PREFIX_READ MUT_GUARD_CLIENT_HDR_EMPTY_OK MUT_GUARD_LAN_NEEDS_NO_HEADER MUT_GUARD_EXT_SCHEME_OK MUT_GUARD_NESTED_SCHEME_OK MUT_GUARD_NUMERIC_HOST_OK MUT_GUARD_V6_FORMS_OK MUT_GUARD_PLEXDIRECT_OFF MUT_GUARD_PLEXDIRECT_SUFFIX MUT_GUARD_PLEXDIRECT_ANYPEER
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_phone_status_page.ok
$(ROOT)/build/test_phone_status_page.ok: $(ROOT)/tests/unit/test_phone_status_page.cpp $(ROOT)/tests/unit/hpp_mutant_runner.sh $(ROOT)/host/libmisterplex/phone_status_page.hpp
	@mkdir -p $(ROOT)/build
	SAN=1 ROOT=$(ROOT) bash $(ROOT)/tests/unit/hpp_mutant_runner.sh tests/unit/test_phone_status_page.cpp MUT_PS_NO_REDACT MUT_PS_NO_ESCAPE MUT_PS_BUTTON_ALWAYS MUT_PS_ORIGIN_IGNORED MUT_PS_NONCE_PREFIX MUT_PS_ANY_VERB_UPDATE MUT_PS_NONCE_SKIP MUT_PS_PLAYING_NOCONFIRM
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_status_facts.ok
$(ROOT)/build/test_status_facts.ok: $(ROOT)/tests/unit/test_status_facts.cpp $(ROOT)/tests/unit/hpp_mutant_runner.sh $(ROOT)/host/libmisterplex/status_facts.hpp $(ROOT)/host/libmisterplex/phone_status_page.hpp
	@mkdir -p $(ROOT)/build
	SAN=1 ROOT=$(ROOT) bash $(ROOT)/tests/unit/hpp_mutant_runner.sh tests/unit/test_status_facts.cpp MUT_SF_HELPER_UNKNOWN MUT_SF_MD5_PLACEHOLDER MUT_SF_TITLE_PLACEHOLDER
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_queue_add.ok
$(ROOT)/build/test_queue_add.ok: $(ROOT)/tests/unit/test_queue_add.cpp $(ROOT)/tests/unit/hpp_mutant_runner.sh $(ROOT)/arm/misterplexd/queue_add.hpp $(ROOT)/arm/misterplexd/url_query_codec.hpp
	@mkdir -p $(ROOT)/build
	SAN=1 ROOT=$(ROOT) bash $(ROOT)/tests/unit/hpp_mutant_runner.sh tests/unit/test_queue_add.cpp MUT_QADD_IDLE_APPEND_PLAYS MUT_QADD_INVENT_QUEUE MUT_QADD_NO_ENCODE MUT_QADD_NEXT_SWAPPED MUT_QADD_5XX_REFUSED MUT_QADD_VERSION_IGNORED MUT_QADD_DEDUPE_OFF MUT_QADD_DEDUPE_ANY_POSITION
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_analog_raster.ok
$(ROOT)/build/test_analog_raster.ok: $(ROOT)/tests/unit/test_analog_raster.cpp $(ROOT)/tests/unit/hpp_mutant_runner.sh $(ROOT)/host/libmisterplex/analog_raster.hpp
	@mkdir -p $(ROOT)/build
	SAN=1 ROOT=$(ROOT) bash $(ROOT)/tests/unit/hpp_mutant_runner.sh tests/unit/test_analog_raster.cpp MUT_RASTER_ANY_CORE MUT_RASTER_ACTIVE_IS_REQUESTED MUT_RASTER_CFG_WRONG_BIT MUT_RASTER_UNSET_ACTS MUT_RASTER_AUTO_ACTS MUT_RASTER_NO_INFLIGHT MUT_RASTER_IGNORE_OSD MUT_RASTER_IGNORE_PLAYING MUT_RASTER_CREATE_CFG MUT_RASTER_FIGHT_OSD MUT_RASTER_NO_SETTLE MUT_RASTER_NO_CAP MUT_RASTER_TRUNCATE_CFG MUT_RASTER_NO_1101
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_analog_raster_wiring.ok
$(ROOT)/build/test_analog_raster_wiring.ok: $(ROOT)/tests/unit/test_analog_raster_wiring.sh $(ROOT)/arm/misterplexd/main.cpp \
		$(ROOT)/arm/misterplexd/media_player.cpp $(ROOT)/host/libmisterplex/plexob_ini.hpp
	@mkdir -p $(ROOT)/build
	MPX_SOURCE_ROOT=$(ROOT) bash $(ROOT)/tests/unit/test_analog_raster_wiring.sh
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_track_shortcuts.ok
$(ROOT)/build/test_track_shortcuts.ok: $(ROOT)/tests/unit/test_track_shortcuts.cpp $(ROOT)/tests/unit/hpp_mutant_runner.sh $(ROOT)/host/libmisterplex/track_shortcuts.hpp $(ROOT)/host/libmisterplex/track_select.hpp
	@mkdir -p $(ROOT)/build
	SAN=1 ROOT=$(ROOT) CXX="$(CXX)" bash $(ROOT)/tests/unit/hpp_mutant_runner.sh tests/unit/test_track_shortcuts.cpp MUT_TK_NO_WRAP MUT_TK_SUB_NO_OFF MUT_TK_IMAGE_NOT_SKIPPED MUT_TK_IGNORE_PLAYING MUT_TK_ENABLED_IGNORED MUT_TK_REPEAT_ON_HOLD MUT_TK_NO_DEBOUNCE MUT_TK_OTHER_PART MUT_TK_CHORD_NO_MOD MUT_TK_AUDIO_TAKES_SUB
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_update_manifest.ok
$(ROOT)/build/test_update_manifest.ok: $(ROOT)/tests/unit/test_update_manifest.cpp $(ROOT)/tests/unit/hpp_mutant_runner.sh $(ROOT)/host/libmisterplex/update_manifest.hpp $(ROOT)/host/libmisterplex/update_sha256.hpp
	@mkdir -p $(ROOT)/build
	SAN=1 ROOT=$(ROOT) bash $(ROOT)/tests/unit/hpp_mutant_runner.sh tests/unit/test_update_manifest.cpp MUT_MANIFEST_NO_VERIFY MUT_CONF_DEFAULT_ON MUT_VERSION_LEXICAL
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_update_swap.ok
$(ROOT)/build/test_update_swap.ok: $(ROOT)/tests/unit/test_update_swap.cpp $(ROOT)/tests/unit/hpp_mutant_runner.sh $(ROOT)/host/libmisterplex/update_swap.hpp $(ROOT)/host/libmisterplex/update_manifest.hpp $(ROOT)/host/libmisterplex/update_sha256.hpp
	@mkdir -p $(ROOT)/build
	SAN=1 ROOT=$(ROOT) bash $(ROOT)/tests/unit/hpp_mutant_runner.sh tests/unit/test_update_swap.cpp MUT_MARKER_ANY MUT_NO_ROLLBACK_ON_UNHEALTHY MUT_UPDATE_IGNORES_ENABLE MUT_UPDATE_WHILE_PLAYING MUT_ALLOW_DOWNGRADE MUT_AUX_UNGATED MUT_NO_SIZE_CAP MUT_ACCEPT_TRUNCATED MUT_SKIP_HASH MUT_NO_PREV MUT_SWAP_NONATOMIC
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_update_realfs.ok
$(ROOT)/build/test_update_realfs.ok: $(ROOT)/tests/unit/test_update_realfs.cpp $(ROOT)/tests/unit/hpp_mutant_runner.sh $(ROOT)/host/libmisterplex/update_realfs.hpp $(ROOT)/host/libmisterplex/update_swap.hpp $(ROOT)/host/libmisterplex/update_manifest.hpp $(ROOT)/host/libmisterplex/update_sha256.hpp
	@mkdir -p $(ROOT)/build
	SAN=1 ROOT=$(ROOT) bash $(ROOT)/tests/unit/hpp_mutant_runner.sh tests/unit/test_update_realfs.cpp MUT_SWAP_NONATOMIC
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_update_ed25519.ok
$(ROOT)/build/test_update_ed25519.ok: $(ROOT)/tests/unit/test_update_ed25519.cpp $(ROOT)/tests/unit/hpp_mutant_runner.sh $(ROOT)/host/libmisterplex/update_ed25519.hpp $(ROOT)/host/libmisterplex/update_swap.hpp $(ROOT)/host/libmisterplex/update_manifest.hpp $(ROOT)/host/libmisterplex/update_sha256.hpp
	@mkdir -p $(ROOT)/build
	LDLIBS=-lcrypto SAN=1 ROOT=$(ROOT) bash $(ROOT)/tests/unit/hpp_mutant_runner.sh tests/unit/test_update_ed25519.cpp MUT_ED_ALWAYS_TRUE MUT_ED_EMPTY_MSG MUT_MANIFEST_NO_VERIFY
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_update_wire.ok
$(ROOT)/build/test_update_wire.ok: $(ROOT)/tests/unit/test_update_wire.cpp $(ROOT)/tests/unit/hpp_mutant_runner.sh $(ROOT)/host/libmisterplex/update_wire.hpp $(ROOT)/host/libmisterplex/update_ed25519.hpp $(ROOT)/host/libmisterplex/update_realfs.hpp $(ROOT)/host/libmisterplex/update_swap.hpp $(ROOT)/host/libmisterplex/update_manifest.hpp
	@mkdir -p $(ROOT)/build
	LDLIBS="-lcrypto -pthread" SAN=1 ROOT=$(ROOT) bash $(ROOT)/tests/unit/hpp_mutant_runner.sh tests/unit/test_update_wire.cpp MUT_WIRE_ZERO_KEY_OK MUT_WIRE_ALLOW_HTTP MUT_ED_ALWAYS_TRUE MUT_UPDATE_IGNORES_ENABLE MUT_UPDATE_WHILE_PLAYING
	@M=$(ROOT)/arm/misterplexd/main.cpp; for w in 'recoverOnBoot(updFs, up)' 'keyUsable(MPX_UPDATE_PUBKEY_HEX)' 'followRedirects = false' 'setUpdateStart' 'makeHttpsFetch' 'UPDATE_ENABLE'; do grep -qF "$$w" $$M || { echo "update wiring missing in main.cpp: $$w"; exit 1; }; done; ! grep -q 'UPDATE_ENABLE=1' $$M || { echo 'update default must stay off'; exit 1; }
	touch $@
# PR #30 follow-up 10-23, Job Q 10-24: the boot script keeps the prod environment (MiSTerPlex.rbf first, set -u safe PLEX_RBF, conf MISTERPLEXD_BIN, MPX_PREWARM=0).
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_repro_arm_build_order.ok
$(ROOT)/build/test_repro_arm_build_order.ok: $(ROOT)/tests/unit/test_repro_arm_build_order.sh $(ROOT)/scripts/repro_arm_build.sh
	bash $(ROOT)/tests/unit/test_repro_arm_build_order.sh
	@mkdir -p $(ROOT)/build
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_boot_rc3_prod_env.ok
$(ROOT)/build/test_boot_rc3_prod_env.ok: $(ROOT)/tests/unit/test_boot_rc3_prod_env.sh $(ROOT)/scripts/misterplexd-boot-rc3.sh
	bash $(ROOT)/tests/unit/test_boot_rc3_prod_env.sh
	@mkdir -p $(ROOT)/build
	touch $@
# Goal #54. LAN-origin Plex Web playbar userscript. Host node test only; daemon untouched.
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_plexweb_userscript.ok
$(ROOT)/build/test_plexweb_userscript.ok: $(ROOT)/tests/unit/test_plexweb_userscript.sh \
		$(ROOT)/tests/unit/test_plexweb_userscript.js \
		$(ROOT)/docs/scripts/plexweb-playbar/mister-playbar-identity.user.js
	@mkdir -p $(ROOT)/build
	bash $(ROOT)/tests/unit/test_plexweb_userscript.sh
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_auto_picture_mode.ok
$(ROOT)/build/test_auto_picture_mode.ok: $(ROOT)/tests/unit/test_auto_picture_mode.cpp $(ROOT)/tests/unit/hpp_mutant_runner.sh $(ROOT)/host/libmisterplex/auto_picture_mode.hpp
	@mkdir -p $(ROOT)/build
	SAN=1 ROOT=$(ROOT) bash $(ROOT)/tests/unit/hpp_mutant_runner.sh tests/unit/test_auto_picture_mode.cpp MUT_AP_ALWAYS_SAME MUT_AP_SKIP_LOG MUT_AP_240_ANY_FPS MUT_AP_240_HEIGHT MUT_AP_PAIRED_LOSES MUT_AP_CONF_LOOSE MUT_AP_SUMMARY_NO_MISMATCH MUT_AP_SUMMARY_IGNORES_OFF MUT_AP_DEFAULT_ADVISE
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_airplay_features.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_handoff_hold.ok
$(ROOT)/build/test_handoff_hold.ok: $(ROOT)/tests/unit/test_handoff_hold.cpp $(ROOT)/tests/unit/hpp_mutant_runner.sh $(ROOT)/arm/misterplexd/gcast/handoff_hold.hpp $(ROOT)/arm/misterplexd/main.cpp $(ROOT)/arm/misterplexd/media_player.cpp
	@mkdir -p $(ROOT)/build
	SAN=1 ROOT=$(ROOT) bash $(ROOT)/tests/unit/hpp_mutant_runner.sh tests/unit/test_handoff_hold.cpp MUT_HH_NEVER MUT_HH_ANY_OWNER MUT_HH_ON_SEEK MUT_HH_MIRROR_EAGER MUT_HH_MIRROR_NO_STOP MUT_HH_RATE_RESUMES MUT_HH_NO_ABORT MUT_HH_NO_COMMIT
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_yt_open_budget.ok
$(ROOT)/build/test_yt_open_budget.ok: $(ROOT)/tests/unit/test_yt_open_budget.cpp $(ROOT)/tests/unit/hpp_mutant_runner.sh $(ROOT)/arm/misterplexd/gcast/yt_open_budget.hpp $(ROOT)/arm/misterplexd/owenb_av_source.cpp
	@mkdir -p $(ROOT)/build
	SAN=1 ROOT=$(ROOT) bash $(ROOT)/tests/unit/hpp_mutant_runner.sh tests/unit/test_yt_open_budget.cpp MUT_YB_FLAT MUT_YB_ALL MUT_YB_SHORT
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_airplay_mirror_pcm.ok
$(ROOT)/build/test_airplay_mirror_pcm.ok: $(ROOT)/tests/unit/test_airplay_mirror_pcm.cpp $(ROOT)/tests/unit/hpp_mutant_runner.sh $(ROOT)/arm/misterplexd/gcast/airplay_mirror_pcm.hpp
	@mkdir -p $(ROOT)/build
	SAN=1 ROOT=$(ROOT) bash $(ROOT)/tests/unit/hpp_mutant_runner.sh tests/unit/test_airplay_mirror_pcm.cpp MUT_MPCM_NO_ALIGN MUT_MPCM_NO_BOUND MUT_MPCM_WRONG_SESSION MUT_MPCM_KEEP_CARRY_INACTIVE
	@A=$(ROOT)/arm/misterplexd; grep -qF 'apCfg.onMirrorPcm' $$A/main.cpp && grep -qF 'mirrorActive_.load() && cfg_.onMirrorPcm' $$A/gcast/airplay_fe.cpp && grep -qF 'MediaPlayer::pushAirplayMirrorPcm' $$A/media_player.cpp && grep -qF 'airplayMirrorPcm_.reset()' $$A/media_player.cpp || { echo 'mirror PCM wiring missing'; exit 1; }
	touch $@
$(ROOT)/build/test_airplay_features.ok: $(ROOT)/tests/unit/test_airplay_features.cpp $(ROOT)/tests/unit/hpp_mutant_runner.sh $(ROOT)/arm/misterplexd/gcast/airplay_features.hpp
	@mkdir -p $(ROOT)/build
	SAN=1 ROOT=$(ROOT) bash $(ROOT)/tests/unit/hpp_mutant_runner.sh tests/unit/test_airplay_features.cpp MUT_APF_BIT27_MIRROR_ONLY
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_spelunkr_schedule.ok
$(ROOT)/build/test_spelunkr_schedule: $(ROOT)/tests/unit/test_spelunkr_schedule.cpp \
		$(ROOT)/arm/misterplexd/spelunkr/broadcast.hpp \
		$(ROOT)/arm/misterplexd/spelunkr/archive_resolve.hpp \
		$(ROOT)/arm/misterplexd/spelunkr/dead_run.hpp \
		$(ROOT)/arm/misterplexd/spelunkr/schedule.hpp \
		$(ROOT)/arm/misterplexd/spelunkr/fe.hpp \
		$(ROOT)/arm/misterplexd/spelunkr/aspect.hpp \
		$(ROOT)/arm/misterplexd/spelunkr/banner.hpp \
		$(ROOT)/arm/misterplexd/spelunkr/header_cache.hpp \
		$(ROOT)/arm/misterplexd/spelunkr/prefetch_timeout.hpp \
		$(ROOT)/arm/misterplexd/gcast/mini_json.hpp \
		$(ROOT)/arm/misterplexd/main.cpp \
		$(ROOT)/host/libmisterplex/owenb_menu.hpp \
		$(ROOT)/host/libmisterplex/owenb_osd_renderer.hpp \
		$(ROOT)/host/libmisterplex/owenb_osd_draw.hpp \
		$(ROOT)/host/libmisterplex/input_mailbox.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -std=c++17 -O1 -Wall -Wextra -pthread -I$(ROOT)/host -I$(ROOT)/arm/misterplexd \
		-I$(OWENB_RING_DIR)/include \
		-DSPELUNKR_FIXTURE_DIR='"$(ROOT)/tests/unit/fixtures/spelunkr"' -DMPX_SOURCE_ROOT='"$(ROOT)"' \
		-o $@ $(ROOT)/tests/unit/test_spelunkr_schedule.cpp
$(ROOT)/build/test_spelunkr_schedule.ok: $(ROOT)/build/test_spelunkr_schedule \
		$(ROOT)/tests/unit/fixtures/spelunkr/lineup.json \
		$(ROOT)/tests/unit/fixtures/spelunkr/happy-trees-v12.json
	$(ROOT)/build/test_spelunkr_schedule
	$(CXX) $(CXXFLAGS) -std=c++17 -O1 -Wall -Wextra -pthread -I$(ROOT)/host -I$(ROOT)/arm/misterplexd \
		-I$(OWENB_RING_DIR)/include \
		-DSPELUNKR_FIXTURE_DIR='"$(ROOT)/tests/unit/fixtures/spelunkr"' -DMPX_SOURCE_ROOT='"$(ROOT)"' -DMUT_SP_SKIP_STARTINDEX \
		-o $(ROOT)/build/test_spelunkr_schedule.mut $(ROOT)/tests/unit/test_spelunkr_schedule.cpp
	if $(ROOT)/build/test_spelunkr_schedule.mut >/dev/null 2>&1; then echo 'MUTANT SURVIVED: MUT_SP_SKIP_STARTINDEX'; exit 1; fi
	@echo 'test_spelunkr_schedule: MUT_SP_SKIP_STARTINDEX FAIL as required'
	touch $@
.PHONY: test-spelunkr test-spelunkr-mutants
test-spelunkr: $(ROOT)/build/test_spelunkr_schedule.ok $(ROOT)/build/test_owenb_play \
		$(ROOT)/build/test_ddr_frame_geometry_coded_source
	$(ROOT)/build/test_owenb_play --caps-only
	$(ROOT)/build/test_owenb_play --eof-only
	python3 $(ROOT)/tests/unit/test_spelunkr_auto_video.py
	$(ROOT)/build/test_ddr_frame_geometry_coded_source
	$(if $(OWENB_AV_E2E_OK),$(MAKE) -f $(firstword $(MAKEFILE_LIST)) $(ROOT)/build/test_spelunkr_header_cache_av.ok,@echo "SKIP test_spelunkr_header_cache_av (no FFmpeg dev libs)")

test-spelunkr-mutants: $(ROOT)/build/test_spelunkr_schedule $(ROOT)/build/test_owenb_play \
		$(ROOT)/build/test_ffmpeg_header_relay
	python3 $(ROOT)/tests/unit/test_spelunkr_continuous_mutants.py

unit-unlocked unit-host-unlocked: test-spelunkr

.PHONY: test-service-switch
test-service-switch: $(ROOT)/build/test_service_switch
	$(ROOT)/build/test_service_switch
	python3 $(ROOT)/tests/unit/test_service_switch_script.py
	python3 $(ROOT)/tests/unit/test_service_status_rtl.py

$(ROOT)/build/test_service_switch: $(ROOT)/tests/unit/test_service_switch.cpp \
		$(ROOT)/host/libmisterplex/service_switch.hpp $(MPLEX_HDR)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(OWENB_SESS_FLAGS) -o $@ $<

unit-unlocked unit-host-unlocked: test-service-switch
$(ROOT)/build/test_spelunkr_schedule: $(ROOT)/arm/misterplexd/spelunkr/video.hpp
$(ROOT)/build/test_airplay_advertise: $(ROOT)/tests/unit/test_airplay_advertise.cpp \
		$(ROOT)/arm/misterplexd/gcast/airplay_fe.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -DMPX_SOURCE_ROOT='"$(ROOT)"' -o $@ \
		$(ROOT)/tests/unit/test_airplay_advertise.cpp
$(ROOT)/build/test_airplay_advertise.ok: $(ROOT)/build/test_airplay_advertise
	$(ROOT)/build/test_airplay_advertise
	@mkdir -p $(ROOT)/build
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_airplay_advertise.ok
$(ROOT)/build/test_airplay_admit: $(ROOT)/tests/unit/test_airplay_admit.cpp \
		$(ROOT)/arm/misterplexd/gcast/airplay_admit.hpp \
		$(ROOT)/arm/misterplexd/gcast/airplay_state.hpp \
		$(ROOT)/host/libmisterplex/owenb_osd_renderer.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -I$(ROOT)/third_party/owenb-h264ring/include \
		-DMPX_SOURCE_ROOT='"$(ROOT)"' -o $@ $(ROOT)/tests/unit/test_airplay_admit.cpp
$(ROOT)/build/test_airplay_admit.ok: $(ROOT)/build/test_airplay_admit
	$(ROOT)/build/test_airplay_admit
	@mkdir -p $(ROOT)/build
	touch $@
$(ROOT)/build/test_airplay_admit.mutant-check: $(ROOT)/build/test_airplay_admit
	@$(ROOT)/build/test_airplay_admit > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -I$(ROOT)/third_party/owenb-h264ring/include \
		-DMPX_SOURCE_ROOT='"$(ROOT)"' -DMUT_AP_INFO_1080 \
		-o $(ROOT)/build/test_airplay_admit.mut $(ROOT)/tests/unit/test_airplay_admit.cpp
	@if $(ROOT)/build/test_airplay_admit.mut > $@.mut.log 2>&1; then \
		echo "test_airplay_admit: MUT_AP_INFO_1080 SURVIVED"; exit 1; fi
	@grep -q "info is 1280x720 at 30" $@.mut.log || { echo "MUTANT MUT_AP_INFO_1080 KILLED BY THE WRONG CHECK"; cat $@.mut.log; exit 1; }
	@echo "test_airplay_admit: MUT_AP_INFO_1080 FAIL as required" | tee $@
	@rm -f $(ROOT)/build/test_airplay_admit.mut
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_airplay_admit.ok
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_airplay_admit.mutant-check
$(ROOT)/build/test_airplay_mirror: $(ROOT)/tests/unit/test_airplay_mirror.cpp \
		$(ROOT)/arm/misterplexd/gcast/airplay_mirror.hpp \
		$(ROOT)/arm/misterplexd/gcast/airplay_fe.hpp \
		$(ROOT)/arm/misterplexd/gcast/airplay_state.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host \
		-I$(ROOT)/third_party/owenb-h264ring/include \
		-DMPX_SOURCE_ROOT='"$(ROOT)"' -o $@ $(ROOT)/tests/unit/test_airplay_mirror.cpp
$(ROOT)/build/test_airplay_mirror.ok: $(ROOT)/build/test_airplay_mirror
	$(ROOT)/build/test_airplay_mirror
	@mkdir -p $(ROOT)/build
	touch $@
$(ROOT)/build/test_airplay_mirror.MUT_AP_MIRROR_DEFAULT_ON: $(ROOT)/tests/unit/test_airplay_mirror.cpp \
		$(ROOT)/arm/misterplexd/gcast/airplay_mirror.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host \
		-I$(ROOT)/third_party/owenb-h264ring/include \
		-DMPX_SOURCE_ROOT='"$(ROOT)"' -DMUT_AP_MIRROR_DEFAULT_ON -o $@ \
		$(ROOT)/tests/unit/test_airplay_mirror.cpp
$(ROOT)/build/test_airplay_mirror.MUT_AP_MIRROR_NO_CAP: $(ROOT)/tests/unit/test_airplay_mirror.cpp \
		$(ROOT)/arm/misterplexd/gcast/airplay_mirror.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host \
		-I$(ROOT)/third_party/owenb-h264ring/include \
		-DMPX_SOURCE_ROOT='"$(ROOT)"' -DMUT_AP_MIRROR_NO_CAP -o $@ \
		$(ROOT)/tests/unit/test_airplay_mirror.cpp
$(ROOT)/build/test_airplay_mirror.MUT_AP_MIRROR_NO_ANNEXB: $(ROOT)/tests/unit/test_airplay_mirror.cpp \
		$(ROOT)/arm/misterplexd/gcast/airplay_mirror.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host \
		-I$(ROOT)/third_party/owenb-h264ring/include \
		-DMPX_SOURCE_ROOT='"$(ROOT)"' -DMUT_AP_MIRROR_NO_ANNEXB -o $@ \
		$(ROOT)/tests/unit/test_airplay_mirror.cpp
$(ROOT)/build/test_airplay_mirror.MUT_AP_MIRROR_NO_CTR: $(ROOT)/tests/unit/test_airplay_mirror.cpp \
		$(ROOT)/arm/misterplexd/gcast/airplay_mirror.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host \
		-I$(ROOT)/third_party/owenb-h264ring/include \
		-DMPX_SOURCE_ROOT='"$(ROOT)"' -DMUT_AP_MIRROR_NO_CTR -o $@ \
		$(ROOT)/tests/unit/test_airplay_mirror.cpp
$(ROOT)/build/test_airplay_mirror.MUT_AP_MIRROR_IGNORE_OPTIN: $(ROOT)/tests/unit/test_airplay_mirror.cpp \
		$(ROOT)/arm/misterplexd/gcast/airplay_mirror.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host \
		-I$(ROOT)/third_party/owenb-h264ring/include \
		-DMPX_SOURCE_ROOT='"$(ROOT)"' -DMUT_AP_MIRROR_IGNORE_OPTIN -o $@ \
		$(ROOT)/tests/unit/test_airplay_mirror.cpp
$(ROOT)/build/test_airplay_mirror.mutant-check: $(ROOT)/build/test_airplay_mirror \
		$(ROOT)/build/test_airplay_mirror.MUT_AP_MIRROR_DEFAULT_ON \
		$(ROOT)/build/test_airplay_mirror.MUT_AP_MIRROR_NO_CAP \
		$(ROOT)/build/test_airplay_mirror.MUT_AP_MIRROR_NO_ANNEXB \
		$(ROOT)/build/test_airplay_mirror.MUT_AP_MIRROR_NO_CTR \
		$(ROOT)/build/test_airplay_mirror.MUT_AP_MIRROR_IGNORE_OPTIN
	@$(ROOT)/build/test_airplay_mirror > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@if $(ROOT)/build/test_airplay_mirror.MUT_AP_MIRROR_DEFAULT_ON > $@.d.log 2>&1; then \
		echo "MUTANT SURVIVED: MUT_AP_MIRROR_DEFAULT_ON"; exit 1; fi
	@if $(ROOT)/build/test_airplay_mirror.MUT_AP_MIRROR_NO_CAP > $@.c.log 2>&1; then \
		echo "MUTANT SURVIVED: MUT_AP_MIRROR_NO_CAP"; exit 1; fi
	@if $(ROOT)/build/test_airplay_mirror.MUT_AP_MIRROR_NO_ANNEXB > $@.a.log 2>&1; then \
		echo "MUTANT SURVIVED: MUT_AP_MIRROR_NO_ANNEXB"; exit 1; fi
	@if $(ROOT)/build/test_airplay_mirror.MUT_AP_MIRROR_NO_CTR > $@.k.log 2>&1; then \
		echo "MUTANT SURVIVED: MUT_AP_MIRROR_NO_CTR"; exit 1; fi
	@if $(ROOT)/build/test_airplay_mirror.MUT_AP_MIRROR_IGNORE_OPTIN > $@.o.log 2>&1; then \
		echo "MUTANT SURVIVED: MUT_AP_MIRROR_IGNORE_OPTIN"; exit 1; fi
	@echo "test_airplay_mirror: mutants FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_airplay_mirror.ok \
	$(ROOT)/build/test_airplay_mirror.mutant-check
$(ROOT)/build/test_uxplay_hdr431.ok: $(ROOT)/tests/unit/test_uxplay_hdr431.sh \
		$(ROOT)/tests/unit/test_uxplay_hdr431.c \
		$(ROOT)/tools/gcast/uxplay-patches/apply_gcast_uxplay_hdr431.py \
		$(ROOT)/scripts/build_arm_airplay.sh
	bash $(ROOT)/tests/unit/test_uxplay_hdr431.sh $(ROOT)
	@mkdir -p $(ROOT)/build
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_uxplay_hdr431.ok
$(ROOT)/build/test_uxplay_twitterav.ok: $(ROOT)/tests/unit/test_uxplay_twitterav.sh \
		$(ROOT)/tests/unit/test_uxplay_twitterav.c \
		$(ROOT)/tools/gcast/uxplay-patches/gcast_twitterav.h \
		$(ROOT)/tools/gcast/uxplay-patches/apply_gcast_uxplay_twitterav.py \
		$(ROOT)/scripts/build_arm_airplay.sh
	bash $(ROOT)/tests/unit/test_uxplay_twitterav.sh $(ROOT)
	@mkdir -p $(ROOT)/build
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_uxplay_twitterav.ok
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
$(ROOT)/build/test_notice_source_offer.ok: $(ROOT)/tests/unit/test_notice_source_offer.py \
		$(ROOT)/NOTICE $(ROOT)/README.md $(ROOT)/packaging/rc3/README.md \
		$(ROOT)/scripts/package_release.sh $(ROOT)/scripts/render_notice.py
	python3 $(ROOT)/tests/unit/test_notice_source_offer.py $(ROOT)
	python3 $(ROOT)/tests/unit/test_notice_source_offer.py $(ROOT) --mutants
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
	$(CXX) $(CXXFLAGS) $(MPLEX_INC) $(MPX_GIT_DEFINE) $(OWENB_DAEMON_FLAGS) -DMPX_TEST_BUILD -pthread -o $@ $(MPLEX_SRC) \
		$(OWENB_PLAY_SRCS) $(OWENB_RING_OBJS) -lm -lcrypto

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
ARM_TOOLCHAIN_BIN ?= $(HOME)/Projects/mistercast-linux/third_party/arm-gnu-toolchain/bin
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
ARM_PLEXD_SPELUNKR ?= 0
ifeq ($(ARM_PLEXD_SPELUNKR),1)
ARM_SERVICE_FLAGS := -DMPX_SPELUNKR_BINARY
endif
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
	arm-linux-gnueabihf-gcc -std=c11 -O2 -Wall -no-pie -D_FILE_OFFSET_BITS=64 -D_TIME_BITS=64 \
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

# Kernel tree for arm-kmod (a MiSTer Linux kernel checkout). Set by the
# operator; there is no default in-tree path.
KDIR_MISTER ?=
ARM_CROSS ?= arm-none-linux-gnueabihf-
# K1 KM-0: the module must be built with the compiler that configured KDIR_MISTER (CONFIG_GCC_PLUGINS=y: the
# arm_ssp_per_task plugin only loads in that gcc). The MiSTer 5.15.1 tree wants Arm GNU Toolchain 10.2-2020.11.
KMOD_CROSS ?= $(HOME)/mistercast/toolchains/gcc-arm-10.2-2020.11-x86_64-arm-none-linux-gnueabihf/bin/arm-none-linux-gnueabihf-
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
	@bash $(ROOT)/scripts/check_arm_toolchain_consistency.sh "$(ARM_CXX:g++=gcc)" "$(ARM_AIRPLAY_PREFIX)" "$(ARM_FFMPEG_PREFIX)"
endif
	$(ARM_CXX) -std=c++17 -O2 -Wall -static -ffile-prefix-map=$(ROOT)=. -D_TIME_BITS=64 -D_FILE_OFFSET_BITS=64 $(MPLEX_INC) $(MPX_GIT_DEFINE) -DMPX_HAVE_LIBAV $(ARM_FFMPEG_CFLAGS) \
		$(OWENB_DAEMON_FLAGS) -DMPX_FPGA_AV_TRACE=$(ARM_PLEXD_AV_TRACE) $(ARM_AIRPLAY_FLAGS) $(ARM_SERVICE_FLAGS) \
		-o "$(ARM_PLEXD_OUTPUT)" $(MPLEX_SRC) \
		$(ROOT)/arm/misterplexd/av_inproc_decode.cpp $(ROOT)/arm/misterplexd/owenb_av_source.cpp \
		$(filter-out %/theme_decode.cpp,$(OWENB_PLAY_SRCS)) $(OWENB_RING_ARM_OBJS) \
		$(ARM_FFMPEG_LIBS) $(ARM_AIRPLAY_LIBS) \
		-Wl,--whole-archive -lpthread -Wl,--no-whole-archive
	@echo MPX_HAVE_LIBAV=YES prefix=$(ARM_FFMPEG_PREFIX)
else
	@echo MPX_HAVE_LIBAV=NO prefix=$(ARM_FFMPEG_PREFIX)
	@echo "arm-plexd-daemon: libav static libs missing under $(ARM_FFMPEG_PREFIX)." >&2
	@echo "GH#9: a no-libav daemon cannot arm OB idle (no Ready card). ALLOW_NO_AIRPLAY drops AirPlay only." >&2
	@echo "Build them with 'make arm-ffmpeg' and scripts/build_arm_airplay.sh." >&2
	@exit 1
endif
	# Arm GNU's static libc and libstdc++ are built -g. Linking them leaves
	# about 14 MB of DWARF in the ship binary (the c9fb88e daemon was 25 MB
	# with a 6.4 MB .text). 55ead85's 11 MB daemon was linked by Ubuntu's
	# armhf g++ and kept only a little debug. Drop .debug_* after the link.
	# The symbol table stays. The ASan binary is built separately and is not
	# stripped.
	$(patsubst %g++,%strip,$(ARM_CXX)) --strip-debug "$(ARM_PLEXD_OUTPUT)"
	python3 $(ROOT)/scripts/redact_elf_build_paths.py "$(ARM_PLEXD_OUTPUT)"
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
	$(MAKE) --no-print-directory $(ROOT)/build/arm/owenb_idle_card
	@file $(ROOT)/build/arm/push_frame $(ROOT)/build/arm/set_status $(ROOT)/build/arm/input_mailbox_probe $(ROOT)/build/arm/owenb_idle_card
	@echo "Built push_frame + set_status + input_mailbox_probe + owenb_idle_card"

# Colorbar removal 10-23: the "daemon not running" painter, armhf static, owenb ring/session/overlay only (no libav).
$(ROOT)/build/arm/owenb_idle_card: $(ROOT)/tools/owenb_idle_card.cpp $(ROOT)/host/libmisterplex/owenb_idle_card_ctl.hpp \
		$(OWENB_PLAY_SRCS) $(OWENB_RING_ARM_OBJS)
	@if [ -z "$(ARM_CXX)" ]; then echo "No armhf g++ found"; exit 1; fi
	@mkdir -p $(ROOT)/build/arm
	$(ARM_CXX) -std=c++17 -O2 -Wall -static -D_TIME_BITS=64 -D_FILE_OFFSET_BITS=64 -DMPX_HAVE_OWENB_RING -I$(ROOT)/host \
		$(OWENB_SESS_FLAGS) -o $@ $(ROOT)/tools/owenb_idle_card.cpp $(OWENB_PLAY_SRCS) $(OWENB_RING_ARM_OBJS) \
		-Wl,--whole-archive -lpthread -Wl,--no-whole-archive
	$(patsubst %g++,%strip,$(ARM_CXX)) --strip-debug $@

$(ROOT)/build/arm/input_mailbox_probe: $(ROOT)/tools/input_mailbox_probe.cpp \
		$(ROOT)/host/libmisterplex/input_mailbox.hpp \
		$(ROOT)/host/libmisterplex/fpga_build_ids.hpp \
		$(ROOT)/host/libmisterplex/plx_page.hpp \
		$(ROOT)/host/libmisterplex/mailbox_abi_spec.hpp
	@if [ -z "$(ARM_CXX)" ]; then echo "No armhf g++ found"; exit 1; fi
	@mkdir -p $(ROOT)/build/arm
	$(ARM_CXX) -std=c++17 -O2 -Wall -I$(ROOT)/host \
		-o $@ $(ROOT)/tools/input_mailbox_probe.cpp -static

MISTER_DEV ?= $(HOME)/Projects/misterfpga-dev
build-rbf:
	$(MISTER_DEV)/scripts/mister-dev build $(ROOT)/fpga/Plex_MiSTer --qpf Plex.qpf

package:
	$(ROOT)/scripts/package_release.sh
	REQUIRE_ARTIFACT=1 $(ROOT)/tests/unit/test_no_private_data.sh
	REQUIRE_ARTIFACT=1 $(ROOT)/tests/unit/test_release_rbf_hash.sh

clean:
	rm -rf $(ROOT)/build

$(ROOT)/build/test_gdm_filter: $(ROOT)/tests/unit/test_gdm_filter.cpp \
		$(ROOT)/host/libmisterplex/gdm_filter.hpp $(ROOT)/arm/misterplexd/companion.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -o $@ $(ROOT)/tests/unit/test_gdm_filter.cpp
$(ROOT)/build/test_gdm_filter.ok: $(ROOT)/build/test_gdm_filter
	cd $(ROOT) && $(ROOT)/build/test_gdm_filter
	$(CXX) $(CXXFLAGS) -DMUT_GDM_WAN_REPLY -o $(ROOT)/build/test_gdm_filter.mut \
		$(ROOT)/tests/unit/test_gdm_filter.cpp
	@if cd $(ROOT) && $(ROOT)/build/test_gdm_filter.mut >/dev/null 2>&1; then \
		echo "MUTANT SURVIVED: MUT_GDM_WAN_REPLY"; exit 1; fi
	@echo "test_gdm_filter: MUT_GDM_WAN_REPLY FAIL as required"
	@rm -f $(ROOT)/build/test_gdm_filter.mut
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_gdm_filter.ok

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

# T-UNIT-DANGLING-3 (rev 2 §3.4, F2; FPGA Bug 09:03 C1): MUT_C1_LOCK_ORDER_ABBA must trip the C1 watchdog. It runs
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

# F1 PLXL beat (F1-PREREG v2.2 ba1ae917; FPGA Bug 01:39 k1-k3, H264 53b1779c x1-x3): f1_beat.hpp's Beat on the
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

# MPX_PREWARM probe: off by default, binary-safe status read ( lab C1 http=0).
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

# Decision calls per seek burst ( lab A3 55 s recovery): seek reuses the session; superseded resolves skip the decision.
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
	@for m in MUT_NO_VEF_TERM:E1 MUT_VEF_CABAC_ON:E2 MUT_340_DECISION_QUERY:E6t; do n=$${m%%:*}; pin=$${m##*:}; \
	  $(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -D$$n -o $@.$$n $(ROOT)/tests/unit/test_encode_flags_extra.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp || exit 1; \
	  if $@.$$n > $@.$$n.log 2>&1; then echo "MUTANT SURVIVED: $$n"; exit 1; fi; \
	  grep -m1 '^FAIL ' $@.$$n.log | grep -q "FAIL $$pin " || { echo "MUTANT KILLED BY THE WRONG CHECK: $$n"; cat $@.$$n.log; exit 1; }; done
	@echo "test_encode_flags_extra: mutants MUT_NO_VEF_TERM MUT_VEF_CABAC_ON MUT_340_DECISION_QUERY FAIL at their pinned checks as required"
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

# SEC-M2: companion :3005 peer ACL + browser Origin allow-list + playMedia target check, through the real httpLoop.
$(ROOT)/build/test_cast_token_flow.ok: \
		$(ROOT)/tests/unit/test_cast_token_flow.sh $(ROOT)/tests/unit/test_cast_token_flow.cpp \
		$(ROOT)/arm/misterplexd/cast_token_flow.hpp $(ROOT)/arm/misterplexd/pms_reach.hpp \
		$(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/companion.hpp
	@mkdir -p $(ROOT)/build
	ROOT=$(ROOT) CXX=$(CXX) bash $(ROOT)/tests/unit/test_cast_token_flow.sh
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_cast_token_flow.ok

$(ROOT)/build/test_companion_guard_http.ok: \
		$(ROOT)/tests/unit/test_companion_guard_http.sh $(ROOT)/tests/unit/test_companion_guard_http.cpp \
		$(ROOT)/host/libmisterplex/companion_guard.hpp $(ROOT)/arm/misterplexd/companion.cpp \
		$(ROOT)/arm/misterplexd/companion.hpp $(ROOT)/arm/misterplexd/pms_timeline.cpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/host/libmisterplex/plexui/plexui_http.cpp
	@mkdir -p $(ROOT)/build
	ROOT=$(ROOT) SAN=1 bash $(ROOT)/tests/unit/test_companion_guard_http.sh
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_companion_guard_http.ok

# N2/N3: the DIAL port (8008) refuses web Origins / rebinding Hosts / non-LAN peers and sends no CORS grant; real YtFrontend, live sockets.
$(ROOT)/build/test_dial_admit_live.ok: \
		$(ROOT)/tests/unit/test_dial_admit_live.sh $(ROOT)/tests/unit/test_dial_admit_live.cpp \
		$(ROOT)/arm/misterplexd/gcast/yt_fe.cpp $(ROOT)/arm/misterplexd/gcast/dial_proto.hpp \
		$(ROOT)/host/libmisterplex/companion_guard.hpp
	@mkdir -p $(ROOT)/build
	ROOT=$(ROOT) SAN=1 bash $(ROOT)/tests/unit/test_dial_admit_live.sh
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_dial_admit_live.ok

# Queue play-next wiring: POST /misterplex/queue/add through the real httpLoop with a stub PMS, guard in front.
$(ROOT)/build/test_queue_add_http.ok: \
		$(ROOT)/tests/unit/test_queue_add_http.sh $(ROOT)/tests/unit/test_queue_add_http.cpp \
		$(ROOT)/arm/misterplexd/queue_add_http.hpp $(ROOT)/arm/misterplexd/queue_add.hpp \
		$(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/companion.hpp \
		$(ROOT)/arm/misterplexd/pms_timeline.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/host/libmisterplex/plexui/plexui_http.cpp
	@mkdir -p $(ROOT)/build
	ROOT=$(ROOT) SAN=1 bash $(ROOT)/tests/unit/test_queue_add_http.sh
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_queue_add_http.ok

# Queue play-next PMS transport: real no-shell curl GET/PUT against a loopback server; token header-only.
$(ROOT)/build/test_queue_add_pms.ok: \
		$(ROOT)/tests/unit/test_queue_add_pms.sh $(ROOT)/tests/unit/test_queue_add_pms.cpp \
		$(ROOT)/arm/misterplexd/queue_add_pms.hpp $(ROOT)/arm/misterplexd/queue_add_http.hpp \
		$(ROOT)/arm/misterplexd/queue_add.hpp $(ROOT)/host/libmisterplex/plexui/plexui_http.cpp \
		$(ROOT)/host/libmisterplex/plexui/plexui_http.hpp
	@mkdir -p $(ROOT)/build
	ROOT=$(ROOT) SAN=1 bash $(ROOT)/tests/unit/test_queue_add_pms.sh
	touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_queue_add_pms.ok

# GET /misterplex/status and POST /misterplex/update (issue #57). Pure renderer and router.
# Each _MUT_ build must fail. ASan/UBSan on the host test. Glass-passed page from b786a8a.
# canUpdate false: no Update button, POST does not ask for a start (503).
# canUpdate (issue #59): one-tap button; an accepted idle POST sets startUpdate. The companion
# calls the starter. This router does not fetch or swap.
PHONE_STATUS_MUTANTS := MUT_PHONE_STATUS_ANY_VERB MUT_PHONE_STATUS_ALLOW_QUERY \
	MUT_PHONE_STATUS_NO_CAP MUT_PHONE_STATUS_NO_ESCAPE MUT_PHONE_STATUS_ECHO_SECRET \
	MUT_PHONE_STATUS_SKIP_NONCE MUT_PHONE_STATUS_SKIP_BUSY MUT_PHONE_STATUS_SHOW_BUTTON \
	MUT_PHONE_STATUS_NO_START MUT_PHONE_STATUS_NO_BUTTON MUT_PHONE_STATUS_START_ANY_ORIGIN
PHONE_STATUS_SAN := -fsanitize=address,undefined -fno-omit-frame-pointer -g
PHONE_STATUS_DEPS := $(ROOT)/tests/unit/test_phone_status_http.cpp \
	$(ROOT)/host/libmisterplex/phone_status_http.hpp $(ROOT)/host/libmisterplex/report_code.hpp
$(ROOT)/build/test_phone_status_http: $(PHONE_STATUS_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(PHONE_STATUS_SAN) -pthread -o $@ $(ROOT)/tests/unit/test_phone_status_http.cpp
$(ROOT)/build/test_phone_status_http_%: $(PHONE_STATUS_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) $(PHONE_STATUS_SAN) -pthread -D$* -o $@ $(ROOT)/tests/unit/test_phone_status_http.cpp
$(ROOT)/build/test_phone_status_http.mutant-check: $(ROOT)/build/test_phone_status_http \
		$(addprefix $(ROOT)/build/test_phone_status_http_,$(PHONE_STATUS_MUTANTS))
	@$(ROOT)/build/test_phone_status_http > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	@for m in $(PHONE_STATUS_MUTANTS); do \
		if $(ROOT)/build/test_phone_status_http_$$m > $@.$$m.log 2>&1; then \
			echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
	done
	@echo "test_phone_status_http: mutants $(PHONE_STATUS_MUTANTS) FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_phone_status_http.mutant-check

# The same routes through Companion::httpLoop on a loopback port.
$(ROOT)/build/test_companion_phone_status: $(ROOT)/tests/unit/test_companion_phone_status.cpp \
		$(PHONE_STATUS_DEPS) $(ROOT)/arm/misterplexd/companion.cpp \
		$(ROOT)/arm/misterplexd/companion.hpp $(ROOT)/arm/misterplexd/companion_reqlog.hpp \
		$(ROOT)/arm/misterplexd/pms_timeline.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/host/libmisterplex/plexui/plexui_http.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host/libmisterplex/plexui -pthread \
		-DMPX_MAST_RELOAD_MARKER_PATH='"$(ROOT)/build/test_companion_phone_status.marker"' -o $@ \
		$(ROOT)/tests/unit/test_companion_phone_status.cpp $(ROOT)/arm/misterplexd/companion.cpp \
		$(ROOT)/arm/misterplexd/pms_timeline.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/host/libmisterplex/plexui/plexui_http.cpp
$(ROOT)/build/test_companion_phone_status.run: $(ROOT)/build/test_companion_phone_status
	@$(ROOT)/build/test_companion_phone_status > $@.log 2>&1 || { cat $@.log; exit 1; }
	@cp $@.log $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_companion_phone_status.run
# Dual/triple BUILD_ID allow-list (0x0B59C11A, 0x0BFCE8C5, 0x0B2403A7): the four unit tests that pin every site,
# built with ASan+UBSan. Exact match only; neighbours and unknown ids must be refused.
BUILDID_ASAN_BINS := $(ROOT)/build/test_buildid_allowlists_asan $(ROOT)/build/test_fpga_build_ids_asan $(ROOT)/build/test_core_id_select_asan \
		$(ROOT)/build/test_plexob_yc_asan $(ROOT)/build/test_owenb_core_gate_asan
$(ROOT)/build/test_buildid_allowlists_asan: $(ROOT)/tests/unit/test_buildid_allowlists.cpp \
		$(ROOT)/host/libmisterplex/fpga_build_ids.hpp $(ROOT)/host/libmisterplex/plexob_yc.hpp \
		$(ROOT)/host/libmisterplex/owenb_core_identity.hpp $(ROOT)/host/libmisterplex/core_id_select.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(PLEXUI_ASAN_CXXFLAGS) -I$(ROOT)/host -pthread -o $@ $(ROOT)/tests/unit/test_buildid_allowlists.cpp
$(ROOT)/build/test_fpga_build_ids_asan: $(ROOT)/tests/unit/test_fpga_build_ids.cpp \
		$(ROOT)/host/libmisterplex/fpga_build_ids.hpp $(ROOT)/host/libmisterplex/plexob_yc.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(PLEXUI_ASAN_CXXFLAGS) -I$(ROOT)/host -pthread -o $@ $(ROOT)/tests/unit/test_fpga_build_ids.cpp
$(ROOT)/build/test_core_id_select_asan: $(ROOT)/tests/unit/test_core_id_select.cpp \
		$(ROOT)/host/libmisterplex/core_id_select.hpp $(ROOT)/host/libmisterplex/display_raster.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(PLEXUI_ASAN_CXXFLAGS) -I$(ROOT)/host -o $@ $(ROOT)/tests/unit/test_core_id_select.cpp
$(ROOT)/build/test_plexob_yc_asan: $(ROOT)/tests/unit/test_plexob_yc.cpp $(ROOT)/host/libmisterplex/plexob_yc.hpp \
		$(ROOT)/host/libmisterplex/owenb_core_identity.hpp $(ROOT)/arm/misterplexd/media_player.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(PLEXUI_ASAN_CXXFLAGS) -I$(ROOT)/host -pthread -DMPX_SOURCE_ROOT='"$(ROOT)"' -o $@ $(ROOT)/tests/unit/test_plexob_yc.cpp
$(ROOT)/build/test_owenb_core_gate_asan: $(OWENB_GATE_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(PLEXUI_ASAN_CXXFLAGS) -I$(ROOT)/host $(OWENB_GATE_FLAGS) -o $@ $(ROOT)/tests/unit/test_owenb_core_gate.cpp
.PHONY: buildid-asan
buildid-asan: $(BUILDID_ASAN_BINS)
	@set -e; for b in $(BUILDID_ASAN_BINS); do echo "== $$b"; ASAN_OPTIONS=detect_leaks=1 $$b; done
	@SAN=1 ROOT=$(ROOT) bash $(ROOT)/tests/unit/test_combined_240p_gate.sh
# Boot hook finds the on-card gcast helper in the nested linux/gcast/gcast/ layout too (10-24).
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_boot_gcast_layout.ok
$(ROOT)/build/test_boot_gcast_layout.ok: $(ROOT)/tests/unit/test_boot_gcast_layout.sh $(ROOT)/scripts/misterplexd-boot-rc3.sh
	@mkdir -p $(ROOT)/build
	sh $(ROOT)/tests/unit/test_boot_gcast_layout.sh && touch $@

# gcast helper layout and md5 pins: the package ships it flat with MD5SUMS, both card layouts boot, docs pins are derived (10-24).
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_gcast_helper_pins_layout.ok
$(ROOT)/build/test_gcast_helper_pins_layout.ok: $(ROOT)/tests/unit/test_gcast_helper_pins_layout.sh $(ROOT)/scripts/gcast_pins.sh \
		$(ROOT)/scripts/package_release.sh $(ROOT)/scripts/misterplexd-boot-rc3.sh $(ROOT)/docs/gcast-helper.md \
		$(ROOT)/tools/gcast/gcast_xcode.py $(ROOT)/tools/gcast/yt_standalone/run-gcast-xcode.sh
	@mkdir -p $(ROOT)/build
	bash $(ROOT)/tests/unit/test_gcast_helper_pins_layout.sh && touch $@

# Hot deploys carry the on-card AirPlay helper (11-10: lab ran the 11-04 helper 01c7fce6 under 11-10 binaries, so
# X/Twitter audio was re-encoded on the ARM): packaged bytes == tools/airplay, install/verify/rollback, mutants (11-10).
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_airplay_helper_card.ok
$(ROOT)/build/test_airplay_helper_card.ok: $(ROOT)/tests/unit/test_airplay_helper_card.sh $(ROOT)/scripts/airplay_helper_card.sh \
		$(ROOT)/scripts/package_release.sh $(ROOT)/tools/airplay/airplay_xcode.py
	@mkdir -p $(ROOT)/build
	bash $(ROOT)/tests/unit/test_airplay_helper_card.sh && touch $@

# stage_raster_conf.py accepts both base conf pins (933a4104, c891be93), synthetic bases only (10-24).
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_stage_raster_conf_pins.ok
$(ROOT)/build/test_stage_raster_conf_pins.ok: $(ROOT)/tests/unit/test_stage_raster_conf_pins.py $(ROOT)/scripts/stage_raster_conf.py
	@mkdir -p $(ROOT)/build
	python3 $(ROOT)/tests/unit/test_stage_raster_conf_pins.py && touch $@

# gcast_xcode.py: -protocol_whitelist on every ffmpeg/ffprobe input, local sources refused for remote clients (#63).
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_gcast_helper_hardening.ok
$(ROOT)/build/test_gcast_helper_hardening.ok: $(ROOT)/tests/unit/test_gcast_helper_hardening.py $(ROOT)/tools/gcast/gcast_xcode.py
	@mkdir -p $(ROOT)/build
	python3 $(ROOT)/tests/unit/test_gcast_helper_hardening.py && touch $@
# #66: the live-glass pin hashes Plex.rbf in-process (no popen/md5sum, so no shell sees the path).
# The driver builds plain + ASan/UBSan and runs files named with shell metacharacters.
$(ROOT)/build/test_present_bank_md5.ok: $(ROOT)/tests/unit/test_present_bank_md5.py \
		$(ROOT)/tests/unit/test_present_bank_md5.cpp $(ROOT)/host/libmisterplex/present_bank.hpp
	@mkdir -p $(ROOT)/build
	python3 $(ROOT)/tests/unit/test_present_bank_md5.py
	@touch $@
$(ROOT)/build/test_present_bank_md5.mutant-check: $(ROOT)/build/test_present_bank_md5.ok
	python3 $(ROOT)/tests/unit/test_present_bank_md5.py --mutants
	@touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_present_bank_md5.ok $(ROOT)/build/test_present_bank_md5.mutant-check
# #67: the tarball package_release.sh writes must carry inner SHA256SUMS that verify (rc7.1 shipped a stale one).
$(ROOT)/build/test_release_inner_sums.ok: $(ROOT)/tests/unit/test_release_inner_sums.py \
		$(ROOT)/scripts/verify_release_tarball.sh $(ROOT)/scripts/package_release.sh
	@mkdir -p $(ROOT)/build
	python3 $(ROOT)/tests/unit/test_release_inner_sums.py
	@touch $@
$(ROOT)/build/test_release_inner_sums.mutant-check: $(ROOT)/build/test_release_inner_sums.ok
	python3 $(ROOT)/tests/unit/test_release_inner_sums.py --mutants
	@touch $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_release_inner_sums.ok $(ROOT)/build/test_release_inner_sums.mutant-check
# Controller track shortcuts, wired path (glue over the real ObInputDecoder). Each mutant must compile and FAIL.
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_track_shortcuts_glue.ok
$(ROOT)/build/test_track_shortcuts_glue.ok: $(ROOT)/tests/unit/test_track_shortcuts_glue.sh \
		$(ROOT)/tests/unit/test_track_shortcuts_glue.cpp $(ROOT)/host/libmisterplex/track_shortcuts_glue.hpp \
		$(ROOT)/host/libmisterplex/track_shortcuts.hpp $(ROOT)/host/libmisterplex/owenb_input.hpp $(OWENB_RING_OBJS)
	@mkdir -p $(ROOT)/build
	ROOT=$(ROOT) SAN=1 CXX="$(CXX)" bash $(ROOT)/tests/unit/test_track_shortcuts_glue.sh
	touch $@
# Playbar port 10-23: ASan+UBSan run of the unit tests that pin the playbar commits (one command per commit).
PLAYBAR_ASAN_BINS := $(ROOT)/build/test_pms_timeline_asan $(ROOT)/build/test_resolve_asan $(ROOT)/build/test_companion_live_clock_asan
$(ROOT)/build/test_pms_timeline_asan: $(ROOT)/tests/unit/test_pms_timeline.cpp $(ROOT)/arm/misterplexd/pms_timeline.cpp \
		$(ROOT)/arm/misterplexd/pms_timeline.hpp $(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/arm/misterplexd/plex_resolve.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(PLEXUI_ASAN_CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -o $@ \
		$(ROOT)/tests/unit/test_pms_timeline.cpp $(ROOT)/arm/misterplexd/pms_timeline.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp
$(ROOT)/build/test_resolve_asan: $(TEST_RESOLVE_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(PLEXUI_ASAN_CXXFLAGS) -I$(ROOT)/arm/misterplexd -o $@ \
		$(ROOT)/tests/unit/test_resolve.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp
$(ROOT)/build/test_companion_live_clock_asan: $(ROOT)/tests/unit/test_companion_live_clock.cpp \
		$(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/arm/misterplexd/companion.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(PLEXUI_ASAN_CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -o $@ \
		$(ROOT)/tests/unit/test_companion_live_clock.cpp $(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/host/libmisterplex/plexui/plexui_http.cpp
.PHONY: playbar-asan
playbar-asan: $(PLAYBAR_ASAN_BINS)
	@set -e; for b in $(PLAYBAR_ASAN_BINS); do echo "== $$b"; ASAN_OPTIONS=detect_leaks=1 $$b; done
	python3 $(ROOT)/tests/unit/test_companion_timeline_tick.py
	python3 $(ROOT)/tests/unit/test_playbar_source_pins.py
# Playbar IPv6 lead 10-24: ASan+UBSan unit tests that mock the unreachable IPv6 ULA Plex Web hands the cast target.
IPV6_ASAN_BINS := $(ROOT)/build/test_ipv6_fallback_host_asan $(ROOT)/build/test_pms_reach_asan $(ROOT)/build/test_plextv_local_asan $(ROOT)/build/test_pms_timeline_ipv6_asan $(ROOT)/build/test_net_infra_asan
$(ROOT)/build/test_ipv6_fallback_host_asan: $(ROOT)/tests/unit/test_ipv6_fallback_host.cpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/arm/misterplexd/plex_resolve.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(PLEXUI_ASAN_CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -o $@ \
		$(ROOT)/tests/unit/test_ipv6_fallback_host.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp
$(ROOT)/build/test_pms_reach_asan: $(ROOT)/tests/unit/test_pms_reach.cpp $(ROOT)/arm/misterplexd/pms_reach.hpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/arm/misterplexd/plex_resolve.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(PLEXUI_ASAN_CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -o $@ \
		$(ROOT)/tests/unit/test_pms_reach.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp
# #339 review N3: MUT_336_KEY_NO_ID and MUT_336_NO_CACHE must fail test_pms_reach.
$(ROOT)/build/test_pms_reach.mutant-check: $(ROOT)/tests/unit/test_pms_reach.cpp $(ROOT)/arm/misterplexd/pms_reach.hpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/arm/misterplexd/plex_resolve.hpp
	@mkdir -p $(ROOT)/build
	@set -e; for m in MUT_336_KEY_NO_ID MUT_336_NO_CACHE MUT_344D_NO_ID_REQUIRED MUT_344D_SKIP_PROBE MUT_347_NO_HTTPS_MAP MUT_347_NO_PLEXDIRECT_MAP MUT_347C_PROBE_ANY_TARGET MUT_347C_TIMELINE_GATEWAY MUT_347D_PROBE_DURING_CAST MUT_351_KEEP_ULA MUT_351_PROVISIONAL_FALLBACK MUT_351_WINDOW_REFILL MUT_351_TOKEN_GATE_OFF MUT_351_TOKEN_GATE_NO_ID MUT_351_SEEK_KEEPS_ADDRESS MUT_351_SEEK_KEEPS_TOKEN; do \
		$(CXX) $(CXXFLAGS) -pthread -I$(ROOT)/arm/misterplexd -D$$m -o $@.$$m \
			$(ROOT)/tests/unit/test_pms_reach.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp; \
		if $@.$$m > $@.$$m.log 2>&1; then echo "MUTANT SURVIVED: $$m"; exit 1; fi; done
	@echo "test_pms_reach: MUT_336_KEY_NO_ID MUT_336_NO_CACHE MUT_344D_NO_ID_REQUIRED MUT_344D_SKIP_PROBE MUT_347_NO_HTTPS_MAP MUT_347_NO_PLEXDIRECT_MAP MUT_347C_PROBE_ANY_TARGET MUT_347C_TIMELINE_GATEWAY MUT_347D_PROBE_DURING_CAST MUT_351_KEEP_ULA MUT_351_PROVISIONAL_FALLBACK MUT_351_WINDOW_REFILL MUT_351_TOKEN_GATE_OFF MUT_351_TOKEN_GATE_NO_ID FAIL as required" | tee $@
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_pms_reach.mutant-check
$(ROOT)/build/test_net_infra_asan: $(ROOT)/tests/unit/test_net_infra.cpp $(ROOT)/arm/misterplexd/net_infra.hpp \
		$(ROOT)/arm/misterplexd/pms_reach.hpp $(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/arm/misterplexd/plex_resolve.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(PLEXUI_ASAN_CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -o $@ \
		$(ROOT)/tests/unit/test_net_infra.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp
$(ROOT)/build/test_plextv_local_asan: $(ROOT)/tests/unit/test_plextv_local.cpp $(ROOT)/arm/misterplexd/pms_reach.hpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/arm/misterplexd/plex_resolve.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(PLEXUI_ASAN_CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -o $@ \
		$(ROOT)/tests/unit/test_plextv_local.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp
$(ROOT)/build/test_pms_timeline_ipv6_asan: $(ROOT)/tests/unit/test_pms_timeline_ipv6.cpp \
		$(ROOT)/arm/misterplexd/pms_timeline.cpp $(ROOT)/arm/misterplexd/pms_timeline.hpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/arm/misterplexd/plex_resolve.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(PLEXUI_ASAN_CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -o $@ \
		$(ROOT)/tests/unit/test_pms_timeline_ipv6.cpp $(ROOT)/arm/misterplexd/pms_timeline.cpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp -lm
.PHONY: ipv6-asan
ipv6-asan: $(IPV6_ASAN_BINS)
	@set -e; $(foreach b,$(IPV6_ASAN_BINS),echo "== $(notdir $(b))"; ASAN_OPTIONS=detect_leaks=1 $(b);)
	python3 $(ROOT)/tests/unit/test_ipv6_source_pins.py
	python3 $(ROOT)/tests/unit/test_log_token_pins.py
	@if MPX_LOG_PIN_MUTANT=1 python3 $(ROOT)/tests/unit/test_log_token_pins.py >/dev/null; then echo "MUTANT SURVIVED: MPX_LOG_PIN_MUTANT"; exit 1; fi

# Playbar no-proxy 10-23: ASan+UBSan run of the unit tests that pin the four no-proxy commits (a-d).
# Separate target and binary names from the older playbar branch's `playbar-asan` (e34f226) so both can coexist.
NOPROXY_ASAN_BINS := $(ROOT)/build/test_pms_timeline_np_asan $(ROOT)/build/test_companion_poll_hold_np_asan \
		$(ROOT)/build/test_companion_live_clock_np_asan $(ROOT)/build/test_companion_timeline_fields_np_asan \
		$(ROOT)/build/test_companion_cors_expose_np_asan
$(ROOT)/build/test_companion_poll_hold_np_asan: $(ROOT)/tests/unit/test_companion_poll_hold.cpp \
		$(ROOT)/arm/misterplexd/companion_poll_hold.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(PLEXUI_ASAN_CXXFLAGS) -I$(ROOT)/arm/misterplexd -o $@ $(ROOT)/tests/unit/test_companion_poll_hold.cpp
# companion.cpp itself (poll handler, timelineXml) under ASan; links plexui_http.cpp like e34f226 did.
$(ROOT)/build/test_companion_live_clock_np_asan: $(ROOT)/tests/unit/test_companion_live_clock.cpp \
		$(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/arm/misterplexd/companion.hpp \
		$(ROOT)/arm/misterplexd/companion_poll_hold.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(PLEXUI_ASAN_CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -o $@ \
		$(ROOT)/tests/unit/test_companion_live_clock.cpp $(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/host/libmisterplex/plexui/plexui_http.cpp
$(ROOT)/build/test_pms_timeline_np_asan: $(ROOT)/tests/unit/test_pms_timeline.cpp $(ROOT)/arm/misterplexd/pms_timeline.cpp \
		$(ROOT)/arm/misterplexd/pms_timeline.hpp $(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/arm/misterplexd/plex_resolve.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(PLEXUI_ASAN_CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -o $@ \
		$(ROOT)/tests/unit/test_pms_timeline.cpp $(ROOT)/arm/misterplexd/pms_timeline.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp
$(ROOT)/build/test_companion_timeline_fields_np_asan: $(ROOT)/tests/unit/test_companion_timeline_fields.cpp \
		$(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/arm/misterplexd/companion.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(PLEXUI_ASAN_CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -o $@ \
		$(ROOT)/tests/unit/test_companion_timeline_fields.cpp $(ROOT)/arm/misterplexd/companion.cpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/host/libmisterplex/plexui/plexui_http.cpp
$(ROOT)/build/test_companion_cors_expose_np_asan: $(ROOT)/tests/unit/test_companion_cors_expose.cpp \
		$(ROOT)/arm/misterplexd/companion_cors.hpp $(ROOT)/arm/misterplexd/companion.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(PLEXUI_ASAN_CXXFLAGS) -I$(ROOT)/arm/misterplexd -DMPX_SOURCE_ROOT='"$(ROOT)"' -o $@ \
		$(ROOT)/tests/unit/test_companion_cors_expose.cpp
.PHONY: noproxy-asan
noproxy-asan: $(NOPROXY_ASAN_BINS)
	@set -e; $(foreach b,$(NOPROXY_ASAN_BINS),echo "== $(notdir $(b))"; ASAN_OPTIONS=detect_leaks=1 $(b);)
	python3 $(ROOT)/tests/unit/test_noproxy_source_pins.py

# unit-unlocked runs ipv6-asan and noproxy-asan (binaries and the two source-pin
# scripts). Lists are expanded per binary so the target audit can see each run.
# UNIT_RUN_ASAN_BINS below stays #285's per-binary .check wiring.
unit-unlocked unit-host-unlocked: ipv6-asan noproxy-asan
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_companion_timeline_fields_np_asan.ok
$(ROOT)/build/test_companion_timeline_fields_np_asan.ok: $(ROOT)/build/test_companion_timeline_fields_np_asan \
		$(ROOT)/tests/unit/test_companion_timeline_fields.cpp $(ROOT)/arm/misterplexd/companion.cpp
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread \
		-o $(ROOT)/build/test_companion_timeline_fields \
		$(ROOT)/tests/unit/test_companion_timeline_fields.cpp $(ROOT)/arm/misterplexd/companion.cpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/host/libmisterplex/plexui/plexui_http.cpp
	$(ROOT)/build/test_companion_timeline_fields
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 $(ROOT)/build/test_companion_timeline_fields_np_asan
	# Cast-owner token gate. Host -Wall binary, ASan binary, and the two mutants.
	# MUT_TIMELINE_TOKEN_ANY_CLIENT restores the old emit-to-everyone behavior and must fail.
	# MUT_TIMELINE_OMIT_CAST_TOKEN drops the attribute for the owner and must fail.
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -DMUT_TIMELINE_TOKEN_ANY_CLIENT \
		-o $(ROOT)/build/test_companion_timeline_fields.MUT_TIMELINE_TOKEN_ANY_CLIENT \
		$(ROOT)/tests/unit/test_companion_timeline_fields.cpp $(ROOT)/arm/misterplexd/companion.cpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/host/libmisterplex/plexui/plexui_http.cpp
	@if $(ROOT)/build/test_companion_timeline_fields.MUT_TIMELINE_TOKEN_ANY_CLIENT >/dev/null 2>&1; then \
		echo "MUTANT SURVIVED: MUT_TIMELINE_TOKEN_ANY_CLIENT"; exit 1; fi
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -DMUT_TIMELINE_TOKEN_IGNORE_PEER \
		-o $(ROOT)/build/test_companion_timeline_fields.MUT_TIMELINE_TOKEN_IGNORE_PEER \
		$(ROOT)/tests/unit/test_companion_timeline_fields.cpp $(ROOT)/arm/misterplexd/companion.cpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/host/libmisterplex/plexui/plexui_http.cpp
	@if $(ROOT)/build/test_companion_timeline_fields.MUT_TIMELINE_TOKEN_IGNORE_PEER >/dev/null 2>&1; then \
		echo "MUTANT SURVIVED: MUT_TIMELINE_TOKEN_IGNORE_PEER"; exit 1; fi
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -DMUT_TIMELINE_TOKEN_IGNORE_CALLBACK_HOST \
		-o $(ROOT)/build/test_companion_timeline_fields.MUT_TIMELINE_TOKEN_IGNORE_CALLBACK_HOST \
		$(ROOT)/tests/unit/test_companion_timeline_fields.cpp $(ROOT)/arm/misterplexd/companion.cpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/host/libmisterplex/plexui/plexui_http.cpp
	@if $(ROOT)/build/test_companion_timeline_fields.MUT_TIMELINE_TOKEN_IGNORE_CALLBACK_HOST >/dev/null 2>&1; then \
		echo "MUTANT SURVIVED: MUT_TIMELINE_TOKEN_IGNORE_CALLBACK_HOST"; exit 1; fi
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -DMUT_TIMELINE_KEEP_OLD_TOKEN \
		-o $(ROOT)/build/test_companion_timeline_fields.MUT_TIMELINE_KEEP_OLD_TOKEN \
		$(ROOT)/tests/unit/test_companion_timeline_fields.cpp $(ROOT)/arm/misterplexd/companion.cpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/host/libmisterplex/plexui/plexui_http.cpp
	@if $(ROOT)/build/test_companion_timeline_fields.MUT_TIMELINE_KEEP_OLD_TOKEN >/dev/null 2>&1; then \
		echo "MUTANT SURVIVED: MUT_TIMELINE_KEEP_OLD_TOKEN"; exit 1; fi
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -DMUT_TIMELINE_PUSH_HOST_ONLY \
		-o $(ROOT)/build/test_companion_timeline_fields.MUT_TIMELINE_PUSH_HOST_ONLY \
		$(ROOT)/tests/unit/test_companion_timeline_fields.cpp $(ROOT)/arm/misterplexd/companion.cpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/host/libmisterplex/plexui/plexui_http.cpp
	@if $(ROOT)/build/test_companion_timeline_fields.MUT_TIMELINE_PUSH_HOST_ONLY >/dev/null 2>&1; then \
		echo "MUTANT SURVIVED: MUT_TIMELINE_PUSH_HOST_ONLY"; exit 1; fi
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -DMUT_TIMELINE_SUBSCRIBE_ADDRESS \
		-o $(ROOT)/build/test_companion_timeline_fields.MUT_TIMELINE_SUBSCRIBE_ADDRESS \
		$(ROOT)/tests/unit/test_companion_timeline_fields.cpp $(ROOT)/arm/misterplexd/companion.cpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/host/libmisterplex/plexui/plexui_http.cpp
	@if $(ROOT)/build/test_companion_timeline_fields.MUT_TIMELINE_SUBSCRIBE_ADDRESS >/dev/null 2>&1; then \
		echo "MUTANT SURVIVED: MUT_TIMELINE_SUBSCRIBE_ADDRESS"; exit 1; fi
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -DMUT_TIMELINE_REUSE_ON_ID \
		-o $(ROOT)/build/test_companion_timeline_fields.MUT_TIMELINE_REUSE_ON_ID \
		$(ROOT)/tests/unit/test_companion_timeline_fields.cpp $(ROOT)/arm/misterplexd/companion.cpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/host/libmisterplex/plexui/plexui_http.cpp
	@if $(ROOT)/build/test_companion_timeline_fields.MUT_TIMELINE_REUSE_ON_ID >/dev/null 2>&1; then \
		echo "MUTANT SURVIVED: MUT_TIMELINE_REUSE_ON_ID"; exit 1; fi
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -DMUT_TIMELINE_PUSH_LOOP_OWNER_ID \
		-o $(ROOT)/build/test_companion_timeline_fields.MUT_TIMELINE_PUSH_LOOP_OWNER_ID \
		$(ROOT)/tests/unit/test_companion_timeline_fields.cpp $(ROOT)/arm/misterplexd/companion.cpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/host/libmisterplex/plexui/plexui_http.cpp
	@if $(ROOT)/build/test_companion_timeline_fields.MUT_TIMELINE_PUSH_LOOP_OWNER_ID >/dev/null 2>&1; then \
		echo "MUTANT SURVIVED: MUT_TIMELINE_PUSH_LOOP_OWNER_ID"; exit 1; fi
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -DMUT_MIRROR_SETCASTOWNER \
		-o $(ROOT)/build/test_companion_timeline_fields.MUT_MIRROR_SETCASTOWNER \
		$(ROOT)/tests/unit/test_companion_timeline_fields.cpp $(ROOT)/arm/misterplexd/companion.cpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/host/libmisterplex/plexui/plexui_http.cpp
	@if $(ROOT)/build/test_companion_timeline_fields.MUT_MIRROR_SETCASTOWNER >/dev/null 2>&1; then \
		echo "MUTANT SURVIVED: MUT_MIRROR_SETCASTOWNER"; exit 1; fi
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -DMUT_PLAYMEDIA_SETCASTOWNER \
		-o $(ROOT)/build/test_companion_timeline_fields.MUT_PLAYMEDIA_SETCASTOWNER \
		$(ROOT)/tests/unit/test_companion_timeline_fields.cpp $(ROOT)/arm/misterplexd/companion.cpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/host/libmisterplex/plexui/plexui_http.cpp
	@if $(ROOT)/build/test_companion_timeline_fields.MUT_PLAYMEDIA_SETCASTOWNER >/dev/null 2>&1; then \
		echo "MUTANT SURVIVED: MUT_PLAYMEDIA_SETCASTOWNER"; exit 1; fi
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -DMUT_TIMELINE_SUBSCRIBE_PEER_IS_CALLBACK \
		-o $(ROOT)/build/test_companion_timeline_fields.MUT_TIMELINE_SUBSCRIBE_PEER_IS_CALLBACK \
		$(ROOT)/tests/unit/test_companion_timeline_fields.cpp $(ROOT)/arm/misterplexd/companion.cpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/host/libmisterplex/plexui/plexui_http.cpp
	@if $(ROOT)/build/test_companion_timeline_fields.MUT_TIMELINE_SUBSCRIBE_PEER_IS_CALLBACK >/dev/null 2>&1; then \
		echo "MUTANT SURVIVED: MUT_TIMELINE_SUBSCRIBE_PEER_IS_CALLBACK"; exit 1; fi
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -DMUT_TIMELINE_UNSUB_ID_ONLY \
		-o $(ROOT)/build/test_companion_timeline_fields.MUT_TIMELINE_UNSUB_ID_ONLY \
		$(ROOT)/tests/unit/test_companion_timeline_fields.cpp $(ROOT)/arm/misterplexd/companion.cpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/host/libmisterplex/plexui/plexui_http.cpp
	@if $(ROOT)/build/test_companion_timeline_fields.MUT_TIMELINE_UNSUB_ID_ONLY >/dev/null 2>&1; then \
		echo "MUTANT SURVIVED: MUT_TIMELINE_UNSUB_ID_ONLY"; exit 1; fi
	$(CXX) $(PLEXUI_ASAN_CXXFLAGS) -I$(ROOT)/arm/misterplexd -I$(ROOT)/host -pthread -DMUT_TIMELINE_OMIT_CAST_TOKEN \
		-o $(ROOT)/build/test_companion_timeline_fields_np_asan.MUT_TIMELINE_OMIT_CAST_TOKEN \
		$(ROOT)/tests/unit/test_companion_timeline_fields.cpp $(ROOT)/arm/misterplexd/companion.cpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/host/libmisterplex/plexui/plexui_http.cpp
	@if ASAN_OPTIONS=detect_leaks=1 $(ROOT)/build/test_companion_timeline_fields_np_asan.MUT_TIMELINE_OMIT_CAST_TOKEN >/dev/null 2>&1; then \
		echo "MUTANT SURVIVED: MUT_TIMELINE_OMIT_CAST_TOKEN"; exit 1; fi
	@echo "test_companion_timeline_fields: owner gate OK; all mutants FAIL as required" | tee $@

# PMS /:/timeline heartbeat tolerates tick jitter (playbar LAN-origin investigation, 10-24).
$(ROOT)/build/test_pms_timeline_cadence: $(ROOT)/tests/unit/test_pms_timeline_cadence.cpp \
		$(ROOT)/arm/misterplexd/pms_timeline.cpp $(ROOT)/arm/misterplexd/pms_timeline.hpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT)/arm/misterplexd -pthread -o $@ \
		$(ROOT)/tests/unit/test_pms_timeline_cadence.cpp \
		$(ROOT)/arm/misterplexd/pms_timeline.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp

$(ROOT)/build/test_pms_timeline_cadence_asan: $(ROOT)/tests/unit/test_pms_timeline_cadence.cpp \
		$(ROOT)/arm/misterplexd/pms_timeline.cpp $(ROOT)/arm/misterplexd/pms_timeline.hpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -g -I$(ROOT)/arm/misterplexd -pthread -o $@ \
		$(ROOT)/tests/unit/test_pms_timeline_cadence.cpp \
		$(ROOT)/arm/misterplexd/pms_timeline.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp

# fr-heartbeat-1025: transcode ping/stop + paused/buffering keepalive + supersede stop (fake-PMS sink, mutants).
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_pms_heartbeat.ok
$(ROOT)/build/test_pms_heartbeat.ok: $(ROOT)/tests/unit/test_pms_heartbeat.cpp $(ROOT)/tests/unit/hpp_mutant_runner.sh \
		$(ROOT)/arm/misterplexd/pms_timeline.cpp $(ROOT)/arm/misterplexd/pms_timeline.hpp \
		$(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/arm/misterplexd/plex_resolve.hpp
	@mkdir -p $(ROOT)/build
	LDLIBS="$(ROOT)/arm/misterplexd/pms_timeline.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp -pthread" SAN=1 ROOT=$(ROOT) \
		bash $(ROOT)/tests/unit/hpp_mutant_runner.sh tests/unit/test_pms_heartbeat.cpp MUT_HB_NO_PING MUT_HB_NO_STOP \
		MUT_HB_STOP_AFTER_TIMELINE MUT_HB_NO_SUPERSEDE MUT_HB_NO_STATE_RESEND MUT_HB_NO_FLUSH_STOP \
		MUT_HB_PLAYER_IDENTITY MUT_HB_LOCAL_BASE MUT_HB_NO_RATELIMIT \
		MUT_F2_STOP_404_FAILED MUT_F1_AUTH_EVERY_TIME MUT_F3_REPORTER_ZERO MUT_F4_LOCAL_SKIPS
	touch $@

# fr-resume-1025: remote start probe budget, abandoned-start stop URL, heartbeat session gate (pure logic, mutants).
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_remote_start_probe.ok
$(ROOT)/build/test_remote_start_probe.ok: $(ROOT)/tests/unit/test_remote_start_probe.cpp $(ROOT)/tests/unit/hpp_mutant_runner.sh \
		$(ROOT)/arm/misterplexd/remote_start_probe.hpp
	@mkdir -p $(ROOT)/build
	SAN=1 ROOT=$(ROOT) bash $(ROOT)/tests/unit/hpp_mutant_runner.sh tests/unit/test_remote_start_probe.cpp \
		MUT_RSP_NO_BUDGET MUT_RSP_RETRY_HARD MUT_RSP_STOP_TOKEN_URL MUT_RSP_PING_DIRECT MUT_PROBE_FIRST_BLOCK MUT_PROBE_NO_CLAMP \
		MUT_RSP_NO_PAUSE MUT_RS9_NEG_CAP
	touch $@

# rs9 (backlog #9): remote start probe latency budget against a fake streaming PMS (headers + 64 KiB, not 24 s).
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_rs9_start_latency.ok
$(ROOT)/build/test_rs9_start_latency.ok: $(ROOT)/tests/unit/test_rs9_start_latency.cpp $(ROOT)/tests/unit/hpp_mutant_runner.sh \
		$(ROOT)/arm/misterplexd/remote_start_probe.hpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/arm/misterplexd/pms_timeline.cpp
	@mkdir -p $(ROOT)/build
	LDLIBS="$(ROOT)/arm/misterplexd/pms_timeline.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp -pthread" SAN=1 ROOT=$(ROOT) \
		bash $(ROOT)/tests/unit/hpp_mutant_runner.sh tests/unit/test_rs9_start_latency.cpp \
		MUT_RS9_CAP_DEFAULT_OFF MUT_RS9_IGNORE_CAP MUT_RS9_CAP0_CAPPED MUT_RSP_NO_PAUSE
	touch $@

# fr-playbar-1025: Companion playbar feed (keepalive pushes while paused/buffering, commandID echo, reply content).
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_companion_playbar.ok
$(ROOT)/build/test_companion_playbar.ok: $(ROOT)/tests/unit/test_companion_playbar.cpp $(ROOT)/tests/unit/hpp_mutant_runner.sh \
		$(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/companion.hpp \
		$(ROOT)/arm/misterplexd/pms_timeline.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/host/libmisterplex/plexui/plexui_http.cpp
	@mkdir -p $(ROOT)/build
	LDLIBS="$(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/pms_timeline.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/host/libmisterplex/plexui/plexui_http.cpp -pthread" SAN=1 ROOT=$(ROOT) \
		bash $(ROOT)/tests/unit/hpp_mutant_runner.sh tests/unit/test_companion_playbar.cpp MUT_PB_NO_KEEPALIVE \
		MUT_PB_KEEPALIVE_ALWAYS MUT_PB_CMD_ZERO MUT_PB_NO_SEEKRANGE MUT_PB_NO_CONTAINERKEY
	touch $@

# fr-glass-1032-1025 F3: the post-Stop buffering hold reports the last time/duration, not 0/0.
unit-unlocked unit-host-unlocked: $(ROOT)/build/test_companion_stop_hold.ok
$(ROOT)/build/test_companion_stop_hold.ok: $(ROOT)/tests/unit/test_companion_stop_hold.cpp $(ROOT)/tests/unit/hpp_mutant_runner.sh \
		$(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/companion.hpp \
		$(ROOT)/arm/misterplexd/pms_timeline.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp \
		$(ROOT)/host/libmisterplex/plexui/plexui_http.cpp
	@mkdir -p $(ROOT)/build
	LDLIBS="$(ROOT)/arm/misterplexd/companion.cpp $(ROOT)/arm/misterplexd/pms_timeline.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/host/libmisterplex/plexui/plexui_http.cpp -pthread" SAN=1 ROOT=$(ROOT) \
		bash $(ROOT)/tests/unit/hpp_mutant_runner.sh tests/unit/test_companion_stop_hold.cpp MUT_F3_HOLD_ZERO MUT_IDLEKEY_ALWAYS_BLANK MUT_IDLEKEY_NO_EXPIRY
	touch $@

DTASK_DEPS := $(ROOT)/tests/unit/test_delayed_task.cpp $(ROOT)/host/libmisterplex/delayed_task.hpp $(ROOT)/arm/misterplexd/main.cpp $(ROOT)/arm/misterplexd/media_player.cpp $(ROOT)/host/libmisterplex/owenb_osd_renderer.hpp
$(ROOT)/build/test_delayed_task: $(DTASK_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -DMPX_SOURCE_ROOT='"$(ROOT)"' -o $@ $(ROOT)/tests/unit/test_delayed_task.cpp
$(ROOT)/build/test_delayed_task.ok: $(ROOT)/build/test_delayed_task
	$(ROOT)/build/test_delayed_task
	@touch $@
$(ROOT)/build/test_delayed_task_asan: $(DTASK_DEPS)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -pthread -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -DMPX_SOURCE_ROOT='"$(ROOT)"' -o $@ $(ROOT)/tests/unit/test_delayed_task.cpp
$(ROOT)/build/test_delayed_task_asan.ok: $(ROOT)/build/test_delayed_task_asan
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 $(ROOT)/build/test_delayed_task_asan
	@touch $@
$(ROOT)/build/test_delayed_task.mutant-check: $(ROOT)/build/test_delayed_task
	@$(ROOT)/build/test_delayed_task > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }
	$(CXX) $(CXXFLAGS) -pthread -DMPX_SOURCE_ROOT='"$(ROOT)"' -DMUT_DTASK_DETACH -o $(ROOT)/build/test_delayed_task.MUT_DTASK_DETACH $(ROOT)/tests/unit/test_delayed_task.cpp
	@if $(ROOT)/build/test_delayed_task.MUT_DTASK_DETACH > $@.log 2>&1; then echo "MUTANT SURVIVED: MUT_DTASK_DETACH"; exit 1; fi
	@grep -q "FAIL: D1 " $@.log || { echo "MUTANT MUT_DTASK_DETACH KILLED BY THE WRONG CHECK (want D1)"; cat $@.log; exit 1; }
	@echo "test_delayed_task: 1 mutant FAIL as required" | tee $@
unit-unlocked unit-host-unlocked plexui-tests: $(ROOT)/build/test_delayed_task.ok $(ROOT)/build/test_delayed_task_asan.ok $(ROOT)/build/test_delayed_task.mutant-check
# tipA LAN_AUTH (#62/#63, supersedes #140): flag default off; ASan/UBSan + mutants.
unit-unlocked unit-host-unlocked plexui-tests: $(ROOT)/build/test_lan_auth.ok
$(ROOT)/build/test_lan_auth.ok: $(ROOT)/tests/unit/test_lan_auth.cpp $(ROOT)/tests/unit/hpp_mutant_runner.sh \
		$(ROOT)/host/libmisterplex/lan_auth.hpp $(ROOT)/tools/airplay/airplay_xcode.py \
		$(ROOT)/tools/airplay/test_lan_auth_xcode.py $(ROOT)/tools/airplay/lan_auth_sender.py \
		$(ROOT)/tools/airplay/test_lan_auth_sender.py
	@mkdir -p $(ROOT)/build
	SAN=1 ROOT=$(ROOT) bash $(ROOT)/tests/unit/hpp_mutant_runner.sh tests/unit/test_lan_auth.cpp \
		MUT_LAN_AUTH_DEFAULT_ON MUT_LAN_AUTH_OFF_STILL_BINDS MUT_LAN_AUTH_ON_IGNORED MUT_LAN_AUTH_WRONG_CONTEXT \
		MUT_LAN_AUTH_FAT_RELAX_ALWAYS MUT_LAN_AUTH_FAT_ALLOW_WW MUT_LAN_AUTH_OWNER_ANY
	python3 $(ROOT)/tools/airplay/test_lan_auth_xcode.py
	python3 $(ROOT)/tools/airplay/test_lan_auth_sender.py
	@for m in defaulton ignored noloopexempt; do \
		if LAN_AUTH_MUT=$$m python3 $(ROOT)/tools/airplay/test_lan_auth_xcode.py >/dev/null 2>&1; then \
			echo "MUTANT SURVIVED: LAN_AUTH_MUT=$$m"; exit 1; fi; done
	@echo "test_lan_auth_xcode: 3 mutants FAIL as required"
	touch $@

# #272: no X-Plex-Token on curl argv (browse_fetch URL, plexui_http URL, FU8 runArgv scrub).
# Mutants MUT_272_URL_TOKEN_KEPT / MUT_FU8_TOKEN_ARGV must FAIL.
TOK272_FLAGS := -std=c++17 -Wall -Wextra -DMPX_PLEXUI_UNIT_TEST -I$(ROOT)/host -I$(ROOT)/host/libmisterplex -I$(ROOT)/host/libmisterplex/plexui -I$(ROOT)/arm/misterplexd -I$(ROOT)/third_party/owenb-h264ring/include -pthread -DMPX_SRC_ROOT=\"$(ROOT)\"
TOK272_MUTANTS := MUT_272_URL_TOKEN_KEPT MUT_FU8_TOKEN_ARGV MUT_272_REDIRECT_ANY_HOST MUT_272_NO_REDIRECT MUT_FU10_BROWSE_FD_LEAK
$(ROOT)/build/test_token_argv_272.ok: $(ROOT)/tests/unit/test_token_argv_272.cpp $(ROOT)/host/libmisterplex/curl_argv_token.hpp $(ROOT)/host/libmisterplex/browse_fetch.hpp $(PLEXUI_SRC)
	@mkdir -p $(ROOT)/build
	$(CXX) $(TOK272_FLAGS) -O2 -o $(ROOT)/build/test_token_argv_272 $< $(PLEXUI_SRC)
	$(CXX) $(TOK272_FLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -o $(ROOT)/build/test_token_argv_272_asan $< $(PLEXUI_SRC)
	$(ROOT)/build/test_token_argv_272
	ASAN_OPTIONS=detect_leaks=1 $(ROOT)/build/test_token_argv_272_asan
	@for m in $(TOK272_MUTANTS); do \
		$(CXX) $(TOK272_FLAGS) -D$$m -o $(ROOT)/build/test_token_argv_272.$$m $< $(PLEXUI_SRC) || { echo "MUTANT DID NOT COMPILE: $$m"; exit 1; }; \
		if $(ROOT)/build/test_token_argv_272.$$m > /dev/null 2>&1; then echo "MUTANT SURVIVED: $$m"; exit 1; fi; \
	done
	@echo "test_token_argv_272: $(words $(TOK272_MUTANTS)) mutants FAIL as required" | tee $@

# FU8/#272 e2e: plex_resolve curl helpers against a fake curl (argv + stdin recorded).
$(ROOT)/build/test_fu8_e2e_argv.ok: $(ROOT)/tests/unit/test_fu8_e2e_argv.cpp $(ROOT)/arm/misterplexd/plex_resolve.cpp $(ROOT)/host/libmisterplex/curl_argv_token.hpp
	@mkdir -p $(ROOT)/build
	$(CXX) $(PLEXHTTPGET_INJ_CXXFLAGS) -o $(ROOT)/build/test_fu8_e2e_argv $< $(ROOT)/arm/misterplexd/plex_resolve.cpp
	$(CXX) $(PLEXHTTPGET_INJ_CXXFLAGS) -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -o $(ROOT)/build/test_fu8_e2e_argv_asan $< $(ROOT)/arm/misterplexd/plex_resolve.cpp
	$(ROOT)/build/test_fu8_e2e_argv
	ASAN_OPTIONS=detect_leaks=1 $(ROOT)/build/test_fu8_e2e_argv_asan
	$(CXX) $(PLEXHTTPGET_INJ_CXXFLAGS) -DMUT_FU8_TOKEN_ARGV -o $(ROOT)/build/test_fu8_e2e_argv.mut $< $(ROOT)/arm/misterplexd/plex_resolve.cpp
	@if $(ROOT)/build/test_fu8_e2e_argv.mut > /dev/null 2>&1; then echo "MUTANT SURVIVED: MUT_FU8_TOKEN_ARGV"; exit 1; fi
	@echo "test_fu8_e2e_argv: OK, MUT_FU8_TOKEN_ARGV FAILs as required" | tee $@

# #285 (1101): ASan/plain binaries that only ran from phony targets unit-unlocked never reaches (buildid-asan,
# playbar-asan, ipv6-asan, noproxy-asan) or had no run rule (pms_timeline_cadence). Each runs on its own here; the
# phony targets keep their extra script pins. test_companion_timeline_fields_np_asan runs
# (the cast token is delivered only to the cast owner) and is not a unit_build_only.txt row.
UNIT_RUN_ASAN_BINS := $(BUILDID_ASAN_BINS) $(PLAYBAR_ASAN_BINS) $(IPV6_ASAN_BINS) \
		$(NOPROXY_ASAN_BINS) \
		$(ROOT)/build/test_pms_timeline_cadence $(ROOT)/build/test_pms_timeline_cadence_asan
$(addsuffix .check,$(UNIT_RUN_ASAN_BINS)): %.check: %
	ASAN_OPTIONS=detect_leaks=1 $<
	@touch $@
unit-unlocked unit-host-unlocked: $(addsuffix .check,$(UNIT_RUN_ASAN_BINS))
