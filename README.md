# DX12 ECS — Vampire Survivors 風デモ

**日本語** | [English](README.en.md)

**EnTT による ECS**、**マルチスレッド**、**DirectX 12 の描画パフォーマンス** を、Vampire Survivors 風の見下ろし型ゲームデモを通して研究する個人プロジェクトです。

![スクリーンショット](ECS_study.png)

> 現在も開発中です。新しいシステムや最適化を継続的に追加しています。

---

## 目的

リアルタイムのゲームエンジンで重要となる技術を、自主的に学ぶためのプロジェクトです。

- **Entity-Component-System（EnTT）** — データ指向の設計、キャッシュ効率の良いコンポーネント配置
- **ジョブシステム / スレッドプール** — 敵 AI を 128 エンティティ単位のバッチに分け、ワーカースレッドで並列に処理
- **GPU 側のカリング** — コンピュートシェーダーでフラスタム判定を行い、`ExecuteIndirect` により CPU へのリードバックなしで見えない描画コールを削減
- **DirectX 12** — 明示的なリソース管理、ルートシグネチャ、ディスクリプタヒープ、PSO、コマンドリストの記録

---

## 機能

| システム | 内容 |
|---|---|
| ECS（EnTT） | プレイヤー、敵、弾、経験値オーブの各エンティティ。コンポーネントベースの更新ループ |
| スレッドプール | `std::thread` によるワーカー。毎フレーム、敵 AI を並列ジョブとして実行 |
| インスタンシング描画 | 構造化バッファを使い、全エンティティを 1 回の `DrawIndexedInstanced` で描画 |
| GPU フラスタムカリング | インスタンスごとに 1 スレッドを CS でディスパッチ。可視インスタンスを `InterlockedAdd` で詰めて格納し、描画引数を GPU 上で生成して `ExecuteIndirect` で描画 |
| ImGui デバッグパネル | FPS、エンティティ数、敵の数、スレッド数、プレイヤーの HP / レベルバー、最大敵数スライダー、GPU カリングの切り替えをリアルタイムに表示 |

---

## 技術スタック

- **言語**：C++20
- **レンダラー**：DirectX 12（Windows）
- **ECS**：[EnTT](https://github.com/skypjack/entt)（シングルヘッダー、`third_party/` に同梱）
- **UI**：Dear ImGui（DX12 バックエンド、`VampireSurvivor/ImGUI/` に同梱）
- **ビルド**：Visual Studio 2022、MSBuild。HLSL シェーダーはビルド時に `.cso` へコンパイル

---

## ビルド方法

1. **Visual Studio 2022** で `D3D12HelloWorld.sln` を開く
2. **VampireSurvivor** プロジェクトを選択し、プラットフォームを **x64**、構成を **Debug** または **Release** にする
3. ビルドする（シェーダーはビルド前のステップで自動的にコンパイルされます）
4. Visual Studio から実行する（F5）か、`VampireSurvivor/bin/x64/Debug/` を開いて起動する

**必要環境**：Windows 10/11、DirectX 12 対応の GPU、Visual Studio 2022（C++ デスクトップ開発ワークロードと Windows SDK 10.0 以上）

---

## プロジェクト構成

```
D3D12HelloWorld.sln
VampireSurvivor/
  src/
    ecs/          コンポーネント、World、SystemGraph
    game/         GameApp（メインループ）、各システム（AI、戦闘、経験値）
    jobs/         ThreadPool、TaskGraph
    rendering/    InstancedRenderer（CPU パス + GPU カリングパス）
    framework/    DX12 の基底クラス（DXSample、Win32Application）
  shaders/
    instance.hlsl        インスタンス描画用の VS + PS
    gpu_cull_cs.hlsl     コンピュートシェーダーによるフラスタムカリング + 間接描画引数の生成
  ImGUI/                 Dear ImGui のソース（DX12 バックエンド）
third_party/
  entt/entt.hpp          EnTT シングルヘッダー ECS ライブラリ
```

---

## ロードマップ

- [x] EnTT による ECS アーキテクチャ
- [x] スレッドプールによる AI の並列化
- [x] インスタンシング描画（CPU パス）
- [x] `ExecuteIndirect` を使った GPU フラスタムカリング
- [x] ImGui のリアルタイムデバッグパネル
- [ ] パフォーマンス計測データ（エンティティ数ごとの FPS、シングルスレッドとマルチスレッドの比較）
- [ ] PIX / Tracy によるプロファイリングの導入
- [ ] 弾と敵の衝突判定への空間ハッシュの導入
- [ ] ウェーブ制の敵スポナー
