// SPDX-License-Identifier: GPL-3.0-only
// EditUtils 实现：见 EditUtils.hpp（纯函数，无 GUI/core 模型依赖）。
#include "EditUtils.hpp"

#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>

namespace beatbench::app {
namespace {

/// 变体是否「有值」（缺失 / null / invalid 都算没有；对齐 JS undefined 通配语义）。
bool hasValue(const QVariantMap& m, const QString& key) {
    const auto it = m.constFind(key);
    return it != m.constEnd() && it->isValid() && !it->isNull();
}

/// 64 位 gcd（0 → 1）。QML 数字是 double，先转 qint64 避免 int 溢出。
qint64 gcd64(qint64 a, qint64 b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b != 0) {
        const qint64 t = b;
        b = a % b;
        a = t;
    }
    return a != 0 ? a : 1;
}

const QRegularExpression& dataLineRe() {
    static const QRegularExpression re(QStringLiteral("^#[0-9]{3}[0-9A-Za-z]{2,}:(.*)$"));
    return re;
}
const QRegularExpression& dataLineLooseRe() {
    static const QRegularExpression re(QStringLiteral("^#[0-9]{3}[0-9A-Za-z]{2,}:"));
    return re;
}
const QRegularExpression& defLineRe() {
    // 定义行 id 字段（捕获组 = id 文本；值/文件名不参与 Base62 判定）。
    static const QRegularExpression re(
        QStringLiteral("^#(?:WAV|BMP|BPM|STOP)([0-9A-Za-z]{1,2})[ \\t]"));
    return re;
}
const QRegularExpression& lowerRe() {
    static const QRegularExpression re(QStringLiteral("[a-z]"));
    return re;
}
const QRegularExpression& base62DeclRe() {
    static const QRegularExpression re(QStringLiteral("^#BASE[ \\t]+62([ \\t]|$)"),
                                       QRegularExpression::CaseInsensitiveOption);
    return re;
}

}  // namespace

EditUtils::EditUtils(QObject* parent) : QObject(parent) {}

int EditUtils::gcd(int a, int b) const { return static_cast<int>(gcd64(a, b)); }

bool EditUtils::refEquals(const QVariantMap& a, const QVariantMap& b) const {
    if (a.isEmpty() || b.isEmpty()) return false;  // JS: a && b
    if (a.value(QStringLiteral("measure")) != b.value(QStringLiteral("measure"))) return false;
    if (a.value(QStringLiteral("sample")) != b.value(QStringLiteral("sample"))) return false;
    const QVariantMap la = a.value(QStringLiteral("lane")).toMap();
    const QVariantMap lb = b.value(QStringLiteral("lane")).toMap();
    if (la.value(QStringLiteral("kind")) != lb.value(QStringLiteral("kind"))) return false;
    if (la.value(QStringLiteral("index")) != lb.value(QStringLiteral("index"))) return false;
    if (la.value(QStringLiteral("player")) != lb.value(QStringLiteral("player"))) return false;
    const QVariantMap pa = a.value(QStringLiteral("pos")).toMap();
    const QVariantMap pb = b.value(QStringLiteral("pos")).toMap();
    if (pa.value(QStringLiteral("num")) != pb.value(QStringLiteral("num"))) return false;
    if (pa.value(QStringLiteral("den")) != pb.value(QStringLiteral("den"))) return false;
    const bool aSub = hasValue(a, QStringLiteral("sub_line"));
    const bool bSub = hasValue(b, QStringLiteral("sub_line"));
    if (aSub && bSub && a.value(QStringLiteral("sub_line")) != b.value(QStringLiteral("sub_line")))
        return false;
    return true;
}

QVariantMap EditUtils::addPosDelta(const QVariantMap& ref, const QVariantMap& delta) const {
    const QVariantMap rp = ref.value(QStringLiteral("pos")).toMap();
    const QVariantMap dp = delta.value(QStringLiteral("pos")).toMap();
    const qint64 rn = rp.value(QStringLiteral("num")).toLongLong();
    const qint64 rd = rp.value(QStringLiteral("den")).toLongLong();
    const qint64 dn = dp.value(QStringLiteral("num")).toLongLong();
    const qint64 dd = dp.value(QStringLiteral("den")).toLongLong();
    if (rd == 0 || dd == 0) return {};
    const qint64 newNum = rn * dd + dn * rd;
    const qint64 newDen = rd * dd;
    qint64 carry = newNum / newDen;
    // JS Math.floor：负分数向下取整（C++ 整数除法向零截断）。
    if ((newNum % newDen != 0) && ((newNum < 0) != (newDen < 0))) --carry;
    const qint64 rem = newNum - carry * newDen;
    const qint64 g = gcd64(rem, newDen);
    QVariantMap pos;
    pos.insert(QStringLiteral("num"), static_cast<int>(rem / g));
    pos.insert(QStringLiteral("den"), static_cast<int>(newDen / g));
    QVariantMap out;
    out.insert(QStringLiteral("measure"), ref.value(QStringLiteral("measure")).toInt() +
                                              delta.value(QStringLiteral("measure")).toInt() +
                                              static_cast<int>(carry));
    out.insert(QStringLiteral("pos"), pos);
    return out;
}

bool EditUtils::looksLikeBmsText(const QString& text) const {
    const QStringList lines = text.split(QRegularExpression(QStringLiteral("\\r?\\n")));
    for (const QString& raw : lines) {
        const QString s = raw.trimmed();
        if (s.isEmpty() || !s.startsWith(QLatin1Char('#'))) continue;
        if (dataLineLooseRe().match(s).hasMatch()) return true;
        if (defLineRe().match(s).hasMatch()) return true;
    }
    return false;
}

bool EditUtils::textUsesBase62Ids(const QString& text) const {
    const QStringList lines = text.split(QRegularExpression(QStringLiteral("\\r?\\n")));
    for (const QString& raw : lines) {
        const QString s = raw.trimmed();
        if (s.isEmpty() || !s.startsWith(QLatin1Char('#'))) continue;
        if (base62DeclRe().match(s).hasMatch()) return true;
        const auto dm = defLineRe().match(s);
        if (dm.hasMatch() && lowerRe().match(dm.captured(1)).hasMatch()) return true;
        const auto lm = dataLineRe().match(s);
        if (lm.hasMatch() && lowerRe().match(lm.captured(1)).hasMatch()) return true;
    }
    return false;
}

QString EditUtils::classifyExternalFile(const QString& path) const {
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext.isEmpty()) return QStringLiteral("unknown");
    // 扩展名与 UI 过滤器 / 后端能力对齐：
    //   谱面 FileDialog(*.bms *.bml *.bme *.pms)
    //   音频 audio_extension_supported()（wav/ogg/oga/mp3/flac）
    //   MIDI FileDialog(*.mid *.midi)
    static const QStringList kChart = {QStringLiteral("bms"), QStringLiteral("bml"),
                                       QStringLiteral("bme"), QStringLiteral("pms")};
    static const QStringList kAudio = {QStringLiteral("wav"), QStringLiteral("ogg"),
                                       QStringLiteral("oga"), QStringLiteral("mp3"),
                                       QStringLiteral("flac")};
    static const QStringList kMidi = {QStringLiteral("mid"), QStringLiteral("midi")};
    if (kChart.contains(ext)) return QStringLiteral("chart");
    if (kAudio.contains(ext)) return QStringLiteral("audio");
    if (kMidi.contains(ext)) return QStringLiteral("midi");
    return QStringLiteral("unknown");
}

}  // namespace beatbench::app
