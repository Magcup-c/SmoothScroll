// ctrlwheel_probe3.cpp - EnableMouseInPointer(TRUE)環境下でのCtrl+ホイール検出比較。
//
// 背景:
//   ctrlwheel_probe2.cpp (EnableMouseInPointerを呼ばない) では、
//   Ctrlキーを押さなくてもピンチ動作でCtrl+ホイール相当のシグナルが
//   16回連続で安定して検出できた。
//   一方、SmoothScrollデモ(EnableMouseInPointer(TRUE)呼び出し済み)では
//   同じ検出が7回程度で止まっていた。
//   この違いが「EnableMouseInPointerの有無」によるものかを確認するため、
//   EnableMouseInPointer(TRUE)を呼んだ状態で
//   (a) classic WM_MOUSEWHEEL が引き続き届くか
//   (b) WM_POINTERWHEEL 自体にCtrl修飾が乗っているか
//   の両方を同時にログする。
//
// ビルド (Developer Command Prompt):
//   cl /EHsc /std:c++17 ctrlwheel_probe3.cpp user32.lib
//
// 使い方:
//   ウィンドウにフォーカスした状態で、Ctrlキーを押さずに
//   タッチパッドを2本指でピンチしてみる。

#include <Windows.h>
#include <windowsx.h>
#include <cstdio>

namespace {

void SetupDebugConsole() {
    AllocConsole();
    FILE* fp = nullptr;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);
    SetConsoleTitleW(L"Ctrl Detection under EnableMouseInPointer");
    printf("=== Probe ready (EnableMouseInPointer=TRUE) ===\n");
    printf("Do NOT press Ctrl yourself. Pinch with 2 fingers.\n\n");
}

int g_wheelCount = 0;
int g_pointerWheelCount = 0;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    // classic マウスホイール。EnableMouseInPointer(TRUE)環境下でも
    // 本当にこれが届き続けるか?というのが今回の核心の確認事項。
    case WM_MOUSEWHEEL: {
        ++g_wheelCount;
        short delta = GET_WHEEL_DELTA_WPARAM(wParam);
        bool ctrlAsync = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        printf("[WHEEL  #%04d] delta=%+5d ctrl(async)=%s\n",
               g_wheelCount, delta, ctrlAsync ? "YES" : "no");
        return 0;
    }

    // WM_POINTERWHEEL自体にもCtrl修飾フラグ(wParamの下位ワード)が
    // 乗っているかどうかを確認する。
    case WM_POINTERWHEEL:
    case WM_POINTERHWHEEL: {
        ++g_pointerWheelCount;
        short delta = GET_WHEEL_DELTA_WPARAM(wParam);
        WORD keys = GET_KEYSTATE_WPARAM(wParam);
        bool ctrlFromWparam = (keys & MK_CONTROL) != 0;
        bool ctrlAsync = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        printf("[POINTERWHEEL #%04d] delta=%+5d ctrl(wParam)=%s ctrl(async)=%s%s\n",
               g_pointerWheelCount, delta,
               ctrlFromWparam ? "YES" : "no ",
               ctrlAsync ? "YES" : "no ",
               (msg == WM_POINTERHWHEEL) ? " [horizontal]" : "");
        return 0; // 消費(通常デモと同じ挙動に合わせる)
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

    // デモと同じ条件を再現するため、ここで有効化する。
    BOOL r = EnableMouseInPointer(TRUE);
    printf("EnableMouseInPointer(TRUE) = %d\n\n", r);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"CtrlDetectProbe3";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName,
        L"Ctrl Detection (EnableMouseInPointer) - watch console",
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