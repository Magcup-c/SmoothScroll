#include "PhysicsEngine.h"
#include <cmath>

PhysicsEngine::PhysicsEngine(const SS_Config& cfg) : cfg_(cfg) {}

void PhysicsEngine::feedDelta(float dx, float dy, float dt_sec) {
    touching_ = true;
    momentum_ = false;

    // タッチ中は生デルタをそのまま消費側へ渡すため速度だけ推定する。
    // 瞬間速度をEMAで平滑化（ノイズの多いタッチパッドのデルタを滑らかに）
    if (dt_sec > 0.0005f) {
        float ivx = dx / dt_sec;
        float ivy = dy / dt_sec;
        float a = cfg_.velocity_ema;
        vx_ = vx_ * (1.0f - a) + ivx * a;
        vy_ = vy_ * (1.0f - a) + ivy * a;
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

void PhysicsEngine::cancel() {
    vx_ = vy_ = 0;
    rem_x_ = rem_y_ = 0;
    touching_ = false;
    momentum_ = false;
}

bool PhysicsEngine::isActive() const { return touching_ || momentum_; }
