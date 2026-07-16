#include "srtla-web.hpp"
#include "httplib.h"
#include <util/config-file.h>
#include <obs-frontend-api.h>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <QProcess>
#include <QCoreApplication>
#include <QMetaObject>
#include <atomic>
#include "srtla-ui.hpp"
#include "multistream.hpp"
#include <QJsonArray>
extern "C" {
void srtla_proxy_settings_changed();
void srtla_get_all_receivers_json(char *out_buffer, int max_len);
void srtla_get_connection_details(int *listen_port, int *failed_conns, char *out_buffer, int max_len);
void srtla_force_start_by_name(const char *name);
void srtla_force_stop_by_name(const char *name);
void srtla_force_restart_by_name(const char *name);
}

static httplib::Server *svr = nullptr;
static std::thread *server_thread = nullptr;
static std::atomic<bool> is_running(false);

static bool check_auth(const httplib::Request &req, httplib::Response &res)
{
	config_t *global_config = obs_frontend_get_profile_config();
	if (!global_config)
		return true;
	const char *pwd = config_get_string(global_config, "SRTLA", "WebAccessPassword");
	if (!pwd || strlen(pwd) == 0)
		return true;

	if (req.has_header("Cookie")) {
		std::string cookie_str = req.get_header_value("Cookie");
		QString qstr = QString::fromStdString(cookie_str);
		QStringList cookies = qstr.split(';', Qt::SkipEmptyParts);
		for (const QString &cookie : cookies) {
			QString trimmed = cookie.trimmed();
			if (trimmed.startsWith("auth_token=")) {
				QString token = trimmed.mid(11);
				if (token == QString(pwd))
					return true;
			}
		}
	}
	res.status = 401;
	res.set_content("{\"status\":\"unauthorized\"}", "application/json");
	return false;
}

#define REQUIRE_AUTH(req, res) if (!check_auth(req, res)) return;

static void handle_api_auth(const httplib::Request &req, httplib::Response &res)
{
	config_t *global_config = obs_frontend_get_profile_config();
	const char *pwd = global_config ? config_get_string(global_config, "SRTLA", "WebAccessPassword") : nullptr;
	if (!pwd || strlen(pwd) == 0) {
		res.set_content("{\"status\":\"ok\"}", "application/json");
		return;
	}

	QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(req.body));
	if (doc.isObject()) {
		QString attempt = doc.object()["password"].toString();
		if (attempt == QString(pwd)) {
			std::string cookie =
				"auth_token=" + attempt.toStdString() + "; Path=/; Max-Age=31536000; SameSite=Strict";
			res.set_header("Set-Cookie", cookie.c_str());
			res.set_content("{\"status\":\"ok\"}", "application/json");
			return;
		}
	}
	res.status = 401;
	res.set_content("{\"status\":\"unauthorized\"}", "application/json");
}

static void handle_api_logout(const httplib::Request &, httplib::Response &res)
{
	std::string cookie = "auth_token=; Path=/; Max-Age=0; SameSite=Strict";
	res.set_header("Set-Cookie", cookie.c_str());
	res.set_content("{\"status\":\"ok\"}", "application/json");
}

static void handle_api_settings_get(const httplib::Request &req, httplib::Response &res)
{
	REQUIRE_AUTH(req, res)
	config_t *global_config = obs_frontend_get_profile_config();
	QJsonObject obj;
	if (global_config) {
		obj["proxy_enabled"] = config_get_bool(global_config, "SRTLA_Proxy", "Enabled");
		obj["autoswitch_enabled"] = config_get_bool(global_config, "SRTLA_AutoSwitch", "Enabled");
		obj["vis_autoswitch_enabled"] = config_get_bool(global_config, "SRTLA_AutoSwitch", "VisEnabled");
		const char *pwd = config_get_string(global_config, "SRTLA", "WSPassword");
		obj["ws_password"] = pwd ? QString(pwd) : "";

		const char *wpwd = config_get_string(global_config, "SRTLA", "WebAccessPassword");
		obj["web_access_password"] = wpwd ? QString(wpwd) : "";

		obj["sync_with_obs_live"] = MultistreamManager::instance().getSyncWithObs();
	}
	QJsonDocument doc(obj);
	res.set_content(doc.toJson(QJsonDocument::Compact).toStdString(), "application/json");
}

static void handle_api_settings_post(const httplib::Request &req, httplib::Response &res)
{
	REQUIRE_AUTH(req, res)
	config_t *global_config = obs_frontend_get_profile_config();
	if (!global_config) {
		res.status = 500;
		return;
	}

	QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(req.body));
	if (doc.isObject()) {
		QJsonObject obj = doc.object();
		if (obj.contains("proxy_enabled"))
			config_set_bool(global_config, "SRTLA_Proxy", "Enabled", obj["proxy_enabled"].toBool());
		if (obj.contains("autoswitch_enabled"))
			config_set_bool(global_config, "SRTLA_AutoSwitch", "Enabled",
					obj["autoswitch_enabled"].toBool());
		if (obj.contains("vis_autoswitch_enabled"))
			config_set_bool(global_config, "SRTLA_AutoSwitch", "VisEnabled",
					obj["vis_autoswitch_enabled"].toBool());
		if (obj.contains("ws_password"))
			config_set_string(global_config, "SRTLA", "WSPassword",
					  obj["ws_password"].toString().toUtf8().constData());
		if (obj.contains("web_access_password"))
			config_set_string(global_config, "SRTLA", "WebAccessPassword",
					  obj["web_access_password"].toString().toUtf8().constData());

		if (obj.contains("sync_with_obs_live"))
			MultistreamManager::instance().setSyncWithObs(obj["sync_with_obs_live"].toBool());

		config_save_safe(global_config, "tmp", nullptr);
		srtla_proxy_settings_changed();

		res.status = 200;
		res.set_content("{\"status\":\"ok\"}", "application/json");
	} else {
		res.status = 400;
	}
}

static void handle_api_obs_ws_config(const httplib::Request &req, httplib::Response &res)
{
	REQUIRE_AUTH(req, res)
	config_t *profile_config = obs_frontend_get_profile_config();
	QJsonObject obj;
	if (profile_config) {
		obj["ws_enabled"] = config_get_bool(profile_config, "OBSWebSocket", "ServerEnabled");
		const char *pwd = config_get_string(profile_config, "OBSWebSocket", "ServerPassword");
		obj["ws_password"] = pwd ? QString(pwd) : "";
		obj["ws_port"] = static_cast<int>(config_get_int(profile_config, "OBSWebSocket", "ServerPort"));
	}
	QJsonDocument doc(obj);
	res.set_content(doc.toJson(QJsonDocument::Compact).toStdString(), "application/json");
}

#include <QWidget>
#include <QDir>
#include <QTemporaryFile>
#include <QTextStream>
#include <QCoreApplication>

static void handle_api_restart(const httplib::Request &req, httplib::Response &res)
{
	REQUIRE_AUTH(req, res)
	res.set_content("{\"status\":\"restarting\"}", "application/json");
	QMetaObject::invokeMethod(
		qApp,
		[]() {
			QString path = QDir::toNativeSeparators(qApp->applicationFilePath());
			QString workDir = QDir::toNativeSeparators(qApp->applicationDirPath());
			qint64 pid = QCoreApplication::applicationPid();

#ifdef _WIN32
			QString batPath = QDir::toNativeSeparators(QDir::tempPath() + QDir::separator() +
								   "obs_pyleirl_restart.bat");
			QFile file(batPath);
			if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
				QTextStream out(&file);
				out << "@echo off\n";
				out << "set PID=" << pid << "\n";
				out << "set count=0\n";
				out << ":loop\n";
				out << "tasklist /FI \"PID eq %PID%\" 2>NUL | find \"%PID%\" >NUL\n";
				out << "if errorlevel 1 goto launch\n";
				out << "timeout /t 1 >nul\n";
				out << "set /a count+=1\n";
				out << "if %count% GTR 10 (\n";
				out << "    taskkill /PID %PID% /F >nul\n";
				out << "    goto launch\n";
				out << ")\n";
				out << "goto loop\n";
				out << ":launch\n";
				out << "cd /d \"" << workDir << "\"\n";
				out << "start \"\" \"" << path << "\"\n";
				out << "del \"%~f0\"\n";
				file.close();

				QProcess::startDetached("cmd.exe", QStringList() << "/c" << batPath);
			}
#else
			QString shPath = QDir::tempPath() + QDir::separator() + "obs_pyleirl_restart.sh";
			QFile file(shPath);
			if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
				QTextStream out(&file);
				out << "#!/bin/sh\n";
				out << "PID=" << pid << "\n";
				out << "COUNT=0\n";
				out << "while kill -0 $PID 2>/dev/null; do\n";
				out << "  sleep 1\n";
				out << "  COUNT=$((COUNT+1))\n";
				out << "  if [ $COUNT -gt 10 ]; then\n";
				out << "    kill -9 $PID 2>/dev/null\n";
				out << "    break\n";
				out << "  fi\n";
				out << "done\n";
				out << "cd \"" << workDir << "\"\n";
				out << "nohup \"" << path << "\" >/dev/null 2>&1 &\n";
				out << "rm -f \"$0\"\n";
				file.close();

				QFile::setPermissions(shPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
								      QFileDevice::ExeOwner | QFileDevice::ReadGroup |
								      QFileDevice::ExeGroup | QFileDevice::ReadOther |
								      QFileDevice::ExeOther);
				QProcess::startDetached("/bin/sh", QStringList() << shPath);
			}
#endif

			// Graceful exit via Main Window closeEvent
			QWidget *mainWindow = (QWidget *)obs_frontend_get_main_window();
			if (mainWindow) {
				mainWindow->close();
			} else {
				qApp->quit();
			}
		},
		Qt::QueuedConnection);
}

static void handle_api_stream_key_get(const httplib::Request &req, httplib::Response &res)
{
	REQUIRE_AUTH(req, res)
	obs_service_t *service = obs_frontend_get_streaming_service();
	QJsonObject obj;
	obj["key"] = "";
	obj["server"] = "";
	if (service) {
		obs_data_t *settings = obs_service_get_settings(service);
		const char *key = obs_data_get_string(settings, "key");
		const char *server = obs_data_get_string(settings, "server");
		if (key)
			obj["key"] = QString(key);
		if (server)
			obj["server"] = QString(server);
		obs_data_release(settings);
	}
	QJsonDocument doc(obj);
	res.set_content(doc.toJson(QJsonDocument::Compact).toStdString(), "application/json");
}

static void handle_api_stream_key_post(const httplib::Request &req, httplib::Response &res)
{
	REQUIRE_AUTH(req, res)
	QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(req.body));
	if (doc.isObject()) {
		QString newKey = doc.object()["key"].toString();
		QString newServer = doc.object()["server"].toString();
		obs_service_t *service = obs_frontend_get_streaming_service();
		if (service) {
			obs_data_t *settings = obs_service_get_settings(service);
			obs_data_set_string(settings, "key", newKey.toUtf8().constData());
			obs_data_set_string(settings, "server", newServer.toUtf8().constData());
			obs_service_update(service, settings);
			obs_data_release(settings);
			obs_frontend_save_streaming_service();
			res.set_content("{\"status\":\"ok\"}", "application/json");
			return;
		}
	}
	res.status = 400;
}

static void handle_api_autoswitch_get(const httplib::Request &req, httplib::Response &res)
{
	REQUIRE_AUTH(req, res)
	config_t *global_config = obs_frontend_get_profile_config();
	QJsonObject obj;
	if (global_config) {
		const char *rules = config_get_string(global_config, "SRTLA_AutoSwitch", "RulesJSON");
		const char *visRules = config_get_string(global_config, "SRTLA_AutoSwitch", "VisibilityRulesJSON");
		obj["rules"] = rules ? QString(rules) : "[]";
		obj["visibility_rules"] = visRules ? QString(visRules) : "[]";
		obj["delay"] = static_cast<int>(config_get_int(global_config, "SRTLA_AutoSwitch", "Delay"));
		obj["vis_delay"] = static_cast<int>(config_get_int(global_config, "SRTLA_AutoSwitch", "VisDelay"));
	}
	QJsonDocument doc(obj);
	res.set_content(doc.toJson(QJsonDocument::Compact).toStdString(), "application/json");
}

static void handle_api_autoswitch_post(const httplib::Request &req, httplib::Response &res)
{
	REQUIRE_AUTH(req, res)
	config_t *global_config = obs_frontend_get_profile_config();
	if (!global_config) {
		res.status = 500;
		return;
	}

	QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(req.body));
	if (doc.isObject()) {
		QJsonObject obj = doc.object();
		if (obj.contains("rules"))
			config_set_string(global_config, "SRTLA_AutoSwitch", "RulesJSON",
					  obj["rules"].toString().toUtf8().constData());
		if (obj.contains("visibility_rules"))
			config_set_string(global_config, "SRTLA_AutoSwitch", "VisibilityRulesJSON",
					  obj["visibility_rules"].toString().toUtf8().constData());
		if (obj.contains("delay"))
			config_set_int(global_config, "SRTLA_AutoSwitch", "Delay", obj["delay"].toInt());
		if (obj.contains("vis_delay"))
			config_set_int(global_config, "SRTLA_AutoSwitch", "VisDelay", obj["vis_delay"].toInt());
		config_save_safe(global_config, "tmp", nullptr);

		QMetaObject::invokeMethod(
			qApp, []() { SrtlaAutoSwitcher::instance().reloadRules(); }, Qt::QueuedConnection);

		res.set_content("{\"status\":\"ok\"}", "application/json");
		return;
	}
	res.status = 400;
}

static void handle_api_receivers(const httplib::Request &req, httplib::Response &res)
{
	REQUIRE_AUTH(req, res)
	char buf[4096] = {0};
	srtla_get_all_receivers_json(buf, sizeof(buf));
	res.set_content(buf, "application/json");
}

static void handle_api_stats(const httplib::Request &req, httplib::Response &res)
{
	REQUIRE_AUTH(req, res)
	int listen_port = 0, failed = 0;
	char buf[4096] = {0};
	srtla_get_connection_details(&listen_port, &failed, buf, sizeof(buf));
	if (strlen(buf) == 0)
		strcpy(buf, "{}");
	res.set_content(buf, "application/json");
}

static void handle_api_receiver_action(const httplib::Request &req, httplib::Response &res)
{
	REQUIRE_AUTH(req, res)
	QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(req.body));
	if (doc.isObject()) {
		QString action = doc.object()["action"].toString();
		QString name = doc.object()["name"].toString();
		QByteArray nameBA = name.toUtf8();

		if (action == "start")
			srtla_force_start_by_name(nameBA.constData());
		else if (action == "stop")
			srtla_force_stop_by_name(nameBA.constData());
		else if (action == "restart")
			srtla_force_restart_by_name(nameBA.constData());

		res.set_content("{\"status\":\"ok\"}", "application/json");
		return;
	}
	res.status = 400;
}

static void handle_api_multistream_targets(const httplib::Request &req, httplib::Response &res)
{
	REQUIRE_AUTH(req, res)
	QJsonArray arr;
	auto targets = MultistreamManager::instance().getTargets();
	for (auto t : targets) {
		QJsonObject obj = t->getConfig().toJson();
		int s = t->getStatus();
		QString statusStr = "Stopped";
		if (s == MultistreamTarget::STARTING)
			statusStr = "Starting";
		else if (s == MultistreamTarget::STREAMING)
			statusStr = "Streaming";
		else if (s == MultistreamTarget::STOPPING)
			statusStr = "Stopping";
		else if (s == MultistreamTarget::RECONNECTING)
			statusStr = "Reconnecting";
		obj["status_str"] = statusStr;

		QJsonObject metrics = t->getMetrics();
		obj["metrics"] = metrics;

		arr.append(obj);
	}
	QJsonDocument doc(arr);
	res.set_content(doc.toJson(QJsonDocument::Compact).toStdString(), "application/json");
}

static void handle_api_multistream_action(const httplib::Request &req, httplib::Response &res)
{
	REQUIRE_AUTH(req, res)
	QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(req.body));
	if (doc.isObject()) {
		QString action = doc.object()["action"].toString();
		QString id = doc.object()["id"].toString();

		auto t = MultistreamManager::instance().getTarget(id);
		if (t) {
			QMetaObject::invokeMethod(
				qApp,
				[id, action]() {
					auto target = MultistreamManager::instance().getTarget(id);
					if (target) {
						if (action == "start")
							target->start();
						else if (action == "stop")
							target->stop();
					}
				},
				Qt::QueuedConnection);
		}
		res.set_content("{\"status\":\"ok\"}", "application/json");
		return;
	}
	res.status = 400;
}

static void handle_api_multistream_manage(const httplib::Request &req, httplib::Response &res)
{
	REQUIRE_AUTH(req, res)
	QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(req.body));
	if (doc.isObject()) {
		QString action = doc.object()["action"].toString();
		QJsonObject payload = doc.object()["target"].toObject();

		QMetaObject::invokeMethod(
			qApp,
			[action, payload]() {
				if (action == "add") {
					MultistreamTargetConfig cfg = MultistreamTargetConfig::fromJson(payload);
					MultistreamManager::instance().addTarget(cfg);
				} else if (action == "edit") {
					QString id = payload["id"].toString();
					MultistreamTargetConfig cfg = MultistreamTargetConfig::fromJson(payload);
					MultistreamManager::instance().updateTarget(id, cfg);
				} else if (action == "delete") {
					QString id = payload["id"].toString();
					MultistreamManager::instance().deleteTarget(id);
				}
			},
			Qt::QueuedConnection);

		res.set_content("{\"status\":\"ok\"}", "application/json");
		return;
	}
	res.status = 400;
}

void srtla_web_server_start(int port)
{
	if (is_running)
		return;

	svr = new httplib::Server();

	svr->Get("/", [](const httplib::Request &, httplib::Response &res) {
		QFile file(":/web/index.html");
		if (file.open(QIODevice::ReadOnly)) {
			res.set_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
			res.set_header("Pragma", "no-cache");
			res.set_content(file.readAll().toStdString(), "text/html");
		} else {
			res.status = 404;
			res.set_content("Not Found", "text/plain");
		}
	});

	svr->Get("/api/settings", handle_api_settings_get);
	svr->Post("/api/settings", handle_api_settings_post);
	svr->Get("/api/obs_ws_config", handle_api_obs_ws_config);
	svr->Post("/api/restart", handle_api_restart);
	svr->Get("/api/stream_key", handle_api_stream_key_get);
	svr->Post("/api/stream_key", handle_api_stream_key_post);
	svr->Get("/api/autoswitch", handle_api_autoswitch_get);
	svr->Post("/api/autoswitch", handle_api_autoswitch_post);
	svr->Get("/api/receivers", handle_api_receivers);
	svr->Get("/api/stats", handle_api_stats);
	svr->Post("/api/receivers/action", handle_api_receiver_action);
	svr->Get("/api/multistream/targets", handle_api_multistream_targets);
	svr->Post("/api/multistream/action", handle_api_multistream_action);
	svr->Post("/api/multistream/manage", handle_api_multistream_manage);
	svr->Post("/api/auth", handle_api_auth);
	svr->Post("/api/logout", handle_api_logout);

	is_running = true;
	server_thread = new std::thread([port]() { svr->listen("0.0.0.0", port); });
}

void srtla_web_server_stop()
{
	if (!is_running || !svr)
		return;

	svr->stop();
	if (server_thread && server_thread->joinable()) {
		server_thread->join();
	}

	delete svr;
	svr = nullptr;
	delete server_thread;
	server_thread = nullptr;
	is_running = false;
}

bool srtla_web_server_is_running()
{
	return is_running;
}
