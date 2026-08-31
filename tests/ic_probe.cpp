// ic_probe.cpp - Interaction Context API (interactioncontext.h) を使って
// Precision Touchpad のパン/ピンチ(スケール)/回転をコンソールに出力する検証プログラム。
//
// 背景:
//   RegisterPointerInputTarget は「他プロセスへの入力リダイレクト」用の
//   UI Access専用APIであり(ERROR_ACCESS_DENIEDは仕様通り)、今回の用途には無関係だった。
//   DirectManipulationは経路としては正しいが、既定では
//   OS標準のジェスチャーエンジンが2本指ジェスチャを勝手に横取りして
//   別物(WM_POINTERWHEELやCtrl+ホイール)に変換してしまうため、
//   「自分で生のポインタを処理する」と明示的に申告する必要がある。
//   その申告 + 実際のジェスチャ解析(パン/スケール/回転 + 慣性)をまとめて
//   面倒見てくれるのが Interaction Context API。
//
// ビルド (Developer Command Prompt):
//   cl /EHsc /std:c++17 ic_probe.cpp user32.lib Ninput.lib
//
// 使い方:
//   ウィンドウにフォーカスした状態でタッチパッドを2本指で
//   パン/ピンチ/回転すると、コンソールに値がリアルタイム出力される。

#ifndef NTDDI_VERSION
#define NTDDI_VERSION NTDDI_WIN10
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT _WIN32_WINNT_WIN10
#endif

#include <Windows.h>
#include <windowsx.h>
#include <interactioncontext.h>
#include <cstdio>
#include <cmath>

#pragma comment(lib, "Ninput.lib")

namespace {

void SetupDebugConsole() {
    AllocConsole();
    FILE* fp = nullptr;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);
    SetConsoleTitleW(L"SmoothScroll InteractionContext Probe");
    printf("=== InteractionContext Probe: console ready ===\n");
    printf("Pan / pinch / rotate with 2 fingers on your precision touchpad.\n\n");
}

HINTERACTIONCONTEXT g_ic = nullptr;
constexpr UINT_PTR INERTIA_TIMER_ID = 7;
constexpr UINT     INERTIA_TIMER_MS = 16; // 約60Hz

const char* InteractionIdName(INTERACTION_ID id) {
    switch (id) {
        case INTERACTION_ID_MANIPULATION:  return "MANIPULATION";
        case INTERACTION_ID_TAP:           return "TAP";
        case INTERACTION_ID_SECONDARY_TAP: return "SECONDARY_TAP";
        case INTERACTION_ID_HOLD:          return "HOLD";
        case INTERACTION_ID_CROSS_SLIDE:   return "CROSS_SLIDE";
        default: return "OTHER";
    }
}

// Interaction Context からの出力コールバック。
// パン(translationX/Y) / ピンチ(scale) / 回転(rotation) が
// このフレームでの差分値(delta)と開始からの累積値(cumulative)の
// 2種類でまとめて渡ってくる。
//
// 注意: 実際のヘッダ定義では
//   INTERACTION_CONTEXT_OUTPUT::interactionId (INTERACTION_ID型, "interactionType"ではない)
//   INTERACTION_ARGUMENTS_MANIPULATION::delta / cumulative (MANIPULATION_TRANSFORM型の入れ子)
// という構造になっている。以前のコードは推測で書いた誤った構造体名を使っていたため
// 実際のヘッダ(interactioncontext.h)の定義に合わせて修正した。
void CALLBACK InteractionOutputCallback(void* /*clientData*/, const INTERACTION_CONTEXT_OUTPUT* output) {
    if (!output) return;

    static int callbackCount = 0;
    ++callbackCount;

    if (output->interactionId != INTERACTION_ID_MANIPULATION) {
        printf("\n[interaction #%d] id=%s\n", callbackCount, InteractionIdName(output->interactionId));
        return;
    }

    const auto& m = output->arguments.manipulation;
    bool isInertia = (output->interactionFlags & INTERACTION_FLAG_INERTIA) != 0;
    bool isBegin   = (output->interactionFlags & INTERACTION_FLAG_BEGIN) != 0;
    bool isEnd     = (output->interactionFlags & INTERACTION_FLAG_END) != 0;

    float rotation_deg = m.delta.rotation * 180.0f / 3.14159265f;
    float cum_rotation_deg = m.cumulative.rotation * 180.0f / 3.14159265f;

    if (isBegin) printf("\n[begin]\n");

    printf("\r[manip%s] dx=%+7.2f dy=%+7.2f scale=%.4f rot=%+6.2fdeg | cum: dx=%+7.1f dy=%+7.1f scale=%.4f rot=%+7.1fdeg      ",
           isInertia ? "*inertia*" : "",
           m.delta.translationX, m.delta.translationY, m.delta.scale, rotation_deg,
           m.cumulative.translationX, m.cumulative.translationY, m.cumulative.scale, cum_rotation_deg);
    fflush(stdout);

    if (isEnd) printf("\n[end]\n");
}

bool InitInteractionContext(HWND /*hwnd*/) {
    HRESULT hr = CreateInteractionContext(&g_ic);
    if (FAILED(hr)) { printf("CreateInteractionContext failed: 0x%08X\n", hr); return false; }

    // パン(X/Y) + ピンチ(スケール) + 回転 + それぞれの慣性を有効化。
    INTERACTION_CONTEXT_CONFIGURATION config[] = {
        { INTERACTION_ID_MANIPULATION,
          INTERACTION_CONFIGURATION_FLAG_MANIPULATION_TRANSLATION_X |
          INTERACTION_CONFIGURATION_FLAG_MANIPULATION_TRANSLATION_Y |
          INTERACTION_CONFIGURATION_FLAG_MANIPULATION_TRANSLATION_INERTIA |
          INTERACTION_CONFIGURATION_FLAG_MANIPULATION_SCALING |
          INTERACTION_CONFIGURATION_FLAG_MANIPULATION_SCALING_INERTIA |
          INTERACTION_CONFIGURATION_FLAG_MANIPULATION_ROTATION |
          INTERACTION_CONFIGURATION_FLAG_MANIPULATION_ROTATION_INERTIA }
    };
    hr = SetInteractionConfigurationInteractionContext(g_ic, ARRAYSIZE(config), config);
    if (FAILED(hr)) { printf("SetInteractionConfigurationInteractionContext failed: 0x%08X\n", hr); return false; }

    // 座標/移動量の単位を画面ピクセルに固定する(既定値のはずだが明示しておく)。
    // INTERACTION_CONTEXT_PROPERTY_MEASUREMENT_UNITS_SCREEN のような名前付き定数は
    // 実際のヘッダには存在せず、ドキュメント上の説明値に過ぎない。
    // 実際には SetPropertyInteractionContext に生の数値(0=HIMETRIC, 1=SCREEN)を
    // 直接渡す仕様になっている。
    constexpr UINT32 MEASUREMENT_UNITS_SCREEN = 1;
    hr = SetPropertyInteractionContext(g_ic, INTERACTION_CONTEXT_PROPERTY_MEASUREMENT_UNITS,
                                        MEASUREMENT_UNITS_SCREEN);
    if (FAILED(hr)) { printf("[warn] SetPropertyInteractionContext(MEASUREMENT_UNITS) failed: 0x%08X\n", hr); }

    // フィルタをOFFにする = 「特定のpointerIdだけ処理する」制限を外し、
    // このウィンドウに来た生ポインタ入力をすべてInteraction Contextに渡す。
    // (これが今回の本題: OS標準ジェスチャーエンジンによる横取り・変換を止め、
    //  自前でジェスチャ解析を受け取れるようにする申告)
    // こちらも同様に生の数値(0=OFF, 1=ON)を渡す。
    constexpr UINT32 FILTER_POINTERS_OFF = 0;
    hr = SetPropertyInteractionContext(g_ic, INTERACTION_CONTEXT_PROPERTY_FILTER_POINTERS, FILTER_POINTERS_OFF);
    if (FAILED(hr)) { printf("SetPropertyInteractionContext failed: 0x%08X\n", hr); return false; }

    hr = RegisterOutputCallbackInteractionContext(g_ic, InteractionOutputCallback, nullptr);
    if (FAILED(hr)) { printf("RegisterOutputCallbackInteractionContext failed: 0x%08X\n", hr); return false; }

    printf("InteractionContext initialized OK.\n");
    return true;
}

void ShutdownInteractionContext() {
    if (g_ic) {
        DestroyInteractionContext(g_ic);
        g_ic = nullptr;
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        // WM_POINTERxxx を受け取るために必須。
        BOOL enableResult = EnableMouseInPointer(TRUE);
        printf("[info] EnableMouseInPointer(TRUE) result=%d (err=%lu if 0)\n",
               enableResult, enableResult ? 0 : GetLastError());
        printf("[info] IsMouseInPointerEnabled() = %d\n", IsMouseInPointerEnabled());

        if (!InitInteractionContext(hwnd)) {
            printf("InteractionContext init failed. Exiting.\n");
            PostQuitMessage(1);
            return -1;
        }
        SetTimer(hwnd, INERTIA_TIMER_ID, INERTIA_TIMER_MS, nullptr);
        return 0;
    }

    case WM_ACTIVATE:
        printf("\n[info] WM_ACTIVATE state=%d\n", LOWORD(wParam));
        return 0;

    case WM_SETFOCUS:
        printf("\n[info] WM_SETFOCUS\n");
        return 0;

    case WM_POINTERDOWN:
    case WM_POINTERUPDATE:
    case WM_POINTERUP: {
        if (!g_ic) break;

        UINT32 pointerId = GET_POINTERID_WPARAM(wParam);
        POINTER_INFO info{};
        if (GetPointerInfo(pointerId, &info)) {
            const char* msgName =
                (msg == WM_POINTERDOWN)   ? "DOWN" :
                (msg == WM_POINTERUPDATE) ? "UPDATE" : "UP";
            const char* typeName =
                (info.pointerType == PT_TOUCHPAD) ? "TOUCHPAD" :
                (info.pointerType == PT_TOUCH)    ? "TOUCH" :
                (info.pointerType == PT_PEN)      ? "PEN" :
                (info.pointerType == PT_MOUSE)    ? "MOUSE" : "OTHER";

            // 受信確認用の生ログ(問題切り分けのため成功/失敗問わず出す)
            printf("\n[msg=%s] id=%u type=%s pos=(%ld,%ld)\n",
                   msgName, pointerId, typeName, info.ptPixelLocation.x, info.ptPixelLocation.y);

            HRESULT hr = ProcessPointerFramesInteractionContext(g_ic, 1, 1, &info);
            printf("[ProcessPointerFrames] id=%u hr=0x%08X\n", pointerId, hr);
        } else {
            printf("\n[msg] GetPointerInfo failed for id=%u, err=%lu\n", pointerId, GetLastError());
        }
        return 0;
    }

    case WM_TIMER:
        if (wParam == INERTIA_TIMER_ID && g_ic) {
            // 指を離した後の慣性フェーズを進める。
            // 慣性が動いていない間は呼んでも実質何もしない軽い呼び出し。
            ProcessInertiaInteractionContext(g_ic);
            return 0;
        }
        break;

    case WM_DESTROY:
        KillTimer(hwnd, INERTIA_TIMER_ID);
        ShutdownInteractionContext();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int show) {
    SetupDebugConsole();

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"ICProbe";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName,
        L"InteractionContext Probe - watch the console window",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,
        nullptr, nullptr, hInst, nullptr);
    ShowWindow(hwnd, show);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}