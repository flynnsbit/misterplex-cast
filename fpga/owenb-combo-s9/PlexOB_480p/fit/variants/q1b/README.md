# Q1b: FPGA Bug hold fix 2 (targeted hold-only exceptions). Constraint-only, parity Y. Excludes fifo_dc, pic_*/fit_*, f0_tag, vid_pll->vid_pll (see sdc header).
# Q1 (all 41 pairs) is kept as data only: it also covers fifo_dc and the raster_dn_xing pic_*/fit_* captures, which FPGA Bug says need a sim proof first. Not recommended for shipping.
