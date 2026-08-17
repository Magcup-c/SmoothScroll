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

} // extern "C"
