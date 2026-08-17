// demo/main.cpp - SmoothScroll DLL の最小利用例
// 白いウィンドウに縦長のダミーコンテンツを描画し、タッチパッドで慣性スクロールさせる。
#include <Windows.h>
#include <windowsx.h>
#include "../SmoothScroll/SmoothScroll.h"

#pragma comment(lib, "../x64/Release/SmoothScroll.lib") // ビルド配置に合わせて変更

namespace {
    float g_scrollY = 0;       // 現在のスクロール位置 [px]
    float g_maxScroll = 2000;  // コンテンツ長に応じた上限
    HANDLE g_ss = nullptr;
}

// DLLの物理スレッドから呼ばれる → UIスレッドへ転送
void CALLBACK OnScroll(float dx, float dy, void* user) {
    HWND hwnd = (HWND)user;
    // bit詰めして送る（floatをそのまま渡せないため）
    int packed_x = *(int*)&dx;
    int packed_y = *(int*)&dy;
    PostMessageW(hwnd, WM_APP + 1, (WPARAM)packed_x, (LPARAM)packed_y);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        SS_Config cfg; ss_default_config(&cfg);
        g_ss = ss_attach(hwnd, &cfg, OnScroll, hwnd);
        return 0;
    }
    case WM_APP + 1: { // スムーズ化済みデルタ受信
        float dx = *(float*)&wParam;
        float dy = *(float*)&lParam;
        g_scrollY += dy;
        if (g_scrollY < 0) g_scrollY = 0;                 // 上端ラバーバンドの代わりにクランプ
        if (g_scrollY > g_maxScroll) g_scrollY = g_maxScroll;
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, (HBRUSH)(COLOR_WINDOW + 1));

        SetBkMode(hdc, TRANSPARENT);
        for (int i = 0; i < 80; i++) {
            int y = 20 + i * 40 - (int)g_scrollY;
            if (y < -30 || y > rc.bottom + 30) continue;
            RECT line = { 40, y, rc.right - 40, y + 24 };
            HBRUSH br = CreateSolidBrush(RGB(200, 210, 230));
            FillRect(hdc, &line, br);
            DeleteObject(br);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        if (g_ss) { ss_detach(g_ss); g_ss = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int show) {
    // タッチパッドのポインタ入力を有効化（Windows 10 1607+）
    EnableMouseInPointer(FALSE);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"SSDemo";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"SmoothScroll Demo",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 640, 800,
        nullptr, nullptr, hInst, nullptr);
    ShowWindow(hwnd, show);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
