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

// 重要: これらのタイマーIDは ss_attach 先の HWND に対して SetTimer/KillTimer される。
// ホストアプリが同じ HWND に対して独自の SetTimer(hwnd, 1, ...) のような
// 小さい/衝突しやすいIDを使っていた場合、SubclassProc がホストより先に
// WM_TIMER を横取りして必ず return 0 してしまうため、ホスト側のタイマーハンドラに
// メッセージが一切届かなくなるという実害のあるバグを踏む(実際に踏んだ)。
// そのため、一般的なアプリが使うことがまずないユニークな値にしてある。
constexpr UINT_PTR PAN_TIMER_ID     = 0x53534D01; // パン用: 90ms無入力でリリース判定
constexpr UINT_PTR ZOOM_TIMER_ID    = 0x53534D02; // ズーム用: 同様に90ms無入力でリリース判定
constexpr UINT     WM_SS_WAKE       = WM_APP + 0x53; // 物理ループ起床用

// Precision Touchpad は HID Digitizer コレクション
// (Usage Page 0x0D = Digitizers, Usage 0x05 = Touch Pad) として
// Raw Input を送り続ける。この専用チャンネルでの活動有無を
// 「タッチパッドかどうか」の判定材料に使う。
constexpr USHORT HID_USAGE_PAGE_DIGITIZER = 0x0D;
constexpr USHORT HID_USAGE_TOUCH_PAD      = 0x05;
constexpr ULONGLONG TOUCHPAD_ACTIVITY_WINDOW_MS = 250;

// QueryPerformanceCounter の周波数はプロセス内で不変なので一度だけ取得してキャッシュする。
LARGE_INTEGER GetQpcFrequency() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    return freq;
}

// ズーム用の物理エンジンには、パン用とは別の減衰・停止しきい値を使う
// (対数スケール空間の値はピクセル空間と単位のスケール感が全く異なるため)。
SS_Config MakeZoomPhysicsConfig(const SS_Config& c) {
    SS_Config zc = c;
    zc.decay_rate    = c.zoom_decay_rate;
    zc.stop_velocity = c.zoom_stop_velocity;
    zc.max_velocity  = c.zoom_max_velocity;
    return zc;
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
    HANDLE wake_event = nullptr;   // デルタ到着で起床
    std::atomic<bool> quit{false};

    ULONGLONG last_tick_ms = 0;    // 前回パンイベント時刻(ms, リリース判定用)
    LARGE_INTEGER last_qpc{};      // 前回パンイベント時刻(高分解能, dt計算用)
    float pending_dx = 0, pending_dy = 0; // 未送出のタッチ中パンデルタ

    ULONGLONG last_zoom_tick_ms = 0; // 前回ズームイベント時刻(ms)
    LARGE_INTEGER last_zoom_qpc{};   // 前回ズームイベント時刻(高分解能)
    float pending_zoom_log = 0;      // 未送出のズーム対数デルタ
    float zoom_anchor_x = 0, zoom_anchor_y = 0; // ズーム中心点(クライアント座標)

    std::atomic<ULONGLONG> last_touchpad_raw_tick{0}; // 直近のタッチパッドRaw Input時刻
    bool raw_input_registered = false;

    explicit Instance(const SS_Config& c)
        : cfg(c), engine(c), zoom_engine(MakeZoomPhysicsConfig(c)) {}
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

        // ---- パン: タッチ中の生デルタをそのまま送出 ----
        float tdx = 0, tdy = 0;
        {
            std::lock_guard<std::mutex> lk(inst->mtx);
            tdx = inst->pending_dx; tdy = inst->pending_dy;
            inst->pending_dx = inst->pending_dy = 0;
        }
        if (tdx != 0 || tdy != 0)
            inst->cb(tdx, tdy, inst->user);

        // ---- パン: 慣性フェーズ ----
        // pan_subpixel が有効なら整数丸めをしない stepContinuous() を使い、
        // 慣性終盤の量子化ジッター(「ピタッ…ピタッ」という1px刻みの飛び)を回避する。
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

        // ---- ズーム: タッチ(ホイール操作)中の生デルタをそのまま送出 ----
        float tzoom = 0;
        float zoomAnchorX = 0, zoomAnchorY = 0;
        {
            std::lock_guard<std::mutex> lk(inst->mtx);
            tzoom = inst->pending_zoom_log;
            inst->pending_zoom_log = 0;
            zoomAnchorX = inst->zoom_anchor_x;
            zoomAnchorY = inst->zoom_anchor_y;
        }
        if (tzoom != 0 && inst->cb_zoom) {
            float scaleFactor = std::exp(tzoom);
            inst->cb_zoom(scaleFactor, zoomAnchorX, zoomAnchorY, inst->user);
        }

        // ---- ズーム: 慣性フェーズ(対数空間で減衰させ、exp()で倍率に戻す) ----
        float mzoom = 0, dummy = 0;
        bool zoomActive;
        {
            std::lock_guard<std::mutex> lk(inst->mtx);
            zoomActive = inst->zoom_engine.stepContinuous(frame_dt, mzoom, dummy);
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

        // 重要: EnableMouseInPointer(TRUE)が有効な環境では、ピンチズームの
        // 「Ctrl+ホイール」互換シグナルも classic WM_MOUSEWHEEL ではなく、
        // この WM_POINTERWHEEL として届く。
        // ただし wParam に埋め込まれたキー修飾フラグ(MK_CONTROL)には
        // 実機検証の結果、決して乗らない(常にno)ことが確認されている。
        // GetKeyState(VK_CONTROL) も、キーを物理的に押しっぱなしにしても
        // 数回で検出が途切れることがあり信頼できなかった。
        // 最終的に GetAsyncKeyState(VK_CONTROL) で毎回直接OSに問い合わせる
        // 方式が最も安定していることが実機検証(複数回・数百イベント規模)で
        // 確認された。純粋なパン動作ではほぼ反応せず、実際のピンチ動作でのみ
        // 高頻度でtrueになる。
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
                    inst->pending_zoom_log += logStep;
                    inst->zoom_anchor_x = (float)pt.x;
                    inst->zoom_anchor_y = (float)pt.y;
                }
                SetEvent(inst->wake_event);
                SetTimer(hwnd, ZOOM_TIMER_ID, 90, nullptr);
                return 0; // パンとしては処理しない
            }
        }

        if (msg == WM_POINTERHWHEEL && !inst->cfg.enable_horizontal) break;

        short delta = GET_WHEEL_DELTA_WPARAM(wParam);
        float px = (float)delta / (float)WHEEL_DELTA * inst->cfg.pixel_per_wheel;

        // ホスト側の希望に応じて符号を反転する(「ナチュラルスクロール」方向の切り替え)。
        if (msg == WM_POINTERWHEEL && inst->cfg.invert_vertical) px = -px;
        if (msg == WM_POINTERHWHEEL && inst->cfg.invert_horizontal) px = -px;

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
                inst->engine.feedDelta(0, px, dt, /*update_x=*/false, /*update_y=*/true);
            else
                inst->engine.feedDelta(px, 0, dt, /*update_x=*/true, /*update_y=*/false);

            if (msg == WM_POINTERWHEEL) inst->pending_dy += px;
            else                        inst->pending_dx += px;
        }
        SetEvent(inst->wake_event);
        SetTimer(hwnd, PAN_TIMER_ID, 30, nullptr);
        return 0;
    }

    // classic マウスホイール経由のCtrl+ホイール(EnableMouseInPointerを
    // 使わない環境向けのフォールバック経路)。
    case WM_MOUSEWHEEL: {
        if (!inst->cfg.enable_zoom || !inst->cb_zoom) break;

        bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        if (!ctrlDown) break; // 通常ホイールは素通し

        short delta = GET_WHEEL_DELTA_WPARAM(wParam);
        float logStep = (float)delta / (float)WHEEL_DELTA * inst->cfg.zoom_step;

        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd, &pt);

        LARGE_INTEGER now_qpc;
        QueryPerformanceCounter(&now_qpc);
        ULONGLONG now_ms = GetTickCount64();

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
            inst->pending_zoom_log += logStep;
            inst->zoom_anchor_x = (float)pt.x;
            inst->zoom_anchor_y = (float)pt.y;
        }
        SetEvent(inst->wake_event);
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

    float overshoot = 0.0f;
    bool low = false;
    if (pos < min_pos) { overshoot = min_pos - pos; low = true; }
    else if (pos > max_pos) { overshoot = pos - max_pos; }

    if (overshoot > 0.0f) {
        // 既にオーバースクロール中: 押し込む方向のみ抵抗する非対称な挙動にする。
        // 「戻ろうとする」方向(境界内へ近づく方向)は抵抗せずそのまま通す。
        bool movingBack = low ? (raw_delta > 0.0f) : (raw_delta < 0.0f);
        if (movingBack) {
            return raw_delta;
        }
        float resistance = coeff / (coeff + overshoot);
        return raw_delta * resistance;
    }

    float new_pos = pos + raw_delta;
    if (new_pos < min_pos) {
        float within = pos - min_pos;
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