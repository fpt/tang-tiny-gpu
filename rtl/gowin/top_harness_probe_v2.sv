// ハーネス v2 の配線テスト用トップ (docs/HARNESS2.md, harness/firmware の wiretest と対で使う)
//   FPGA -> RP2 は 3 本 (CIPO, IRQ, VSYNC) しかないので、RP2 -> FPGA の 9 本は番号にして返す:
//   通常: CIPO / IRQ / VSYNC をそれぞれ違う周波数 (27MHz / 2^(N+1)) でトグルさせる
//   エコー: RP2 -> FPGA の線のどれかが「立っている」間は、その線の番号 (1〜9、2 本以上なら 15) を
//     VSYNC が Low の間は {IRQ, CIPO} = 番号の下 2 ビット、High の間は上 2 ビットで出す (VSYNC はトグルのまま)
//   番号: 1 SCK, 2 COPI, 3 CS, 4 RESET, 5〜8 HSTX_D0〜D3, 9 HSTX_CLK
//   SCK (20)、COPI (19) は LED5 / LED4 (3.3V につながっている) でプルアップされていて、離すと High なので、Low を「立っている」とする
//   (pins_v2.json の "idle": "high")。
//   HSTX_CLK (80、GCLKT_0) はクロックとして受け、「トグルしている」を立っているとする。本番 (HSTX の受信) と同じ使い方で、
//   データの入力として読むと apicula / nextpnr では値が届かない (常に High に見えた。実機で確かめた)
//   RP2 -> FPGA の線は FPGA は絶対に駆動しない (入力専用)
module top_harness_probe_v2 (
    input  logic       clk,          // 27MHz
    input  logic       spi_sck,
    input  logic       spi_copi,
    input  logic       spi_cs,
    input  logic       rst_in,
    input  logic [3:0] hstx_d,
    input  logic       hstx_clk,
    output logic       spi_cipo,
    output logic       vdp_irq,
    output logic       vsync_out
);
    logic [12:0] cnt = '0;
    always_ff @(posedge clk) cnt <= cnt + 13'd1;

    // HSTX_CLK: 立ち上がりごとに反転する FF を clk に同期し、変化してから 1024 クロック (38us) の間を「トグルしている」とする
    logic       hclk_t = 1'b0;
    always_ff @(posedge hstx_clk) hclk_t <= ~hclk_t;
    logic [2:0] hclk_sync;
    logic [9:0] hclk_hold = '0;
    always_ff @(posedge clk) begin
        hclk_sync <= {hclk_sync[1:0], hclk_t};
        if (hclk_sync[2] != hclk_sync[1]) hclk_hold <= '1;
        else if (hclk_hold != 10'd0)       hclk_hold <= hclk_hold - 10'd1;
    end

    // 入力を 2 段の FF で同期してから見る
    logic [8:0] in_meta, in_sync;
    always_ff @(posedge clk) begin
        in_meta <= {hclk_hold != 10'd0, hstx_d[3], hstx_d[2], hstx_d[1], hstx_d[0], rst_in, spi_cs, ~spi_copi, ~spi_sck};
        in_sync <= in_meta;
    end

    logic [3:0] code;
    logic [3:0] n_on;
    always_comb begin
        code = 4'd0;
        n_on = 4'd0;
        for (int i = 0; i < 9; i++)
            if (in_sync[i]) begin
                code = 4'(i + 1);
                n_on = n_on + 4'd1;
            end
        if (n_on > 4'd1) code = 4'd15;
    end

    logic echo, half;
    assign echo = n_on != 4'd0;
    assign half = cnt[12];

    // 周波数は harness/firmware/src/main.c の PROBE_BITS と合わせる
    always_ff @(posedge clk) begin
        spi_cipo  <= echo ? (half ? code[2] : code[0]) : cnt[10];   // 13.2kHz
        vdp_irq   <= echo ? (half ? code[3] : code[1]) : cnt[11];   // 6.6kHz
        vsync_out <= half;                                          // 3.3kHz
    end
endmodule
