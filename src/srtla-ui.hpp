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

class SrtlaAutoSwitchDialog : public QDialog {
	Q_OBJECT

public:
	SrtlaAutoSwitchDialog(QWidget *parent = nullptr);

private slots:
	void saveSettings();
	void addNewRule();
	void addNewVisibilityRule();

private:
	QComboBox *enableAutoSwitch;
	QSpinBox *switchDelay;
	QSpinBox *recoveryDelay;
	QTableWidget *rulesTable;

	QComboBox *enableVisSwitch;
	QSpinBox *visSwitchDelay;
	QTableWidget *visibilityRulesTable;

	QStringList availableScenes;
	QStringList availableSources;

	void addRuleRow(int minKbps, int maxKbps, const QString &targetScene);
	void addVisibilityRuleRow(int minKbps, int maxKbps, const QString &sourceName);
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
	class QLineEdit *wsPassword;
	class QLineEdit *wsUrlOverride;
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
