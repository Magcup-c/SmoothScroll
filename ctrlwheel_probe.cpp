// ctrlwheel_probe.cpp - Precision Touchpad のピンチ動作が
// 「Ctrl + マウスホイール」に変換されて届くかどうかを確認する検証プログラム。
//
// 背景:
//   DirectManipulation / Interaction Context / WM_GESTURE という
//   3つの公式APIすべてで、このマシンでは2本指の生ジェスチャデータを
//   取得できなかった。一方でEdge/Photosでは実際にピンチズームが機能している。
//   これは、アプリがDirectManipulation等に「対応していない」と判断された場合、
//   OSがピンチ動作を互換シグナルとして Ctrl+マウスホイール に変換して
//   渡す仕組みがあるためと推測される。この経路なら生ジェスチャの
//   直接受信に依存しないため、今まで試した方式とは独立して機能する可能性が高い。
//
// ビルド (Developer Command Prompt):
//   cl /EHsc /std:c++17 ctrlwheel_probe.cpp user32.lib
//
// 使い方:
//   ウィンドウにフォーカスした状態でタッチパッドを2本指でピンチすると、
//   Ctrlキーを押していなくても Ctrl+ホイール相当のイベントとして
//   検出できるかどうかがコンソールに出力される。

#include <Windows.h>
#include <windowsx.h>
#include <cstdio>

namespace {

void SetupDebugConsole() {
    AllocConsole();
    FILE* fp = nullptr;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);
    SetConsoleTitleW(L"SmoothScroll Ctrl+Wheel Probe");
    printf("=== Ctrl+Wheel Probe: console ready ===\n");
    printf("Pinch with 2 fingers on your precision touchpad (no need to press Ctrl yourself).\n\n");
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_MOUSEWHEEL: {
        short delta = GET_WHEEL_DELTA_WPARAM(wParam);
        WORD keys = GET_KEYSTATE_WPARAM(wParam); // MK_CONTROL等が立っているか
        bool ctrlDown = (keys & MK_CONTROL) != 0;
        // GetKeyState でも二重チェック(念のため)
        bool ctrlDownReal = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

        printf("\n[WM_MOUSEWHEEL] delta=%d ctrl(from wParam)=%s ctrl(GetKeyState)=%s\n",
               delta, ctrlDown ? "YES" : "no", ctrlDownReal ? "YES" : "no");

        if (ctrlDown || ctrlDownReal) {
            printf("  => This looks like a PINCH-ZOOM compatibility signal!\n");
        }
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
    wc.lpszClassName = L"CtrlWheelProbe";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName,
        L"Ctrl+Wheel Probe - watch the console window",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,
        nullptr, nullptr, hInst, nullptr);
    ShowWindow(hwnd, show);
    SetForegroundWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}