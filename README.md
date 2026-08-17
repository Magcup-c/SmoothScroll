# SmoothScrollLib — Mac風スムース/慣性スクロール DLL（Windows タッチパッド用）

アプリのウィンドウに組み込むだけで、Precision Touchpad の2本指スクロールを
Mac風の滑らかな慣性スクロールに変換するC++ライブラリです。

## 仕組み

1. `ss_attach(hwnd, ...)` でウィンドウを `SetWindowSubclass` によりサブクラス化
2. タッチパッド由来の `WM_POINTERWHEEL / WM_POINTERHWHEEL` を捕捉（`PT_TOUCHPAD` で判定）
3. 生デルタを飲み込み、内部物理エンジンへ入力
   - タッチ中: 生デルタをフレーム単位でそのまま出力しつつ、速度をEMA推定
   - リリース後: 推定速度を初速に `v *= exp(-k·dt)` の指数減衰で慣性スクロール
   - 低速（`stop_velocity` 未満）で「ぴたっと」停止。サブピクセル端数は繰り越し
4. 物理ループは専用スレッド＋高分解能ウェイタブルタイマで実リフレッシュレートに同期。
   アイドル時はイベント待機でCPUを消費しません
5. スムーズ化されたピクセルデルタをコールバックで返す

## ビルド

```bat
cmake -B build -A x64
cmake --build build --config Release
```
`build/Release/SmoothScroll.dll` / `.lib` と `SSDemo.exe` が生成されます。
Visual Studio 2022 で `.sln` を開きたい場合は `cmake -B build -G "Visual Studio 17 2022" -A x64`。

## 使い方（3行）

```cpp
SS_Config cfg; ss_default_config(&cfg);
HANDLE h = ss_attach(hwnd, &cfg, OnScroll, myData);
// ...終了時に ss_detach(h);
```

コールバックで受け取る `dx, dy` は「このフレームでスクロールすべきピクセル量」です。
コールバックは物理スレッドから呼ばれるので、UI更新は `PostMessage` でUIスレッドへ
回してください（demo/main.cpp 参照）。

## チューニング（SS_Config）

| パラメータ | 既定 | 意味 |
|---|---|---|
| `decay_rate` | 4.0 | 慣性の減衰率。小さいほど「よく滑る」。Mac風は3〜5 |
| `velocity_ema` | 0.35 | 指速度推定の平滑化。小さいほど滑らかだが反応は鈍くなる |
| `stop_velocity` | 25.0 | この速度[px/s]未満で即停止 |
| `max_velocity` | 6000 | フリック初速の上限 |
| `pixel_per_wheel` | 100 | ホイールデルタ120あたりの換算ピクセル |
| `enable_horizontal` | TRUE | 横方向も処理するか |

## 注意事項

- 対象は **Precision Touchpad（2本指パン → WM_POINTERWHEEL/HWHEEL）** です。
  通常のUSBマウスのホイールは素通しされます（アプリ既定の挙動が維持されます）
- ジェスチャ終了はOSから明示通知されないため、90ms無入力でリリースと判定しています
- 直接タッチ（タッチスクリーンやタッチパッド接触イベント）を使いたい場合は
  `RegisterPointerInputTarget(hwnd, PT_TOUCHPAD)` の追加が必要です
- アプリ内の1ウィンドウ専用です。OS全体に効かせる場合はグローバルフック型の
  別アーキテクチャが必要になります
# SmoothScroll
