# サーバ側セットアップ

Tab5 thinclient はSSH接続時に、Tab5側のmicroSDとUSBメモリをSSH先サーバへFUSEで公開します。
サーバ側では以下のパスとして見えます。

- `~/sd`: Tab5 microSD
- `~/usb`: Tab5 USBメモリ

## 必須条件

SSH先サーバには次が必要です。

- Linuxサーバ
- Python 3
- FUSEユーザーマウント
- Python FUSEバインディング
- `/dev/fuse` が利用可能であること
- `fusermount3` または `fusermount` が利用可能であること

## Ubuntu 24.04でのインストール例

今回の検証サーバでは、次のパッケージが必要でした。

```bash
sudo apt-get update
sudo apt-get install -y libfuse2t64 python3-fusepy
```

環境によっては `python3-fusepy` が無い、またはpipで入れる方がよい場合があります。
その場合は次を使います。

```bash
python3 -m pip install --user fusepy
```

ただし `fusepy` だけでは不足することがあります。`Unable to find libfuse` が出る場合は、
OS側のネイティブFUSEライブラリが不足しています。Ubuntu 24.04では `libfuse2t64` を入れてください。

## ファームウェアが自動で行うこと

SSH接続後、Tab5ファームウェアはサーバ側で以下を自動実行します。

- `~/.tab5/bin/tab5-server-setup.sh` へsudo用セットアップスクリプトを展開
- `~/.tab5/bin/tab5-fuse-server.py` へFUSEヘルパーを展開
- 古い `tab5-fuse-server.py` プロセスを停止
- 古い `~/.tab5/mnt` / `~/sd` / `~/usb` のFUSEマウントを解除
- `~/.tab5/mnt` をFUSEマウントポイントとして作成
- `~/sd` を `~/.tab5/mnt/sd` へのシンボリックリンクにする
- `~/usb` を `~/.tab5/mnt/usb` へのシンボリックリンクにする

既に `~/sd` または `~/usb` が通常ディレクトリとして存在する場合は、
上書きせず `~/.tab5/sd.local.<timestamp>` または `~/.tab5/usb.local.<timestamp>` へ退避します。


## 自動セットアップに失敗した場合

Python 3、FUSE、`fusepy` の不足などでTab5側からの自動準備に失敗した場合、ファームウェアはサーバ上に作成済みのsudo用スクリプトと実行コマンドを画面へ表示します。

SSH先サーバで次を実行してください。

```bash
sudo sh ~/.tab5/bin/tab5-server-setup.sh
```

このスクリプトはroot権限が必要なOSパッケージをインストールします。Ubuntu/Debian系では `python3`、`python3-pip`、`fuse3`、`python3-fusepy`、`libfuse2t64` または `libfuse2` を試します。dnf/yum/apk環境では対応するPython/FUSEパッケージを試します。

実行完了後、Tab5から再接続してください。

```text
connect
```

## 動作確認コマンド

SSH先のシェルで以下を実行します。

```bash
ls -al ~/sd
ls -al ~/usb
printf sd-ok > ~/sd/sd_test.txt
printf usb-ok > ~/usb/usb_test.txt
cat ~/sd/sd_test.txt
cat ~/usb/usb_test.txt
```

Tab5のSerial APIでは、同じファイルを次で読み戻せます。

```text
fs read sd:/sd_test.txt 0 64
fs read usb:/usb_test.txt 0 64
```

## 注意点

- FUSEマウントはSSHセッション中のTab5ファームウェアと通信します。Tab5が切断・再起動するとマウントは使えなくなります。
- サーバ側にFUSE権限が無い場合、`~/sd` と `~/usb` は作成されてもTab5実ストレージには接続されません。
- `allow_other` は使っていません。基本的にSSHログインユーザー自身から利用する前提です。
- USBメモリはTab5側でUSB MSCとして認識できる必要があります。認識状態はSerial APIの `fs volumes` で確認できます。
