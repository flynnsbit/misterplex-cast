# MPX-FORK owenb fit Q1b (review fit plan 2026-10-16 23:50, hold fix 2): hold-only exception on exactly the h264.sdc-bounded
# cross-PLL handshake captures (clk_vid <-> clk_mem/clk_sys). Each set_max_delay 12.000 in h264.sdc stays; this adds the matching
# set_min_delay -12.000 so hold is not timed against two-PLL skew. These are quasi-static captures taken >= 2 destination cycles
# after the qualifier crossed (h264.sdc:27-28).
# NOT covered, on purpose: fifo_dc / dpb_mem gray pointers (inside sys_pll, no hold miss); disp_reader pic_*/fit_* (sources
# raster_dn_xing|dst_data, plan 2b: prove in sim first); sync_gen mode -> f0_tag_s1 (plan 2a); any vid_pll->vid_pll path;
# sys_top.sdc framework false paths. Loaded after h264/h264.sdc.
# cdc_snap (raster_dn_xing and the other instances): the copy of the latched bus and the two toggle first stages
set_min_delay -from [get_keepers {*cdc_snap:*|hold*}] -to [get_keepers {*cdc_snap:*|dst_data*}] -12.000
set_min_delay -from [get_keepers {*cdc_snap:*|ack*}]  -to [get_keepers {*cdc_snap:*|d_q1*}]     -12.000
set_min_delay -from [get_keepers {*cdc_snap:*|req*}]  -to [get_keepers {*cdc_snap:*|s_q1*}]     -12.000
# disp_reader: command toggle, command/chroma fields (cmd_*, ccmd_* only: raster_dn_xing|dst_data sources excluded), done toggle
set_min_delay -from [get_keepers {*disp_reader:*|cmd_tog*}] -to [get_keepers {*disp_reader:*|m_tog[0]*}] -12.000
set_min_delay -from [get_keepers {*disp_reader:*|cmd_* *disp_reader:*|ccmd_*}] \
              -to [get_keepers {*disp_reader:*|m_store* *disp_reader:*|m_w[* *disp_reader:*|m_hf* *disp_reader:*|m_field* *disp_reader:*|m_valid* *disp_reader:*|m_first* *disp_reader:*|m_par* *disp_reader:*|m_dh* *disp_reader:*|m_dv* *disp_reader:*|m_hpf* *disp_reader:*|m_vpf* *disp_reader:*|m_slide* *disp_reader:*|m_n* *disp_reader:*|m_row* *disp_reader:*|m_slot* *disp_reader:*|m_cn* *disp_reader:*|m_crow* *disp_reader:*|m_cslot* *disp_reader:*|fc_* *disp_reader:*|fetch_changes*}] -12.000
set_min_delay -from [get_keepers {*disp_reader:*|done_tog*}] -to [get_keepers {*disp_reader:*|cmd_done_s1*}] -12.000
# ovl_plane: command toggle + fields (cmd_* -> m_*/o_*/n_ops/i_*), descriptor request/answer, done, d_w -> dq (protocol-held)
set_min_delay -from [get_keepers {*ovl_plane:*|cmd_tog*}] -to [get_keepers {*ovl_plane:*|m_tog[0]*}] -12.000
set_min_delay -from [get_keepers {*ovl_plane:*|cmd_*}] \
              -to [get_keepers {*ovl_plane:*|m_slot* *ovl_plane:*|m_fmt* *ovl_plane:*|m_w[* *ovl_plane:*|m_words* *ovl_plane:*|n_ops* *ovl_plane:*|o_v* *ovl_plane:*|o_a* *ovl_plane:*|o_k* *ovl_plane:*|i_left* *ovl_plane:*|i_addr*}] -12.000
set_min_delay -from [get_keepers {*ovl_plane:*|d_req*}] -to [get_keepers {*ovl_plane:*|dr_m[0]*}] -12.000
set_min_delay -from [get_keepers {*ovl_plane:*|done_tog*}] -to [get_keepers {*ovl_plane:*|done_s1*}] -12.000
set_min_delay -from [get_keepers {*ovl_plane:*|d_ack*}] -to [get_keepers {*ovl_plane:*|da_s1*}] -12.000
set_min_delay -from [get_keepers {*ovl_plane:*|d_w[*}] -to [get_keepers {*ovl_plane:*|dq[*}] -12.000
# cdc_pulse: frame-event toggle first stage
set_min_delay -from [get_keepers {*cdc_pulse:*|tog*}] -to [get_keepers {*cdc_pulse:*|t1*}] -12.000
