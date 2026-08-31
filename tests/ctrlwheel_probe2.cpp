// ctrlwheel_probe2.cpp - Ctrlキー判定方法の比較検証プログラム。
//
// 背景:
//   ピンチ動作をCtrl+ホイールとして検出する際、wParamに埋め込まれた
//   キー修飾フラグ(MK_CONTROL)や GetKeyState(VK_CONTROL) では、
//   Ctrlキーを押しっぱなしにしていても数回で検出が途切れる現象が
//   実機で確認された。
//   これが「OS側の合成イベントの仕様上の癖」なのか、
//   「今回使っている判定関数(GetKeyState)固有の問題」なのかを
//   切り分けるため、3種類の判定方法を同時にログ出力して比較する。
//
// ビルド (Developer Command Prompt):
//   cl /EHsc /std:c++17 ctrlwheel_probe2.cpp user32.lib
//
// 使い方:
//   ウィンドウにフォーカスした状態で、Ctrlキーを押しっぱなしにして
//   タッチパッドを2本指でスクロール(またはピンチ)してみる。
//   3つの判定方法(wParam / GetKeyState / GetAsyncKeyState)の
//   結果が一致し続けるか、途中でずれるかを確認する。

#include <Windows.h>
#include <windowsx.h>
#include <cstdio>

namespace {

void SetupDebugConsole() {
    AllocConsole();
    FILE* fp = nullptr;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);
    SetConsoleTitleW(L"Ctrl Detection Comparison Probe");
    printf("=== Ctrl Detection Comparison Probe: console ready ===\n");
    printf("Hold Ctrl and scroll/pinch with 2 fingers on your precision touchpad.\n\n");
}

int g_eventCount = 0;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_MOUSEWHEEL: {
        ++g_eventCount;
        short delta = GET_WHEEL_DELTA_WPARAM(wParam);

        // 方法1: wParamに埋め込まれたキー修飾フラグ
        WORD keys = GET_KEYSTATE_WPARAM(wParam);
        bool ctrlFromWparam = (keys & MK_CONTROL) != 0;

        // 方法2: GetKeyState (最後にメッセージキューを処理した時点のキー状態)
        bool ctrlFromGetKeyState = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

        // 方法3: GetAsyncKeyState (呼び出した瞬間の物理的なキー状態を直接OSに問い合わせる)
        bool ctrlFromAsync = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

        printf("[#%04d] delta=%+5d | wParam=%s GetKeyState=%s GetAsyncKeyState=%s%s\n",
               g_eventCount, delta,
               ctrlFromWparam ? "YES" : "no ",
               ctrlFromGetKeyState ? "YES" : "no ",
               ctrlFromAsync ? "YES" : "no ",
               (ctrlFromWparam != ctrlFromGetKeyState || ctrlFromGetKeyState != ctrlFromAsync) ? "  <-- MISMATCH" : "");
        return 0;
    }

    case WM_DESTROY:
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
    wc.lpszClassName = L"CtrlDetectProbe";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName,
        L"Ctrl Detection Comparison - watch the console window",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,
        nullptr, nullptr, hInst, nullptr);
    ShowWindow(hwnd, show);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}