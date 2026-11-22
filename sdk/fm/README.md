[English](#english)

# acesms FM Plugin SDK v1

対象はWindows x64のYM2413/OPLL。SDKとymfmアダプターはMIT License（同フォルダのLICENSE）。バックエンドに使うライブラリには各ライセンスが適用される。

`acesms_fm.h`はC/C++共通でWin32/STLに依存しない。pluginはACESMS_FM_BUILD_PLUGINを定義して`acesms_fm_get_api`だけをexportする。未知のrequested ABIにはnullを返し、ABI 1には読取り専用api tableを返す。tableとmoduleは全instanceのdestroy完了まで有効でなければならない。v1はpack 8、Windows x64/cdecl。descriptor 156byte、api 232byte、config 12byte、load context 24byteを照合する。ABI用paddingはfileへ保存しない。

descriptorはbackend ID 16byte、UTF-8/NUL終端name[64]/revision[48]、状態版/互換ID、対応clock/divider。独自backendは他実装と衝突しない固定128bit IDを選ぶ。互換性はDLL path/hash/表示名とは独立で、状態解釈や音源動作が非互換になるときに状態版/互換IDを更新する。Debug/Releaseが同じ状態仕様なら同じIDを使う。

呼出し順はget_api→create→reset/advance_to/write_io/state操作→destroy。同じtableが発行した有効なhandleを使い、そのtableで破棄する。別DLLのdestroyやhost deleteは使わない。destroy(null)だけはno-op。他のnull handleはINVALID_ARGUMENT。任意の偽pointerを安全に扱う契約ではない。chip操作は既存emulation workerのみで行い、呼出し中のunloadや実行中のbackend交換は行わない。

- createはconfigのstruct_size、clock=3579545、divider=72を照合する。out handleを最初にnullにし、失敗時に中途instanceを返さない。出力sample rateや補間方式はbackendへ渡さない。
- advance_toは最後のmaster tickからtargetまで進める。必要個数はtarget/72−previous/72。最初のsampleはtick 72。uint64時刻は単調、同時刻は許す。native更新→同時刻PCM採取→write_ioの順はhostが保つ。出力は順序通りのint32 mono、振幅32768で1.0。native Hzの整数丸めはしない。
- advance_toのproduced、save_stateのwritten、state_sizeのbytesは、out pointerが有効なら失敗時0。容量不足や逆行時刻ではchipと出力bufferを変更しない。個数0のadvanceはoutput=nullを許す。個数>0なら有効bufferと必要容量が必要。
- write_ioのportは0=address/1=dataのみ。resetはclock/address/user patch/全履歴を初期値へ戻す。gate/Muteはhost側にあり、backendと変換時計は止めない。
- state_sizeは現在configの固定サイズを返す。save_stateはcapacity以上を書かず、writtenで実長を返す。load_stateはexact lengthと値を検証し、contextのclock/divider/master_ticksに整合する場合だけ反映する。失敗で現在instanceを変更しない。header/version/ROM/host補間状態の検証はhostが別途担当する。

結果はOK=0、INVALID_ARGUMENT=1、UNSUPPORTED_CONFIG=2、BUFFER_TOO_SMALL=3、INVALID_STATE=4、OUT_OF_MEMORY=5。例外/STL/C++ vtable/size_tやCRT資源の所有を境界へ出さない。hostはエラーを停止/復元失敗として報告し、暗黙fallbackで別backendへ置換しない。

DLLは実行ファイルの隣の `plugins/fm` に配置してacesmsを再起動する。FM backendで内蔵emu2413、同梱ymfm、または同フォルダにあるDLLを選ぶ。ABIと構成は選択したバックエンドでROMを起動するときに検証する。DLLが見つからないか非互換のときは現在のROMを維持する。バックエンドと補間方式はROM起動時の設定で決まる。DLLを交換する前にacesmsを終了する。ビルド可能な実装例は [ymfmプラグイン](../../src/plugins/ymfm/README.md)。

## English

The SDK targets YM2413/OPLL on Windows x64. The SDK and ymfm adapter use the MIT License. Backend libraries retain their own licenses.

`acesms_fm.h` supports C and C++ without Win32 or STL dependencies. Define `ACESMS_FM_BUILD_PLUGIN` and export only `acesms_fm_get_api`. Return null for an unsupported ABI and a read-only API table for ABI 1. Keep the table and module valid until every instance has been destroyed. ABI 1 uses 8-byte packing and Windows x64/cdecl. Descriptor, API table, config, and load context sizes are 156, 232, 12, and 24 bytes. Exclude padding from serialized state.

The descriptor includes a 16-byte backend ID, NUL-terminated UTF-8 name[64] and revision[48], state version and compatibility ID, and supported clock and divider. Use a unique fixed 128-bit backend ID. Update the state version or compatibility ID when state interpretation or emulation becomes incompatible. Debug and Release should share the ID when their states are compatible. DLL paths, hashes, and display names do not establish state compatibility.

Call get_api, create, then reset/advance_to/write_io and state operations, and finally destroy. Use a valid handle from the same table and free it through that table. destroy(null) is a no-op. Other null handles yield INVALID_ARGUMENT. Chip calls run on the emulation worker. Close acesms before exchanging plug-ins.

- create validates struct_size, clock 3579545, and divider 72. Set the output handle to null before attempting creation. The host does not pass a sample rate or resampling method.
- advance_to produces target/72 minus previous/72 samples, starting at master tick 72. Tick counts are nondecreasing uint64 values. The host advances the chip and collects that tick's PCM before any port writes at the same tick. Return ordered int32 mono samples at scale 32768 = 1.0.
- On failure, set produced, written, or bytes to zero if the respective output pointer is valid. On insufficient capacity or backward time, leave the chip and output buffer unchanged. A zero-sample advance permits a null output. A nonzero count requires a valid buffer with sufficient capacity.
- write_io accepts port 0 (address) or 1 (data). reset resets the clock, address, user patch, and history. The host handles gating and mute without stopping the chip clock.
- state_size reports the fixed size for the current configuration. save_state writes at most capacity bytes and returns the actual length. load_state validates the exact length, payload, and context clock, divider, and master_ticks before applying state. On failure it retains the current instance. The host validates the outer state header, format, ROM, and resampler state.

Result codes are OK (0), INVALID_ARGUMENT (1), UNSUPPORTED_CONFIG (2), BUFFER_TOO_SMALL (3), INVALID_STATE (4), and OUT_OF_MEMORY (5). Do not pass exceptions, STL types, C++ vtables, size_t, or ownership of CRT resources across the ABI. The host reports backend errors and keeps backend selection explicit.

Place DLLs in `plugins/fm` next to the executable and restart acesms. Select the backend and resampling method in the ROM settings. acesms loads and validates the selected DLL when opening the ROM. If the DLL is missing or incompatible, the current ROM stays open. See the [ymfm plug-in example](../../src/plugins/ymfm/README.md) for a buildable implementation.
