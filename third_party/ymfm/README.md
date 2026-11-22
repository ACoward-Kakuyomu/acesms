[English](#english)

# ymfm

OPLLプラグインに必要な上流原本を固定リビジョン `81aec25ccbb98f4873a255f7551ac4dadac59b4a` から無改変で収録しています。ymfmはBSD 3-Clauseで、自作のSDKとアダプターはMITです。上流のREADMEは `UPSTREAM-README.md`、原本のSHA-256は `UPSTREAM-SHA256SUMS.txt` にあります。実装例は `src/plugins/ymfm` を参照してください。 [上流プロジェクト](https://github.com/aaronsgiles/ymfm)

## English

- Upstream: https://github.com/aaronsgiles/ymfm
- Fixed revision: `81aec25ccbb98f4873a255f7551ac4dadac59b4a`
- License: BSD-3-Clause. Preserve the unmodified LICENSE and source copyright/notices in source/binary distributions.
- Local modifications to upstream sources: none.
- Included: required `src/ymfm*` header/implementation files only. The DLL exposes OPLL through the acesms SDK; it does not expose all library chip types.
- UPSTREAM-README.md is the original README; UPSTREAM-SHA256SUMS.txt records original basename/hash pairs (README.md maps to UPSTREAM-README.md, other source basenames map to src/).

The self-contained build example is in src/plugins/ymfm at the repository/package root. acesms SDK/adapter code is MIT; upstream ymfm remains BSD-3-Clause.
