[English](#english)

# acesms

acesms（ACoward's Emulator Sega Master System）は Windows 11 x64 向けの Sega Master System、Game Gear、SG-1000 エミュレーターです。

[Releases](https://github.com/ACoward-Kakuyomu/acesms/releases) から Windows x64 版を入手し、書込み可能なフォルダにZIP全体を展開して `acesms.exe` を起動してください。操作、設定、セーブデータのバックアップ方法は[マニュアル](docs/PACKAGE.md)に記載しています。

### ソースからビルド

Visual Studio 2026 の「C++ によるデスクトップ開発」と C++ ATL、Windows SDK、CMake 4.2 以降を使います。依存ライブラリは `third_party` に収録しています。

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

CTest の `package_content` が `acesms_package` を実行し、`build/packages/Release` の配布ZIPを検査します。GitHub ActionsはブランチとプルリクエストでWindows x64のReleaseを検証します。CMakeの版と一致する `vX.Y.Z` タグがpushされると、検証に合格したZIPをGitHub Releaseに添付します。

リリースのタイトルは `acesms X.Y.Z` です。タグを付けるコミットに日英のリリースノートを `docs/releases/X.Y.Z.md` として用意します。

acesmsのライセンスは [MIT](LICENSE) です。第三者ライブラリのライセンスは各 `third_party` フォルダにあります。FMバックエンドを開発する場合は [FM SDK](sdk/fm/README.md) を参照してください。

## English

acesms (ACoward's Emulator Sega Master System) is a Sega Master System, Game Gear, and SG-1000 emulator for Windows 11 x64.

Download the Windows x64 package from [Releases](https://github.com/ACoward-Kakuyomu/acesms/releases), extract the entire ZIP to a writable folder, and run `acesms.exe`. See the [manual](docs/PACKAGE.md#english) for controls, settings, and save-data backups.

### Build from source

Use Visual Studio 2026 with Desktop development with C++, C++ ATL, a Windows SDK, and CMake 4.2 or later. Dependencies are bundled under `third_party`.

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The `package_content` CTest runs `acesms_package` and validates the ZIP in `build/packages/Release`. GitHub Actions validates the Windows x64 Release build on branches and pull requests. Pushing a `vX.Y.Z` tag that matches the CMake project version attaches the verified ZIP to a GitHub Release.

The release title is `acesms X.Y.Z`. Include the Japanese and English release notes as `docs/releases/X.Y.Z.md` in the commit you tag.

acesms uses the [MIT License](LICENSE). Third-party licenses are in the corresponding `third_party` folders. See the [FM SDK](sdk/fm/README.md#english) for backend development.
