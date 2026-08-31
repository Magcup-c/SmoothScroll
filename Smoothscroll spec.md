# SmoothScroll ライブラリ仕様書

Windows (10以降) / Precision Touchpad 向けの Mac風スムーススクロール・
慣性スクロール DLL。パン(スクロール)・ピンチズーム・オーバースクロール
(ラバーバンド + バネ)を提供する。

対象ファイル: `SmoothScroll/SmoothScroll.h`, `SmoothScroll/SmoothScroll.cpp`,
`SmoothScroll/PhysicsEngine.h`, `SmoothScroll/PhysicsEngine.cpp`,
`SmoothScroll/SmoothScrollHelper.h` (C++向け薄いラッパー、任意)

---

## 1. 全体構成

```
ホストアプリ(HWND)
   │  ss_attach()でサブクラス化
   ▼
SmoothScroll.dll
   ├─ SubclassProc          … WM_POINTERWHEEL/HWHEEL, WM_MOUSEWHEEL などを捕捉
   ├─ PhysicsEngine (パン用)  … 慣性(指数減衰)モデル
   ├─ PhysicsEngine (ズーム用) … 対数空間での慣性モデル
   └─ PhysicsThread          … 慣性/バネ引き継ぎ後のアニメーションだけを駆動する物理ループ
```

- **タッチ中(指でなぞっている間)** の生デルタは、入力メッセージを受け取った
  スレッドから**直接**コールバックが呼ばれる(レイテンシ最小化のため)。
- **指を離した後の慣性フェーズ**は、DLL内部の `PhysicsThread` (ディスプレイの
  リフレッシュレートに同期した専用スレッド)からコールバックが呼ばれる。
- **オーバースクロール時のラバーバンド抵抗・バネ演算はホスト側の責務**。
  DLLは状態を持たない純粋関数(`ss_rubberband_delta` / `ss_spring_step` /
  `ss_spring_settled`)を提供するのみで、位置・速度はホストが保持する。

---

## 2. `SS_Config` 設定項目

`ss_default_config()` で既定値を取得できる。

| フィールド | 型 | 既定値 | 説明 |
|---|---|---|---|
| `decay_rate` | float | 4.0 | 慣性の減衰率 [/sec]。大きいほど早く止まる(Mac風は3.0〜5.0)。 |
| `velocity_ema` | float | 0.35 | 指速度推定のEMA係数(0..1]。小さいほど滑らかだが追従が鈍い。 |
| `stop_velocity` | float | 25.0 | この速度[px/s]以下で慣性を完全停止。 |
| `max_velocity` | float | 6000.0 | 初速クランプ[px/s]。異常デルタでの飛びすぎ防止。 |
| `pixel_per_wheel` | float | 100.0 | ホイールデルタ120あたりのピクセル換算。 |
| `enable_horizontal` | BOOL | TRUE | 横スクロール(`WM_POINTERHWHEEL`)も処理するか。 |
| `pass_through_wheel` | BOOL | TRUE | 通常マウスホイールをスムーズ化せず素通しするか。 |
| `pan_subpixel` | BOOL | FALSE | TRUEで慣性フェーズが整数ピクセルに丸めず浮動小数点のサブピクセル値を返す(量子化ジッター回避)。 |
| `invert_vertical` | BOOL | FALSE | 縦方向の符号を反転(ナチュラルスクロール切替)。 |
| `invert_horizontal` | BOOL | FALSE | 横方向の符号を反転。 |
| `enable_zoom` | BOOL | FALSE | Ctrl+ホイール(ピンチ互換シグナル)によるズームを有効にするか。 |
| `zoom_step` | float | 0.10 | ホイールデルタ120あたりの対数スケール増分。 |
| `zoom_decay_rate` | float | 4.0 | ズーム慣性の減衰率[/sec]。 |
| `zoom_stop_velocity` | float | 0.05 | この対数速度未満で慣性を完全停止。 |
| `zoom_max_velocity` | float | 6.0 | ズーム初速クランプ(対数速度)。 |

---

## 3. 公開API一覧

### ライフサイクル

| 関数 | 説明 |
|---|---|
| `void ss_default_config(SS_Config* cfg)` | 既定値入りの設定を取得。 |
| `HANDLE ss_attach(HWND hwnd, const SS_Config* cfg, SS_ScrollCallback cb, SS_ZoomCallback cb_zoom, void* user)` | 指定ウィンドウに取り付け。成功でハンドル、失敗でNULL。`cb_zoom`は`enable_zoom=FALSE`ならnullptrでよい。 |
| `void ss_detach(HANDLE h)` | 取り外し。物理スレッド停止とサブクラス解除を行う。 |

### 状態問い合わせ

| 関数 | 説明 |
|---|---|
| `BOOL ss_is_scrolling(HANDLE h)` | タッチ中 or 慣性でまだ動いている最中ならTRUE。 |
| `BOOL ss_is_touching(HANDLE h)` | 現在タッチ(操作)中かどうかだけを返す(慣性の有無は問わない)。 |

### 慣性の制御

| 関数 | 説明 |
|---|---|
| `void ss_cancel_momentum(HANDLE h)` | 慣性を完全キャンセル。タッチ中フラグも含め全状態リセット。**「本当に新規セッションが始まった」場面(例: 新しいタッチ開始)専用**。 |
| `void ss_stop_inertia(HANDLE h)` | 慣性(モメンタム)のみ停止。タッチ中フラグには影響しない。**まだ操作中に「境界に到達したので慣性の暴走を止めたい」場面**で使う。 |
| `void ss_scale_velocity(HANDLE h, float sx, float sy)` | 内部速度に補正係数を掛ける低レベルAPI。通常は不要。 |
| `void ss_get_velocity(HANDLE h, float* vx, float* vy)` | 内部速度を取得。**慣性→バネのハンドオフ**で、バネの初速をゼロからでなくこの値から始めるために使う(速度不連続=ジャーク回避)。 |

### 動的設定変更

| 関数 | 説明 |
|---|---|
| `void ss_set_invert(HANDLE h, BOOL invert_vertical, BOOL invert_horizontal)` | パン方向反転を実行中に変更(例: 設定画面での切替)。 |

### オーバースクロール(ラバーバンド/バネ)ユーティリティ

これらは**状態を持たない純粋関数**。位置・速度はホスト側で保持すること。

| 関数 | 説明 |
|---|---|
| `float ss_rubberband_delta(float raw_delta, float pos, float min_pos, float max_pos, float coeff)` | 範囲外への移動量を圧縮曲線 `f(o)=coeff*o/(coeff+o)` で抵抗させる。押し込み・戻し・境界またぎのすべてで連続。`coeff`は小さいほど強く抵抗(推奨既定 0.55)。 |
| `float ss_spring_step(float pos, float target, float* velocity, float dt_sec, float stiffness, float damping)` | posをtargetへバネで1ステップ近づける。`velocity`はホストが保持し毎フレーム更新すること。stiffness既定200目安、damping既定20目安(臨界減衰に近い)。 |
| `BOOL ss_spring_settled(float pos, float target, float velocity, float pos_epsilon, float vel_epsilon)` | バネが収束したかどうかの判定ヘルパー。 |

---

## 4. コールバックと呼び出しスレッド

```c
typedef void (CALLBACK *SS_ScrollCallback)(float dx, float dy, void* user);
typedef void (CALLBACK *SS_ZoomCallback)(float scale_factor, float anchor_x, float anchor_y, void* user);
```

- `SS_ScrollCallback`: `dx, dy` はこのフレームでスクロールすべきピクセル移動量。
  `pan_subpixel=FALSE`(既定)なら整数ピクセル、`TRUE`ならサブピクセル浮動小数点。
- `SS_ZoomCallback`: `scale_factor` は現在のズーム倍率に**乗算**すべき係数
  (1.0 = 変化なし。加算ではない点に注意)。`anchor_x/y` はズーム中心点
  (クライアント座標)。

**呼び出しスレッドはフェーズによって異なる:**

| フェーズ | 呼び出し元スレッド |
|---|---|
| タッチ中(1:1パス動作) | 入力メッセージを受け取ったスレッド(通常UIスレッド)から**同期的に直接** |
| 慣性/バネ引き継ぎ後のアニメーション | DLL内部の物理ループスレッドから |

いずれの場合も、ホスト側でUIを直接更新せず `PostMessage` 等でUIスレッドの
管理下に処理を戻すこと。

> **推奨:** この呼び出し元スレッドの違いを毎回意識するのは実装ミスの
> 温床になりやすい。C++で組み込む場合は、生のCコールバックを直接扱わず
> 6章の `SmoothScrollHelper` を使うことを推奨する。

---

## 5. 典型的な利用パターン(`demo/main.cpp` 抜粋・要約)

### 5.1 取り付け

```cpp
SS_Config cfg; ss_default_config(&cfg);
cfg.enable_zoom = TRUE;
cfg.pan_subpixel = TRUE;
cfg.invert_vertical = TRUE;

HANDLE g_ss = ss_attach(hwnd, &cfg, OnScroll, OnZoom, hwnd);
```

`OnScroll`/`OnZoom` はDLLスレッドから直接叩かれうるため、UIスレッドへは
`PostMessageW(hwnd, WM_APP + N, ...)` で戻す。float を wParam/lParam に
そのまま積むには `memcpy` でビットパターンをコピーする(`main.cpp` 参照)。

### 5.2 パン受信時: ラバーバンド適用

```cpp
float appliedX = ss_rubberband_delta(contentDx, g_scrollX, 0.0f, g_maxScrollX, RUBBERBAND_COEFF);
g_scrollX += appliedX;

g_overscrollX = (g_scrollX < 0.0f || g_scrollX > g_maxScrollX);
if (g_overscrollX || g_overscrollY) {
    ss_stop_inertia(g_ss); // まだ操作中の暴走だけ止める。cancel_momentumは使わない
}
```

### 5.3 バネへの引き継ぎ(タイマー駆動、例: 16ms周期)

```cpp
bool overNow = /* g_scrollX/Y が範囲外か */;

if (overNow && !ss_is_touching(g_ss)) {
    if (!g_springHandoffDone) {           // ★ 区間につき一度だけ
        float evx, evy;
        ss_get_velocity(g_ss, &evx, &evy);
        ss_cancel_momentum(g_ss);
        g_springVelX = evx / g_zoomScale;
        g_springVelY = evy / g_zoomScale;
        g_springHandoffDone = true;
    }
} else {
    g_springHandoffDone = false;          // 再アーム
}

if (!ss_is_scrolling(g_ss)) {
    g_scrollX = ss_spring_step(g_scrollX, target, &g_springVelX, dt, STIFFNESS, DAMPING);
    if (ss_spring_settled(g_scrollX, target, g_springVelX, POS_EPS, VEL_EPS)) {
        g_scrollX = target; g_springVelX = 0.0f;
    }
}
```

### 5.4 取り外し

```cpp
ss_detach(g_ss);
```

---

## 6. C++ラッパー `SmoothScrollHelper`(推奨)

`SmoothScroll/SmoothScrollHelper.h` は、上記の生API(`ss_attach`等)の上に
被せる薄いヘッダオンリーのC++クラス。**既存DLLのABIには一切手を加えていない**
(新規ヘッダ1枚を追加するだけで導入できる)。

### 解決する問題

4章で述べた通り、コールバックの呼び出し元スレッドはフェーズによって
異なる(タッチ中はUIスレッド、慣性/バネ中は物理ループスレッド)。組み込み側が
この違いを見落として物理ループスレッドから直接UI操作(`InvalidateRect`等)を
行ってしまうと、クラッシュや未定義動作につながる。`SmoothScrollHelper`は
呼び出し元スレッドを常に判定し、UIスレッドと異なる場合は自動的に
`PostMessage`経由でディスパッチし直すことで、この種の実装ミスを構造的に防ぐ。

### 使い方

```cpp
#include "SmoothScrollHelper.h"

SmoothScrollHelper g_ssHelper;

case WM_CREATE: {
    SS_Config cfg; ss_default_config(&cfg);
    cfg.enable_zoom = TRUE;
    g_ssHelper.Attach(hwnd, &cfg,
        [](float dx, float dy) {
            // 常にUIスレッド上から呼ばれることが保証される。
            // 直接InvalidateRect等のUI操作をしてよい。
            g_scrollX += dx; g_scrollY += dy;
            InvalidateRect(hwnd, nullptr, FALSE);
        },
        [](float scale, float ax, float ay) {
            // ズームコールバックも同様に安全
        });
    return 0;
}

case SmoothScrollHelper::kScrollMessage:
case SmoothScrollHelper::kZoomMessage:
    g_ssHelper.HandleMessage(msg, wParam, lParam);
    return 0;

case WM_DESTROY:
    g_ssHelper.Detach(); // 物理スレッドの終了を待つのでコールバックとの競合はない
    ...
```

### API

| メンバ | 説明 |
|---|---|
| `bool Attach(HWND hwnd, const SS_Config* cfg, ScrollCallback, ZoomCallback = nullptr)` | `ss_attach`相当。`std::function`/ラムダをそのまま渡せる。 |
| `void Detach()` | `ss_detach`相当。デストラクタからも自動的に呼ばれる。 |
| `void HandleMessage(UINT msg, WPARAM, LPARAM)` | `WndProc`から`kScrollMessage`/`kZoomMessage`受信時に呼ぶ。 |
| `HANDLE raw_handle() const` | 生の`HANDLE`が必要な場合(`ss_get_velocity`等を直接呼びたい場合)に使用。 |
| `static constexpr UINT kScrollMessage / kZoomMessage` | `WM_APP + 0x5301` / `0x5302`。ホスト側で衝突する場合はヘッダ内の値を調整する。 |

### 設計上の注意

- タッチ中(1:1パス動作)は呼び出し元スレッドが既にUIスレッドと一致しているため、
  `PostMessage`を経由せずその場で直接コールバックを呼ぶ(7章8番の落とし穴で
  述べたレイテンシ削減の効果を損なわない)。物理ループスレッドから来た場合のみ
  `PostMessage`に切り替わる。
- コピー・ムーブ禁止(DLLに`this`ポインタを`user`として渡しているため)。
- オーバースクロールのラバーバンド/バネ計算(`ss_rubberband_delta`等)は
  引き続きホスト側の責務であり、`SmoothScrollHelper`はスクロール/ズーム
  コールバックのスレッド安全な配送のみを担う。

---

## 7. 実装上の注意点(実機検証で判明した既知の落とし穴)

これらはすべて `SmoothScroll.cpp` / `PhysicsEngine.cpp` 内のコメントとして
残されている。ライブラリを改変する際は特に注意すること。

1. **タイマーIDの衝突:** DLL内部の `PAN_TIMER_ID`/`ZOOM_TIMER_ID` は、
   ホストアプリが同じHWNDに対して小さい/衝突しやすいIDでSetTimerしていると
   サブクラスプロシージャに横取りされる。そのため一般アプリが使わない
   ユニークな値(`0x53534D01`等)にしてある。
2. **Ctrl+ホイールのキー検出:** `EnableMouseInPointer(TRUE)`環境下では
   Ctrl相当のシグナルはwParamのキー修飾フラグに乗らない。
   `GetKeyState`も途切れることがあり、`GetAsyncKeyState(VK_CONTROL)`での
   直接問い合わせが最も安定していた。
3. **軸ロックと速度凍結:** Precision Touchpadは縦横が別メッセージ
   (`WM_POINTERWHEEL`/`WM_POINTERHWHEEL`)で届くため、片方の軸のイベントが
   一時的に来なくなることがある。この間、その軸の速度を0に強制するのではなく
   指数減衰でなだらかにフェードさせる(`PhysicsEngine::feedDelta`の
   `update_x`/`update_y`引数)。
4. **速度推定dtと凍結減衰dtの分離:** 縦横の速度推定用dt(同一軸の前回イベント
   基準)と、凍結側減衰用dt(縦横共通の前回呼び出し基準)を混同すると、
   軸ロック解除の瞬間に凍結側の速度が不自然に急減衰する
   (`feedDelta`の`freeze_dt_sec`引数で分離)。
5. **リリース直前のdt肥大:** 指を離す直前はPTPのサンプリングが疎になりやすく、
   最後の1サンプルのdtが異常に広くなることがある。これをそのままEMAへ
   フルの重みでブレンドすると「離した瞬間に失速する」ため、
   `kReleaseArtifactDt`(30ms)を超えるdtはEMA重みを弱める。
6. **ラバーバンドの連続性:** 境界をまたぐ瞬間・押し込み/戻しを切り替える瞬間の
   両方で抵抗のかかり方が不連続にならないよう、圧縮曲線 `f(o)=coeff*o/(coeff+o)`
   とその逆関数を介した単一の式に統一している(個別の場合分けをしない)。
7. **バネ引き継ぎは一区間につき一度だけ:** 慣性→バネの速度ハンドオフを
   毎フレーム実行すると、2回目以降は速度が既に0になっているため
   バネ初速を0で上書きし続けてしまう。ホスト側で一度きりのフラグ管理が必要
   (DLL側のAPIはステートレスなのでホスト側の責務)。
8. **タッチ中はスレッドを跨がない:** 1:1パス動作(慣性でもバネでもない、
   動かした分をそのまま返すだけの処理)を物理ループスレッド経由にすると、
   スレッド起床待ちの分だけ常に遅延が乗る。計算が不要な経路は
   入力を受けたスレッドから直接コールバックを呼ぶ。