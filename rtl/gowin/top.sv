// Tang Nano 20K トップ (ハーネス v1: RP2040-Zero、docs/HARNESS.md、harness/pins.json、constraints/tangnano20k.cst)
//   中身は top_core.sv。HSTX は使わない (v1 には線がない)
module top #(
    parameter int GPU3D = 0,
    parameter int MUL_PROBE = 0          // 3D GPU で 0x50 に mul_probe (DSP の確かめ、G0) を入れる (make bitstream-gpu3d MUL_PROBE=1)
) (
    input  logic       clk,          // 27MHz
    input  logic       btn_s1,       // リセット (押下で 1)
    input  logic       btn_s2,
    output logic [5:0] led,          // 負論理
    output logic       tmds_clk_p,
    output logic       tmds_clk_n,
    output logic [2:0] tmds_d_p,
    output logic [2:0] tmds_d_n,
    // ハーネス (RP2 -> FPGA は入力専用、正論理)
    input  logic       spi_sck,
    input  logic       spi_copi,
    input  logic       spi_cs,
    input  logic       rst_in,
    output logic       spi_cipo,
    output logic       vdp_irq,
    output logic       vsync_out,
    output logic       heartbeat,
    output logic       error_out,
    output logic       reconfig_n,   // RECONFIG_N (ピン 9)。Low で再コンフィグ (--reconfign_as_gpio は付けない)
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
    // 基板の音のアンプ (MAX98357A、I2S)。K5a
    output logic        hp_bck,
    output logic        hp_ws,
    output logic        hp_din,
    output logic        pa_en
);
    top_core #(.GPU3D(GPU3D), .MUL_PROBE(MUL_PROBE), .HSTX(0)) u_top (
        .clk, .btn_s1, .btn_s2, .led, .tmds_clk_p, .tmds_clk_n, .tmds_d_p, .tmds_d_n, .spi_sck, .spi_copi, .spi_cs, .rst_in,
        .spi_cipo, .vdp_irq, .vsync_out, .heartbeat, .error_out, .reconfig_n, .O_sdram_clk, .O_sdram_cke, .O_sdram_cs_n,
        .O_sdram_ras_n, .O_sdram_cas_n, .O_sdram_wen_n, .O_sdram_ba, .O_sdram_addr, .O_sdram_dqm, .IO_sdram_dq, .hp_bck, .hp_ws,
        .hp_din, .pa_en,
        .hstx_clk(1'b0), .hstx_d(4'h0)
    );
endmodule
