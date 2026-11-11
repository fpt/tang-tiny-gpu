# ハーネス v2: Pico 2 (Pico ピン配置) ⇔ Tang Nano 20K

本番のホストリンク (HSTX 4 データ + CLK) をそのまま載せたハーネス。戻りは v1 と同じ SPI と IRQ/READY、VSYNC、RESET。
M9 までのハーネスで使わなかった線 (ERROR、LED0〜5、OLED) と、VSYNC と同じ情報しか持たない HEARTBEAT は省いた。
**40 ピン RGB LCD (LCD_INIT の 15〜18 も)、I2S (MAX98357A)、HDMI の映像のピンは使わない**ので、ハーネスのまま LCD と音を出せる。
HDMI からは DDC_CLK (52) だけを借りる (EDID は読まない)。ジャンパはない。

- ホスト: **Raspberry Pi Pico 2** (RP2350A、PSRAM なし。リンクと翻訳層の検証には足りる)。同じ Pico ピン配置の **Pico Plus 2** (RP2350B、PSRAM 8MB、CS = GPIO 47) と
  **Pico LiPo 2** (同) にもそのまま載る。RP2350-PiZero は入手できなくなったので使わない
- 回路図: [`harness/kicad/v2/rp-tang-harness-v2.kicad_sch`](../harness/kicad/v2/rp-tang-harness-v2.kicad_sch) (基板は同じ場所の `rp-tang-harness-v2.kicad_pcb`)
- 配線表: [`harness/kicad/v2/pins_v2.json`](../harness/kicad/v2/pins_v2.json) (v1 と同じ形式。信号名も v1 に合わせてあるので `gen_pins_h.py` がそのまま使える)
- 検査: `make kicad-check` で、基板 (`.kicad_pcb`) をこの配線表と比べ、配線のつながり・短絡・間隔と、発注したガーバー (`harness/kicad/v2/gerber/`) を確かめる (`scripts/kicad_check.py`)

## 1. 配線表 (12 本 + GND)

| 信号 | 向き | RP2350 GPIO | Pico 物理ピン | FPGA ピン | FPGA 側の共有 | 備考 |
|---|---|---|---|---|---|---|
| HSTX_D0 | P → F | GP12 (HSTX bit0) | 16 | 71 | なし | 直列 33R (R4) |
| HSTX_D1 | P → F | GP13 (bit1) | 17 | 72 | なし | 直列 33R (R5) |
| HSTX_D2 | P → F | GP14 (bit2) | 19 | 73 | なし | 直列 33R (R6) |
| HSTX_D3 | P → F | GP15 (bit3) | 20 | 74 | なし | 直列 33R (R7) |
| HSTX_CLK | P → F | GP16 (bit4) | 21 | 80 (GCLKT_0) | SD D2、10k プルアップ | 直列 33R (R8) |
| SCK | P → F | GP10 (SPI1 SCK) | 14 | 20 | LED5 | LED で 3.3V につながり、離すと High (2.4) |
| COPI | P → F | GP11 (SPI1 TX) | 15 | 19 | LED4 | 同上 |
| CIPO | F → P | GP8 (SPI1 RX) | 11 | 75 | BL616 MISO | v1 で実績 |
| CS | P → F | GP9 | 12 | 86 | BL616 SPI_DIR | v1 で実績。正論理、4.7k プルダウン (R9) |
| IRQ (/READY) | F → P | GP20 | 26 | 85 | SD D1、10k プルアップ | v1 と同じ意味 (CONTROL bit4 で READY)。未コンフィグ時 High |
| VSYNC | F → P | GP22 | 29 | 52 | HDMI DDC_CLK、2.2k プルアップ (3.3V) | 同期パルス中 High。ready の判定にも使う (§1.2)。未コンフィグ時 High |
| RESET | P → F | GP21 | 27 | 79 | WS2812 DIN | 4.7k プルダウン (R10) |
| GND | | | 3, 8, 13, 18, 23, 28, 33, 38 | GND | | 隣り合う GND を HSTX の線の間に |

**HSTX のビット割り当て**: bit0..3 = D0..D3 (GP12〜15、ピン 16/17/19/20、間の 18 が GND)、bit4 = CLK (GP16、ピン 21)。
Pico ピン配置では HSTX の 8 本が 16〜25 に連続して並ぶので、5 本がまとまる。GP17〜19 (ピン 22/24/25) は bit5..7 で、レーンを増やすときの予備。配線しない。

### 1.1 選んだ理由

- **FPGA 側**: LCD を使うとヘッダに出ているクロック入力は 80 (GCLKT_0) しか残らない。他の部品がつながっていないヘッダピンは 71〜74 の 4 本だけ。
  残りは共有先が無害なもの (19/20 の LED、85 の SD プルアップ、79 の WS2812、52 の DDC_CLK) と v1 で実績のある 75/86。
  76 は BL616 の JTAG MOSI につながっていて BL616 が駆動するので使わない。
  棚卸しの詳細は Tang Nano 20K 回路図 Rev 1.3 のヘッダ J5/J6
- **RP2350 側**: HSTX は GPIO 12〜19 固定。SPI1 は GPIO 8〜11 にまとめた。GP4/5 (Qw/ST の I2C になるボードがある) と GP0/1 (UART) は空けてある
- **正論理のまま** (v1 と同じ)。CS と RESET に外付け 4.7k プルダウン (RP2350-E9: 入力のピンが内蔵プルダウンで約 2V に留まる不具合。外付けは 8.2k 以下が要る)
- **IRQ と READY は 1 本** (v1 と同じく CONTROL bit4 で切り替え)。HSTX は片方向なので VBLANK のタイミングは VSYNC の線で取る
- **VSYNC は HDMI の DDC_CLK (52) と共有**。LCD_INIT (15〜18、ST7701 系のパネルの SPI 初期化) とぶつからないので、ジャンパが要らない。
  HDMI にモニタやキャプチャをつないだままでも、SDA (53) は触らないので High のままで、I2C の開始条件ができず EDID の EEPROM は応答しない (EEPROM は SCL を駆動しない)

### 1.2 HEARTBEAT をやめた理由

- v1 の HEARTBEAT はフレームの先頭ごとに反転するだけ (`rtl/gowin/top.sv`) で、VSYNC と同じ情報しか持たない。どちらも `video_timing` から作られ、RESET 中は止まる
  (リセットでカウンタが 0 に戻り、`vsync` は非アクティブのまま)
- ファームウェアの ready の判定 (`fpga_ready()`: 直近 100ms 以内にエッジがある) は VSYNC (60Hz) のエッジで見る。ハーネスの設計が入っていないと 52 は 60Hz で動かないので、
  「SPI の線を駆動してよいか」の判定の意味は変わらない
- v2 のファームウェアとテストで変えるところ: `fpga_ready()` を `edge_vs` に、`harness.py` の HEARTBEAT 30Hz の項目を VSYNC 60Hz に、
  RESET の項目は VSYNC の立ち上がりを数える。`top_harness_probe` のエコーの線が 1 本減る

## 2. 電気的な注意

- 全部 3.3V。レベル変換なし
- HSTX の 5 本は RP2350 側に直列 33R (R4〜R8)。最初 33R、波形を見て 0〜47R。15cm 程度のリボンまでなら 37.5MHz DDR で問題ない見込み。
  各 HSTX 線の隣に GND を 1 本置く (Pico の GND はピン 18 が D1 (17) と D2 (19) の間、23 が CLK (21) の 2 つ隣。間の 22 は予備の GP17)。75MHz DDR に上げるときは基板化する
- 85 と 80 の 10k プルアップ、19/20 の LED は RP2350 の出力でそのまま駆動できる
- 52 の 2.2k プルアップ (3.3V) は FPGA の出力でそのまま駆動できる。VSYNC はほとんどの時間 Low なので約 1.5mA 流れる (気になるなら極性を反転して High を既定にする)
- 電源: **GND だけ共通にして USB を 2 本**挿す。5V の線はない (Pico の VBUS と Tang の 5V はどちらもつながない)。3V3 同士もつながない
- PSRAM: Pico 2 にはない (リンクと翻訳層の検証には不要)。Pico Plus 2 / Pico LiPo 2 では CS = GPIO 47、SDK の `psram` 初期化に 47 を渡す
- DDC_CLK を VSYNC に使うので、ハーネス v2 をつないでいるあいだは EDID を読めない (今の VDP / 3D GPU は読んでいない)

### 2.1 基板の確かめ (`rp-tang-harness-v2.kicad_pcb` のネット)

- 12 本の信号は Pico と Tang の両方で表の物理ピンにつながっている (Tang の記号のピン名: 1 = 73、2 = 74、3 = 75、4 = 85、16 = 20、17 = 19、21 = 52、
  23 = 71、24 = 72、27 = 79、28 = 86、37 = 80。22 = 53 はつながない)。GND は Pico の 8 本と Tang の 20 / 26 / 39、両面に GND のベタ
- +3V3 は Pico の 36 (3V3 の出力) と C4 (0.1u) だけ。Tang の 3V3 とはつながっていない
- HSTX の配線長 (Pico → 33R → Tang): D0 17mm、D1 12mm、D2 61mm、D3 61mm、CLK 66mm。最大の差 54mm で約 0.35ns。37.5MHz DDR の 1 ビット (13ns) にも
  75MHz DDR (6.7ns) にも小さい。CLK が一番長いので、データはクロックより少し先に着く

### 2.2 v2 で動かすときに変えるもの

- **v2 が既定** (Makefile の `HARNESS ?= v2`)。配線表が `pins_v2.json`、ファームウェアが `pico2` / `rp2350-arm-s` (`harness/firmware/build_v2/`)、`pincheck` が v2 になる。
  v1 をつなぐときは `export HARNESS=v1` (`harness.py` が呼ぶ `make` にも効く)。`make bitstream` / `prog` / `prog-3d` / `flash` は
  v2 のピンの `vdp2` / `gpu3d2` (`top_v2.sv` + `constraints/tangnano20k_v2.cst`) を使う。v1 のピンの `vdp` / `gpu3d` は `pincheck` で止まる
  (71、19、20 を FPGA が駆動するため)。**フラッシュも v2 の組にしておく** (`make flash`)。v1 の組が残っていると、RECONFIG で v1 の 3D GPU が立ち上がり、
  Pico が SPI に使う 19 / 20 を LED として駆動する
- ファームウェア (済み): `pins.h` を `pins_v2.json` から作り、`PIN_HEARTBEAT` があるかどうかで v1 / v2 を分ける。v2 は `fpga_ready()` を VSYNC のエッジで、
  SPI1 (SCK の GPIO から決める)、`wiretest` は v2 用 (2.4)。HSTX の送り出し (`hstx*`、3 章)
- FPGA (済み): `top_v2.sv` は `top.sv` を包んでピンだけ v2 にする。HEARTBEAT、ERROR、LED4 / LED5 はピンに出さない (LED0〜3 は残す)。
  配線テストは `top_harness_probe_v2`。HSTX の受信 (71〜74、80。3 章)
- `harness.py` (済み): selftest はブリッジの `status` に HEARTBEAT がなければ VSYNC で見る (ready、RESET で止まること。300ms で約 18 回)

### 2.4 配線テスト (`wiretest`、実機で確かめた)

```bash
make prog-probe2                       # FPGA に top_harness_probe_v2 (pincheck 込み)
make harness-deploy                    # Pico 2 を BOOTSEL にしてから。HARNESS_RAM=1 なら RAM だけで動かす (フラッシュの中身は残る)
python3 tools/harness/harness.py wiretest
```

- 第 1 段階: FPGA → Pico の 3 本 (CIPO 13.2kHz、IRQ 6.6kHz、VSYNC 3.3kHz) の周波数と、Pico → FPGA の 9 本が離した状態のまま動かないこと
- 第 2 段階: Pico → FPGA の 9 本を 1 本ずつ立て、FPGA が返す番号 (1〜9。2 本以上なら 15) を VSYNC の Low / High の半周期ずつ {IRQ, CIPO} の 2 ビットで読む。
  何も立てていないときは番号が読めない (-1) こと
- 2026-11-10 に v2 の基板で 12 本とも通った (20 回続けて)
- 同じ日に v2 のピンの VDP / 3D GPU (`vdp2` / `gpu3d2`) で `selftest` と、2D (M5、M7、E1) と 3D (G1〜G5) のシナリオの FRAME_CRC が、
  配置の種 2 つ (既定と `SEED=2`) で 2 回ずつ全部一致した。SPI は 8MHz まで、RECONFIG での入れ替えも v2 のフラッシュの組で動く

確かめたときに分かったこと:

- **SCK (20) と COPI (19) は離すと High**。LED5 / LED4 が 3.3V につながっていて、Pico の内蔵プルダウンでは Low にならない (中間の電圧になって FPGA から 0 / 1 が揺れる)。
  `pins_v2.json` で `"idle": "high"` にし、ファームウェアは離すときにプルアップにする。HSTX_CLK (80) も SD の 10k で同じ。SPI のモード 0 では SCK は Low で待つので、
  本番で SPI を使うあいだは SCK を Low に駆動しておくか、CS が立っていないあいだの SCK を FPGA が見ないこと (今の `spi_slave` は CS で止まる)
- **RP2350-E9 は HSTX の線で起きる**。High を出したあと入力 + 内蔵プルダウンに戻すと約 2V に留まり、FPGA から High に見える (外付けのプルダウンがあるのは CS と RESET だけ)。
  離す前にいったん Low を出す。本番の HSTX は出力のままなので関係しない
- **80 (GCLKT_0) はデータの入力として読めない** (apicula / nextpnr: 専用の配線で失敗したあと一般の配線でつながるが、値が常に High に見えた)。
  FPGA から 80 を駆動すると Pico には届くので、線は正しい。クロックとして受ければ届く (`top_harness_probe_v2` は立ち上がりで反転する FF で受け、
  Pico は読むあいだ HSTX_CLK をトグルする)。本番の HSTX の受信も 80 はクロックとして使う
- VSYNC (52) は DDC_CLK と共有で HDMI のケーブルにもつながり、エッジがなまって揺れることがある。VSYNC で半周期を分けて読むときは、変わってから 10us を捨て、
  9 割以上の多数決にする (`read_code`)。VSYNC の立ち上がりで数えるもの (ready の判定など) は今のところ周波数どおり (3.3kHz) に数えている

### 2.3 再コンフィグ (VDP と 3D GPU の切り替え) に線は要らない

RECONFIG_N (ピン 9) は Tang Nano 20K のヘッダに出ていない (基板の TP1 だけ) が、**設計が自分のピン 9 を Low にすれば再コンフィグする**ので、
ハーネスから線を引かなくてよい。ホストは SPI で `RECONFIG` (0x7C) に `0xA5` を書くだけ (`harness.py reconfig`)。

- フラッシュに VDP を 0x000000、3D GPU を 0x100000 に置き、それぞれ相手の番地を「次のビットストリーム」にする (`gowin_pack --multiboot_address`、Makefile)
- `gowin_pack --reconfign_as_gpio` は**付けない**。付けるとピンが GPIO になり、Low にしても再コンフィグしない (実機で確かめた)。付けなくても設計の出力はピンを駆動できる
- 切り替えは約 1.7 秒 (VDP → 3D、3D → VDP とも)。`gowin_pack -c` (圧縮) は GW2A では効かない (同じファイル)。速くするならビットストリームのヘッダの読み込みの速さ
- 電源を入れると 0x000000 (VDP) から立ち上がる。切り替えたら SDRAM の中身 (テクスチャなど) はホストが送り直す

## 3. HSTX (動いている)

コマンドを RP2350 の HSTX で送る。レジスタの読み書きと、空きを確かめる `GPU_FREE` の読み出しは SPI のまま。

### 3.1 線と速さ

- RP2350: HSTX のビット 0〜3 (GP12〜15) がシフトレジスタの下位 4 ビット、ビット 4 (GP16) が作ったクロック。1 サイクルに 4 ビットずつ 8 回で 1 ワード
  (`SHIFT` 4、`N_SHIFTS` 8)。クロックは `CLKDIV` 2 / `CLKPHASE` 1 で、ニブルの真ん中に立ち上がり (下位) と立ち下がり (上位)。
  クロックは送るデータがあるときだけ動き、止まると Low
- **clk_hstx の分周は 2 ビットしかない** (1〜4)。元は clk_sys (150MHz) か pll_usb (48MHz)。1 サイクルに 1 ニブルなので clk_hstx / 2 バイト/秒:

| `--hstx` | clk_hstx | 上限 | 実測 (`hstxbench`、NOP 1MiB) |
| --- | --- | --- | --- |
| `usb4` (既定、立ち上げ用) | 12MHz | 6MB/s | 5.9MB/s |
| `usb2` | 24MHz | 12MB/s | 11.8MB/s |
| `sys4` | 37.5MHz | 18.75MB/s | 18.5MB/s |
| `sys3` | 50MHz | 25MB/s | 24.6MB/s |
| **`sys2`** | 75MHz | 37.5MB/s | **36.9MB/s** (エラーなし) |
| `sys1` | 150MHz | 75MB/s | CRC のエラーとあふれ (FPGA が取れない) |

- FPGA ([`hstx_rx.sv`](../rtl/vdp/hstx_rx.sv)): 80 (GCLKT_0) をクロックとして受け、立ち上がりで下位、立ち下がりで上位のニブルを取って立ち下がりでバイトにする。
  16 バイトの非同期 FIFO (グレイコードのポインタ、FF) で `clk_sys` へ。パケットを解析して、ワードをコマンド FIFO ([`gpu.sv`](../rtl/vdp/gpu.sv) の `ext_*`) に入れる。
  受信の数はレジスタ `0x60`〜 (SPECIFICATION.md)
- パケット: マジック `55 AA 5A A5`、長さ (ワード数、16bit)、ワード、CRC-16 (CCITT、初期値 FFFF)。全体で 4 バイトの倍数になる (8 + 4 × 長さ)
- 流し方: ブリッジがパケットを送る前に SPI で `GPU_FREE` を読み、「パケットのワード数 + まだ送っている途中の DMA の残り + 余裕 32」以上空いていれば送る。
  空きを読む SPI が遅いとパケットごとの待ちが律速になる (1MHz では 18MB/s で頭打ち。8MHz にする)。パケットは 128 ワード (VDP の FIFO 512 に 2 つと余裕が入る)

### 3.2 使い方

```bash
python3 tools/harness/harness.py hstxbench --hstx sys2                     # NOP を Pico の RAM から流して速さを測る
python3 tools/harness/harness.py scenario scenarios/m5_basic.json --link hstx --hstx sys2   # コマンドを HSTX で送る (USB の往復はパケットごと)
python3 tools/harness/harness.py hstxdemo city --hstx sys2 --frames 10 --reps 6            # G5 の場面を Pico の RAM から流して fps
```

G5 の場面を Pico の RAM から HSTX (`sys2`) で流したとき (`hstxdemo`、10 フレームを 12 回):

| 場面 | SPI (USB から) | HSTX (Pico の RAM から) | そのときのリンク |
| --- | --- | --- | --- |
| logo | 33.5fps | **60.7fps** | 743KB/s |
| tunnel | 47.3 | **60.8** | 502KB/s |
| room | 49.8 | **60.9** | 454KB/s |
| city | 29.8 | **46.5** (GPU が律速) | 622KB/s |

ブリッジのコマンドは `hstx <sys|usb> <div>` / `hstx off`、`hstxw`、`hstxload`、`hstxrep`、`hstxprobe` (ピンのエッジを数える確かめ用)。`harness/firmware/src/main.c` の先頭。

### 3.3 立ち上げで分かったこと

- **clk_hstx の分周は 2 ビット**: `clock_configure_int_divider(clk_hstx, …, 255)` は 3 になり 50MHz で動いた。`hstx` は周波数を測って返す
- **立ち下がりの FF の非同期クリアで、書き込みのポインタが進まなかった** (ARCHITECTURE.md の 10 章)。FIFO のポインタはリセットしない
- HSTX を有効にした直後の最初のパケットは 1 バイト欠けることがある (CLK の線は SD の 10k でプルアップされていて、有効にすると Low に落ちる)。
  欠けたパケットは捨てられ、次のパケットのマジックで同期し直す。`0x60` に書くと解析もやり直す
- 本番の速さで HSTX が効くのは RP2350 がコマンドを作るとき。PC から USB で送る今のハーネスでは USB (約 900KB/s) が律速で、`--link hstx` はむしろ遅い
  (パケットごとに USB の往復がある)

## 4. v1 からの変更

| | v1 (RP2040-Zero) | v2 (Pico 2) |
|---|---|---|
| 本線 | SPI 8MHz | HSTX 4 + CLK (37.5MHz DDR、実測 36.9MB/s)。SPI はレジスタの読み書きと空きの確かめに残る |
| SPI の GPIO | SPI0 GP2〜5 | SPI1 GPIO 8〜11 (`pins.h` の番号と SPI インスタンスだけ) |
| 戻り線 | IRQ, VSYNC, HEARTBEAT, ERROR, LED0〜5 | IRQ, VSYNC (HEARTBEAT、ERROR、LED は廃止。`wiretest` の表から外す。ready は VSYNC で判定) |
| LCD との衝突 | 41, 42, 48 (VSYNC/HEARTBEAT/ERROR) | なし (LCD_INIT の 15〜18 も空いている) |
| OLED | I2C | 廃止 |
| 電源 | 別々の USB | 別々の USB (5V の線はない) |

## 5. 完成形に向けて (ホストのモジュールの選択)

同じ Pico ピン配置で、ハーネス v2 の配線のまま載せ替えられるものと、ベースボードを起こすときの選択肢。

| | GPIO | PSRAM | 固定の方法 | 向き |
|---|---|---|---|---|
| Pico 2 | 0〜22, 26〜28 | なし | ピンヘッダ / ソケット | ハーネス (今) |
| Pico Plus 2 | 同上 (+ 30〜47 は小さいコネクタ) | 8MB (CS 47) | ピンヘッダ / ソケット、キャステレーションをハンダ | ハーネス v2 の配線のままの最短の完成形 |
| Pico LiPo 2 | 同上 | 8MB (CS 47) | キャステレーションをベースボードに直接ハンダ (クリップは仮止め向き) | 電池を使うとき |
| PGA2350 | 0〜47 全部 (7x7 の PGA) | 8MB (CS 47) | 2.54mm のソケット (丸ピン) | 取り外せる完成形。USB-C、BOOTSEL / RESET のボタンをベースボードに持つ |

ベースボードを起こすなら PGA2350 を勧める。40 ピン FPC (LCD)、バックライトの昇圧、Tang Nano 20K のソケット、HSTX 5 本 + SPI 4 本 + 戻り 4 本、
に USB-C とボタン 2 つが増えるだけで、48 本の GPIO が全部使える (HSTX 8 レーン、音、ボタン、SD)。
ハーネス v2 の配線表 (GPIO 番号) はそのまま使えるので、ファームウェアは変わらない。
