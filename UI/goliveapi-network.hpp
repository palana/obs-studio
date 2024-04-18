#pragma once

#include <obs.hpp>
#include <QFuture>
#include <QString>
#include <nlohmann/json_fwd.hpp>

/** Returns either GO_LIVE_API_PRODUCTION_URL or a command line override. */
QString MultitrackVideoAutoConfigURL(obs_service_t *service);

class QWidget;

OBSDataAutoRelease DownloadGoLiveConfig(QWidget *parent, QString url,
					const nlohmann::json &postData);
