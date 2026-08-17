// SmoothScroll.cpp - DLL本体: サブクラス化 + 物理ループスレッド
#define SMOOTHSCROLL_EXPORTS
#include <Windows.h>
#include <windowsx.h>
#include <CommCtrl.h>
#include <mmsystem.h>
#include <atomic>
#include <mutex>
#include "SmoothScroll.h"
#include "PhysicsEngine.h"
#include <cmath>

#pragma comment(lib, "winmm.lib")

namespace {

constexpr UINT_PTR SUBCLASS_ID = 0x53534D4B; // 'SSMK'
constexpr UINT     WM_SS_WAKE  = WM_APP + 0x53; // 物理ループ起床用

struct Instance {
    HWND hwnd = nullptr;
    SS_Config cfg{};
    SS_ScrollCallback cb = nullptr;
    void* user = nullptr;

    PhysicsEngine engine;
    std::mutex mtx;

    HANDLE thread = nullptr;
    HANDLE wake_event = nullptr;   // デルタ到着で起床
    std::atomic<bool> quit{false};

    ULONGLONG last_tick = 0;       // 前回イベント時刻
    float pending_dx = 0, pending_dy = 0; // 未送出のタッチ中デルタ

    explicit Instance(const SS_Config& c) : engine(c), cfg(c) {}
};

// ---- 物理ループスレッド --------------------------------------------------
// 高分解能ウェイタブルタイマでディスプレイ周期(既定60Hz相当)に駆動。
// アイドル時はイベント待機でCPUを消費しない。
DWORD WINAPI PhysicsThread(LPVOID param) {
    Instance* inst = static_cast<Instance*>(param);

    timeBeginPeriod(1);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

    // リフレッシュレート推定（既定 60Hz。取得できれば実値を使う）
    int hz = 60;
    DEVMODEW dm{}; dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        hz = dm.dmDisplayFrequency;
    const float frame_dt = 1.0f / (float)hz;

    HANDLE waits[2] = { inst->wake_event, timer };
    bool animating = false;

    while (!inst->quit.load()) {
        if (!animating) {
            // 慣性中でなければデルタ到着まで寝る
            WaitForSingleObject(inst->wake_event, INFINITE);
            if (inst->quit.load()) break;
        }

        // タイマーを次フレームにセット
        LARGE_INTEGER due{};
        due.QuadPart = -(LONGLONG)(frame_dt * 10000000.0f);
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);

        DWORD r = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (inst->quit.load()) break;

        // タッチ中の滞留デルタを先に吐き出す（追従フェーズ）
        float tdx = 0, tdy = 0;
        {
            std::lock_guard<std::mutex> lk(inst->mtx);
            tdx = inst->pending_dx; tdy = inst->pending_dy;
            inst->pending_dx = inst->pending_dy = 0;
        }
        if (tdx != 0 || tdy != 0)
            inst->cb(tdx, tdy, inst->user);

        // 慣性ステップ
        float mdx = 0, mdy = 0;
        bool active;
        {
            std::lock_guard<std::mutex> lk(inst->mtx);
            active = inst->engine.step(frame_dt, mdx, mdy);
        }
        if (mdx != 0 || mdy != 0)
            inst->cb(mdx, mdy, inst->user);

        animating = active;
    }

    if (timer) CloseHandle(timer);
    timeEndPeriod(1);
    return 0;
}

// ---- サブクラスプロシージャ ----------------------------------------------
bool IsTouchpadPointer(WPARAM wParam) {
    UINT32 id = GET_POINTERID_WPARAM(wParam);
    POINTER_INFO pi{};
    if (!GetPointerInfo(id, &pi)) return false;
    return pi.pointerType == PT_TOUCHPAD;
}

LRESULT CALLBACK SubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                              UINT_PTR, DWORD_PTR refData) {
    Instance* inst = reinterpret_cast<Instance*>(refData);

    switch (msg) {
    case WM_POINTERWHEEL:
    case WM_POINTERHWHEEL: {
        if (!IsTouchpadPointer(wParam)) break; // マウス等は既定処理へ

        if (msg == WM_POINTERHWHEEL && !inst->cfg.enable_horizontal) break;

        short delta = GET_WHEEL_DELTA_WPARAM(wParam);
        float px = (float)delta / (float)WHEEL_DELTA * inst->cfg.pixel_per_wheel;

        ULONGLONG now = GetTickCount64();
        float dt;
        {
            std::lock_guard<std::mutex> lk(inst->mtx);
            dt = inst->last_tick ? (float)(now - inst->last_tick) / 1000.0f : 1.0f / 60.0f;
            inst->last_tick = now;
            if (dt > 0.25f) dt = 1.0f / 60.0f; // 長い空白後は初速を暴発させない

            if (msg == WM_POINTERWHEEL)
                inst->engine.feedDelta(0, px, dt);
            else
                inst->engine.feedDelta(px, 0, dt);

            // タッチ中の生デルタは滞留バッファへ（物理スレッドがフレーム単位で送出）
            if (msg == WM_POINTERWHEEL) inst->pending_dy += px;
            else                        inst->pending_dx += px;
        }
        SetEvent(inst->wake_event);
        // ジェスチャ終了検知タイマーをリセット（デルタ到着のたびに再武装）
        SetTimer(hwnd, SUBCLASS_ID, 30, nullptr);
        return 0; // 生メッセージは飲み込む（二重スクロール防止）
    }

    // タッチパッドの2本指パンは「ジェスチャ終了」を明示するメッセージが無い。
    // 一定時間デルタが来なければ離れたとみなすタイマーで release を判定する。
    case WM_TIMER:
        if (wParam == SUBCLASS_ID) {
            std::lock_guard<std::mutex> lk(inst->mtx);
            if (GetTickCount64() - inst->last_tick > 90) { // 90ms無入力でリリース
                inst->engine.release();
                KillTimer(hwnd, SUBCLASS_ID);
                SetEvent(inst->wake_event);
            }
            return 0;
        }
        break;

    case WM_POINTERDOWN:
        if (IsTouchpadPointer(wParam)) {
            std::lock_guard<std::mutex> lk(inst->mtx);
            inst->engine.cancel(); // 新しいタッチで慣性をキャンセル
        }
        break;

    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, SubclassProc, SUBCLASS_ID);
        break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

} // namespace

// ---- 公開API --------------------------------------------------------------
SS_API void ss_default_config(SS_Config* cfg) {
    if (!cfg) return;
    cfg->decay_rate        = 4.0f;
    cfg->velocity_ema      = 0.35f;
    cfg->stop_velocity     = 25.0f;
    cfg->max_velocity      = 6000.0f;
    cfg->pixel_per_wheel   = 100.0f;
    cfg->enable_horizontal = TRUE;
    cfg->pass_through_wheel = TRUE;
}

SS_API HANDLE ss_attach(HWND hwnd, const SS_Config* cfg, SS_ScrollCallback cb, void* user) {
    if (!hwnd || !cb) return nullptr;

    SS_Config c;
    if (cfg) c = *cfg; else ss_default_config(&c);

    Instance* inst = new (std::nothrow) Instance(c);
    if (!inst) return nullptr;
    inst->hwnd = hwnd;
    inst->cb = cb;
    inst->user = user;
    inst->wake_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (!SetWindowSubclass(hwnd, SubclassProc, SUBCLASS_ID,
                           reinterpret_cast<DWORD_PTR>(inst))) {
        CloseHandle(inst->wake_event);
        delete inst;
        return nullptr;
    }

    inst->thread = CreateThread(nullptr, 0, PhysicsThread, inst, 0, nullptr);
    if (!inst->thread) {
        RemoveWindowSubclass(hwnd, SubclassProc, SUBCLASS_ID);
        CloseHandle(inst->wake_event);
        delete inst;
        return nullptr;
    }
    return (HANDLE)inst;
}

// ============================================================
// オーバースクロール ユーティリティ実装
// ============================================================

SS_API float ss_rubberband_delta(float raw_delta, float pos, float min_pos, float max_pos, float coeff) {
    if (coeff <= 0.0f) coeff = 0.55f;

    // 現在すでに境界外にいる場合（オーバースクロール中にさらに動かす/戻す）
    float overshoot = 0.0f;
    if (pos < min_pos) overshoot = min_pos - pos;
    else if (pos > max_pos) overshoot = pos - max_pos;

    if (overshoot > 0.0f) {
        // 食い込み量が大きいほど生デルタを強く減衰させる
        float resistance = coeff / (coeff + overshoot);
        return raw_delta * resistance;
    }

    // 範囲内: このフレームで境界を超える分だけ圧縮する
    float new_pos = pos + raw_delta;
    if (new_pos < min_pos) {
        float within = min_pos - pos;        // 境界までの距離（>=0）
        float beyond = min_pos - new_pos;    // 境界を超えた生の量
        float resisted = coeff * beyond / (coeff + beyond);
        return -within - resisted;
    }
    if (new_pos > max_pos) {
        float within = max_pos - pos;
        float beyond = new_pos - max_pos;
        float resisted = coeff * beyond / (coeff + beyond);
        return within + resisted;
    }
    return raw_delta; // 範囲内で完結、抵抗なし
}

SS_API float ss_spring_step(float pos, float target, float* velocity, float dt_sec,
                             float stiffness, float damping) {
    if (!velocity) return pos;
    if (stiffness <= 0.0f) stiffness = 200.0f;
    if (damping <= 0.0f) damping = 20.0f;

    // 減衰調和振動子: a = -k*x - c*v
    float displacement = pos - target;
    float accel = -stiffness * displacement - damping * (*velocity);
    *velocity += accel * dt_sec;
    pos += (*velocity) * dt_sec;
    return pos;
}

SS_API BOOL ss_spring_settled(float pos, float target, float velocity,
                               float pos_epsilon, float vel_epsilon) {
    return (std::fabs(pos - target) < pos_epsilon && std::fabs(velocity) < vel_epsilon) ? TRUE : FALSE;
}

SS_API void ss_detach(HANDLE h) {
    Instance* inst = static_cast<Instance*>(h);
    if (!inst) return;
    inst->quit.store(true);
    SetEvent(inst->wake_event);
    if (inst->thread) {
        WaitForSingleObject(inst->thread, 2000);
        CloseHandle(inst->thread);
    }
    RemoveWindowSubclass(inst->hwnd, SubclassProc, SUBCLASS_ID);
    CloseHandle(inst->wake_event);
    delete inst;
}

SS_API BOOL ss_is_scrolling(HANDLE h) {
    Instance* inst = static_cast<Instance*>(h);
    if (!inst) return FALSE;
    std::lock_guard<std::mutex> lk(inst->mtx);
    return inst->engine.isActive() ? TRUE : FALSE;
}

SS_API void ss_cancel_momentum(HANDLE h) {
    Instance* inst = static_cast<Instance*>(h);
    if (!inst) return;
    std::lock_guard<std::mutex> lk(inst->mtx);
    inst->engine.cancel();
}
