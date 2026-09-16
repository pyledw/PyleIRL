#include "multistream.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>
#include <util/config-file.h>
#include <util/bmem.h>

QJsonObject MultistreamTargetConfig::toJson() const
{
	QJsonObject obj;
	obj["id"] = id;
	obj["name"] = name;
	obj["type"] = type;
	obj["url"] = url;
	obj["key"] = key;
	obj["enabled"] = enabled;
	obj["isVertical"] = isVertical;
	obj["targetScene"] = targetScene;
	return obj;
}

MultistreamTargetConfig MultistreamTargetConfig::fromJson(const QJsonObject &obj)
{
	MultistreamTargetConfig c;
	c.id = obj["id"].toString();
	c.name = obj["name"].toString();
	c.type = obj["type"].toString();
	c.url = obj["url"].toString();
	c.key = obj["key"].toString();
	c.enabled = obj["enabled"].toBool(true);
	c.isVertical = obj["isVertical"].toBool(false);
	c.targetScene = obj["targetScene"].toString();
	if (c.id.isEmpty()) {
		c.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
	}
	return c;
}

MultistreamTarget::MultistreamTarget(const MultistreamTargetConfig &config, QObject *parent)
	: QObject(parent),
	  config(config)
{
}

MultistreamTarget::~MultistreamTarget()
{
	cleanupOutput();
}

void MultistreamTarget::initOutput()
{
	if (output)
		return;

	obs_data_t *settings = obs_data_create();
	obs_data_t *service_settings = nullptr;

	if (config.type == "SRT") {
		// For SRT, use ffmpeg_muxer
		obs_data_set_string(settings, "path", config.url.toUtf8().constData());
		obs_data_set_string(settings, "url", config.url.toUtf8().constData());
		obs_data_set_string(settings, "format_name", "mpegts");
		obs_data_set_string(settings, "muxer_settings", "");

		QString outName = "srt_output_" + config.id;
		output = obs_output_create("ffmpeg_muxer", outName.toUtf8().constData(), settings, nullptr);
	} else {
		// Default to RTMP
		service_settings = obs_data_create();
		obs_data_set_string(service_settings, "server", config.url.toUtf8().constData());
		obs_data_set_string(service_settings, "key", config.key.toUtf8().constData());

		QString srvName = "rtmp_service_" + config.id;
		service = obs_service_create("rtmp_custom", srvName.toUtf8().constData(), service_settings, nullptr);

		QString outName = "rtmp_output_" + config.id;
		output = obs_output_create("rtmp_output", outName.toUtf8().constData(), nullptr, nullptr);
		if (output && service) {
			obs_output_set_service(output, service);
		}
	}

	obs_data_release(settings);
	if (service_settings)
		obs_data_release(service_settings);

	if (output) {
		signal_handler_t *sh = obs_output_get_signal_handler(output);
		signal_handler_connect(sh, "start", onStart, this);
		signal_handler_connect(sh, "stop", onStop, this);
		signal_handler_connect(sh, "starting", onStarting, this);
		signal_handler_connect(sh, "stopping", onStopping, this);
		signal_handler_connect(sh, "reconnect", onReconnect, this);
		signal_handler_connect(sh, "reconnect_success", onReconnectSuccess, this);
	}
}

void MultistreamTarget::cleanupOutput()
{
	if (output) {
		obs_output_stop(output);
		signal_handler_t *sh = obs_output_get_signal_handler(output);
		signal_handler_disconnect(sh, "start", onStart, this);
		signal_handler_disconnect(sh, "stop", onStop, this);
		signal_handler_disconnect(sh, "starting", onStarting, this);
		signal_handler_disconnect(sh, "stopping", onStopping, this);
		signal_handler_disconnect(sh, "reconnect", onReconnect, this);
		signal_handler_disconnect(sh, "reconnect_success", onReconnectSuccess, this);

		obs_output_release(output);
		output = nullptr;
	}
	if (service) {
		obs_service_release(service);
		service = nullptr;
	}
	if (custom_video_enc) {
		obs_encoder_release(custom_video_enc);
		custom_video_enc = nullptr;
	}
	if (view) {
		if (video_output) {
			obs_view_remove(view);
			video_output = nullptr;
		}
		obs_view_destroy(view);
		view = nullptr;
	}
}

bool MultistreamTarget::setupEncoders()
{
	if (!output)
		return false;

	obs_output_t *main_output = obs_frontend_get_streaming_output();
	if (!main_output)
		return false;

	if (config.isVertical && !config.targetScene.trimmed().isEmpty()) {
		obs_source_t *target_scene = obs_get_source_by_name(config.targetScene.toUtf8().constData());
		if (target_scene) {
			uint32_t width = obs_source_get_width(target_scene);
			uint32_t height = obs_source_get_height(target_scene);

			struct obs_video_info ovi = {0};
			obs_get_video_info(&ovi);

			if (width > height && height > 0) {
				// Horizontal source (e.g. 1920x1080). Crop the center 9:16 column.
				uint32_t target_w = (height * 9) / 16;
				float offset_x = (float)(width - target_w) / 2.0f;

				obs_scene_t *crop_scene = obs_scene_create_private("multistream_crop_scene");
				obs_sceneitem_t *item = obs_scene_add(crop_scene, target_scene);
				
				// Force Top-Left alignment so positioning is absolute
				obs_sceneitem_set_alignment(item, OBS_ALIGN_TOP | OBS_ALIGN_LEFT);
				
				struct vec2 pos = {-offset_x, 0.0f};
				obs_sceneitem_set_pos(item, &pos);
				
				obs_source_t *crop_source = obs_scene_get_source(crop_scene);

				view = obs_view_create();
				obs_view_set_source(view, 0, crop_source);
				
				obs_source_release(target_scene);
				obs_scene_release(crop_scene);

				ovi.base_width = target_w;
				ovi.base_height = height;
			} else {
				// Already vertical or square, no cropping needed
				view = obs_view_create();
				obs_view_set_source(view, 0, target_scene);
				obs_source_release(target_scene);

				ovi.base_width = width > 0 ? width : 1080;
				ovi.base_height = height > 0 ? height : 1920;
			}

			ovi.output_width = 1080;
			ovi.output_height = 1920;

			video_output = obs_view_add2(view, &ovi);
			if (video_output) {
				obs_encoder_t *main_vid = obs_output_get_video_encoder(main_output);
				if (main_vid) {
					obs_data_t *main_settings = obs_encoder_get_settings(main_vid);
					const char *enc_id = obs_encoder_get_id(main_vid);
					
					custom_video_enc = obs_video_encoder_create(enc_id, "multistream_vert_enc", main_settings, nullptr);
					obs_data_release(main_settings);

					if (custom_video_enc) {
						obs_encoder_set_video(custom_video_enc, video_output);
						obs_output_set_video_encoder(output, custom_video_enc);
					}
				}
			}
		} else {
			blog(LOG_WARNING, "Multistream: Target scene %s not found for vertical stream", config.targetScene.toUtf8().constData());
		}
	}

	// Fallback or normal clone for video if not setup
	if (!custom_video_enc) {
		obs_encoder_t *video_enc = obs_output_get_video_encoder(main_output);
		if (video_enc) {
			obs_output_set_video_encoder(output, video_enc);
		}
	}

	// Clone audio encoders (up to max audio mixes)
	for (size_t i = 0; i < MAX_AUDIO_MIXES; i++) {
		obs_encoder_t *audio_enc = obs_output_get_audio_encoder(main_output, i);
		if (audio_enc) {
			obs_output_set_audio_encoder(output, audio_enc, i);
		}
	}

	obs_output_release(main_output);
	return true;
}

QJsonObject MultistreamTarget::getMetrics() const
{
	QJsonObject metrics;
	if (output && status != STOPPED) {
		metrics["bytes"] = (qint64)obs_output_get_total_bytes(output);
		metrics["frames_dropped"] = obs_output_get_frames_dropped(output);
		metrics["total_frames"] = obs_output_get_total_frames(output);
		metrics["congestion"] = obs_output_get_congestion(output);
	} else {
		metrics["bytes"] = 0;
		metrics["frames_dropped"] = 0;
		metrics["total_frames"] = 0;
		metrics["congestion"] = 0.0;
	}
	return metrics;
}

void MultistreamTarget::start()
{
	if (status != STOPPED)
		return;

	initOutput();
	if (!setupEncoders()) {
		// Could not setup encoders
		setStatus(STOPPED);
		return;
	}

	obs_output_start(output);
}

void MultistreamTarget::stop()
{
	if (output) {
		obs_output_stop(output);
	}
}

void MultistreamTarget::updateConfig(const MultistreamTargetConfig &newConfig)
{
	config = newConfig;
	if (status == STOPPED) {
		cleanupOutput();
	}
}

void MultistreamTarget::setStatus(Status newStatus)
{
	if (status != newStatus) {
		status = newStatus;
		emit statusChanged(config.id, status);
	}
}

void MultistreamTarget::onStart(void *data, calldata_t *)
{
	auto target = static_cast<MultistreamTarget *>(data);
	target->setStatus(STREAMING);
}

void MultistreamTarget::onStop(void *data, calldata_t *)
{
	auto target = static_cast<MultistreamTarget *>(data);
	target->setStatus(STOPPED);
}

void MultistreamTarget::onStarting(void *data, calldata_t *)
{
	auto target = static_cast<MultistreamTarget *>(data);
	target->setStatus(STARTING);
}

void MultistreamTarget::onStopping(void *data, calldata_t *)
{
	auto target = static_cast<MultistreamTarget *>(data);
	target->setStatus(STOPPING);
}

void MultistreamTarget::onReconnect(void *data, calldata_t *)
{
	auto target = static_cast<MultistreamTarget *>(data);
	target->setStatus(RECONNECTING);
}

void MultistreamTarget::onReconnectSuccess(void *data, calldata_t *)
{
	auto target = static_cast<MultistreamTarget *>(data);
	target->setStatus(STREAMING);
}

MultistreamManager &MultistreamManager::instance()
{
	static MultistreamManager inst;
	return inst;
}

MultistreamManager::MultistreamManager(QObject *parent) : QObject(parent)
{
	obs_frontend_add_event_callback(obsFrontendEvent, this);
}

MultistreamManager::~MultistreamManager()
{
	obs_frontend_remove_event_callback(obsFrontendEvent, this);
	for (auto t : targets) {
		delete t;
	}
	targets.clear();
}

void MultistreamManager::loadConfig()
{
	config_t *global_config = obs_frontend_get_profile_config();
	if (!global_config)
		return;

	syncWithObs = config_get_bool(global_config, "SRTLA_Multistream", "SyncWithObs");

	const char *targetsJson = config_get_string(global_config, "SRTLA_Multistream", "Targets");
	if (targetsJson && *targetsJson) {
		QJsonDocument doc = QJsonDocument::fromJson(QByteArray(targetsJson));
		if (doc.isArray()) {
			for (auto t : targets) {
				delete t;
			}
			targets.clear();

			QJsonArray arr = doc.array();
			for (int i = 0; i < arr.size(); i++) {
				MultistreamTargetConfig cfg = MultistreamTargetConfig::fromJson(arr[i].toObject());
				MultistreamTarget *target = new MultistreamTarget(cfg, this);
				connect(target, &MultistreamTarget::statusChanged, this,
					&MultistreamManager::targetStatusChanged);
				targets.append(target);
			}
			emit targetsChanged();
		}
	}
}

void MultistreamManager::saveConfig()
{
	config_t *global_config = obs_frontend_get_profile_config();
	if (!global_config)
		return;

	config_set_bool(global_config, "SRTLA_Multistream", "SyncWithObs", syncWithObs);

	QJsonArray arr;
	for (auto t : targets) {
		arr.append(t->getConfig().toJson());
	}
	QJsonDocument doc(arr);
	config_set_string(global_config, "SRTLA_Multistream", "Targets",
			  doc.toJson(QJsonDocument::Compact).constData());

	config_save_safe(global_config, "tmp", nullptr);
}

void MultistreamManager::setSyncWithObs(bool sync)
{
	syncWithObs = sync;
	saveConfig();
}


MultistreamTarget *MultistreamManager::getTarget(const QString &id) const
{
	for (auto t : targets) {
		if (t->getConfig().id == id)
			return t;
	}
	return nullptr;
}

void MultistreamManager::addTarget(const MultistreamTargetConfig &config)
{
	MultistreamTargetConfig cfg = config;
	if (cfg.id.isEmpty()) {
		cfg.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
	}
	MultistreamTarget *target = new MultistreamTarget(cfg, this);
	connect(target, &MultistreamTarget::statusChanged, this, &MultistreamManager::targetStatusChanged);
	targets.append(target);
	saveConfig();
	emit targetsChanged();
}

void MultistreamManager::updateTarget(const QString &id, const MultistreamTargetConfig &config)
{
	for (auto t : targets) {
		if (t->getConfig().id == id) {
			t->updateConfig(config);
			saveConfig();
			emit targetsChanged();
			break;
		}
	}
}

void MultistreamManager::deleteTarget(const QString &id)
{
	for (int i = 0; i < targets.size(); i++) {
		if (targets[i]->getConfig().id == id) {
			MultistreamTarget *t = targets.takeAt(i);
			delete t;
			saveConfig();
			emit targetsChanged();
			break;
		}
	}
}

void MultistreamManager::startAll()
{
	for (auto t : targets) {
		if (t->getConfig().enabled) {
			t->start();
		}
	}
}

void MultistreamManager::stopAll()
{
	for (auto t : targets) {
		t->stop();
	}
}

void MultistreamManager::obsFrontendEvent(enum obs_frontend_event event, void *private_data)
{
	auto mgr = static_cast<MultistreamManager *>(private_data);
	if (event == OBS_FRONTEND_EVENT_STREAMING_STARTED) {
		if (mgr->getSyncWithObs()) {
			mgr->startAll();
		}
	} else if (event == OBS_FRONTEND_EVENT_STREAMING_STOPPED) {
		if (mgr->getSyncWithObs()) {
			mgr->stopAll();
		}
	}
}
