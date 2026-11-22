[English](#english)

# ymfm OPLL plugin example

自作adapterはMIT（sdk/fm/LICENSE）、固定ymfm原本はBSD-3-Clause（third_party/ymfm/LICENSE）。revisionと原本hashはthird_party/ymfm/README.mdに記録する。依存の自動取得はしない。

repositoryまたは配布directoryのrootから、Windows x64 / Visual Studio 2026 C++環境で次を実行する。

```powershell
cmake -S src/plugins/ymfm -B .data/ymfm-example -G "Visual Studio 18 2026" -A x64
cmake --build .data/ymfm-example --config Release
```

`.data/ymfm-example/Release/acesms-fm-ymfm.dll`とymfm-LICENSE.txtが出力される。配布時もsdk/fm、src/plugins/ymfm、third_party/ymfmの相対配置を維持する。DLLの採用先は本体のplugins/fm。実行中のDLL交換は行わない。

nativeはmaster clock/72。2 DAC streamをint32で加算してmono振幅32768基準とし、rate変換/gate/Muteはhostへ委ねる。SDK v1以外にはnullを返す。全instanceを同じtableのdestroyで破棄した後にmoduleを解放する。

backend state v1は327byte（u64 master tick＋固定ymfm 319byte明示LE）。長さ/clock/時刻/値を先に検証し、独立候補へrestoreしてから交換する。上流resetは全時計の初期化ではないためSDK resetはfresh chipの構築・交換とする。派生cacheは上流APIで再構築し、stateにpointer/C++ objectを保存しない。

ABIは [FM SDK](../../../sdk/fm/README.md) を参照する。

## English

The adapter uses the MIT License (`sdk/fm/LICENSE`). The vendored ymfm sources use BSD 3-Clause (`third_party/ymfm/LICENSE`). The pinned revision and checksums are in `third_party/ymfm/README.md`. The build uses the bundled dependencies.

From the repository or package root, build for Windows x64 with Visual Studio 2026 and its C++ workload.

```powershell
cmake -S src/plugins/ymfm -B .data/ymfm-example -G "Visual Studio 18 2026" -A x64
cmake --build .data/ymfm-example --config Release
```

The output folder is `.data/ymfm-example/Release`. It contains `acesms-fm-ymfm.dll` and `ymfm-LICENSE.txt`. Keep the relative locations of `sdk/fm`, `src/plugins/ymfm`, and `third_party/ymfm` when distributing the sample. Install the DLL into `plugins/fm` while acesms is closed.

The native sample rate is the master clock divided by 72. The plug-in sums both DAC streams to int32 mono, scaled so 32768 represents 1.0. The host handles resampling, gating, and mute. Only SDK ABI 1 is supported. Destroy all instances through their creating API table before unloading the module.

Backend state v1 is 327 bytes, an unsigned 64-bit master tick and a fixed 319-byte ymfm state in little-endian order. Loading validates length, clock, tick, and values before restoring into a candidate and swapping it in. SDK reset constructs a fresh chip so every clock resets. The plug-in rebuilds derived caches through upstream APIs and serializes no pointers or C++ objects. See the [FM SDK](../../../sdk/fm/README.md) for the ABI.
