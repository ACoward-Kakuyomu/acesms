[English](#english)

# Windows Template Library 10.01

WTL 10.01公式アーカイブから、Windowsフロントエンドに必要なヘッダー6個を無改変で `Include/` に収録しています。ATLはVisual StudioのC++ ATL x64コンポーネントを使います。配布ZIPにはMS-PLの原文を `third_party/wtl/MS-PL.txt` として収録します。 [上流プロジェクト](https://wtl.sourceforge.io/)

## English

- Upstream: https://wtl.sourceforge.io/
- Official archive: https://sourceforge.net/projects/wtl/files/WTL%2010/WTL%2010.01%20Release/WTL10_01_Release.zip/download
- Archive SHA-256: `1A62EA728D088C7C5C7CFC76DB445E5C9F04923F92CADCF27B5D7678D85826A2`
- Retrieved: 2026-11-16, via the download URL supplied by the official download page. The archive hash matches the SHA-256 published on that page.
- License: [Microsoft Public License (MS-PL)](MS-PL.txt). Preserve this license and the copyright notices when distributing source or binaries.

`Include/` contains the six unmodified headers required by `atlapp.h` and `atlframe.h`: `atlapp.h`, `atlframe.h`, `atlres.h`, `atlwinx.h`, `atluser.h`, `atlgdi.h`. Samples, wizards and unused headers are omitted. ATL itself is supplied by Visual Studio's C++ ATL x64 component and is not redistributed here.

Only the Windows application and window integration test use these headers. CMake does not download dependencies. Keep this version pinned; updating it is a separate change.

The release ZIP includes the original `MS-PL.txt` under `third_party/wtl`.
