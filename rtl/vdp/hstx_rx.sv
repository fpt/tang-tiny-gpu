// HSTX の受信 (ハーネス v2 / 本番のホストリンク、docs/HARNESS2.md の 3 章)。ベンダ非依存
//
// 線: hclk (RP2350 の HSTX が作るクロック) と hd[3:0]。RP2350 は clk_hstx の 1 サイクルごとにニブルを 1 つ出し、
// クロック (CLKDIV = 2、CLKPHASE = 1) はニブルの真ん中で立ち上がり・立ち下がる。立ち上がりで下位、立ち下がりで上位のニブルを取り、
// 立ち下がりで 1 バイトになる。クロックは送るデータがあるときだけ動き、止まるときは Low (32 ビットのワードがクロック 4 周期)。
//
// パケット (バイトの順。ワードはリトルエンディアン):
//   マジック 55 AA 5A A5、長さ (ワード数、16 ビット)、ワード × 長さ、CRC-16 (CCITT、初期値 FFFF、ワードのバイトに対して)、詰め物 2 バイト
//   全体が 4 バイトの倍数になる (8 + 4 × 長さ)。ワードはコマンド FIFO へそのまま入れる (CRC を見る前に入れる。CRC の不一致は数えるだけ)
//
// クロックの乗り換え: hclk の立ち下がり → clk はバイトの非同期 FIFO (グレイコードのポインタ、フリップフロップ)。
// hclk は止まることがあるので、書き込み側のリセットは非同期。
module hstx_rx #(
    parameter int DEPTH_LOG2 = 4             // バイトの FIFO の深さ (2^DEPTH_LOG2)
) (
    input  logic        hclk,
    input  logic [3:0]  hd,
    input  logic        clk,
    input  logic        rst,                 // clk に同期 (書き込み側には非同期に効かせる)
    // コマンド FIFO へ (word_ready が 1 のクロックで受け取られる)
    output logic        word_valid,
    output logic [31:0] word,
    input  logic        word_ready,
    // 状態 (clk)
    input  logic        stat_clear,
    output logic [7:0]  pkts,                // CRC が合ったパケット
    output logic [7:0]  crc_errs,            // CRC が合わなかったパケット
    output logic [7:0]  sync_errs,           // パケットのあとにマジックが来なかった回数
    output logic [7:0]  drops,               // バイトの FIFO があふれて捨てたバイト
    output logic [31:0] bytes                // 受け取ったワードのバイト数
);
    localparam int D = DEPTH_LOG2;

    function automatic logic [D:0] bin2gray(input logic [D:0] b);
        return b ^ (b >> 1);
    endfunction

    function automatic logic [15:0] crc16_byte(input logic [15:0] c, input logic [7:0] d);
        logic [15:0] x;
        logic        fb;
        x = c;
        for (int i = 7; i >= 0; i--) begin
            fb = x[15] ^ d[i];
            x  = {x[14:0], 1'b0} ^ (fb ? 16'h1021 : 16'h0000);
        end
        return x;
    endfunction

    // ================================================================ 書き込み側 (hclk)
    logic [3:0] lo;
    always_ff @(posedge hclk) lo <= hd;

    (* ram_style = "logic" *) logic [7:0] mem [0:(1 << D) - 1];
    logic [D:0] wbin, wgray;
    logic [D:0] rgray;                       // 読み出し側 (clk) のポインタ
    logic [D:0] rgray_w1, rgray_w2;
    logic       full;
    logic       drop_tog;

    assign full = wgray == {~rgray_w2[D:D-1], rgray_w2[D-2:0]};

    always_ff @(negedge hclk or posedge rst) begin
        if (rst) begin
            rgray_w1 <= '0;
            rgray_w2 <= '0;
            wbin     <= '0;
            wgray    <= '0;
            drop_tog <= 1'b0;
        end else begin
            rgray_w1 <= rgray;
            rgray_w2 <= rgray_w1;
            if (!full) begin
                wbin  <= wbin + 1'b1;
                wgray <= bin2gray(wbin + 1'b1);
            end else begin
                drop_tog <= ~drop_tog;
            end
        end
    end

    always_ff @(negedge hclk) begin
        if (!full) mem[wbin[D-1:0]] <= {hd, lo};
    end

    // ================================================================ 読み出し側 (clk)
    logic [D:0] rbin;
    logic [D:0] wgray_r1, wgray_r2;
    logic [2:0] drop_sync;
    logic       empty, take;
    logic [7:0] b;

    assign empty = rgray == wgray_r2;
    assign b     = mem[rbin[D-1:0]];

    // ================================================================ パケットの解析 (clk)
    typedef enum logic [3:0] {
        P_M0, P_M1, P_M2, P_M3, P_LEN0, P_LEN1, P_DATA, P_CRC0, P_CRC1, P_PAD0, P_PAD1
    } pstate_t;
    pstate_t     ps;
    logic [15:0] len, left, crc, crc_rx;
    logic [1:0]  bcnt;
    logic [23:0] asm_b;
    logic        synced;

    localparam logic [7:0] M0 = 8'h55, M1 = 8'hAA, M2 = 8'h5A, M3 = 8'hA5;

    // バイトを 1 つ進める。ワードの最後のバイト (出力を作る) だけは、前のワードが受け取られるまで待つ
    logic emit_next;
    assign emit_next = ps == P_DATA && bcnt == 2'd3;
    assign take      = !empty && (!emit_next || !word_valid || word_ready);

    always_ff @(posedge clk) begin
        if (rst) begin
            wgray_r1  <= '0;
            wgray_r2  <= '0;
            rbin      <= '0;
            rgray     <= '0;
            drop_sync <= '0;
            ps        <= P_M0;
            synced    <= 1'b0;
            word_valid <= 1'b0;
            pkts      <= '0;
            crc_errs  <= '0;
            sync_errs <= '0;
            drops     <= '0;
            bytes     <= '0;
        end else begin
            wgray_r1  <= wgray;
            wgray_r2  <= wgray_r1;
            drop_sync <= {drop_sync[1:0], drop_tog};
            if (word_valid && word_ready) word_valid <= 1'b0;

            if (stat_clear) begin
                pkts      <= '0;
                crc_errs  <= '0;
                sync_errs <= '0;
                drops     <= '0;
                bytes     <= '0;
            end else if (drop_sync[2] != drop_sync[1]) begin
                drops <= drops + 8'd1;
            end

            if (take) begin
                rbin  <= rbin + 1'b1;
                rgray <= bin2gray(rbin + 1'b1);
                case (ps)
                    P_M0: if (b == M0) ps <= P_M1;
                          else if (synced) begin synced <= 1'b0; sync_errs <= sync_errs + 8'd1; end
                    P_M1: if (b == M1) ps <= P_M2;   else if (b == M0) ps <= P_M1; else ps <= P_M0;
                    P_M2: if (b == M2) ps <= P_M3;   else if (b == M0) ps <= P_M1; else ps <= P_M0;
                    P_M3: if (b == M3) ps <= P_LEN0; else if (b == M0) ps <= P_M1; else ps <= P_M0;
                    P_LEN0: begin len[7:0] <= b; ps <= P_LEN1; end
                    P_LEN1: begin
                        len[15:8] <= b;
                        left      <= {b, len[7:0]};
                        crc       <= 16'hFFFF;
                        bcnt      <= '0;
                        synced    <= 1'b1;
                        if ({b, len[7:0]} == 16'd0) ps <= P_CRC0;
                        else                        ps <= P_DATA;
                    end
                    P_DATA: begin
                        crc  <= crc16_byte(crc, b);
                        bcnt <= bcnt + 2'd1;
                        if (bcnt == 2'd3) begin
                            word       <= {b, asm_b};
                            word_valid <= 1'b1;
                            bytes      <= bytes + 32'd4;
                            left       <= left - 16'd1;
                            if (left == 16'd1) ps <= P_CRC0;
                        end else begin
                            asm_b[8 * bcnt +: 8] <= b;
                        end
                    end
                    P_CRC0: begin crc_rx[7:0] <= b; ps <= P_CRC1; end
                    P_CRC1: begin
                        if ({b, crc_rx[7:0]} == crc) pkts     <= pkts + 8'd1;
                        else                         crc_errs <= crc_errs + 8'd1;
                        ps <= P_PAD0;
                    end
                    P_PAD0: ps <= P_PAD1;
                    P_PAD1: ps <= P_M0;
                    default: ps <= P_M0;
                endcase
            end
        end
    end
endmodule
