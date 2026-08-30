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

constexpr UINT_PTR SUBCLASS_ID      = 0x53534D4B; // 'SSMK'

constexpr UINT_PTR PAN_TIMER_ID     = 0x53534D01; // パン用: 90ms無入力でリリース判定
constexpr UINT_PTR ZOOM_TIMER_ID    = 0x53534D02; // ズーム用: 同様に90ms無入力でリリース判定
constexpr UINT     WM_SS_WAKE       = WM_APP + 0x53; // 物理ループ起床用

constexpr USHORT HID_USAGE_PAGE_DIGITIZER = 0x0D;
constexpr USHORT HID_USAGE_TOUCH_PAD      = 0x05;
constexpr ULONGLONG TOUCHPAD_ACTIVITY_WINDOW_MS = 250;

LARGE_INTEGER GetQpcFrequency() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    return freq;
}

SS_Config MakeZoomPhysicsConfig(const SS_Config& c) {
    SS_Config zc = c;
    zc.decay_rate    = c.zoom_decay_rate;
    zc.stop_velocity = c.zoom_stop_velocity;
    zc.max_velocity  = c.zoom_max_velocity;
    return zc;
}

// ============================================================
// ラバーバンド圧縮曲線 f(o) = coeff*o / (coeff+o)
// ============================================================
inline float RubberbandCompress(float o, float coeff) {
    if (o <= 0.0f) return 0.0f;
    return coeff * o / (coeff + o);
}

inline float RubberbandExpand(float d, float coeff) {
    if (d <= 0.0f) return 0.0f;
    if (d >= coeff) d = coeff * 0.999999f;
    return coeff * d / (coeff - d);
}

struct Instance {
    HWND hwnd = nullptr;
    SS_Config cfg{};
    SS_ScrollCallback cb = nullptr;
    SS_ZoomCallback   cb_zoom = nullptr;
    void* user = nullptr;

    PhysicsEngine engine;       // パン用
    PhysicsEngine zoom_engine;  // ズーム用(対数スケール空間の1次元位置として使う)
    std::mutex mtx;

    HANDLE thread = nullptr;
    HANDLE wake_event = nullptr;   // 慣性/バネフェーズの起床用(タッチ中の生デルタはもう経由しない)
    std::atomic<bool> quit{false};

    // リリース判定(PAN_TIMER)専用: 縦横どちらの軸のイベントでも更新される
    // 「直近のパン活動時刻」。90ms操作なしで release() する判定にのみ使う。
    ULONGLONG last_tick_ms = 0;

    // 「凍結側の軸を減衰させる」ために使う、縦横共通の高分解能時刻。
    LARGE_INTEGER last_shared_qpc{};

    // 速度推定(EMA)専用: 縦横それぞれ独立に持つ「同じ軸の前回イベントからの
    // 経過時間」計算用。凍結側の減衰に流用してはいけない
    // (詳細はPhysicsEngine::feedDeltaのコメント参照)。
    ULONGLONG last_v_tick_ms = 0;    // 直近の縦(WM_POINTERWHEEL)イベント時刻
    LARGE_INTEGER last_v_qpc{};
    ULONGLONG last_h_tick_ms = 0;    // 直近の横(WM_POINTERHWHEEL)イベント時刻
    LARGE_INTEGER last_h_qpc{};

    ULONGLONG last_zoom_tick_ms = 0; // 前回ズームイベント時刻(ms)
    LARGE_INTEGER last_zoom_qpc{};   // 前回ズームイベント時刻(高分解能)
    float zoom_anchor_x = 0, zoom_anchor_y = 0; // ズーム中心点(クライアント座標、慣性フェーズが参照する)

    std::atomic<ULONGLONG> last_touchpad_raw_tick{0}; // 直近のタッチパッドRaw Input時刻
    bool raw_input_registered = false;

    explicit Instance(const SS_Config& c)
        : cfg(c), engine(c), zoom_engine(MakeZoomPhysicsConfig(c)) {}
};

// ---- 物理ループスレッド --------------------------------------------------
// 高分解能ウェイタブルタイマでディスプレイ周期(既定60Hz相当)に駆動。
// アイドル時はイベント待機でCPUを消費しない。
//
// 重要: タッチ中の生デルタの1:1パス送出は、ここではなくSubclassProcから
// 直接コールバックを呼ぶように変更した(レイテンシ削減のため)。
// このスレッドが担当するのは、指が離れた後の慣性/バネへ引き継がれた後の
// 「時間駆動で計算が必要なアニメーション」だけである。
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

        // ---- パン: 慣性フェーズ ----
        float mdx = 0, mdy = 0;
        bool panActive;
        {
            std::lock_guard<std::mutex> lk(inst->mtx);
            panActive = inst->cfg.pan_subpixel
                ? inst->engine.stepContinuous(frame_dt, mdx, mdy)
                : inst->engine.step(frame_dt, mdx, mdy);
        }
        if (mdx != 0 || mdy != 0)
            inst->cb(mdx, mdy, inst->user);

        // ---- ズーム: 慣性フェーズ(対数空間で減衰させ、exp()で倍率に戻す) ----
        float mzoom = 0, dummy = 0;
        bool zoomActive;
        float zoomAnchorX, zoomAnchorY;
        {
            std::lock_guard<std::mutex> lk(inst->mtx);
            zoomActive = inst->zoom_engine.stepContinuous(frame_dt, mzoom, dummy);
            zoomAnchorX = inst->zoom_anchor_x;
            zoomAnchorY = inst->zoom_anchor_y;
        }
        if (mzoom != 0.0f && inst->cb_zoom) {
            float scaleFactor = std::exp(mzoom);
            inst->cb_zoom(scaleFactor, zoomAnchorX, zoomAnchorY, inst->user);
        }

        animating = panActive || zoomActive;
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

        {
            bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            if (ctrlDown && inst->cfg.enable_zoom && inst->cb_zoom) {
                short zdelta = GET_WHEEL_DELTA_WPARAM(wParam);
                float logStep = (float)zdelta / (float)WHEEL_DELTA * inst->cfg.zoom_step;

                POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                ScreenToClient(hwnd, &pt);

                LARGE_INTEGER now_qpc;
                QueryPerformanceCounter(&now_qpc);
                ULONGLONG now_ms = GetTickCount64();
                float scaleFactor;

                {
                    std::lock_guard<std::mutex> lk(inst->mtx);
                    float dt;
                    if (inst->last_zoom_tick_ms == 0) {
                        dt = 1.0f / 60.0f;
                    } else {
                        LARGE_INTEGER freq = GetQpcFrequency();
                        LONGLONG diff_ticks = now_qpc.QuadPart - inst->last_zoom_qpc.QuadPart;
                        dt = (freq.QuadPart > 0)
                            ? (float)((double)diff_ticks / (double)freq.QuadPart)
                            : 1.0f / 60.0f;
                        if (dt <= 0.0f || dt > 0.25f) dt = 1.0f / 60.0f;
                    }
                    inst->last_zoom_tick_ms = now_ms;
                    inst->last_zoom_qpc = now_qpc;

                    inst->zoom_engine.feedDelta(logStep, 0, dt);
                    inst->zoom_anchor_x = (float)pt.x;
                    inst->zoom_anchor_y = (float)pt.y;
                    scaleFactor = std::exp(logStep);
                }
                // タッチ(ホイール操作)中の生デルタは、物理ループスレッドの
                // 起床待ちを挟まず、このスレッドから直接コールバックを呼ぶ。
                inst->cb_zoom(scaleFactor, pt.x, pt.y, inst->user);
                SetTimer(hwnd, ZOOM_TIMER_ID, 90, nullptr);
                return 0; // パンとしては処理しない
            }
        }

        if (msg == WM_POINTERHWHEEL && !inst->cfg.enable_horizontal) break;

        short delta = GET_WHEEL_DELTA_WPARAM(wParam);
        float px = (float)delta / (float)WHEEL_DELTA * inst->cfg.pixel_per_wheel;

        if (msg == WM_POINTERWHEEL && inst->cfg.invert_vertical) px = -px;
        if (msg == WM_POINTERHWHEEL && inst->cfg.invert_horizontal) px = -px;

        LARGE_INTEGER now_qpc;
        QueryPerformanceCounter(&now_qpc);
        ULONGLONG now_ms = GetTickCount64();

        {
            std::lock_guard<std::mutex> lk(inst->mtx);

            LARGE_INTEGER freq = GetQpcFrequency();

            // (1) 速度推定用dt: 「同じ軸の」前回イベントからの経過時間。
            bool isVertical = (msg == WM_POINTERWHEEL);
            ULONGLONG& lastAxisTickMs = isVertical ? inst->last_v_tick_ms : inst->last_h_tick_ms;
            LARGE_INTEGER& lastAxisQpc = isVertical ? inst->last_v_qpc : inst->last_h_qpc;

            float dt;
            if (lastAxisTickMs == 0) {
                dt = 1.0f / 60.0f;
            } else {
                LONGLONG diff_ticks = now_qpc.QuadPart - lastAxisQpc.QuadPart;
                dt = (freq.QuadPart > 0)
                    ? (float)((double)diff_ticks / (double)freq.QuadPart)
                    : 1.0f / 60.0f;
                if (dt > 0.25f) dt = 1.0f / 60.0f;
            }
            lastAxisTickMs = now_ms;
            lastAxisQpc = now_qpc;

            // (2) 凍結側減衰用dt: 縦横どちらのイベントでもよい、前回呼び出しからの経過時間。
            float freezeDt;
            if (inst->last_shared_qpc.QuadPart == 0) {
                freezeDt = 1.0f / 60.0f;
            } else {
                LONGLONG diff_ticks = now_qpc.QuadPart - inst->last_shared_qpc.QuadPart;
                freezeDt = (freq.QuadPart > 0)
                    ? (float)((double)diff_ticks / (double)freq.QuadPart)
                    : 1.0f / 60.0f;
                if (freezeDt > 0.25f) freezeDt = 1.0f / 60.0f;
            }
            inst->last_shared_qpc = now_qpc;

            inst->last_tick_ms = now_ms;

            if (msg == WM_POINTERWHEEL)
                inst->engine.feedDelta(0, px, dt, freezeDt, /*update_x=*/false, /*update_y=*/true);
            else
                inst->engine.feedDelta(px, 0, dt, freezeDt, /*update_x=*/true, /*update_y=*/false);
        }

        // タッチ中の生デルタは物理ループスレッドを経由させず、このスレッドから
        // 直接コールバックを呼ぶ(1:1パス動作に不要な1往復分のレイテンシを削る)。
        if (msg == WM_POINTERWHEEL) inst->cb(0, px, inst->user);
        else                        inst->cb(px, 0, inst->user);

        SetTimer(hwnd, PAN_TIMER_ID, 30, nullptr);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        if (!inst->cfg.enable_zoom || !inst->cb_zoom) break;

        bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        if (!ctrlDown) break;

        short delta = GET_WHEEL_DELTA_WPARAM(wParam);
        float logStep = (float)delta / (float)WHEEL_DELTA * inst->cfg.zoom_step;

        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd, &pt);

        LARGE_INTEGER now_qpc;
        QueryPerformanceCounter(&now_qpc);
        ULONGLONG now_ms = GetTickCount64();
        float scaleFactor;

        {
            std::lock_guard<std::mutex> lk(inst->mtx);
            float dt;
            if (inst->last_zoom_tick_ms == 0) {
                dt = 1.0f / 60.0f;
            } else {
                LARGE_INTEGER freq = GetQpcFrequency();
                LONGLONG diff_ticks = now_qpc.QuadPart - inst->last_zoom_qpc.QuadPart;
                dt = (freq.QuadPart > 0)
                    ? (float)((double)diff_ticks / (double)freq.QuadPart)
                    : 1.0f / 60.0f;
                if (dt <= 0.0f || dt > 0.25f) dt = 1.0f / 60.0f;
            }
            inst->last_zoom_tick_ms = now_ms;
            inst->last_zoom_qpc = now_qpc;

            inst->zoom_engine.feedDelta(logStep, 0, dt);
            inst->zoom_anchor_x = (float)pt.x;
            inst->zoom_anchor_y = (float)pt.y;
            scaleFactor = std::exp(logStep);
        }
        inst->cb_zoom(scaleFactor, pt.x, pt.y, inst->user);
        SetTimer(hwnd, ZOOM_TIMER_ID, 90, nullptr);
        return 0;
    }

    case WM_TIMER:
        if (wParam == PAN_TIMER_ID) {
            std::lock_guard<std::mutex> lk(inst->mtx);
            if (GetTickCount64() - inst->last_tick_ms > 90) {
                inst->engine.release();
                KillTimer(hwnd, PAN_TIMER_ID);
                SetEvent(inst->wake_event);
            }
            return 0;
        }
        if (wParam == ZOOM_TIMER_ID) {
            std::lock_guard<std::mutex> lk(inst->mtx);
            if (GetTickCount64() - inst->last_zoom_tick_ms > 90) {
                inst->zoom_engine.release();
                KillTimer(hwnd, ZOOM_TIMER_ID);
                SetEvent(inst->wake_event);
            }
            return 0;
        }
        break;

    case WM_POINTERDOWN:
        if (IsTouchpadActive(inst)) {
            std::lock_guard<std::mutex> lk(inst->mtx);
            inst->engine.cancel();
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

    cfg->pan_subpixel      = FALSE;

    cfg->invert_vertical   = FALSE;
    cfg->invert_horizontal = FALSE;

    cfg->enable_zoom        = FALSE;
    cfg->zoom_step          = 0.10f;
    cfg->zoom_decay_rate    = 4.0f;
    cfg->zoom_stop_velocity = 0.05f;
    cfg->zoom_max_velocity  = 6.0f;
}

SS_API HANDLE ss_attach(HWND hwnd, const SS_Config* cfg, SS_ScrollCallback cb, SS_ZoomCallback cb_zoom, void* user) {
    if (!hwnd || !cb) return nullptr;

    SS_Config c;
    if (cfg) c = *cfg; else ss_default_config(&c);

    Instance* inst = new (std::nothrow) Instance(c);
    if (!inst) return nullptr;
    inst->hwnd = hwnd;
    inst->cb = cb;
    inst->cb_zoom = cb_zoom;
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

    bool high;
    float excess;
    float delta;

    if (pos > max_pos) {
        high = true;
        excess = pos - max_pos;
        delta = raw_delta;
    } else if (pos < min_pos) {
        high = false;
        excess = min_pos - pos;
        delta = -raw_delta;
    } else {
        float new_pos = pos + raw_delta;
        if (new_pos > max_pos) {
            float beyond = new_pos - max_pos;
            float resisted = RubberbandCompress(beyond, coeff);
            return (max_pos - pos) + resisted;
        }
        if (new_pos < min_pos) {
            float beyond = min_pos - new_pos;
            float resisted = RubberbandCompress(beyond, coeff);
            return (min_pos - pos) - resisted;
        }
        return raw_delta;
    }

    float virtualExcess = RubberbandExpand(excess, coeff) + delta;

    if (virtualExcess <= 0.0f) {
        float total = -excess + virtualExcess;
        return high ? total : -total;
    }

    float newExcess = RubberbandCompress(virtualExcess, coeff);
    float appliedInPushDir = newExcess - excess;
    return high ? appliedInPushDir : -appliedInPushDir;
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

SS_API BOOL ss_is_touching(HANDLE h) {
    Instance* inst = static_cast<Instance*>(h);
    if (!inst) return FALSE;
    std::lock_guard<std::mutex> lk(inst->mtx);
    return inst->engine.isTouching() ? TRUE : FALSE;
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

SS_API void ss_scale_velocity(HANDLE h, float sx, float sy) {
    Instance* inst = static_cast<Instance*>(h);
    if (!inst) return;
    std::lock_guard<std::mutex> lk(inst->mtx);
    inst->engine.scaleVelocity(sx, sy);
}

SS_API void ss_get_velocity(HANDLE h, float* vx, float* vy) {
    Instance* inst = static_cast<Instance*>(h);
    if (!inst) return;
    std::lock_guard<std::mutex> lk(inst->mtx);
    float x = 0, y = 0;
    inst->engine.getVelocity(x, y);
    if (vx) *vx = x;
    if (vy) *vy = y;
}

SS_API void ss_set_invert(HANDLE h, BOOL invert_vertical, BOOL invert_horizontal) {
    Instance* inst = static_cast<Instance*>(h);
    if (!inst) return;
    std::lock_guard<std::mutex> lk(inst->mtx);
    inst->cfg.invert_vertical = invert_vertical;
    inst->cfg.invert_horizontal = invert_horizontal;
}