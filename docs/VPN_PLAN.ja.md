# VPNログイン機能 実装・実測計画

対象: Tab5 SSH Client ファームウェア (ESP32-P4)
目的: Tab5 から特定のゲートウェイサーバへ VPN 接続し、そのサーバ経由で
Tailscale ネットワーク (tailnet) 内のホストへ SSH できるようにする。

```text
[Tab5] --Wi-Fi--> [インターネット] --WireGuard--> [ゲートウェイサーバ]
                                                     |  tailscaled
                                                     v
                                              [tailnet 100.64.0.0/10]
                                                     |
                                                     v
                                              [目的のSSHホスト]
```

## 1. 方式検討

### 1.1 前提

Tailscale クライアント本体 (tailscaled) は Go 製で、ESP32-P4 上では動かせない。
そのため Tab5 自身を tailnet のノードにするのではなく、**Tab5 と 1 台の
ゲートウェイサーバの間に素の WireGuard トンネルを張り、ゲートウェイが
tailnet へのルータになる**構成を採る。

### 1.2 候補比較

| 方式 | Tab5側の実装 | サーバ側 | 評価 |
|---|---|---|---|
| A. WireGuard トンネル + ゲートウェイで tailnet へ NAT | lwIP 用 WireGuard 実装を組み込み | wg + tailscale + IP forwarding | **採用**。標準的で部品が揃っている |
| B. SSH 多段 (ProxyJump 相当) | 既存 SSH のみ | sshd のみ | VPN ではないが、現状でも `ssh gateway` → `ssh 100.x` で実現可能。フォールバックとして文書化 |
| C. Tailscale ノード化 | 不可能 (tailscaled が動かない) | — | 不採用 |
| D. OpenVPN / IPsec | ESP32 系実装が重い・保守性低 | — | 不採用 |

方式 A の Tab5 側実装は、lwIP の WireGuard 実装
([ciniml/WireGuard-ESP32-Arduino](https://github.com/ciniml/WireGuard-ESP32-Arduino)、
ESP-IDF 向けの [trombik/esp_wireguard](https://github.com/trombik/esp_wireguard))
をベースにする。どちらも lwIP netif として実装されているため、既存の
LibSSH-ESP32 のソケットはトンネル経由のルーティングにそのまま乗る。
ESP32-P4 (RISC-V) + pioarduino コアでの動作実績は未確認のため、
最初のマイルストーンで移植性を検証する (§3 M0)。

## 2. 実装計画

### 2.1 プロファイルとストレージ

`profiles.json` に `vpn` セクションを追加する。

```json
"vpn": [
  {
    "name": "tailscale-gw",
    "endpoint": "vpn.example.com",
    "port": 51820,
    "privateKey": "(Tab5のWG秘密鍵)",
    "publicKey": "(サーバのWG公開鍵)",
    "presharedKey": "",
    "address": "10.100.0.2",
    "allowedIps": "100.64.0.0/10,10.100.0.0/24",
    "dns": "100.100.100.100",
    "keepalive": 25
  }
]
```

- 秘密鍵はパスワードと同様にフラッシュ (LittleFS) に保存し、UI ではマスク表示。
- 公開リポジトリには実鍵をコミットしない (`profiles.local.json` 運用を踏襲)。

### 2.2 UI / CLI

- ヘッダメニューに `VPN` を追加 (`CONF` の右、Rect は 44px グリッドに追従)。
- 画面: Wi-Fi プロファイルと同型の一覧 + 編集画面
  (`Name / Endpoint / Port / PrivKey / PubKey / PSK / Address / AllowedIPs / DNS / Keepalive`)。
- ヘッダ状態表示を `WiFi ok  VPN ok  SSH ok` の 3 セグメントに拡張。
- CLI コマンド:

```text
vpn list
vpn connect <index|name>
vpn disconnect
vpn status          # handshake時刻, endpoint, 転送量, 経路
```

- SSH プロファイルに `vpn` フィールド (任意) を追加し、`CONNECT` 時に
  指定 VPN が未接続なら先に自動接続する (VPNログイン → SSHログインの連鎖)。

### 2.3 ファームウェア変更点

| # | 変更 | 対象 |
|---|---|---|
| 1 | WireGuard lwIP モジュールの組み込みと P4 対応パッチ | `lib/` or `lib_deps` |
| 2 | `VpnProfiles` (WifiProfiles と同型) + SettingsStore 拡張 | `src/`, `include/` |
| 3 | 接続ステートマシン (idle → handshake → up → retry) | `src/main.cpp` |
| 4 | 既定経路は物理 I/F のまま、AllowedIPs のみトンネルへ (split tunnel) | lwIP ルーティング |
| 5 | DNS: トンネル接続中は MagicDNS (100.100.100.100) を優先リゾルバに追加 | `dns_setserver` |
| 6 | UI 画面 / ヘッダ / CLI / シリアル診断コマンド | `src/main.cpp` |
| 7 | ドキュメント (`docs/VPN_SETUP.ja.md`: サーバ構築手順含む) | `docs/` |

### 2.4 ゲートウェイサーバ側の設定 (ドキュメント化する内容)

```text
# WireGuard 受け口
wg genkey / wg-quick up wg0      # 10.100.0.1/24, ListenPort 51820

# tailnet への転送
sysctl net.ipv4.ip_forward=1
iptables -t nat -A POSTROUTING -s 10.100.0.0/24 -o tailscale0 -j MASQUERADE

# 逆方向は NAT なので追加経路広告は不要
# MagicDNS を使う場合: Tab5 の DNS を 100.100.100.100 に向け、
# UDP 53 を tailscale0 へフォワードするか、gateway 上で dnsmasq を中継させる
```

注意: tailnet 側 ACL で「gateway ノード → 目的ホスト:22」が許可されている
必要がある。SSH 元はあくまで gateway ノードとして扱われる。

## 3. 実測計画 (マイルストーンと測定項目)

各マイルストーンは前段が green になってから進める。測定はすべて
シリアル診断 (`115200`) のログとサーバ側 `wg show` / `tcpdump` で行う。

### M0. ライブラリ移植性検証 (PoC)

- `WireGuard-ESP32-Arduino` を Tab5 環境 (pioarduino / ESP32-P4) でビルド。
- 合否: コンパイル・リンク成功、`wireguardif_init` が起動時にクラッシュしない。
- 失敗時: trombik/esp_wireguard へ切替、それも不可なら方式 B (SSH多段) に転進。

### M1. ハンドシェイク確立

- 測定: handshake 完了までの時間 (10 回試行の中央値 / 最大)、失敗率。
- 目標: 中央値 < 3 s、失敗率 < 10% (Wi-Fi 良好時)。
- 確認: サーバ `wg show` の latest handshake、鍵不一致時のエラー表示。

### M2. トンネル疎通

- 測定: `ping 10.100.0.1` (トンネル内 RTT) vs 物理経路 RTT → オーバーヘッド算出。
- 測定: `ping 100.x.y.z` (tailnet ホスト) 成功率。
- 目標: トンネル RTT 増分 < 20 ms (同一リージョン)。

### M3. VPN 経由 SSH ログイン

- 測定: `ssh connect` からプロンプト表示までの時間 (VPN あり/なし比較)。
- 測定: `vim` / `htop` / `sl` の描画体感、キー入力遅延。
- 目標: ログイン完了 < 8 s、対話操作で顕著な引っかかりがない。

### M4. スループットと実用負荷

- 測定: `scp get` / `scp put` の実効速度 (1 MB / 10 MB、VPN あり/なし)。
- 測定: FUSE ストレージマウント経由のファイル操作の成否と速度。
- 測定: CPU 使用率・空きヒープ (`status` 出力に計測値を追加)。
- 目標: VPN 経由スループットが素の SSH の 50% 以上、ヒープ残 > 80 KB。

### M5. 安定性・運用

- 測定: 8 時間連続接続での切断回数、keepalive による NAT 維持。
- 測定: Wi-Fi 切断→復帰時の自動再ハンドシェイク時間。
- 測定: 電池での連続稼働時間 (VPN あり/なし)。
- 確認: 鍵・エンドポイント誤設定時のエラーメッセージが UI で判別できること。

### 記録フォーマット

計測結果は `docs/VPN_MEASUREMENTS.md` に以下の表で記録する。

| 日付 | FWコミット | 項目 | 条件 | 結果 | 判定 |
|---|---|---|---|---|---|
| 2026-08-XX | abc1234 | M1 handshake | 自宅Wi-Fi / RTT 8ms | 中央値 1.9s | OK |

## 4. リスクと対策

| リスク | 影響 | 対策 |
|---|---|---|
| WireGuard 実装が P4/lwIP バージョンでビルド不可 | 方式A不成立 | M0 で早期判定、trombik 版 / lwIP 本家 contrib へ切替、最終手段は方式B |
| RAM 逼迫 (SSH + MicroPython + WG 同時) | 接続不安定 | WG バッファを PSRAM へ、`status` にヒープ監視を追加 |
| NAT 越え失敗 (キャリアNAT等) | handshake 不可 | keepalive 25s、エンドポイント側ポート開放を前提条件として文書化 |
| MagicDNS 名前解決が複雑化 | ホスト名で繋げない | 初期リリースは 100.x 直指定を正とし、DNS は任意機能に |
| 鍵管理ミス (実鍵のコミット) | 認証情報漏洩 | `profiles.local.json` 運用の徹底、README に警告、flash ツールの既存マスク機構を流用 |

## 5. スコープ外 (今回やらないこと)

- Tab5 自身の tailnet ノード化 (tsnet / tailscaled の移植)
- OpenVPN / IPsec / L2TP 対応
- 複数 VPN の同時接続
- exit node としての利用 (全トラフィックのトンネリング)
