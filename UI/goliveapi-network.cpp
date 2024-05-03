#include "goliveapi-network.hpp"
#include "goliveapi-censoredjson.hpp"

#include <obs.hpp>
#include <obs-app.hpp>
#include <remote-text.hpp>
#include "multitrack-video-error.hpp"

#include <qstring.h>
#include <string>
#include <QMessageBox>
#include <QThreadPool>

#include <nlohmann/json.hpp>

Qt::ConnectionType BlockingConnectionTypeFor(QObject *object);

void HandleGoLiveApiErrors(QWidget *parent, obs_data_t *config_data)
{
	OBSDataAutoRelease status = obs_data_get_obj(config_data, "status");
	if (!status)
		return;

	auto result = obs_data_get_string(status, "result");
	if (!result || strcmp(result, "success") == 0)
		return;

	auto html_en_us = obs_data_get_string(status, "html_en_us");
	if (strcmp(result, "warning") == 0) {
		OBSDataArrayAutoRelease encoder_configurations =
			obs_data_get_array(config_data,
					   "encoder_configurations");
		if (obs_data_array_count(encoder_configurations) == 0) {
			throw MultitrackVideoError::warning(html_en_us);
		} else {
			bool ret = false;
			QMetaObject::invokeMethod(
				parent,
				[=] {
					QMessageBox mb(parent);
					mb.setIcon(QMessageBox::Warning);
					mb.setWindowTitle(QTStr(
						"ConfigDownload.WarningMessageTitle"));
					mb.setTextFormat(Qt::RichText);
					mb.setText(
						html_en_us +
						QTStr("FailedToStartStream.WarningRetry"));
					mb.setStandardButtons(
						QMessageBox::StandardButton::Yes |
						QMessageBox::StandardButton::No);
					return mb.exec() ==
					       QMessageBox::StandardButton::No;
				},
				BlockingConnectionTypeFor(parent), &ret);
			if (ret)
				throw MultitrackVideoError::cancel();
		}
	} else if (strcmp(result, "error") == 0) {
		throw MultitrackVideoError::critical(html_en_us);
	}
}

OBSDataAutoRelease DownloadGoLiveConfig(QWidget *parent, QString url,
					const GoLiveApi::PostData &post_data)
{
	json post_data_json = post_data;
	blog(LOG_INFO, "Go live POST data: %s",
	     censoredJson(post_data_json).toUtf8().constData());

	if (url.isEmpty())
		throw MultitrackVideoError::critical(
			QTStr("FailedToStartStream.MissingConfigURL"));

	std::string encodeConfigText;
	std::string libraryError;

	std::vector<std::string> headers;
	headers.push_back("Content-Type: application/json");
	bool encodeConfigDownloadedOk = GetRemoteFile(
		url.toLocal8Bit(), encodeConfigText,
		libraryError, // out params
		nullptr,
		nullptr, // out params (response code and content type)
		"POST", post_data_json.dump().c_str(), headers,
		nullptr, // signature
		5);      // timeout in seconds

	if (!encodeConfigDownloadedOk)
		throw MultitrackVideoError::warning(
			QTStr("FailedToStartStream.ConfigRequestFailed")
				.arg(url, libraryError.c_str()));

	OBSDataAutoRelease encodeConfigObsData =
		obs_data_create_from_json(encodeConfigText.c_str());
	blog(LOG_INFO, "Go live Response data: %s",
	     censoredJson(encodeConfigObsData, true).toUtf8().constData());

	HandleGoLiveApiErrors(parent, encodeConfigObsData);

	return encodeConfigObsData;
}

QString MultitrackVideoAutoConfigURL(obs_service_t *service)
{
	static const QString url = [service]() -> QString {
		auto args = qApp->arguments();
		for (int i = 0; i < args.length() - 1; i++) {
			if (args[i] == "--config-url" &&
			    args.length() > (i + 1)) {
				return args[i + 1];
			}
		}
		OBSDataAutoRelease settings = obs_service_get_settings(service);
		return obs_data_get_string(
			settings, "multitrack_video_configuration_url");
	}();

	blog(LOG_INFO, "Go live URL: %s", url.toUtf8().constData());
	return url;
}
