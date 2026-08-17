// demo/main.cpp - SmoothScroll DLL の最小利用例（オーバースクロール検証版）
#include <Windows.h>
#include <windowsx.h>
#include <stdio.h>
#include "../SmoothScroll/SmoothScroll.h"

namespace {
    float g_scrollY = 0;       // 現在のスクロール位置 [px]（範囲外にも一時的にはみ出せる）
    float g_maxScroll = 2000;  // コンテンツ長に応じた上限
    HANDLE g_ss = nullptr;

    // オーバースクロール(ラバーバンド)関連の状態はホスト側で保持する
    float g_springVel = 0.0f;

    constexpr UINT_PTR SPRING_TIMER_ID = 1;
    constexpr UINT     SPRING_TIMER_MS = 16; // 約60Hz
    constexpr float    RUBBERBAND_COEFF = 0.55f;
    constexpr float    SPRING_STIFFNESS = 220.0f;
    constexpr float    SPRING_DAMPING   = 22.0f;
    constexpr float    SPRING_POS_EPS   = 0.4f;
    constexpr float    SPRING_VEL_EPS   = 4.0f;
}

// DLLの物理スレッドから呼ばれる → UIスレッドへ転送
void CALLBACK OnScroll(float dx, float dy, void* user) {
    HWND hwnd = (HWND)user;
    int packed_x = *(int*)&dx;
    int packed_y = *(int*)&dy;
    PostMessageW(hwnd, WM_APP + 1, (WPARAM)packed_x, (LPARAM)packed_y);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        SS_Config cfg; ss_default_config(&cfg);
        g_ss = ss_attach(hwnd, &cfg, OnScroll, hwnd);
        // バネ判定用タイマーは常時回しておく（負荷は軽微）
        SetTimer(hwnd, SPRING_TIMER_ID, SPRING_TIMER_MS, nullptr);
        return 0;
    }

    case WM_APP + 1: {
        static int s_count = 0;
        float dx = *(float*)&wParam;
        float dy = *(float*)&lParam;

        float applied = ss_rubberband_delta(dy, g_scrollY, 0.0f, g_maxScroll, RUBBERBAND_COEFF);
        g_scrollY += applied;
        g_springVel = 0.0f;

        // ★デバッグ: タイトルバーで受信状況を可視化
        wchar_t buf[128];
        swprintf_s(buf, L"count=%d dy=%.2f applied=%.2f scrollY=%.2f", ++s_count, dy, applied, g_scrollY);
        SetWindowTextW(hwnd, buf);

        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }

    case WM_TIMER: {
        if (wParam == SPRING_TIMER_ID) {
            // 慣性が完全に止まっていて、かつ範囲外にいる場合だけバネを動かす
            if (!ss_is_scrolling(g_ss) && (g_scrollY < 0.0f || g_scrollY > g_maxScroll)) {
                float target = (g_scrollY < 0.0f) ? 0.0f : g_maxScroll;
                g_scrollY = ss_spring_step(g_scrollY, target, &g_springVel,
                                            SPRING_TIMER_MS / 1000.0f,
                                            SPRING_STIFFNESS, SPRING_DAMPING);

                if (ss_spring_settled(g_scrollY, target, g_springVel, SPRING_POS_EPS, SPRING_VEL_EPS)) {
                    g_scrollY = target;
                    g_springVel = 0.0f;
                }
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        }
        break;
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
        KillTimer(hwnd, SPRING_TIMER_ID);
        if (g_ss) { ss_detach(g_ss); g_ss = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int show) {
    EnableMouseInPointer(FALSE);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"SSDemo";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"SmoothScroll Demo (Overscroll)",
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