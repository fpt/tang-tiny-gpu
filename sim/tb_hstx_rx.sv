// hstx_rx のテストベンチ
//   RP2350 の HSTX (CLKDIV = 2、CLKPHASE = 1) と同じ形でクロックとニブルを出す: ニブルを出してから半分で立ち上がり (下位)、
//   次のニブルの半分で立ち下がり (上位)。パケットのあいだはクロックを止める (Low)
//   確かめること: ワードが順に出る、CRC が合う / 合わない、マジックが来ないとき、受け取りを止めてもバイトを落とさない、速いクロック
`timescale 1ns/1ps
module tb_hstx_rx;
    logic clk = 1'b0, rst = 1'b0;      // 0 から始めて立ち上げる (書き込み側の非同期リセットを効かせる)
    always #7.9 clk = ~clk;            // 約 63MHz (clk_sys)

    logic       hclk = 1'b0;
    logic [3:0] hd = 4'h0;
    real        half_ns = 53.0;        // ニブル 1 つの長さ (clk_hstx の 1 周期)。53ns = 約 9.4MHz

    logic        word_valid, word_ready, stat_clear;
    logic [31:0] word;
    logic [7:0]  pkts, crc_errs, sync_errs, drops;
    logic [31:0] bytes;

    hstx_rx dut (
        .hclk, .hd, .clk, .rst, .word_valid, .word, .word_ready,
        .stat_clear, .pkts, .crc_errs, .sync_errs, .drops, .bytes, .dbg_wptr(), .dbg_rptr()
    );

    // 受け取ったワード
    logic [31:0] got [0:4095];
    int          n_got = 0;
    logic        stall = 1'b0;          // 1 なら受け取りをときどき止める
    int unsigned lfsr = 32'h1234_5678;
    always_ff @(posedge clk) begin
        lfsr <= {lfsr[30:0], lfsr[31] ^ lfsr[21] ^ lfsr[1] ^ lfsr[0]};
        if (word_valid && word_ready) begin
            got[n_got] <= word;
            n_got      <= n_got + 1;
        end
    end
    // 止めるときは 4 クロックに 1 回ほどしか受け取らない (本物のコマンド FIFO は満杯でなければ毎クロック受け取る。
    // バイトの FIFO は 16 バイトなので、入りの 2.5 倍ほどで受け取れれば落とさない)
    assign word_ready = !stall || lfsr[1:0] == 2'd0;

    int errors = 0;

    task automatic send_byte(input logic [7:0] v);
        hd = v[3:0];
        #(half_ns / 2) hclk = 1'b1;
        #(half_ns / 2) hd = v[7:4];
        #(half_ns / 2) hclk = 1'b0;
        #(half_ns / 2);
    endtask

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

    // パケットを送る。words[0..n-1]、bad_crc なら CRC を 1 ビット反転
    logic [31:0] words [0:4095];
    task automatic send_packet(input int n, input bit bad_crc);
        logic [15:0] crc;
        crc = 16'hFFFF;
        send_byte(8'h55); send_byte(8'hAA); send_byte(8'h5A); send_byte(8'hA5);
        send_byte(n[7:0]); send_byte(n[15:8]);
        for (int i = 0; i < n; i++)
            for (int k = 0; k < 4; k++) begin
                send_byte(words[i][8 * k +: 8]);
                crc = crc16_byte(crc, words[i][8 * k +: 8]);
            end
        if (bad_crc) crc = crc ^ 16'h0001;
        send_byte(crc[7:0]); send_byte(crc[15:8]);
        #(half_ns * 3);                   // クロックを止める
    endtask

    task automatic check(input string what, input bit ok);
        if (!ok) begin
            $display("FAIL: %s", what);
            errors++;
        end
    endtask

    task automatic wait_words(input int n);
        int t;
        t = 0;
        while (n_got < n && t < 200000) begin @(posedge clk); t++; end
        repeat (20) @(posedge clk);
    endtask

    int expect_n = 0;
    logic [31:0] expect_w [0:8191];

    task automatic queue_packet(input int n, input int seed);
        for (int i = 0; i < n; i++) begin
            words[i] = 32'h9E37_79B9 * (seed * 1000 + i + 1);
            expect_w[expect_n + i] = words[i];
        end
        expect_n += n;
    endtask

    initial begin
        $dumpfile("build/sim/tb_hstx_rx.vcd");
        $dumpvars(0, tb_hstx_rx);
        stat_clear = 1'b0;
        #1 rst = 1'b1;
        repeat (10) @(posedge clk);
        rst = 1'b0;
        repeat (10) @(posedge clk);

        // 1. ごみのあとに 3 ワード
        send_byte(8'h12); send_byte(8'h55); send_byte(8'h00);
        queue_packet(3, 1);
        send_packet(3, 1'b0);
        wait_words(expect_n);
        check("packet 1: 3 words", n_got == 3);
        check("packet 1: pkts == 1", pkts == 8'd1);
        check("packet 1: no crc error", crc_errs == 8'd0);
        check("packet 1: bytes == 12", bytes == 32'd12);

        // 2. CRC が合わないパケット (ワードは入る、エラーを数える)
        queue_packet(5, 2);
        send_packet(5, 1'b1);
        wait_words(expect_n);
        check("packet 2: crc error counted", crc_errs == 8'd1 && pkts == 8'd1);

        // 3. 続けてマジックでないバイト → 同期のエラー。そのあとの正しいパケットは受け取る
        send_byte(8'h77);
        queue_packet(2, 3);
        send_packet(2, 1'b0);
        wait_words(expect_n);
        check("sync error counted", sync_errs == 8'd1);
        check("packet 3 accepted", pkts == 8'd2);

        // 4. 長さ 0
        send_packet(0, 1'b0);
        repeat (50) @(posedge clk);
        check("empty packet", pkts == 8'd3);

        // 5. 受け取りを止めながら 300 ワード (クロックは遅いので落とさない)
        stall = 1'b1;
        queue_packet(300, 4);
        send_packet(300, 1'b0);
        wait_words(expect_n);
        stall = 1'b0;
        check("stalled: no drops", drops == 8'd0);
        check("stalled: pkts == 4", pkts == 8'd4);

        // 6. 速いクロック (ニブル 13.3ns = clk_hstx 75MHz、37.5MB/s)
        half_ns = 13.3;
        queue_packet(1000, 5);
        send_packet(1000, 1'b0);
        wait_words(expect_n);
        check("fast: no drops", drops == 8'd0);
        check("fast: pkts == 5", pkts == 8'd5);
        check("fast: no crc errors besides packet 2", crc_errs == 8'd1);

        // 全部のワードが順に出たか
        check($sformatf("word count %0d == %0d", n_got, expect_n), n_got == expect_n);
        for (int i = 0; i < expect_n && i < n_got; i++)
            if (got[i] !== expect_w[i]) begin
                if (errors < 20) $display("FAIL: word %0d got %08x expected %08x", i, got[i], expect_w[i]);
                errors++;
            end

        // 状態のクリア
        @(posedge clk) stat_clear = 1'b1;
        @(posedge clk) stat_clear = 1'b0;
        @(posedge clk);
        check("stat clear", pkts == 0 && crc_errs == 0 && sync_errs == 0 && bytes == 0);

        // 7. パケットの途中でクリアしたら、次のパケットから受け直す (長さ 1000 の頭だけ送ってやめる)
        half_ns = 53.0;
        send_byte(8'h55); send_byte(8'hAA); send_byte(8'h5A); send_byte(8'hA5); send_byte(8'hE8); send_byte(8'h03);
        send_byte(8'h11); send_byte(8'h22);
        repeat (20) @(posedge clk);
        @(posedge clk) stat_clear = 1'b1;
        @(posedge clk) stat_clear = 1'b0;
        n_got = 0;
        expect_n = 0;
        queue_packet(4, 6);
        send_packet(4, 1'b0);
        wait_words(4);
        check("resync after clear", pkts == 8'd1 && n_got == 4 && got[0] == expect_w[0] && got[3] == expect_w[3]);

        if (errors == 0) $display("PASS: tb_hstx_rx (%0d words, %0d packets)", n_got, 5);
        else             $display("FAIL: tb_hstx_rx (%0d errors)", errors);
        $finish;
    end
endmodule
