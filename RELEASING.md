# 公開・リリース手順

## 推奨する公開先

ソースコードと配布ファイルはGitHubで公開し、配布ファイルはGitHub Releasesへ置きます。利用者向けの入口としてAviUtl2 カタログへ登録します。

推奨リポジトリ名は `aviutl2-easyrec2` です。

## 初回公開

1. GitHubでPublicリポジトリを作成し、このフォルダーの内容をpushします。
2. `aviutl2.toml` の `[catalog]` が公開先 `Uncre/aviutl2-easyrec2` を指していることを確認します。
3. `v0.1.2` タグをpushします。
4. GitHub Actionsが `EasyRec2-v0.1.2.au2pkg.zip` と `SHA256SUMS.txt` を作成し、GitHub Releaseへ添付します。
5. 実際のAviUtl 2へ `.au2pkg.zip` をドラッグ＆ドロップし、インストールと起動を最終確認します。
6. AviUtl2 カタログのアプリ内「パッケージ登録」から申請します。

カタログ申請時の推奨値は次のとおりです。

- 種類: 汎用プラグイン
- 名前: 簡易録音2
- 概要: 動画を再生しながらアフレコし、WAV/FLAC/MP3/AAC/Opusで保存・自動挿入
- タグ: 音声、便利ツール
- 説明: `docs/catalog-description.md`
- GitHub Release assetのパターン: `^EasyRec2-v(?<version>\d+\.\d+\.\d+)\.au2pkg\.zip$`
- カタログID: `Uncre.easyrec2`

## 通常の更新

1. `CMakeLists.txt`、`aviutl2.toml`、`package/package.ini`、`package/package.txt` のバージョンを更新します。
2. `CHANGELOG.md` に変更内容を追記します。
3. ローカルで `./package.ps1 -Version X.Y.Z` を実行し、生成物を確認します。
4. 変更をpushし、`vX.Y.Z` タグをpushします。

`package.ini` の `id=EasyRec2` は既存インストールの更新判定に使われるため、今後も変更しません。
