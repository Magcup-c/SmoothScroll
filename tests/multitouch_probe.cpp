// multitouch_probe.cpp - Precision Touchpad の生マルチタッチ座標を
// コンソールに出力する検証用の最小プログラム。
//
// 目的:
//   WM_POINTERWHEEL/HWHEEL (量子化された縦横別ホイール合成イベント) を経由せず、
//   WM_POINTERDOWN/UPDATE/UP で個々の指の生座標を直接取得できることを確認する。
//   ここで安定してpointer IDや座標が取れれば、
//   2本指の距離変化=ズーム、角度変化=回転、中点移動=パン
//   という形で自前実装が可能になる。
//
// ビルド (単体で確認用。SmoothScrollLib本体には未統合):
//   cl /EHsc /std:c++17 multitouch_probe.cpp user32.lib
//
// 使い方:
//   実行するとウィンドウとコンソールの両方が開く。
//   ウィンドウ上でタッチパッドを指1〜4本でタップ/動かすと、
//   コンソールに各指の座標・本数・2本指間の距離角度が出力される。

#include <Windows.h>
#include <windowsx.h>
#include <cstdio>
#include <cmath>
#include <map>

namespace {

// ---- コンソール確保 ----------------------------------------------------
// GUIサブシステムのアプリはデフォルトで標準出力を持たないため、
// AllocConsole + freopen で明示的に紐づける。
void SetupDebugConsole() {
    AllocConsole();
    FILE* fp = nullptr;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);
    // ついでにデバッガのOutput窓にも出したい場合は OutputDebugStringA を併用可能。
    SetConsoleTitleW(L"SmoothScroll MultiTouch Probe");
    printf("=== MultiTouch Probe: console ready ===\n");
    printf("Touch the window with 1-4 fingers on your precision touchpad.\n\n");
}

// ---- 現在アクティブな指(pointer)を追跡 ----------------------------------
struct FingerState {
    POINT pt; // クライアント座標
};
std::map<UINT32, FingerState> g_fingers; // pointerId -> state

void PrintFingerSummary() {
    printf("\r[fingers=%zu] ", g_fingers.size());
    for (auto& kv : g_fingers) {
        printf("id=%u(%ld,%ld) ", kv.first, kv.second.pt.x, kv.second.pt.y);
    }

    // 2本指の場合は距離・角度も計算して表示(ズーム/回転の元ネタ)
    if (g_fingers.size() == 2) {
        auto it = g_fingers.begin();
        POINT a = it->second.pt; ++it;
        POINT b = it->second.pt;

        float dx = (float)(b.x - a.x);
        float dy = (float)(b.y - a.y);
        float dist = std::sqrt(dx * dx + dy * dy);
        float angle_deg = std::atan2(dy, dx) * 180.0f / 3.14159265f;

        printf("| dist=%.1fpx angle=%.1fdeg", dist, angle_deg);
    }

    printf("                    "); // 行末の残り文字を消すためのパディング
    fflush(stdout);
}

// クライアント座標への変換込みで pointer 情報を取得するヘルパー
bool GetPointerClientPos(HWND hwnd, UINT32 pointerId, POINT& outPt) {
    POINTER_INFO info{};
    if (!GetPointerInfo(pointerId, &info)) return false;
    outPt = info.ptPixelLocation;
    ScreenToClient(hwnd, &outPt);
    return true;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        // マルチタッチのPOINTERメッセージを受け取るために必須。
        // (これが無いと合成された WM_POINTERWHEEL 経由にしかならず、
        //  生の複数指データが個別には降ってこない)
        EnableMouseInPointer(TRUE);
        return 0;

    case WM_POINTERDOWN: {
        UINT32 pointerId = GET_POINTERID_WPARAM(wParam);
        POINT pt;
        if (GetPointerClientPos(hwnd, pointerId, pt)) {
            g_fingers[pointerId] = { pt };
            printf("\n[DOWN] id=%u pos=(%ld,%ld) total_fingers=%zu\n",
                   pointerId, pt.x, pt.y, g_fingers.size());
            PrintFingerSummary();
        }
        return 0;
    }

    case WM_POINTERUPDATE: {
        UINT32 pointerId = GET_POINTERID_WPARAM(wParam);
        // まだDOWNを受け取っていないpointer(カーソル追従などノイズ)は無視
        auto found = g_fingers.find(pointerId);
        if (found == g_fingers.end()) return 0;

        POINT pt;
        if (GetPointerClientPos(hwnd, pointerId, pt)) {
            found->second.pt = pt;
            PrintFingerSummary();
        }
        return 0;
    }

    case WM_POINTERUP: {
        UINT32 pointerId = GET_POINTERID_WPARAM(wParam);
        g_fingers.erase(pointerId);
        printf("\n[UP]   id=%u total_fingers=%zu\n", pointerId, g_fingers.size());
        PrintFingerSummary();
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
    wc.lpszClassName = L"MultiTouchProbe";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName,
        L"MultiTouch Probe - watch the console window",
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