// SmoothScrollHelper.h - SmoothScroll.dll のC++向け薄いラッパー
//
// 目的:
//   SmoothScroll.dll のコールバック(SS_ScrollCallback/SS_ZoomCallback)は、
//   フェーズによって呼び出し元スレッドが異なる(SmoothScroll.h参照):
//     - タッチ中(1:1パス動作)   : 入力メッセージを受け取ったスレッド(通常UIスレッド)から直接
//     - 慣性/バネ引き継ぎ後      : DLL内部の物理ループスレッドから
//
//   組み込み側がこの違いを意識せずコールバック内でHWND操作(InvalidateRect等)を
//   直接行ってしまうと、物理ループスレッドから来た呼び出しの場合にクラッシュ/
//   未定義動作を引き起こす。このヘッダは、呼び出し元スレッドを問わず常に
//   PostMessage経由でUIスレッドへディスパッチすることで、この種の実装ミスを
//   構造的に防ぐ。
//
// 使い方:
//   1. WndProc の適切な位置(WM_CREATE等)で SmoothScrollHelper を構築し、
//      std::function または ラムダでコールバックを登録する。
//   2. WndProc の WM_APP+xxx (下記 kScrollMessage/kZoomMessage) を
//      SmoothScrollHelper::HandleMessage() に渡す。
//   3. あとは登録したラムダの中で、常にUIスレッド上にいる前提でUI更新してよい。
//
// 例:
//   SmoothScrollHelper g_ssHelper;
//
//   case WM_CREATE:
//       g_ssHelper.Attach(hwnd, cfg,
//           [](float dx, float dy) { /* UIスレッド上で安全に呼ばれる */ },
//           [](float scale, float ax, float ay) { /* 同上 */ });
//       return 0;
//
//   case SmoothScrollHelper::kScrollMessage:
//   case SmoothScrollHelper::kZoomMessage:
//       g_ssHelper.HandleMessage(msg, wParam, lParam);
//       return 0;
//
//   case WM_DESTROY:
//       g_ssHelper.Detach();
//       ...
//
// 既存DLLのABIには一切手を加えていない。本ヘッダのみを追加すればよい。
#pragma once

#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include "SmoothScroll.h"

class SmoothScrollHelper {
public:
    // ホストのWndProcでこの2つのメッセージを本クラスにディスパッチすること。
    // WM_APPからのオフセットは、ホスト側で既に使っている値と衝突しないように
    // 必要なら変更してよい(片方だけ変更する場合は両方の値がユニークであることを
    // 確認すること)。
    static constexpr UINT kScrollMessage = WM_APP + 0x5301;
    static constexpr UINT kZoomMessage   = WM_APP + 0x5302;

    using ScrollCallback = std::function<void(float dx, float dy)>;
    using ZoomCallback   = std::function<void(float scale_factor, float anchor_x, float anchor_y)>;

    SmoothScrollHelper() = default;

    // コピー・ムーブ禁止(内部でHWNDに紐づくポインタをDLLに渡しているため)。
    SmoothScrollHelper(const SmoothScrollHelper&) = delete;
    SmoothScrollHelper& operator=(const SmoothScrollHelper&) = delete;

    ~SmoothScrollHelper() {
        Detach();
    }

    // hwnd に取り付ける。cfg は必要に応じて呼び出し前に書き換えてよい
    // (nullptrなら既定値を使用)。scrollCb/zoomCb は std::function または
    // ラムダをそのまま渡せる。zoomCb は不要なら省略可(nullptr相当)。
    //
    // 戻り値: 成功したら true。
    //
    // 重要: scrollCb/zoomCb は常にUIスレッド上(hwndのメッセージポンプを
    // 回しているスレッド)から呼ばれることが保証される。呼び出し元が
    // 物理ループスレッドだった場合は、内部で自動的に PostMessage 経由に
    // 切り替えて hwnd 宛にディスパッチする。これによりコールバック内で
    // 直接UI操作(InvalidateRect等)を行っても安全である。
    bool Attach(HWND hwnd, const SS_Config* cfg, ScrollCallback scrollCb, ZoomCallback zoomCb = nullptr) {
        if (!hwnd) return false;
        Detach(); // 二重Attach対策

        hwnd_ = hwnd;
        ui_thread_id_ = GetWindowThreadProcessId(hwnd, nullptr);
        scroll_cb_ = std::move(scrollCb);
        zoom_cb_ = std::move(zoomCb);

        // このインスタンスへの弱くない生ポインタをDLLのuserとして渡す。
        // Detach()内でss_detachしてから解放するため、コールバックが飛んでいる
        // 最中に破棄されることはない(ss_detachが物理スレッドの終了を待つため)。
        handle_ = ss_attach(hwnd, cfg, &SmoothScrollHelper::OnScrollThunk,
                            zoomCb ? &SmoothScrollHelper::OnZoomThunk : nullptr, this);
        if (!handle_) {
            hwnd_ = nullptr;
            return false;
        }
        return true;
    }

    void Detach() {
        if (handle_) {
            ss_detach(handle_); // 物理スレッドの終了を待つのでコールバックとの競合はない
            handle_ = nullptr;
        }
        hwnd_ = nullptr;
        scroll_cb_ = nullptr;
        zoom_cb_ = nullptr;
    }

    // WndProc から kScrollMessage/kZoomMessage を受け取ったらこれを呼ぶこと。
    // ここは必ずUIスレッド上(WndProc内)から呼ばれる前提。
    void HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == kScrollMessage) {
            float dx, dy;
            UnpackFloat(wParam, dx);
            UnpackFloat((WPARAM)lParam, dy);
            if (scroll_cb_) scroll_cb_(dx, dy);
        } else if (msg == kZoomMessage) {
            auto* payload = reinterpret_cast<ZoomPayload*>(lParam);
            if (payload) {
                if (zoom_cb_) zoom_cb_(payload->scale, payload->anchor_x, payload->anchor_y);
                delete payload;
            }
        }
    }

    HANDLE raw_handle() const { return handle_; }

private:
    struct ZoomPayload {
        float scale;
        float anchor_x;
        float anchor_y;
    };

    static void PackFloat(float v, WPARAM& out) {
        int32_t packed;
        std::memcpy(&packed, &v, sizeof(float));
        out = (WPARAM)(intptr_t)packed;
    }
    static void UnpackFloat(WPARAM in, float& out) {
        int32_t packed = (int32_t)(intptr_t)in;
        std::memcpy(&out, &packed, sizeof(float));
    }

    // 現在のスレッドがUIスレッドかどうかを見て、違えばPostMessageで
    // ディスパッチし直す。同じスレッドなら(タッチ中の1:1パス動作など)
    // 待ち時間を作らずその場で直接コールバックを呼ぶ。
    void DispatchScroll(float dx, float dy) {
        if (GetCurrentThreadId() == ui_thread_id_) {
            if (scroll_cb_) scroll_cb_(dx, dy);
            return;
        }
        WPARAM wx; PackFloat(dx, wx);
        WPARAM wy; PackFloat(dy, wy);
        PostMessageW(hwnd_, kScrollMessage, wx, (LPARAM)wy);
    }

    void DispatchZoom(float scale, float ax, float ay) {
        if (GetCurrentThreadId() == ui_thread_id_) {
            if (zoom_cb_) zoom_cb_(scale, ax, ay);
            return;
        }
        auto* payload = new (std::nothrow) ZoomPayload{ scale, ax, ay };
        if (!payload) return;
        if (!PostMessageW(hwnd_, kZoomMessage, 0, (LPARAM)payload)) {
            delete payload; // ポスト自体に失敗した場合はリークしないよう即解放
        }
    }

    static void CALLBACK OnScrollThunk(float dx, float dy, void* user) {
        auto* self = static_cast<SmoothScrollHelper*>(user);
        if (self) self->DispatchScroll(dx, dy);
    }

    static void CALLBACK OnZoomThunk(float scale_factor, float anchor_x, float anchor_y, void* user) {
        auto* self = static_cast<SmoothScrollHelper*>(user);
        if (self) self->DispatchZoom(scale_factor, anchor_x, anchor_y);
    }

    HWND hwnd_ = nullptr;
    DWORD ui_thread_id_ = 0;
    HANDLE handle_ = nullptr;
    ScrollCallback scroll_cb_;
    ZoomCallback zoom_cb_;
};