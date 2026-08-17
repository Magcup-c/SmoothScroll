// SmoothScroll.h - Mac風スムーススクロール/慣性スクロール DLL 公開API
// ターゲット: Windows 10 以降 / Precision Touchpad (WM_POINTER系)
#pragma once

#include <Windows.h>

#ifdef SMOOTHSCROLL_EXPORTS
#define SS_API __declspec(dllexport)
#else
#define SS_API __declspec(dllimport)
#endif

extern "C" {

// スムーズ化されたスクロールデルタを受け取るコールバック。
// dx, dy: ピクセル単位の移動量（このフレームでスクロールすべき量）
// user: ss_attach で渡した任意ポインタ
// 呼び出しスレッド: DLL内部の物理ループスレッド（UI更新はPostMessage等でUIスレッドへ回すこと）
typedef void (CALLBACK *SS_ScrollCallback)(float dx, float dy, void* user);

typedef struct SS_Config {
    float decay_rate;        // 慣性の減衰率 [/sec]。大きいほど早く止まる。既定 4.0 (Mac風は 3.0〜5.0)
    float velocity_ema;      // 指速度推定のEMA係数 (0..1]。小さいほど滑らかだが追従が鈍い。既定 0.35
    float stop_velocity;     // この速度[px/s]以下で完全停止。既定 25.0
    float max_velocity;      // 初速クランプ [px/s]。既定 6000.0
    float pixel_per_wheel;   // ホイールデルタ120あたりのピクセル換算。既定 100.0
    BOOL  enable_horizontal; // 横スクロールも処理するか。既定 TRUE
    BOOL  pass_through_wheel;// 通常マウスホイールをスムーズ化せず素通しするか。既定 TRUE
} SS_Config;

// 既定値入りの設定を取得
SS_API void ss_default_config(SS_Config* cfg);

// 指定ウィンドウにスムーススクロールを取り付ける。
// 成功: ハンドル(非NULL) / 失敗: NULL
// hwnd には SetWindowSubclass でサブクラス化を行い、
// タッチパッド由来の WM_POINTERWHEEL/HWHEEL を捕捉して慣性化する。
SS_API HANDLE ss_attach(HWND hwnd, const SS_Config* cfg, SS_ScrollCallback cb, void* user);

// 取り外し。物理スレッド停止とサブクラス解除を行う。
SS_API void ss_detach(HANDLE h);

// スクロール中かどうか（慣性でまだ動いている最中ならTRUE）
SS_API BOOL ss_is_scrolling(HANDLE h);

// 動作中の慣性を即時停止（新しいタッチやクリック時に呼ぶと自然）
SS_API void ss_cancel_momentum(HANDLE h);

// ============================================================
// オーバースクロール(ラバーバンド)ユーティリティ
// これらは状態を持たない純粋関数。位置・速度はホスト側で保持すること。
// ============================================================

// 範囲外への移動量を圧縮する（Apple風のラバーバンド抵抗）。
// raw_delta : このフレームで加えたい移動量（DLLのコールバックから来た dy 等）
// pos       : 適用前の現在位置
// min_pos, max_pos : コンテンツの許容範囲
// coeff     : 抵抗係数。小さいほど強く抵抗する。既定推奨値 0.55
// 戻り値    : 実際に pos へ加算すべき移動量
SS_API float ss_rubberband_delta(float raw_delta, float pos, float min_pos, float max_pos, float coeff);

// pos を target（境界内の最近接位置など）へバネで近づける1ステップ。
// velocity はホスト側が保持し、毎フレームこの関数経由で更新すること。
// stiffness: バネの強さ（既定 200 目安）, damping: 減衰（既定 20 目安、臨界減衰に近い値）
// 戻り値: 更新後の pos
SS_API float ss_spring_step(float pos, float target, float* velocity, float dt_sec,
                             float stiffness, float damping);

// バネアニメーションが収束したかどうかの判定ヘルパー
SS_API BOOL ss_spring_settled(float pos, float target, float velocity,
                               float pos_epsilon, float vel_epsilon);

} // extern "C"
