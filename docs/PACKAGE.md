[English](#english)

# acesms

ACoward's Emulator Sega Master System。Windows 11 x64向けのSMS / Game Gear / SG-1000エミュレーターです。版は Help → About… または `acesms.exe` のプロパティで確認できます。

Windows 11 x64、MSVCのx64ランタイム、D3D11描画とWASAPI共有音声を利用できる環境が必要です。ZIP全体を同じdirectory構成のまま展開し、acesms.exeを起動してください。インストーラーや自動更新はありません。ROMやBIOSは同梱していません。

DBスキーマは版3です。1.1.0は公開版1.0.0・1.0.1のステートを復元でき、保存時は形式版2を使います。新規DBを作る場合は、本体の終了後に既存DBを手動で退避してください。

## 起動と入力

ステートはROMごとの固定10スロットです。上段の1〜9・0（10番）またはFileメニューで枠を選び、F5で確認なしの保存・上書き、F6で同じ枠を復元します。Manage statesではタイトル編集・復元・確認付き削除を行えます。

File → Open ROM…で手元のROMを選びます。初回はRegister ROMで機種、映像方式、Mapper、RAM容量などを確認し、Register and openで登録・起動します。Cancelで現在のROMに戻ります。次回は同じROMの保存済み設定で起動します。File → Settings…のNew ROM Defaultsは未登録ROMの初期値です。OKで実行ファイル横の `acesms.sqlite3` に保存し、Cancelで編集を破棄します。Help → ROM information…でDBのパス、保存済み設定、現在の実行条件を確認できます。

`.sms`・`.gg`・`.sg`・`.zip` は、File → Open ROM…で選ぶか、エクスプローラーからメインウィンドウへ1ファイルずつドロップして開けます。ZIPは格納順（中央ディレクトリ順）で最初の対応ROMを開きます。同じROMなら、生ファイルで登録した設定・SRAM・ステートも再利用します。

実行中の登録済みROMは、File → Edit ROM settings…で設定を編集できます。Review changesでステートの保持・変換・削除とRAMへの影響を確認し、Apply changesで確定します。どちらの画面でもCancelで取り消せます。エミュレーション設定を変更するとROMが再起動します。

- File → Settings… → Controller Configurationで各プレイヤーの入力元・割り当てを設定します。SMSとSG-1000は2Pまで、GGは1Pを使います。初期設定は1Pがキーボード、2PがDisabledです。
- キーボードの1P既定は矢印で方向、Z / Xでボタン1 / 2です。XInputはスロット1〜4から選び、D-pad / 左スティックで方向、既定のA / Bでボタン1 / 2を操作します。
- 連射1 / 2の既定は1PキーボードがA / S、XInputがX / Yです。長押しで対応ボタンを2フレームずつ押下・解放し、通常ボタンも押している間は押下を維持します。全機種で利用でき、割り当ての変更・解除はController Configurationから行います。
- 1PのEnterまたはXInput StartはSMSのPause、SG-1000のHold、GGのStart（押している間）です。
- Spaceまたは1PのXInput Backでホスト側を一時停止します。Emulation → Muteでミュートします。
- 1Pの入力元がXInputの場合は、Controller Configuration → XInput shortcuts…で本体操作を変更・解除できます。初期割り当てはLBで即時保存、RBで復元、LT／RTで前後のステートスロット、LSで高速駆動、RSで全画面です。スロット変更時はウィンドウ・全画面のどちらでも左上に選択先を表示します。
- Alt+Enterでボーダーレス全画面を切り替えます。メニューからは Emulation → Fullscreen で全画面に入り、全画面中は右クリック → Exit fullscreen で戻ります。
- ウィンドウ表示中は F8 で表示倍率を×1〜×5の範囲で切り替えます。現在のモニターの作業領域に収まる倍率を順に選びます。
- F7で映像右上のFPS表示を切り替えます。通常・高速駆動ともにエミュレーションが完成させたフレーム数を毎秒表示します。
- 登録済みROMではF9でメモリサーチ、Alt+F9でチート編集を開きます。SMS・GG・SG-1000の本体RAMを検索でき、編集画面では単発適用と毎フレームの常時適用を選べます。Save Cheatsで定義をDBに保存します。F9またはResumeで画面を閉じると、開く前の実行・一時停止状態に戻ります。
- チート編集のExportは保存済みの定義を交換用JSONに書き出します。Importは同じROM・機種用の定義を編集一覧に追加します。追加分は無効状態で、Save Cheatsで確定したときにDBへ保存されます。
- Viewメニューで **Pixel aspect 1:1**（既定）と **Screen aspect 4:3** を切り替えます。設定は全ROM共通で、実行ファイル横の `acesms.sqlite3` に保存されます。

F2キーまたは Emulation → Fast forward で高速駆動を切り替えます。高速中は音声の消費やVSyncを待たず、PCMバッファーの空きに入った音声だけを再生します。解除すると通常速度に戻ります。

SMS NTSC/PALの192-line表示、GG Export/NTSC、RomOnly / Sega / Codemastersを対象とします。全市販タイトルの互換性は未確認です。Codemasters RAM有効時のA000〜BFFF書込みはRAMのみ更新してbank latchを保持する暫定仕様で、実機未確認です。

日本向けNTSCのSG-1000では、File → Open ROM…で `.sg` を選び、初回のRegister ROMで **SG-1000** を明示選択します。映像は256×192、音声はモノラルです。対応するROMは特殊なバンク切替・拡張RAMを使わない8 / 16 / 32 / 40 / 48 KiBで、40 KiBはCPUアドレス順に配置されたイメージを使います。

SG-1000を続けて登録する場合は、File → Settings…のMachineでSG-1000を選んで確定すると、未登録ROMの登録画面で初期選択されます。

## StateとSRAM

共通設定、ROM設定、Segaの8 / 16 / 32KiB SRAM、ステート、チート定義の保存先は `acesms.exe` と同じフォルダの `acesms.sqlite3` です。SRAMはROM切替時と正常終了時に、変更がある場合だけ自動保存します。失敗時はTry Againで再試行、Continueで未保存SRAMを破棄して継続、Cancelで現在ROMを保持します。強制終了時の最新内容は保証しません。Codemasters RAMは揮発性です。

File → Edit ROM settings…で現在のSRAM容量を確認できます。Clear SRAM…を確定すると、現在のSRAMだけをゼロにします。保存済みステート内のRAMは残ります。

File → Manage states… で現在のROMのステートを一覧表示します。一覧からタイトル編集・復元・削除を選びます。削除は確認後に実行します。保存にはF5を使います。既存.savやステートファイルの移行は未対応です。

### DBの手動バックアップと復元

書込み可能なフォルダにZIPを展開してください。DBに書き込めない場合は、対象パスと理由を表示します。DBは実行ファイルと同じフォルダに置きます。

1. **バックアップ**　必要なステートを保存し、SRAM保存が成功してから、同じDBを使う本体をすべて正常終了します。実行ファイル横の `acesms.sqlite3` を別のフォルダや媒体へコピーします。
2. **復元**　本体を終了し、現在の `acesms.sqlite3` を別の場所に退避します。バックアップを実行ファイル横へ同じ名前でコピーしてから、本体を起動します。
3. **ZIPの更新・別PCへの移行**　新しいZIPを別フォルダに展開し、旧本体を正常終了してからDBのコピーを新しい実行ファイルの横に置きます。コピー先に既存DBがある場合は先に退避します。

1.1.0は公開版1.0.0・1.0.1のDBを最初に開いた時点で版3へ更新します。既存のステートは保持し、F5で保存・上書きしたときに形式版2を使います。1.0.1以前は版3のDBを開けません。旧版へ戻す場合は、旧本体と更新前のDBバックアップを使います。

アプリ内のバックアップ・復元機能は設けません。異常終了直後はDB本体だけをコピーせず、本体を再起動してROMを開き、正常に終了してからコピーしてください。配布ZIPに利用者のDBは含まれません。DBにはROM本体やFMプラグインは含まれません。

## FMとSDK

新規ROMの初期設定は機種・Mapperとも自動判定、RAM 32 KiB、FM有効です。FMはSMS NTSCで適用され、ほかの機種・映像方式では登録画面のFM項目が無効表示になります。音源と補間の既定は内蔵emu2413 / Sincです。同梱のymfm OPLL DLLと、Sinc / Zero-order hold + low-pass filter / Linearを選択できます。

追加のSDK互換DLLはplugins/fmへ置いて再起動すると、FM backendにファイル名で表示されます。明示したDLLが欠落/非互換なら現在ROMを保持して拒否し、別の音源へ自動変更しません。実行中のDLL交換は行わないでください。

SDKはsdk/fm、再ビルド可能なサンプルはsrc/plugins/ymfm、必要な固定ymfm原本はthird_party/ymfmにあります。Windows x64 / CMake 4.2以降 / Visual Studio 2026 C++環境で、展開先rootから実行できます。

```powershell
cmake -S src/plugins/ymfm -B .data/ymfm-example -G "Visual Studio 18 2026" -A x64
cmake --build .data/ymfm-example --config Release
```

生成DLLは.data/ymfm-example/Release/acesms-fm-ymfm.dllです。コピーする場合は本体を終了してから行ってください。サンプルの詳細はsrc/plugins/ymfm/README.md、C ABIの契約はsdk/fm/README.mdを参照してください。依存のダウンロードは行いません。

## ライセンスと確認範囲

自作部分はMIT License（LICENSE、Copyright (c) 2026 ACoward）。第三者部分はそれぞれの条件を維持します。

- 内蔵emu2413: MIT（`third_party/emu2413/LICENSE`）。
- WTL: MS-PL（`third_party/wtl/MS-PL.txt`）。
- SQLite 3.54.1: public domain（third_party/sqlite/README.md、PUBLIC-DOMAIN.txt）。
- miniz 3.1.2: MIT（third_party/miniz/README.md、LICENSE）。
- picojson: BSD 2-Clause（third_party/picojson/README.md、LICENSE）。
- ymfm: BSD-3-Clause（plugins/fm/ymfm-LICENSE.txt、third_party/ymfm/LICENSEと各sourceのnotice）。
- 自作SDK / ymfm adapter: MIT（sdk/fm/LICENSE）。

音声はWASAPI共有モードを使います。オーディオ機器ごとの互換性やアナログ出力の実機比較は未確認です。

## English

acesms (ACoward's Emulator Sega Master System) is a Sega Master System, Game Gear, and SG-1000 emulator for Windows 11 x64. Check the version under Help → About… or in the properties of `acesms.exe`.

Extract the entire ZIP to a writable folder and run `acesms.exe`. The emulator requires the x64 Microsoft Visual C++ runtime, Direct3D 11, and WASAPI shared-mode audio. The package contains no ROMs, BIOS, installer, or updater.

The database schema is version 3. acesms 1.1.0 loads save states from the public 1.0.0 and 1.0.1 releases and writes save-state format version 2. To start with a fresh database, exit acesms and move the current database out of its folder.

### Opening ROMs and controls

Use File → Open ROM… or drop one file onto the main window. Supported extensions are `.sms`, `.gg`, `.sg`, and `.zip`. For ZIPs, acesms opens the first supported ROM in central directory order. The same ROM shares its settings, SRAM, and states whether opened directly or from a ZIP.

For a new ROM, confirm the machine, video standard, mapper, and RAM in Register ROM, then choose Register and open. Cancel keeps the current session. Previously registered ROMs use their saved settings. File → Settings… → New ROM Defaults sets the initial values. Help → ROM information… shows the database path, saved settings, and current runtime configuration.

To change settings for the current ROM, choose File → Edit ROM settings…, then Review changes. The review describes the effect on RAM and which states will be kept, converted, or deleted. Apply changes confirms the update and restarts the ROM if emulation settings changed. Cancel discards the edit.

- Configure each player's input source and bindings in File → Settings… → Controller Configuration. SMS and SG-1000 support two players. Game Gear uses Player 1. By default Player 1 uses the keyboard and Player 2 is disabled.
- Default Player 1 keys are the arrow keys and Z / X for buttons 1 / 2. XInput supports controller slots 1–4, the D-pad or left stick for directions, and A / B for buttons 1 / 2 by default.
- Hold A / S on the default keyboard or X / Y on XInput for rapid fire. The corresponding button alternates between pressed and released every two frames. Holding the regular button keeps it pressed. Rapid fire bindings can be changed or cleared on all supported systems.
- Player 1 Enter or XInput Start triggers SMS Pause, SG-1000 Hold, or GG Start (while held). Space or Player 1 XInput Back toggles host pause. Emulation → Mute toggles audio.
- With Player 1 set to XInput, open Controller Configuration → XInput shortcuts… to change or clear the bindings. By default, LB saves immediately, RB restores, LT / RT select the previous / next state slot, LS toggles fast forward, and RS toggles fullscreen. Slot changes briefly show the active slot in the upper-left corner in both windowed and fullscreen mode.
- F2 or Emulation → Fast forward toggles fast forward. While active, the emulator skips VSync and audio pacing. It plays samples that fit in the available PCM buffer and drops the rest.
- Alt+Enter toggles borderless fullscreen. Use Emulation → Fullscreen to enter it from the menu, and right-click → Exit fullscreen to leave it.
- F7 toggles an FPS counter in the upper-right corner. It counts completed emulated frames per second at either speed. In windowed mode, F8 cycles the scales from 1× to 5× that fit in the monitor's work area.
- The View menu switches between Pixel aspect 1:1 (default) and Screen aspect 4:3. This setting is shared across ROMs.
- For a registered ROM, F9 opens Memory Search and Alt+F9 opens the cheat editor. Search works on console RAM. The editor supports one-shot and per-frame cheats. Save Cheats writes definitions to the database. F9 or Resume returns to the previous running or paused state.
- The cheat editor's Export button writes saved definitions to JSON. Import adds definitions for the same ROM and machine as disabled entries. Save Cheats commits them.

SMS NTSC/PAL in 192-line mode and GG Export/NTSC are supported with the RomOnly, Sega, and Codemasters mappers. Commercial game compatibility testing is limited. While Codemasters RAM is enabled, writes to A000–BFFF update RAM and leave the bank latch unchanged. This behavior has yet to be verified on hardware.

For Japanese NTSC SG-1000, select SG-1000 when first registering a `.sg` ROM. Video is 256×192 and audio is mono. Supported sizes are 8, 16, 32, 40, and 48 KiB without special bank switching or expansion RAM. A 40 KiB image must be laid out in CPU address order. To register multiple SG-1000 games, select SG-1000 in the New ROM Defaults.

### Save states, SRAM, and backups

Each ROM has ten fixed state slots. Select slot 1–10 using the top-row 1–9 and 0 keys or the File menu. F5 saves or overwrites the selected slot immediately. F6 restores it. File → Manage states… lists slots and allows title editing, restoration, and deletion. Deletion requires confirmation.

Global settings, ROM settings, Sega SRAM (8, 16, or 32 KiB), save states, and cheats are stored in `acesms.sqlite3` next to `acesms.exe`. Changed SRAM is saved when switching ROMs or exiting normally. On save failure, choose Try Again to retry, Continue to discard unsaved SRAM and continue, or Cancel to keep the current ROM. Codemasters RAM is volatile. File → Edit ROM settings… shows the current SRAM size. Clear SRAM… zeroes the current SRAM after confirmation and leaves saved states intact.

1. **Back up.** Save any desired states and close every instance using the database normally so SRAM can be written. Copy `acesms.sqlite3` to another folder or device.
2. **Restore.** Close acesms, move the current database aside, and copy the backup next to the executable under the name `acesms.sqlite3`. Then start acesms.

3. **Upgrade or move to another PC.** Extract the new ZIP to a separate folder. After closing the old installation normally, place a copy of the database next to the new executable. Move aside any database already at the destination.

When 1.1.0 first opens a database from 1.0.0 or 1.0.1, it upgrades it to schema version 3 and preserves existing save states. F5 saves or overwrites a slot using format version 2. Version 1.0.1 and earlier cannot open a schema version 3 database. To roll back, use the old executable and the pre-upgrade database backup.

After a crash, reopen acesms, load a ROM, and exit normally before copying the database. The database contains no ROM binaries or FM plug-ins. If the database cannot be written, acesms displays the path and reason.

### FM and plug-in SDK

New ROM defaults are automatic machine and mapper detection, 32 KiB of RAM, and FM enabled. FM applies to SMS NTSC. The default backend and resampling method are emu2413 and Sinc. You can also select the bundled ymfm OPLL DLL and choose Zero-order hold + low-pass filter or Linear resampling.

Place compatible DLLs in `plugins/fm` and restart acesms to make them available under FM backend. A missing or incompatible selected DLL prevents the requested ROM from opening and keeps the current session. Close acesms before replacing a DLL.

The SDK is under `sdk/fm`, the plug-in sample under `src/plugins/ymfm`, and the fixed ymfm sources under `third_party/ymfm`. To build from the extracted package, use Visual Studio 2026 with C++ and CMake 4.2 or later, and run from the package root.

```powershell
cmake -S src/plugins/ymfm -B .data/ymfm-example -G "Visual Studio 18 2026" -A x64
cmake --build .data/ymfm-example --config Release
```

The output is `.data/ymfm-example/Release/acesms-fm-ymfm.dll`. See the sample README at `src/plugins/ymfm/README.md` and the C ABI documentation at `sdk/fm/README.md`.

### Licenses

acesms, its SDK, and its ymfm adapter use the MIT License (`LICENSE`), Copyright (c) 2026 ACoward. Third-party licenses and notices are included.

- emu2413 — MIT (`third_party/emu2413/LICENSE`)
- WTL — MS-PL (`third_party/wtl/MS-PL.txt`)
- SQLite 3.54.1 — public domain (`third_party/sqlite/PUBLIC-DOMAIN.txt`)
- miniz 3.1.2 — MIT (`third_party/miniz/LICENSE`)
- picojson — BSD 2-Clause (`third_party/picojson/LICENSE`)
- ymfm — BSD 3-Clause (`third_party/ymfm/LICENSE`)

Audio uses WASAPI shared mode. Compatibility across audio devices and comparisons with the original analog output remain unverified.
