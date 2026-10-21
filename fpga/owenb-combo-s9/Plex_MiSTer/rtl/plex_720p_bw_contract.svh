// plex_720p_bw_contract.svh — ONE agreed 720p24 bandwidth contract (w-mem).
// Coordinate: w-mem + w-clock + w-scaler + w-path (host T_copy_arm). Do not invent
// a second B/clk number. Rates (MB/s) and CPU TIME (ms/f) are NOT interchangeable.
// Clocks (pll_0002.v SoT): clk_sys=20e6, clk_ddr=90e6.
// Shipping glass: MULTI + PPC=2 + clk_pix@29.7e6, clk_sys@20e6.
// HOST write is NOT f2sdram — parent HW /dev/mem 1382400 B = 14.978 ms/f (T_copy_arm).
// Not in files.qip until fit opens — constants/docs only (like io_ack_follow).

localparam int P720_CODED_W           = 1280;
localparam int P720_CODED_H           = 720;
localparam int P720_I420_BYTES        = 1_382_400;
localparam int P720_Y_LINE_BYTES      = 1280;
localparam int P720_Y_LINE_QWORDS     = 160;
localparam int P720_C_LINE_QWORDS     = 80;
localparam int P720_BANK_STRIDE       = 32'h0018_0000;
localparam int P720_PHYS_BASE         = 32'h3000_0000;
localparam int P720_DOORBELL_PHYS     = 32'h3047_F000;
localparam int P720_FPS               = 24;
localparam int P720_CLK_SYS_HZ        = 20_000_000;
localparam int P720_CLK_DDR_HZ        = 90_000_000;
localparam int P720_CLK_PIX_HZ        = 29_700_000;
localparam int P720_PPC               = 2;
localparam int P720_LINE_COUNT        = 16;
localparam int P720_FRAME_US          = 41_667;
localparam int P720_DDR_CYC_PER_FRAME = 3_750_000;
localparam int P720_FABRIC_RD_BPS     = P720_I420_BYTES * P720_FPS; // 33177600
localparam int P720_HOST_WR_BPS       = P720_I420_BYTES * P720_FPS;
localparam int P720_HOST_COPY_US      = 14_978;
localparam int P720_COMBINED_AVG_BPS  = 2 * P720_FABRIC_RD_BPS;
// PPC=2 does NOT double frame DDR bytes. Refill ~240 qw/line avg @90MHz ≈2.67µs
// << glass line 55.56µs. LINE=16 → ~889µs look-ahead. Blackout 16*640/20e6=512µs.
localparam int P720_BLACKOUT_US_16    = 512;
localparam int P720_BLACKOUT_US_8     = 256;
// SDRAM: single stick — spill candidate only; sdram_line_spill is M10K stub not PHY.
