// PhysicsEngine.h - 慣性スクロール物理モデル
// モデル: 指数減衰 v(t) = v0 * exp(-k*t)  (Mac風スクロールの減衰曲線)
// 指が触れている間: 生デルタをそのまま通しつつ速度をEMA推定
// 指が離れた後   : 推定速度を初速として慣性フェーズへ遷移
#pragma once
#include <Windows.h>
#include "SmoothScroll.h"

class PhysicsEngine {
public:
    explicit PhysicsEngine(const SS_Config& cfg);

    // 生のタッチパッドデルタを入力（タッチ中）
    // dx, dy: ピクセル換算済みデルタ, dt_sec: 前回イベントからの経過秒
    void feedDelta(float dx, float dy, float dt_sec);

    // タッチ終了 → 慣性フェーズ開始
    void release();

    // 物理ステップ。dt_sec 経過後の移動量を out に出力。
    // 戻り値: まだ動いているなら true（false ならループはスリープしてよい）
    bool step(float dt_sec, float& out_dx, float& out_dy);

    // 完全キャンセル: タッチ中フラグも含めて全状態をリセットする。
    // 新しいタッチ開始(WM_POINTERDOWN)など「本当に新規セッションが始まる」場面専用。
    void cancel();

    // 慣性のみ停止: touching_ フラグには触れない。
    // ユーザーがまだ指を動かしている最中に「境界に到達したので暴れる慣性を止めたい」
    // という場面で使う。touching_ を誤って false にすると、まだドラッグ中なのに
    // ホスト側のバネ処理が「操作終了した」と誤判定して起動し、
    // ユーザーの入力とバネが同じ位置を奪い合って暴れる原因になる。
    void stopMomentumOnly();

    bool isActive() const;
    bool isTouching() const;

private:
    SS_Config cfg_;
    float vx_ = 0, vy_ = 0;      // 現在速度 [px/s]
    float rem_x_ = 0, rem_y_ = 0; // サブピクセル端数の繰り越し
    bool touching_ = false;
    bool momentum_ = false;
};