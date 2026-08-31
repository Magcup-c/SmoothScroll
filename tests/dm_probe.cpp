// dm_probe.cpp - DirectManipulation を使って Precision Touchpad の
// ピンチ(スケール)/回転/パンをコンソールに出力する検証用プログラム。
//
// 背景:
//   WM_POINTERDOWN/UPDATE による生座標追跡では、Precision Touchpad の
//   2本指ジェスチャはOS側のジェスチャーエンジンに横取りされてしまい、
//   アプリには1本指分の入力しか届かないことが multitouch_probe.cpp の
//   検証で確認された。
//   Precision Touchpad でピンチ・回転を得る正攻法は DirectManipulation
//   API 経由でOSが解釈済みの「変換行列」を受け取ること。
//
// ビルド (Developer Command Prompt):
//   cl /EHsc /std:c++17 dm_probe.cpp user32.lib ole32.lib
//
// 使い方:
//   ウィンドウ上で2本指ピンチ/回転/パンをすると、コンソールに
//   scale / rotation(deg) / translateX / translateY が出力される。
//
// 注意:
//   DirectManipulationはDirectCompositionと組み合わせるのが本来の使い方だが、
//   ここでは検証目的のため「手動更新モード」(MANUALUPDATE)で
//   IDirectManipulationUpdateManager::Update() をタイマ駆動で呼び、
//   IDirectManipulationContent::GetContentTransform() で行列を取得するだけの
//   最小構成にしている。

// NTDDI_VERSION/_WIN32_WINNT を明示的に指定する。
// これが無いと DIRECTMANIPULATION_CONFIGURATION_ROTATION 等、
// Windows 8.1以降で追加されたAPI定義が <directmanipulation.h> 内で
// #if NTDDI_VERSION >= NTDDI_WINBLUE により丸ごと無効化されてしまう。
// 必ず <Windows.h> や <sdkddkver.h> がインクルードされるより前に定義すること。
#ifndef NTDDI_VERSION
#define NTDDI_VERSION NTDDI_WIN10
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT _WIN32_WINNT_WIN10
#endif

#include <Windows.h>
#include <windowsx.h>
#include <directmanipulation.h>
#include <cstdio>
#include <cmath>
#include <wrl/client.h> // Microsoft::WRL::ComPtr

#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace {

void SetupDebugConsole() {
    AllocConsole();
    FILE* fp = nullptr;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);
    SetConsoleTitleW(L"SmoothScroll DirectManipulation Probe");
    printf("=== DirectManipulation Probe: console ready ===\n");
    printf("Pinch / rotate / pan with 2 fingers on your precision touchpad.\n\n");
}

ComPtr<IDirectManipulationManager>       g_manager;
ComPtr<IDirectManipulationUpdateManager> g_updateManager;
ComPtr<IDirectManipulationViewport>      g_viewport;
ComPtr<IDirectManipulationContent>       g_content;
DWORD g_eventCookie = 0;

constexpr UINT_PTR DM_TIMER_ID = 42;
constexpr UINT     DM_TIMER_MS = 16; // 約60Hz

// ---- イベントハンドラ ----------------------------------------------------
// ジェスチャの開始/終了状態や、行列更新のたびに呼ばれる。
// ここでは状態遷移のログのみ出し、実際の値取得はタイマ側でポーリングする
// (シンプルさ優先。本実装ではOnContentUpdated側で直接読む形にしても良い)。
class DMEventHandler : public IDirectManipulationViewportEventHandler {
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirectManipulationViewportEventHandler)) {
            *ppv = static_cast<IDirectManipulationViewportEventHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnViewportStatusChanged(
        IDirectManipulationViewport*, DIRECTMANIPULATION_STATUS current, DIRECTMANIPULATION_STATUS previous) override {
        const char* names[] = { "BUILDING", "ENABLED", "DISABLED", "RUNNING", "INERTIA", "READY", "SUSPENDED" };
        auto nameOf = [&](DIRECTMANIPULATION_STATUS s) -> const char* {
            int idx = (int)s;
            return (idx >= 0 && idx < (int)(sizeof(names) / sizeof(names[0]))) ? names[idx] : "UNKNOWN";
        };
        printf("\n[status] %s -> %s\n", nameOf(previous), nameOf(current));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnViewportUpdated(IDirectManipulationViewport*) override {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnContentUpdated(
        IDirectManipulationViewport*, IDirectManipulationContent*) override {
        // 実際の値表示はタイマ側で一括して行うのでここでは何もしない。
        return S_OK;
    }

private:
    volatile LONG ref_ = 1;
};

void PrintTransform() {
    if (!g_content) return;

    float matrix[6] = {};
    HRESULT hr = g_content->GetContentTransform(matrix, ARRAYSIZE(matrix));
    if (FAILED(hr)) return;

    // DirectManipulationの2D変換行列レイアウト:
    //   [ m11  m12  0 ]
    //   [ m21  m22  0 ]
    //   [ dx   dy   1 ]
    // matrix[] = { m11, m12, m21, m22, dx, dy }
    float m11 = matrix[0], m12 = matrix[1];
    float m21 = matrix[2], m22 = matrix[3];
    float dx  = matrix[4], dy  = matrix[5];

    float scale = std::sqrt(m11 * m11 + m12 * m12);
    float rotation_deg = std::atan2(m12, m11) * 180.0f / 3.14159265f;

    // 変化が小さいフレームは表示を省略しノイズを減らす
    static float last_scale = 1.0f, last_rot = 0.0f, last_dx = 0.0f, last_dy = 0.0f;
    bool changed = std::fabs(scale - last_scale) > 0.001f ||
                   std::fabs(rotation_deg - last_rot) > 0.05f ||
                   std::fabs(dx - last_dx) > 0.05f ||
                   std::fabs(dy - last_dy) > 0.05f;
    if (!changed) return;
    last_scale = scale; last_rot = rotation_deg; last_dx = dx; last_dy = dy;

    printf("\r[transform] scale=%.4f rotation=%+7.2fdeg dx=%+8.2f dy=%+8.2f          ",
           scale, rotation_deg, dx, dy);
    fflush(stdout);
}

bool InitDirectManipulation(HWND hwnd) {
    HRESULT hr;

    hr = CoCreateInstance(CLSID_DirectManipulationManager, nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(&g_manager));
    if (FAILED(hr)) { printf("CoCreateInstance failed: 0x%08X\n", hr); return false; }

    hr = g_manager->GetUpdateManager(IID_PPV_ARGS(&g_updateManager));
    if (FAILED(hr)) { printf("GetUpdateManager failed: 0x%08X\n", hr); return false; }

    hr = g_manager->CreateViewport(nullptr, hwnd, IID_PPV_ARGS(&g_viewport));
    if (FAILED(hr)) { printf("CreateViewport failed: 0x%08X\n", hr); return false; }

    // パン(X/Y) + ピンチ(スケール) + (可能なら)回転を有効化。
    // DIRECTMANIPULATION_CONFIGURATION_ROTATION / _ROTATION_INERTIA は
    // SDKバージョンによってヘッダに定義が無い場合があるため #ifdef で吸収する。
    // 定義が無い環境では回転フラグ無しでビルドが通り、実行時ログで
    // 回転が実際に取れるかどうかを別途確認する。
    DWORD config =
        DIRECTMANIPULATION_CONFIGURATION_INTERACTION |
        DIRECTMANIPULATION_CONFIGURATION_TRANSLATION_X |
        DIRECTMANIPULATION_CONFIGURATION_TRANSLATION_Y |
        DIRECTMANIPULATION_CONFIGURATION_SCALING |
        DIRECTMANIPULATION_CONFIGURATION_TRANSLATION_INERTIA |
        DIRECTMANIPULATION_CONFIGURATION_SCALING_INERTIA;

#ifdef DIRECTMANIPULATION_CONFIGURATION_ROTATION
    config |= DIRECTMANIPULATION_CONFIGURATION_ROTATION;
    printf("[config] ROTATION flag available in this SDK: enabled.\n");
#else
    printf("[config] ROTATION flag NOT available in this SDK: rotation config bit skipped.\n");
#endif
#ifdef DIRECTMANIPULATION_CONFIGURATION_ROTATION_INERTIA
    config |= DIRECTMANIPULATION_CONFIGURATION_ROTATION_INERTIA;
#endif

    hr = g_viewport->ActivateConfiguration((DIRECTMANIPULATION_CONFIGURATION)config);
    if (FAILED(hr)) { printf("ActivateConfiguration failed: 0x%08X\n", hr); return false; }

    // DirectCompositionを使わず手動でUpdate()を呼ぶモード
    hr = g_viewport->SetViewportOptions(DIRECTMANIPULATION_VIEWPORT_OPTIONS_MANUALUPDATE);
    if (FAILED(hr)) { printf("SetViewportOptions failed: 0x%08X\n", hr); return false; }

    RECT rc; GetClientRect(hwnd, &rc);
    hr = g_viewport->SetViewportRect(&rc);
    if (FAILED(hr)) { printf("SetViewportRect failed: 0x%08X\n", hr); return false; }

    auto* handler = new DMEventHandler();
    hr = g_viewport->AddEventHandler(hwnd, handler, &g_eventCookie);
    handler->Release(); // AddEventHandlerが内部でAddRefするので手元の参照は手放す
    if (FAILED(hr)) { printf("AddEventHandler failed: 0x%08X\n", hr); return false; }

    // アクティブ化後にビューポートが自動生成するプライマリコンテンツを取得。
    // これが「今どれだけ動かされたか」を表すTransformの取得元になる。
    hr = g_viewport->GetPrimaryContent(IID_PPV_ARGS(&g_content));
    if (FAILED(hr)) { printf("GetPrimaryContent failed: 0x%08X\n", hr); return false; }

    hr = g_viewport->Enable();
    if (FAILED(hr)) { printf("Enable failed: 0x%08X\n", hr); return false; }

    printf("DirectManipulation initialized OK.\n");
    return true;
}

void ShutdownDirectManipulation(HWND hwnd) {
    if (g_viewport) {
        if (g_eventCookie) g_viewport->RemoveEventHandler(g_eventCookie);
        g_viewport->Disable();
    }
    g_content.Reset();
    g_viewport.Reset();
    g_updateManager.Reset();
    g_manager.Reset();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        EnableMouseInPointer(TRUE);

        // Precision Touchpad の直接タッチ(接触)イベントを受け取るための登録。
        // これが無いと WM_POINTERDOWN 等がタッチパッド由来では飛んでこず、
        // DirectManipulation の SetContact が一度も呼ばれないため
        // ビューポートが永遠に ENABLED のまま(RUNNINGに遷移しない)可能性がある。
        if (!RegisterPointerInputTarget(hwnd, PT_TOUCHPAD)) {
            printf("[warn] RegisterPointerInputTarget(PT_TOUCHPAD) failed: %lu\n", GetLastError());
        } else {
            printf("[info] RegisterPointerInputTarget(PT_TOUCHPAD) OK.\n");
        }

        if (!InitDirectManipulation(hwnd)) {
            printf("DirectManipulation init failed. Exiting.\n");
            PostQuitMessage(1);
            return -1;
        }
        SetTimer(hwnd, DM_TIMER_ID, DM_TIMER_MS, nullptr);
        return 0;
    }

    // DirectManipulationにポインタ入力を渡す。これが無いとジェスチャ認識が動かない。
    case WM_POINTERDOWN: {
        UINT32 pointerId = GET_POINTERID_WPARAM(wParam);
        POINTER_INFO info{};
        GetPointerInfo(pointerId, &info);
        const char* typeName =
            (info.pointerType == PT_TOUCHPAD) ? "TOUCHPAD" :
            (info.pointerType == PT_TOUCH)    ? "TOUCH" :
            (info.pointerType == PT_PEN)      ? "PEN" :
            (info.pointerType == PT_MOUSE)    ? "MOUSE" : "OTHER";
        printf("\n[WM_POINTERDOWN] id=%u type=%s\n", pointerId, typeName);

        if (g_viewport) {
            HRESULT hr = g_viewport->SetContact(pointerId);
            printf("[SetContact] id=%u hr=0x%08X\n", pointerId, hr);
        }
        return 0;
    }

    // 前回はここが無く、タップ(クリック相当)しない限り一切ログが出なかった。
    // 「触れて動かす」動作自体はDOWNを伴わずUPDATEだけが飛んでくることが多いため
    // (ボタンを押さないマウス移動と同じ扱い)、ここでも記録する。
    case WM_POINTERUPDATE:
    case WM_POINTERUP: {
        UINT32 pointerId = GET_POINTERID_WPARAM(wParam);
        POINTER_INFO info{};
        if (GetPointerInfo(pointerId, &info)) {
            const char* typeName =
                (info.pointerType == PT_TOUCHPAD) ? "TOUCHPAD" :
                (info.pointerType == PT_TOUCH)    ? "TOUCH" :
                (info.pointerType == PT_PEN)      ? "PEN" :
                (info.pointerType == PT_MOUSE)    ? "MOUSE" : "OTHER";
            // 動きの度に大量に出るとログが埋まるので type が TOUCH/TOUCHPAD の時だけ強調表示。
            if (info.pointerType == PT_TOUCH || info.pointerType == PT_TOUCHPAD) {
                printf("\n[%s] id=%u type=%s pos=(%ld,%ld)\n",
                       (msg == WM_POINTERUP) ? "WM_POINTERUP" : "WM_POINTERUPDATE",
                       pointerId, typeName, info.ptPixelLocation.x, info.ptPixelLocation.y);
            } else {
                // MOUSE型は大量に出るので簡易表示のみ(存在確認用)
                printf(".");
                fflush(stdout);
            }
        }
        return 0;
    }

    case WM_POINTERUP: {
        UINT32 pointerId = GET_POINTERID_WPARAM(wParam);
        printf("\n[WM_POINTERUP] id=%u\n", pointerId);
        return 0;
    }

    case WM_SIZE: {
        if (g_viewport) {
            RECT rc; GetClientRect(hwnd, &rc);
            g_viewport->SetViewportRect(&rc);
        }
        return 0;
    }

    case WM_TIMER:
        if (wParam == DM_TIMER_ID && g_updateManager) {
            g_updateManager->Update(nullptr);
            PrintTransform();

            // 状態遷移イベントを取りこぼしていないかの保険として、
            // 数秒おきにポーリングでも現在状態を出す。
            static int tick = 0;
            if (g_viewport && (++tick % 120) == 0) { // 約2秒毎
                DIRECTMANIPULATION_STATUS status;
                if (SUCCEEDED(g_viewport->GetStatus(&status))) {
                    printf("\n[poll] current viewport status = %d\n", (int)status);
                }
            }
            return 0;
        }
        break;

    case WM_DESTROY:
        KillTimer(hwnd, DM_TIMER_ID);
        ShutdownDirectManipulation(hwnd);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int show) {
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetupDebugConsole();

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"DMProbe";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName,
        L"DirectManipulation Probe - watch the console window",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,
        nullptr, nullptr, hInst, nullptr);
    ShowWindow(hwnd, show);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (SUCCEEDED(hrCo)) CoUninitialize();
    return 0;
}