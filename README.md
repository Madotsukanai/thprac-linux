# thprac-th06nc (Linux / Wine 向けビルド)

[thprac](https://github.com/touhouworldcup/thprac) の非公式フォーク [thprac-th06nc](https://github.com/zxxsmart/thprac-th06nc) を、**Linux 上でビルドし、Wine / Proton で動かす**ための移植版です。

- Windows 用の Visual Studio は不要です。MinGW-w64 によるクロスコンパイルで `thprac-th06nc.exe` を生成します。
- 生成した exe をゲームのフォルダに置いて起動すると、同じフォルダにある東方の exe を検出し、thprac をフックした状態で起動します。

## 必要なもの

| ツール | 用途 |
| --- | --- |
| `i686-w64-mingw32-{gcc,g++,windres,objdump}` | 32bit ランチャーのビルド |
| `x86_64-w64-mingw32-{gcc,g++,windres}` | 64bit モジュール / ロケール生成ツールのビルド |
| CMake 3.24 以上 | 64bit モジュールと FreeType のビルド |
| Python 3 | リソース(ペイロード)生成 |
| Wine | ロケール定義の生成(`loc_json.exe`)の実行 |
| `curl`, `tar`, `xz` | FreeType のソース取得と展開 |
| GNU Make | ビルド |

ゲーム本体は別途ご自身で入手・インストールしてください。このリポジトリにはゲームの実行ファイルや素材は含まれません。

## ビルド

```sh
make        # = make all
```

成功すると `build/linux-x64/dist/thprac-th06nc.exe` が生成されます。

```sh
make clean  # build/ 以下を削除
```

### ビルドの流れ

1. Wine で `loc_json.exe` を実行し、ロケール定義(`thprac_locale_def.cpp/.h`)を生成
2. CMake で 64bit モジュール(`thprac_th06nc`)とブリッジ(`thprac_bridge64`)をビルド
3. FreeType 2.13.3 のソースを `build/linux-x86/freetype/` に取得し、32bit 静的ライブラリとしてビルド
4. 32bit ランチャーを静的リンクでビルドし、上記のモジュール類をリソースとして埋め込む
5. 生成した exe が `libstdc++` / `libwinpthread` / `libgcc_s` の DLL に依存していないかを検査

### Make の変数

```sh
make FT_VERSION=2.13.3 FT_URL=https://.../freetype-2.13.3.tar.xz
make MINGW32_CXX=i686-w64-mingw32-g++ WINE=wine
```

主な変数: `MINGW32_CXX` / `MINGW32_CC` / `MINGW64_CXX` / `MINGW64_CC` / `WINDRES32` / `WINDRES64` / `OBJDUMP32` / `WINE` / `PYTHON` / `BUILD_DIR` / `FT_VERSION` / `FT_URL`

初回ビルドでは FreeType のダウンロードにネットワーク接続が必要です。`build/linux-x86/freetype/freetype-<バージョン>.tar.xz` に tarball を置いておけばダウンロードは省略されます。

## 使い方

### ゲームのフォルダに置いて起動する(推奨)

1. `thprac-th06nc.exe` を、ゲームの exe(例: `th10.exe`)と**同じフォルダ**にコピーします。
2. Wine で起動します。

   ```sh
   cd /path/to/th10
   wine thprac-th06nc.exe
   ```

3. 同じフォルダにある既知の東方の exe が見つかると、自動的に thprac をフックしてゲームを起動します。

動作の詳細:

- 検索順は、まず thprac 本体と同じフォルダ、次に thprac を起動したカレントディレクトリです。
- 通常の名前(`thXX.exe`)のゲームを先に探し、その後に名前が異なる exe を探します。
- フックはゲームをサスペンド状態で起動して注入する方式です。失敗した場合は、ゲーム起動後に最大約10秒リトライして再注入します。
- 見つからない場合や、設定で自動起動を無効にしている場合は、通常のランチャー画面を開きます(設定項目: `existing_game_launch_action`)。

### Steam(Proton)から起動する

非 Steam ゲームとして `thprac-th06nc.exe` を追加し、Proton で起動します。ゲームの exe と同じフォルダに置いてください。

### 東方紅魔郷 New Classic

`th06nc` 向けの機能は、64bit モジュールとブリッジ経由で動作します。使い方と機能は [README_NC.md](README_NC.md) を参照してください。

## ディレクトリ構成

| パス | 内容 |
| --- | --- |
| `Makefile` | Linux 向けビルド(`make` / `make all`) |
| `tools/launcher_srcs.txt` | 32bit ランチャーのソース一覧 |
| `tools/make_payload_rc.py` | 埋め込みリソース(ペイロード)の生成 |
| `CMakeLists.txt` | 64bit モジュール / ブリッジ |
| `thprac/src/thprac/` | thprac 本体(ゲーム別モジュール、フック、ランチャー) |
| `thprac/src/thprac/th06nc/` | New Classic 向けの実装 |
| `thprac/src/3rdParties/` | ImGui, MinHook, yyjson, FreeType ヘッダなど |
| `build/linux-x64/` | 64bit 側の中間生成物と最終成果物(`dist/`) |
| `build/linux-x86/` | 32bit ランチャーと FreeType の中間生成物 |

## ライセンスと謝辞

- thprac 本体: Copyright (c) 2022 Ack and thprac contributors, [MIT License](LICENCE)
- サードパーティ: [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) を参照してください。FreeType は FTL に基づいて使用しています(This software is based in part on the work of the FreeType Team)。
- 変更履歴: [CHANGELOG.md](CHANGELOG.md)

東方Projectおよび対応ゲームは上海アリス幻樂団および各権利者に帰属します。本ツールは非公式のファンメイド練習ツールで、ゲームの開発元・発行元、thprac のメンテナー、TWC のいずれにも承認されたものではありません。ゲームの実行ファイルや素材は含みません。
