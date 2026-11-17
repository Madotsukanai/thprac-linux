# thprac-th06nc (Linux / Wine・Proton 向けローカルビルド)

[thprac](https://github.com/touhouworldcup/thprac) の非公式フォーク [zxxsmart/thprac-th06nc](https://github.com/zxxsmart/thprac-th06nc) に、以下のローカル変更を加え、**Linux (MinGW-w64 クロスコンパイル) でビルドして Steam の Proton で動かす**ためのリポジトリです。。

動作中のゲームにフックするためにはWineから起動する必要があります。（Protonではprefixを合わせないと不可）

## ローカル変更 (アップストリームとの差分)

### 1. 追加スペルカードの BGM 修正 (`thprac/src/thprac/th06nc/runtime.cpp`)

Extra Phantomのスペルカードをプラクティスメニューから選んだ際、BGMがExtraのままになっていたのを、正しい曲に修正しています。

- 対象: `呪弾「フラジャイルウィング」` / `呪弾「這い寄る血痕」` / `「弾幕の心臓」` (sections.h の `FragileWing=71`〜`DanmakuHeart=73`)

### 2. ゲームの直接起動 (Steam ライブラリ解決)

ランチャーのゲームリストから th06nc を起動すると、Proton/Wine 環境では `steam://rungameid/4659620` が解決できずフリーズ (60 秒待ち → 失敗) する問題を修正しています。

- ブリッジが Linux 側 Steam の情報から **th06nc.exe のインストール場所を解決し、直接 `CreateProcess` で起動**します。
- 解決順:
  1. `STEAM_COMPAT_DATA_PATH` / `WINEPREFIX` の `compatdata/4659620` からライブラリを特定 (自プレフィックスと確認できた場合のみ直接起動)
  2. `HOME` 配下の Steam (`~/.local/share/Steam`, `~/.steam/steam`, Flatpak 版) の `steamapps/common/th06nc/`
  3. `libraryfolders.vdf` から AppID 4659620 のライブラリを検索
  4. `Z:\home\*` のワイルドカード走査
- 解決できない場合は従来どおり `steam://` にフォールバックします。

### 3. デバッグログ (暫定)

`thprac-th06nc.log` への診断ログ (`[dll] diag ...`) が残っています。動作確認が済んだら削除推奨 (`runtime.cpp` の `rlog`/`rlogw` 呼び出し)。

## 必要なもの (ビルド)

| ツール | 用途 |
| --- | --- |
| `x86_64-w64-mingw32-{gcc,g++,windres}` | 64bit モジュール / ブリッジ / ロケール生成ツール |
| `i686-w64-mingw32-{gcc,g++,windres,objdump}` | 32bit ランチャー (統合 exe) |
| CMake 3.24+ | 64bit モジュールと FreeType のビルド |
| Python 3 | 埋め込みリソース生成 |
| Wine | ロケール定義生成 (`loc_json.exe`) の実行 |
| `curl`, `tar`, `xz` | FreeType 2.13.3 ソースの取得 |
| GNU Make | ビルド |

Debian/Ubuntu 系なら `apt install mingw-w64 cmake python3 wine make` 等で揃います。

## ビルド

```sh
make nc    # 64bit モジュール (thprac_th06nc.dll) とブリッジのみ
make       # 統合 exe まで (build/linux-x64/dist/thprac-th06nc.exe)
```

成果物: `build/linux-x64/dist/thprac-th06nc.exe`
32bit FreeType は初回 `make` 時に自動ダウンロードして静的ビルドされます。`make clean` で `build/` を削除。

## ライセンスと謝辞

- thprac 本体: Copyright (c) 2022 Ack and thprac contributors, [MIT License](LICENCE)
- サードパーティ: [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) を参照。FreeType は FTL に基づき使用 (This software is based in part on the work of the FreeType Team)。
- 変更履歴: [CHANGELOG.md](CHANGELOG.md) / NC ツールの詳細: [README_NC.md](README_NC.md)

東方Projectおよび『東方紅魔郷：New Classic』は上海アリス幻樂団・上海アリスReprise、各権利者に帰属します。本ツールは非公式のファンメイド練習ツールで、ゲームの開発元・発行元、thprac のメンテナー、TWC のいずれにも承認されたものではありません。
