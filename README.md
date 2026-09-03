# 簡易録音2 (EasyRec2)

AviUtl ExEdit2 (AviUtl 2) のウィンドウ内からマイク録音を行う64bit汎用プラグインです。

配布はGitHub Releasesの `.au2pkg.zip` を正本とし、AviUtl2 カタログへの掲載を予定しています。パッケージ識別子 `EasyRec2` は更新時も固定します。

- 配布: [GitHub Releases](https://github.com/Uncre/aviutl2-easyrec2/releases)
- 不具合・要望: [GitHub Issues](https://github.com/Uncre/aviutl2-easyrec2/issues)

## 主な機能

- Windows WASAPI共有モードによる録音（デバイスの既定フォーマットを自動利用）
- WASAPIを開始できない機器ではWindows互換録音へ自動フォールバック
- WAV / FLAC / MP3 / AAC (M4A) / Opus 出力
- 44.1 / 48 / 96 / 192 kHz、モノラル / ステレオ出力
- 形式ごとのビット深度・圧縮率・ビットレート選択
- 保存先を明示的に選択・記憶
- ミリ秒付き日時ファイル名と重複時の連番により上書きを防止
- 録音開始時のカーソル位置へ自動追加。使用中なら下の空きレイヤーを探索
- 録音準備完了後、録音開始時のカーソル位置からプレビューを自動再生
- 音声ファイルの実時間からタイムライン上の長さを計算
- 録音時間を0.1秒単位でリアルタイム表示
- FFmpegの終了を待ってからタイムラインへ追加するため、未完成ファイルを読ませない

## 必要なもの

- Windows 10/11 64bit
- AviUtl ExEdit2 2.1.0以降
- 64bit版 `ffmpeg.exe`

`ffmpeg.exe` は次の順で自動検出します。

1. `EasyRec2.aux2` と同じフォルダー
2. WindowsのPATH
3. 画面で手動指定した場所

FFmpegを同梱していないのは、配布サイズとライセンスを利用者が選べるようにするためです。

## インストール

### AviUtl2 カタログ（掲載後）

AviUtl2 カタログで「簡易録音2」を検索し、インストールしてください。GitHub Releasesから入手した更新版もカタログから更新できます。

### 手動

GitHub Releasesから `EasyRec2-vX.Y.Z.au2pkg.zip` をダウンロードし、ZIPを展開せずにAviUtl 2のプレビュー画面へドラッグ＆ドロップしてください。AviUtl 2を再起動し、「表示」メニューから「簡易録音2」を開きます。

自動追加する形式は、AviUtl 2側の入力プラグインでも読める必要があります。標準構成で読めないFLAC / Opus等には、対応する入力プラグインを導入してください。録音ファイル自体の保存は入力プラグインがなくても成功します。

「録音開始と同時に現在位置からプレビュー再生」は、AviUtl 2の既定ショートカットであるSpaceキーを利用します。Spaceキーの割り当てを変更している場合は自動再生できないため、録音開始後に設定済みの再生キーを押してください。

## ビルド

Visual Studio 2022の「C++によるデスクトップ開発」をインストールし、PowerShellで次を実行します。

```powershell
.\build.ps1 -Configuration Release
```

生成物は `build\EasyRec2.aux2` です。

配布用の `.au2pkg.zip` とSHA-256チェックサムを作る場合は次を実行します。

```powershell
.\package.ps1 -Version 0.1.2
```

生成物は `dist` フォルダーに出力されます。`vX.Y.Z` タグをGitHubへプッシュすると、GitHub Actionsが同じ処理を行い、GitHub Releaseを自動作成します。

## 不具合報告

GitHub Issuesへ、AviUtl 2のバージョン、EasyRec2のバージョン、再現手順、ログ、使用した音声形式を添えて報告してください。録音ファイルやログに個人情報が含まれる場合は、公開前に必ず除去してください。

## 旧「簡易録音」との関係

これは旧 `rec.auf` の逆コンパイルや改変ではなく、公開されているAviUtl ExEdit2 Plugin SDKを使った新規実装です。旧版の保存先が想定外になる問題、ファイル名衝突の危険、録音機器が指定形式を直接サポートしないと開始できない制約を設計上回避しています。

## ライセンス

EasyRec2のソースコードはMIT Licenseです。AviUtl ExEdit2 Plugin SDKもMIT Licenseで、`third_party/aviutl2_sdk/license.txt` にライセンスを収録しています。
