#pragma once

#include <QDockWidget>
#include <QLabel>
#include <QTimer>
#include <QTreeWidget>
#include <QMap>

#include <obs-frontend-api.h>

class SrtlaStatusWidget : public QDockWidget {
	Q_OBJECT

public:
	SrtlaStatusWidget(QWidget *parent = nullptr);
	~SrtlaStatusWidget();

private slots:
	void updateStatus();

private:
	QLabel *statusLabel;
	QLabel *portLabel;
	QLabel *encodersLabel;
	QLabel *connectionsLabel;
	QLabel *failedConnectionsLabel;
	class QTableWidget *receiversTable;
	QTreeWidget *treeWidget;
	QTimer *updateTimer;

	QMap<QString, uint64_t> previousBytes;

private slots:
	void openLogFolder();
};

#include <QDialog>
#include <QLineEdit>
#include <QCheckBox>
#include <QSpinBox>

class SrtlaReverseProxyDialog : public QDialog {
	Q_OBJECT

public:
	SrtlaReverseProxyDialog(QWidget *parent = nullptr);

private slots:
	void saveSettings();

private:
	QCheckBox *enableProxy;
	QLineEdit *serverAddress;
	QSpinBox *serverPort;
	QLineEdit *authToken;
	QLineEdit *forwardPorts;
};
#include <QComboBox>
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
	QString action; // "Show" or "Hide"
};

class SrtlaAutoSwitchDialog : public QDialog {
	Q_OBJECT

public:
	SrtlaAutoSwitchDialog(QWidget *parent = nullptr);

private slots:
	void saveSettings();
	void addNewRule();
	void addNewVisibilityRule();

private:
	QCheckBox *enableAutoSwitch;
	QSpinBox *switchDelay;
	QTableWidget *rulesTable;

	QCheckBox *enableVisSwitch;
	QSpinBox *visSwitchDelay;
	QTableWidget *visibilityRulesTable;

	QStringList availableScenes;
	QStringList availableSources;

	void addRuleRow(int minKbps, int maxKbps, const QString &targetScene);
	void addVisibilityRuleRow(int minKbps, int maxKbps, const QString &sourceName, const QString &action);
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

private slots:
	void checkBitrate();

private:
	SrtlaAutoSwitcher(QObject *parent = nullptr);
	~SrtlaAutoSwitcher();

	QTimer *timer;
	QMap<QString, uint64_t> previousBytes;

	QVector<AutoSwitchRule> rules;
	QVector<SourceVisibilityRule> visibilityRules;
	int currentMatchedRuleIndex;
	int currentlyAppliedRuleIndex;
	int matchDurationCounter;

	int currentMatchedVisRuleIndex;
	int currentlyAppliedVisRuleIndex;
	int visMatchDurationCounter;

	QString originalSceneName;

	void loadRules();

	static void handleFrontendEvent(enum obs_frontend_event event, void *private_data);
};
