# SmoothScroll

Windows (10以降) / Precision Touchpad 向けの Mac風スムーススクロール・
慣性スクロール DLL。パン(スクロール)・ピンチズーム・オーバースクロール
(ラバーバンド + バネ)を提供します。

- 指を離した後の慣性(指数減衰)スクロール
- Ctrl+ホイール(ピンチ互換シグナル)によるカーソル中心のスムーズなズーム
- 境界でのラバーバンド抵抗 + バネで押し戻すオーバースクロール演出
- タッチ中の入力は待ち時間なくそのまま反映(物理ループスレッドを経由しない)

## 構成

```
SmoothScroll/
  SmoothScroll.h          … 公開C API
  SmoothScroll.cpp         … DLL本体(サブクラス化 + 物理ループスレッド)
  SmoothScrollHelper.h      … C++向け薄いラッパー(スレッド安全なコールバック配送、推奨)
  PhysicsEngine.h / .cpp   … 慣性の物理モデル
demo/
  main.cpp                 … 利用例(グリッドスクロール + オーバースクロール + ズーム)
CMakeLists.txt
```

## ビルド

```
cmake -B build
cmake --build build
```

`SmoothScroll.dll`(共有ライブラリ)と、動作確認用の`SSDemo.exe`が生成されます。
`SSDemo`を実行すると、斜めスクロール・端でのラバーバンド/バネ・Ctrl+ホイールでの
ズームを試せます。

## クイックスタート

C++で組み込む場合は、生のC APIを直接叩くのではなく `SmoothScrollHelper` の
利用を推奨します(コールバックの呼び出し元スレッドの違いを自動で吸収します)。

```cpp
#include "SmoothScrollHelper.h"

SmoothScrollHelper g_ssHelper;

case WM_CREATE: {
    SS_Config cfg; ss_default_config(&cfg);
    cfg.enable_zoom = TRUE;
    g_ssHelper.Attach(hwnd, &cfg,
        [](float dx, float dy) {
            // 常にUIスレッド上で呼ばれる。直接UI更新してよい。
            g_scrollX += dx; g_scrollY += dy;
            InvalidateRect(hwnd, nullptr, FALSE);
        },
        [](float scale, float ax, float ay) {
            // ズームコールバック(不要なら省略可)
        });
    return 0;
}

case SmoothScrollHelper::kScrollMessage:
case SmoothScrollHelper::kZoomMessage:
    g_ssHelper.HandleMessage(msg, wParam, lParam);
    return 0;

case WM_DESTROY:
    g_ssHelper.Detach();
    ...
```

境界でのラバーバンド抵抗やバネによる押し戻しはホスト側の責務です。
`ss_rubberband_delta()` / `ss_spring_step()` / `ss_spring_settled()` という
状態を持たない純粋関数が提供されているので、これらを使って自前のスクロール
位置に適用してください(具体的な組み込み例は `demo/main.cpp` および
下記の仕様書を参照)。

## ドキュメント

| ドキュメント | 内容 |
|---|---|
| [`SmoothScroll_Spec.md`](./SmoothScroll_Spec.md) | 全APIリファレンス、`SS_Config`全項目、`SmoothScrollHelper`の使い方、実装上の落とし穴集 |
| [`DEBUG_NOTES.md`](./DEBUG_NOTES.md) | 過去のデバッグ記録(端のカクつき/ジャンプ、軸ロック時の突っかかり、タッチ中の遅延、それぞれの原因と修正) |

初めて触る場合はこのREADMEでビルド・動作確認まで行い、実際に組み込む際は
`SmoothScroll_Spec.md`を参照するのがおすすめです。過去に踏んだ実装上の
落とし穴(タイマーID衝突、軸ロック時の速度凍結、ラバーバンドの連続性など)は
すべて仕様書の最終章にまとめてあります。

## 動作要件

- Windows 10以降
- Precision Touchpad (WM_POINTERWHEEL/WM_POINTERHWHEEL に対応したドライバ)
- MSVC (CMakeLists.txt は `/utf-8` オプションを前提にしています)

## 既知の制約

- Windows設定の「2本指ドラッグでスクロール」がOFFの場合、そもそもOS側が
  ジェスチャーをスクロールイベントとして合成しないため、本ライブラリは
  何もできません(他の一般的なアプリと同様の制約です)。