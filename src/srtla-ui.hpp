#pragma once

#include <QDockWidget>
#include <QLabel>
#include <QTimer>
#include <QTreeWidget>
#include <QMap>
#include <QSplitter>
#include <QSettings>

#include <obs-frontend-api.h>

class SrtlaStatusWidget : public QDockWidget {
	Q_OBJECT

public:
	SrtlaStatusWidget(QWidget *parent = nullptr);
	~SrtlaStatusWidget();

private slots:
	void updateStatus();
	void onSplitterMoved(int pos, int index);

private:
	QLabel *metricsLabel;
	class QTableWidget *receiversTable;
	QTreeWidget *treeWidget;
	QSplitter *splitter;
	QTimer *updateTimer;

	QMap<QString, uint64_t> previousBytes;

private slots:
	void openLogFolder();
};

#include <QDialog>
#include <QLineEdit>
#include <QComboBox>
#include <QSpinBox>

class SrtlaReverseProxyDialog : public QDialog {
	Q_OBJECT

public:
	SrtlaReverseProxyDialog(QWidget *parent = nullptr);

private slots:
	void saveSettings();

private:
	QComboBox *enableProxy;
	QLineEdit *serverAddress;
	QSpinBox *serverPort;
	QLineEdit *authToken;
	QLineEdit *forwardPorts;
};

#include <QTableWidget>
#include <QVector>

struct AutoSwitchRule {
	int minKbps;
	int maxKbps; // 0 means unlimited
	QString targetScene;
};

struct SourceVisibilityRule {
	int minKbps;
	int maxKbps; // 0 means unlimited
	QString sourceName;
};

struct VolumeVisibilityRule {
	QString audioSource;
	int minDb;
	int maxDb;
	QString targetSource;
};

struct MonitoredAudioSource {
	obs_volmeter_t *volmeter = nullptr;
	obs_source_t *source = nullptr;
	float lastDb = -100.0f;
	uint64_t lastUpdateTime = 0;
};

class QListWidget;

class SrtlaAutoSwitchDialog : public QDialog {
	Q_OBJECT

public:
	SrtlaAutoSwitchDialog(QWidget *parent = nullptr);

private slots:
	void saveSettings();
	void addNewRule();
	void addNewVisibilityRule();
	void addNewVolumeRule();

private:
	QComboBox *enableAutoSwitch;
	QSpinBox *switchDelay;
	QSpinBox *recoveryDelay;
	QTableWidget *rulesTable;
	QListWidget *noFailoverList;

	QComboBox *enableVisSwitch;
	QSpinBox *visSwitchDelay;
	QTableWidget *visibilityRulesTable;

	QComboBox *enableVolSwitch;
	QSpinBox *volSwitchDelay;
	QTableWidget *volumeRulesTable;

	QStringList availableScenes;
	QStringList availableSources;
	QStringList availableAudioSources;

	void addRuleRow(int minKbps, int maxKbps, const QString &targetScene);
	void addVisibilityRuleRow(int minKbps, int maxKbps, const QString &sourceName);
	void addVolumeRuleRow(const QString &audioSource, int minDb, int maxDb, const QString &targetSource);
};

class SrtlaAutoSwitcher : public QObject {
	Q_OBJECT

public:
	static SrtlaAutoSwitcher &instance()
	{
		static SrtlaAutoSwitcher inst;
		return inst;
	}

	void start();
	void stop();
	void reloadRules();

private slots:
	void checkBitrate();

private:
	SrtlaAutoSwitcher(QObject *parent = nullptr);
	~SrtlaAutoSwitcher();

	QTimer *timer;
	QMap<QString, uint64_t> previousBytes;

	QVector<AutoSwitchRule> rules;
	QVector<SourceVisibilityRule> visibilityRules;
	QVector<VolumeVisibilityRule> volumeRules;
	QSet<QString> noFailoverScenes;
	int currentMatchedRuleIndex;
	int currentlyAppliedRuleIndex;
	int matchDurationCounter;

	QSet<int> currentMatchedVisRules;
	QSet<int> currentlyAppliedVisRules;
	int visMatchDurationCounter;

	QMap<QString, MonitoredAudioSource *> monitoredAudioSources;
	QSet<int> currentMatchedVolRules;
	QSet<int> currentlyAppliedVolRules;
	int volMatchDurationCounter;

	QString originalSceneName;

	void loadRules();
	void updateMonitoredAudioSources();
	void clearMonitoredAudioSources();

	static void handleFrontendEvent(enum obs_frontend_event event, void *private_data);
	static void volmeterCallback(void *param, const float magnitude[MAX_AUDIO_CHANNELS],
				     const float peak[MAX_AUDIO_CHANNELS],
				     const float input_peak[MAX_AUDIO_CHANNELS]);
};

class SrtlaWebInterfaceDialog : public QDialog {
	Q_OBJECT

public:
	SrtlaWebInterfaceDialog(QWidget *parent = nullptr);

private slots:
	void saveSettings();

private:
	QComboBox *enableWeb;
	QSpinBox *webPort;
	class QLineEdit *accessPassword;
};

class SrtlaAboutDialog : public QDialog {
	Q_OBJECT

public:
	SrtlaAboutDialog(QWidget *parent = nullptr);
};

class QCheckBox;

class SrtlaMultistreamDialog : public QDialog {
	Q_OBJECT

public:
	SrtlaMultistreamDialog(QWidget *parent = nullptr);

private slots:
	void saveSettings();
	void addTarget();
	void editTarget();
	void deleteTarget();
	void reloadList();

private:
	QCheckBox *syncWithObsCheck;

	class QTableWidget *targetsTable;
};

class SrtlaMultistreamDock : public QDockWidget {
	Q_OBJECT

public:
	SrtlaMultistreamDock(QWidget *parent = nullptr);

private slots:
	void updateList();
	void startTarget();
	void stopTarget();

private:
	class QTableWidget *statusTable;
};
