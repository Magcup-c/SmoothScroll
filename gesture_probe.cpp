// gesture_probe.cpp - レガシー WM_GESTURE API を使って Precision Touchpad の
// ピンチ(ズーム)/回転/パンをコンソールに出力する検証プログラム。
//
// 背景:
//   ic_probe.cpp (Interaction Context, WM_POINTER方式) を試したところ、
//   このマシンのタッチパッドは type=MOUSE の単一ポインタしか送ってこず、
//   2本指ジェスチャが生の複数コンタクトとしては全く届かないことが判明した。
//   これは EnableMouseInPointer(TRUE) によってOSが「ポインタモデル」に
//   一本化され、代わりに古典的な WM_GESTURE 経路(OS内蔵の2本指ジェスチャ認識結果を
//   GID_ZOOM/GID_PAN/GID_ROTATE として渡す仕組み)が無効化されてしまった
//   可能性がある。今回は EnableMouseInPointer を一切呼ばず、
//   代わりにこのWM_GESTURE経路だけを試す。
//
// ビルド (Developer Command Prompt):
//   cl /EHsc /std:c++17 gesture_probe.cpp user32.lib
//
// 使い方:
//   ウィンドウにフォーカスした状態でタッチパッドを2本指でズーム/回転/パンすると
//   コンソールに値が出力される。

#include <Windows.h>
#include <windowsx.h>
#include <cstdio>
#include <cmath>

namespace {

void SetupDebugConsole() {
    AllocConsole();
    FILE* fp = nullptr;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);
    SetConsoleTitleW(L"SmoothScroll WM_GESTURE Probe");
    printf("=== WM_GESTURE Probe: console ready ===\n");
    printf("Pinch(zoom) / rotate / pan with 2 fingers on your precision touchpad.\n\n");
}

// GID_ROTATE の ullArguments は角度をUINT16スケールにエンコードした値。
// 公式マクロ GID_ROTATE_ANGLE_FROM_ARGUMENT と同じ変換式。
double RotateAngleFromArgument(ULONGLONG arg) {
    constexpr double PI = 3.14159265358979323846;
    return (((double)(arg) / 65535.0) * 4.0 * PI) - (2.0 * PI);
}

const char* GestureIdName(DWORD id) {
    switch (id) {
        case GID_ZOOM:          return "ZOOM";
        case GID_PAN:           return "PAN";
        case GID_ROTATE:        return "ROTATE";
        case GID_TWOFINGERTAP:  return "TWOFINGERTAP";
        case GID_PRESSANDTAP:   return "PRESSANDTAP";
        case GID_BEGIN:         return "BEGIN";
        case GID_END:           return "END";
        default: return "UNKNOWN";
    }
}

bool SetupGestureConfig(HWND hwnd) {
    GESTURECONFIG gc[] = {
        { GID_ZOOM,   GC_ZOOM,   0 },
        { GID_ROTATE, GC_ROTATE, 0 },
        { GID_PAN,    GC_PAN | GC_PAN_WITH_SINGLE_FINGER_VERTICALLY |
                       GC_PAN_WITH_SINGLE_FINGER_HORIZONTALLY, 0 },
        { GID_TWOFINGERTAP,  GC_TWOFINGERTAP,  0 },
        { GID_PRESSANDTAP,   GC_PRESSANDTAP,   0 },
    };
    BOOL ok = SetGestureConfig(hwnd, 0, ARRAYSIZE(gc), gc, sizeof(GESTURECONFIG));
    if (!ok) {
        printf("SetGestureConfig failed: %lu\n", GetLastError());
        return false;
    }
    printf("SetGestureConfig OK (ZOOM/ROTATE/PAN/TWOFINGERTAP/PRESSANDTAP enabled).\n");
    return true;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        // 注意: ここでは EnableMouseInPointer を呼ばない。
        // WM_GESTURE(レガシーAPI)を使う場合、ポインタモデルへの切り替えは不要かつ
        // むしろOS内蔵の2本指ジェスチャ認識を妨げる可能性があるため。
        SetupGestureConfig(hwnd);
        return 0;
    }

    // WM_GESTURENOTIFY は「これからジェスチャが始まりそうだ」という事前通知。
    // ここでも設定し直しておくと、ウィンドウ生成直後の取りこぼしに強くなる。
    case WM_GESTURENOTIFY:
        SetupGestureConfig(hwnd);
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    case WM_GESTURE: {
        GESTUREINFO gi{};
        gi.cbSize = sizeof(GESTUREINFO);
        BOOL ok = GetGestureInfo((HGESTUREINFO)lParam, &gi);
        if (!ok) {
            DWORD err = GetLastError();
            if (err) printf("\n[warn] GetGestureInfo failed: %lu\n", err);
            break;
        }

        bool isBegin = (gi.dwFlags & GF_BEGIN) != 0;
        bool isEnd   = (gi.dwFlags & GF_END) != 0;
        bool isInertia = (gi.dwFlags & GF_INERTIA) != 0;

        switch (gi.dwID) {
        case GID_ZOOM: {
            // ullArguments: 2点間の距離(ピクセル)。GF_BEGIN時が基準距離。
            static ULONGLONG baseDist = 0;
            if (isBegin) baseDist = gi.ullArguments;
            double ratio = (baseDist != 0) ? (double)gi.ullArguments / (double)baseDist : 1.0;
            printf("\r[ZOOM%s] dist=%llu ratio=%.4f center=(%ld,%ld)          ",
                   isInertia ? "*inertia*" : (isBegin ? " begin" : ""),
                   (unsigned long long)gi.ullArguments, ratio, gi.ptsLocation.x, gi.ptsLocation.y);
            fflush(stdout);
            break;
        }
        case GID_ROTATE: {
            // ullArguments: GF_BEGIN時は絶対角度、以降は前回からの変化量。
            double angle_rad = RotateAngleFromArgument(gi.ullArguments);
            double angle_deg = angle_rad * 180.0 / 3.14159265358979323846;
            printf("\r[ROTATE%s] angle=%+7.2fdeg center=(%ld,%ld)          ",
                   isBegin ? " begin" : "",
                   angle_deg, gi.ptsLocation.x, gi.ptsLocation.y);
            fflush(stdout);
            break;
        }
        case GID_PAN: {
            printf("\r[PAN%s] dist=%llu pos=(%ld,%ld)          ",
                   isInertia ? "*inertia*" : (isBegin ? " begin" : ""),
                   (unsigned long long)gi.ullArguments, gi.ptsLocation.x, gi.ptsLocation.y);
            fflush(stdout);
            break;
        }
        default:
            printf("\n[gesture] id=%s flags=0x%08X\n", GestureIdName(gi.dwID), gi.dwFlags);
            break;
        }

        if (isEnd) printf("\n[end: %s]\n", GestureIdName(gi.dwID));

        // 仕様上、GID_BEGIN/GID_ENDを含め、処理しなかった場合は
        // DefWindowProcに渡す必要がある(レガシーサポートのため)。
        return DefWindowProcW(hwnd, msg, wParam, lParam);
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
    wc.lpszClassName = L"GestureProbe";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName,
        L"WM_GESTURE Probe - watch the console window",
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