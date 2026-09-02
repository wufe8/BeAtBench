// SPDX-License-Identifier: GPL-3.0-only
#include "bridge/CommandDispatcher.hpp"

#include "beatbench/core/Version.hpp"
#include "beatbench/core/command/Command.hpp"
#include "beatbench/core/json/Json.hpp"

#include <QString>

namespace beatbench::app {

CommandDispatcher::CommandDispatcher(QObject* parent) : QObject(parent) {}

QString CommandDispatcher::dispatch(const QString& request) const {
    using beatbench::json::Json;
    using beatbench::json::JsonError;

    Json req;
    try {
        req = Json::parse(request.toStdString());
    } catch (const JsonError& e) {
        // 请求 JSON 非法：给一个 bad_request 信封（与 CLI 退出码 2 场景一致，doc/06 §3.4）
        Json err = Json::object();
        err.set("ok", false);
        Json eo = Json::object();
        eo.set("code", "bad_request");
        eo.set("message", std::string("请求 JSON 非法: ") + e.what());
        err.set("error", std::move(eo));
        return QString::fromUtf8(err.dump().c_str());
    }

    // dispatch 自身不抛：信封/参数/命令错误全部收编为 ok:false 响应（doc/06 §3.2）
    const Json res = beatbench::cmd::global_registry().dispatch(req);
    return QString::fromUtf8(res.dump().c_str());
}

QString CommandDispatcher::version() const {
    using beatbench::json::Json;
    Json req = Json::object();
    req.set("command", "version");
    return dispatch(QString::fromUtf8(req.dump().c_str()));
}

QString CommandDispatcher::versionString() const {
    return QString::fromUtf8(beatbench::kVersion.data(),
                             static_cast<int>(beatbench::kVersion.size()));
}

}  // namespace beatbench::app
