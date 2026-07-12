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

static void handle_api_settings_get(const httplib::Request &, httplib::Response &res) {
    config_t *global_config = obs_frontend_get_profile_config();
    QJsonObject obj;
    if (global_config) {
        obj["proxy_enabled"] = config_get_bool(global_config, "SRTLA_Proxy", "Enabled");
        obj["autoswitch_enabled"] = config_get_bool(global_config, "SRTLA_AutoSwitch", "Enabled");
        obj["vis_autoswitch_enabled"] = config_get_bool(global_config, "SRTLA_AutoSwitch", "VisEnabled");
    }
    QJsonDocument doc(obj);
    res.set_content(doc.toJson(QJsonDocument::Compact).toStdString(), "application/json");
}

static void handle_api_settings_post(const httplib::Request &req, httplib::Response &res) {
    config_t *global_config = obs_frontend_get_profile_config();
    if (!global_config) { res.status = 500; return; }

    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(req.body));
    if (doc.isObject()) {
        QJsonObject obj = doc.object();
        if (obj.contains("proxy_enabled")) config_set_bool(global_config, "SRTLA_Proxy", "Enabled", obj["proxy_enabled"].toBool());
        if (obj.contains("autoswitch_enabled")) config_set_bool(global_config, "SRTLA_AutoSwitch", "Enabled", obj["autoswitch_enabled"].toBool());
        if (obj.contains("vis_autoswitch_enabled")) config_set_bool(global_config, "SRTLA_AutoSwitch", "VisEnabled", obj["vis_autoswitch_enabled"].toBool());
        
        config_save_safe(global_config, "tmp", nullptr);
        srtla_proxy_settings_changed();
        
        res.status = 200;
        res.set_content("{\"status\":\"ok\"}", "application/json");
    } else {
        res.status = 400;
    }
}

static void handle_api_obs_ws_config(const httplib::Request &, httplib::Response &res) {
    config_t *app_config = obs_frontend_get_app_config();
    QJsonObject obj;
    if (app_config) {
        obj["ws_enabled"] = config_get_bool(app_config, "obs-websocket", "ServerEnabled");
        const char* pwd = config_get_string(app_config, "obs-websocket", "ServerPassword");
        obj["ws_password"] = pwd ? QString(pwd) : "";
        obj["ws_port"] = config_get_int(app_config, "obs-websocket", "ServerPort");
    }
    QJsonDocument doc(obj);
    res.set_content(doc.toJson(QJsonDocument::Compact).toStdString(), "application/json");
}

#include <QWidget>
#include <QDir>
#include <QTemporaryFile>
#include <QTextStream>

static void handle_api_restart(const httplib::Request &, httplib::Response &res) {
    res.set_content("{\"status\":\"restarting\"}", "application/json");
    QMetaObject::invokeMethod(qApp, []() {
        QString path = QDir::toNativeSeparators(qApp->applicationFilePath());
        QString workDir = QDir::toNativeSeparators(qApp->applicationDirPath());
        
        QString batPath = QDir::toNativeSeparators(QDir::tempPath() + QDir::separator() + "obs_pyleirl_restart.bat");
        QFile file(batPath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&file);
            out << "@echo off\n";
            out << "timeout /t 2 >nul\n";
            out << "cd /d \"" << workDir << "\"\n";
            out << "start \"\" \"" << path << "\"\n";
            out << "del \"%~f0\"\n"; // self delete
            file.close();
            
            QProcess::startDetached("cmd.exe", QStringList() << "/c" << batPath);
        }
        
        // Graceful exit via Main Window closeEvent
        QWidget *mainWindow = (QWidget*)obs_frontend_get_main_window();
        if (mainWindow) {
            mainWindow->close();
        } else {
            qApp->quit();
        }
    }, Qt::QueuedConnection);
}

static void handle_api_stream_key_get(const httplib::Request &, httplib::Response &res) {
    obs_service_t *service = obs_frontend_get_streaming_service();
    QJsonObject obj;
    obj["key"] = "";
    obj["server"] = "";
    if (service) {
        obs_data_t *settings = obs_service_get_settings(service);
        const char *key = obs_data_get_string(settings, "key");
        const char *server = obs_data_get_string(settings, "server");
        if (key) obj["key"] = QString(key);
        if (server) obj["server"] = QString(server);
        obs_data_release(settings);
    }
    QJsonDocument doc(obj);
    res.set_content(doc.toJson(QJsonDocument::Compact).toStdString(), "application/json");
}

static void handle_api_stream_key_post(const httplib::Request &req, httplib::Response &res) {
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

static void handle_api_autoswitch_get(const httplib::Request &, httplib::Response &res) {
    config_t *global_config = obs_frontend_get_profile_config();
    QJsonObject obj;
    if (global_config) {
        const char* rules = config_get_string(global_config, "SRTLA_AutoSwitch", "RulesJSON");
        const char* visRules = config_get_string(global_config, "SRTLA_AutoSwitch", "VisibilityRulesJSON");
        obj["rules"] = rules ? QString(rules) : "[]";
        obj["visibility_rules"] = visRules ? QString(visRules) : "[]";
        obj["delay"] = config_get_int(global_config, "SRTLA_AutoSwitch", "Delay");
        obj["vis_delay"] = config_get_int(global_config, "SRTLA_AutoSwitch", "VisDelay");
    }
    QJsonDocument doc(obj);
    res.set_content(doc.toJson(QJsonDocument::Compact).toStdString(), "application/json");
}

static void handle_api_autoswitch_post(const httplib::Request &req, httplib::Response &res) {
    config_t *global_config = obs_frontend_get_profile_config();
    if (!global_config) { res.status = 500; return; }
    
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(req.body));
    if (doc.isObject()) {
        QJsonObject obj = doc.object();
        if (obj.contains("rules")) config_set_string(global_config, "SRTLA_AutoSwitch", "RulesJSON", obj["rules"].toString().toUtf8().constData());
        if (obj.contains("visibility_rules")) config_set_string(global_config, "SRTLA_AutoSwitch", "VisibilityRulesJSON", obj["visibility_rules"].toString().toUtf8().constData());
        if (obj.contains("delay")) config_set_int(global_config, "SRTLA_AutoSwitch", "Delay", obj["delay"].toInt());
        if (obj.contains("vis_delay")) config_set_int(global_config, "SRTLA_AutoSwitch", "VisDelay", obj["vis_delay"].toInt());
        config_save_safe(global_config, "tmp", nullptr);
        
        QMetaObject::invokeMethod(qApp, []() {
            SrtlaAutoSwitcher::instance().reloadRules();
        }, Qt::QueuedConnection);
        
        res.set_content("{\"status\":\"ok\"}", "application/json");
        return;
    }
    res.status = 400;
}

static void handle_api_receivers(const httplib::Request &, httplib::Response &res) {
    char buf[4096] = {0};
    srtla_get_all_receivers_json(buf, sizeof(buf));
    res.set_content(buf, "application/json");
}

static void handle_api_stats(const httplib::Request &, httplib::Response &res) {
    int listen_port = 0, failed = 0;
    char buf[4096] = {0};
    srtla_get_connection_details(&listen_port, &failed, buf, sizeof(buf));
    if (strlen(buf) == 0) strcpy(buf, "{}");
    res.set_content(buf, "application/json");
}

static void handle_api_receiver_action(const httplib::Request &req, httplib::Response &res) {
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(req.body));
    if (doc.isObject()) {
        QString action = doc.object()["action"].toString();
        QString name = doc.object()["name"].toString();
        QByteArray nameBA = name.toUtf8();
        
        if (action == "start") srtla_force_start_by_name(nameBA.constData());
        else if (action == "stop") srtla_force_stop_by_name(nameBA.constData());
        else if (action == "restart") srtla_force_restart_by_name(nameBA.constData());
        
        res.set_content("{\"status\":\"ok\"}", "application/json");
        return;
    }
    res.status = 400;
}

void srtla_web_server_start(int port) {
    if (is_running) return;

    svr = new httplib::Server();

    svr->Get("/", [](const httplib::Request &, httplib::Response &res) {
        QFile file(":/web/index.html");
        if (file.open(QIODevice::ReadOnly)) {
            QByteArray content = file.readAll();
            res.set_content(content.toStdString(), "text/html");
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

    is_running = true;
    server_thread = new std::thread([port]() {
        svr->listen("0.0.0.0", port);
    });
}

void srtla_web_server_stop() {
    if (!is_running || !svr) return;
    
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

bool srtla_web_server_is_running() {
    return is_running;
}
