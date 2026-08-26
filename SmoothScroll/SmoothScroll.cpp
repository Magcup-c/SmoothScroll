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

// Precision Touchpad は HID Digitizer コレクション
// (Usage Page 0x0D = Digitizers, Usage 0x05 = Touch Pad) として
// Raw Input を送り続ける。この専用チャンネルでの活動有無を
// 「タッチパッドかどうか」の判定材料に使う。
// (WM_POINTERWHEEL の POINTER_INFO.sourceDevice は
//  EnableMouseInPointer(TRUE) 環境下では合成イベントとなり
//  無効ハンドル(-1)になるため、そちら経由での判定は不可能)
constexpr USHORT HID_USAGE_PAGE_DIGITIZER = 0x0D;
constexpr USHORT HID_USAGE_TOUCH_PAD      = 0x05;
constexpr ULONGLONG TOUCHPAD_ACTIVITY_WINDOW_MS = 250; // この時間内のRaw Input活動があればタッチパッド由来とみなす

// QueryPerformanceCounter の周波数はプロセス内で不変なので一度だけ取得してキャッシュする。
// (GetTickCount64は分解能が粗く(既定約15.6ms)、指を離した瞬間の速度計算に使うと
//  瞬間速度が過大に跳ねて「離した瞬間に飛んでいく」原因になり得るため、
//  イベント間dtの計算には高分解能タイマを使う)
LARGE_INTEGER GetQpcFrequency() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    return freq;
}

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

    ULONGLONG last_tick_ms = 0;    // 前回イベント時刻(ms, リリース判定=90ms無入力チェック用)
    LARGE_INTEGER last_qpc{};      // 前回イベント時刻(高分解能, dt計算用)
    float pending_dx = 0, pending_dy = 0; // 未送出のタッチ中デルタ

    std::atomic<ULONGLONG> last_touchpad_raw_tick{0}; // 直近のタッチパッドRaw Input時刻
    bool raw_input_registered = false;

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

    int hz = 60;
    DEVMODEW dm{}; dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        hz = dm.dmDisplayFrequency;
    const float frame_dt = 1.0f / (float)hz;

    HANDLE waits[2] = { inst->wake_event, timer };
    bool animating = false;

    while (!inst->quit.load()) {
        if (!animating) {
            WaitForSingleObject(inst->wake_event, INFINITE);
            if (inst->quit.load()) break;
        }

        LARGE_INTEGER due{};
        due.QuadPart = -(LONGLONG)(frame_dt * 10000000.0f);
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);

        DWORD r = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (inst->quit.load()) break;

        float tdx = 0, tdy = 0;
        {
            std::lock_guard<std::mutex> lk(inst->mtx);
            tdx = inst->pending_dx; tdy = inst->pending_dy;
            inst->pending_dx = inst->pending_dy = 0;
        }
        if (tdx != 0 || tdy != 0)
            inst->cb(tdx, tdy, inst->user);

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

// ---- タッチパッド判定 ------------------------------------------------
bool IsTouchpadActive(Instance* inst) {
    ULONGLONG now = GetTickCount64();
    ULONGLONG last = inst->last_touchpad_raw_tick.load();
    return (last != 0) && ((now - last) <= TOUCHPAD_ACTIVITY_WINDOW_MS);
}

LRESULT CALLBACK SubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                              UINT_PTR, DWORD_PTR refData) {
    Instance* inst = reinterpret_cast<Instance*>(refData);

    switch (msg) {
    case WM_INPUT: {
        inst->last_touchpad_raw_tick.store(GetTickCount64());
        break;
    }

    case WM_POINTERWHEEL:
    case WM_POINTERHWHEEL: {
        if (!IsTouchpadActive(inst)) break;

        if (msg == WM_POINTERHWHEEL && !inst->cfg.enable_horizontal) break;

        short delta = GET_WHEEL_DELTA_WPARAM(wParam);
        float px = (float)delta / (float)WHEEL_DELTA * inst->cfg.pixel_per_wheel;

        LARGE_INTEGER now_qpc;
        QueryPerformanceCounter(&now_qpc);
        ULONGLONG now_ms = GetTickCount64();

        {
            std::lock_guard<std::mutex> lk(inst->mtx);

            float dt;
            if (inst->last_tick_ms == 0) {
                dt = 1.0f / 60.0f;
            } else {
                LARGE_INTEGER freq = GetQpcFrequency();
                LONGLONG diff_ticks = now_qpc.QuadPart - inst->last_qpc.QuadPart;
                dt = (freq.QuadPart > 0)
                    ? (float)((double)diff_ticks / (double)freq.QuadPart)
                    : 1.0f / 60.0f;
                if (dt > 0.25f) dt = 1.0f / 60.0f;
            }
            inst->last_tick_ms = now_ms;
            inst->last_qpc = now_qpc;

            if (msg == WM_POINTERWHEEL)
                inst->engine.feedDelta(0, px, dt);
            else
                inst->engine.feedDelta(px, 0, dt);

            if (msg == WM_POINTERWHEEL) inst->pending_dy += px;
            else                        inst->pending_dx += px;
        }
        SetEvent(inst->wake_event);
        SetTimer(hwnd, SUBCLASS_ID, 30, nullptr);
        return 0;
    }

    case WM_TIMER:
        if (wParam == SUBCLASS_ID) {
            std::lock_guard<std::mutex> lk(inst->mtx);
            if (GetTickCount64() - inst->last_tick_ms > 90) {
                inst->engine.release();
                KillTimer(hwnd, SUBCLASS_ID);
                SetEvent(inst->wake_event);
            }
            return 0;
        }
        break;

    case WM_POINTERDOWN:
        if (IsTouchpadActive(inst)) {
            std::lock_guard<std::mutex> lk(inst->mtx);
            inst->engine.cancel(); // 新しいタッチで慣性をキャンセル（本当の新規セッション）
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

    RAWINPUTDEVICE rid{};
    rid.usUsagePage = HID_USAGE_PAGE_DIGITIZER;
    rid.usUsage = HID_USAGE_TOUCH_PAD;
    rid.dwFlags = RIDEV_INPUTSINK;
    rid.hwndTarget = hwnd;
    inst->raw_input_registered = RegisterRawInputDevices(&rid, 1, sizeof(rid)) == TRUE;

    inst->thread = CreateThread(nullptr, 0, PhysicsThread, inst, 0, nullptr);
    if (!inst->thread) {
        if (inst->raw_input_registered) {
            RAWINPUTDEVICE rem{};
            rem.usUsagePage = HID_USAGE_PAGE_DIGITIZER;
            rem.usUsage = HID_USAGE_TOUCH_PAD;
            rem.dwFlags = RIDEV_REMOVE;
            rem.hwndTarget = nullptr;
            RegisterRawInputDevices(&rem, 1, sizeof(rem));
        }
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

    float overshoot = 0.0f;
    if (pos < min_pos) overshoot = min_pos - pos;
    else if (pos > max_pos) overshoot = pos - max_pos;

    if (overshoot > 0.0f) {
        float resistance = coeff / (coeff + overshoot);
        return raw_delta * resistance;
    }

    float new_pos = pos + raw_delta;
    if (new_pos < min_pos) {
        float within = min_pos - pos;
        float beyond = min_pos - new_pos;
        float resisted = coeff * beyond / (coeff + beyond);
        return -within - resisted;
    }
    if (new_pos > max_pos) {
        float within = max_pos - pos;
        float beyond = new_pos - max_pos;
        float resisted = coeff * beyond / (coeff + beyond);
        return within + resisted;
    }
    return raw_delta;
}

SS_API float ss_spring_step(float pos, float target, float* velocity, float dt_sec,
                             float stiffness, float damping) {
    if (!velocity) return pos;
    if (stiffness <= 0.0f) stiffness = 200.0f;
    if (damping <= 0.0f) damping = 20.0f;

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
    if (inst->raw_input_registered) {
        RAWINPUTDEVICE rem{};
        rem.usUsagePage = HID_USAGE_PAGE_DIGITIZER;
        rem.usUsage = HID_USAGE_TOUCH_PAD;
        rem.dwFlags = RIDEV_REMOVE;
        rem.hwndTarget = nullptr;
        RegisterRawInputDevices(&rem, 1, sizeof(rem));
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

SS_API void ss_stop_inertia(HANDLE h) {
    Instance* inst = static_cast<Instance*>(h);
    if (!inst) return;
    std::lock_guard<std::mutex> lk(inst->mtx);
    inst->engine.stopMomentumOnly();
}