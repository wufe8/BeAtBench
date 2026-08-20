// SPDX-License-Identifier: GPL-3.0-only
// beatbench：Qt Widgets 图形界面（需要 Qt 6.8 LTS；找不到 Qt 时 CMake 自动跳过本目标）。
// 阶段说明：M0 空壳；M2 起填充工作区外壳（编辑/切音）与时间轴视口。
#include <QApplication>
#include <QLabel>
#include <QMainWindow>
#include <Qt>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QMainWindow window;
    window.setWindowTitle(QStringLiteral("BeAtBench"));
    auto* label = new QLabel(QStringLiteral(
        "M0 骨架已就绪。\n"
        "下一步：M1 格式层（core/bms codec + timing）\n"
        "        M2 面板（元信息/文件绑定/时间轴 + 工作区外壳）"));
    label->setAlignment(Qt::AlignCenter);
    window.setCentralWidget(label);
    window.resize(960, 540);
    window.show();
    return app.exec();
}
