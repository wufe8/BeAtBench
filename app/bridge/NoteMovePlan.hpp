// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

namespace beatbench::app {

// 纯快照接口：不访问 QObject、活动文档、选择集或视口，也不执行命令。
// refs / grabRef 必须含完整源 identity；ranks 与 refs 同序，取自源列布局。
// columns 为 editColumns() 的完整序列（含 meta/BGA 占位及聚合 BGM sub_line=-1）。
struct NoteMoveSource {
    QVariantList refs;
    QVariantMap grabRef;
    QVector<int> ranks;
    int grabRank = -1;
    QVariantList columns;
};
struct NoteMoveDrop {
    // 目标 viewport 的原始 pointer musical F - 冻结的 source press musical F。
    double deltaF = 0.0;
    int targetRank = -1;
    QVariantMap targetColumn;
    QVariantList columns;
    int snapNum = 1;
    int snapDen = 16;
    bool timeLocked = false;
    // 纯时间移动可无任何列快照，严格保留 lane/sub_line。
    bool laneLocked = false;
};
struct NoteMovePlan {
    bool ok = false;
    bool changed = false;
    QString errorCode;
    QString errorMessage;
    QVariantList moves;
    QVariantList selection;
};
// 成功也可能 changed=false，调用方不得为此执行 note.move；失败输出永远无部分 moves/selection。
// 横向映射不夹边：演出目标列越界、BGM 负子行或身份碰撞则整组拒绝。
// BGM 合法落点允许扩展新的子行，不要求该子行已存在于目标显示列中。
// 聚合 BGM 源的不可见子行差保留；聚合目标以源抓起 BGM 子行为基线（play 源为 0）。
NoteMovePlan buildNoteMovePlan(const NoteMoveSource& source, const NoteMoveDrop& drop);

}  // namespace beatbench::app
