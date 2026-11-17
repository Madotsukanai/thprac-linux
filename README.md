# thprac-linux

[thprac](https://github.com/touhouworldcup/thprac) の非公式フォーク [zxxsmart/thprac-th06nc](https://github.com/zxxsmart/thprac-th06nc) に、以下のローカル変更を加え、**Linux (MinGW-w64 クロスコンパイル) でビルドして Steam の Proton で動かす**ためのリポジトリです。。

絶対にthpracをProtonではなく**Wineから起動して下さい。**Protonで起動するとゲームプロセスにthpracをフックさせる事ができません。

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

成果物: `build/linux-x64/dist/thprac-linux.exe`<br>
32bit FreeType は初回 `make` 時に自動ダウンロードして静的ビルドされます。`make clean` で `build/` を削除。

## ライセンスと謝辞

- thprac 本体: Copyright (c) 2022 Ack and thprac contributors, [MIT License](LICENCE)
- サードパーティ: [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) を参照。FreeType は FTL に基づき使用 (This software is based in part on the work of the FreeType Team)。
- 変更履歴: [CHANGELOG.md](CHANGELOG.md) / NC ツールの詳細: [README_NC.md](README_NC.md)

東方Projectおよび『東方紅魔郷：New Classic』は上海アリス幻樂団・上海アリスReprise、各権利者に帰属します。本ツールは非公式のファンメイド練習ツールで、ゲームの開発元・発行元、thprac のメンテナー、TWC のいずれにも承認されたものではありません。
