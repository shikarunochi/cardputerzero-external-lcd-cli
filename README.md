# CardputerZero 外部LCD CLI

CardputerZeroの標準CLIを外部ST7789液晶で使うためのLinuxアプリです。公開ランチャーのソースから、端末のANSI処理、画面描画、スクロールバック、FreeTypeの描画コードを流用しています。画面全体を使う通常モードは **45列×16行** です。標準CLIと同じ12ピクセルのJetBrains Mono Boldと7×15ピクセルの文字セルを使用します。

## CardputerZeroでビルドする

展開したディレクトリに移動してください。Debian系のLinuxで必要なパッケージを入れて、LVGL 9.5.0を取得してからビルドします。

```sh
sudo apt update
sudo apt install build-essential cmake git pkg-config libfreetype-dev libevdev-dev libxkbcommon-dev xkb-data bash ncurses-term
git clone --depth 1 --branch v9.5.0 https://github.com/lvgl/lvgl.git lvgl
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
```

入手済みのLVGL 9.5.0を使う場合は、最初のcmakeコマンドに `-DLVGL_SOURCE_DIR=/path/to/lvgl` を追加できます。GPIOキャラクターデバイスABI v2を含むLinuxヘッダーが必要です。ランチャーSDK、libvterm、Pythonは不要です。

## 実行する

同じディレクトリにある `fonts/` と `panel.conf` を使います。キーボード入力が届く対話端末で実行してください。SSHでも対話TTYがあれば操作できます。

```sh
./build/cardputer-cli panel.conf
```

SPIとGPIOデバイスにアクセスできる一般ユーザーで実行してください。同じ外付け画面やDC/RSTを使っているプロセスは先に終了します。起動後に `stty size` と入力すると、通常は `16 45` と表示されます。bashで `exit` を入力すると元の端末へ戻ります。アプリを閉じるには `Ctrl+]` に続けて `q` を入力する方法もあります。

同梱の `ext5v auto` は、起動時にExtPortのExt5VをONにし、150 ms待ってから液晶を初期化します。DevTermと同じ `/sys/class/leds/ext_5v_out/brightness` で起動前の状態を読み取り、CLIがOFFからONにしたときだけ終了時にOFFへ戻します。起動前からONだった場合と、状態を読み取れなかった場合は、終了時にOFFを書き込みません。このファイルがない場合は、CardputerZeroと識別できる環境でGPIOを使い、取得したラインを終了時にLOWにして解放します。通常終了とSIGTERMなどで終了処理が動作します。SIGKILLや電源断では終了処理を実行できません。

外部電源などでCLIから給電を制御しない場合は、`panel.conf` に `ext5v off` を指定します。

- 通常時に `Ctrl+]` → `k` または `j` で履歴を上下に移動します。
- `./build/cardputer-cli --wide panel.conf` では80列×24行の仮想端末を使います。 `Ctrl+]` に続けて `h`、`j`、`k`、`l` を入力すると表示範囲を移動できます。
- `Ctrl+]` を2回入力すると、PTYへ文字として送ります。

## 外付けLCDの配線

CardputerZeroの **Ext5V（5 V）を降圧モジュールで3.3 Vに変換**し、その出力をST7789液晶の **3V3** に接続します。液晶の3V3端子へ5 Vを直接つながないでください。CardputerZero、降圧モジュール、液晶のGNDは共通にします。

| CardputerZero側 | 接続先 |
|---|---|
| Ext5V（5 V） | 降圧モジュールの入力＋。降圧後の3.3 V出力＋を液晶の3V3へ |
| GND | 降圧モジュールのGNDと液晶のGND |
| GPIO11（SPI0 SCLK） | 液晶のSCLK / SCK（モジュールによってはSCL） |
| GPIO10（SPI0 MOSI） | 液晶のMOSI / DIN（モジュールによってはSDA） |
| GPIO7（SPI0 CE1） | 液晶のCS |
| GPIO22 | 液晶のDC |
| GPIO26 | 液晶のRST / RES |

GPIO番号は物理ピン番号と区別してください。この構成はSPIの書き込み専用で、MISOは使いません。液晶のバックライトは常時点灯の構成です。

## パネル設定

| 項目 | 同梱設定 |
|---|---|
| 画面 | ST7789、320×240、RGB565 |
| SPI | /dev/spidev0.1、32 MHz、モード0、ハードウェアCS |
| GPIO | /dev/gpiochip0 のDC 22、RST 26 |
| 初期化 | st7789_test.py のリセット、待ち時間、横向き設定。色反転はOFF |

初期化や色反転を変更する場合は `panel.conf` を編集します。ライブラリはLVGLから更新範囲を受け取り、RGB565で転送します。

## 本体キーボードとランチャー登録

`--keyboard` を付けると、起動元のTTYに頼らず、本体キーボードのLinux入力イベントを直接読みます。ランチャー指定の入力デバイス、内蔵キーボードの固定パス、TCA8418のデバイス名の順で探します。手動で指定するときは `--keyboard=/dev/input/event3` のようにします。

ビルドが済んだら、展開先で次を実行します。同梱の登録ファイルは `Terminal=false` で、標準CLIを経由せず起動します。ランチャーは外部アプリの実行中、自動ロックを一時停止します。

```sh
sudo install -Dm755 build/cardputer-cli /opt/cardputer-cli/cardputer-cli
sudo install -Dm755 start-cardputer-cli /opt/cardputer-cli/start-cardputer-cli
sudo install -Dm644 panel.conf /opt/cardputer-cli/panel.conf
sudo install -Dm644 fonts/JetBrainsMono-Bold.ttf /opt/cardputer-cli/fonts/JetBrainsMono-Bold.ttf
sudo install -Dm644 cardputer-external-cli.png /usr/share/APPLaunch/share/images/cardputer-external-cli.png
sudo install -Dm644 cardputer-external-cli.desktop /usr/share/APPLaunch/applications/cardputer-external-cli.desktop
```

HOME画面の **External LCD CLI** を選んで起動し、bashで `exit` を入力するとHOMEへ戻ります。ランチャーがアプリを起動するユーザーに、キーボードの読み取りとSPI・GPIO・Ext5Vの操作権限が必要です。

HOMEに表示されない場合は、`/opt/cardputer-cli/start-cardputer-cli` に実行権限があり、`cardputer-external-cli.desktop` が `/usr/share/APPLaunch/applications/` にあることを確認してください。

本体の矢印キー、Shift/Ctrl、Fn/SYMで生成される記号、文字の長押しに対応します。Ctrl+CやCtrl+DはPTYに送ります。直接入力時の履歴はShift+PageUp/PageDownまたはCtrl+Alt+PageUp/PageDownで操作します。ワイドモードの表示範囲はF4/F5/F7/F8で移動します。

## 本体液晶のステータス画面

HOMEの **External LCD CLI** を選ぶと、外付けLCDにはCLI、本体の320×170液晶にはステータス画面が出ます。ステータス画面には日付・時刻、電池残量、RAMとルートファイルシステムの空き容量、Wi-Fiと有線LANのIPv4アドレスを表示します。電池が充電中と報告されたときは電池バーに光が流れます。CPUグラフは毎秒1点ずつ描き、直近60点を保持します。データを取得できない項目には `--` を表示します。

手動で起動するときは、展開先から `--status` を付けて実行します。

```sh
./build/cardputer-cli --status panel.conf
```

本体液晶はランチャーが指定したフレームバッファーを優先し、320×170でアクセスできる場合に使用します。手動で選ぶ場合は `--status=/dev/fbN` を使います。本体液晶への初期化に失敗した場合も、外付けCLIは起動します。標準CLIから試す場合、本体画面にランチャーの画面が重なることがあるため、HOMEの登録項目から起動してください。

## 制約と確認状況

- `--keyboard` を省いた場合は起動元の端末から入力を受け取ります。この場合、全画面アプリの一部の特殊キーは起動元端末の送信するキー列に依存します。
- 英数字用フォントは同梱です。補助フォントは、システムにあればNoto Sans CJK、DejaVu Sans Monoを使用します。Debian系で追加する場合は `fonts-noto-cjk fonts-dejavu-core` をインストールします。
- 模擬PTY/LVGLで通常とワイドの行列数、ANSI出力からラベルへの描画、キー入力転送、カーソル位置応答、終了コードを確認済みです。Linuxでの実ビルド、FreeTypeとの実リンク、液晶での表示はこの環境では未検証です。

CLIとFreeTypeの改変元はCardputerZero launcherです。ソースと同梱フォントのライセンスは `LICENSE`、`freetype/LICENSE.txt`、`fonts/JetBrainsMono-OFL.txt` に記載しています。 [公開ランチャー](https://github.com/CardputerZero/launcher)
