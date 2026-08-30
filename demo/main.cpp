// demo/main.cpp - SmoothScroll DLL の利用例
// - 縦横グリッドで斜めスクロールを可視化
// - 境界を超えた瞬間(エッジ検出)に慣性を打ち切ってバネへ引き継ぐことで
//   「慣性の残り香」と「バネ」が同時に位置を取り合うことによる
//   カクつき・巻き戻りを防止
// - ダブルバッファリングでチカチカを解消
// - Ctrl+ホイール(Precision Touchpadのピンチ互換シグナル)によるズームに対応。
//   カーソル位置を中心に拡大縮小する。
#include <Windows.h>
#include <windowsx.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include "../SmoothScroll/SmoothScroll.h"

namespace {
    constexpr int GRID_COLS   = 3;
    constexpr int GRID_ROWS   = 20;
    constexpr int CARD_W      = 260;
    constexpr int CARD_H      = 88;
    constexpr int GAP         = 20;
    constexpr int MARGIN      = 40;
    constexpr int TOP_PAD     = 24;

    float g_scrollX = 0, g_scrollY = 0;
    float g_maxScrollX = 0, g_maxScrollY = 0;
    bool  g_overscrollX = false, g_overscrollY = false;

    bool g_wasOverX = false, g_wasOverY = false;

    float g_zoomScale = 1.0f;
    constexpr float ZOOM_MIN = 0.3f;
    constexpr float ZOOM_MAX = 3.0f;

    HANDLE g_ss = nullptr;
    float g_springVelX = 0.0f, g_springVelY = 0.0f;

    // 慣性→バネの速度引き継ぎ(ハンドオフ)を「オーバースクロール1回の区間につき
    // 一度だけ」行うためのフラグ。
    // これが無いと、overNow(オーバースクロール中) && !ss_is_touching(指離れ)が
    // trueである間、WM_TIMERが呼ばれるたびに毎回 ss_get_velocity()で速度を
    // 取得し直してしまう。しかし最初のハンドオフで ss_cancel_momentum() 済みの
    // ため、2回目以降に取得できる速度は常に0。その0でg_springVelX/Yを
    // 毎フレーム上書きしてしまい、バネが「16ms分だけ加速→即0にリセット」を
    // 延々繰り返す羽目になり、これが端での「カクつき/ジャンプ」の主因だった。
    bool g_springHandoffDone = false;

    constexpr UINT_PTR SPRING_TIMER_ID = 1;
    constexpr UINT     SPRING_TIMER_MS = 16;
    constexpr float    RUBBERBAND_COEFF = 0.55f;
    constexpr float    SPRING_STIFFNESS = 220.0f;
    constexpr float    SPRING_DAMPING   = 22.0f;
    constexpr float    SPRING_POS_EPS   = 0.4f;
    constexpr float    SPRING_VEL_EPS   = 4.0f;
    constexpr float    MAX_OVERSCROLL   = 60.0f;

    HFONT g_font = nullptr;

    HBITMAP g_memBitmap = nullptr;
    HDC     g_memDC = nullptr;
    int     g_memW = 0, g_memH = 0;

    void EnsureBackBuffer(HDC refDC, int w, int h) {
        if (g_memDC && g_memW == w && g_memH == h) return;
        if (g_memBitmap) { DeleteObject(g_memBitmap); g_memBitmap = nullptr; }
        if (g_memDC) { DeleteDC(g_memDC); g_memDC = nullptr; }
        if (w <= 0 || h <= 0) return;
        g_memDC = CreateCompatibleDC(refDC);
        g_memBitmap = CreateCompatibleBitmap(refDC, w, h);
        SelectObject(g_memDC, g_memBitmap);
        g_memW = w; g_memH = h;
    }

    void RecalcMaxScroll(HWND hwnd) {
        RECT rc; GetClientRect(hwnd, &rc);
        int contentW = MARGIN * 2 + GRID_COLS * CARD_W + (GRID_COLS - 1) * GAP;
        int contentH = TOP_PAD * 2 + GRID_ROWS * CARD_H + (GRID_ROWS - 1) * GAP;
        float viewW = (float)(rc.right - rc.left) / g_zoomScale;
        float viewH = (float)(rc.bottom - rc.top) / g_zoomScale;
        g_maxScrollX = max(0.0f, (float)contentW - viewW);
        g_maxScrollY = max(0.0f, (float)contentH - viewH);
    }

    inline float ContentToScreenX(float contentX) { return (contentX - g_scrollX) * g_zoomScale; }
    inline float ContentToScreenY(float contentY) { return (contentY - g_scrollY) * g_zoomScale; }

    inline int RoundPx(float v) { return (int)std::lround(v); }
}

void CALLBACK OnScroll(float dx, float dy, void* user) {
    HWND hwnd = (HWND)user;
    int32_t packed_x, packed_y;
    std::memcpy(&packed_x, &dx, sizeof(float));
    std::memcpy(&packed_y, &dy, sizeof(float));
    PostMessageW(hwnd, WM_APP + 1, (WPARAM)(intptr_t)packed_x, (LPARAM)(intptr_t)packed_y);
}

void CALLBACK OnZoom(float scale_factor, float anchor_x, float anchor_y, void* user) {
    HWND hwnd = (HWND)user;

    float* payload = new (std::nothrow) float[3];
    if (!payload) return;
    payload[0] = scale_factor;
    payload[1] = anchor_x;
    payload[2] = anchor_y;
    PostMessageW(hwnd, WM_APP + 2, 0, (LPARAM)payload);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_font = CreateFontW(20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Yu Gothic UI");

        SS_Config cfg; ss_default_config(&cfg);
        cfg.enable_zoom = TRUE;
        cfg.pan_subpixel = TRUE;
        cfg.invert_vertical = TRUE;

        g_ss = ss_attach(hwnd, &cfg, OnScroll, OnZoom, hwnd);

        RecalcMaxScroll(hwnd);
        SetTimer(hwnd, SPRING_TIMER_ID, SPRING_TIMER_MS, nullptr);
        return 0;
    }

    case WM_SIZE:
        RecalcMaxScroll(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_APP + 1: {
        int32_t packed_x = (int32_t)(intptr_t)wParam;
        int32_t packed_y = (int32_t)(intptr_t)lParam;
        float dx, dy;
        std::memcpy(&dx, &packed_x, sizeof(float));
        std::memcpy(&dy, &packed_y, sizeof(float));

        float contentDx = dx / g_zoomScale;
        float contentDy = dy / g_zoomScale;

        float appliedX = ss_rubberband_delta(contentDx, g_scrollX, 0.0f, g_maxScrollX, RUBBERBAND_COEFF);
        float appliedY = ss_rubberband_delta(contentDy, g_scrollY, 0.0f, g_maxScrollY, RUBBERBAND_COEFF);
        g_scrollX += appliedX;
        g_scrollY += appliedY;

        g_scrollX = max(-MAX_OVERSCROLL, min(g_maxScrollX + MAX_OVERSCROLL, g_scrollX));
        g_scrollY = max(-MAX_OVERSCROLL, min(g_maxScrollY + MAX_OVERSCROLL, g_scrollY));

        g_springVelX = g_springVelY = 0.0f;

        g_overscrollX = (g_scrollX < 0.0f || g_scrollX > g_maxScrollX);
        g_overscrollY = (g_scrollY < 0.0f || g_scrollY > g_maxScrollY);

        if (g_overscrollX || g_overscrollY) {
            ss_stop_inertia(g_ss);
        }
        g_wasOverX = g_overscrollX;
        g_wasOverY = g_overscrollY;

        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_APP + 2: {
        float* payload = (float*)lParam;
        float scaleFactor = payload[0];
        float anchorX = payload[1];
        float anchorY = payload[2];
        delete[] payload;

        float contentAnchorX = anchorX / g_zoomScale + g_scrollX;
        float contentAnchorY = anchorY / g_zoomScale + g_scrollY;

        float newScale = g_zoomScale * scaleFactor;
        newScale = max(ZOOM_MIN, min(ZOOM_MAX, newScale));

        g_zoomScale = newScale;
        g_scrollX = contentAnchorX - anchorX / g_zoomScale;
        g_scrollY = contentAnchorY - anchorY / g_zoomScale;

        RecalcMaxScroll(hwnd);
        g_scrollX = max(0.0f, min(g_maxScrollX, g_scrollX));
        g_scrollY = max(0.0f, min(g_maxScrollY, g_scrollY));

        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_TIMER: {
        if (wParam == SPRING_TIMER_ID) {
            bool overNow = (g_scrollX < 0.0f || g_scrollX > g_maxScrollX ||
                            g_scrollY < 0.0f || g_scrollY > g_maxScrollY);

            if (overNow && !ss_is_touching(g_ss)) {
                if (!g_springHandoffDone) {
                    float evx = 0.0f, evy = 0.0f;
                    ss_get_velocity(g_ss, &evx, &evy);
                    ss_cancel_momentum(g_ss);
                    g_springVelX = evx / g_zoomScale;
                    g_springVelY = evy / g_zoomScale;
                    g_springHandoffDone = true;
                }
            } else {
                g_springHandoffDone = false;
            }

            if (!ss_is_scrolling(g_ss)) {
                bool moved = false;
                const float dt = SPRING_TIMER_MS / 1000.0f;

                if (g_scrollX < 0.0f || g_scrollX > g_maxScrollX) {
                    float target = (g_scrollX < 0.0f) ? 0.0f : g_maxScrollX;
                    g_scrollX = ss_spring_step(g_scrollX, target, &g_springVelX, dt,
                                                SPRING_STIFFNESS, SPRING_DAMPING);
                    if (ss_spring_settled(g_scrollX, target, g_springVelX, SPRING_POS_EPS, SPRING_VEL_EPS)) {
                        g_scrollX = target; g_springVelX = 0.0f;
                    }
                    moved = true;
                }
                if (g_scrollY < 0.0f || g_scrollY > g_maxScrollY) {
                    float target = (g_scrollY < 0.0f) ? 0.0f : g_maxScrollY;
                    g_scrollY = ss_spring_step(g_scrollY, target, &g_springVelY, dt,
                                                SPRING_STIFFNESS, SPRING_DAMPING);
                    if (ss_spring_settled(g_scrollY, target, g_springVelY, SPRING_POS_EPS, SPRING_VEL_EPS)) {
                        g_scrollY = target; g_springVelY = 0.0f;
                    }
                    moved = true;
                }

                g_overscrollX = (g_scrollX < 0.0f || g_scrollX > g_maxScrollX);
                g_overscrollY = (g_scrollY < 0.0f || g_scrollY > g_maxScrollY);
                g_wasOverX = g_overscrollX;
                g_wasOverY = g_overscrollY;

                if (moved) InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        break;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        int w = rc.right - rc.left, h = rc.bottom - rc.top;

        EnsureBackBuffer(hdc, w, h);
        HDC mdc = g_memDC ? g_memDC : hdc;

        HBRUSH bg = CreateSolidBrush(RGB(246, 247, 250));
        FillRect(mdc, &rc, bg);
        DeleteObject(bg);

        SetBkMode(mdc, TRANSPARENT);
        HFONT oldFont = (HFONT)SelectObject(mdc, g_font);

        for (int r = 0; r < GRID_ROWS; r++) {
            float contentY = (float)(TOP_PAD + r * (CARD_H + GAP));
            int y = RoundPx(ContentToScreenY(contentY));
            int cardH = RoundPx(CARD_H * g_zoomScale);
            if (y + cardH < 0 || y > h) continue;

            for (int c = 0; c < GRID_COLS; c++) {
                float contentX = (float)(MARGIN + c * (CARD_W + GAP));
                int x = RoundPx(ContentToScreenX(contentX));
                int cardW = RoundPx(CARD_W * g_zoomScale);
                if (x + cardW < 0 || x > w) continue;

                RECT card = { x, y, x + cardW, y + cardH };

                COLORREF base = ((r + c) % 2 == 0) ? RGB(255, 255, 255) : RGB(233, 238, 247);
                HBRUSH cardBrush = CreateSolidBrush(base);
                HPEN cardPen = CreatePen(PS_SOLID, 1, RGB(210, 216, 228));
                HGDIOBJ oldBrush = SelectObject(mdc, cardBrush);
                HGDIOBJ oldPen = SelectObject(mdc, cardPen);
                int radius = max(2, RoundPx(14 * g_zoomScale));
                RoundRect(mdc, card.left, card.top, card.right, card.bottom, radius, radius);
                SelectObject(mdc, oldBrush);
                SelectObject(mdc, oldPen);
                DeleteObject(cardBrush);
                DeleteObject(cardPen);

                wchar_t label[32];
                swprintf_s(label, L"R%02d C%d", r + 1, c + 1);
                RECT text = { card.left + RoundPx(20 * g_zoomScale), card.top,
                              card.right - RoundPx(20 * g_zoomScale), card.bottom };
                SetTextColor(mdc, RGB(60, 66, 82));
                DrawTextW(mdc, label, -1, &text, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
            }
        }

        SelectObject(mdc, oldFont);

        {
            int trackX = w - 10;
            RECT track = { trackX, 8, trackX + 4, h - 8 };
            HBRUSH trackBrush = CreateSolidBrush(RGB(224, 227, 234));
            FillRect(mdc, &track, trackBrush);
            DeleteObject(trackBrush);

            float contentH = (float)(TOP_PAD * 2 + GRID_ROWS * CARD_H + (GRID_ROWS - 1) * GAP);
            float trackH = (float)(track.bottom - track.top);
            float thumbH = max(24.0f, trackH * ((float)h / (contentH * g_zoomScale)));
            float ratio = (g_maxScrollY > 0.0f) ? (g_scrollY / g_maxScrollY) : 0.0f;
            ratio = max(0.0f, min(1.0f, ratio));
            float thumbY = track.top + (trackH - thumbH) * ratio;

            RECT thumb = { trackX, RoundPx(thumbY), trackX + 4, RoundPx(thumbY + thumbH) };
            HBRUSH thumbBrush = CreateSolidBrush(g_overscrollY ? RGB(230, 120, 90) : RGB(120, 140, 220));
            FillRect(mdc, &thumb, thumbBrush);
            DeleteObject(thumbBrush);
        }

        {
            int trackY = h - 10;
            RECT track = { 8, trackY, w - 8, trackY + 4 };
            HBRUSH trackBrush = CreateSolidBrush(RGB(224, 227, 234));
            FillRect(mdc, &track, trackBrush);
            DeleteObject(trackBrush);

            float contentW = (float)(MARGIN * 2 + GRID_COLS * CARD_W + (GRID_COLS - 1) * GAP);
            float trackW = (float)(track.right - track.left);
            float thumbW = max(24.0f, trackW * ((float)w / (contentW * g_zoomScale)));
            float ratio = (g_maxScrollX > 0.0f) ? (g_scrollX / g_maxScrollX) : 0.0f;
            ratio = max(0.0f, min(1.0f, ratio));
            float thumbX = track.left + (trackW - thumbW) * ratio;

            RECT thumb = { RoundPx(thumbX), trackY, RoundPx(thumbX + thumbW), trackY + 4 };
            HBRUSH thumbBrush = CreateSolidBrush(g_overscrollX ? RGB(230, 120, 90) : RGB(120, 140, 220));
            FillRect(mdc, &thumb, thumbBrush);
            DeleteObject(thumbBrush);
        }

        {
            wchar_t zoomLabel[32];
            swprintf_s(zoomLabel, L"Zoom: %.0f%%", g_zoomScale * 100.0f);
            RECT zr = { 12, 8, 160, 32 };
            SetTextColor(mdc, RGB(90, 96, 112));
            DrawTextW(mdc, zoomLabel, -1, &zr, DT_SINGLELINE | DT_LEFT | DT_TOP);
        }

        if (g_memDC) {
            BitBlt(hdc, 0, 0, w, h, g_memDC, 0, 0, SRCCOPY);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, SPRING_TIMER_ID);
        if (g_ss) { ss_detach(g_ss); g_ss = nullptr; }
        if (g_memBitmap) { DeleteObject(g_memBitmap); g_memBitmap = nullptr; }
        if (g_memDC) { DeleteDC(g_memDC); g_memDC = nullptr; }
        if (g_font) { DeleteObject(g_font); g_font = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int show) {
    EnableMouseInPointer(TRUE);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"SSDemo";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"SmoothScroll Demo (Diagonal + Overscroll + Zoom)",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 760, 800,
        nullptr, nullptr, hInst, nullptr);
    ShowWindow(hwnd, show);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}