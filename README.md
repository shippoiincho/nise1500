# 偽 1500 for MZ-700

![nise1500](/pictures/nise1500.jpg)

## これはなに？

MZ-700 を MZ-1500 もどきにします。
本体の拡張コネクタに装着します。
映像は VGA コネクタから、音声は 3.5mm Jack から出力されます。
QuickDisk は読み込み専用でサポートされています。

偽 RAMFILE の以下の機能もありますが、フラシュサイズの関係で一部容量が削減されています。

- MZ-1R18 RAMFILE
- MZ-1R23 漢字 ROM
- MZ-1R24 辞書 ROM
- MZ-1R12 SRAM メモリ
- PIO-3034 EMM
- PCG-700

---
## 回路

![board](/pictures/board.jpg)

Z80 Bus に直結するために、
ピンの多い RP2350B のボード(WeAct RP2350B Core Board)を使用します。
カートリッジスロットの信号を、単純に Raspberry Pi Pico2 に接続しただけです。
Pico2 が 5V 耐性なのを良いことに直結しています。

```
GP0-7:  D0-7
GP8-23: A0-15
GP24: EXINT
GP25: RESET
GP26: M1
GP28: MERQ
GP29: IORQ
GP30: WR
GP31: RD
GP32: EXWAIT
VBUS: 5V
GND: GND
```

![Schematics](/pictures/schematics.png)

付属の基板データ `GERBER-nise1500.zip` を用いた場合、MZ-700 の背面コネクタに直結する形を想定しています。
延長ケーブルを使う際はコネクタの装着方向に注意してください。
3.5mm Jack は[ST-005-G](https://akizukidenshi.com/catalog/g/g102460/)を使っています。

MZ-700 の拡張コネクタには電源が出ていません。
別途 Pico の USB 端子などに電源を接続する必要があります。

---
## バイナリとか

`nise1500.uf2` をフラッシュに書き込んでください。
PCG書き込み時にウェイトが入るように調整していますが、環境によってはただしく動作しないことがあります。
BASIC 上で `CLS 2` を実行して正しく画面消去されるかどうか確認してください。

```
INIT "CRT:G"
PAINT [7]0,0
CLS 2
```

これで画面にゴミが残るようであれば、PCG ウェイトが正しく動作していません。

ソース上の `#define PCGWAIT` をコメントアウトすると PCG 書き込みのウェイトがなくなり安定動作しますが、
ソフトによっては速度が速すぎて不具合が生じることがあります。
`nise1500_nopcgwait.uf2` はウェイトオフで作成していますので、こちらを書き込んでください。

---
## 拡張ROM

MZ-1500 の拡張 ROM が必要です。実機から入手してください。

```
picotool.exe load -v -x ext.rom  -t bin -o 0x1001d000
```

---
## QD

QD のイメージ(QDF) ファイルは LittleFS 上に配置します。
LittleFS 領域は 8MiB なので、以下のように作成します。

```
mklittlefs -b 4096 -s 8388608 -c ./nise1500 nise1500.img
```

作った LittleFS イメージを Pico の Flash に書き込みます。

```
picotool.exe load -v -x nise1500.img -t bin -o 0x10800000
```


なお、QuickDisk は読み込み専用です。書き込みはできません。

LittleFS の取り扱いについては、詳しくは[こちらの記事](https://shippoiincho.github.io/posts/39/)を見てください。

QD ファイルの選択は SW1/SW2 を使います。
VGA 画面の一番下(26行目)に選択したファイル名が一定時間表示されます。

---
## MZ-1R18 RAMFILE

64KB の RAM FILE のエミュレーションをします。
BASIC から INIT "RAM:" などで使用することができます。

---
## MZ-1R23 漢字ROM

漢字ROM のエミュレートをします。
あらかじめ picotool などで漢字 ROM のデータを書き込んでおく必要があります。

```
picotool.exe load -v -x kanji.rom  -t bin -o 0x10020000
```

---
## MZ-1R24 辞書ROM

同じく辞書ROM のエミュレートをします。
あらかじめ picotool などで辞書 ROM のデータを書き込んでおく必要があります。

```
picotool.exe load -v -x dict.rom  -t bin -o 0x10040000
```

---
## MZ-1R12 SRAM メモリ

32KB の SRAM メモリのエミュレーションをします。
電源投入時は Checksum Error で止まるようにしています。

フラッシュとのやり取りは、I/O ポートの $BA,$BB を通して行えます。
内部的には 0-63 のバックアップスロットがあって、リード・ライトを制御できます。

$BA に出力するとその番号のスロットから SRAM に読み込みます。

```
OUT@ $BA,1
```

このあとリセットすると、SRAM から起動できます。

$BB に出力すると、SRAM の内容をその番号のスロットに書き込みます。

```
OUT@ $BB,2
```

なお、書き込み中は VGA 出力が停止します。

スロットに書き込みを行っていないデータは電源を切ると消滅します。

基本的な使い方は、モニタの ES コマンドでテープのソフトを SRAM に保存したのち、BASIC 上で上記コマンドで書き込みを行うことになるでしょう。

なお、エミュレータなどで作成したデータを記録することもできます。
アドレス 0x10080000 から 32KiB 単位でおいてください。
最初(0番)のデータは以下のように書き込みます。

```
picotool.exe load -v -x MZboot.rom  -t bin -o 0x10080000
```

起動できるデータの作成を PC 上でするには、
yanataka60さんの[MZ-1500SD 付属のツール 1Z-1R12_Header](https://github.com/yanataka60/MZ-1500_SD)
を使用すると便利です。

---
## PIO-3034 EMM

320KB の EMM をエミュレートします。
Hu-BASIC などで使用することができます。

こちらも I/O ポートの $BC,$BD で読み書きの制御が可能です。
偽 RAMFILE と違ってスロットは 0 - 15 しかありません。

$BC にスロット番号を出力すると EMM に読み込みます。
HuBASIC では以下のように操作します。

```
OUT &HBC,1
```

$BD にスロット番号を出力すると、EMM の内容をフラッシュに書き込みます。

```
OUT &HBD,2
```

なお、書き込み中は VGA 出力が停止します。

フラッシュに書き込みを行っていないデータは電源を切ると消滅します。

---
## PCG-700

VGA 出力に PCG-700 のエミュレーションをします。
本体出力との同期をとっていませんので、ソフトウェアによっては正しく描画されないことがあります。

本来 PCG-700 は、バンク切り替えに関係なく反応しますが、
MZ-1500 用ソフトの動作に支障があるために、VRAM 選択時にのみ動作します。

なお、あらかじめ本体の CG-ROM の内容をフラッシュに書き込んでおく必要があります。

```
picotool.exe load -v -x font.rom  -t bin -o 0x1001f000
```

---
## 制限事項

- ~~MZ-700 に存在しない PCG-RAM のアクセス時にウェイトが入りません。いくつかのソフトで MZ-1500 実機より早くなります~~
- QD は読み込み専用です
- ソフトによっては本体のスピーカーからノイズが発生します。本体のスピーカーの音量調節で対応してください。

---
## 使用ライブラリなど

- [LittleFS](https://github.com/littlefs-project/littlefs)
- [VGA ライブラリ(一部改変)](https://github.com/vha3/Hunter-Adams-RP2040-Demos/tree/master/VGA_Graphics)