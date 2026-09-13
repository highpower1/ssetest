# SkyrimUpscalerProbe — 検証用ログ収集プラグイン

実機の Skyrim から、移植に必要な情報（レンダラ構成・レンダーターゲット一覧・
各テクスチャの解像度/フォーマット/ビュー・スワップチェイン・ENB有無）を
ログに吐き出す、**軽量な単体SKSEプラグイン**です。

進行中の本体（Upscaler/Menu 等）とは切り離してあるので、**単体でビルドして起動確認**できます。

---

## 1. ビルド

必要環境: Windows x64 / Visual Studio 2022（C++デスクトップ開発）/ [xmake](https://xmake.io) 2.9+ / Git

```sh
cd SkyrimUpscaler

# CommonLibSSE-NG は lib/commonlibsse-ng に同梱済み（追加取得不要）

# 構成（VR無効・SE/AE対象）
xmake f -m releasedbg -y

# 診断プラグインだけをビルド
xmake build SkyrimUpscalerProbe
```

出力: `build/windows/x64/releasedbg/SkyrimUpscalerProbe.dll`

> うまくいかない時は、まず `xmake build SkyrimUpscalerProbe` の**エラーメッセージ全文**を
> そのまま貼ってください。こちらで直します。

---

## 2. インストール

```
<Skyrimインストール先>/Data/SKSE/Plugins/SkyrimUpscalerProbe.dll
```

MO2 等を使っている場合は、`SkyrimUpscalerProbe.dll` を含む
`Data/SKSE/Plugins/` 構造を Mod として追加でもOKです。

---

## 3. 実行してログを取る

1. Skyrim（SKSE経由 `skse64_loader.exe`）を起動
2. **セーブデータをロードして実際にゲーム世界に入る**（重要：ゲーム内RTを埋めるため）
3. そのまま **30秒ほど待つ**（自動で2回目のダンプが走ります）
4. ゲームを終了

ログの場所:

```
Documents/My Games/Skyrim Special Edition/SKSE/SkyrimUpscalerProbe.log
```

（AE も同じフォルダ。環境により `Skyrim Special Edition GOG` の場合あり）

**この `SkyrimUpscalerProbe.log` を丸ごと貼ってください。**

---

## 4. ログに何が出るか（例）

```
======== SkyrimUpscaler diagnostics dump [kDataLoaded] ========
Module base: 0x7FF6...
Runtime version: 1.6.1170.0
ENB present: yes
RendererData: device(forwarder)=0x... context=0x...
BSGraphics::State screen: 2560x1440 (insideFrame=1)
Swapchain: 2560x1440 fmt=28 (R8G8B8A8_UNORM) buffers=1 windowed=1 flags=0x2
--- Render targets (RE::RENDER_TARGET::kTOTAL = 124) ---
RT[  0] kFRAMEBUFFER               2560x1440 fmt=... | RTV=y SRV=y UAV=- copy=-
RT[  1] kMAIN                      2560x1440 fmt=... | ...
RT[  7] kMOTION_VECTOR             2560x1440 fmt=... | ...
...
--- Depth-stencil targets ---
DS[ 0] 2560x1440 fmt=... depthSRV=y ...
======== end diagnostics dump [kDataLoaded] ========
```

これで **どのレンダーターゲットがどの解像度・形式で存在するか**が分かり、
アップスケーラのレンダーターゲット・スケーリング移植（`PORTING.md` (e)）を
正確に進められます。

## 5. まだ取れないもの（次段）
- カメラのビュー/射影行列・TAAジッター … これらは `BSGraphics::State` に露出して
  おらず、レンダーフックで捕捉する必要があります（次の検証ビルドで追加予定）。
