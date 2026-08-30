// PhysicsEngine.h - 慣性スクロール物理モデル
// モデル: 指数減衰 v(t) = v0 * exp(-k*t)  (Mac風スクロールの減衰曲線)
// 指が触れている間: 生デルタをそのまま通しつつ速度をEMA推定
// 指が離れた後   : 推定速度を初速として慣性フェーズへ遷移
//
// このクラスは「1次元(または独立な2次元)の位置に対する慣性」という
// 抽象度で作られているため、パンのピクセル位置だけでなく、
// ズームのスケール値(対数空間で扱う)にもそのまま流用できる。
// 詳細は stepContinuous() のコメントを参照。
#pragma once
#include <Windows.h>
#include "SmoothScroll.h"

class PhysicsEngine {
public:
    explicit PhysicsEngine(const SS_Config& cfg);

    // 生のタッチパッドデルタを入力（タッチ中）
    // dx, dy: ピクセル換算済みデルタ, dt_sec: 前回「同じ軸の」イベントからの経過秒
    // freeze_dt_sec: 更新されない軸(update_x/update_yがfalseの軸)を減衰させる際に
    //   使う経過秒。「前回、縦横どちらかのイベントが来てからの経過時間」を渡すこと。
    //   省略(負値)した場合は dt_sec をそのまま使う(後方互換/ズーム用途向け)。
    //
    //   重要: dt_sec と freeze_dt_sec を混同してはいけない。
    //   Precision TouchpadはWM_POINTERWHEEL(縦)とWM_POINTERHWHEEL(横)が
    //   別々のメッセージで届くため、軸ロックで例えば縦だけイベントが
    //   来ている間、横軸は「前回の横イベントからの経過時間」で見ると
    //   実際には何十msも間が空いていることがある。
    //   もしこの「同じ軸基準の長い間隔」を凍結側(横)の減衰計算にそのまま
    //   使ってしまうと、ロックが外れて久しぶりに横イベントが来た瞬間、
    //   横軸の減衰が一気にドカッと効いてしまい(本来はロック中もなだらかに
    //   減衰し続けているべきなのに)、軸切り替えの瞬間だけ速度が不自然に
    //   カクつく原因になる(実際に踏んだ)。
    //   そのため凍結側の減衰には「縦横どちらのイベントでもよいので前回の
    //   呼び出しからの経過時間」を渡す必要がある。
    //
    // update_x, update_y: この呼び出しで実際に値が更新された軸かどうか。
    //   Precision TouchpadはWM_POINTERWHEEL(縦)とWM_POINTERHWHEEL(横)が
    //   別々のメッセージで届くため、片方の軸のイベントしか来ていない時に
    //   もう片方を「0」として渡すと、その軸の速度が実際には動き続けている
    //   かもしれないのに強制的にゼロへ引っ張られてしまう
    //   (OSの軸ロックがかかった瞬間に「カクッ」と不自然に感じる原因)。
    //   このフラグをfalseにした軸は速度を変更せず、そのまま維持する。
    void feedDelta(float dx, float dy, float dt_sec, float freeze_dt_sec = -1.0f,
                   bool update_x = true, bool update_y = true);

    // タッチ終了 → 慣性フェーズ開始
    void release();

    // 物理ステップ。dt_sec 経過後の移動量を out に出力。
    // 戻り値: まだ動いているなら true（false ならループはスリープしてよい）
    // パンのピクセル位置向け: 整数ピクセルへ丸め、端数(サブピクセル)を繰り越す。
    // 注意: 低速域(慣性の終盤)では丸めにより「ピタッ…ピタッ」という
    // 量子化ジッターが体感されることがある。ホスト側でfloatの位置を
    // 保持できるなら、代わりに stepContinuous() の使用を検討すること
    // (SS_Config::pan_subpixel を TRUE にすると DLL 側が自動的に
    //  こちらを使うようになる)。
    bool step(float dt_sec, float& out_dx, float& out_dy);

    // step() の連続値版: 整数ピクセルへの丸め・端数繰り越しを行わない。
    // ズームのスケール値(対数空間の位置として扱う)は
    // 丸めると精度が失われ、特に慣性の終盤で不自然にカクつく。
    // 速度・減衰の計算ロジック自体は step() と全く同じ(同じ vx_/vy_ 状態を使う)。
    // ズーム用途では dy チャンネルは使わず 0 のまま渡してよい(1次元として使う)。
    // ホスト側がfloatのスクロール位置を保持できる場合、パン用途にもそのまま
    // 使ってよい(量子化ジッターが発生しない)。
    bool stepContinuous(float dt_sec, float& out_dx, float& out_dy);

    // 完全キャンセル: タッチ中フラグも含めて全状態をリセットする。
    // 新しいタッチ開始(WM_POINTERDOWN)など「本当に新規セッションが始まる」場面専用。
    void cancel();

    // 慣性のみ停止: touching_ フラグには触れない。
    // ユーザーがまだ指を動かしている最中に「境界に到達したので暴れる慣性を止めたい」
    // という場面で使う。touching_ を誤って false にすると、まだドラッグ中なのに
    // ホスト側のバネ処理が「操作終了した」と誤判定して起動し、
    // ユーザーの入力とバネが同じ位置を奪い合って暴れる原因になる。
    void stopMomentumOnly();

    // 現在のエンジン内部速度(vx_/vy_)を取得する。単位はfeedDelta()に渡した
    // dx/dyと同じ(パン用途ではピクセル/秒)。
    // 用途: 慣性からバネ(スプリング)アニメーションへ引き継ぐ際、バネの初速を
    // 0からではなく実際の速度から始めることで、速度の不連続(体感上の
    // 「ガクッ」というジャーク)を避けるために使う。
    void getVelocity(float& vx, float& vy) const;

    // エンジン内部速度(vx_/vy_)にホスト側から補正係数を掛ける。
    // 通常は使わなくてよい低レベルAPI(必要な場面が生じた場合のための保険)。
    void scaleVelocity(float sx, float sy);

    bool isActive() const;
    bool isTouching() const;

private:
    SS_Config cfg_;
    float vx_ = 0, vy_ = 0;      // 現在速度 [px/s] (ズーム用途では「対数速度」)
    float rem_x_ = 0, rem_y_ = 0; // サブピクセル端数の繰り越し(step()専用。stepContinuous()は未使用)
    bool touching_ = false;
    bool momentum_ = false;

    // 指を離す直前、Precision Touchpadはイベント間隔(dt)が広がりやすい
    // (サンプリングが疎になる)ことが実機観測で確認されている。
    // その最後の1サンプルだけを見ると瞬間速度が実際のフリックより小さく
    // 計測されがちで、これをそのままEMAへフルの重みでブレンドすると
    // 「指を離した瞬間に失速して伸びない」原因になる。
    // dt がこの値を超えるサンプルは、EMAの重み(velocity_ema)を
    // dt に反比例させて弱め、直前までの速度推定をなるべく保つ。
    static constexpr float kReleaseArtifactDt = 0.03f; // 30ms
};