# アーキテクチャ

Tang Nano 20K を GPU 本体、RP2350 をホスト CPU とする構成の設計と、今の実装の姿をまとめます。
ホストから見た使い方 (レジスタ、コマンド、データの形式) は [SPECIFICATION.md](SPECIFICATION.md) (VDP) と [SPECIFICATION_3D.md](SPECIFICATION_3D.md) (3D GPU)、
ハーネス (検証用の RP2 + HDMI キャプチャ) は [HARNESS.md](HARNESS.md) と [HARNESS2.md](HARNESS2.md) にあります。

## 1. 方針

1. **通信方式より先に「ホストから GPU がどう見えるか」(レジスタとコマンド) を決める**
2. **SPI 受信と GPU 本体を直接結びつけない。** SPI は「レジスタバスへの変換器」として扱い、リンクを HSTX などに替えても GPU 側は触らない
3. **パッケージ内 SDRAM にフレームバッファを置き、コマンドで描く。** 図形・BLIT・三角形を GPU が SDRAM に描き、表示回路が読み出して出す
4. **高速な処理は FPGA 内に閉じ込め、外部との境界は低速にする。** ホストは描画せず、32bit ワードのコマンド列を送るだけ
5. **2D と 3D は別のビットストリーム。** 1 つの FPGA に同居させず、フラッシュに 2 つ置いて実行時に入れ替える (2 章)
6. **画は 3 通りで突き合わせる。** 同じシナリオ (コマンド列) から Python の参照モデル・Verilator・実機を動かし、フレームの CRC で比べる (9 章)

## 2. 2 つのビットストリーム

| | VDP | 3D GPU |
| --- | --- | --- |
| 作り方 | `make bitstream-vdp` (`top.sv` の `GPU3D = 0`) | `make bitstream-gpu3d` (`GPU3D = 1`) |
| フラッシュ | 0x000000 (電源を入れるとこれ) | 0x100000 |
| `VERSION` (0x7F) | bit7 = 0 (0x01) | bit7 = 1 (0x81) |
| 描画 | コマンド GPU (2D 全部) | コマンド GPU (2D の一部) + `rtl/gpu3d/` |
| 表示 | `line_compositor` (面・スプライト・Mode 7・ライトマップ) | `fb_line` (ビットマップ RGB565 だけ) |

- 共通: SPI スレーブ、レジスタ、SDRAM のコントローラと調停、コマンド FIFO、`FRAME_CRC`、DVI の出力
- **切り替え**: それぞれ相手の番地を multiboot の「次」にして書く (`gowin_pack --multiboot_address`)。ホストが `RECONFIG` (0x7C) に `0xA5` を書くと、
  `top.sv` が RECONFIG_N (ピン 9。基板では TP1 だけに出ている) を 256 クロック Low にし、FPGA がフラッシュの次のビットストリームを読み直す。約 1.7 秒
- `gowin_pack --reconfign_as_gpio` は付けない (付けるとピンが GPIO になり、Low にしても再コンフィグしない)。`gowin_pack -c` (圧縮) は GW2A では何も変わらない

## 3. 全体像

```text
ホスト ──SPI──► spi_slave ──► レジスタバス ──┬─► vdp_regs  (ID, VERSION, STATUS, CONTROL, FRAME_CRC, RECONFIG …)
                                            ├─► mem_test  (SDRAM の BIST、1 ワード読み書き、クロックの位相。3D GPU は BIST なし)
                                            └─► gpu       (コマンド FIFO → 2D のコマンド、VRAM / パレット / OAM の書き込み)
                                                  │          └─► raster3d (3D GPU だけ: tri_engine → tile_engine)
     sdram_ctrl ◄── sdram_arb ◄───────────────────┴──── 表示 (line_compositor または fb_line) → DVI → HDMI
                       ▲  (表示 > 音 > GPU > メモリテスト)
                       └── audio_out (VDP だけ: SDRAM の PCM のリング → I2S)
            パッケージ内 SDRAM 8MiB
```

## 4. クロック

| クロック | 周波数 | 作り方 | 中身 |
| --- | --- | --- | --- |
| `clk_ser` | 126MHz | rPLL | DVI のシリアライザ (OSER10) |
| `clk_pix` | 25.2MHz | CLKDIV /5 | 表示タイミング、DVI |
| `clk_sys` | 63MHz | 2 つ目の rPLL (27MHz x 7/3) | SPI スレーブ、レジスタ、SDRAM、GPU、ライン合成 |
| SDRAM クロック | 63MHz | 同じ rPLL の `CLKOUTP` を ODDR で反転 | パッケージ内 SDRAM。位相は `MEM_CLK` で約 1ns 刻み (既定 0 = 逆相) |

`clk_pix` と `clk_sys` の間は非同期として扱い、同期化 (2 段の FF、トグル) だけでまたぎます。

## 5. レジスタバス

GPU 本体は「レジスタバス」だけを外に見せます。バスを駆動する側 (今は [`spi_slave.sv`](../rtl/vdp/spi_slave.sv)) は差し替えられます。
`rtl/vdp/` と `rtl/gpu3d/` はベンダ非依存なので、ほかの FPGA にも持っていけます。

| 信号 | 向き (GPU から見て) | 内容 |
| --- | --- | --- |
| `reg_addr[6:0]` | in | レジスタアドレス |
| `reg_wdata[7:0]` | in | 書き込みデータ |
| `reg_we` | in | 1 クロックのパルスで書き込み |
| `reg_re` | in | 読み出した値を取り込んだとき 1 クロック (読むとクリアされるレジスタ用) |
| `reg_rdata[7:0]` | out | `reg_addr` の値 (組み合わせ回路) |

SPI のトランザクションの形式とレジスタの一覧は [SPECIFICATION.md](SPECIFICATION.md) の 2・3 章にあります。

## 6. SDRAM

GW2AR-18 のパッケージ内 SDRAM (64Mbit、2M ワード x 32bit、4 バンク x 2048 行 x 256 列) を使います。

- パッケージのピンではないので、cst の `IO_LOC` には apicula の [doc/sdram.md](https://github.com/YosysHQ/apicula/blob/master/doc/sdram.md) にある内部 I/O の名前 (`IOR11B` など) を書く
- コントローラ ([`sdram_ctrl.sv`](../rtl/vdp/sdram_ctrl.sv)): CL2。読み書きとも同じ行の中のバースト。書き込みはワードごとのバイトマスク (DQM) 付き。
  利用者は [`sdram_arb.sv`](../rtl/vdp/sdram_arb.sv) で調停する (表示 > 音 > GPU > メモリテスト)。GPU のバーストは 64 ワードまでなので、
  表示の読み出しが待たされても最大約 5.6us (締め切りは 63.5us)
- ワードアドレスは `{行, バンク, 列}`。1 ライン (320 ピクセル x 16bit = 160 ワード) を 1 行 (1KiB) に置くと、隣のラインは別のバンクになる
- メモリの配置は SPECIFICATION.md / SPECIFICATION_3D.md の「SDRAM の配置」

### 6.1 クロックの位相と取り込み位置

- 2 つ目の rPLL で `clk_sys` を作り、同じ PLL の `CLKOUTP` (位相をずらした 63MHz) を ODDR で反転して SDRAM のクロックにする。
  位相 (`PSDA`、1/16 周期 = 約 1ns 刻み) は `MEM_CLK` で実行時に変えられる (apicula は `DYN_DA_EN` に対応している)
- **`DUTYDA` は立ち下がりの絶対位置**。`PSDA` だけ変えると立ち上がりしか動かず、位相 8, 9 で両方のエッジが重なってクロックが潰れる。RTL で `DUTYDA = PSDA + 8`
- 読み出しデータの取り込み位置は半周期単位 (`MEM_CFG`、既定 5)。位相 x 取り込み位置の BIST (`harness.py memtest --phases 0-15`) で、
  既定 (位相 0、位置 5) から前に 4 段・後ろに 3 段 (約 4ns・3ns) の余裕がある。位相 6〜11 はコマンド・アドレスの保持時間が足りなくなる
- `harness.py selftest` が毎回この余裕を測り、どちらかが 2 段を切ったら不合格にする

## 7. VDP

### 7.1 コマンド GPU (`gpu.sv`)

コマンド FIFO (BSRAM) から 32bit ワードのコマンドを読み、SDRAM の描く面に描きます (`CLEAR` `FILL_RECT` `POINT` `DRAW_LINE` `BLIT` `BLIT4` `STRETCH_BLIT` `UPLOAD` …)。
`SWAP` で表示する面と描く面を入れ替え、`WAIT_LINE` / `WAIT_VBLANK` + `REG_WRITE` でラインごとの効果 (スクロール、背景色、ライトマップの切り替え) を作ります。
`VRAM_WRITE` `PAL_WRITE` `OAM_WRITE` `LIGHT_WRITE` は合成器の BSRAM に書きます。3D GPU のビットストリームでは `FULL2D = 0` で 8bpp・BLIT 系を外します。

### 7.2 ラインごとの合成 (`line_compositor.sv`)

フレームバッファを全部描き直すのではなく、論理 1 ライン (走査線 2 本 = 63MHz で約 4,000 クロック) ごとに作業ラインを合成し、そのまま DVI に出します。

- 層: 背景色 → 面 B / 面 A (タイル、または面 A の代わりに Mode 7) → ビットマップ (SDRAM、RGB565 か 8bpp) → スプライト (OAM 128 個、ライン上限) → ウィンドウ。
  順位付きの作業ラインで重ね、最後にライトマップ (8x8 のセルごとの RGB の倍率) と霧を掛ける
- VRAM (タイル + マップ 64KiB)、パレット 512 色、OAM、ライトマップ、ラインごとの表は BSRAM。タイルの取り出しは 1〜2 クロックなので、
  2 面 + スプライトで約 1,300 / 4,000 クロック (SDRAM の単発アクセスだと 1 回約 8 クロックで、4 面は苦しい)
- BSRAM は 44 / 46 (VDP のビットストリーム)。足すものは VRAM・SDRAM に置くか、ライトマップのセルを 10 x 8 にして 2 → 1 個にする

### 7.3 音 (`audio_out.sv`)

SDRAM の PCM のリングを読み、I2S で基板の MAX98357A (ピン 51/54/55/56) に出します。BSRAM は使いません。ホストは 1 フレーム分 (800 標本) を `UPLOAD` して `PCM_WR` を進めます。

## 8. 3D GPU (`rtl/gpu3d/`)

タイルベースの描画 (TBR)。`BEGIN_SCENE` から `END_SCENE` までの三角形をビニングし、`END_SCENE` で 32 x 32 のタイル (横 10 x 縦 8 = 80 個) ごとに
オンチップのタイルバッファで描いて書き出します。SDRAM へのランダムアクセスは三角形のレコードとテクスチャの外れだけになります。

遅延描画 (TBDR、PowerVR の ISP → TSP) ではありません。タイルの中で隠面消去を先に済ませてから見えるピクセルだけをシェーディングするのではなく、
三角形を投入順に描き、Z テストに通ったピクセルをその場でシェーディングします (Mali や Adreno と同じ TBR。8.3 節)。

```text
gpu.sv (TRI / BEGIN_SCENE / END_SCENE / SET_TEX / CLUT_WRITE / CLEAR_Z)
  └► raster3d ─┬► tri_engine  ビニング: tri_setup → レコード (24 ワード) をパラメータバッファへ、触れるタイルのリストに番号を追加
               └► tile_engine タイルのパス: タイルバッファを用意 → リストの三角形を投入順に描く → 書き出し
```

### 8.1 セットアップ (`tri_setup.sv`)

- 頂点の範囲 (ガードバンド) は **-512〜+831**。この範囲で 12.4 の頂点からのエッジ関数が 28bit に収まる。外にはみ出す三角形はホストがクリップする
- 三角形の外接矩形の左上 (原点) での辺の値 3 本と属性 6 個 (z, u, v, r, g, b) の値と勾配を計算する。**1 / 面積は正規化した形** (17bit の仮数 + シフト)。
  勾配 = (Δ属性 x エッジの係数) x 仮数 >> シフト
- 乗算は `smul36x18` (符号なしの DSP 2 つ + 符号の補正、2 クロックに 1 回)。セットアップは三角形 1 つ最大 134 クロック
- 結果は FF でなく SDP の BSRAM に置く (レコードと同じ番号で、tri_engine がそこから流す)
- 計算の規則 (上左の規則、切り捨て、Z の「小さいとき」) は [`tools/harness/raster.py`](../tools/harness/raster.py) が正。Verilator と実機はこれとピクセル単位で一致する

### 8.2 ビニング (`tri_engine.sv`)

- レコードはパラメータバッファ (ワードアドレス 0x20000 + 番号 x 32)、タイルのリストは 0x40000 + タイル x 1024 (1 項目 1 ワード)
- タイルごとの数はオンチップ (BSRAM)。1 シーン 4,096 個、1 タイル 1,024 個まで。超えた分は描かず `GPU_STATUS` のエラー
- ビニングとタイルのパスは重ねない (`END_SCENE` はタイルのパスが終わるまで待つ)。そのため `END_SCENE` のあとの 2D のコマンドは描き終わった面に効く
- シーンの外の `TRI` は 1 つだけのシーンとして描く (外接矩形のタイルだけ、描く面を読んで)

### 8.3 タイルのパス (`tile_engine.sv`)

1. タイルバッファ (色 1,024 x 16bit、Z 1,024 x 16bit、どちらも偶数・奇数に分けた DPX9B) を消すか、描く面と Z バッファから読む
2. リストの三角形を投入順に: レコードを読み、重なりの左上の値を `s + gx * dx + gy * dy` で作る (組ごとの積 24 個を 2 クロックに 1 つ。足し算は 2 段)。
   重なりを 1 クロックに 1 ピクセル走査する。行の左右の両端で負の辺があればその行を捨て、中のピクセルを出したあと外に出たら行を終える
3. 色 (と Z) を書き出す。書き出しは読み出しのポインタと 3 ワードの FIFO で先読みし、書いたワードのタイルバッファは次のタイルの値に戻す (消すクロックがかからない)

ピクセルの段:

| 段 | 内容 |
| --- | --- |
| A | 辺の判定、属性、テクセルのワードアドレス (1 クロック前に FF に)、キャッシュのタグ。外れなら A を止めて 1 行 (8 ワード) を読む |
| B | キャッシュのワードからテクセルを取り出す (16bpp はそのまま、8 / 4bpp は CLUT を引く) |
| C | テクセルの色が決まる。タイルの Z を読む |
| D | 透明 (テクセル 0)、Z を比べる、Gouraud の変調の部分積 |
| E | 部分積を足して飽和。タイルの色を読む (半透明で混ぜる相手) |
| F | Z を書く (半透明は書かない)。半透明の 4 式 |
| G | 色をタイルバッファに書く |

- ISP / TSP (隠面消去とシェーディング) には分けず、1 回の走査で Z もシェーディングもする。テクセルの取り出しは描いたピクセルの約 1.1 倍
- テクスチャのキャッシュ: 直接写像 64 行 x 8 ワード (データ BSRAM 1 個、タグ 12bit は SDP の BSRAM)。テクスチャは 8 ワードのかたまりの並び
  (16bpp 4 x 4、8bpp 4 x 8、4bpp 8 x 8) で、テクスチャの先頭はテクスチャのワード数の倍数 (アドレスを足し算でなく OR で作る)。シーンの始めに空にする
- CLUT は 256 エントリ 1 組 (`CLUT_WRITE` はすぐ効く。シーンの中では変えない)

### 8.4 資源 (nextpnr)

| | LUT4 (ALU の LUT 込み) | DFF | BSRAM |
| --- | --- | --- | --- |
| VDP | 10,313 | 4,794 | 44 / 46 |
| 3D GPU | 13,829 | 6,201 | 17 / 46 |

3D GPU は LUT より **DFF と制御集合** が先に詰まる (6,800 前後から配置が失敗する)。

## 9. 検証

- **参照モデル**: [`gpu_model.py`](../tools/harness/gpu_model.py) (コマンドの解釈、フレームバッファ、合成) と [`raster.py`](../tools/harness/raster.py) (三角形の整数の計算)。Python 標準ライブラリだけ
- **シナリオ**: コマンド列と check (`scenarios/`、3D は `scenarios/gpu3d/`)。`make scenario` が Verilator のシステム全体で走らせ、モデルの CRC と比べる。
  `harness.py scenario` が実機で走らせ、`FRAME_CRC` と HDMI キャプチャを比べる
- **モジュール**: `sim/tb_<名前>.sv` (Icarus Verilog と Verilator)
- **ネットリスト**: `scripts/bsram_check.py` (DP の BSRAM の番地が FF から直接か)、`scripts/pincheck.py` (ハーネスが駆動するピンを FPGA が駆動しないか)
- **実機の完了**: 2 つ以上の配置の種で、それぞれ 2 回

## 10. 実装で分かったこと (Gowin + オープンソースのツール)

CLAUDE.md の RTL の方針の根拠です。どれも Verilator では合い、実機だけで化けました。

- **符号付きの乗算を DSP に置かない**: yosys は `MULT18X18` の `ASIGN` / `BSIGN` を VCC につなぎ nextpnr も配線するが、実機は符号なしで掛けた値を出す。
  符号なしで掛けて補正する (`A * B = Au * Bu - (A < 0 ? Bu << 18 : 0) - (B < 0 ? Au << 18 : 0)`。[`mul_probe.sv`](../rtl/gpu3d/mul_probe.sv) と `harness.py mulprobe` で確かめた)。
  符号なしの乗算 (VDP のライトマップと霧の `MULT9X9`) は合う
- **LUT の RAM (`RAM16SDP4`) を使わない**: `tri_setup` の 4 x 36bit が LUT の RAM に置かれ、タイミングに余裕があっても三角形が崩れた。`synth_gowin -nolutram`
- **nextpnr の Gowin のタイミング解析には見ない経路がある**:
  - DSP (`MULT*`) の経路に遅れがない → DSP の乗算は 2 クロックかける (`smul36x18`)
  - 真のデュアルポートの BSRAM (`DPX9B`。幅が 32 未満の RAM はこれになる) の B ポートの番地 (`ADB`) のセットアップが別の種類のセルに登録されている
    (`gowin_arch_gen.py`) → DP の番地は両方のポートとも FF から直接
  - 配線のあとの解析が長い組み合わせの経路を落とすことがある (最小の再現は `make timing-repro`、`tools/repro/nextpnr_gowin_timing/`) →
    配置のあとの Max frequency も見る。長い桁上げの連鎖 (変調のシフトと加算 6 段、組ごとの足し算) も報告より遅いので段に分ける
- **長い組み合わせから同じクロックに複数の FF に取る値** (キャッシュが外れたときのアドレス・行・タグ) は、配置によって食い違う。1 段 FF に置いてから使う
- **yosys は組み合わせで読む小さい配列を BSRAM にし、番地の FF を取り込んで FF の D 入力 (論理) を番地にすることがある**。RTL を見ても分からないので
  `bsram_check.py` が合成したネットリストで確かめる (3D GPU は違反なら止まる)。小さい配列で困るなら `(* ram_style = "logic" *)`
- **yosys の `$buf`**: 合成のあとに残る `$buf` セルを `scripts/fix_netlist.py` が外す (E4 と K3 で出た)
- **立ち下がりで動く FF の非同期クリア (`DFFNC` / `DFFNCE`) で、書き込みのポインタが進まなかった** (HSTX の受信、止まることのある外のクロックの立ち下がり)。
  非同期のリセットを外したら動いた。クロックをまたぐ FIFO のポインタはリセットしない (コンフィグで 0 から始まり、両側が回り続ける)
- **データの入力として使う GCLKT のピン** (80) は、値が常に High に見えた (専用の配線が失敗し、一般の配線になる)。クロックとして受ければ届く (HARNESS2.md の 2.4)
- 配置の種 (`nextpnr --seed`) を変えて実機の結果が変わるなら、上のどれかを疑う

## 11. ホストとのリンク

| リンク | 速さ | 状態 |
| --- | --- | --- |
| SPI 8MHz (ハーネス v1、RP2040-Zero のブリッジ v3 の `stream`) | 約 764KB/s | 動いている |
| SPI 7.5MHz (ハーネス v2、Pico 2、`stream`) | 約 756KB/s | 動いている (既定) |
| HSTX のデータ 4 本 + CLK (ハーネス v2、clk_hstx 75MHz、クロック 37.5MHz で DDR) | **36.9MB/s** (実測。1 フレーム 620KiB) | 動いている ([HARNESS2.md](HARNESS2.md) の 3 章) |

- FPGA は SCK を `clk_sys` (63MHz) でサンプリングするので、SPI の上限は 10〜15MHz
- **HSTX**: パケット = マジック (32bit) + 長さ + ワードの列 (コマンド FIFO に入れる) + CRC16 ([`hstx_rx.sv`](../rtl/vdp/hstx_rx.sv))。
  受信側はバイトでマジックを探して同期する。ワードは CRC を見る前に FIFO に入れ、CRC の不一致は数えるだけ (再送はしない)。
  HSTX は送信専用なので、ホストは SPI で `GPU_FREE` を読んで空きを確かめてから送る
- **ホストの分け方**: 翻訳層 (エミュレータの VDP や Kessel のスクリーン → コマンド) とトランスポート (SPI / HSTX) の間は「32bit ワードを積む / フラッシュする / READY を待つ」だけ
- ホストの PC から USB で送る今のハーネスでは、USB (フルスピード、約 900KB/s) が律速。HSTX の速さが効くのは、RP2350 がコマンドを作るとき
  (本番) と、Pico の RAM に置いたものを流すとき (`harness.py hstxbench` / `hstxdemo`)

## 12. 段階の記録

RTL やツールのコメントにある段階の名前 (M / E / K / G) の意味です。

| 段階 | 内容 |
| --- | --- |
| M0 | ハーネスの RP2 ブリッジとホストツール (`harness.py`) |
| M1 | SPI スレーブとレジスタ |
| M2 | `FRAME_CRC` とキャプチャの比較 |
| M3 | SDRAM のコントローラと BIST |
| M4 | フレームバッファの表示 (SDRAM のバースト読み出し)、`UPLOAD` |
| M5 | コマンド FIFO と `CLEAR` `FILL_RECT` `SWAP`、SDRAM の書き込みのバースト、参照モデルとシナリオ |
| M6 | `DRAW_LINE` `POINT` `CLIP` |
| M7 | `BLIT` |
| M9 | 2D のデモ (`make demo`) とフラッシュへの書き込み (M8 は E2 で不要になった) |
| E1 | ラインごとの合成 (`line_compositor`) |
| E2 / E2.5 | タイル レイヤ (VRAM、パレット 512 色) / 順位付きの作業ライン |
| E3 | スプライト (OAM 128 個) |
| E4 | 面 B、ウィンドウ、横スクロールの表 (メガドラの VDP の状態の変換は `md.py`) |
| E7 | Mode 7 と霧 |
| K1 | 8bpp のビットマップと BLIT4 ([rs-kessel](https://github.com/fpt/rs-kessel) を載せるため) |
| K2 | スプライトのセル順 2 (行優先) とスプライトごとのバンク |
| K3 | `STRETCH_BLIT` |
| K4 | ライトマップ |
| K5a | 音 (I2S、SDRAM の PCM のリング) |
| G0 | 3D GPU のビットストリームの骨格 (`fb_line`)、DSP の確かめ (`mul_probe`) |
| G1 | 三角形のセットアップと走査、参照モデル (`raster.py`) |
| G2 | TBR (ビニングとタイルのパス) |
| G3 | テクスチャ (キャッシュ、CLUT、変調) |
| G4 | 半透明 |
| G5 | 3D の場面 (`demo3d.py`)、配置に強くする、テクスチャのキャッシュ 64 行、ビットストリームの切り替え |

## 13. rs-kessel を載せるとき

[rs-kessel](https://github.com/fpt/rs-kessel) (16bit スタック VM のファンタジーコンソール) を VDP で動かすための、ホスト側の規則です。VDP の機能 (K1〜K5a) はそろっています。

- **描画の深さを 3 段にする**: 面 (`map`、1 回目が面 B・2 回目が面 A、3 回目以降は BLIT4) < 即時描画 (ビットマップ: `cls` `pset` `rect` `spr` `spr_scaled` `text` …) <
  ハードスプライト (opt-in)。`BITMAP_RANK = 6` で、高優先のスプライトはビットマップの上、低優先は下
- **透過**: そのフレームに面がなければビットマップの透過を切る (色 0 も不透明に塗れる)。面があるフレームだけ、使われていないインデックスを `BITMAP_KEY` に選ぶ
- **光**: セル (8 x 8) 単位のライトマップ。ホストがセルの中心で計算して `LIGHT_WRITE`
- **同じ絵を出す規則** (定義は SPECIFICATION.md):
  - `spr_scaled` は `STRETCH_BLIT` の拾い方 (ピクセルの中心)。反転は描いた結果を鏡にする (rs-kessel#107 で合わせた)
  - `POINT` `DRAW_LINE` `BLIT` の座標は符号付き。`FILL_RECT` の x, y, w, h は符号なしなので、ホストが画面の中に切ってから出す
  - ライティングの桁 (VDP は RGB565 の成分に `min(最大値, (c * L) >> 4)`、L は 6bit) は Kessel 側が未決
  - `pal()` の RGB888 → 565 の丸めは、参照と VDP バックエンドで同じ関数にする
