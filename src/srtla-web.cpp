#include "srtla-web.hpp"
#include "httplib.h"
#include <util/config-file.h>
#include <obs-frontend-api.h>
#include <obs.h>
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
#include <QTcpSocket>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QStandardPaths>
extern "C" {
void srtla_proxy_settings_changed();
void srtla_get_connection_stats(bool *is_listening, int *active_groups, int *active_connections);
void srtla_get_connection_details(int *listen_port, int *failed_conns, char *out_buffer, int max_len);
void srtla_get_all_receivers_json(char *out_buffer, int max_len);
void srtla_force_start_by_name(const char *name);
void srtla_force_stop_by_name(const char *name);
void srtla_force_restart_by_name(const char *name);
void srtla_force_restart_all();

void rist_get_connection_stats(bool *is_listening, int *active_groups, int *active_connections);
void rist_get_connection_details(char *out_buffer, int max_len);
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

		const char *wsurl = config_get_string(global_config, "SRTLA", "WSUrl");
		obj["ws_url_override"] = wsurl ? QString(wsurl) : "";

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

		if (obj.contains("ws_url_override"))
			config_set_string(global_config, "SRTLA", "WSUrl",
					  obj["ws_url_override"].toString().toUtf8().constData());

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
static QString fetch_obs_websocket_screenshot(const QString &sourceName)
{
    int port = 0;
    QString password = "";
    
    // Try obs-websocket config.json directly (OBS 30+)
    QString configPath = qEnvironmentVariable("APPDATA") + "/obs-studio/plugin_config/obs-websocket/config.json";
    blog(LOG_INFO, "[PyleIRL Web] Checking OBS WebSocket config at: %s", configPath.toUtf8().constData());
    QFile file(configPath);
    if (file.open(QIODevice::ReadOnly)) {
        QByteArray raw = file.readAll();
        blog(LOG_INFO, "[PyleIRL Web] Raw WebSocket config JSON: %s", raw.constData());
        QJsonObject obj = QJsonDocument::fromJson(raw).object();
        
        if (obj.contains("ServerPort")) port = obj["ServerPort"].toInt();
        else if (obj.contains("server_port")) port = obj["server_port"].toInt();
        else if (obj.contains("serverPort")) port = obj["serverPort"].toInt();

        if (obj.contains("ServerPassword")) password = obj["ServerPassword"].toString();
        else if (obj.contains("server_password")) password = obj["server_password"].toString();
        else if (obj.contains("serverPassword")) password = obj["serverPassword"].toString();

        blog(LOG_INFO, "[PyleIRL Web] Parsed WebSocket config: Port=%d", port);
    } else {
        blog(LOG_WARNING, "[PyleIRL Web] Could not open WebSocket config file");
    }
    
    // Try global_config (OBS 30 earlier)
    if (port <= 0) {
        config_t *global_config = obs_frontend_get_global_config();
        if (global_config) {
            port = config_get_int(global_config, "OBSWebSocket", "ServerPort");
            const char *pwd = config_get_string(global_config, "OBSWebSocket", "ServerPassword");
            if (pwd && strlen(pwd) > 0) password = QString(pwd);
        }
    }
    
    // Try profile_config (OBS 28)
    if (port <= 0) {
        config_t *profile_config = obs_frontend_get_profile_config();
        if (profile_config) {
            port = config_get_int(profile_config, "OBSWebSocket", "ServerPort");
            const char *pwd = config_get_string(profile_config, "OBSWebSocket", "ServerPassword");
            if (pwd && strlen(pwd) > 0) password = QString(pwd);
        }
    }

    if (port <= 0) port = 4455;

    QTcpSocket socket;
    socket.connectToHost("127.0.0.1", port);
    if (!socket.waitForConnected(2000)) {
        blog(LOG_WARNING, "[PyleIRL Web] Failed to connect to WebSocket on port %d", port);
        return "";
    }

    QString handshake = QString("GET / HTTP/1.1\r\n"
                                "Host: 127.0.0.1:%1\r\n"
                                "Upgrade: websocket\r\n"
                                "Connection: Upgrade\r\n"
                                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                "Sec-WebSocket-Version: 13\r\n\r\n").arg(port);
    socket.write(handshake.toUtf8());
    socket.waitForBytesWritten(1000);

    QByteArray buffer;
    QElapsedTimer timer;
    timer.start();
    bool handshake_done = false;

    auto read_frame = [&](QJsonObject &outJson, const char* step) -> bool {
        while (timer.elapsed() < 3000) {
            if (socket.bytesAvailable() > 0) buffer.append(socket.readAll());
            if (!handshake_done) {
                int endIdx = buffer.indexOf("\r\n\r\n");
                if (endIdx != -1) {
                    buffer.remove(0, endIdx + 4);
                    handshake_done = true;
                }
            }
            if (handshake_done && buffer.size() >= 2) {
                uint8_t payload_len = buffer[1] & 0x7F;
                int header_size = 2;
                uint64_t actual_len = payload_len;
                if (payload_len == 126) {
                    if (buffer.size() < 4) goto wait_more;
                    header_size = 4;
                    actual_len = ((uint8_t)buffer[2] << 8) | (uint8_t)buffer[3];
                } else if (payload_len == 127) {
                    if (buffer.size() < 10) goto wait_more;
                    header_size = 10;
                    actual_len = 0;
                    for (int i=0; i<8; i++) actual_len = (actual_len << 8) | (uint8_t)buffer[2+i];
                }
                if (buffer.size() >= header_size + (int)actual_len) {
                    QByteArray payload = buffer.mid(header_size, actual_len);
                    buffer.remove(0, header_size + actual_len);
                    outJson = QJsonDocument::fromJson(payload).object();
                    return true;
                }
            }
        wait_more:
            socket.waitForReadyRead(100);
        }
        blog(LOG_WARNING, "[PyleIRL Web] Timeout reading frame during: %s", step);
        return false;
    };

    QJsonObject helloPayload;
    if (!read_frame(helloPayload, "Hello")) return "";

    QJsonObject identifyPayload;
    identifyPayload["op"] = 1;
    QJsonObject dObj;
    dObj["rpcVersion"] = 1;

    if (helloPayload.contains("d") && helloPayload["d"].toObject().contains("authentication") && !password.isEmpty()) {
        QJsonObject authObj = helloPayload["d"].toObject()["authentication"].toObject();
        QString challenge = authObj["challenge"].toString();
        QString salt = authObj["salt"].toString();

        QByteArray secret = QCryptographicHash::hash((password + salt).toUtf8(), QCryptographicHash::Sha256).toBase64();
        QByteArray authResponse = QCryptographicHash::hash((secret + challenge.toUtf8()), QCryptographicHash::Sha256).toBase64();
        dObj["authentication"] = QString(authResponse);
    }
    identifyPayload["d"] = dObj;

    auto send_frame = [&](const QJsonObject &obj) {
        QByteArray payload = QJsonDocument(obj).toJson(QJsonDocument::Compact);
        QByteArray frame;
        frame.append((char)0x81);
        if (payload.size() < 126) {
            frame.append((char)(payload.size() | 0x80));
        } else if (payload.size() <= 65535) {
            frame.append((char)(126 | 0x80));
            frame.append((char)((payload.size() >> 8) & 0xFF));
            frame.append((char)(payload.size() & 0xFF));
        } else {
            frame.append((char)(127 | 0x80));
            for (int i=7; i>=0; i--) frame.append((char)((payload.size() >> (i*8)) & 0xFF));
        }
        QByteArray mask = "ABCD";
        frame.append(mask);
        for (int i=0; i<payload.size(); i++) frame.append((char)(payload[i] ^ mask[i % 4]));
        socket.write(frame);
        socket.waitForBytesWritten(1000);
    };

    send_frame(identifyPayload);

    QJsonObject identifiedPayload;
    if (!read_frame(identifiedPayload, "Identified")) return "";
    if (identifiedPayload["op"].toInt() != 2) {
        blog(LOG_WARNING, "[PyleIRL Web] Auth failed or unexpected op: %d", identifiedPayload["op"].toInt());
        return "";
    }

    QJsonObject reqPayload;
    reqPayload["op"] = 6;
    QJsonObject reqD;
    reqD["requestType"] = "GetSourceScreenshot";
    reqD["requestId"] = "req_1";
    QJsonObject reqData;
    reqData["sourceName"] = sourceName;
    reqData["imageFormat"] = "jpeg";
    reqData["imageWidth"] = 640;
    reqData["imageHeight"] = 360;
    reqData["imageCompressionQuality"] = 45;
    reqD["requestData"] = reqData;
    reqPayload["d"] = reqD;
    
    send_frame(reqPayload);

    QJsonObject resPayload;
    if (!read_frame(resPayload, "Response")) return "";

    if (resPayload["op"].toInt() == 7) {
        QJsonObject d = resPayload["d"].toObject();
        QJsonObject status = d["requestStatus"].toObject();
        if (status["result"].toBool()) {
            return d["responseData"].toObject()["imageData"].toString();
        } else {
            blog(LOG_WARNING, "[PyleIRL Web] GetSourceScreenshot failed: %s", status["code"].toVariant().toString().toUtf8().constData());
        }
    } else {
        blog(LOG_WARNING, "[PyleIRL Web] Unexpected response op: %d", resPayload["op"].toInt());
    }
    return "";
}

static void handle_api_obs_screenshot(const httplib::Request &req, httplib::Response &res)
{
    REQUIRE_AUTH(req, res)
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(req.body));
    QString sourceName = doc.object()["sourceName"].toString();
    QString img = fetch_obs_websocket_screenshot(sourceName);

    QJsonObject obj;
    if (!img.isEmpty()) obj["imageData"] = img;
    res.set_content(QJsonDocument(obj).toJson(QJsonDocument::Compact).toStdString(), "application/json");
}

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
		const char *noFailover = config_get_string(global_config, "SRTLA_AutoSwitch", "NoFailoverScenes");
		const char *visRules = config_get_string(global_config, "SRTLA_AutoSwitch", "VisibilityRulesJSON");
		const char *volRules = config_get_string(global_config, "SRTLA_AutoSwitch", "VolumeRulesJSON");
		obj["rules"] = rules ? QString(rules) : "[]";
		obj["no_failover_scenes"] = noFailover ? QString(noFailover) : "[]";
		obj["visibility_rules"] = visRules ? QString(visRules) : "[]";
		obj["volume_rules"] = volRules ? QString(volRules) : "[]";
		obj["delay"] = static_cast<int>(config_get_int(global_config, "SRTLA_AutoSwitch", "Delay"));
		obj["vis_delay"] = static_cast<int>(config_get_int(global_config, "SRTLA_AutoSwitch", "VisDelay"));
		obj["vol_enabled"] = config_get_bool(global_config, "SRTLA_AutoSwitch", "VolEnabled");
		obj["vol_delay"] = static_cast<int>(config_get_int(global_config, "SRTLA_AutoSwitch", "VolDelay"));
	}
	QJsonDocument doc(obj);
	res.set_content(doc.toJson(QJsonDocument::Compact).toStdString(), "application/json");
}

static void handle_api_audio_levels(const httplib::Request &req, httplib::Response &res)
{
	REQUIRE_AUTH(req, res)
	QJsonObject obj = SrtlaAutoSwitcher::instance().getAudioLevels();
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
		if (obj.contains("no_failover_scenes"))
			config_set_string(global_config, "SRTLA_AutoSwitch", "NoFailoverScenes",
					  obj["no_failover_scenes"].toString().toUtf8().constData());
		if (obj.contains("visibility_rules"))
			config_set_string(global_config, "SRTLA_AutoSwitch", "VisibilityRulesJSON",
					  obj["visibility_rules"].toString().toUtf8().constData());
		if (obj.contains("volume_rules"))
			config_set_string(global_config, "SRTLA_AutoSwitch", "VolumeRulesJSON",
					  obj["volume_rules"].toString().toUtf8().constData());
		if (obj.contains("delay"))
			config_set_int(global_config, "SRTLA_AutoSwitch", "Delay", obj["delay"].toInt());
		if (obj.contains("vis_delay"))
			config_set_int(global_config, "SRTLA_AutoSwitch", "VisDelay", obj["vis_delay"].toInt());
		if (obj.contains("vol_enabled"))
			config_set_bool(global_config, "SRTLA_AutoSwitch", "VolEnabled", obj["vol_enabled"].toBool());
		if (obj.contains("vol_delay"))
			config_set_int(global_config, "SRTLA_AutoSwitch", "VolDelay", obj["vol_delay"].toInt());
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
	char buf[4096] = {0};
	srtla_get_all_receivers_json(buf, sizeof(buf));
	res.set_content(buf, "application/json");
}

static void handle_api_stats(const httplib::Request &req, httplib::Response &res)
{
	int listen_port = 0, failed = 0;
	char buf[4096] = {0};
	srtla_get_connection_details(&listen_port, &failed, buf, sizeof(buf));
	
	char rist_buf[4096] = {0};
	rist_get_connection_details(rist_buf, sizeof(rist_buf));

	QJsonDocument doc = QJsonDocument::fromJson(QByteArray(buf));
	QJsonDocument ristDoc = QJsonDocument::fromJson(QByteArray(rist_buf));
	
	QJsonObject root;
	if (doc.isObject()) {
		root = doc.object();
	} else {
		root["groups"] = QJsonArray();
		root["ports"] = QJsonArray();
	}
	
	if (ristDoc.isObject()) {
		QJsonArray groups = root["groups"].toArray();
		QJsonArray ristGroups = ristDoc.object()["groups"].toArray();
		for (int i = 0; i < ristGroups.size(); i++) {
			groups.append(ristGroups[i]);
		}
		root["groups"] = groups;

		QJsonArray ports = root["ports"].toArray();
		QJsonArray ristPorts = ristDoc.object()["ports"].toArray();
		for (int i = 0; i < ristPorts.size(); i++) {
			ports.append(ristPorts[i]);
		}
		root["ports"] = ports;
	}
	
	QJsonDocument mergedDoc(root);
	QString outJson = mergedDoc.toJson(QJsonDocument::Compact);
	if (outJson.isEmpty() || outJson == "null")
		outJson = "{}";
	res.set_content(outJson.toStdString(), "application/json");
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

static void handle_api_obs_overview(const httplib::Request &req, httplib::Response &res)
{
	QJsonObject obj;
	obj["status"] = "ok";
	obj["connected"] = true;

	QJsonObject streamStatus;
	bool isStreaming = obs_frontend_streaming_active();
	streamStatus["outputActive"] = isStreaming;
	streamStatus["outputState"] = isStreaming ? "OBS_WEBSOCKET_OUTPUT_STARTED" : "OBS_WEBSOCKET_OUTPUT_STOPPED";
	obj["stream_status"] = streamStatus;

	obj["vcam_active"] = obs_frontend_virtualcam_active();
	obj["replay_buffer_active"] = obs_frontend_replay_buffer_active();
	obj["recording_active"] = obs_frontend_recording_active();

	obs_source_t *currentSceneSrc = obs_frontend_get_current_scene();
	QString currentSceneName = "";
	if (currentSceneSrc) {
		const char *name = obs_source_get_name(currentSceneSrc);
		if (name)
			currentSceneName = QString::fromUtf8(name);
	}
	obj["current_program_scene"] = currentSceneName;

	struct obs_frontend_source_list sceneList = {};
	obs_frontend_get_scenes(&sceneList);
	QJsonArray scenesArr;
	for (size_t i = 0; i < sceneList.sources.num; i++) {
		obs_source_t *s = sceneList.sources.array[i];
		if (s) {
			const char *n = obs_source_get_name(s);
			if (n) {
				QJsonObject sObj;
				sObj["sceneName"] = QString::fromUtf8(n);
				scenesArr.append(sObj);
			}
		}
	}
	obs_frontend_source_list_free(&sceneList);
	obj["scenes"] = scenesArr;

	QJsonArray itemsArr;
	if (currentSceneSrc) {
		obs_scene_t *scene = obs_scene_from_source(currentSceneSrc);
		if (scene) {
			obs_scene_enum_items(
				scene,
				[](obs_scene_t *, obs_sceneitem_t *item, void *param) -> bool {
					auto *arr = static_cast<QJsonArray *>(param);
					obs_source_t *itemSrc = obs_sceneitem_get_source(item);
					if (itemSrc) {
						const char *n = obs_source_get_name(itemSrc);
						QJsonObject iObj;
						iObj["sceneItemId"] = (qint64)obs_sceneitem_get_id(item);
						iObj["sourceName"] = n ? QString::fromUtf8(n) : "";
						iObj["sceneItemEnabled"] = obs_sceneitem_visible(item);
						arr->append(iObj);
					}
					return true;
				},
				&itemsArr);
		}
		obs_source_release(currentSceneSrc);
	}
	obj["scene_items"] = itemsArr;

	struct obs_frontend_source_list transList = {};
	obs_frontend_get_transitions(&transList);
	QJsonArray transArr;
	for (size_t i = 0; i < transList.sources.num; i++) {
		obs_source_t *t = transList.sources.array[i];
		if (t) {
			const char *n = obs_source_get_name(t);
			if (n) {
				QJsonObject tObj;
				tObj["transitionName"] = QString::fromUtf8(n);
				transArr.append(tObj);
			}
		}
	}
	obs_frontend_source_list_free(&transList);
	obj["transitions"] = transArr;

	obs_source_t *curTrans = obs_frontend_get_current_transition();
	if (curTrans) {
		const char *n = obs_source_get_name(curTrans);
		obj["current_transition"] = n ? QString::fromUtf8(n) : "";
		obs_source_release(curTrans);
	} else {
		obj["current_transition"] = "";
	}

	QJsonDocument doc(obj);
	res.set_content(doc.toJson(QJsonDocument::Compact).toStdString(), "application/json");
}

static void handle_api_obs_set_scene(const httplib::Request &req, httplib::Response &res)
{
	REQUIRE_AUTH(req, res)
	QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(req.body));
	if (doc.isObject()) {
		QString sceneName = doc.object()["sceneName"].toString();
		if (!sceneName.isEmpty()) {
			QMetaObject::invokeMethod(
				qApp,
				[sceneName]() {
					obs_source_t *src = obs_get_source_by_name(sceneName.toUtf8().constData());
					if (src) {
						obs_frontend_set_current_scene(src);
						obs_source_release(src);
					}
				},
				Qt::QueuedConnection);
			res.set_content("{\"status\":\"ok\"}", "application/json");
			return;
		}
	}
	res.status = 400;
}

static void handle_api_obs_set_source_visibility(const httplib::Request &req, httplib::Response &res)
{
	REQUIRE_AUTH(req, res)
	QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(req.body));
	if (doc.isObject()) {
		QString sceneName = doc.object()["sceneName"].toString();
		qint64 itemId = doc.object()["sceneItemId"].toInteger();
		bool enabled = doc.object()["sceneItemEnabled"].toBool();

		QMetaObject::invokeMethod(
			qApp,
			[sceneName, itemId, enabled]() {
				obs_source_t *src = obs_get_source_by_name(sceneName.toUtf8().constData());
				if (src) {
					obs_scene_t *scene = obs_scene_from_source(src);
					if (scene) {
						std::pair<int64_t, bool> data(itemId, enabled);
						obs_scene_enum_items(
							scene,
							[](obs_scene_t *, obs_sceneitem_t *item, void *param) -> bool {
								auto *d = static_cast<std::pair<int64_t, bool> *>(param);
								if (obs_sceneitem_get_id(item) == d->first) {
									obs_sceneitem_set_visible(item, d->second);
									return false;
								}
								return true;
							},
							&data);
					}
					obs_source_release(src);
				}
			},
			Qt::QueuedConnection);
		res.set_content("{\"status\":\"ok\"}", "application/json");
		return;
	}
	res.status = 400;
}

static void handle_api_obs_toggle_stream(const httplib::Request &req, httplib::Response &res)
{
	REQUIRE_AUTH(req, res)
	QMetaObject::invokeMethod(
		qApp,
		[]() {
			if (obs_frontend_streaming_active()) {
				obs_frontend_streaming_stop();
			} else {
				obs_frontend_streaming_start();
			}
		},
		Qt::QueuedConnection);
	res.set_content("{\"status\":\"ok\"}", "application/json");
}

static void handle_api_obs_feature(const httplib::Request &req, httplib::Response &res)
{
	REQUIRE_AUTH(req, res)
	QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(req.body));
	if (doc.isObject()) {
		QString feature = doc.object()["feature"].toString();
		bool state = doc.object()["state"].toBool();

		QMetaObject::invokeMethod(
			qApp,
			[feature, state]() {
				if (feature == "vcam") {
					if (state) obs_frontend_start_virtualcam();
					else obs_frontend_stop_virtualcam();
				} else if (feature == "replay") {
					if (state) obs_frontend_replay_buffer_start();
					else obs_frontend_replay_buffer_stop();
				}
			},
			Qt::QueuedConnection);
		res.set_content("{\"status\":\"ok\"}", "application/json");
		return;
	}
	res.status = 400;
}

static void handle_api_obs_transition(const httplib::Request &req, httplib::Response &res)
{
	REQUIRE_AUTH(req, res)
	QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(req.body));
	if (doc.isObject()) {
		QString transName = doc.object()["transitionName"].toString();
		QMetaObject::invokeMethod(
			qApp,
			[transName]() {
				obs_source_t *src = obs_get_source_by_name(transName.toUtf8().constData());
				if (src) {
					obs_frontend_set_current_transition(src);
					obs_source_release(src);
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

	svr->Get("/overlay", [](const httplib::Request &, httplib::Response &res) {
		QFile file(":/web/overlay.html");
		if (file.open(QIODevice::ReadOnly)) {
			res.set_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
			res.set_header("Pragma", "no-cache");
			res.set_content(file.readAll().toStdString(), "text/html");
		} else {
			res.status = 404;
			res.set_content("Not Found", "text/plain");
		}
	});

	svr->Get("/overlay/config", [](const httplib::Request &, httplib::Response &res) {
		QFile file(":/web/overlay_config.html");
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
	svr->Post("/api/obs/screenshot", handle_api_obs_screenshot);
	svr->Get("/api/obs/ws_config", handle_api_obs_ws_config);
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

	svr->Get("/api/obs/overview", handle_api_obs_overview);
	svr->Post("/api/obs/set_scene", handle_api_obs_set_scene);
	svr->Post("/api/obs/set_source_visibility", handle_api_obs_set_source_visibility);
	svr->Post("/api/obs/toggle_stream", handle_api_obs_toggle_stream);
	svr->Post("/api/obs/feature", handle_api_obs_feature);
	svr->Post("/api/obs/transition", handle_api_obs_transition);
	svr->Get("/api/audio_levels", handle_api_audio_levels);

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
