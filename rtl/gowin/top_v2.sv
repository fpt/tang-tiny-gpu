// Tang Nano 20K トップ (ハーネス v2: Pico 2、docs/HARNESS2.md、harness/kicad/v2/pins_v2.json)
//   中身は top_core.sv。ピンを v2 に合わせる: SPI は 20 / 19 / 75 / 86、IRQ は 85、VSYNC は 52、RESET は 79、HSTX は 71〜74 と 80 (クロック)。
//   v2 にない線 (HEARTBEAT、ERROR) と、SPI に使う LED4 / LED5 (19 / 20) はピンに出さない (top_core の出力を中でつながない)。
//   LED0〜3 (15〜18) は動作確認用に残す
module top_v2 #(
    parameter int GPU3D = 0,
    parameter int MUL_PROBE = 0
) (
    input  logic       clk,          // 27MHz
    input  logic       btn_s1,
    input  logic       btn_s2,
    output logic [3:0] led,          // LED0〜3 (負論理)
    output logic       tmds_clk_p,
    output logic       tmds_clk_n,
    output logic [2:0] tmds_d_p,
    output logic [2:0] tmds_d_n,
    // ハーネス v2 (RP2 -> FPGA は入力専用、正論理)
    input  logic       spi_sck,
    input  logic       spi_copi,
    input  logic       spi_cs,
    input  logic       rst_in,
    output logic       spi_cipo,
    output logic       vdp_irq,
    output logic       vsync_out,
    output logic       reconfig_n,
    input  logic       hstx_clk,     // 80 (GCLKT_0)
    input  logic [3:0] hstx_d,       // 71〜74
    // パッケージ内 SDRAM
    output logic        O_sdram_clk,
    output logic        O_sdram_cke,
    output logic        O_sdram_cs_n,
    output logic        O_sdram_ras_n,
    output logic        O_sdram_cas_n,
    output logic        O_sdram_wen_n,
    output logic [1:0]  O_sdram_ba,
    output logic [10:0] O_sdram_addr,
    output logic [3:0]  O_sdram_dqm,
    inout  wire  [31:0] IO_sdram_dq,
    // 基板の音のアンプ (MAX98357A、I2S)
    output logic        hp_bck,
    output logic        hp_ws,
    output logic        hp_din,
    output logic        pa_en
);
    logic [5:0] led_all;

    top_core #(.GPU3D(GPU3D), .MUL_PROBE(MUL_PROBE), .HSTX(1)) u_top (
        .clk, .btn_s1, .btn_s2, .led(led_all),
        .tmds_clk_p, .tmds_clk_n, .tmds_d_p, .tmds_d_n,
        .spi_sck, .spi_copi, .spi_cs, .rst_in, .spi_cipo, .vdp_irq, .vsync_out,
        .heartbeat(), .error_out(), .reconfig_n, .hstx_clk, .hstx_d,
        .O_sdram_clk, .O_sdram_cke, .O_sdram_cs_n, .O_sdram_ras_n, .O_sdram_cas_n, .O_sdram_wen_n,
        .O_sdram_ba, .O_sdram_addr, .O_sdram_dqm, .IO_sdram_dq,
        .hp_bck, .hp_ws, .hp_din, .pa_en
    );

    assign led = led_all[3:0];
endmodule
