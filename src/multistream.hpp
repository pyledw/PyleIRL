#pragma once

#include <QObject>
#include <QString>
#include <QVector>
#include <QJsonObject>
#include <obs.h>
#include <obs-frontend-api.h>

struct MultistreamTargetConfig {
	QString id;
	QString name;
	QString type; // "RTMP" or "SRT"
	QString url;
	QString key;
	bool enabled = true;

	QJsonObject toJson() const;
	static MultistreamTargetConfig fromJson(const QJsonObject &obj);
};

class MultistreamTarget : public QObject {
	Q_OBJECT
public:
	MultistreamTarget(const MultistreamTargetConfig &config, QObject *parent = nullptr);
	~MultistreamTarget();

	enum Status { STOPPED, STARTING, STREAMING, STOPPING, RECONNECTING };

	void start();
	void stop();
	void updateConfig(const MultistreamTargetConfig &config);

	Status getStatus() const { return status; }
	MultistreamTargetConfig getConfig() const { return config; }
	QJsonObject getMetrics() const;

signals:
	void statusChanged(const QString &id, int status);

private:
	MultistreamTargetConfig config;
	obs_output_t *output = nullptr;
	obs_service_t *service = nullptr;
	Status status = STOPPED;

	void initOutput();
	void cleanupOutput();
	bool cloneEncoders();

	void setStatus(Status newStatus);

	// OBS Signal callbacks
	static void onStart(void *data, calldata_t *cd);
	static void onStop(void *data, calldata_t *cd);
	static void onStarting(void *data, calldata_t *cd);
	static void onStopping(void *data, calldata_t *cd);
	static void onReconnect(void *data, calldata_t *cd);
	static void onReconnectSuccess(void *data, calldata_t *cd);
};

class MultistreamManager : public QObject {
	Q_OBJECT
public:
	static MultistreamManager &instance();

	void loadConfig();
	void saveConfig();

	bool getSyncWithObs() const { return syncWithObs; }
	void setSyncWithObs(bool sync);

	QVector<MultistreamTarget *> getTargets() const { return targets; }
	MultistreamTarget *getTarget(const QString &id) const;

	void addTarget(const MultistreamTargetConfig &config);
	void updateTarget(const QString &id, const MultistreamTargetConfig &config);
	void deleteTarget(const QString &id);

	void startAll();
	void stopAll();

signals:
	void targetsChanged();
	void targetStatusChanged(const QString &id, int status);

private:
	MultistreamManager(QObject *parent = nullptr);
	~MultistreamManager();

	QVector<MultistreamTarget *> targets;
	bool syncWithObs = false;

	static void obsFrontendEvent(enum obs_frontend_event event, void *private_data);
};
