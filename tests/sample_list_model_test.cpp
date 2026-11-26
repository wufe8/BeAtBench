// SPDX-License-Identifier: GPL-3.0-only
// 多文档切换不能把上张谱面同 ID 的缺失/选中状态带进下一张。
#include <gtest/gtest.h>

#include "bridge/SampleListModel.hpp"

namespace {
using beatbench::app::SampleListModel;
const QString info = QStringLiteral(
    R"({"result":{"id_base":36,"samples":{"wav":[{"id":"01","file":"test.wav","refs":1}]}}})");
const QString missing =
    QStringLiteral(R"({"result":{"lint":{"missing_wav":[{"id":"01"}],"wav_ext_mismatch":[]}}})");
const QString present =
    QStringLiteral(R"({"result":{"lint":{"missing_wav":[],"wav_ext_mismatch":[]}}})");

TEST(SampleListModelDocument, ClearResetsAvailabilityAndSelectionAcrossDocuments) {
    SampleListModel model;
    model.loadFromInfo(info);
    model.loadFromCheck(missing);
    model.selectId("01");
    EXPECT_TRUE(model.data(model.index(0), SampleListModel::MissingRole).toBool());
    model.clear();
    EXPECT_TRUE(model.currentSample().isEmpty());
    model.loadFromInfo(info);
    EXPECT_FALSE(model.data(model.index(0), SampleListModel::MissingRole).toBool());
}

TEST(SampleListModelDocument, InMemoryCheckRefreshesFlagsForSameDocument) {
    SampleListModel model;
    model.loadFromInfo(info);
    model.loadFromCheck(missing);
    model.loadFromInfo(info);
    EXPECT_TRUE(model.data(model.index(0), SampleListModel::MissingRole).toBool());
    model.loadFromCheck(present);
    EXPECT_FALSE(model.data(model.index(0), SampleListModel::MissingRole).toBool());
}
}  // namespace
