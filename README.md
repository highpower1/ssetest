# SkyrimUpscaler

Fallout 4 用アップスケーラー [`highpower1/fo4test`](https://github.com/highpower1/fo4test)
を **Skyrim Special Edition / Anniversary Edition** 向けにリバースエンジニアリング
移植する SKSE プラグイン。

**機能:** FSR ／ DLSS ／ DLSS Frame Generation ／ DLSS Ray Reconstruction（Neural）
＋ フォルダ配置した **RenoDX 製 DLL による Neural Rendering** ＋ **ENB 対応**。

---

## ⚠️ 現在の状態（重要）

これは **完成品ではなく、移植の「土台（foundation）」** です。元プロジェクトは
約 16,000 行あり、Fallout 4 のレンダラ内部に深く依存しているため、
**全機能を実機で動作させるには複数段階の作業（各バージョンでの実機テストと
Address Library ID の特定）が必須** です。正直に区別すると:

**この土台で完成している部分**
- CommonLibSSE-NG ベースの **ビルド可能なプロジェクト構成**（SE 1.5.97 + AE 1.6.x、VR/GOG 除外）
- SKSE エントリポイント（1 つの DLL で SE/AE 両対応）
- レンダラ抽象化層（D3D11 デバイス/コンテキストをフックで捕捉：全バージョン安全）
- ENB 検出・連携パスの配線
- **外部 Neural DLL（RenoDX 等）ローダーの完全実装**
- **設定 UI（[SKSE Menu Framework 3](https://github.com/QTR-Modding/SKSE-Menu-Framework-3)）** ＋ INI 設定ストア
- 再利用可能な描画バックエンド（Streamline/FidelityFX/DX12プロキシ）の保全と移植下準備

**残っている作業** → [`PORTING.md`](./PORTING.md) に工程表を明記
- Streamline / FidelityFX SDK の配置
- `Util` / `Upscaling` の Skyrim レンダラへの再マッピング（RE の核心）
- レンダーターゲット index の実機特定
- SKSE 版設定メニュー
- DLSS Ray Reconstruction の GPU 評価配線

---

## 対象バージョン

| ランタイム | 対応 |
|-----------|------|
| Skyrim SE 1.5.97 (Steam) | ✅ |
| Skyrim AE 1.6.x (Steam)  | ✅ |
| Skyrim VR                | ❌ 対象外 |
| GOG 版                    | ❌ 対象外 |

CommonLibSSE-NG の Address Library により、**1 つの DLL が SE と AE の両方**で動作します。

---

## ビルド

```sh
# 依存の取得
git clone <this-repo> SkyrimUpscaler
cd SkyrimUpscaler
git submodule update --init --recursive   # lib/commonlibsse-ng

# （描画バックエンドを有効化する場合のみ）SDK を extern/ に配置:
#   extern/Streamline/            … NVIDIA Streamline SDK
#   extern/FidelityFX-SDK/        … AMD FidelityFX SDK 2.2.0
# そのうえで xmake.lua の "RENDER BACKEND" ブロックを有効化

# 構成（VR は既定で無効）
xmake f -m releasedbg

# ビルド
xmake

# Visual Studio ソリューション生成（任意）
xmake project -k vsxmake
```

必要環境: Windows x64 / Visual Studio 2022 (C++ Desktop) / xmake 2.9+ / Git。

---

## インストール

ビルド成果物と付属ファイルを Skyrim の `Data` 以下へ:

```
Data/SKSE/Plugins/SkyrimUpscaler.dll
Data/SKSE/Plugins/SkyrimUpscaler/          … シェーダ・Streamline ランタイム DLL
Data/SKSE/Plugins/SkyrimUpscaler/Neural/   … RenoDX 製など外部 Neural DLL を配置
Data/SKSE/Plugins/FrameGeneration/         … フレーム生成用シェーダ
```

**必須の前提 Mod:** 設定 UI には
[SKSE Menu Framework 3](https://github.com/QTR-Modding/SKSE-Menu-Framework-3)
が必要です（`Data/SKSE/Plugins/SKSEMenuFramework.dll`）。ゲーム内の Mod Control Panel
の「SkyrimUpscaler / Settings」ページから設定できます。未導入でも本体は動作し、
設定は INI から読み込まれます。

---

## Neural Rendering（外部 DLL）

`Data/SKSE/Plugins/SkyrimUpscaler/Neural/` に置いた DLL が自動ロードされます。
DLL 側は次の 1 関数を export するだけで組み込めます:

```c
const SkyrimUpscalerNeuralModuleV1* SkyrimUpscalerNeural_GetModuleV1(void);
```

ABI 定義は [`src/Neural/NeuralRendering.h`](./src/Neural/NeuralRendering.h) を参照。

---

## ライセンス

元プロジェクトが GPL-3.0 のため本移植も **GPL-3.0**。NVIDIA Streamline / AMD
FidelityFX / DLSS モデルは各社ライセンスに従います（`package/` 内 license ファイル参照）。
