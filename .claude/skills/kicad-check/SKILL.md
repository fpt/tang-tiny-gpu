---
name: kicad-check
description: ハーネスの KiCad の基板 (harness/kicad/) を配線表 (pins_v2.json) と比べ、配線のつながり・短絡・間隔、発注したガーバーまで検査する。KiCad の回路図や基板を直したあと、ガーバーを発注する前、届いた基板を確かめるとき、「基板を見て」「KiCad をチェックして」と言われたときに使う。
---

# KiCad の基板の検査

`scripts/kicad_check.py` (標準ライブラリだけ) が `.kicad_pcb` とガーバーを読んで確かめる。KiCad 本体は要らない。

## 実行

```bash
make kicad-check                                         # v2 の基板 + pins_v2.json (+ 隣の gerber/ があればそれも)
make kicad-check KICAD_GERBERS=harness/kicad/v2/gerber   # ガーバーのディレクトリを指定
make kicad-check KICAD_PCB=... KICAD_PINS=              # 配線表なし (つながりと短絡だけ)
```

Docker が動いていなければ、ホストの python3 で同じことができる (ファイルを読むだけ):
`python3 scripts/kicad_check.py harness/kicad/v2/rp-tang-harness-v2.kicad_pcb --pins harness/kicad/v2/pins_v2.json --gerbers <dir>`

KiCad で保存したあとに走らせる。ガーバーは KiCad 10 の既定の出力 (X2 の属性 `%TO.N` / `%TO.P` 付き) を前提にする。
属性のないガーバー (古い形式、ほかのツール) ではネットが分からないので、ガーバーの検査は意味がない。

## 何を見るか

| 区分 | 内容 |
|---|---|
| 配線表 (`--pins`) | 信号ごとに Pico の物理ピンと Tang のピン (パッドの pinfunction の先頭の数字 = FPGA のピン) が同じネットか。`series_r` は直列抵抗 (値も)、`pulldown` は GND への抵抗 (値も)。ほかの Pico / Tang のピンが同じネットにないか。`gnd_pico_pins` と Tang の GND がすべて GND か。Pico (VBUS / VSYS / 3V3) と Tang (3V3 / 5V) の電源をつないでいないか |
| `pcb:` | ネットごとに、パッドが銅 (配線、ビア、パッド、ゾーンの塗り) で 1 つの島か。パッドに届かない銅。違うネットの銅が触れていないか (短絡)、clearance (`.kicad_pro` の Default のネットクラス) より近くないか |
| `gerber:` | 発注したファイルで同じことをやり直す。パッドのネットが `.kicad_pcb` と同じか (違えばガーバーが古い) |

出力は `OK` / `WARN` / `FAIL` の行と、最後に `PASS:` か `FAIL:`。エラーがあれば終了コード 1。

## 結果の読み方

- `FAIL ... is split into N islands` — 配線が切れている (未配線)。島ごとのパッドが出るので、その間をつなぐ
- `FAIL short A <-> B` — 違うネットの銅が触れている。場所 (KiCad の座標、mm) が出る
- `FAIL clearance` — 触れてはいないが近すぎる。製造で短絡しうる
- `FAIL gerbers differ from the PCB` — ガーバーと `.kicad_pcb` が食い違う。ガーバーを出し直すか、どちらが発注したものかを確かめる
- `WARN ... single pad` — 回路図のラベルが 1 つしか残っていない (消し忘れのラベルや、片側だけ配線したネット)。意図したものか確かめる
- `WARN ... copper connected to no pad` — どこにもつながらない配線のかけら
- `INFO net ... is not in the pins file` — 配線表にないネット (パスコンなど)。問題ではない

Pico のフットプリントはピンごとにスルーホールとキャステレーションの 2 つのパッドを持ち、未接続のピンでは KiCad が別々のネット
(`unconnected-(...)` と `..._1`) を付ける。同じピンの 2 つのパッドは同じものとして扱うので、短絡としては出ない。

## 見ないもの (KiCad の DRC で確かめる)

基板の外形との間隔、穴と銅の間隔、サーマルのスポーク、ゾーンの塗りが最新か (`.kicad_pcb` は保存時の塗りを読む)、シルク、
ネットのない銅の図形 (数だけ WARN)。実物の基板の断線や短絡はテスターかハーネスの `wiretest` で確かめる。

## 直したら

ピン割り当てを変えたときは `harness/kicad/v2/pins_v2.json` と `docs/HARNESS2.md` の配線表を同じ変更にし、`make kicad-check` が PASS になることを確かめる。
