// ハーネスの RP2 ブリッジ (docs/ARCHITECTURE.md の段階 M0)
//
// GPU のレジスタやコマンドの意味は知らず、ホストから受け取った SPI のバイト列を素通しするだけ。
// 一度書き込んだら、原則として書き換えない。
//
// USB シリアルで 1 行 1 コマンド、応答は 1 行 JSON ("ok" が true/false):
//   ping                 ファームウェア名・版・ボード
//   status               全ハーネスピンの値、HEARTBEAT / VSYNC / IRQ の周期、ready 判定、カウンタ
//   spi <hex>            CS を上げて送受信し、受信を hex で返す (最大 4096 バイト)。ready でなければ何も駆動しない
//   spib <n>             改行 (\n 1 文字) の直後に n バイトの生データを続けて送る。送るだけで、応答は受信の最初の 1 バイト (同期確認用) だけ。
//                        大きいデータの書き込み用 (16 進にしないので USB の量が半分、受信も送り返さない)。v2 から
//   stream <n> <port> <free>
//                        改行の直後に n バイト (4 の倍数) の生データ。レジスタ <port> (アドレスの進まないポート) に、
//                        レジスタ <free> (16bit、空きのワード数) を SPI で読んで空いている分ずつ書く。v3 から。
//                        ホストとの USB の往復なしに流し、SPI は DMA で送るあいだに次の分を USB から受け取る。
//                        ブリッジはレジスタの意味を知らない (番地はホストが渡す)。応答は 1 回の SPI の数と、空きを待った回数
//   freq <hz>            SPI の周波数
//   reset [ms]           RESET を指定時間 High にする (既定 10ms)
//   irqwait [ms]         IRQ の立ち上がりを待つ (既定 100ms)
//   wiretest             配線テスト (FPGA に top_harness_probe、v2 は top_harness_probe_v2 が入っているとき)
//   bootsel              BOOTSEL (UF2 書き込み) モードで再起動する
//   (v2 だけ。HSTX、docs/HARNESS2.md の 3 章。パケットは rtl/vdp/hstx_rx.sv)
//   hstx <sys|usb> <div> HSTX を clk_hstx = (clk_sys 150MHz か pll_usb 48MHz) / div (1〜4。clk_hstx の分周は 2 ビットしかない) で動かす。
//                        hstx off で止めてピンを離す。ニブルを 1 サイクルに 1 つ (clk_hstx / 2 バイト/秒)、クロックは CLKDIV 2 / CLKPHASE 1
//   hstxw <n> <free>     改行の直後に n バイト (4 の倍数) の生データ。レジスタ <free> (GPU_FREE) で空きを待ってから、1 つのパケットで送る
//   hstxload <n> [p]     改行の直後に n バイト。p ワード (既定 256) ずつのパケットに組んで RAM に置く (CRC もここで計算)
//   hstxrep <reps> <free>  hstxload の中身を reps 回、空きを見ながら DMA で流し、かかった時間を返す (USB を通さずにリンクの速さを測る)
//
// 衝突させないための約束:
//   - ハーネスのピンは、駆動するとき以外は常に「入力 + プルダウン」(release_all / release。Tang 側でプルアップされている線はプルアップ)
//   - RP2 -> FPGA の線は正論理なので、プルダウンされた状態は「何もしていない」
//   - SPI 線を駆動するのは、HEARTBEAT (v2 は VSYNC) が直近 100ms 以内にトグルしている (FPGA に設計が入って動いている) ときだけ
//   - ピンの向きと機能を変えるのはこのファイルだけ。ピン番号は harness/pins.json (v2 は harness/kicad/v2/pins_v2.json) から生成した pins.h
//
// v1 (RP2040-Zero) と v2 (Pico 2) の違いは pins.h の信号の有無で分ける (PIN_HEARTBEAT は v1 だけ、PIN_HSTX_CLK は v2 だけ)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "hardware/dma.h"
#include "pins.h"

#define FW_NAME          "harness-bridge"
#define FW_VERSION       3
// SCK の GPIO から SPI の番号が決まる (GPIO 0〜7 / 16〜23 は SPI0、8〜15 / 24〜 は SPI1)。v1 は GP2 で SPI0、v2 は GP10 で SPI1
#define BRIDGE_SPI       (((PIN_SCK >> 3) & 1) ? spi1 : spi0)
#define READY_WINDOW_US  100000u
#define MAX_SPI_BYTES    4096
#define LINE_MAX         (16 + 2 * MAX_SPI_BYTES)

#ifdef PIN_HSTX_CLK
#define HARNESS_REV      "v2"
#define READY_SIGNAL     "VSYNC"
#else
#define HARNESS_REV      "v1"
#define READY_SIGNAL     "HEARTBEAT"
#endif

#ifndef HARNESS_BOARD
#define HARNESS_BOARD "unknown"
#endif

// ---------------------------------------------------------------- ピン

// 入力にする。プルは基本プルダウンだが、Tang 側でプルアップされている線 (pins の idle_high。v2 の SCK / COPI は LED、
// HSTX_CLK は 10k) はプルアップにする (プルダウンだと中間の電圧になり、FPGA から 0 / 1 が定まらない。実機で見た)
static void release(unsigned gpio) {
    bool up = false;
    for (int i = 0; i < HARNESS_PIN_COUNT; i++)
        if (HARNESS_PINS[i].gpio == gpio) up = HARNESS_PINS[i].idle_high;
    gpio_init(gpio);                   // SIO, 入力, 出力値 0
    gpio_set_pulls(gpio, up, !up);
}

// ハーネスの全ピン (と未使用の GP0/GP1) を入力 + プルダウンにする
static void release_all(void) {
    for (int i = 0; i < HARNESS_PIN_COUNT; i++) release(HARNESS_PINS[i].gpio);
    release(0);
    release(1);
}

static const char *pin_name(unsigned gpio) {
    for (int i = 0; i < HARNESS_PIN_COUNT; i++)
        if (HARNESS_PINS[i].gpio == gpio) return HARNESS_PINS[i].name;
    return "?";
}

static uint32_t harness_mask(bool fpga_output) {
    uint32_t m = 0;
    for (int i = 0; i < HARNESS_PIN_COUNT; i++)
        if (HARNESS_PINS[i].fpga_output == fpga_output) m |= 1u << HARNESS_PINS[i].gpio;
    return m;
}

// ---------------------------------------------------------------- エッジの監視

typedef struct {
    volatile uint64_t last_us;    // 直近の立ち上がり
    volatile uint32_t period_us;  // 直近 2 回の立ち上がりの間隔
    volatile uint32_t rises;
} edge_t;

static edge_t edge_hb, edge_vs, edge_irq;

static void gpio_cb(uint gpio, uint32_t events) {
    if (!(events & GPIO_IRQ_EDGE_RISE)) return;
    edge_t *e = gpio == PIN_VSYNC ? &edge_vs : gpio == PIN_IRQ ? &edge_irq : NULL;
#ifdef PIN_HEARTBEAT
    if (gpio == PIN_HEARTBEAT) e = &edge_hb;
#endif
    if (!e) return;
    uint64_t now = time_us_64();
    if (e->last_us) e->period_us = (uint32_t)(now - e->last_us);
    e->last_us = now;
    e->rises++;
}

static void monitor_enable(void) {
    gpio_set_irq_enabled_with_callback(PIN_VSYNC, GPIO_IRQ_EDGE_RISE, true, gpio_cb);
#ifdef PIN_HEARTBEAT
    gpio_set_irq_enabled(PIN_HEARTBEAT, GPIO_IRQ_EDGE_RISE, true);
#endif
    gpio_set_irq_enabled(PIN_IRQ, GPIO_IRQ_EDGE_RISE, true);
}

// FPGA に設計が入って動いているか (v1 は HEARTBEAT、v2 は VSYNC が直近 100ms 以内にトグルしている)
static bool fpga_ready(void) {
#ifdef PIN_HEARTBEAT
    uint64_t last = edge_hb.last_us;
#else
    uint64_t last = edge_vs.last_us;
#endif
    return last != 0 && time_us_64() - last < READY_WINDOW_US;
}

static void print_edge(const char *name, const edge_t *e) {
    uint64_t last = e->last_us;
    uint64_t age_us = last ? time_us_64() - last : 0;
    // 1 秒以上エッジがなければ止まっているとみなす
    double hz = (last && age_us < 1000000 && e->period_us) ? 1e6 / e->period_us : 0.0;
    printf("\"%s\":{\"hz\":%.2f,\"rises\":%lu,\"age_ms\":%ld}", name, hz, (unsigned long)e->rises,
           last ? (long)(age_us / 1000) : -1L);
}

// ---------------------------------------------------------------- SPI

static uint32_t spi_hz;
static uint32_t n_spi_ok, n_spi_refused;

// CS を上げて送受信し、下げる。終わったら全部入力 + プルダウンに戻す
static void spi_xfer(const uint8_t *tx, uint8_t *rx, size_t n) {
    gpio_put(PIN_CS, 0);
    gpio_set_dir(PIN_CS, GPIO_OUT);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_COPI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_CIPO, GPIO_FUNC_SPI);
    gpio_put(PIN_CS, 1);
    spi_write_read_blocking(BRIDGE_SPI, tx, rx, n);
    gpio_put(PIN_CS, 0);
    release(PIN_SCK);
    release(PIN_COPI);
    release(PIN_CIPO);
    release(PIN_CS);
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// ---------------------------------------------------------------- 配線テスト

#define WIRETEST_SAMPLE_US 500000u
#define FPGA_CLK_HZ        27000000.0

#ifdef PIN_HEARTBEAT
// FPGA -> RP2 の各線が使うカウンタのビット (rtl/gowin/top_harness_probe.sv と合わせる)
static const struct { unsigned gpio; int bit; } PROBE_BITS[] = {
    {PIN_CIPO, 10}, {PIN_IRQ, 11}, {PIN_VSYNC, 12}, {PIN_HEARTBEAT, 13}, {PIN_ERROR, 14},
    {PIN_LED0, 15}, {PIN_LED1, 16}, {PIN_LED2, 17}, {PIN_LED3, 18}, {PIN_LED4, 19}, {PIN_LED5, 20},
};

// RP2 が High にする線と、FPGA が折り返してくる線
static const struct { unsigned drive; unsigned echo; } LOOPBACK[] = {
    {PIN_SCK, PIN_CIPO}, {PIN_COPI, PIN_IRQ}, {PIN_CS, PIN_VSYNC}, {PIN_RESET, PIN_HEARTBEAT},
};
#else
// v2: FPGA -> RP2 は 3 本 (rtl/gowin/top_harness_probe_v2.sv と合わせる)
static const struct { unsigned gpio; int bit; } PROBE_BITS[] = {
    {PIN_CIPO, 10}, {PIN_IRQ, 11}, {PIN_VSYNC, 12},
};

// RP2 -> FPGA の線と、FPGA が返す番号。VSYNC が Low の間は {IRQ, CIPO} が番号の下 2 ビット、High の間は上 2 ビット
static const struct { unsigned drive; unsigned code; } LOOPBACK[] = {
    {PIN_SCK, 1}, {PIN_COPI, 2}, {PIN_CS, 3}, {PIN_RESET, 4},
    {PIN_HSTX_D0, 5}, {PIN_HSTX_D1, 6}, {PIN_HSTX_D2, 7}, {PIN_HSTX_D3, 8}, {PIN_HSTX_CLK, 9},
};

static bool idle_high(unsigned gpio) {
    for (int i = 0; i < HARNESS_PIN_COUNT; i++)
        if (HARNESS_PINS[i].gpio == gpio) return HARNESS_PINS[i].idle_high;
    return false;
}

// 1ms (VSYNC 3 周期ほど) 読んで、VSYNC の Low / High のそれぞれで {IRQ, CIPO} がほぼ一定 (9 割以上同じ値) なら番号を返す。
// そうでなければ (エコーになっていない = FPGA に線が届いていない。エコーでないときは CIPO / IRQ が半周期に数回トグルする) -1。
// VSYNC (52) は HDMI の DDC_CLK と共有で 2.2k のプルアップとケーブルがつながり、エッジがなまって揺れることがあるので、
// VSYNC が変わってから 10us の間は読まない (半周期は 152us)。
// toggle が 0 以上なら、その GPIO (出力にしてあること) を読みながら反転させ続ける (HSTX_CLK はクロックとして受けるので)
static int read_code(int toggle) {
    uint32_t count[2][4] = {{0}};
    uint32_t prev_vs = 2;
    uint64_t changed_us = 0;
    if (toggle >= 0) {
        const uint64_t warm = time_us_64() + 100;
        while (time_us_64() < warm) gpio_xor_mask(1u << toggle);
    }
    const uint64_t end = time_us_64() + 1000;
    uint64_t now;
    while ((now = time_us_64()) < end) {
        if (toggle >= 0) gpio_xor_mask(1u << toggle);
        uint32_t v = gpio_get_all();
        uint32_t vs = (v >> PIN_VSYNC) & 1;
        if (vs != prev_vs) { prev_vs = vs; changed_us = now; continue; }
        if (now - changed_us < 10) continue;
        count[vs][((v >> PIN_IRQ) & 1) << 1 | ((v >> PIN_CIPO) & 1)]++;
    }
    int code[2];
    for (int h = 0; h < 2; h++) {
        uint32_t total = 0, best = 0;
        code[h] = -1;
        for (int k = 0; k < 4; k++) {
            total += count[h][k];
            if (count[h][k] > best) { best = count[h][k]; code[h] = k; }
        }
        if (total < 20 || best * 10 < total * 9) return -1;
    }
    return code[1] << 2 | code[0];
}
#endif

#ifdef PIN_HSTX_CLK
static void hstx_off(void);
#endif

static void cmd_wiretest(void) {
#ifdef PIN_HSTX_CLK
    hstx_off();                          // HSTX のピンも離す
#endif
    const uint32_t from_fpga = harness_mask(true);
    const uint32_t to_fpga   = harness_mask(false);
    bool pass = true;

    release_all();
    sleep_ms(10);

    // 第 1 段階: 読むだけ
    static uint32_t rises[32];
    memset(rises, 0, sizeof rises);
    uint32_t prev = gpio_get_all(), ever_high = prev, ever_low = ~prev;
    (void)ever_low;
    const uint64_t end = time_us_64() + WIRETEST_SAMPLE_US;
    while (time_us_64() < end) {
        uint32_t v = gpio_get_all();
        uint32_t rise = v & ~prev;
        ever_high |= v;
        ever_low |= ~v;
        while (rise) {
            rises[__builtin_ctz(rise)]++;
            rise &= rise - 1;
        }
        prev = v;
    }

    printf("{\"phase1\":[");
    for (size_t i = 0; i < count_of(PROBE_BITS); i++) {
        unsigned g = PROBE_BITS[i].gpio;
        double expect = FPGA_CLK_HZ / (double)(1u << (PROBE_BITS[i].bit + 1)) * (WIRETEST_SAMPLE_US / 1e6);
        bool ok = abs((int)rises[g] - (int)(expect + 0.5)) <= (int)(expect / 10) + 2;
        pass &= ok;
        printf("%s{\"sig\":\"%s\",\"rises\":%lu,\"expect\":%.0f,\"ok\":%s}", i ? "," : "", pin_name(g),
               (unsigned long)rises[g], expect, ok ? "true" : "false");
    }
    // RP2 -> FPGA の線は FPGA が駆動していないはずなので、ずっと Low (v2 の SCK / COPI / HSTX_CLK は Tang 側のプルアップでずっと High)
    for (int g = 0; g < 32; g++) {
        if (!(to_fpga & (1u << g))) continue;
#ifdef PIN_HEARTBEAT
        bool ok = rises[g] == 0 && !(ever_high & (1u << g));
#else
        bool moved = idle_high(g) ? (ever_low >> g) & 1 : (ever_high >> g) & 1;
        bool ok = rises[g] == 0 && !moved;
#endif
        pass &= ok;
        printf(",{\"sig\":\"%s\",\"rises\":%lu,\"high_seen\":%s,\"ok\":%s}", pin_name(g), (unsigned long)rises[g],
               (ever_high & (1u << g)) ? "true" : "false", ok ? "true" : "false");
    }
    printf("]");

    // 第 2 段階: 第 1 段階が全部通ったときだけ、1 本ずつ 100us High にして折り返しを見る
#ifdef PIN_HEARTBEAT
    if (pass) {
        printf(",\"phase2\":[");
        for (size_t i = 0; i < count_of(LOOPBACK); i++) {
            unsigned d = LOOPBACK[i].drive;
            gpio_put(d, 1);
            gpio_set_dir(d, GPIO_OUT);
            sleep_us(100);
            uint32_t v = gpio_get_all() & from_fpga;
            release(d);
            sleep_us(100);
            bool ok = v == (1u << LOOPBACK[i].echo);   // 折り返し先だけが High
            pass &= ok;
            printf("%s{\"drive\":\"%s\",\"echo\":\"%s\",\"seen\":\"0x%08lx\",\"ok\":%s}", i ? "," : "",
                   pin_name(d), pin_name(LOOPBACK[i].echo), (unsigned long)v, ok ? "true" : "false");
        }
        printf("]");
    } else {
        printf(",\"phase2\":\"skipped\"");
    }
#else
    // v2: 1 本ずつ立てて (離すと High の線は Low に、HSTX_CLK はトグル)、FPGA が返す番号を見る。
    // 15 は 2 本以上が立っている (短絡)、-1 は返ってこない (断線)
    if (pass) {
        printf(",\"phase2\":[");
        for (size_t i = 0; i < count_of(LOOPBACK); i++) {
            unsigned d = LOOPBACK[i].drive;
            gpio_put(d, !idle_high(d));
            gpio_set_dir(d, GPIO_OUT);
            sleep_us(100);
            int code = read_code(d == PIN_HSTX_CLK ? (int)d : -1);
            // RP2350-E9: High を出していたピンを入力 + 内蔵プルダウンに戻すと、約 2V に留まって FPGA から High に見える
            // (外付けのプルダウンがない HSTX の線)。いったん Low を出してから離す
            if (!idle_high(d)) {
                gpio_put(d, 0);
                sleep_us(10);
            }
            release(d);
            sleep_us(200);
            bool ok = code == (int)LOOPBACK[i].code;
            pass &= ok;
            printf("%s{\"drive\":\"%s\",\"expect\":%u,\"seen\":%d,\"ok\":%s}", i ? "," : "",
                   pin_name(d), LOOPBACK[i].code, code, ok ? "true" : "false");
        }
        // 何も立てていないときは、エコーにならない (番号が読めない) はず
        int idle = read_code(-1);
        bool ok = idle < 0;
        pass &= ok;
        printf(",{\"drive\":\"none\",\"seen\":%d,\"ok\":%s}]", idle, ok ? "true" : "false");
    } else {
        printf(",\"phase2\":\"skipped\"");
    }
#endif

    release_all();
    monitor_enable();
    printf(",\"ok\":%s}\n", pass ? "true" : "false");
}

// ---------------------------------------------------------------- コマンド

static void reply_error(const char *msg) {
    printf("{\"ok\":false,\"error\":\"%s\"}\n", msg);
}

static void cmd_status(void) {
    printf("{\"ok\":true,\"ready\":%s,\"uptime_ms\":%lu,\"spi_hz\":%lu,\"spi_ok\":%lu,\"spi_refused\":%lu,\"pins\":{",
           fpga_ready() ? "true" : "false", (unsigned long)(time_us_64() / 1000), (unsigned long)spi_hz,
           (unsigned long)n_spi_ok, (unsigned long)n_spi_refused);
    for (int i = 0; i < HARNESS_PIN_COUNT; i++)
        printf("%s\"%s\":%d", i ? "," : "", HARNESS_PINS[i].name, gpio_get(HARNESS_PINS[i].gpio));
    printf("},");
#ifdef PIN_HEARTBEAT
    print_edge("heartbeat", &edge_hb);
    printf(",");
#endif
    print_edge("vsync", &edge_vs);
    printf(",");
    print_edge("irq", &edge_irq);
    printf("}\n");
}

static void cmd_spi(const char *hex) {
    static uint8_t tx[MAX_SPI_BYTES], rx[MAX_SPI_BYTES];
    size_t len = strlen(hex);
    if (len == 0 || len % 2 || len / 2 > MAX_SPI_BYTES) { reply_error("spi: need 1..4096 bytes of hex"); return; }
    for (size_t i = 0; i < len / 2; i++) {
        int hi = hexval(hex[2 * i]), lo = hexval(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) { reply_error("spi: bad hex"); return; }
        tx[i] = (uint8_t)(hi << 4 | lo);
    }
    if (!fpga_ready()) {
        n_spi_refused++;
        { reply_error("not ready: " READY_SIGNAL " is not toggling, nothing was driven"); return; }
    }
    spi_xfer(tx, rx, len / 2);
    n_spi_ok++;
    static const char digits[] = "0123456789abcdef";
    static char out[2 * MAX_SPI_BYTES + 1];
    for (size_t i = 0; i < len / 2; i++) {
        out[2 * i] = digits[rx[i] >> 4];
        out[2 * i + 1] = digits[rx[i] & 15];
    }
    out[len] = 0;
    printf("{\"ok\":true,\"rx\":\"%s\"}\n", out);
}

// ---------------------------------------------------------------- USB からの入力
// 1 文字ずつではなく、届いている分をまとめて in_buf に読む (v1 は getchar で 1 文字ずつ読んでいて、2KiB の spi に 60ms かかっていた)
#define IN_BUF_SIZE (LINE_MAX + MAX_SPI_BYTES)
static uint8_t in_buf[IN_BUF_SIZE];
static size_t  in_head, in_tail;      // [in_head, in_tail) が未処理

static void in_fill(uint32_t timeout_us) {
    if (in_head == in_tail) {
        in_head = in_tail = 0;
    } else if (in_tail == IN_BUF_SIZE) {
        memmove(in_buf, in_buf + in_head, in_tail - in_head);
        in_tail -= in_head;
        in_head = 0;
    }
    if (in_tail == IN_BUF_SIZE) return;
    int n = stdio_get_until((char *)in_buf + in_tail, (int)(IN_BUF_SIZE - in_tail), make_timeout_time_us(timeout_us));
    if (n > 0) in_tail += (size_t)n;
}

// 次の 1 行 (\n か \r まで、改行は含めない)。長すぎる行は *overflow を立てて残りを読み捨てる
static void in_line(char *line, size_t max, bool *overflow) {
    size_t len = 0;
    *overflow = false;
    while (true) {
        while (in_head < in_tail) {
            char c = (char)in_buf[in_head++];
            if (c == '\n' || c == '\r') {
                if (len || *overflow) {
                    line[len] = 0;
                    return;
                }
                continue;                   // 空行
            }
            if (len < max - 1) line[len++] = c;
            else               *overflow = true;
        }
        in_fill(100000);
    }
}

// n バイトの生データ。timeout_us 以内に揃わなければ false (それまでに読んだ分は捨てる)
static bool in_bytes(uint8_t *dst, size_t n, uint32_t timeout_us) {
    absolute_time_t until = make_timeout_time_us(timeout_us);
    size_t got = 0;
    while (got < n) {
        size_t k = in_tail - in_head;
        if (k > n - got) k = n - got;
        memcpy(dst + got, in_buf + in_head, k);
        in_head += k;
        got += k;
        if (got < n) {
            if (time_reached(until)) return false;
            in_fill(1000);
        }
    }
    return true;
}

static void cmd_spib(const char *arg) {
    static uint8_t tx[MAX_SPI_BYTES], rx[MAX_SPI_BYTES];
    long n = arg ? atol(arg) : 0;
    if (n < 1 || n > MAX_SPI_BYTES) { reply_error("spib: 1..4096 bytes"); return; }
    if (!in_bytes(tx, (size_t)n, 2000000)) { reply_error("spib: timeout waiting for data"); return; }
    if (!fpga_ready()) {
        n_spi_refused++;
        reply_error("not ready: " READY_SIGNAL " is not toggling, nothing was driven");
        return;
    }
    spi_xfer(tx, rx, (size_t)n);
    n_spi_ok++;
    printf("{\"ok\":true,\"n\":%ld,\"rx0\":\"%02x\"}\n", n, rx[0]);
}

// ---------------------------------------------------------------- stream (v3)

#define STREAM_CHUNK 2048
#define SYNC_BYTE    0xA5

static void spi_pins_on(void) {
    gpio_put(PIN_CS, 0);
    gpio_set_dir(PIN_CS, GPIO_OUT);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_COPI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_CIPO, GPIO_FUNC_SPI);
}

static void spi_pins_off(void) {
    release(PIN_SCK);
    release(PIN_COPI);
    release(PIN_CIPO);
    release(PIN_CS);
}

// 受信 FIFO を空にして、あふれの印を消す (DMA で送るだけのとき受信は読まない)
static void spi_drain(void) {
    while (spi_is_busy(BRIDGE_SPI)) tight_loop_contents();
    while (spi_is_readable(BRIDGE_SPI)) (void)spi_get_hw(BRIDGE_SPI)->dr;
    spi_get_hw(BRIDGE_SPI)->icr = SPI_SSPICR_RORIC_BITS;
}

// 16bit のレジスタを読む (ピンは spi_pins_on のまま)。同期バイトが違えば -1
static int spi_read16(uint8_t reg) {
    uint8_t tx[3] = {(uint8_t)(0x80 | reg), 0, 0}, rx[3];
    gpio_put(PIN_CS, 1);
    spi_write_read_blocking(BRIDGE_SPI, tx, rx, 3);
    gpio_put(PIN_CS, 0);
    if (rx[0] != SYNC_BYTE) return -1;
    return rx[1] | rx[2] << 8;
}

static void cmd_stream(char *arg) {
    static uint8_t buf[2][STREAM_CHUNK];
    char *a_port = strtok(NULL, " \t"), *a_free = strtok(NULL, " \t");
    long n = arg ? atol(arg) : 0;
    if (n < 4 || n % 4 || !a_port || !a_free) { reply_error("stream: <n (multiple of 4)> <port> <free>"); return; }
    uint8_t port = (uint8_t)strtol(a_port, NULL, 0), free_reg = (uint8_t)strtol(a_free, NULL, 0);
    size_t left = (size_t)n, have, off = 0, next = 0;
    int cur = 0;
    have = left < STREAM_CHUNK ? left : STREAM_CHUNK;
    if (!in_bytes(buf[0], have, 2000000)) { reply_error("stream: timeout waiting for data"); return; }
    if (!fpga_ready()) {
        // 残りのデータは読み捨てる (次のコマンドの行と混ざらないように)
        for (size_t rest = left - have; rest; ) {
            size_t k = rest < STREAM_CHUNK ? rest : STREAM_CHUNK;
            if (!in_bytes(buf[1], k, 2000000)) break;
            rest -= k;
        }
        n_spi_refused++;
        reply_error("not ready: " READY_SIGNAL " is not toggling, nothing was driven");
        return;
    }
    int dma = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_dreq(&c, spi_get_dreq(BRIDGE_SPI, true));
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    spi_pins_on();
    uint32_t xfers = 0, waits = 0;
    const char *err = NULL;
    absolute_time_t until = make_timeout_time_us(2000000);
    while (left) {
        // 空きを待つ (読むたびに 2 秒の期限を延ばすのは、空きが増えたときだけ)
        int fr = spi_read16(free_reg);
        if (fr < 0) { err = "stream: bad sync byte"; break; }
        size_t room = (size_t)fr * 4;
        if (room == 0) {
            waits++;
            if (time_reached(until)) { err = "stream: FIFO stays full"; break; }
            continue;
        }
        until = make_timeout_time_us(2000000);
        size_t k = have - off;
        if (k > room) k = room;
        // ポートのアドレスを送ってから、データを DMA で
        uint8_t hdr = port & 0x7F, rx0;
        gpio_put(PIN_CS, 1);
        spi_write_read_blocking(BRIDGE_SPI, &hdr, &rx0, 1);
        if (rx0 != SYNC_BYTE) { gpio_put(PIN_CS, 0); err = "stream: bad sync byte"; break; }
        dma_channel_configure(dma, &c, &spi_get_hw(BRIDGE_SPI)->dr, buf[cur] + off, k, true);
        // 送っているあいだに次の分を USB から
        if (off + k == have && left > have && next == 0) {
            next = left - have < STREAM_CHUNK ? left - have : STREAM_CHUNK;
            if (!in_bytes(buf[cur ^ 1], next, 2000000)) {
                dma_channel_wait_for_finish_blocking(dma);
                spi_drain();
                gpio_put(PIN_CS, 0);
                err = "stream: timeout waiting for data";
                break;
            }
        }
        dma_channel_wait_for_finish_blocking(dma);
        spi_drain();
        gpio_put(PIN_CS, 0);
        xfers++;
        off += k;
        left -= k;
        if (off == have && left) {
            cur ^= 1;
            have = next;
            next = 0;
            off = 0;
        }
    }
    spi_pins_off();
    dma_channel_unclaim(dma);
    if (err) { reply_error(err); return; }
    n_spi_ok += xfers;
    printf("{\"ok\":true,\"n\":%ld,\"xfers\":%lu,\"waits\":%lu}\n", n, (unsigned long)xfers, (unsigned long)waits);
}

// ---------------------------------------------------------------- HSTX (ハーネス v2)
#ifdef PIN_HSTX_CLK
#include "hardware/clocks.h"
#include "hardware/resets.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"

#define HSTX_MAGIC  0xA55AAA55u          // バイトの順で 55 AA 5A A5
#define HSTX_GUARD  32u                  // 送ったが GPU の FIFO にまだ入っていないワード (HSTX の FIFO 8 + FPGA のバイトの FIFO など) の見積もり
#define REP_WORDS   (48u * 1024u)        // hstxload の RAM (192KB)

static uint32_t hstx_hz;                 // 0 なら止めている
static uint16_t crc_table[256];
static uint32_t rep_buf[REP_WORDS];      // パケットに組んだもの
static uint32_t rep_len;                 // rep_buf のワード数
static uint32_t rep_payload;             // その中のコマンドのワード数
static uint32_t rep_pwords;              // 1 パケットのコマンドのワード数 (最後だけ短い)

static void crc_init(void) {
    for (int i = 0; i < 256; i++) {
        uint16_t c = (uint16_t)(i << 8);
        for (int k = 0; k < 8; k++) c = (uint16_t)((c & 0x8000) ? (c << 1) ^ 0x1021 : c << 1);
        crc_table[i] = c;
    }
}

static inline uint16_t crc16_word(uint16_t c, uint32_t w) {
    for (int k = 0; k < 4; k++, w >>= 8) c = (uint16_t)((c << 8) ^ crc_table[(c >> 8) ^ (w & 0xFF)]);
    return c;
}

// パケットに組む (マジック、長さ、ワード、CRC。ワードは 2 バイトずれるので、隣のワードと半分ずつ合わせる)。dst に n + 2 ワード書く
static uint32_t hstx_pack(uint32_t *dst, const uint32_t *p, uint32_t n) {
    uint16_t crc = 0xFFFF;
    for (uint32_t i = 0; i < n; i++) crc = crc16_word(crc, p[i]);
    uint32_t k = 0;
    dst[k++] = HSTX_MAGIC;
    if (n == 0) {
        dst[k++] = (uint32_t)crc << 16;
        return k;
    }
    dst[k++] = n | p[0] << 16;
    for (uint32_t i = 1; i < n; i++) dst[k++] = p[i - 1] >> 16 | p[i] << 16;
    dst[k++] = p[n - 1] >> 16 | (uint32_t)crc << 16;
    return k;
}

static void hstx_off(void) {
    hstx_ctrl_hw->csr = 0;
    for (unsigned g = PIN_HSTX_D0; g <= PIN_HSTX_CLK; g++) release(g);
    hstx_hz = 0;
}

static void hstx_on(bool from_usb, uint32_t div) {
    if (from_usb) clock_configure_int_divider(clk_hstx, 0, CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB, 48000000, div);
    else          clock_configure_int_divider(clk_hstx, 0, CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS, clock_get_hz(clk_sys), div);
    unreset_block_num_wait_blocking(RESET_HSTX);
    hstx_ctrl_hw->csr = 0;
    // 出力のビット n は GPIO 12 + n。D0〜D3 (GP12〜15) はシフトレジスタの下位 4 ビット (前半も後半も同じ = 1 サイクルに 1 ニブル)、CLK (GP16) はクロック
    for (unsigned i = 0; i < 4; i++)
        hstx_ctrl_hw->bit[PIN_HSTX_D0 - 12 + i] = i << HSTX_CTRL_BIT0_SEL_P_LSB | i << HSTX_CTRL_BIT0_SEL_N_LSB;
    hstx_ctrl_hw->bit[PIN_HSTX_CLK - 12] = HSTX_CTRL_BIT0_CLK_BITS;
    // 4 ビットずつ 8 回で 1 ワード。クロックは 2 サイクルで 1 周期、ニブルの真ん中に立ち上がり・立ち下がり (CLKPHASE 1)
    hstx_ctrl_hw->csr = HSTX_CTRL_CSR_EN_BITS | 2u << HSTX_CTRL_CSR_CLKDIV_LSB | 1u << HSTX_CTRL_CSR_CLKPHASE_LSB |
                        8u << HSTX_CTRL_CSR_N_SHIFTS_LSB | 4u << HSTX_CTRL_CSR_SHIFT_LSB;
    for (unsigned g = PIN_HSTX_D0; g <= PIN_HSTX_CLK; g++) gpio_set_function(g, GPIO_FUNC_HSTX);
    hstx_hz = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_HSTX) * 1000;   // 測った値 (設定がそのとおりか確かめる)
}

static inline void hstx_put(uint32_t w) {
    while (hstx_fifo_hw->stat & HSTX_FIFO_STAT_FULL_BITS) tight_loop_contents();
    hstx_fifo_hw->fifo = w;
}

static void hstx_wait_empty(void) {
    while (!(hstx_fifo_hw->stat & HSTX_FIFO_STAT_EMPTY_BITS)) tight_loop_contents();
    busy_wait_us(1);                     // シフトレジスタの最後のワード
}

// GPU の FIFO の空きが need ワード以上になるまで待つ。dma >= 0 なら、その DMA がまだ送っていないワードも足す (読むたびに計り直す)。
// SPI のピンは呼ぶ側で on にしておく。期限切れなら false
static int hstx_last_free;              // 失敗したときの報告用 (最後に読んだ空き。-1 は同期バイトが違った)

static bool hstx_wait_free(uint8_t free_reg, uint32_t need, int dma, uint32_t *waits) {
    absolute_time_t until = make_timeout_time_us(2000000);
    while (true) {
        // 送り終えていない DMA のワードもまだ GPU の FIFO に入っていない (TRANS_COUNT の上の 4 ビットは MODE)
        uint32_t inflight = (dma >= 0 && dma_channel_is_busy((uint)dma)) ? dma_hw->ch[dma].transfer_count & 0x0FFFFFFFu : 0;
        int fr = spi_read16(free_reg);
        hstx_last_free = fr;
        if (fr >= 0 && (uint32_t)fr >= need + inflight) return true;
        (*waits)++;
        if (time_reached(until)) return false;
    }
}

static void cmd_hstx(const char *arg) {
    char *a_div = strtok(NULL, " \t");
    long div = a_div ? atol(a_div) : 0;
    if (arg && !strcmp(arg, "off")) {
        hstx_off();
    } else if (arg && (!strcmp(arg, "sys") || !strcmp(arg, "usb")) && div >= 1 && div <= 4) {
        if (!fpga_ready()) { reply_error("not ready: " READY_SIGNAL " is not toggling, nothing was driven"); return; }
        hstx_on(!strcmp(arg, "usb"), (uint32_t)div);
    } else {
        reply_error("hstx: <sys|usb> <div 1..4> | off");
        return;
    }
    printf("{\"ok\":true,\"clk_hstx\":%lu,\"bytes_per_s\":%lu}\n", (unsigned long)hstx_hz, (unsigned long)(hstx_hz / 2));
}

static void cmd_hstxw(char *arg) {
    static uint32_t buf[MAX_SPI_BYTES / 4], pkt[MAX_SPI_BYTES / 4 + 2];
    char *a_free = strtok(NULL, " \t");
    long n = arg ? atol(arg) : 0;
    if (n < 0 || n % 4 || n > MAX_SPI_BYTES || !a_free) { reply_error("hstxw: <n (multiple of 4, <= 4096)> <free>"); return; }
    if (!in_bytes((uint8_t *)buf, (size_t)n, 2000000)) { reply_error("hstxw: timeout waiting for data"); return; }
    if (!hstx_hz) { reply_error("hstxw: hstx is off"); return; }
    if (!fpga_ready()) { reply_error("not ready: " READY_SIGNAL " is not toggling, nothing was driven"); return; }
    uint32_t words = (uint32_t)n / 4, waits = 0;
    spi_pins_on();
    bool ok = hstx_wait_free((uint8_t)strtol(a_free, NULL, 0), words + HSTX_GUARD, -1, &waits);
    spi_pins_off();
    if (!ok) { reply_error("hstxw: FIFO stays full"); return; }
    uint32_t k = hstx_pack(pkt, buf, words);
    for (uint32_t i = 0; i < k; i++) hstx_put(pkt[i]);
    hstx_wait_empty();
    printf("{\"ok\":true,\"words\":%lu,\"waits\":%lu}\n", (unsigned long)words, (unsigned long)waits);
}

// 確かめ用: HSTX で words ワード (交互の模様) を流しながら、ピンを読んで CLK と D0〜D3 のエッジを数える
static void cmd_hstxprobe(const char *arg) {
    long words = arg ? atol(arg) : 64;
    char *a_pat = strtok(NULL, " \t");
    uint32_t pat = a_pat ? (uint32_t)strtoul(a_pat, NULL, 0) : 0;
    if (words < 1 || words > 4096) { reply_error("hstxprobe: <words 1..4096> [pattern]"); return; }
    if (!hstx_hz) { reply_error("hstxprobe: hstx is off"); return; }
    uint32_t edges[5] = {0}, prev = gpio_get_all(), pushed = 0, samples = 0, clk_high = 0, d0_high = 0;
    uint64_t end = time_us_64() + 100000;
    while (time_us_64() < end) {
        if (pushed < (uint32_t)words && !(hstx_fifo_hw->stat & HSTX_FIFO_STAT_FULL_BITS)) {
            hstx_fifo_hw->fifo = a_pat ? pat : (pushed & 1) ? 0x5A5A5A5Au : 0xA5A5A5A5u;
            pushed++;
        }
        uint32_t v = gpio_get_all(), ch = v ^ prev;
        samples++;
        clk_high += (v >> PIN_HSTX_CLK) & 1;
        d0_high += (v >> PIN_HSTX_D0) & 1;
        prev = v;
        for (unsigned i = 0; i < 5; i++) edges[i] += (ch >> (PIN_HSTX_D0 + i)) & 1;
    }
    printf("{\"ok\":true,\"pushed\":%lu,\"edges\":{\"D0\":%lu,\"D1\":%lu,\"D2\":%lu,\"D3\":%lu,\"CLK\":%lu},\"stat\":\"0x%08lx\"}\n",
           (unsigned long)pushed, (unsigned long)edges[0], (unsigned long)edges[1], (unsigned long)edges[2], (unsigned long)edges[3],
           (unsigned long)edges[4], (unsigned long)hstx_fifo_hw->stat);
    printf("# samples=%lu clk_high=%lu d0_high=%lu\n", (unsigned long)samples, (unsigned long)clk_high, (unsigned long)d0_high);
    printf("# csr=0x%08lx bit0=0x%08lx bit4=0x%08lx clk_hstx=%lu measured_khz=%lu reset=0x%08lx\n", (unsigned long)hstx_ctrl_hw->csr,
           (unsigned long)hstx_ctrl_hw->bit[0], (unsigned long)hstx_ctrl_hw->bit[4], (unsigned long)clock_get_hz(clk_hstx),
           (unsigned long)frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_HSTX), (unsigned long)resets_hw->reset);
}

static void cmd_hstxload(char *arg) {
    static uint32_t chunk[1024];
    char *a_p = strtok(NULL, " \t");
    long n = arg ? atol(arg) : 0;
    long pw = a_p ? atol(a_p) : 256;
    if (n < 4 || n % 4 || pw < 1 || pw > 1024) { reply_error("hstxload: <n (multiple of 4)> [words per packet 1..1024]"); return; }
    uint32_t words = (uint32_t)n / 4;
    uint32_t packets = (words + (uint32_t)pw - 1) / (uint32_t)pw;
    bool fits = words + 2 * packets <= REP_WORDS;
    rep_len = rep_payload = 0;
    rep_pwords = (uint32_t)pw;
    for (uint32_t left = words; left; ) {
        uint32_t k = left < (uint32_t)pw ? left : (uint32_t)pw;
        if (!in_bytes((uint8_t *)chunk, k * 4, 2000000)) { rep_len = 0; reply_error("hstxload: timeout waiting for data"); return; }
        if (fits) rep_len += hstx_pack(rep_buf + rep_len, chunk, k);
        rep_payload += k;
        left -= k;
    }
    if (!fits) { rep_len = rep_payload = 0; reply_error("hstxload: too large for the buffer"); return; }
    printf("{\"ok\":true,\"words\":%lu,\"packets\":%lu,\"buffer_words\":%lu}\n",
           (unsigned long)rep_payload, (unsigned long)packets, (unsigned long)rep_len);
}

static void cmd_hstxrep(char *arg) {
    char *a_free = strtok(NULL, " \t");
    long reps = arg ? atol(arg) : 0;
    if (reps < 1 || !a_free) { reply_error("hstxrep: <reps> <free>"); return; }
    if (!rep_len) { reply_error("hstxrep: nothing loaded (hstxload)"); return; }
    if (!hstx_hz) { reply_error("hstxrep: hstx is off"); return; }
    if (!fpga_ready()) { reply_error("not ready: " READY_SIGNAL " is not toggling, nothing was driven"); return; }
    uint8_t free_reg = (uint8_t)strtol(a_free, NULL, 0);
    int dma = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_dreq(&c, DREQ_HSTX);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    spi_pins_on();
    uint32_t waits = 0, packets = 0;
    const char *err = NULL;
    uint64_t t0 = time_us_64();
    for (long r = 0; r < reps && !err; r++) {
        for (uint32_t off = 0; off < rep_len; ) {
            uint32_t n = rep_buf[off + 1] & 0xFFFF;          // このパケットのワード数
            uint32_t k = n + 2;
            if (!hstx_wait_free(free_reg, n + HSTX_GUARD, dma, &waits)) { err = "hstxrep: FIFO stays full"; break; }
            dma_channel_wait_for_finish_blocking(dma);
            dma_channel_configure(dma, &c, &hstx_fifo_hw->fifo, rep_buf + off, k, true);
            off += k;
            packets++;
        }
    }
    dma_channel_wait_for_finish_blocking(dma);
    hstx_wait_empty();
    uint64_t us = time_us_64() - t0;
    spi_pins_off();
    dma_channel_unclaim(dma);
    if (err) {
        printf("{\"ok\":false,\"error\":\"%s\",\"last_free\":%d,\"packets\":%lu}\n", err, hstx_last_free, (unsigned long)packets);
        return;
    }
    uint64_t bytes = (uint64_t)rep_payload * 4 * (uint64_t)reps;
    printf("{\"ok\":true,\"bytes\":%llu,\"us\":%llu,\"kb_per_s\":%lu,\"packets\":%lu,\"waits\":%lu}\n",
           (unsigned long long)bytes, (unsigned long long)us, (unsigned long)(us ? bytes * 1000 / us : 0),
           (unsigned long)packets, (unsigned long)waits);
}
#endif

static void handle(char *line) {
    char *cmd = strtok(line, " \t");
    char *arg = strtok(NULL, " \t");
    if (!cmd) return;

    if (!strcmp(cmd, "ping")) {
        printf("{\"ok\":true,\"fw\":\"%s\",\"version\":%d,\"board\":\"%s\",\"harness\":\"%s\"}\n", FW_NAME, FW_VERSION,
               HARNESS_BOARD, HARNESS_REV);
    } else if (!strcmp(cmd, "status")) {
        cmd_status();
    } else if (!strcmp(cmd, "spib")) {
        cmd_spib(arg);
    } else if (!strcmp(cmd, "stream")) {
        cmd_stream(arg);
    } else if (!strcmp(cmd, "spi")) {
        cmd_spi(arg ? arg : "");
    } else if (!strcmp(cmd, "freq")) {
        long hz = arg ? atol(arg) : 0;
        if (hz < 10000 || hz > 16000000) { reply_error("freq: 10000..16000000"); return; }
        spi_hz = spi_set_baudrate(BRIDGE_SPI, (uint)hz);
        printf("{\"ok\":true,\"spi_hz\":%lu}\n", (unsigned long)spi_hz);
    } else if (!strcmp(cmd, "reset")) {
        // FPGA 側は入力専用 (pincheck で保証)。止まった FPGA を戻すためのものなので ready は見ない
        long ms = arg ? atol(arg) : 10;
        if (ms < 1 || ms > 5000) { reply_error("reset: 1..5000 ms"); return; }
        gpio_put(PIN_RESET, 1);
        gpio_set_dir(PIN_RESET, GPIO_OUT);
        sleep_ms((uint32_t)ms);
        release(PIN_RESET);
        printf("{\"ok\":true,\"ms\":%ld}\n", ms);
    } else if (!strcmp(cmd, "irqwait")) {
        long ms = arg ? atol(arg) : 100;
        if (ms < 1 || ms > 10000) { reply_error("irqwait: 1..10000 ms"); return; }
        uint32_t start_rises = edge_irq.rises;
        uint64_t start = time_us_64(), deadline = start + (uint64_t)ms * 1000;
        while (edge_irq.rises == start_rises && time_us_64() < deadline) tight_loop_contents();
        if (edge_irq.rises == start_rises) { reply_error("irqwait: timeout"); return; }
        printf("{\"ok\":true,\"waited_us\":%lu}\n", (unsigned long)(edge_irq.last_us - start));
    } else if (!strcmp(cmd, "wiretest")) {
        cmd_wiretest();
#ifdef PIN_HSTX_CLK
    } else if (!strcmp(cmd, "hstx")) {
        cmd_hstx(arg);
    } else if (!strcmp(cmd, "hstxw")) {
        cmd_hstxw(arg);
    } else if (!strcmp(cmd, "hstxprobe")) {
        cmd_hstxprobe(arg);
    } else if (!strcmp(cmd, "hstxload")) {
        cmd_hstxload(arg);
    } else if (!strcmp(cmd, "hstxrep")) {
        cmd_hstxrep(arg);
#endif
    } else if (!strcmp(cmd, "bootsel")) {
        printf("{\"ok\":true}\n");
        stdio_flush();
        sleep_ms(100);
        release_all();
        reset_usb_boot(0, 0);
    } else {
        reply_error("unknown command");
    }
}

int main(void) {
    release_all();
    stdio_init_all();
    spi_hz = spi_init(BRIDGE_SPI, 1000 * 1000);   // mode 0, 8bit, MSB first
    monitor_enable();
#ifdef PIN_HSTX_CLK
    crc_init();
#endif

    static char line[LINE_MAX];
    bool overflow;
    while (true) {
        in_line(line, sizeof line, &overflow);
        if (overflow) reply_error("line too long");
        else          handle(line);
        stdio_flush();
    }
}
