# fo4test → SkyrimUpscaler 移植ロードマップ

Fallout 4 用アップスケーラー [`jarari/fo4test`](https://github.com/jarari/fo4test)（大本。
ローカルクローンは fork の `highpower1/fo4test`＝同一コード）
を、**Skyrim SE 1.5.97 / AE 1.6.x**（VR・GOG は対象外）で動作する SKSE プラグインへ
リバースエンジニアリング移植するための工程表。

対象機能: **FSR / DLSS / DLSS Frame Generation / DLSS Ray Reconstruction（Neural）**
＋ **外部 RenoDX 製 DLL による Neural Rendering** ＋ **ENB 対応**。

---

## 1. 依存の分離（調査済み）

`fo4test/src` 全ファイルの `RE::` / `F4SE` 参照数を実測して分類した結果:

| 分類 | ファイル | 状態 |
|------|----------|------|
| **完全移植可能**（RE/F4SE ゼロ） | `DX11Hooks` `DX12SwapChain` `D3D12UIComposite` `TaggedTextureDebug` `OSD` `FidelityFX.h` `FrameCount.h` | `src/Render/` へコピー済み。SDK 配置後そのままビルド可 |
| **軽微な適応のみ** | `Buffer.h`(22) `Streamline.cpp`(2) `FidelityFX.cpp`(1) | `RE::BSGraphics::GetRendererData()->device/context` を `Game::GetRendererData()` へ置換するだけ |
| **本格 RE 作業** | `Upscaling.cpp`(76) `Upscaling.h`(5) `Util.cpp/h` | FO4 固有のレンダーターゲット列挙・Address Library ID の再マッピングが必要 |
| **UI 差し替え** | `UpscalingMenu` + `F4SEMenuFramework.h` | ✅ **移植済み** → `src/UI/Menu.cpp`（[SKSE Menu Framework 3 / QTR-Modding](https://github.com/QTR-Modding/SKSE-Menu-Framework-3)）|

> **UI メモ:** F4SE Menu Framework 3 と SKSE Menu Framework 3 は同じ `ImGuiMCP`
> バインディング名前空間を共有するため、ウィジェット本体はそのまま流用できた。
> 差分は登録 API（`F4SEMenuFramework::` → `SKSEMenuFramework::`）とライフサイクル
> イベントのみ。設定は未移植の `Upscaling` から切り離した `SettingsStore` 経由で
> 読み書きする（`src/Settings/`）。

FO4 固有ファイルは `src/Game/*.fo4ref` として**参照用に保全**してある。

---

## 2. 完了済み（この土台）

- [x] CommonLibSSE-NG サブモジュール構成の `xmake.lua`（SE+AE、VR 無効化）
- [x] `PCH.h` / `Plugin.h` を F4SE→SKSE へ移植
- [x] `main.cpp`: SKSE エントリ（`SKSEPlugin_Version` で SE/AE 両対応）、ENB 検出、メッセージング
- [x] `Game::Renderer`: D3D11 デバイス/コンテキストを **`D3D11CreateDeviceAndSwapChain` フックで捕捉**（構造体レイアウト非依存＝全バージョン安全）
- [x] `Hooks/DX11Hooks`: デバイス捕捉＋feature level 11_1 強制（軽量版）
- [x] `Neural/NeuralRendering`: **外部 RenoDX 製 DLL ローダーを完全実装**（C ABI 定義済み）＋ DLSS-RR スキャフォールド
- [x] **UI: SKSE Menu Framework 3 で設定ページを実装**（`src/UI/Menu.cpp` + `src/Settings/`）
- [x] **`Util` の移植（前半）**（`src/Game/Util.*`）: 数学ヘルパ・FOV抽出・カメラ射影/基底・`CompileShader` を移植。FO4の `cameraDataCache` 依存は、レンダーフックが毎フレーム埋める `Util::CameraFrame` ホルダへ置換（デバイス捕捉と同じ設計）
- [x] **`Upscaling` 制御層の移植**（`src/Upscaler/Upscaling.*`）: メソッド選択（`GetUpscaleMethod` ＋ DLSS→FSR フォールバック）・フレーム生成判定・メニューイベントシンク（`RE::MenuOpenCloseEvent`）・カメラ注入契約（`SetCameraFrame`）を移植。バックエンド呼び出しは可用性フラグ（`featureDLSS`/`featureDLSSG`/`dx12Ready`）へ分離しビルド可能に
- [x] ENB SDK ヘッダ・Detours・HLSL シェーダを移植

---

## 3. 残作業（次セッション以降）

### (a) SDK 配置
`extern/Streamline/`（NVIDIA Streamline 2.x）と
`extern/FidelityFX-SDK/`（AMD FidelityFX SDK 2.2.0）を配置し、`xmake.lua` の
「RENDER BACKEND」ブロックのコメントを解除する。

### (b) `Util` の Skyrim 化 — ⚙️ 前半完了 / 残: カメラ注入フック
`src/Game/Util.*` に移植済み（数学・FOV・射影/基底・`CompileShader`）。FO4 は
`RE::BSGraphics::State::cameraDataCache` の各エントリ（`camViewData` の
`viewMat`/`viewProjUnjittered`/`projMat`）を読んでいたが、Skyrim ではこの構造体
レイアウトが異なるため**捏造せず**、`Util::CameraFrame` ホルダを設けてそこから読む
方式に変更した（`Game::Renderer` のデバイス捕捉と同じ思想）。

**残タスク**: `Util::CameraFrame::GetSingleton()` を毎フレーム埋めるフックを
`Upscaling` 移植時に追加する。Skyrim でのカメラ行列・TAA ジッターの取得元候補:
- `RE::BSGraphics::State::GetSingleton()`（現在のビュー/射影・ジッター）
- `RE::PlayerCamera::GetSingleton()` / `RE::NiCamera`（ワールド変換・視錐台 FOV）
- レンダーターゲット群は `RE::BSGraphics::Renderer::GetSingleton()` と
  `RE::RENDER_TARGET`（`Util::RenderTarget` に主要ターゲットを列挙済み）

### (c) レンダーターゲット index の再導出 — ⚙️ 実機ダンプで確定
`SkyrimUpscalerProbe` の実機ログ（AE 1.6.1170 / ENB / 1920x1080）で確定:
- 全RTが**ネイティブ解像度で確保**（→シーン描画中に低解像度プロキシへ差し替える方式）
- kMAIN(1)=R16G16B16A16_FLOAT（カラー入力）/ kMOTION_VECTOR(7)=R16G16_FLOAT /
  kNORMAL_TAAMASK_SSRMASK(4) / kTEMPORAL_AA_ACCUMULATION_1/2(81/82) /
  kTEMPORAL_AA_MASK(85) / kSSR系(109/110/111, 半解像度)
- スケール候補集合は `Util::kScaledRenderTargets` に反映済み
- **未確定**: メイン深度は `depthStencils[]` に期待した形で入っていなかった（要フック捕捉）。
  厳密なスケール対象集合は RenderDoc で最終確認。

### (d) `Buffer.h` / `Streamline.cpp` / `FidelityFX.cpp` の置換適用
`RE::BSGraphics::GetRendererData()` → `Game::GetRendererData()` へ機械置換し、
`#include "Game/Renderer.h"` を追加。DLL パス `Data\F4SE\Plugins\Upscaling\` →
`Data\SKSE\Plugins\SkyrimUpscaler\` へ変更。

### (e) `Upscaling` GPU 本体 ＋ エンジンフック（最大の作業）— ⚙️ 制御層のみ完了
制御層（メソッド選択・設定・メニュー・カメラ契約）は `src/Upscaler/Upscaling.*`
に移植済み。**残り**は GPU 本体（レンダーターゲットのスケーリング／override・reset、
深度/モーションベクター生成、DLSS/FSR 評価、約8000行）と、それを駆動する
`InstallHooks()` の **Skyrim 用 Address Library ID テーブル**（現状は足場でIDは未設定）。
FO4 の約20フックの目的は `Upscaling::InstallHooks()` のコメントに列挙済み。各フック点
（TAA無効化、動的解像度＋ジッター、サンプラmipバイアス、MV/深度キャプチャ、SSR/レンズ
フレア/被写界深度のdyn-res補正、ENB合成handoff、最終アップスケール評価）の Skyrim ID を
SE 1.5.97 / AE 1.6.x それぞれで実機特定する。カメラ設定フックからは毎フレーム
`Upscaling::SetCameraFrame(...)` を呼んで `Util::CameraFrame` を満たす。

### (f) DLSS Ray Reconstruction（Neural (A)）
`Streamline` ラッパで DLSS super-resolution の代わりに **DLSS-D（RR）プリセット**を
選択し、ガイドバッファ（albedo / normal-roughness / specular hitdist）をタグ付け。
必要ランタイム（`sl.dlss_d.dll` / `nvngx_dlssd.dll` / `nvngx_dlssnr.dll`）は同梱済み。
`NeuralRendering::Initialize()` の RR 分岐から `Streamline::GetSingleton()` を呼ぶ。

### (g) SKSE メニュー — ✅ 完了
`src/UI/Menu.cpp` が SKSE Menu Framework 3 で設定ページを提供。設定は
`Data/SKSE/Plugins/SkyrimUpscaler/SkyrimUpscaler.ini` に保存。
残タスクは (e) の `Upscaling` 移植後に `SettingsStore` と実際のアップスケーラ状態
（Streamline 可用性表示など）を接続すること。

---

## 4. 外部 Neural DLL の C ABI（実装済み）

RenoDX などで作った DLL を `Data/SKSE/Plugins/SkyrimUpscaler/Neural/*.dll` に置くと
自動ロードされる。DLL 側は次の 1 関数を export するだけ:

```c
const SkyrimUpscalerNeuralModuleV1* SkyrimUpscalerNeural_GetModuleV1(void);
```

戻り値の `Init / Evaluate / Shutdown` コールバックがフレーム毎に呼ばれる
（詳細は `src/Neural/NeuralRendering.h`）。ReShade アドオン形式
（`ReShadeRegisterAddon` を export）の DLL は検出して警告を出す
（ReShade ランタイムのホスティングは別課題）。
