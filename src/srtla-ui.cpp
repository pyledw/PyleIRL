#include "srtla-ui.hpp"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QFont>
#include <QScrollArea>
#include <QTableWidget>
#include <QPushButton>
#include <QDesktopServices>
#include <QUrl>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QHeaderView>

#include <obs.h>
#include <util/config-file.h>
#include <obs-frontend-api.h>

extern "C" {
void srtla_get_connection_stats(bool *is_listening, int *active_groups, int *active_connections);
void srtla_get_connection_details(int *listen_port, int *failed_conns, char *out_buffer, int max_len);
void srtla_get_all_receivers_json(char *out_buffer, int max_len);
void srtla_force_start_by_name(const char *name);
void srtla_force_stop_by_name(const char *name);
void srtla_force_restart_by_name(const char *name);
char *srtla_get_frpc_path(void);
}

SrtlaStatusWidget::SrtlaStatusWidget(QWidget *parent) : QDockWidget("SRTLA Status", parent)
{
	QWidget *centralWidget = new QWidget(this);
	QVBoxLayout *mainLayout = new QVBoxLayout(centralWidget);

	QFont boldFont;
	boldFont.setBold(true);

	auto addRow = [&](const QString &titleText, QLabel *&labelOut, QVBoxLayout *layout) {
		QHBoxLayout *rowLayout = new QHBoxLayout();
		QLabel *title = new QLabel(titleText, this);
		title->setFont(boldFont);
		labelOut = new QLabel("-", this);
		rowLayout->addWidget(title);
		rowLayout->addWidget(labelOut);
		rowLayout->addStretch();
		layout->addLayout(rowLayout);
	};

	addRow("Receiver Status:", statusLabel, mainLayout);
	addRow("Listening UDP Port:", portLabel, mainLayout);

	QFrame *line1 = new QFrame();
	line1->setFrameShape(QFrame::HLine);
	line1->setFrameShadow(QFrame::Sunken);
	mainLayout->addWidget(line1);

	addRow("Active Encoders (Groups):", encodersLabel, mainLayout);
	addRow("Active Bonded Connections:", connectionsLabel, mainLayout);
	addRow("Failed/Dropped Connections:", failedConnectionsLabel, mainLayout);

	QFrame *line2 = new QFrame();
	line2->setFrameShape(QFrame::HLine);
	line2->setFrameShadow(QFrame::Sunken);
	mainLayout->addWidget(line2);

	QLabel *receiversTitle = new QLabel("Configured Receivers:", this);
	receiversTitle->setFont(boldFont);
	mainLayout->addWidget(receiversTitle);

	receiversTable = new QTableWidget(this);
	receiversTable->setColumnCount(4);
	receiversTable->setHorizontalHeaderLabels(QStringList() << "Name" << "Port" << "Status" << "Actions");
	receiversTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
	receiversTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	receiversTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	receiversTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
	receiversTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
	receiversTable->setSelectionMode(QAbstractItemView::NoSelection);
	receiversTable->setAlternatingRowColors(true);
	receiversTable->setStyleSheet(
		"QTableWidget { background-color: #1e1e1e; color: #d4d4d4; } QHeaderView::section { background-color: #2d2d2d; color: white; padding: 4px; }");
	receiversTable->verticalHeader()->setVisible(false);
	mainLayout->addWidget(receiversTable, 1);

	QLabel *detailsTitle = new QLabel("Connected Devices (IP:Port):", this);
	detailsTitle->setFont(boldFont);
	mainLayout->addWidget(detailsTitle);

	treeWidget = new QTreeWidget(this);
	treeWidget->setHeaderLabels(QStringList() << "Name / IP" << "Port" << "Bandwidth");
	treeWidget->header()->setSectionResizeMode(0, QHeaderView::Stretch);
	treeWidget->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	treeWidget->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	treeWidget->setAlternatingRowColors(true);
	treeWidget->setStyleSheet(
		"QTreeWidget { background-color: #1e1e1e; color: #d4d4d4; } QHeaderView::section { background-color: #2d2d2d; color: white; padding: 4px; }");

	mainLayout->addWidget(treeWidget, 1); // Expand to fill available space

	QPushButton *logButton = new QPushButton("View Detailed Plugin Logs", this);
	connect(logButton, &QPushButton::clicked, this, &SrtlaStatusWidget::openLogFolder);
	mainLayout->addWidget(logButton);

	setWidget(centralWidget);
	setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
		    QDockWidget::DockWidgetFloatable);

	// Set up timer
	updateTimer = new QTimer(this);
	connect(updateTimer, &QTimer::timeout, this, &SrtlaStatusWidget::updateStatus);
	updateTimer->start(500); // 500ms

	updateStatus(); // initial update
}

SrtlaStatusWidget::~SrtlaStatusWidget() {}

void SrtlaStatusWidget::openLogFolder()
{
	QString logPath = QString::fromLocal8Bit(qgetenv("APPDATA")) + "/obs-studio/logs";
	QDesktopServices::openUrl(QUrl::fromLocalFile(logPath));
}

void SrtlaStatusWidget::updateStatus()
{
	try {
		bool is_listening = false;
		int groups = 0;
		int connections = 0;
		int listen_port = 0;
		int failed_conns = 0;
		char details_buffer[4096] = {0};
		char receivers_buffer[4096] = {0};

		srtla_get_connection_stats(&is_listening, &groups, &connections);
		srtla_get_connection_details(&listen_port, &failed_conns, details_buffer, sizeof(details_buffer));
		srtla_get_all_receivers_json(receivers_buffer, sizeof(receivers_buffer));

		if (is_listening) {
			statusLabel->setText("Listening");
			statusLabel->setStyleSheet("color: #4CAF50; font-weight: bold;"); // Green
		} else {
			statusLabel->setText("Not Listening");
			statusLabel->setStyleSheet("color: gray;");
		}

		encodersLabel->setText(QString::number(groups));
		connectionsLabel->setText(QString::number(connections));
		failedConnectionsLabel->setText(QString::number(failed_conns));

		QJsonDocument rDoc = QJsonDocument::fromJson(QByteArray(receivers_buffer));
		if (rDoc.isArray()) {
			QJsonArray rArray = rDoc.array();
			receiversTable->setRowCount(rArray.size());
			for (int i = 0; i < rArray.size(); ++i) {
				QJsonObject rObj = rArray[i].toObject();
				QString name = rObj["name"].toString();
				QString port = QString::number(rObj["listen_port"].toInt());
				bool running = rObj["running"].toVariant().toBool();

				QTableWidgetItem *nameItem = new QTableWidgetItem(name);
				QTableWidgetItem *portItem = new QTableWidgetItem(port);
				QTableWidgetItem *statusItem = new QTableWidgetItem(running ? "Running" : "Stopped");
				statusItem->setForeground(running ? QBrush(QColor("#4CAF50")) : QBrush(QColor("gray")));

				receiversTable->setItem(i, 0, nameItem);
				receiversTable->setItem(i, 1, portItem);
				receiversTable->setItem(i, 2, statusItem);

				// Add action buttons
				QWidget *actionWidget = new QWidget();
				QHBoxLayout *actionLayout = new QHBoxLayout(actionWidget);
				actionLayout->setContentsMargins(2, 2, 2, 2);

				QPushButton *startBtn = new QPushButton("Start");
				QPushButton *stopBtn = new QPushButton("Stop");
				QPushButton *restartBtn = new QPushButton("Restart");

				startBtn->setEnabled(!running);
				stopBtn->setEnabled(running);

				QObject::connect(startBtn, &QPushButton::clicked,
						 [name]() { srtla_force_start_by_name(name.toUtf8().constData()); });
				QObject::connect(stopBtn, &QPushButton::clicked,
						 [name]() { srtla_force_stop_by_name(name.toUtf8().constData()); });
				QObject::connect(restartBtn, &QPushButton::clicked,
						 [name]() { srtla_force_restart_by_name(name.toUtf8().constData()); });

				actionLayout->addWidget(startBtn);
				actionLayout->addWidget(stopBtn);
				actionLayout->addWidget(restartBtn);
				receiversTable->setCellWidget(i, 3, actionWidget);
			}
		}

		QJsonDocument doc = QJsonDocument::fromJson(QByteArray(details_buffer));
		QString portsString = "-";

		if (doc.isObject()) {
			QJsonObject root = doc.object();

			QJsonArray portsArray = root["ports"].toArray();
			if (!portsArray.isEmpty()) {
				QStringList portList;
				for (int i = 0; i < portsArray.size(); i++) {
					portList << QString::number(portsArray[i].toInt());
				}
				portsString = portList.join(", ");
			}

			portLabel->setText(portsString);

			QJsonArray groupsArray = root["groups"].toArray();

			// Keep track of which items exist to remove stale ones
			QSet<QString> currentGroupIds;

			for (int i = 0; i < groupsArray.size(); i++) {
				QJsonObject gObj = groupsArray[i].toObject();
				QString groupIdStr = QString::number(gObj["id"].toVariant().toULongLong());
				QString listenPortStr = QString::number(gObj["listen_port"].toInt());
				currentGroupIds.insert(groupIdStr);

				uint64_t gBytes = gObj["bytes"].toVariant().toULongLong();
				uint64_t gPrevBytes = previousBytes.value(groupIdStr, gBytes);
				previousBytes[groupIdStr] = gBytes;

				double gKbps = ((gBytes - gPrevBytes) * 8.0) / 1000.0 / 0.5; // bits per 0.5s -> kbps

				QString nodeName = "Port " + listenPortStr + " (Encoder #" + groupIdStr + ")";

				QTreeWidgetItem *groupItem = nullptr;
				QList<QTreeWidgetItem *> found = treeWidget->findItems(nodeName, Qt::MatchExactly, 0);
				if (!found.isEmpty()) {
					groupItem = found.first();
				} else {
					groupItem = new QTreeWidgetItem(treeWidget);
					groupItem->setText(0, nodeName);
					groupItem->setExpanded(true);
				}

				groupItem->setText(1, "-");
				groupItem->setText(2, QString::number(gKbps, 'f', 1) + " Kbps");

				QJsonArray connsArray = gObj["conns"].toArray();
				QSet<QString> currentConnIds;

				for (int j = 0; j < connsArray.size(); j++) {
					QJsonObject cObj = connsArray[j].toObject();
					QString ip = cObj["ip"].toString();
					QString port = QString::number(cObj["port"].toInt());
					QString connIdStr = groupIdStr + "_" + ip + ":" + port;
					currentConnIds.insert(connIdStr);

					uint64_t cBytes = cObj["bytes"].toVariant().toULongLong();
					uint64_t cPrevBytes = previousBytes.value(connIdStr, cBytes);
					previousBytes[connIdStr] = cBytes;

					double cKbps = ((cBytes - cPrevBytes) * 8.0) / 1000.0 / 0.5;

					QTreeWidgetItem *connItem = nullptr;
					for (int k = 0; k < groupItem->childCount(); k++) {
						if (groupItem->child(k)->text(0) == ip &&
						    groupItem->child(k)->text(1) == port) {
							connItem = groupItem->child(k);
							break;
						}
					}
					if (!connItem) {
						connItem = new QTreeWidgetItem(groupItem);
						connItem->setText(0, ip);
						connItem->setText(1, port);
					}
					connItem->setText(2, QString::number(cKbps, 'f', 1) + " Kbps");
				}

				// Remove disconnected children
				for (int k = groupItem->childCount() - 1; k >= 0; k--) {
					QString ip = groupItem->child(k)->text(0);
					QString port = groupItem->child(k)->text(1);
					QString connIdStr = groupIdStr + "_" + ip + ":" + port;
					if (!currentConnIds.contains(connIdStr)) {
						delete groupItem->takeChild(k);
					}
				}
			}

			// Remove disconnected groups
			for (int i = treeWidget->topLevelItemCount() - 1; i >= 0; i--) {
				QTreeWidgetItem *item = treeWidget->topLevelItem(i);
				QString text = item->text(0);
				int hashIdx = text.indexOf("#");
				int closeParen = text.indexOf(")");
				if (hashIdx != -1 && closeParen != -1) {
					QString idStr = text.mid(hashIdx + 1, closeParen - hashIdx - 1);
					if (!currentGroupIds.contains(idStr)) {
						delete treeWidget->takeTopLevelItem(i);
					}
				} else {
					delete treeWidget->takeTopLevelItem(i);
				}
			}
		}
	} catch (...) {
		statusLabel->setText("Backend Error");
		statusLabel->setStyleSheet("color: red; font-weight: bold;");
		treeWidget->clear();
		QTreeWidgetItem *errItem = new QTreeWidgetItem(treeWidget);
		errItem->setText(0, "A severe exception was caught.");
	}
}

extern "C" void *create_srtla_dock()
{
	return new SrtlaStatusWidget();
}

#include <QFormLayout>
#include <QDialogButtonBox>
#include <QMessageBox>

SrtlaReverseProxyDialog::SrtlaReverseProxyDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle("SRTLA Reverse Proxy (FRP) Settings");
	setMinimumWidth(400);

	QVBoxLayout *mainLayout = new QVBoxLayout(this);
	QFormLayout *formLayout = new QFormLayout();

	enableProxy = new QCheckBox("Enable Reverse Proxy Tunnel");

	serverAddress = new QLineEdit();
	serverAddress->setPlaceholderText("e.g. proxy.mydomain.com or IP");

	serverPort = new QSpinBox();
	serverPort->setRange(1, 65535);
	serverPort->setValue(7000); // Default FRP port

	authToken = new QLineEdit();
	authToken->setEchoMode(QLineEdit::PasswordEchoOnEdit);
	authToken->setPlaceholderText("Optional FRP authentication token");

	forwardPorts = new QLineEdit();
	forwardPorts->setPlaceholderText("e.g. 5000-5010");
	forwardPorts->setToolTip("Comma separated list of ports or ranges to forward from the proxy to this machine.");

	formLayout->addRow("", enableProxy);
	formLayout->addRow("Server Address:", serverAddress);
	formLayout->addRow("Server Port:", serverPort);
	formLayout->addRow("Auth Token:", authToken);
	formLayout->addRow("Forward Ports:", forwardPorts);

	mainLayout->addLayout(formLayout);

	QDialogButtonBox *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	mainLayout->addWidget(buttonBox);

	connect(buttonBox, &QDialogButtonBox::accepted, this, &SrtlaReverseProxyDialog::saveSettings);
	connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

	// Load existing settings
	config_t *global_config = obs_frontend_get_profile_config();
	if (global_config) {
		enableProxy->setChecked(config_get_bool(global_config, "SRTLA_Proxy", "Enabled"));
		const char *addr = config_get_string(global_config, "SRTLA_Proxy", "ServerAddress");
		if (addr && *addr)
			serverAddress->setText(addr);

		int port = config_get_int(global_config, "SRTLA_Proxy", "ServerPort");
		if (port > 0)
			serverPort->setValue(port);

		const char *token = config_get_string(global_config, "SRTLA_Proxy", "AuthToken");
		if (token && *token)
			authToken->setText(token);

		const char *ports = config_get_string(global_config, "SRTLA_Proxy", "ForwardPorts");
		if (ports && *ports)
			forwardPorts->setText(ports);
	}
}

extern "C" void srtla_proxy_settings_changed();

void SrtlaReverseProxyDialog::saveSettings()
{
	config_t *global_config = obs_frontend_get_profile_config();
	if (global_config) {
		config_set_bool(global_config, "SRTLA_Proxy", "Enabled", enableProxy->isChecked());
		config_set_string(global_config, "SRTLA_Proxy", "ServerAddress",
				  serverAddress->text().toUtf8().constData());
		config_set_int(global_config, "SRTLA_Proxy", "ServerPort", serverPort->value());
		config_set_string(global_config, "SRTLA_Proxy", "AuthToken", authToken->text().toUtf8().constData());
		config_set_string(global_config, "SRTLA_Proxy", "ForwardPorts",
				  forwardPorts->text().toUtf8().constData());

		config_save_safe(global_config, "tmp", nullptr);
	}

	srtla_proxy_settings_changed();
	accept();
}

#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QMainWindow>
#include <obs-frontend-api.h>

extern "C" {
void srtla_force_stop_all();
void srtla_force_start_all();
void srtla_force_restart_all();
}

#include <QProcess>
#include <QFile>
#include <QTextStream>
#include <QStandardPaths>
#include <QDir>
#include <QCoreApplication>

static QProcess *frpcProcess = nullptr;

extern "C" void srtla_proxy_settings_changed()
{
	if (!frpcProcess) {
		frpcProcess = new QProcess();
		QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, [=]() {
			if (frpcProcess) {
				frpcProcess->kill();
				frpcProcess->waitForFinished(1000);
			}
		});
	}

	config_t *global_config = obs_frontend_get_profile_config();
	if (!global_config)
		return;

	bool enabled = config_get_bool(global_config, "SRTLA_Proxy", "Enabled");
	if (!enabled) {
		if (frpcProcess->state() != QProcess::NotRunning) {
			frpcProcess->kill();
			frpcProcess->waitForFinished(1000);
		}
		return;
	}

	QString serverAddress = config_get_string(global_config, "SRTLA_Proxy", "ServerAddress");
	int serverPort = config_get_int(global_config, "SRTLA_Proxy", "ServerPort");
	QString authToken = config_get_string(global_config, "SRTLA_Proxy", "AuthToken");
	QString forwardPorts = config_get_string(global_config, "SRTLA_Proxy", "ForwardPorts");

	if (serverAddress.isEmpty() || serverPort <= 0 || forwardPorts.isEmpty()) {
		return; // Missing configuration
	}

	// Write frpc.ini
	QString configDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
	QDir().mkpath(configDir);
	QString iniPath = configDir + "/frpc.ini";

	QFile file(iniPath);
	if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
		QTextStream out(&file);
		out << "[common]\n";
		out << "server_addr = " << serverAddress << "\n";
		out << "server_port = " << serverPort << "\n";
		if (!authToken.isEmpty()) {
			out << "token = " << authToken << "\n";
		}

		// Handle port ranges/lists, e.g., 5000-5010 or 5000,5001
		out << "\n[srtla_udp]\n";
		out << "type = udp\n";
		out << "local_ip = 127.0.0.1\n";
		out << "local_port = " << forwardPorts << "\n";
		out << "remote_port = " << forwardPorts << "\n";
		file.close();
	}

	// Restart process
	if (frpcProcess->state() != QProcess::NotRunning) {
		frpcProcess->kill();
		frpcProcess->waitForFinished(1000);
	}

	// Find bundled frpc
	QString frpcExecutable = "frpc";
	char *bundled_path = srtla_get_frpc_path();
	if (bundled_path) {
		frpcExecutable = QString::fromUtf8(bundled_path);
		bfree(bundled_path);
	}

	frpcProcess->start(frpcExecutable, QStringList() << "-c" << iniPath);
}

// -----------------------------------------------------------
// Auto-Switcher Implementation
// -----------------------------------------------------------
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMessageBox>
#include <QComboBox>
#include <QTimer>
#include <QMap>
#include <QTabWidget>

SrtlaAutoSwitchDialog::SrtlaAutoSwitchDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle("SRTLA Auto-Switch Settings");
	setMinimumWidth(600);

	QVBoxLayout *mainLayout = new QVBoxLayout(this);

	QTabWidget *tabs = new QTabWidget();

	QWidget *sceneTab = new QWidget();
	QFormLayout *sceneLayout = new QFormLayout(sceneTab);

	enableAutoSwitch = new QCheckBox("Enable Range-Based Auto-Switch");

	switchDelay = new QSpinBox();
	switchDelay->setRange(0, 60);
	switchDelay->setSuffix(" seconds");
	switchDelay->setValue(2); // Default 2 seconds

	// Populate scenes
	struct obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; i++) {
		obs_source_t *source = scenes.sources.array[i];
		const char *name = obs_source_get_name(source);
		if (name) {
			availableScenes.append(QString::fromUtf8(name));
		}
	}
	obs_frontend_source_list_free(&scenes);

	// Populate all sources
	obs_enum_sources(
		[](void *data, obs_source_t *source) {
			QStringList *list = static_cast<QStringList *>(data);
			const char *name = obs_source_get_name(source);
			if (name) {
				QString nameStr = QString::fromUtf8(name);
				if (!list->contains(nameStr))
					list->append(nameStr);
			}
			return true;
		},
		&availableSources);
	availableSources.sort();

	sceneLayout->addRow("", enableAutoSwitch);
	sceneLayout->addRow("Switch Delay:", switchDelay);

	rulesTable = new QTableWidget();
	rulesTable->setColumnCount(4);
	rulesTable->setHorizontalHeaderLabels(QStringList()
					      << "Min Kbps" << "Max Kbps (0=unlimited)" << "Target Scene" << "");
	rulesTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
	rulesTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);

	QPushButton *addRuleBtn = new QPushButton("Add Scene Rule");
	connect(addRuleBtn, &QPushButton::clicked, this, &SrtlaAutoSwitchDialog::addNewRule);

	sceneLayout->addRow(rulesTable);
	sceneLayout->addRow(addRuleBtn);

	tabs->addTab(sceneTab, "Scene Switching");

	QWidget *visTab = new QWidget();
	QFormLayout *visLayout = new QFormLayout(visTab);

	enableVisSwitch = new QCheckBox("Enable Range-Based Source Visibility");

	visSwitchDelay = new QSpinBox();
	visSwitchDelay->setRange(0, 60);
	visSwitchDelay->setSuffix(" seconds");
	visSwitchDelay->setValue(2); // Default 2 seconds

	visibilityRulesTable = new QTableWidget();
	visibilityRulesTable->setColumnCount(5);
	visibilityRulesTable->setHorizontalHeaderLabels(QStringList() << "Min Kbps" << "Max Kbps (0=unlimited)"
								      << "Source Name" << "Action" << "");
	visibilityRulesTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
	visibilityRulesTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);

	QPushButton *addVisRuleBtn = new QPushButton("Add Visibility Rule");
	connect(addVisRuleBtn, &QPushButton::clicked, this, &SrtlaAutoSwitchDialog::addNewVisibilityRule);

	visLayout->addRow("", enableVisSwitch);
	visLayout->addRow("Switch Delay:", visSwitchDelay);
	visLayout->addRow(visibilityRulesTable);
	visLayout->addRow(addVisRuleBtn);

	tabs->addTab(visTab, "Source Visibility");

	mainLayout->addWidget(tabs);

	QDialogButtonBox *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	mainLayout->addWidget(buttonBox);

	connect(buttonBox, &QDialogButtonBox::accepted, this, &SrtlaAutoSwitchDialog::saveSettings);
	connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

	// Load existing settings
	config_t *global_config = obs_frontend_get_profile_config();
	if (global_config) {
		enableAutoSwitch->setChecked(config_get_bool(global_config, "SRTLA_AutoSwitch", "Enabled"));
		enableVisSwitch->setChecked(config_get_bool(global_config, "SRTLA_AutoSwitch", "VisEnabled"));

		int delay = config_get_int(global_config, "SRTLA_AutoSwitch", "Delay");
		if (config_has_user_value(global_config, "SRTLA_AutoSwitch", "Delay")) {
			switchDelay->setValue(delay);
		}

		int visDelay = config_get_int(global_config, "SRTLA_AutoSwitch", "VisDelay");
		if (config_has_user_value(global_config, "SRTLA_AutoSwitch", "VisDelay")) {
			visSwitchDelay->setValue(visDelay);
		}

		const char *rulesJson = config_get_string(global_config, "SRTLA_AutoSwitch", "RulesJSON");
		if (rulesJson && *rulesJson) {
			QJsonDocument doc = QJsonDocument::fromJson(QByteArray(rulesJson));
			if (doc.isArray()) {
				QJsonArray arr = doc.array();
				for (int i = 0; i < arr.size(); i++) {
					QJsonObject obj = arr[i].toObject();
					addRuleRow(obj["minKbps"].toInt(), obj["maxKbps"].toInt(),
						   obj["targetScene"].toString());
				}
			}
		}

		const char *visRulesJson = config_get_string(global_config, "SRTLA_AutoSwitch", "VisibilityRulesJSON");
		if (visRulesJson && *visRulesJson) {
			QJsonDocument doc = QJsonDocument::fromJson(QByteArray(visRulesJson));
			if (doc.isArray()) {
				QJsonArray arr = doc.array();
				for (int i = 0; i < arr.size(); i++) {
					QJsonObject obj = arr[i].toObject();
					addVisibilityRuleRow(obj["minKbps"].toInt(), obj["maxKbps"].toInt(),
							     obj["sourceName"].toString(), obj["action"].toString());
				}
			}
		}
	}
}

void SrtlaAutoSwitchDialog::addRuleRow(int minKbps, int maxKbps, const QString &targetScene)
{
	int row = rulesTable->rowCount();
	rulesTable->insertRow(row);

	QSpinBox *minSp = new QSpinBox();
	minSp->setRange(0, 999999);
	minSp->setValue(minKbps);
	rulesTable->setCellWidget(row, 0, minSp);

	QSpinBox *maxSp = new QSpinBox();
	maxSp->setRange(0, 999999);
	maxSp->setValue(maxKbps);
	rulesTable->setCellWidget(row, 1, maxSp);

	QComboBox *sceneCb = new QComboBox();
	sceneCb->addItem("[Return to Original Screen]");
	sceneCb->addItems(availableScenes);
	int index = sceneCb->findText(targetScene);
	if (index >= 0)
		sceneCb->setCurrentIndex(index);
	rulesTable->setCellWidget(row, 2, sceneCb);

	QPushButton *removeBtn = new QPushButton("Remove");
	connect(removeBtn, &QPushButton::clicked, [this, removeBtn]() {
		for (int i = 0; i < rulesTable->rowCount(); i++) {
			if (rulesTable->cellWidget(i, 3) == removeBtn) {
				rulesTable->removeRow(i);
				break;
			}
		}
	});
	rulesTable->setCellWidget(row, 3, removeBtn);
}

void SrtlaAutoSwitchDialog::addNewRule()
{
	addRuleRow(0, 0, "[Return to Original Screen]");
}

void SrtlaAutoSwitchDialog::addVisibilityRuleRow(int minKbps, int maxKbps, const QString &sourceName,
						 const QString &action)
{
	int row = visibilityRulesTable->rowCount();
	visibilityRulesTable->insertRow(row);

	QSpinBox *minSp = new QSpinBox();
	minSp->setRange(0, 999999);
	minSp->setValue(minKbps);
	visibilityRulesTable->setCellWidget(row, 0, minSp);

	QSpinBox *maxSp = new QSpinBox();
	maxSp->setRange(0, 999999);
	maxSp->setValue(maxKbps);
	visibilityRulesTable->setCellWidget(row, 1, maxSp);

	QComboBox *sourceCb = new QComboBox();
	sourceCb->setEditable(true);
	sourceCb->addItems(availableSources);
	int index = sourceCb->findText(sourceName);
	if (index >= 0)
		sourceCb->setCurrentIndex(index);
	else
		sourceCb->setCurrentText(sourceName);
	visibilityRulesTable->setCellWidget(row, 2, sourceCb);

	QComboBox *actionCb = new QComboBox();
	actionCb->addItem("Show");
	actionCb->addItem("Hide");
	actionCb->setCurrentText(action);
	visibilityRulesTable->setCellWidget(row, 3, actionCb);

	QPushButton *removeBtn = new QPushButton("Remove");
	connect(removeBtn, &QPushButton::clicked, [this, removeBtn]() {
		for (int i = 0; i < visibilityRulesTable->rowCount(); i++) {
			if (visibilityRulesTable->cellWidget(i, 4) == removeBtn) {
				visibilityRulesTable->removeRow(i);
				break;
			}
		}
	});
	visibilityRulesTable->setCellWidget(row, 4, removeBtn);
}

void SrtlaAutoSwitchDialog::addNewVisibilityRule()
{
	addVisibilityRuleRow(0, 0, "", "Show");
}

void SrtlaAutoSwitchDialog::saveSettings()
{
	config_t *global_config = obs_frontend_get_profile_config();
	if (global_config) {
		config_set_bool(global_config, "SRTLA_AutoSwitch", "Enabled", enableAutoSwitch->isChecked());
		config_set_int(global_config, "SRTLA_AutoSwitch", "Delay", switchDelay->value());

		config_set_bool(global_config, "SRTLA_AutoSwitch", "VisEnabled", enableVisSwitch->isChecked());
		config_set_int(global_config, "SRTLA_AutoSwitch", "VisDelay", visSwitchDelay->value());

		QJsonArray arr;
		for (int i = 0; i < rulesTable->rowCount(); i++) {
			QSpinBox *minSp = qobject_cast<QSpinBox *>(rulesTable->cellWidget(i, 0));
			QSpinBox *maxSp = qobject_cast<QSpinBox *>(rulesTable->cellWidget(i, 1));
			QComboBox *sceneCb = qobject_cast<QComboBox *>(rulesTable->cellWidget(i, 2));

			if (minSp && maxSp && sceneCb) {
				QJsonObject obj;
				obj["minKbps"] = minSp->value();
				obj["maxKbps"] = maxSp->value();
				obj["targetScene"] = sceneCb->currentText();
				arr.append(obj);
			}
		}
		QJsonDocument doc(arr);
		QString jsonString = doc.toJson(QJsonDocument::Compact);

		config_set_string(global_config, "SRTLA_AutoSwitch", "RulesJSON", jsonString.toUtf8().constData());

		QJsonArray visArr;
		for (int i = 0; i < visibilityRulesTable->rowCount(); i++) {
			QSpinBox *minSp = qobject_cast<QSpinBox *>(visibilityRulesTable->cellWidget(i, 0));
			QSpinBox *maxSp = qobject_cast<QSpinBox *>(visibilityRulesTable->cellWidget(i, 1));
			QComboBox *sourceCb = qobject_cast<QComboBox *>(visibilityRulesTable->cellWidget(i, 2));
			QComboBox *actionCb = qobject_cast<QComboBox *>(visibilityRulesTable->cellWidget(i, 3));

			if (minSp && maxSp && sourceCb && actionCb) {
				QJsonObject obj;
				obj["minKbps"] = minSp->value();
				obj["maxKbps"] = maxSp->value();
				obj["sourceName"] = sourceCb->currentText();
				obj["action"] = actionCb->currentText();
				visArr.append(obj);
			}
		}
		QJsonDocument visDoc(visArr);
		QString visJsonString = visDoc.toJson(QJsonDocument::Compact);

		config_set_string(global_config, "SRTLA_AutoSwitch", "VisibilityRulesJSON",
				  visJsonString.toUtf8().constData());

		config_save_safe(global_config, "tmp", nullptr);
	}

	// Restart or re-read settings in the background task
	SrtlaAutoSwitcher::instance().start();
	accept();
}

SrtlaAutoSwitcher::SrtlaAutoSwitcher(QObject *parent)
	: QObject(parent),
	  timer(new QTimer(this)),
	  currentMatchedRuleIndex(-1),
	  currentlyAppliedRuleIndex(-1),
	  matchDurationCounter(0),
	  currentMatchedVisRuleIndex(-1),
	  currentlyAppliedVisRuleIndex(-1),
	  visMatchDurationCounter(0)
{
	connect(timer, &QTimer::timeout, this, &SrtlaAutoSwitcher::checkBitrate);
	obs_frontend_add_event_callback(handleFrontendEvent, this);
}

SrtlaAutoSwitcher::~SrtlaAutoSwitcher()
{
	obs_frontend_remove_event_callback(handleFrontendEvent, this);
}

void SrtlaAutoSwitcher::handleFrontendEvent(enum obs_frontend_event event, void *private_data)
{
	SrtlaAutoSwitcher *switcher = static_cast<SrtlaAutoSwitcher *>(private_data);
	if (event == OBS_FRONTEND_EVENT_SCENE_CHANGED) {
		// If the user manually changes the scene while a rule is applied,
		// we check if they navigated away from the rule's target scene.
		if (switcher->currentlyAppliedRuleIndex >= 0 &&
		    switcher->currentlyAppliedRuleIndex < switcher->rules.size()) {
			const AutoSwitchRule &rule = switcher->rules[switcher->currentlyAppliedRuleIndex];

			obs_source_t *currentScene = obs_frontend_get_current_scene();
			const char *currentName = currentScene ? obs_source_get_name(currentScene) : nullptr;

			if (currentName && rule.targetScene != "[Return to Original Screen]" &&
			    QString::fromUtf8(currentName) != rule.targetScene) {
				// User manually navigated away from the auto-switched scene.
				// Reset state so we are back in "manual" mode.
				switcher->originalSceneName = "";
				switcher->currentlyAppliedRuleIndex = -1;
			}
			if (currentScene)
				obs_source_release(currentScene);
		}
	}
}

void SrtlaAutoSwitcher::loadRules()
{
	rules.clear();
	visibilityRules.clear();

	config_t *global_config = obs_frontend_get_profile_config();
	if (global_config) {
		const char *rulesJson = config_get_string(global_config, "SRTLA_AutoSwitch", "RulesJSON");
		if (rulesJson && *rulesJson) {
			QJsonDocument doc = QJsonDocument::fromJson(QByteArray(rulesJson));
			if (doc.isArray()) {
				QJsonArray arr = doc.array();
				for (int i = 0; i < arr.size(); i++) {
					QJsonObject obj = arr[i].toObject();
					AutoSwitchRule r;
					r.minKbps = obj["minKbps"].toInt();
					r.maxKbps = obj["maxKbps"].toInt();
					r.targetScene = obj["targetScene"].toString();
					rules.append(r);
				}
			}
		}

		const char *visRulesJson = config_get_string(global_config, "SRTLA_AutoSwitch", "VisibilityRulesJSON");
		if (visRulesJson && *visRulesJson) {
			QJsonDocument doc = QJsonDocument::fromJson(QByteArray(visRulesJson));
			if (doc.isArray()) {
				QJsonArray arr = doc.array();
				for (int i = 0; i < arr.size(); i++) {
					QJsonObject obj = arr[i].toObject();
					SourceVisibilityRule r;
					r.minKbps = obj["minKbps"].toInt();
					r.maxKbps = obj["maxKbps"].toInt();
					r.sourceName = obj["sourceName"].toString();
					r.action = obj["action"].toString();
					visibilityRules.append(r);
				}
			}
		}
	}
}

void SrtlaAutoSwitcher::start()
{
	loadRules();
	currentMatchedRuleIndex = -1;
	matchDurationCounter = 0;

	currentMatchedVisRuleIndex = -1;
	visMatchDurationCounter = 0;

	if (!timer->isActive()) {
		timer->start(1000); // Check every second
	}
}

void SrtlaAutoSwitcher::stop()
{
	timer->stop();
}

void SrtlaAutoSwitcher::checkBitrate()
{
	config_t *global_config = obs_frontend_get_profile_config();
	if (!global_config)
		return;

	bool enabled = config_get_bool(global_config, "SRTLA_AutoSwitch", "Enabled");
	bool visEnabled = config_get_bool(global_config, "SRTLA_AutoSwitch", "VisEnabled");

	if ((!enabled || rules.isEmpty()) && (!visEnabled || visibilityRules.isEmpty())) {
		currentMatchedRuleIndex = -1;
		matchDurationCounter = 0;
		currentMatchedVisRuleIndex = -1;
		visMatchDurationCounter = 0;
		return;
	}

	int delay = config_get_int(global_config, "SRTLA_AutoSwitch", "Delay");
	int visDelay = config_get_int(global_config, "SRTLA_AutoSwitch", "VisDelay");

	// Fetch stats
	bool is_listening = false;
	int groups = 0;
	int connections = 0;
	int listen_port = 0;
	int failed_conns = 0;
	char details_buffer[4096] = {0};

	srtla_get_connection_stats(&is_listening, &groups, &connections);
	srtla_get_connection_details(&listen_port, &failed_conns, details_buffer, sizeof(details_buffer));

	double totalKbps = 0;
	int activeGroupsWithData = 0;

	QJsonDocument doc = QJsonDocument::fromJson(QByteArray(details_buffer));
	if (doc.isObject()) {
		QJsonObject root = doc.object();
		QJsonArray groupsArray = root["groups"].toArray();
		for (int i = 0; i < groupsArray.size(); i++) {
			QJsonObject gObj = groupsArray[i].toObject();
			QString groupIdStr = QString::number(gObj["id"].toVariant().toULongLong());

			uint64_t gBytes = gObj["bytes"].toVariant().toULongLong();
			uint64_t gPrevBytes = previousBytes.value(groupIdStr, gBytes);
			previousBytes[groupIdStr] = gBytes;

			// We check every 1 second here
			double gKbps = ((gBytes - gPrevBytes) * 8.0) / 1000.0 / 1.0;
			totalKbps += gKbps;
			activeGroupsWithData++;
		}
	}

	// Clean up previousBytes for disconnected groups
	if (activeGroupsWithData == 0) {
		previousBytes.clear();
		totalKbps = 0; // Ensure 0 if no active data
	}

	if (enabled && !rules.isEmpty()) {
		// Find matching rule
		int matchedRule = -1;
		for (int i = 0; i < rules.size(); i++) {
			if (totalKbps >= rules[i].minKbps && (rules[i].maxKbps == 0 || totalKbps < rules[i].maxKbps)) {
				matchedRule = i;
				break; // Stop at first match
			}
		}

		if (matchedRule != currentMatchedRuleIndex) {
			// Bitrate changed to a different rule range
			currentMatchedRuleIndex = matchedRule;
			matchDurationCounter = 0;
		}

		if (currentMatchedRuleIndex >= 0) {
			matchDurationCounter++;
			if (matchDurationCounter >= delay && currentMatchedRuleIndex != currentlyAppliedRuleIndex) {
				// Apply rule
				const AutoSwitchRule &rule = rules[currentMatchedRuleIndex];

				if (rule.targetScene == "[Return to Original Screen]") {
					if (!originalSceneName.isEmpty()) {
						obs_source_t *prevSceneSrc =
							obs_get_source_by_name(originalSceneName.toUtf8().constData());
						if (prevSceneSrc) {
							obs_frontend_set_current_scene(prevSceneSrc);
							obs_source_release(prevSceneSrc);
						}
					}
					originalSceneName = ""; // Clear manual override state
					currentlyAppliedRuleIndex = currentMatchedRuleIndex;
				} else {
					obs_source_t *currentScene = obs_frontend_get_current_scene();
					if (currentScene) {
						const char *currentName = obs_source_get_name(currentScene);
						if (currentName && QString::fromUtf8(currentName) != rule.targetScene) {
							if (originalSceneName.isEmpty()) {
								// Only save the original scene if we weren't already auto-switched
								originalSceneName = QString::fromUtf8(currentName);
							}

							obs_source_t *targetSceneSrc = obs_get_source_by_name(
								rule.targetScene.toUtf8().constData());
							if (targetSceneSrc) {
								obs_frontend_set_current_scene(targetSceneSrc);
								obs_source_release(targetSceneSrc);
								currentlyAppliedRuleIndex = currentMatchedRuleIndex;
							}
						} else if (currentName &&
							   QString::fromUtf8(currentName) == rule.targetScene) {
							// Already on the target scene, just update index
							currentlyAppliedRuleIndex = currentMatchedRuleIndex;
						}
						obs_source_release(currentScene);
					}
				}
			}
		}
	}

	if (visEnabled && !visibilityRules.isEmpty()) {
		// Evaluate visibility rules
		int matchedVisRule = -1;
		for (int i = 0; i < visibilityRules.size(); i++) {
			if (totalKbps >= visibilityRules[i].minKbps &&
			    (visibilityRules[i].maxKbps == 0 || totalKbps < visibilityRules[i].maxKbps)) {
				matchedVisRule = i;
				break;
			}
		}

		if (matchedVisRule != currentMatchedVisRuleIndex) {
			currentMatchedVisRuleIndex = matchedVisRule;
			visMatchDurationCounter = 0;
		}

		if (currentMatchedVisRuleIndex >= 0) {
			visMatchDurationCounter++;
			if (visMatchDurationCounter >= visDelay &&
			    currentMatchedVisRuleIndex != currentlyAppliedVisRuleIndex) {
				const SourceVisibilityRule &rule = visibilityRules[currentMatchedVisRuleIndex];

				obs_source_t *currentSceneSource = obs_frontend_get_current_scene();
				if (currentSceneSource) {
					obs_scene_t *scene = obs_scene_from_source(currentSceneSource);
					if (scene) {
						obs_sceneitem_t *item = obs_scene_find_source_recursive(
							scene, rule.sourceName.toUtf8().constData());
						if (item) {
							obs_sceneitem_set_visible(item, (rule.action == "Show"));
						}
					}
					obs_source_release(currentSceneSource);
				}
				currentlyAppliedVisRuleIndex = currentMatchedVisRuleIndex;
			}
		}
	}
}

extern "C" void setup_srtla_menu()
{
	QMainWindow *mainWindow = (QMainWindow *)obs_frontend_get_main_window();
	if (!mainWindow)
		return;

	QMenuBar *menuBar = mainWindow->menuBar();
	QMenu *toolsMenu = mainWindow->findChild<QMenu *>("toolsMenu");

	QMenu *srtlaMenu = new QMenu("SRTLA Receiver", mainWindow);
	if (toolsMenu) {
		toolsMenu->addMenu(srtlaMenu);
	} else {
		menuBar->addMenu(srtlaMenu);
	}

	QAction *logsAction = srtlaMenu->addAction("View Detailed Logs");
	QObject::connect(logsAction, &QAction::triggered, []() {
		QString logPath = QString::fromLocal8Bit(qgetenv("APPDATA")) + "/obs-studio/logs";
		QDesktopServices::openUrl(QUrl::fromLocalFile(logPath));
	});

	srtlaMenu->addSeparator();

	QAction *startAction = srtlaMenu->addAction("Start All Listeners");
	QObject::connect(startAction, &QAction::triggered, []() { srtla_force_start_all(); });

	QAction *restartAction = srtlaMenu->addAction("Restart All Listeners");
	QObject::connect(restartAction, &QAction::triggered, []() { srtla_force_restart_all(); });

	QAction *stopAction = srtlaMenu->addAction("Stop All Listeners");
	QObject::connect(stopAction, &QAction::triggered, []() { srtla_force_stop_all(); });

	srtlaMenu->addSeparator();

	QAction *proxyAction = srtlaMenu->addAction("Reverse Proxy Settings...");
	QObject::connect(proxyAction, &QAction::triggered, [mainWindow]() {
		SrtlaReverseProxyDialog dialog(mainWindow);
		dialog.exec();
	});

	QAction *autoSwitchAction = srtlaMenu->addAction("Auto-Switch Settings...");
	QObject::connect(autoSwitchAction, &QAction::triggered, [mainWindow]() {
		SrtlaAutoSwitchDialog dialog(mainWindow);
		dialog.exec();
	});

	// Start proxy on initial load if enabled
	srtla_proxy_settings_changed();

	// Start auto-switcher on initial load
	SrtlaAutoSwitcher::instance().start();
}
