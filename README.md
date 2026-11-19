# thprac-linux

[thprac](https://github.com/touhouworldcup/thprac) の非公式フォークです。東方紅魔郷 New Classic (th06nc / NC) のプラクティスサポートを含み、Linux 上で Wine/Proton を使って動かすためのビルドです。

成果物は単一の Windows 実行ファイル `thprac-linux.exe` で、32bit ランチャー + 64bit NC モジュール + Unix ヘルパーを内蔵しています。

## 特徴

- NC (th06nc.exe) 専用のプラクティスモジュール (`thprac_th06nc.dll` + `thprac_bridge64.exe`) を内蔵
- Proton/Wine の別コンテナ・別 prefix で動いているゲームにもアタッチできるクロスプレフィックス機構
  - Unix 側の処理は静的リンクのネイティブ ELF (`thprac-xattach`) が担当
  - `--alongside-steam` 経由でホスト側にヘルパーを再実行し、nsenter / `steam-runtime-launch-client` で対象コンテナに入る
- 起動後すぐにフックするための常駐ウォッチャー
- GitHub Releases からの自動アップデート

## ビルド

必要なもの:

- GNU make, cmake, python3
- `i686-w64-mingw32-g++`, `x86_64-w64-mingw32-g++`, `windres`
- ホスト用 `g++` (ネイティブ ELF ヘルパー用)
- `wine` (ロケール定義の生成に使用)

```sh
make          # すべてビルド (既定ターゲット)
make nc       # NC モジュール (dll + bridge) のみ再ビルド
make clean    # build/ を削除
```

初回ビルド時に 32bit/64bit の FreeType をダウンロードして静的ビルドします。

成果物:

- `build/linux-x64/dist/thprac-linux.exe` — ランチャーと全ペイロードを統合した単一 exe
- `build/linux-x86/payload/thprac-xattach` — 埋め込まれる Unix ヘルパー (静的 ELF)

CI では `.github/workflows/nc.yml` が同じexeをビルドし、`thprac-linux` という名前の artifact としてアップロードします。

## 構成

| ファイル | 役割 |
| --- | --- |
| `thprac/src/thprac/thprac_th06nc.cpp` | NC のプラクティスモジュール本体。bridge も同じファイル内 (`THPRAC_TH06NC_BRIDGE`) |
| `thprac/src/thprac/thprac_th06nc.h` | NC 共通定義 |
| `thprac/src/thprac/thprac_th06nc_map.h` | 生成されたステージ/シーンデータ |
| `thprac/src/thprac/thprac_wine_attach.cpp` | クロスプレフィックスアタッチの Windows 側 |
| `tools/thprac_xattach.cpp` | Unix 側のネイティブ ELF ヘルパー (scan / apply / cmd / update) |
| `thprac/src/thprac/thprac_native_bundle.h` | リソースの抽出とヘルパーの起動 |
| `tools/make_payload_rc.py` | ペイロードのリソース ID とバンドル ID を生成 |

`FILES` の並び順は `tools/make_payload_rc.py`、`tools/package_nc_single.py`、`thprac_native_bundle.h` の3箇所で一致させる必要があります。

## 使い方

`thprac-linux.exe` を Wine または Proton のランチャーとして起動します。ゲーム一覧から対象を選んで起動するか、「Tools」タブの「実行中のゲームに thprac を適用」で起動済みのゲームを選びます。

別の Proton コンテナで動いているゲームを適用する場合、ヘルパーが `--alongside-steam` でホスト側に再実行され、対象のコンテナに入ってアタッチします。時間がかかる場合は数秒待ってください。

バージョンは `thprac/src/thprac/thprac_fork.h` の `THPRAC_FORK_VERSION` と `THPRAC_FORK_VERSION_NUMBERS` を編集します。

## ライセンスと謝辞

- thprac 本体: Copyright (c) 2022 Ack and thprac contributors, [MIT License](LICENCE)
- サードパーティ: [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) を参照。FreeType は FTL に基づき使用 (This software is based in part on the work of the FreeType Team)。

東方Projectおよび『東方紅魔郷：New Classic』は上海アリス幻樂団・上海アリスReprise、各権利者に帰属します。本ツールは非公式のファンメイド練習ツールで、ゲームの開発元・発行元、thprac のメンテナー、TWC のいずれにも承認されたものではありません。
