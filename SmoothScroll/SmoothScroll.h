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
//   SS_Config::pan_subpixel が FALSE(既定)の場合は整数ピクセル値。
//   TRUE の場合は慣性フェーズも浮動小数点のサブピクセル値になる
//   (ホスト側がfloatでスクロール位置を保持できる場合に量子化ジッターを避けられる)。
// user: ss_attach で渡した任意ポインタ
// 呼び出しスレッド: DLL内部の物理ループスレッド（UI更新はPostMessage等でUIスレッドへ
// 回してください）
typedef void (CALLBACK *SS_ScrollCallback)(float dx, float dy, void* user);

// ピンチズームのスムーズ化されたスケール変化を受け取るコールバック。
// scale_factor: このフレームで現在のズーム倍率に乗算すべき係数(例: 1.002 = +0.2%)。
//               1.0が「変化なし」。ホスト側は自身のズーム倍率にこれを掛け合わせて更新すること
//               (加算ではなく乗算である点に注意。指数減衰モデルとの相性のため)。
// anchor_x, anchor_y: ズーム操作の中心点(クライアント座標)。
//               ホスト側で「この点を中心にズームする」処理に使う。
// user: ss_attach で渡した任意ポインタ
// 呼び出しスレッド: SS_ScrollCallback と同じ物理ループスレッド
typedef void (CALLBACK *SS_ZoomCallback)(float scale_factor, float anchor_x, float anchor_y, void* user);

typedef struct SS_Config {
    float decay_rate;        // 慣性の減衰率 [/sec]。大きいほど早く止まる。既定 4.0 (Mac風は 3.0〜5.0)
    float velocity_ema;      // 指速度推定のEMA係数 (0..1]。小さいほど滑らかだが追従が鈍い。既定 0.35
    float stop_velocity;     // この速度[px/s]以下で完全停止。既定 25.0
    float max_velocity;      // 初速クランプ [px/s]。既定 6000.0
    float pixel_per_wheel;   // ホイールデルタ120あたりのピクセル換算。既定 100.0
    BOOL  enable_horizontal; // 横スクロールも処理するか。既定 TRUE
    BOOL  pass_through_wheel;// 通常マウスホイールをスムーズ化せず素通しするか。既定 TRUE

    // TRUEにすると、慣性フェーズのコールバックが整数ピクセルへ丸めず
    // 浮動小数点のサブピクセル値をそのまま返すようになる(内部的には
    // PhysicsEngine::stepContinuous() を使用)。
    // 低速域(慣性の終盤)で「ピタッ…ピタッ」と1ピクセル刻みに飛ぶ
    // 量子化ジッターが気になる場合、ホスト側がfloatでスクロール位置を
    // 保持できるならこちらを有効にすることで解消できる。
    // 逆に、コールバックの値をそのまま整数ピクセル移動量として使いたい
    // (例えば ScrollWindowEx に直接渡すなど)ホストは既定の FALSE のままでよい。
    // 既定 FALSE(従来通り整数ピクセル丸め、後方互換)。
    BOOL  pan_subpixel;

    // Precision Touchpad からのホイールデルタの符号をライブラリ内部で反転するか。
    // OS/ドライバやアプリの慣習によって「2本指で上へなぞる」がコンテンツを
    // 上に動かすか下に動かすか(いわゆる「ナチュラルスクロール」の向き)が
    // 異なって感じられることがあるため、ホスト側で選べるようにしてある。
    // ss_attach 時の初期値としてここで指定できるほか、
    // アタッチ後に動的へ切り替えたい場合は ss_set_invert() を使うこと。
    // 既定は FALSE(反転なし、Windowsの生の符号のまま)。
    BOOL  invert_vertical;   // 縦方向(WM_POINTERWHEEL)の符号を反転するか。既定 FALSE
    BOOL  invert_horizontal; // 横方向(WM_POINTERHWHEEL)の符号を反転するか。既定 FALSE

    // ---- ピンチズーム関連 ----
    // Precision Touchpad は、アプリが生のマルチタッチ/DirectManipulation等に
    // 対応していないと判断すると、ピンチ動作を「Ctrl+ホイール」の互換シグナルとして
    // 送ってくる。本ライブラリはこの経路を利用してズームを検出する。
    //
    // 実機検証の結果:
    //   - EnableMouseInPointer(TRUE) 環境下では、このCtrl相当のシグナルは
    //     WM_POINTERWHEEL/HWHEEL のwParamには乗らない(常にno)。
    //   - GetKeyState(VK_CONTROL) も、Ctrlキー物理押下時ですら数回で
    //     途切れることがあり信頼できなかった。
    //   - GetAsyncKeyState(VK_CONTROL) で毎回直接OSに問い合わせる方式が
    //     最も安定していることが実機検証で確認された
    //     (純粋なパン動作では反応せず、実際のピンチ動作でのみ高頻度でYESになる)。
    //   そのためライブラリ内部では GetAsyncKeyState を使用している。
    BOOL  enable_zoom;        // ピンチズームを有効にするか。既定 FALSE
    float zoom_step;          // ホイールデルタ120あたりの対数スケール増分。既定 0.10 (約10%相当/クリック)
    float zoom_decay_rate;    // ズーム慣性の減衰率 [/sec]。既定 4.0
    float zoom_stop_velocity; // この対数速度未満で完全停止。既定 0.05
    float zoom_max_velocity;  // 初速クランプ(対数速度)。既定 6.0
} SS_Config;

// 既定値入りの設定を取得
SS_API void ss_default_config(SS_Config* cfg);

// 指定ウィンドウにスムーススクロールを取り付ける。
// 成功: ハンドル(非NULL) / 失敗: NULL
// hwnd には SetWindowSubclass でサブクラス化を行い、
// タッチパッド由来の WM_POINTERWHEEL/HWHEEL を捕捉して慣性化する。
// cb_zoom: cfg->enable_zoom が TRUE の場合に呼ばれるズームコールバック。不要な場合は nullptr でよい。
SS_API HANDLE ss_attach(HWND hwnd, const SS_Config* cfg, SS_ScrollCallback cb, SS_ZoomCallback cb_zoom, void* user);

// 取り外し。物理スレッド停止とサブクラス解除を行う。
SS_API void ss_detach(HANDLE h);

// スクロール中かどうか（タッチ中 or 慣性でまだ動いている最中ならTRUE）
SS_API BOOL ss_is_scrolling(HANDLE h);

// 現在タッチ(操作)中かどうかだけを返す（慣性の有無は問わない）。
// ホスト側でオーバースクロール時のバネ制御を行う際、「慣性がまだ残っているか」
// に関わらず「指がもう離れているかどうか」だけで判断したい場合に使う。
SS_API BOOL ss_is_touching(HANDLE h);

// 動作中の慣性を完全キャンセル（新しいタッチやクリック時に呼ぶと自然）。
// タッチ中フラグも含めて全状態をリセットするため、
// 「本当に新規セッションが始まった」場面（例: WM_POINTERDOWN相当）でのみ使うこと。
SS_API void ss_cancel_momentum(HANDLE h);

// 慣性(モメンタム)のみを停止する。タッチ中フラグ(touching)には影響しない。
// ユーザーがまだ指を動かしている最中に「境界へ到達したので慣性の飛び出しを
// 止めたい」というホスト側の意図（オーバースクロール制御など）に使う。
// ss_cancel_momentum との違い: こちらはタッチ操作の継続性を壊さない。
SS_API void ss_stop_inertia(HANDLE h);

// エンジンの内部速度(パン用)にホスト側から補正係数を掛ける。
// 通常は使わなくてよい低レベルAPI(必要な場面が生じた場合のための保険)。
// sx, sy: 横方向・縦方向それぞれの倍率。1.0で無補正。
SS_API void ss_scale_velocity(HANDLE h, float sx, float sy);

// エンジン内部速度(パン用)を取得する。単位はコールバックのdx/dyと同じ
// (スクリーンピクセル/秒、ズーム倍率で割る前の値)。
// 用途: 慣性からホスト側のバネ(スプリング)アニメーションへ引き継ぐ際、
// バネの初速をゼロからではなくこの値から始めることで、速度の不連続
// (「ガクッ」というジャーク)を避けるために使う。
SS_API void ss_get_velocity(HANDLE h, float* vx, float* vy);

// アタッチ後に、パン方向の反転設定を動的に変更する。
// ss_attach 時に SS_Config::invert_vertical / invert_horizontal で
// 初期値を指定できるが、実行中に(例えば設定画面でユーザーが切り替えた場合など)
// 変更したい場合はこちらを使う。
SS_API void ss_set_invert(HANDLE h, BOOL invert_vertical, BOOL invert_horizontal);

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
//
// 注意: 既にオーバースクロール中(overshoot > 0)の場合、境界へ「戻ろうとする」
// 方向の delta には抵抗をかけない(そのまま通す)。押し込む方向のみ抵抗する
// 非対称な挙動にしてある。
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