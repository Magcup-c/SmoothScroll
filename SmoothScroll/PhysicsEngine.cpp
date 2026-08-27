#include "PhysicsEngine.h"
#include <cmath>
#include <algorithm>

PhysicsEngine::PhysicsEngine(const SS_Config& cfg) : cfg_(cfg) {}

void PhysicsEngine::feedDelta(float dx, float dy, float dt_sec, bool update_x, bool update_y) {
    touching_ = true;
    momentum_ = false;

    // タッチ中は生デルタをそのまま消費側へ渡すため速度だけ推定する。
    // 瞬間速度をEMAで平滑化（ノイズの多いタッチパッドのデルタを滑らかに）
    if (dt_sec > 0.0005f) {
        float ivx = dx / dt_sec;
        float ivy = dy / dt_sec;

        // タイマ分解能の粗さ等により dt が異常に小さく計測された場合、
        // 瞬間速度が跳ね上がりEMAを汚染して「離した瞬間に飛ぶ」原因になる。
        // 外れ値をクランプしてから平滑化する。
        const float spike_limit = cfg_.max_velocity * 3.0f;
        float ispeed = std::sqrt(ivx * ivx + ivy * ivy);
        if (ispeed > spike_limit && ispeed > 0.0f) {
            float s = spike_limit / ispeed;
            ivx *= s; ivy *= s;
        }

        float a = cfg_.velocity_ema;
        // 更新対象の軸はEMAで平滑化して反映する。
        // 更新対象でない軸は「完全に凍結」ではなく、慣性と同じ指数減衰で
        // 緩やかにフェードさせる。完全凍結だと、軸ロックが長く続いた後に
        // 古い速度がいきなり慣性に反映されて不自然になるため。
        // (Precision Touchpadの軸ロックで片方の軸のイベントが一時的に
        //  来なくなった時、その軸の速度をなだらかに減衰させることで
        //  強制的に0へ引っ張る/いつまでも凍結する、両方の不自然さを避ける)
        float decay = std::exp(-cfg_.decay_rate * dt_sec);
        if (update_x) vx_ = vx_ * (1.0f - a) + ivx * a;
        else          vx_ *= decay;
        if (update_y) vy_ = vy_ * (1.0f - a) + ivy * a;
        else          vy_ *= decay;
    }
}

void PhysicsEngine::release() {
    touching_ = false;

    // 初速クランプ（異常デルタでの飛びすぎ防止）
    float speed = std::sqrt(vx_ * vx_ + vy_ * vy_);
    if (speed > cfg_.max_velocity) {
        float s = cfg_.max_velocity / speed;
        vx_ *= s; vy_ *= s;
    }
    // 低速フリックは慣性を出さず即停止（Macの「ぴたっと止まる」感）
    if (speed < cfg_.stop_velocity) {
        vx_ = vy_ = 0;
        momentum_ = false;
        return;
    }
    momentum_ = true;
}

bool PhysicsEngine::step(float dt_sec, float& out_dx, float& out_dy) {
    out_dx = out_dy = 0;
    if (!momentum_) return false;

    // 指数減衰: v *= exp(-k*dt)
    float decay = std::exp(-cfg_.decay_rate * dt_sec);

    float dx = vx_ * dt_sec;
    float dy = vy_ * dt_sec;

    vx_ *= decay;
    vy_ *= decay;

    float speed = std::sqrt(vx_ * vx_ + vy_ * vy_);
    if (speed < cfg_.stop_velocity) {
        vx_ = vy_ = 0;
        momentum_ = false;
    }

    // サブピクセル端数を繰り越してカクつきを防ぐ
    dx += rem_x_; dy += rem_y_;
    float ix = std::round(dx), iy = std::round(dy);
    rem_x_ = dx - ix; rem_y_ = dy - iy;
    out_dx = ix; out_dy = iy;

    return momentum_ || (ix != 0 || iy != 0);
}

bool PhysicsEngine::stepContinuous(float dt_sec, float& out_dx, float& out_dy) {
    out_dx = out_dy = 0;
    if (!momentum_) return false;

    // step()と全く同じ減衰計算だが、整数丸め・端数繰り越しを行わない。
    // ズームのスケール値(対数空間)のような、量子化してはいけない値のための経路。
    float decay = std::exp(-cfg_.decay_rate * dt_sec);

    out_dx = vx_ * dt_sec;
    out_dy = vy_ * dt_sec;

    vx_ *= decay;
    vy_ *= decay;

    float speed = std::sqrt(vx_ * vx_ + vy_ * vy_);
    if (speed < cfg_.stop_velocity) {
        vx_ = vy_ = 0;
        momentum_ = false;
    }

    return momentum_ || (out_dx != 0.0f || out_dy != 0.0f);
}

void PhysicsEngine::cancel() {
    vx_ = vy_ = 0;
    rem_x_ = rem_y_ = 0;
    touching_ = false;
    momentum_ = false;
}

void PhysicsEngine::stopMomentumOnly() {
    vx_ = vy_ = 0;
    rem_x_ = rem_y_ = 0;
    momentum_ = false;
    // touching_ はそのまま維持する（呼び出し元がまだ操作中の可能性があるため）
}

bool PhysicsEngine::isActive() const { return touching_ || momentum_; }
bool PhysicsEngine::isTouching() const { return touching_; }