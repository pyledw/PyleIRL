#include "srtla-ui.hpp"
#include "srtla-web.hpp"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QFont>
#include <QScrollArea>
#include <QTableWidget>
#include <QPushButton>
#include <QTextBrowser>
#include <QCheckBox>
#include <QListWidget>
#include "multistream.hpp"

#include <QDesktopServices>
#include <QUrl>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QHeaderView>
#include <QResizeEvent>
#include <QScrollBar>

#include <obs.h>
#include <util/config-file.h>
#include <obs-frontend-api.h>
#include <QMessageBox>

#include <plugin-support.h>
#include <cmath>
#include <utility>

extern "C" {
void srtla_get_connection_stats(bool *is_listening, int *active_groups, int *active_connections);
void srtla_get_connection_details(int *listen_port, int *failed_conns, char *out_buffer, int max_len);
void srtla_get_all_receivers_json(char *out_buffer, int max_len);
void srtla_force_start_by_name(const char *name);
void srtla_force_stop_by_name(const char *name);
void srtla_force_restart_by_name(const char *name);
void srtla_force_reload_by_name(const char *name);
void srtla_force_reload_all();
char *srtla_get_frpc_path(void);
bool srtla_is_audio_starved(int listen_port);
extern "C" bool srtla_is_media_playing(int listen_port);
extern "C" bool rist_is_media_playing(int listen_port);
void srtla_auto_recover_hung_sources();

void rist_get_connection_stats(bool *is_listening, int *active_groups, int *active_connections);
void rist_get_connection_details(char *out_buffer, int max_len);
void rist_get_all_receivers_json(char *out_buffer, int max_len);
void rist_force_start_by_name(const char *name);
void rist_force_stop_by_name(const char *name);
void rist_force_restart_by_name(const char *name);
void rist_force_reload_by_name(const char *name);
}

SrtlaStatusWidget::SrtlaStatusWidget(QWidget *parent) : QWidget(parent)
{
	setObjectName("srtla_status_dock");
	setMinimumSize(170, 110);

	QVBoxLayout *mainLayout = new QVBoxLayout(this);
	mainLayout->setContentsMargins(6, 6, 6, 6);
	mainLayout->setSpacing(6);

	// Responsive modern summary header card
	metricsLabel = new QLabel(this);
	metricsLabel->setWordWrap(true);
	metricsLabel->setMinimumWidth(80);
	metricsLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
	metricsLabel->setStyleSheet(
		"QLabel { "
		"  background-color: #18191c; "
		"  border: 1px solid #2e3138; "
		"  border-left: 3px solid #4CAF50; "
		"  border-radius: 5px; "
		"  padding: 5px 8px; "
		"  font-size: 11px; "
		"  color: #dcddde; "
		"}");
	metricsLabel->setTextFormat(Qt::RichText);
	metricsLabel->setText("<span style='color:#4CAF50; font-weight:bold;'>● Listening</span> &nbsp;&bull;&nbsp; <b>Devices:</b> 0 &nbsp;&bull;&nbsp; <b>Links:</b> 0 &nbsp;&bull;&nbsp; <b>Total:</b> 0 Kbps");
	mainLayout->addWidget(metricsLabel);

	// Resizable vertical splitter between Receiver Controls and Connections
	splitter = new QSplitter(Qt::Vertical, this);
	splitter->setChildrenCollapsible(true);
	splitter->setHandleWidth(6);
	splitter->setStyleSheet(
		"QSplitter::handle:vertical { background-color: #2b2d31; height: 4px; margin: 1px 24px; border-radius: 2px; } "
		"QSplitter::handle:vertical:hover { background-color: #4CAF50; }");

	const QString sharedViewStyle =
		"QTableWidget, QTreeWidget { "
		"  background-color: #1b1c20; "
		"  alternate-background-color: #202226; "
		"  color: #dcddde; "
		"  border: 1px solid #2e3138; "
		"  border-radius: 5px; "
		"  font-size: 11px; "
		"  gridline-color: #26282e; "
		"  outline: none; "
		"} "
		"QTableWidget::item, QTreeWidget::item { "
		"  padding: 2px 5px; "
		"  border: none; "
		"} "
		"QTableWidget::item:hover, QTreeWidget::item:hover { "
		"  background-color: #282b30; "
		"} "
		"QHeaderView::section { "
		"  background-color: #23252b; "
		"  color: #b9bbbe; "
		"  padding: 4px 6px; "
		"  font-size: 10px; "
		"  font-weight: 600; "
		"  text-transform: uppercase; "
		"  border: none; "
		"  border-bottom: 1px solid #32353d; "
		"  border-right: 1px solid #2b2d33; "
		"} "
		"QScrollBar:vertical { "
		"  background: #18191c; width: 7px; margin: 0px; border-radius: 3px; "
		"} "
		"QScrollBar::handle:vertical { "
		"  background: #3a3d45; min-height: 20px; border-radius: 3px; "
		"} "
		"QScrollBar::handle:vertical:hover { background: #4e525c; } "
		"QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; } "
		"QScrollBar:horizontal { "
		"  background: #18191c; height: 7px; margin: 0px; border-radius: 3px; "
		"} "
		"QScrollBar::handle:horizontal { "
		"  background: #3a3d45; min-width: 20px; border-radius: 3px; "
		"} "
		"QScrollBar::handle:horizontal:hover { background: #4e525c; } "
		"QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; }";

	// Compact Receivers & Controls Table
	receiversTable = new QTableWidget(splitter);
	receiversTable->setColumnCount(3);
	receiversTable->setHorizontalHeaderLabels(QStringList() << "Receiver" << "Controls" << "Status & Health");
	receiversTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
	receiversTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	receiversTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
	receiversTable->horizontalHeader()->setStretchLastSection(true);
	receiversTable->horizontalHeader()->setMinimumSectionSize(44);
	receiversTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
	receiversTable->setSelectionMode(QAbstractItemView::NoSelection);
	receiversTable->setFocusPolicy(Qt::NoFocus);
	receiversTable->setAlternatingRowColors(true);
	receiversTable->setShowGrid(false);
	receiversTable->setWordWrap(false);
	receiversTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	receiversTable->setStyleSheet(sharedViewStyle);
	receiversTable->verticalHeader()->setVisible(false);
	receiversTable->verticalHeader()->setDefaultSectionSize(32);
	receiversTable->setMinimumHeight(48);
	receiversTable->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

	// Condensed Active Connections & KBPS Tree
	treeWidget = new QTreeWidget(splitter);
	treeWidget->setHeaderLabels(QStringList() << "Active Connection / Telemetry" << "Bitrate");
	treeWidget->header()->setSectionResizeMode(0, QHeaderView::Stretch);
	treeWidget->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	treeWidget->header()->setStretchLastSection(false);
	treeWidget->header()->setMinimumSectionSize(55);
	treeWidget->setAlternatingRowColors(true);
	treeWidget->setFocusPolicy(Qt::NoFocus);
	treeWidget->setIndentation(14);
	treeWidget->setMinimumHeight(44);
	treeWidget->setStyleSheet(sharedViewStyle);

	splitter->addWidget(receiversTable);
	splitter->addWidget(treeWidget);
	splitter->setStretchFactor(0, 1);
	splitter->setStretchFactor(1, 1);

	QSettings settings("obs-studio", "PyleIRL");
	QByteArray splitterState = settings.value("status_splitter_state_v2").toByteArray();
	if (!splitterState.isEmpty()) {
		splitter->restoreState(splitterState);
	} else {
		splitter->setSizes(QList<int>() << 115 << 150);
	}

	connect(splitter, &QSplitter::splitterMoved, this, &SrtlaStatusWidget::onSplitterMoved);

	mainLayout->addWidget(splitter, 1);

	// Set up timer
	updateTimer = new QTimer(this);
	connect(updateTimer, &QTimer::timeout, this, &SrtlaStatusWidget::updateStatus);
	updateTimer->start(500); // 500ms

	applyResponsiveLayout(width());
	updateStatus(); // initial update
}

SrtlaStatusWidget::~SrtlaStatusWidget()
{
	QSettings settings("obs-studio", "PyleIRL");
	if (splitter) {
		settings.setValue("status_splitter_state_v2", splitter->saveState());
	}
}

void SrtlaStatusWidget::onSplitterMoved(int pos, int index)
{
	Q_UNUSED(pos);
	Q_UNUSED(index);
	QSettings settings("obs-studio", "PyleIRL");
	if (splitter) {
		settings.setValue("status_splitter_state_v2", splitter->saveState());
	}
}

void SrtlaStatusWidget::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	applyResponsiveLayout(event->size().width());
}

void SrtlaStatusWidget::applyResponsiveLayout(int w)
{
	bool newCompact = (w < 430);
	bool newUltraCompact = (w < 300);

	if (layout()) {
		int m = newUltraCompact ? 3 : 6;
		layout()->setContentsMargins(m, m, m, m);
		layout()->setSpacing(newUltraCompact ? 4 : 6);
	}

	if (receiversTable && receiversTable->horizontalHeader()) {
		receiversTable->setColumnHidden(2, newUltraCompact);
		if (newUltraCompact) {
			receiversTable->setHorizontalHeaderLabels(QStringList() << "Receiver" << "Act" << "Status");
			receiversTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
			receiversTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
			receiversTable->horizontalHeader()->setStretchLastSection(false);
		} else if (newCompact) {
			receiversTable->setHorizontalHeaderLabels(QStringList() << "Receiver" << "Controls" << "Status");
			receiversTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
			receiversTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
			receiversTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
			receiversTable->horizontalHeader()->setStretchLastSection(true);
		} else {
			receiversTable->setHorizontalHeaderLabels(QStringList() << "Receiver" << "Controls" << "Status & Health");
			receiversTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
			receiversTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
			receiversTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
			receiversTable->horizontalHeader()->setStretchLastSection(true);
		}

		// Update existing row buttons immediately on resize
		for (int r = 0; r < receiversTable->rowCount(); ++r) {
			QWidget *actionWidget = receiversTable->cellWidget(r, 1);
			if (!actionWidget) continue;
			if (QHBoxLayout *al = qobject_cast<QHBoxLayout *>(actionWidget->layout())) {
				al->setContentsMargins(newUltraCompact ? 2 : 4, 2, newUltraCompact ? 2 : 4, 2);
				al->setSpacing(newUltraCompact ? 2 : 4);
			}
			QPushButton *startBtn = actionWidget->findChild<QPushButton *>("startBtn");
			QPushButton *stopBtn = actionWidget->findChild<QPushButton *>("stopBtn");
			QPushButton *restartBtn = actionWidget->findChild<QPushButton *>("restartBtn");
			QPushButton *fixBtn = actionWidget->findChild<QPushButton *>("fixBtn");
			if (startBtn) startBtn->setText(newCompact ? "▶" : "▶ Start");
			if (stopBtn) stopBtn->setText(newCompact ? "⏹" : "⏹ Stop");
			if (restartBtn) restartBtn->setText(newCompact ? "⟳" : "⟳ Restart");
			if (fixBtn) fixBtn->setText(newCompact ? "⚡" : "⚡ Fix A/V");
		}
	}

	if (treeWidget && treeWidget->header()) {
		if (newUltraCompact) {
			treeWidget->setHeaderLabels(QStringList() << "Link / Health" << "Kbps");
			treeWidget->setIndentation(10);
		} else if (newCompact) {
			treeWidget->setHeaderLabels(QStringList() << "Connection / Health" << "Bitrate");
			treeWidget->setIndentation(12);
		} else {
			treeWidget->setHeaderLabels(QStringList() << "Active Connection / Telemetry" << "Bitrate");
			treeWidget->setIndentation(14);
		}
	}

	bool modeChanged = (newCompact != isCompact) || (newUltraCompact != isUltraCompact);
	isCompact = newCompact;
	isUltraCompact = newUltraCompact;
	if (modeChanged) {
		updateStatus();
	}
}

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
		char details_buffer[16384] = {0};
		char receivers_buffer[4096] = {0};

		srtla_auto_recover_hung_sources();

		srtla_get_connection_stats(&is_listening, &groups, &connections);
		srtla_get_connection_details(&listen_port, &failed_conns, details_buffer, sizeof(details_buffer));

		bool rist_listening = false;
		int rist_groups = 0;
		int rist_conns = 0;
		char rist_details_buffer[16384] = {0};
		rist_get_connection_stats(&rist_listening, &rist_groups, &rist_conns);
		rist_get_connection_details(rist_details_buffer, sizeof(rist_details_buffer));

		is_listening = is_listening || rist_listening;
		groups += rist_groups;
		connections += rist_conns;

		srtla_get_all_receivers_json(receivers_buffer, sizeof(receivers_buffer));
		char rist_receivers_buffer[4096] = {0};
		rist_get_all_receivers_json(rist_receivers_buffer, sizeof(rist_receivers_buffer));

		double totalBitrateKbps = 0.0;

		QJsonDocument rDoc = QJsonDocument::fromJson(QByteArray(receivers_buffer));
		QJsonDocument ristRDoc = QJsonDocument::fromJson(QByteArray(rist_receivers_buffer));
		QJsonArray rArray;

		if (rDoc.isArray()) {
			rArray = rDoc.array();
		}
		if (ristRDoc.isArray()) {
			QJsonArray arr = ristRDoc.array();
			for (int i = 0; i < arr.size(); ++i) {
				rArray.append(arr[i]);
			}
		}

		if (!rArray.isEmpty()) {
			QSet<QString> currentReceiverNames;
			for (int i = 0; i < rArray.size(); ++i) {
				QJsonObject rObj = rArray[i].toObject();
				QString name = rObj["name"].toString();
				QString port = QString::number(rObj["listen_port"].toInt());
				bool running = rObj["running"].toVariant().toBool();
				QString protocol = rObj.contains("protocol") ? rObj["protocol"].toString().toUpper() : "SRTLA";
				long long drift_ms = rObj.contains("audio_drift_ms") ? (long long)rObj["audio_drift_ms"].toDouble() : 0;
				int resets = rObj.contains("auto_reset_count") ? rObj["auto_reset_count"].toInt() : 0;
				bool audio_starved = rObj.contains("audio_starved") && rObj["audio_starved"].toVariant().toBool();

				currentReceiverNames.insert(name);

				int foundRow = -1;
				for (int r = 0; r < receiversTable->rowCount(); ++r) {
					QTableWidgetItem *item = receiversTable->item(r, 0);
					if (item && item->data(Qt::UserRole).toString() == name) {
						foundRow = r;
						break;
					}
				}

				if (foundRow == -1) {
					foundRow = receiversTable->rowCount();
					receiversTable->insertRow(foundRow);
					receiversTable->setRowHeight(foundRow, 32);

					QTableWidgetItem *nameItem = new QTableWidgetItem();
					nameItem->setData(Qt::UserRole, name);
					nameItem->setTextAlignment(Qt::AlignVCenter | Qt::AlignLeft);

					QTableWidgetItem *statusItem = new QTableWidgetItem();
					statusItem->setTextAlignment(Qt::AlignVCenter | Qt::AlignLeft);

					receiversTable->setItem(foundRow, 0, nameItem);
					receiversTable->setItem(foundRow, 2, statusItem);

					QWidget *actionWidget = new QWidget();
					actionWidget->setStyleSheet("background: transparent;");
					QHBoxLayout *actionLayout = new QHBoxLayout(actionWidget);
					actionLayout->setContentsMargins(isUltraCompact ? 2 : 4, 2, isUltraCompact ? 2 : 4, 2);
					actionLayout->setSpacing(isUltraCompact ? 2 : 4);

					QPushButton *startBtn = new QPushButton(isCompact ? "▶" : "▶ Start");
					QPushButton *stopBtn = new QPushButton(isCompact ? "⏹" : "⏹ Stop");
					QPushButton *restartBtn = new QPushButton(isCompact ? "⟳" : "⟳ Restart");
					QPushButton *fixBtn = new QPushButton(isCompact ? "⚡" : "⚡ Fix A/V");

					startBtn->setObjectName("startBtn");
					stopBtn->setObjectName("stopBtn");
					restartBtn->setObjectName("restartBtn");
					fixBtn->setObjectName("fixBtn");

					startBtn->setCursor(Qt::PointingHandCursor);
					stopBtn->setCursor(Qt::PointingHandCursor);
					restartBtn->setCursor(Qt::PointingHandCursor);
					fixBtn->setCursor(Qt::PointingHandCursor);

					startBtn->setToolTip("Start this receiver");
					stopBtn->setToolTip("Stop this receiver");
					restartBtn->setToolTip("Full restart of the receiver thread and connection");
					fixBtn->setToolTip("Rapidly reload the internal player to clear audio lag / desync without dropping the SRTLA connection");

					startBtn->setStyleSheet(
						"QPushButton { background-color: #238636; color: #ffffff; font-weight: 600; border: 1px solid #2ea043; border-radius: 4px; padding: 2px 7px; font-size: 11px; min-width: 18px; } "
						"QPushButton:hover { background-color: #2ea043; } "
						"QPushButton:disabled { background-color: #23252b; color: #555860; border: 1px solid #2b2e36; }");
					stopBtn->setStyleSheet(
						"QPushButton { background-color: #da3633; color: #ffffff; font-weight: 600; border: 1px solid #f85149; border-radius: 4px; padding: 2px 7px; font-size: 11px; min-width: 18px; } "
						"QPushButton:hover { background-color: #f85149; } "
						"QPushButton:disabled { background-color: #23252b; color: #555860; border: 1px solid #2b2e36; }");
					restartBtn->setStyleSheet(
						"QPushButton { background-color: #2b2d34; color: #dcddde; border: 1px solid #3c3f49; border-radius: 4px; padding: 2px 6px; font-size: 11px; min-width: 18px; } "
						"QPushButton:hover { background-color: #383b44; color: #ffffff; border-color: #4f5461; } "
						"QPushButton:disabled { background-color: #202226; color: #4c4f57; border: 1px solid #282a30; }");
					fixBtn->setStyleSheet(
						"QPushButton { background-color: #2b2d34; color: #f0b232; border: 1px solid #3c3f49; border-radius: 4px; padding: 2px 6px; font-size: 11px; min-width: 18px; } "
						"QPushButton:hover { background-color: #383b44; color: #ffd166; border-color: #f0b232; } "
						"QPushButton:disabled { background-color: #202226; color: #4c4f57; border: 1px solid #282a30; }");

					QObject::connect(startBtn, &QPushButton::clicked,
							 [name, protocol]() {
							 	if (protocol == "RIST") rist_force_start_by_name(name.toUtf8().constData());
							 	else srtla_force_start_by_name(name.toUtf8().constData());
							 });
					QObject::connect(stopBtn, &QPushButton::clicked,
							 [name, protocol]() {
							 	if (protocol == "RIST") rist_force_stop_by_name(name.toUtf8().constData());
							 	else srtla_force_stop_by_name(name.toUtf8().constData());
							 });
					QObject::connect(restartBtn, &QPushButton::clicked,
							 [name, protocol]() {
							 	if (protocol == "RIST") rist_force_restart_by_name(name.toUtf8().constData());
							 	else srtla_force_restart_by_name(name.toUtf8().constData());
							 });
					QObject::connect(fixBtn, &QPushButton::clicked,
							 [name, protocol]() {
							 	if (protocol == "RIST") rist_force_reload_by_name(name.toUtf8().constData());
							 	else srtla_force_reload_by_name(name.toUtf8().constData());
							 });

					actionLayout->addWidget(startBtn);
					actionLayout->addWidget(stopBtn);
					actionLayout->addWidget(restartBtn);
					actionLayout->addWidget(fixBtn);
					receiversTable->setCellWidget(foundRow, 1, actionWidget);
				}

				QString fullTooltip = QString("Protocol: %1\nReceiver: %2\nListen Port: %3\nStatus: %4\nAudio Drift: %5 ms\nAuto-Resets: %6\nAudio Starved: %7")
					.arg(protocol, name, port)
					.arg(running ? "Running" : "Stopped")
					.arg(drift_ms)
					.arg(resets)
					.arg(audio_starved ? "Yes" : "No");

				QColor stateColor("#888888");
				QString stateDot = "○";
				if (running) {
					if (audio_starved || std::abs(drift_ms) > 500) {
						stateColor = QColor("#f85149"); // Red
						stateDot = "⚠️";
					} else if (std::abs(drift_ms) > 100) {
						stateColor = QColor("#f0b232"); // Amber
						stateDot = "●";
					} else {
						stateColor = QColor("#3fb950"); // Green
						stateDot = "●";
					}
				}

				QTableWidgetItem *nameItem = receiversTable->item(foundRow, 0);
				if (nameItem) {
					if (isUltraCompact) {
						nameItem->setText(QString("%1 %2 :%3").arg(stateDot, name, port));
						nameItem->setForeground(QBrush(running ? stateColor : QColor("#999999")));
					} else if (isCompact) {
						nameItem->setText(QString("%1 :%2").arg(name, port));
						nameItem->setForeground(QBrush(QColor("#dcddde")));
					} else {
						nameItem->setText(QString("[%1] %2 (%3)").arg(protocol, name, port));
						nameItem->setForeground(QBrush(QColor("#dcddde")));
					}
					nameItem->setToolTip(fullTooltip);
				}

				QTableWidgetItem *statusItem = receiversTable->item(foundRow, 2);
				if (statusItem) {
					QString statusText;
					if (running) {
						if (audio_starved || std::abs(drift_ms) > 500) {
							statusText = isCompact
								? QString("⚠️ %1ms (R:%2)").arg(drift_ms).arg(resets)
								: QString("⚠️ Desync (%1ms) • Resets: %2").arg(drift_ms).arg(resets);
						} else {
							statusText = isCompact
								? QString("● %1ms (R:%2)").arg(drift_ms).arg(resets)
								: QString("● Running • %1ms drift • Resets: %2").arg(drift_ms).arg(resets);
						}
					} else {
						statusText = "○ Stopped";
					}
					statusItem->setForeground(QBrush(stateColor));
					statusItem->setText(statusText);
					statusItem->setToolTip(fullTooltip);
				}

				QWidget *actionWidget = receiversTable->cellWidget(foundRow, 1);
				if (actionWidget) {
					QPushButton *startBtn = actionWidget->findChild<QPushButton*>("startBtn");
					QPushButton *stopBtn = actionWidget->findChild<QPushButton*>("stopBtn");
					QPushButton *restartBtn = actionWidget->findChild<QPushButton*>("restartBtn");
					QPushButton *fixBtn = actionWidget->findChild<QPushButton*>("fixBtn");
					if (startBtn && stopBtn) {
						if (isCompact) {
							// In compact/ultra-compact mode, only show the actionable toggle button to save horizontal space
							startBtn->setVisible(!running);
							stopBtn->setVisible(running);
						} else {
							startBtn->setVisible(true);
							stopBtn->setVisible(true);
						}
						startBtn->setEnabled(!running);
						stopBtn->setEnabled(running);
					}
					if (restartBtn) restartBtn->setEnabled(running);
					if (fixBtn) fixBtn->setEnabled(running);
				}
			}

			// Remove stale receivers
			for (int r = receiversTable->rowCount() - 1; r >= 0; r--) {
				QTableWidgetItem *item = receiversTable->item(r, 0);
				if (item && !currentReceiverNames.contains(item->data(Qt::UserRole).toString())) {
					receiversTable->removeRow(r);
				}
			}
		} else {
			receiversTable->setRowCount(0);
		}

		QJsonDocument doc = QJsonDocument::fromJson(QByteArray(details_buffer));
		QJsonDocument ristDoc = QJsonDocument::fromJson(QByteArray(rist_details_buffer));

		if (doc.isObject() || ristDoc.isObject()) {
			QJsonObject root = doc.object();
			QJsonArray groupsArray = root["groups"].toArray();

			if (ristDoc.isObject()) {
				QJsonArray ristGroups = ristDoc.object()["groups"].toArray();
				for (int i = 0; i < ristGroups.size(); i++) {
					groupsArray.append(ristGroups[i]);
				}
			}

			// Keep track of which items exist to remove stale ones
			QSet<QString> currentGroupIds;

			for (int i = 0; i < groupsArray.size(); i++) {
				QJsonObject gObj = groupsArray[i].toObject();
				QString groupIdStr = QString::number(gObj["id"].toVariant().toULongLong());
				QString listenPortStr = QString::number(gObj["listen_port"].toInt());
				QString uniqueGroupIdStr = listenPortStr + "_" + groupIdStr;
				currentGroupIds.insert(uniqueGroupIdStr);

				QString nodeName = isUltraCompact
					? QString(":%1 (#%2)").arg(listenPortStr, groupIdStr)
					: (isCompact
						? QString("Port %1 (#%2)").arg(listenPortStr, groupIdStr)
						: QString("Port %1 (Device #%2)").arg(listenPortStr, groupIdStr));

				QTreeWidgetItem *groupItem = nullptr;
				for (int k = 0; k < treeWidget->topLevelItemCount(); ++k) {
					if (treeWidget->topLevelItem(k)->data(0, Qt::UserRole).toString() == uniqueGroupIdStr) {
						groupItem = treeWidget->topLevelItem(k);
						break;
					}
				}

				if (!groupItem) {
					groupItem = new QTreeWidgetItem(treeWidget);
					groupItem->setData(0, Qt::UserRole, uniqueGroupIdStr);
					groupItem->setExpanded(true);
				}

				if (gObj.contains("quality")) {
					double quality = gObj["quality"].toDouble();
					int lost = gObj["lost"].toInt();
					int retries = gObj["retries"].toInt();
					int missing_q = gObj["missing_queue"].toInt();
					int rtt = gObj["rtt"].toInt();

					if (isUltraCompact) {
						nodeName += QString(" • Q:%1%").arg(quality, 0, 'f', 0);
						if (rtt > 0) nodeName += QString(" • %1ms").arg(rtt);
					} else if (isCompact) {
						nodeName += QString(" • Q:%1% • L:%2").arg(quality, 0, 'f', 0).arg(lost);
						if (rtt > 0) nodeName += QString(" • %1ms").arg(rtt);
					} else {
						nodeName += QString("  •  Q: %1%  •  L: %2  •  Buf: %3ms")
							.arg(quality, 0, 'f', 0)
							.arg(lost)
							.arg(missing_q);
						if (retries > 0) {
							nodeName += QString("  •  R: %1").arg(retries);
						}
						if (rtt > 0) {
							nodeName += QString("  •  %1ms").arg(rtt);
						}
					}
					groupItem->setToolTip(0, QString("Device #%1 (Port %2)\nLink Quality: %3%\nLost Packets: %4\nRetries: %5\nQueue Buffer: %6 ms\nRTT: %7 ms")
						.arg(groupIdStr)
						.arg(listenPortStr)
						.arg(quality, 0, 'f', 1)
						.arg(lost)
						.arg(retries)
						.arg(missing_q)
						.arg(rtt));
				}

				if (srtla_is_audio_starved(listenPortStr.toInt())) {
					groupItem->setText(0, nodeName + (isUltraCompact ? " ⚠️" : "  [⚠️ DESYNC]"));
					groupItem->setForeground(0, QBrush(QColor("#f85149")));
				} else {
					groupItem->setText(0, nodeName);
					groupItem->setForeground(0, QBrush(QColor("#dcddde")));
				}

				QJsonArray connsArray = gObj["conns"].toArray();
				QSet<QString> currentConnIds;
				double calculatedSumKbps = 0.0;

				for (int j = 0; j < connsArray.size(); j++) {
					QJsonObject cObj = connsArray[j].toObject();
					QString ip = cObj["ip"].toString();
					QString port = QString::number(cObj["port"].toInt());
					QString connIdStr = groupIdStr + "_" + ip + ":" + port;
					currentConnIds.insert(connIdStr);

					uint64_t cBytes = cObj["bytes"].toVariant().toULongLong();
					uint64_t cPrevBytes = previousBytes.value(connIdStr, cBytes);
					if (cBytes < cPrevBytes) { cPrevBytes = cBytes; }
					previousBytes[connIdStr] = cBytes;

					double cKbps = ((cBytes - cPrevBytes) * 8.0) / 1000.0 / 0.5;
					calculatedSumKbps += cKbps;

					QTreeWidgetItem *connItem = nullptr;
					for (int k = 0; k < groupItem->childCount(); k++) {
						if (groupItem->child(k)->text(0) == (ip + ":" + port)) {
							connItem = groupItem->child(k);
							break;
						}
					}
					if (!connItem) {
						connItem = new QTreeWidgetItem(groupItem);
						connItem->setText(0, ip + ":" + port);
						connItem->setForeground(0, QBrush(QColor("#9ca3af")));
					}
					connItem->setText(1, QString::number(cKbps, 'f', isUltraCompact ? 0 : 1) + " Kbps");
					connItem->setForeground(1, QBrush(cKbps > 10.0 ? QColor("#3fb950") : QColor("#888888")));
				}

				totalBitrateKbps += calculatedSumKbps;
				groupItem->setText(1, QString::number(calculatedSumKbps, 'f', isUltraCompact ? 0 : 1) + " Kbps");
				groupItem->setForeground(1, QBrush(calculatedSumKbps > 10.0 ? QColor("#3fb950") : QColor("#b9bbbe")));

				// Remove disconnected children
				for (int k = groupItem->childCount() - 1; k >= 0; k--) {
					QString linkText = groupItem->child(k)->text(0);
					QString connIdStr = groupIdStr + "_" + linkText;
					if (!currentConnIds.contains(connIdStr)) {
						delete groupItem->takeChild(k);
					}
				}
			}

			// Remove disconnected groups
			for (int i = treeWidget->topLevelItemCount() - 1; i >= 0; i--) {
				QTreeWidgetItem *item = treeWidget->topLevelItem(i);
				QString uniqueIdStr = item->data(0, Qt::UserRole).toString();
				if (!currentGroupIds.contains(uniqueIdStr) || uniqueIdStr.isEmpty()) {
					delete treeWidget->takeTopLevelItem(i);
				}
			}
		}

		QString borderAccent = is_listening ? (connections > 0 ? "#3fb950" : "#2ea043") : "#555860";
		metricsLabel->setStyleSheet(QString(
			"QLabel { "
			"  background-color: #18191c; "
			"  border: 1px solid #2e3138; "
			"  border-left: 3px solid %1; "
			"  border-radius: 5px; "
			"  padding: %2; "
			"  font-size: 11px; "
			"  color: #dcddde; "
			"}")
			.arg(borderAccent)
			.arg(isUltraCompact ? "3px 6px" : "5px 8px"));

		QString summaryText;
		if (isUltraCompact) {
			summaryText = QString("<span style='color:%1; font-weight:bold;'>● %2</span> &bull; <b>%3</b>D/<b>%4</b>L &bull; <span style='color:#3fb950; font-weight:bold;'>%5k</span>")
				.arg(is_listening ? "#3fb950" : "#888888")
				.arg(is_listening ? "Live" : "Idle")
				.arg(groups)
				.arg(connections)
				.arg(QString::number(totalBitrateKbps, 'f', 0));
		} else if (isCompact) {
			summaryText = QString("<span style='color:%1; font-weight:bold;'>● %2</span> &nbsp;&bull;&nbsp; <b>Dev:</b> %3 &nbsp;&bull;&nbsp; <b>Links:</b> %4 &nbsp;&bull;&nbsp; <span style='color:#3fb950; font-weight:bold;'>%5 Kbps</span>")
				.arg(is_listening ? "#3fb950" : "#888888")
				.arg(is_listening ? "Listening" : "Idle")
				.arg(groups)
				.arg(connections)
				.arg(QString::number(totalBitrateKbps, 'f', 0));
		} else {
			summaryText = QString("<span style='color:%1; font-weight:bold;'>● %2</span> &nbsp;&bull;&nbsp; <b>Devices:</b> %3 &nbsp;&bull;&nbsp; <b>Links:</b> %4 &nbsp;&bull;&nbsp; <b>Total:</b> <span style='color:#3fb950; font-weight:bold;'>%5 Kbps</span>")
				.arg(is_listening ? "#3fb950" : "#888888")
				.arg(is_listening ? "Listening" : "Idle")
				.arg(groups)
				.arg(connections)
				.arg(QString::number(totalBitrateKbps, 'f', 0));
		}
		metricsLabel->setText(summaryText);
		metricsLabel->setToolTip(QString("Status: %1\nConnected Devices: %2\nActive Sub-Links: %3\nCombined Bitrate: %4 Kbps")
			.arg(is_listening ? "Listening" : "Idle")
			.arg(groups)
			.arg(connections)
			.arg(QString::number(totalBitrateKbps, 'f', 1)));

	} catch (...) {
		metricsLabel->setText("<span style='color:#f85149; font-weight:bold;'>● Backend Error</span>");
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
#include <QPixmap>

static void addLogoToLayout(QVBoxLayout *layout)
{
	layout->addSpacing(10);

	QFrame *line = new QFrame();
	line->setFrameShape(QFrame::HLine);
	line->setFrameShadow(QFrame::Sunken);
	line->setStyleSheet("color: #3a3a3a;");
	layout->addWidget(line);

	layout->addSpacing(5);

	QHBoxLayout *bottomLayout = new QHBoxLayout();
	bottomLayout->setAlignment(Qt::AlignCenter);
	bottomLayout->setSpacing(8);

	QLabel *logoLabel = new QLabel();
	QPixmap pixmap(":/pyle-logo.png");
	if (!pixmap.isNull()) {
		logoLabel->setPixmap(pixmap.scaledToHeight(20, Qt::SmoothTransformation));
	}
	bottomLayout->addWidget(logoLabel);

	QLabel *textLabel = new QLabel("Built for Streamers by Streamers");
	textLabel->setStyleSheet("font-size: 10px; color: #888888; font-style: italic;");
	bottomLayout->addWidget(textLabel);

	layout->addLayout(bottomLayout);
}

SrtlaAboutDialog::SrtlaAboutDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle("About PyleIRL");
	setMinimumSize(450, 420);

	QVBoxLayout *mainLayout = new QVBoxLayout(this);

	// Logo
	QLabel *logoLabel = new QLabel();
	QPixmap pixmap(":/pyle-logo.png");
	if (!pixmap.isNull()) {
		logoLabel->setPixmap(pixmap.scaledToHeight(64, Qt::SmoothTransformation));
		logoLabel->setAlignment(Qt::AlignCenter);
		mainLayout->addWidget(logoLabel);
	}

	// App Title & Description
	QLabel *titleLabel = new QLabel(QString("PyleIRL OBS Plugin\nVersion %1").arg(PLUGIN_VERSION));
	titleLabel->setAlignment(Qt::AlignCenter);
	titleLabel->setStyleSheet("font-weight: bold; font-size: 14px;");
	mainLayout->addWidget(titleLabel);

	QLabel *descLabel = new QLabel("Developed by PyleAdventures to improve IRL livestreaming.");
	descLabel->setAlignment(Qt::AlignCenter);
	descLabel->setStyleSheet("color: #aaaaaa; margin-bottom: 10px;");
	mainLayout->addWidget(descLabel);

	// Attribution Header
	QLabel *attrHeader = new QLabel("Open Source Licenses & Attributions:");
	attrHeader->setStyleSheet("font-weight: bold; font-size: 12px;");
	mainLayout->addWidget(attrHeader);

	// Attributions text browser
	QTextBrowser *browser = new QTextBrowser();
	browser->setOpenExternalLinks(true);

	QString html =
		"<h3>Attributions & Credits</h3>"
		"<p>This plugin is built using the following open source libraries and components:</p>"
		"<ul>"
		"<li><b>BELABOX srtla</b><br/>"
		"A multi-link bonding transport proxy for connection aggregation.<br/>"
		"License: GNU Affero General Public License v3.0 (AGPL-3.0)<br/>"
		"Repository: <a href=\"https://github.com/BELABOX/srtla\">github.com/BELABOX/srtla</a></li><br/>"
		"<li><b>frp (Fast Reverse Proxy)</b><br/>"
		"A fast reverse proxy to help expose local servers to the internet.<br/>"
		"License: Apache License 2.0<br/>"
		"Repository: <a href=\"https://github.com/fatedier/frp\">github.com/fatedier/frp</a></li><br/>"
		"<li><b>cpp-httplib</b><br/>"
		"A C++ header-only HTTP/HTTPS server and client library by yhirose.<br/>"
		"License: MIT License<br/>"
		"Repository: <a href=\"https://github.com/yhirose/cpp-httplib\">github.com/yhirose/cpp-httplib</a></li><br/>"
		"<li><b>OBS Studio API (libobs & obs-frontend-api)</b><br/>"
		"The core plugin API of Open Broadcaster Software.<br/>"
		"License: GNU General Public License v2.0 (GPL-2.0)<br/>"
		"Repository: <a href=\"https://github.com/obsproject/obs-studio\">github.com/obsproject/obs-studio</a></li><br/>"
		"<li><b>Qt Framework</b><br/>"
		"Cross-platform software development framework for the UI.<br/>"
		"License: LGPLv3 / GPLv3<br/>"
		"Website: <a href=\"https://www.qt.io/\">qt.io</a></li>"
		"</ul>";

	browser->setHtml(html);
	mainLayout->addWidget(browser);

	// OK button
	QDialogButtonBox *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok);
	connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
	mainLayout->addWidget(buttonBox);
}

SrtlaReverseProxyDialog::SrtlaReverseProxyDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle("SRTLA Reverse Proxy (FRP) Settings");
	setMinimumWidth(400);

	QVBoxLayout *mainLayout = new QVBoxLayout(this);
	QFormLayout *formLayout = new QFormLayout();

	enableProxy = new QComboBox();
	enableProxy->addItem("Disabled");
	enableProxy->addItem("Enabled");

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

	formLayout->addRow("Enable Reverse Proxy Tunnel:", enableProxy);
	formLayout->addRow("Server Address:", serverAddress);
	formLayout->addRow("Server Port:", serverPort);
	formLayout->addRow("Auth Token:", authToken);
	formLayout->addRow("Forward Ports:", forwardPorts);

	mainLayout->addLayout(formLayout);

	QDialogButtonBox *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	mainLayout->addWidget(buttonBox);
	addLogoToLayout(mainLayout);

	connect(buttonBox, &QDialogButtonBox::accepted, this, &SrtlaReverseProxyDialog::saveSettings);
	connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

	// Load existing settings
	config_t *global_config = obs_frontend_get_profile_config();
	if (global_config) {
		enableProxy->setCurrentIndex(config_get_bool(global_config, "SRTLA_Proxy", "Enabled") ? 1 : 0);
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
		config_set_bool(global_config, "SRTLA_Proxy", "Enabled", enableProxy->currentIndex() == 1);
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
void rist_force_stop_all();
void rist_force_start_all();
void rist_force_restart_all();
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
#include <QSet>
#include <obs-audio-controls.h>
#include <util/platform.h>

SrtlaAutoSwitchDialog::SrtlaAutoSwitchDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle("PyleIRL Automation Settings");
	setMinimumWidth(650);

	QVBoxLayout *mainLayout = new QVBoxLayout(this);

	QTabWidget *tabs = new QTabWidget();

	QWidget *sceneTab = new QWidget();
	QFormLayout *sceneLayout = new QFormLayout(sceneTab);

	enableAutoSwitch = new QComboBox();
	enableAutoSwitch->addItem("Disabled");
	enableAutoSwitch->addItem("Enabled");

	switchDelay = new QSpinBox();
	switchDelay->setRange(0, 60);
	switchDelay->setSuffix(" seconds");
	switchDelay->setValue(4); // Default 4 seconds

	returnDelay = new QSpinBox();
	returnDelay->setRange(1, 60);
	returnDelay->setSuffix(" seconds");
	returnDelay->setValue(5); // Default 5 seconds

	failoverSceneBox = new QComboBox();

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

	// Populate audio sources
	obs_enum_sources(
		[](void *data, obs_source_t *source) {
			QStringList *list = static_cast<QStringList *>(data);
			uint32_t flags = obs_source_get_output_flags(source);
			if (flags & OBS_SOURCE_AUDIO) {
				const char *name = obs_source_get_name(source);
				if (name) {
					QString nameStr = QString::fromUtf8(name);
					if (!list->contains(nameStr))
						list->append(nameStr);
				}
			}
			return true;
		},
		&availableAudioSources);
	availableAudioSources.sort();

	for (const QString &sceneName : availableScenes) {
		// primarySceneBox is gone
		failoverSceneBox->addItem(sceneName);
	}

	sceneLayout->addRow("Enable Media Auto-Switch:", enableAutoSwitch);
	sceneLayout->addRow("Failover (Disconnected) Scene:", failoverSceneBox);
	sceneLayout->addRow("Switch Delay (Failover only):", switchDelay);
	sceneLayout->addRow("Return Delay (Restore Live):", returnDelay);

	noFailoverList = new QListWidget();
	noFailoverList->setMaximumHeight(120);
	for (const QString &sceneName : availableScenes) {
		QListWidgetItem *item = new QListWidgetItem(sceneName, noFailoverList);
		item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
		item->setCheckState(Qt::Unchecked);
	}
	sceneLayout->addRow("No Failover Scenes:\n(Auto-switch inactive on these scenes)", noFailoverList);

	tabs->addTab(sceneTab, "Scene Switching");

	QWidget *visTab = new QWidget();
	QFormLayout *visLayout = new QFormLayout(visTab);

	enableVisSwitch = new QComboBox();
	enableVisSwitch->addItem("Disabled");
	enableVisSwitch->addItem("Enabled");

	visSwitchDelay = new QSpinBox();
	visSwitchDelay->setRange(0, 60);
	visSwitchDelay->setSuffix(" seconds");
	visSwitchDelay->setValue(2); // Default 2 seconds

	visibilityRulesTable = new QTableWidget();
	visibilityRulesTable->setColumnCount(5);
	visibilityRulesTable->setHorizontalHeaderLabels(QStringList() << "Min Kbps" << "Max Kbps (0=unlimited)"
								      << "Tracker Port" << "Source Name" << "");
	visibilityRulesTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
	visibilityRulesTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);

	QPushButton *addVisRuleBtn = new QPushButton("Add Visibility Rule");
	connect(addVisRuleBtn, &QPushButton::clicked, this, &SrtlaAutoSwitchDialog::addNewVisibilityRule);

	visLayout->addRow("Enable Range-Based Source Visibility:", enableVisSwitch);
	visLayout->addRow("Switch Delay:", visSwitchDelay);
	visLayout->addRow(visibilityRulesTable);
	visLayout->addRow(addVisRuleBtn);

	tabs->addTab(visTab, "Source Visibility");

	// Dynamic Volume Sources Tab
	QWidget *volTab = new QWidget();
	QFormLayout *volLayout = new QFormLayout(volTab);

	enableVolSwitch = new QComboBox();
	enableVolSwitch->addItem("Disabled");
	enableVolSwitch->addItem("Enabled");

	volSwitchDelay = new QSpinBox();
	volSwitchDelay->setRange(0, 60);
	volSwitchDelay->setSuffix(" seconds");
	volSwitchDelay->setValue(2); // Default 2 seconds

	volumeRulesTable = new QTableWidget();
	volumeRulesTable->setColumnCount(5);
	volumeRulesTable->setHorizontalHeaderLabels(QStringList() << "Audio Source" << "Min dB" << "Max dB"
								  << "Target Layer / Source" << "");
	volumeRulesTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
	volumeRulesTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);

	QPushButton *addVolRuleBtn = new QPushButton("Add Dynamic Volume Rule");
	connect(addVolRuleBtn, &QPushButton::clicked, this, &SrtlaAutoSwitchDialog::addNewVolumeRule);

	volLayout->addRow("Enable Dynamic Volume Sources:", enableVolSwitch);
	volLayout->addRow("Switch Delay:", volSwitchDelay);
	volLayout->addRow(volumeRulesTable);
	volLayout->addRow(addVolRuleBtn);

	tabs->addTab(volTab, "Dynamic Volume Sources");

	mainLayout->addWidget(tabs);

	QDialogButtonBox *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	mainLayout->addWidget(buttonBox);
	addLogoToLayout(mainLayout);

	connect(buttonBox, &QDialogButtonBox::accepted, this, &SrtlaAutoSwitchDialog::saveSettings);
	connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

	// Load existing settings
	config_t *global_config = obs_frontend_get_profile_config();
	if (global_config) {
		enableAutoSwitch->setCurrentIndex(config_get_bool(global_config, "SRTLA_AutoSwitch", "Enabled") ? 1
														: 0);
		enableVisSwitch->setCurrentIndex(config_get_bool(global_config, "SRTLA_AutoSwitch", "VisEnabled") ? 1
														  : 0);
		enableVolSwitch->setCurrentIndex(config_get_bool(global_config, "SRTLA_AutoSwitch", "VolEnabled") ? 1
														  : 0);

		int delay = config_get_int(global_config, "SRTLA_AutoSwitch", "Delay");
		if (config_has_user_value(global_config, "SRTLA_AutoSwitch", "Delay")) {
			switchDelay->setValue(delay);
		} else {
			switchDelay->setValue(4);
		}

		int rDelay = config_get_int(global_config, "SRTLA_AutoSwitch", "ReturnDelay");
		if (config_has_user_value(global_config, "SRTLA_AutoSwitch", "ReturnDelay")) {
			returnDelay->setValue(rDelay);
		} else {
			returnDelay->setValue(5);
		}

		const char *failoverStr = config_get_string(global_config, "SRTLA_AutoSwitch", "FailoverScene");
		if (failoverStr && *failoverStr) {
			int idx = failoverSceneBox->findText(QString::fromUtf8(failoverStr));
			if (idx >= 0) failoverSceneBox->setCurrentIndex(idx);
		}

		const char *noFailoverJson = config_get_string(global_config, "SRTLA_AutoSwitch", "NoFailoverScenes");
		if (noFailoverJson && *noFailoverJson) {
			QJsonDocument doc = QJsonDocument::fromJson(QByteArray(noFailoverJson));
			if (doc.isArray()) {
				QJsonArray arr = doc.array();
				QSet<QString> savedNoFailover;
				for (int i = 0; i < arr.size(); i++) {
					savedNoFailover.insert(arr[i].toString());
				}
				for (int i = 0; i < noFailoverList->count(); i++) {
					QListWidgetItem *item = noFailoverList->item(i);
					if (item && savedNoFailover.contains(item->text())) {
						item->setCheckState(Qt::Checked);
					}
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
							     obj["trackerSource"].toString(),
							     obj["sourceName"].toString());
				}
			}
		}

		const char *volRulesJson = config_get_string(global_config, "SRTLA_AutoSwitch", "VolumeRulesJSON");
		if (volRulesJson && *volRulesJson) {
			QJsonDocument doc = QJsonDocument::fromJson(QByteArray(volRulesJson));
			if (doc.isArray()) {
				QJsonArray arr = doc.array();
				for (int i = 0; i < arr.size(); i++) {
					QJsonObject obj = arr[i].toObject();
					addVolumeRuleRow(obj["audioSource"].toString(), obj["minDb"].toInt(),
							 obj["maxDb"].toInt(), obj["targetSource"].toString());
				}
			}
		}
	}
}


void SrtlaAutoSwitchDialog::addVisibilityRuleRow(int minKbps, int maxKbps, const QString &trackerSource, const QString &sourceName)
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

	QComboBox *trackerSourceCb = new QComboBox();
	trackerSourceCb->setEditable(true);
	trackerSourceCb->addItems(availableSources);
	int tIdx = trackerSourceCb->findText(trackerSource);
	if (tIdx >= 0)
		trackerSourceCb->setCurrentIndex(tIdx);
	else
		trackerSourceCb->setCurrentText(trackerSource);
	visibilityRulesTable->setCellWidget(row, 2, trackerSourceCb);

	QComboBox *sourceCb = new QComboBox();
	sourceCb->setEditable(true);
	sourceCb->addItems(availableSources);
	int index = sourceCb->findText(sourceName);
	if (index >= 0)
		sourceCb->setCurrentIndex(index);
	else
		sourceCb->setCurrentText(sourceName);
	visibilityRulesTable->setCellWidget(row, 3, sourceCb);

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
	addVisibilityRuleRow(0, 0, "", "");
}

void SrtlaAutoSwitchDialog::addVolumeRuleRow(const QString &audioSource, int minDb, int maxDb,
					    const QString &targetSource)
{
	int row = volumeRulesTable->rowCount();
	volumeRulesTable->insertRow(row);

	QComboBox *audioCb = new QComboBox();
	audioCb->setEditable(true);
	audioCb->addItems(availableAudioSources);
	int aIdx = audioCb->findText(audioSource);
	if (aIdx >= 0)
		audioCb->setCurrentIndex(aIdx);
	else
		audioCb->setCurrentText(audioSource);
	volumeRulesTable->setCellWidget(row, 0, audioCb);

	QSpinBox *minSp = new QSpinBox();
	minSp->setRange(-100, 0);
	minSp->setSuffix(" dB");
	minSp->setValue(minDb);
	volumeRulesTable->setCellWidget(row, 1, minSp);

	QSpinBox *maxSp = new QSpinBox();
	maxSp->setRange(-100, 0);
	maxSp->setSuffix(" dB");
	maxSp->setValue(maxDb);
	volumeRulesTable->setCellWidget(row, 2, maxSp);

	QComboBox *targetCb = new QComboBox();
	targetCb->setEditable(true);
	targetCb->addItems(availableSources);
	int tIdx = targetCb->findText(targetSource);
	if (tIdx >= 0)
		targetCb->setCurrentIndex(tIdx);
	else
		targetCb->setCurrentText(targetSource);
	volumeRulesTable->setCellWidget(row, 3, targetCb);

	QPushButton *removeBtn = new QPushButton("Remove");
	connect(removeBtn, &QPushButton::clicked, [this, removeBtn]() {
		for (int i = 0; i < volumeRulesTable->rowCount(); i++) {
			if (volumeRulesTable->cellWidget(i, 4) == removeBtn) {
				volumeRulesTable->removeRow(i);
				break;
			}
		}
	});
	volumeRulesTable->setCellWidget(row, 4, removeBtn);
}

void SrtlaAutoSwitchDialog::addNewVolumeRule()
{
	addVolumeRuleRow(availableAudioSources.isEmpty() ? "" : availableAudioSources[0], -100, -50, "");
}

void SrtlaAutoSwitchDialog::saveSettings()
{
	config_t *global_config = obs_frontend_get_profile_config();
	if (global_config) {
		config_set_bool(global_config, "SRTLA_AutoSwitch", "Enabled", enableAutoSwitch->currentIndex() == 1);
		config_set_int(global_config, "SRTLA_AutoSwitch", "Delay", switchDelay->value());
		config_set_int(global_config, "SRTLA_AutoSwitch", "ReturnDelay", returnDelay->value());
		config_set_string(global_config, "SRTLA_AutoSwitch", "FailoverScene", failoverSceneBox->currentText().toUtf8().constData());

		config_set_bool(global_config, "SRTLA_AutoSwitch", "VisEnabled", enableVisSwitch->currentIndex() == 1);
		config_set_int(global_config, "SRTLA_AutoSwitch", "VisDelay", visSwitchDelay->value());

		config_set_bool(global_config, "SRTLA_AutoSwitch", "VolEnabled", enableVolSwitch->currentIndex() == 1);
		config_set_int(global_config, "SRTLA_AutoSwitch", "VolDelay", volSwitchDelay->value());

		QJsonArray noFailoverArr;
		for (int i = 0; i < noFailoverList->count(); i++) {
			QListWidgetItem *item = noFailoverList->item(i);
			if (item && item->checkState() == Qt::Checked) {
				noFailoverArr.append(item->text());
			}
		}
		QJsonDocument noFailoverDoc(noFailoverArr);
		QString noFailoverJsonString = noFailoverDoc.toJson(QJsonDocument::Compact);
		config_set_string(global_config, "SRTLA_AutoSwitch", "NoFailoverScenes",
				  noFailoverJsonString.toUtf8().constData());

		QJsonArray visArr;
		for (int i = 0; i < visibilityRulesTable->rowCount(); i++) {
			QSpinBox *minSp = qobject_cast<QSpinBox *>(visibilityRulesTable->cellWidget(i, 0));
			QSpinBox *maxSp = qobject_cast<QSpinBox *>(visibilityRulesTable->cellWidget(i, 1));
			QComboBox *trackerSourceCb = qobject_cast<QComboBox *>(visibilityRulesTable->cellWidget(i, 2));
			QComboBox *sourceCb = qobject_cast<QComboBox *>(visibilityRulesTable->cellWidget(i, 3));

			if (minSp && maxSp && trackerSourceCb && sourceCb) {
				QJsonObject obj;
				obj["minKbps"] = minSp->value();
				obj["maxKbps"] = maxSp->value();
				obj["trackerSource"] = trackerSourceCb->currentText();
				obj["sourceName"] = sourceCb->currentText();
				visArr.append(obj);
			}
		}
		QJsonDocument visDoc(visArr);
		QString visJsonString = visDoc.toJson(QJsonDocument::Compact);

		config_set_string(global_config, "SRTLA_AutoSwitch", "VisibilityRulesJSON",
				  visJsonString.toUtf8().constData());

		QJsonArray volArr;
		for (int i = 0; i < volumeRulesTable->rowCount(); i++) {
			QComboBox *audioCb = qobject_cast<QComboBox *>(volumeRulesTable->cellWidget(i, 0));
			QSpinBox *minSp = qobject_cast<QSpinBox *>(volumeRulesTable->cellWidget(i, 1));
			QSpinBox *maxSp = qobject_cast<QSpinBox *>(volumeRulesTable->cellWidget(i, 2));
			QComboBox *targetCb = qobject_cast<QComboBox *>(volumeRulesTable->cellWidget(i, 3));

			if (audioCb && minSp && maxSp && targetCb) {
				QJsonObject obj;
				obj["audioSource"] = audioCb->currentText();
				obj["minDb"] = minSp->value();
				obj["maxDb"] = maxSp->value();
				obj["targetSource"] = targetCb->currentText();
				volArr.append(obj);
			}
		}
		QJsonDocument volDoc(volArr);
		QString volJsonString = volDoc.toJson(QJsonDocument::Compact);

		config_set_string(global_config, "SRTLA_AutoSwitch", "VolumeRulesJSON",
				  volJsonString.toUtf8().constData());

		config_save_safe(global_config, "tmp", nullptr);
	}

	// Restart or re-read settings in the background task
	SrtlaAutoSwitcher::instance().start();
	accept();
}

SrtlaAutoSwitcher::SrtlaAutoSwitcher(QObject *parent)
	: QObject(parent),
	  timer(new QTimer(this)),
	  isCurrentlyFailover(false),
	  matchDurationCounter(0),
	  visMatchDurationCounter(0),
	  volMatchDurationCounter(0),
	  activePrimaryPort(0),
	  frontendCallbackRegistered(true)
{
	connect(timer, &QTimer::timeout, this, &SrtlaAutoSwitcher::checkBitrate);
	obs_frontend_add_event_callback(handleFrontendEvent, this);
}

SrtlaAutoSwitcher::~SrtlaAutoSwitcher()
{
	stop();
}

void SrtlaAutoSwitcher::volmeterCallback(void *param, const float magnitude[MAX_AUDIO_CHANNELS],
					const float peak[MAX_AUDIO_CHANNELS],
					const float input_peak[MAX_AUDIO_CHANNELS])
{
	UNUSED_PARAMETER(magnitude);
	UNUSED_PARAMETER(input_peak);
	MonitoredAudioSource *mas = static_cast<MonitoredAudioSource *>(param);
	if (!mas)
		return;

	// Muted check is performed in checkBitrate() using weak_source

	float frame_max = -100.0f;
	for (int c = 0; c < MAX_AUDIO_CHANNELS; c++) {
		float p = peak[c];
		if (!std::isnan(p) && !std::isinf(p)) {
			if (p > frame_max) {
				frame_max = p;
			}
		}
	}

	if (frame_max > 0.0f)
		frame_max = 0.0f;
	if (frame_max < -100.0f)
		frame_max = -100.0f;

	// Keep track of the highest peak level in the current measurement interval
	if (frame_max > mas->lastDb) {
		mas->lastDb = frame_max;
		mas->lastUpdateTime = os_gettime_ns();
	}
	mas->lastUpdateTime = os_gettime_ns();
}

void SrtlaAutoSwitcher::clearMonitoredAudioSources()
{
	for (auto mas : monitoredAudioSources) {
		if (mas) {
			if (mas->volmeter) {
				obs_volmeter_remove_callback(mas->volmeter, volmeterCallback, mas);
				obs_volmeter_detach_source(mas->volmeter);
				obs_volmeter_destroy(mas->volmeter);
				mas->volmeter = nullptr;
			}
			if (mas->weak_source) {
				obs_weak_source_release(mas->weak_source);
				mas->weak_source = nullptr;
			}
			delete mas;
		}
	}
	monitoredAudioSources.clear();
}

void SrtlaAutoSwitcher::updateMonitoredAudioSources()
{
	QSet<QString> neededSources;
	for (const auto &r : volumeRules) {
		if (!r.audioSource.trimmed().isEmpty()) {
			neededSources.insert(r.audioSource.trimmed());
		}
	}

	// Always monitor all active audio sources for the Web UI levels
	obs_enum_sources(
		[](void *data, obs_source_t *source) {
			QSet<QString> *needed = static_cast<QSet<QString> *>(data);
			uint32_t flags = obs_source_get_output_flags(source);
			if (flags & OBS_SOURCE_AUDIO) {
				const char *name = obs_source_get_name(source);
				if (name) {
					needed->insert(QString::fromUtf8(name));
				}
			}
			return true;
		},
		&neededSources);

	// Remove any sources no longer in rules or whose weak_source has expired
	QList<QString> currentKeys = monitoredAudioSources.keys();
	for (const QString &key : currentKeys) {
		MonitoredAudioSource *existing = monitoredAudioSources.value(key, nullptr);
		bool expired = true;
		if (existing && existing->weak_source) {
			obs_source_t *strong = obs_weak_source_get_source(existing->weak_source);
			if (strong) {
				expired = false;
				obs_source_release(strong);
			}
		}
		if (!neededSources.contains(key) || expired) {
			MonitoredAudioSource *mas = monitoredAudioSources.take(key);
			if (mas) {
				if (mas->volmeter) {
					obs_volmeter_remove_callback(mas->volmeter, volmeterCallback, mas);
					obs_volmeter_detach_source(mas->volmeter);
					obs_volmeter_destroy(mas->volmeter);
				}
				if (mas->weak_source) {
					obs_weak_source_release(mas->weak_source);
				}
				delete mas;
			}
		}
	}

	// Add any newly needed sources using a weak reference so we never block scene collection cleanup
	for (const QString &srcName : neededSources) {
		if (!monitoredAudioSources.contains(srcName)) {
			obs_source_t *src = obs_get_source_by_name(srcName.toUtf8().constData());
			if (src) {
				MonitoredAudioSource *mas = new MonitoredAudioSource();
				mas->weak_source = obs_source_get_weak_source(src);
				mas->lastDb = -100.0f;
				mas->lastUpdateTime = os_gettime_ns();
				mas->volmeter = obs_volmeter_create(OBS_FADER_LOG);
				if (mas->volmeter) {
					obs_volmeter_add_callback(mas->volmeter, volmeterCallback, mas);
					obs_volmeter_attach_source(mas->volmeter, src);
				}
				// Release the strong reference immediately!
				obs_source_release(src);
				monitoredAudioSources.insert(srcName, mas);
			}
		}
	}
}

void SrtlaAutoSwitcher::handleFrontendEvent(enum obs_frontend_event event, void *private_data)
{
	SrtlaAutoSwitcher *switcher = static_cast<SrtlaAutoSwitcher *>(private_data);
	if (event == OBS_FRONTEND_EVENT_SCENE_CHANGED) {
		// If the user manually changes the scene while a rule is applied,
		obs_source_t *currentScene = obs_frontend_get_current_scene();
		const char *currentName = currentScene ? obs_source_get_name(currentScene) : nullptr;

		if (currentName && QString::fromUtf8(currentName) != switcher->failoverScene) {
			// User manually navigated away from the auto-switched scene.
			// Reset state so we are back in "manual" mode.
			switcher->originalSceneName = "";
			switcher->isCurrentlyFailover = false;
		}
		if (currentScene)
			obs_source_release(currentScene);
	} else if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP ||
		   event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING ||
		   event == OBS_FRONTEND_EVENT_PROFILE_CHANGING) {
		switcher->clearMonitoredAudioSources();
	} else if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED ||
		   event == OBS_FRONTEND_EVENT_PROFILE_CHANGED) {
		switcher->loadRules();
	}
}

static int getListenPortForSource(const QString &sourceName)
{
	int port = 5000;
	if (sourceName.isEmpty()) return port;

	obs_source_t *src = obs_get_source_by_name(sourceName.toUtf8().constData());
	if (src) {
		obs_data_t *settings = obs_source_get_settings(src);
		if (settings) {
			int p = (int)obs_data_get_int(settings, "listen_port");
			if (p > 0) port = p;
			obs_data_release(settings);
		}
		obs_source_release(src);
	}
	return port;
}

void SrtlaAutoSwitcher::loadRules()
{
	failoverScene = "";
	visibilityRules.clear();
	volumeRules.clear();
	noFailoverScenes.clear();

	config_t *global_config = obs_frontend_get_profile_config();
	if (global_config) {

		const char *failoverStr = config_get_string(global_config, "SRTLA_AutoSwitch", "FailoverScene");
		if (failoverStr) failoverScene = QString::fromUtf8(failoverStr);

		if (failoverScene.isEmpty()) {
			const char *legacyRulesJson = config_get_string(global_config, "SRTLA_AutoSwitch", "RulesJSON");
			if (legacyRulesJson && *legacyRulesJson) {
				QJsonDocument rDoc = QJsonDocument::fromJson(QByteArray(legacyRulesJson));
				if (rDoc.isArray() && !rDoc.array().isEmpty()) {
					QJsonObject firstRule = rDoc.array()[0].toObject();
					QString sceneFromRule = firstRule["sceneName"].toString();
					if (sceneFromRule.isEmpty()) sceneFromRule = firstRule["failoverScene"].toString();
					if (!sceneFromRule.isEmpty()) {
						failoverScene = sceneFromRule;
					}
				}
			}
		}

		const char *noFailoverJson = config_get_string(global_config, "SRTLA_AutoSwitch", "NoFailoverScenes");
		if (noFailoverJson && *noFailoverJson) {
			QJsonDocument doc = QJsonDocument::fromJson(QByteArray(noFailoverJson));
			if (doc.isArray()) {
				QJsonArray arr = doc.array();
				for (int i = 0; i < arr.size(); i++) {
					noFailoverScenes.insert(arr[i].toString());
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
					r.trackerSource = obj["trackerSource"].toString();
					r.sourceName = obj["sourceName"].toString();
					visibilityRules.append(r);
				}
			}
		}

		const char *volRulesJson = config_get_string(global_config, "SRTLA_AutoSwitch", "VolumeRulesJSON");
		if (volRulesJson && *volRulesJson) {
			QJsonDocument doc = QJsonDocument::fromJson(QByteArray(volRulesJson));
			if (doc.isArray()) {
				QJsonArray arr = doc.array();
				for (int i = 0; i < arr.size(); i++) {
					QJsonObject obj = arr[i].toObject();
					VolumeVisibilityRule r;
					r.audioSource = obj["audioSource"].toString();
					r.minDb = obj["minDb"].toInt();
					r.maxDb = obj["maxDb"].toInt();
					r.targetSource = obj["targetSource"].toString();
					volumeRules.append(r);
				}
			}
		}
	}

	updateMonitoredAudioSources();
}

void SrtlaAutoSwitcher::start()
{
	loadRules();
	isCurrentlyFailover = false;
	matchDurationCounter = 0;
	recoveryDurationCounter = 0;

	currentMatchedVisRules.clear();
	currentlyAppliedVisRules.clear();
	visMatchDurationCounter = 0;

	currentMatchedVolRules.clear();
	currentlyAppliedVolRules.clear();
	volMatchDurationCounter = 0;

	if (!timer->isActive()) {
		timer->start(1000); // Check every second
	}
}

void SrtlaAutoSwitcher::stop()
{
	timer->stop();
	if (frontendCallbackRegistered) {
		obs_frontend_remove_event_callback(handleFrontendEvent, this);
		frontendCallbackRegistered = false;
	}
	clearMonitoredAudioSources();
}

static void set_target_source_visibility_all_scenes(const QString &targetName, bool visible)
{
	struct obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);

	std::pair<QString, bool> paramPair(targetName, visible);

	for (size_t i = 0; i < scenes.sources.num; i++) {
		obs_source_t *scene_source = scenes.sources.array[i];
		obs_scene_t *scene = obs_scene_from_source(scene_source);
		if (scene) {
			obs_scene_enum_items(
				scene,
				[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
					auto data = static_cast<std::pair<QString, bool> *>(param);
					if (obs_sceneitem_is_group(item)) {
						obs_scene_t *groupScene = obs_group_from_source(obs_sceneitem_get_source(item));
						if (groupScene) {
							obs_scene_enum_items(
								groupScene,
								[](obs_scene_t *, obs_sceneitem_t *childItem, void *childParam) {
									auto childData = static_cast<std::pair<QString, bool> *>(childParam);
									obs_source_t *childSrc = obs_sceneitem_get_source(childItem);
									const char *name = obs_source_get_name(childSrc);
									if (name && QString::fromUtf8(name) == childData->first) {
										obs_sceneitem_set_visible(childItem, childData->second);
									}
									return true;
								},
								data);
						}
					}

					obs_source_t *src = obs_sceneitem_get_source(item);
					const char *name = obs_source_get_name(src);
					if (name && QString::fromUtf8(name) == data->first) {
						obs_sceneitem_set_visible(item, data->second);
					}
					return true;
				},
				&paramPair);
		}
	}
	obs_frontend_source_list_free(&scenes);
}

QJsonObject SrtlaAutoSwitcher::getAudioLevels()
{
	QJsonObject obj;
	for (auto it = monitoredAudioSources.constBegin(); it != monitoredAudioSources.constEnd(); ++it) {
		if (it.value()) {
			obj[it.key()] = (double)it.value()->lastDb;
		}
	}
	return obj;
}

void SrtlaAutoSwitcher::checkBitrate()
{
	// 1. Always update and decay audio meters for the Web UI dashboard (even if autoswitch is disabled)
	static int audioUpdateCounter = 0;
	if (audioUpdateCounter++ % 2 == 0) {
		updateMonitoredAudioSources();
	}

	uint64_t now = os_gettime_ns();
	QMap<QString, float> sourceDbMap;
	for (auto it = monitoredAudioSources.begin(); it != monitoredAudioSources.end(); ++it) {
		QString srcKey = it.key();
		MonitoredAudioSource *mas = it.value();
		if (mas) {
			bool isMuted = false;
			if (mas->weak_source) {
				obs_source_t *strong = obs_weak_source_get_source(mas->weak_source);
				if (strong) {
					isMuted = obs_source_muted(strong);
					obs_source_release(strong);
				}
			}
			if (isMuted) {
				mas->lastDb = -100.0f;
			} else if (now > mas->lastUpdateTime && (now - mas->lastUpdateTime > 1500000000ULL)) {
				mas->lastDb = -100.0f;
			} else {
				// Decay by 15 dB per second for smooth VU meter and autoswitch threshold
				mas->lastDb -= 15.0f;
			}
			
			if (mas->lastDb < -100.0f) {
				mas->lastDb = -100.0f;
			}
			
			sourceDbMap[srcKey] = mas->lastDb;
		} else {
			sourceDbMap[srcKey] = -100.0f;
		}
	}

	config_t *global_config = obs_frontend_get_profile_config();
	if (!global_config)
		return;

	bool enabled = config_get_bool(global_config, "SRTLA_AutoSwitch", "Enabled");
	bool visEnabled = config_get_bool(global_config, "SRTLA_AutoSwitch", "VisEnabled");
	bool volEnabled = config_get_bool(global_config, "SRTLA_AutoSwitch", "VolEnabled");

	if ((!enabled || failoverScene.isEmpty()) && (!visEnabled || visibilityRules.isEmpty()) &&
	    (!volEnabled || volumeRules.isEmpty())) {
		isCurrentlyFailover = false;
		matchDurationCounter = 0;
		recoveryDurationCounter = 0;
		currentMatchedVisRules.clear();
		visMatchDurationCounter = 0;
		currentMatchedVolRules.clear();
		volMatchDurationCounter = 0;
		return;
	}

	int delay = config_get_int(global_config, "SRTLA_AutoSwitch", "Delay");
	int returnDelay = config_get_int(global_config, "SRTLA_AutoSwitch", "ReturnDelay");
	if (!config_has_user_value(global_config, "SRTLA_AutoSwitch", "ReturnDelay") || returnDelay <= 0) {
		returnDelay = 5;
	}
	if (!config_has_user_value(global_config, "SRTLA_AutoSwitch", "Delay") || delay < 0) {
		delay = 4;
	}
	int visDelay = config_get_int(global_config, "SRTLA_AutoSwitch", "VisDelay");
	int volDelay = config_get_int(global_config, "SRTLA_AutoSwitch", "VolDelay");
	if (!config_has_user_value(global_config, "SRTLA_AutoSwitch", "VolDelay")) {
		volDelay = 2;
	}

	// 2. Dynamic Volume Sources Automation
	if (volEnabled && !volumeRules.isEmpty()) {
		QSet<int> matchedVolRules;

		for (int i = 0; i < volumeRules.size(); i++) {
			const VolumeVisibilityRule &r = volumeRules[i];
			QString srcKey = r.audioSource.trimmed();
			float currentDb = sourceDbMap.value(srcKey, -100.0f);

			if (currentDb >= r.minDb && currentDb <= r.maxDb) {
				matchedVolRules.insert(i);
			}
		}

		if (matchedVolRules != currentMatchedVolRules) {
			currentMatchedVolRules = matchedVolRules;
			volMatchDurationCounter = 0;
		}

		volMatchDurationCounter++;
		if (volMatchDurationCounter > volDelay) {
			volMatchDurationCounter = volDelay;
		}

		if (volMatchDurationCounter >= volDelay) {
			QSet<QString> allRuleTargets;
			for (const auto &r : volumeRules) {
				if (!r.targetSource.trimmed().isEmpty())
					allRuleTargets.insert(r.targetSource.trimmed());
			}

			QSet<QString> targetsToShow;
			for (int index : currentMatchedVolRules) {
				if (!volumeRules[index].targetSource.trimmed().isEmpty())
					targetsToShow.insert(volumeRules[index].targetSource.trimmed());
			}

			for (const auto &targetName : allRuleTargets) {
				bool shouldShow = targetsToShow.contains(targetName);
				set_target_source_visibility_all_scenes(targetName, shouldShow);
			}

			currentlyAppliedVolRules = currentMatchedVolRules;
		}
	} else {
		currentMatchedVolRules.clear();
		volMatchDurationCounter = 0;
	}

	// 2. SRTLA Bitrate-based Automation (Scene Auto-Switcher & KBPS Source Visibility)
	if ((!enabled || failoverScene.isEmpty()) && (!visEnabled || visibilityRules.isEmpty())) {
		isCurrentlyFailover = false;
		matchDurationCounter = 0;
		currentMatchedVisRules.clear();
		visMatchDurationCounter = 0;
		return;
	}

	// Fetch stats
	bool is_listening = false;
	int groups = 0;
	int connections = 0;
	int listen_port = 0;
	int failed_conns = 0;
	char details_buffer[16384] = {0};
	char rist_details_buffer[16384] = {0};

	srtla_get_connection_stats(&is_listening, &groups, &connections);
	srtla_get_connection_details(&listen_port, &failed_conns, details_buffer, sizeof(details_buffer));
	
	bool rist_listening = false;
	int rist_groups = 0;
	int rist_conns = 0;
	rist_get_connection_stats(&rist_listening, &rist_groups, &rist_conns);
	rist_get_connection_details(rist_details_buffer, sizeof(rist_details_buffer));
	
	is_listening = is_listening || rist_listening;

	if (!is_listening && activePrimaryPort <= 0) {
		isCurrentlyFailover = false;
		matchDurationCounter = 0;
		currentMatchedVisRules.clear();
		visMatchDurationCounter = 0;
		return;
	}

	QMap<int, double> portTotalKbps;
	int activeGroupsWithData = 0;

	QJsonDocument doc = QJsonDocument::fromJson(QByteArray(details_buffer));
	QJsonDocument ristDoc = QJsonDocument::fromJson(QByteArray(rist_details_buffer));
	
	QJsonArray allGroupsArray;
	if (doc.isObject()) {
		QJsonArray srtlaGroups = doc.object()["groups"].toArray();
		for (int i = 0; i < srtlaGroups.size(); i++) allGroupsArray.append(srtlaGroups[i]);
	}
	if (ristDoc.isObject()) {
		QJsonArray ristGroups = ristDoc.object()["groups"].toArray();
		for (int i = 0; i < ristGroups.size(); i++) allGroupsArray.append(ristGroups[i]);
	}

	for (int i = 0; i < allGroupsArray.size(); i++) {
		QJsonObject gObj = allGroupsArray[i].toObject();
		int gPort = gObj["listen_port"].toInt();
		QString groupIdStr = QString::number(gObj["id"].toVariant().toULongLong());

		QJsonArray connsArray = gObj["conns"].toArray();
		double calculatedSumKbps = 0.0;
		for (int j = 0; j < connsArray.size(); j++) {
			QJsonObject cObj = connsArray[j].toObject();
			QString ip = cObj["ip"].toString();
			QString port = QString::number(cObj["port"].toInt());
			QString connIdStr = groupIdStr + "_" + ip + ":" + port;

			uint64_t cBytes = cObj["bytes"].toVariant().toULongLong();
			uint64_t cPrevBytes = previousBytes.value(connIdStr, cBytes);
			if (cBytes < cPrevBytes) {
				cPrevBytes = cBytes;
			}
			previousBytes[connIdStr] = cBytes;

			// We check every 1 second here
			double cKbps = ((cBytes - cPrevBytes) * 8.0) / 1000.0 / 1.0;
			calculatedSumKbps += cKbps;
		}

		portTotalKbps[gPort] += calculatedSumKbps;
		activeGroupsWithData++;
	}

	// Clean up previousBytes for disconnected groups
	if (activeGroupsWithData == 0) {
		previousBytes.clear();
	}

	// Dynamically track primary source based on media playing state or active network stream
	bool currentPrimaryActive = false;
	if (activePrimaryPort > 0 &&
	    (srtla_is_media_playing(activePrimaryPort) || rist_is_media_playing(activePrimaryPort) ||
	     portTotalKbps.value(activePrimaryPort, 0.0) > 10.0)) {
		currentPrimaryActive = true;
	}

	if (!currentPrimaryActive) {
		int newPrimaryPort = 0;
		for (auto it = portTotalKbps.begin(); it != portTotalKbps.end(); ++it) {
			if (srtla_is_media_playing(it.key()) || rist_is_media_playing(it.key()) || it.value() > 10.0) {
				newPrimaryPort = it.key();
				break;
			}
		}

		// Also check all configured receivers in case a receiver is playing before its first group stats tick
		if (newPrimaryPort == 0) {
			char rec_buf[4096] = {0};
			srtla_get_all_receivers_json(rec_buf, sizeof(rec_buf));
			QJsonDocument rDoc = QJsonDocument::fromJson(QByteArray(rec_buf));
			if (rDoc.isArray()) {
				for (const auto &val : rDoc.array()) {
					int p = val.toObject()["listen_port"].toInt();
					if (p > 0 && srtla_is_media_playing(p)) {
						newPrimaryPort = p;
						break;
					}
				}
			}
		}
		if (newPrimaryPort == 0) {
			char rec_buf[4096] = {0};
			rist_get_all_receivers_json(rec_buf, sizeof(rec_buf));
			QJsonDocument rDoc = QJsonDocument::fromJson(QByteArray(rec_buf));
			if (rDoc.isArray()) {
				for (const auto &val : rDoc.array()) {
					int p = val.toObject()["listen_port"].toInt();
					if (p > 0 && rist_is_media_playing(p)) {
						newPrimaryPort = p;
						break;
					}
				}
			}
		}
		
		if (newPrimaryPort > 0 && newPrimaryPort != activePrimaryPort) {
			activePrimaryPort = newPrimaryPort;
			
			struct PortContext {
				int searchPort;
				QString name;
			} ctx;
			ctx.searchPort = newPrimaryPort;
			
			obs_enum_sources([](void *data, obs_source_t *source) {
				PortContext *c = static_cast<PortContext *>(data);
				obs_data_t *settings = obs_source_get_settings(source);
				if (settings) {
					int p = (int)obs_data_get_int(settings, "listen_port");
					if (p == c->searchPort) {
						const char *n = obs_source_get_name(source);
						if (n) c->name = QString::fromUtf8(n);
					}
					obs_data_release(settings);
				}
				return c->name.isEmpty();
			}, &ctx);
			activePrimarySource = ctx.name;
			obs_log(LOG_INFO, "[PyleIRL AutoSwitch] Locked onto primary receiver '%s' on port %d",
				activePrimarySource.toUtf8().constData(), activePrimaryPort);
		}
	}

	if (enabled && activePrimaryPort > 0 && !failoverScene.isEmpty()) {
		obs_source_t *currentScene = obs_frontend_get_current_scene();
		QString currentSceneName;
		if (currentScene) {
			const char *currentName = obs_source_get_name(currentScene);
			if (currentName) {
				currentSceneName = QString::fromUtf8(currentName);
			}
			obs_source_release(currentScene);
		}

		if (noFailoverScenes.contains(currentSceneName)) {
			// Current scene is marked as No Failover: auto scene switching is disabled
			isCurrentlyFailover = false;
			matchDurationCounter = 0;
			recoveryDurationCounter = 0;
			originalSceneName = "";
		} else {
			bool mediaIsPlaying = srtla_is_media_playing(activePrimaryPort) || rist_is_media_playing(activePrimaryPort);
			double primaryKbps = portTotalKbps.value(activePrimaryPort, 0.0);
			if (primaryKbps < 1.0) {
				networkDeadDurationMs[activePrimaryPort] += 1000;
			} else {
				networkDeadDurationMs[activePrimaryPort] = 0;
			}
			bool networkStopped = (networkDeadDurationMs[activePrimaryPort] >= 2000) || !is_listening;
			bool streamHealthy = mediaIsPlaying && !networkStopped;

			if (!streamHealthy) {
				// Connection stopped or media buffer is starved, frozen, or heavily corrupt
				recoveryDurationCounter = 0; // Reset recovery timer immediately!
				if (!isCurrentlyFailover) {
					matchDurationCounter++;
					if (matchDurationCounter >= delay) {
						// Delay met (e.g. 4 seconds), switch to failover
						if (currentSceneName != failoverScene) {
							// Save this scene so we can restore it later
							if (originalSceneName.isEmpty()) {
								originalSceneName = currentSceneName;
							}
							obs_source_t *targetSceneSrc = obs_get_source_by_name(failoverScene.toUtf8().constData());
							if (targetSceneSrc) {
								obs_log(LOG_INFO, "[PyleIRL AutoSwitch] Connection stopped on port %d for %ds — switching from '%s' to failover scene '%s'",
									activePrimaryPort, matchDurationCounter,
									currentSceneName.toUtf8().constData(),
									failoverScene.toUtf8().constData());
								obs_frontend_set_current_scene(targetSceneSrc);
								obs_source_release(targetSceneSrc);
							}
						}
						isCurrentlyFailover = true;
					}
				}
			} else {
				// Media buffer is delivering healthy frames and network is active
				matchDurationCounter = 0; // Reset failover counter
				if (isCurrentlyFailover) {
					// Require continuous good stream for returnDelay (default 5s) before returning to Live!
					recoveryDurationCounter++;
					if (recoveryDurationCounter >= returnDelay) {
						if (!originalSceneName.isEmpty()) {
							QString targetToRestore = originalSceneName;
							originalSceneName = "";
							obs_source_t *targetSceneSrc = obs_get_source_by_name(targetToRestore.toUtf8().constData());
							if (targetSceneSrc) {
								obs_log(LOG_INFO, "[PyleIRL AutoSwitch] Stream recovered on port %d for %ds — restoring scene '%s'",
									activePrimaryPort, recoveryDurationCounter,
									targetToRestore.toUtf8().constData());
								obs_frontend_set_current_scene(targetSceneSrc);
								obs_source_release(targetSceneSrc);
							}
						}
						isCurrentlyFailover = false;
						recoveryDurationCounter = 0;
					}
				} else {
					recoveryDurationCounter = 0;
				}
			}
		}
	}

	if (visEnabled && !visibilityRules.isEmpty()) {
		// Evaluate visibility rules
		QSet<int> matchedVisRules;
		for (int i = 0; i < visibilityRules.size(); i++) {
			const SourceVisibilityRule &r = visibilityRules[i];
			int rPort = getListenPortForSource(r.trackerSource);
			double kbps = portTotalKbps.value(rPort, 0.0);

			if (kbps >= r.minKbps && (r.maxKbps == 0 || kbps <= r.maxKbps)) {
				matchedVisRules.insert(i);
			}
		}

		if (matchedVisRules != currentMatchedVisRules) {
			currentMatchedVisRules = matchedVisRules;
			visMatchDurationCounter = 0;
		}

		visMatchDurationCounter++;
		if (visMatchDurationCounter > visDelay) {
			visMatchDurationCounter = visDelay;
		}

		if (visMatchDurationCounter >= visDelay) {
			// Find all unique source names in rules to hide them by default
			QSet<QString> allRuleSources;
			for (const auto &r : visibilityRules) {
				if (!r.sourceName.trimmed().isEmpty())
					allRuleSources.insert(r.sourceName.trimmed());
			}

			QSet<QString> sourcesToShow;
			for (int index : currentMatchedVisRules) {
				if (!visibilityRules[index].sourceName.trimmed().isEmpty())
					sourcesToShow.insert(visibilityRules[index].sourceName.trimmed());
			}

			for (const auto &sourceName : allRuleSources) {
				bool shouldShow = sourcesToShow.contains(sourceName);
				set_target_source_visibility_all_scenes(sourceName, shouldShow);
			}

			currentlyAppliedVisRules = currentMatchedVisRules;
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

	QMenu *srtlaMenu = new QMenu("PyleIRL", mainWindow);
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
	QObject::connect(startAction, &QAction::triggered, []() { 
		srtla_force_start_all(); 
		rist_force_start_all();
	});

	QAction *restartAction = srtlaMenu->addAction("Restart All Listeners");
	QObject::connect(restartAction, &QAction::triggered, []() { 
		srtla_force_restart_all(); 
		rist_force_restart_all();
	});

	QAction *stopAction = srtlaMenu->addAction("Stop All Listeners");
	QObject::connect(stopAction, &QAction::triggered, []() { 
		srtla_force_stop_all(); 
		rist_force_stop_all();
	});

	srtlaMenu->addSeparator();

	QAction *proxyAction = srtlaMenu->addAction("Reverse Proxy Settings...");
	QObject::connect(proxyAction, &QAction::triggered, [mainWindow]() {
		SrtlaReverseProxyDialog dialog(mainWindow);
		dialog.exec();
	});

	QAction *autoSwitchAction = srtlaMenu->addAction("Automation Settings...");
	QObject::connect(autoSwitchAction, &QAction::triggered, [mainWindow]() {
		SrtlaAutoSwitchDialog dialog(mainWindow);
		dialog.exec();
	});

	QAction *webInterfaceAction = srtlaMenu->addAction("Web Interface Settings...");
	QObject::connect(webInterfaceAction, &QAction::triggered, [mainWindow]() {
		SrtlaWebInterfaceDialog dialog(mainWindow);
		dialog.exec();
	});

	// Start web server if enabled
	config_t *global_config = obs_frontend_get_profile_config();
	if (global_config) {
		bool webEnabled = config_get_bool(global_config, "SRTLA_WebInterface", "Enabled");
		int webPort = config_get_int(global_config, "SRTLA_WebInterface", "Port");
		if (webPort == 0)
			webPort = 5433; // default
		if (webEnabled) {
			srtla_web_server_start(webPort);
		}
	}
	MultistreamManager::instance().loadConfig();

	srtlaMenu->addSeparator();

	QAction *multiAction = srtlaMenu->addAction("Multistream Settings...");
	QObject::connect(multiAction, &QAction::triggered, [mainWindow]() {
		SrtlaMultistreamDialog dialog(mainWindow);
		dialog.exec();
	});

	QAction *aboutAction = srtlaMenu->addAction("About...");
	QObject::connect(aboutAction, &QAction::triggered, [mainWindow]() {
		SrtlaAboutDialog dialog(mainWindow);
		dialog.exec();
	});

	// Start proxy on initial load if enabled
	srtla_proxy_settings_changed();

	// Start auto-switcher on initial load
	SrtlaAutoSwitcher::instance().start();
}

void SrtlaAutoSwitcher::reloadRules()
{
	loadRules();
}

SrtlaWebInterfaceDialog::SrtlaWebInterfaceDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle("SRTLA Web Interface Settings");
	setMinimumWidth(400);

	QVBoxLayout *mainLayout = new QVBoxLayout(this);
	QFormLayout *formLayout = new QFormLayout();

	enableWeb = new QComboBox();
	enableWeb->addItem("Disabled");
	enableWeb->addItem("Enabled");

	webPort = new QSpinBox();
	webPort->setRange(1, 65535);
	webPort->setValue(5433); // Default port

	accessPassword = new QLineEdit();
	accessPassword->setPlaceholderText("Leave blank to disable");
	accessPassword->setEchoMode(QLineEdit::Password);

	formLayout->addRow("Enable Web Interface:", enableWeb);
	formLayout->addRow("Web Server Port:", webPort);
	formLayout->addRow("Web Access Password:", accessPassword);

	mainLayout->addLayout(formLayout);

	QDialogButtonBox *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	mainLayout->addWidget(buttonBox);
	addLogoToLayout(mainLayout);

	connect(buttonBox, &QDialogButtonBox::accepted, this, &SrtlaWebInterfaceDialog::saveSettings);
	connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

	// Load existing settings
	config_t *global_config = obs_frontend_get_profile_config();
	if (global_config) {
		enableWeb->setCurrentIndex(config_get_bool(global_config, "SRTLA_WebInterface", "Enabled") ? 1 : 0);
		int port = config_get_int(global_config, "SRTLA_WebInterface", "Port");
		if (port > 0)
			webPort->setValue(port);
		const char *wpwd = config_get_string(global_config, "SRTLA", "WebAccessPassword");
		if (wpwd)
			accessPassword->setText(QString(wpwd));


	}
}

void SrtlaWebInterfaceDialog::saveSettings()
{
	config_t *global_config = obs_frontend_get_profile_config();
	if (global_config) {
		bool previouslyEnabled = config_get_bool(global_config, "SRTLA_WebInterface", "Enabled");
		int previousPort = config_get_int(global_config, "SRTLA_WebInterface", "Port");

		bool currentlyEnabled = (enableWeb->currentIndex() == 1);
		int currentPort = webPort->value();
		QString currentPwd = accessPassword->text();
		config_set_bool(global_config, "SRTLA_WebInterface", "Enabled", currentlyEnabled);
		config_set_int(global_config, "SRTLA_WebInterface", "Port", currentPort);
		config_set_string(global_config, "SRTLA", "WebAccessPassword", currentPwd.toUtf8().constData());

		config_save_safe(global_config, "tmp", nullptr);

		// Handle server restart or stop
		if (!currentlyEnabled) {
			srtla_web_server_stop();
		} else if (currentlyEnabled && (!previouslyEnabled || currentPort != previousPort)) {
			srtla_web_server_stop();
			srtla_web_server_start(currentPort);
		}
	}

	accept();
}

extern "C" void *create_srtla_multistream_dock()
{
	return new SrtlaMultistreamDock();
}

class MultistreamTargetConfigDialog : public QDialog {
public:
	MultistreamTargetConfig config;
	QLineEdit *nameEdit;
	QComboBox *typeCombo;
	QLineEdit *urlEdit;
	QLineEdit *keyEdit;
	QCheckBox *verticalCheck;
	QComboBox *sceneCombo;

	MultistreamTargetConfigDialog(QWidget *parent, const MultistreamTargetConfig &initial)
		: QDialog(parent),
		  config(initial)
	{
		setWindowTitle(config.id.isEmpty() ? "Add Target" : "Edit Target");
		setMinimumWidth(400);

		QVBoxLayout *layout = new QVBoxLayout(this);
		QFormLayout *form = new QFormLayout();

		nameEdit = new QLineEdit(config.name);
		typeCombo = new QComboBox();
		typeCombo->addItem("RTMP");
		typeCombo->addItem("SRT");
		typeCombo->setCurrentText(config.type.isEmpty() ? "RTMP" : config.type);

		urlEdit = new QLineEdit(config.url);
		urlEdit->setPlaceholderText("rtmp://... or srt://...");
		keyEdit = new QLineEdit(config.key);
		keyEdit->setEchoMode(QLineEdit::PasswordEchoOnEdit);

		verticalCheck = new QCheckBox("Send Vertical Layout");
		verticalCheck->setChecked(config.isVertical);
		
		sceneCombo = new QComboBox();
		sceneCombo->addItem("", "");
		struct obs_frontend_source_list scenes = {};
		obs_frontend_get_scenes(&scenes);
		for (size_t i = 0; i < scenes.sources.num; i++) {
			obs_source_t *scene_source = scenes.sources.array[i];
			const char *name = obs_source_get_name(scene_source);
			if (name) {
				sceneCombo->addItem(QString::fromUtf8(name), QString::fromUtf8(name));
			}
		}
		obs_frontend_source_list_free(&scenes);
		
		if (!config.targetScene.isEmpty()) {
			int idx = sceneCombo->findData(config.targetScene);
			if (idx != -1) sceneCombo->setCurrentIndex(idx);
		}
		
		// Only show scene picker if vertical is checked
		sceneCombo->setEnabled(config.isVertical);
		connect(verticalCheck, &QCheckBox::toggled, sceneCombo, &QWidget::setEnabled);

		form->addRow("Name:", nameEdit);
		form->addRow("Type:", typeCombo);
		form->addRow("URL:", urlEdit);
		form->addRow("Stream Key:", keyEdit);
		form->addRow("", verticalCheck);
		form->addRow("Vertical Target Scene:", sceneCombo);

		layout->addLayout(form);

		QDialogButtonBox *btnBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
		layout->addWidget(btnBox);

		connect(btnBox, &QDialogButtonBox::accepted, this, [this]() {
			this->config.name = nameEdit->text();
			this->config.type = typeCombo->currentText();
			this->config.url = urlEdit->text();
			this->config.key = keyEdit->text();
			this->config.isVertical = verticalCheck->isChecked();
			this->config.targetScene = sceneCombo->currentData().toString();
			accept();
		});
		connect(btnBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
	}
};

SrtlaMultistreamDialog::SrtlaMultistreamDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle("Multistream Settings");
	setMinimumSize(600, 400);

	QVBoxLayout *mainLayout = new QVBoxLayout(this);

	syncWithObsCheck = new QCheckBox("Sync with OBS Live (Start/Stop targets when OBS starts/stops streaming)");
	syncWithObsCheck->setChecked(MultistreamManager::instance().getSyncWithObs());
	mainLayout->addWidget(syncWithObsCheck);



	targetsTable = new QTableWidget();
	targetsTable->setColumnCount(4);
	targetsTable->setHorizontalHeaderLabels(QStringList() << "Name" << "Type" << "URL" << "Enabled");
	targetsTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
	targetsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
	targetsTable->setSelectionMode(QAbstractItemView::SingleSelection);
	mainLayout->addWidget(targetsTable);

	QHBoxLayout *btnLayout = new QHBoxLayout();
	QPushButton *addBtn = new QPushButton("Add Target");
	QPushButton *editBtn = new QPushButton("Edit Target");
	QPushButton *delBtn = new QPushButton("Delete Target");

	btnLayout->addWidget(addBtn);
	btnLayout->addWidget(editBtn);
	btnLayout->addWidget(delBtn);
	btnLayout->addStretch();
	mainLayout->addLayout(btnLayout);

	QDialogButtonBox *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	mainLayout->addWidget(buttonBox);

	connect(addBtn, &QPushButton::clicked, this, &SrtlaMultistreamDialog::addTarget);
	connect(editBtn, &QPushButton::clicked, this, &SrtlaMultistreamDialog::editTarget);
	connect(delBtn, &QPushButton::clicked, this, &SrtlaMultistreamDialog::deleteTarget);
	connect(buttonBox, &QDialogButtonBox::accepted, this, &SrtlaMultistreamDialog::saveSettings);
	connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

	reloadList();
}

void SrtlaMultistreamDialog::reloadList()
{
	targetsTable->setRowCount(0);
	auto targets = MultistreamManager::instance().getTargets();
	for (int i = 0; i < targets.size(); i++) {
		auto cfg = targets[i]->getConfig();
		targetsTable->insertRow(i);

		QTableWidgetItem *nameItem = new QTableWidgetItem(cfg.name);
		nameItem->setData(Qt::UserRole, cfg.id);
		targetsTable->setItem(i, 0, nameItem);

		targetsTable->setItem(i, 1, new QTableWidgetItem(cfg.type));
		targetsTable->setItem(i, 2, new QTableWidgetItem(cfg.url));

		QTableWidgetItem *enabledItem = new QTableWidgetItem();
		enabledItem->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
		enabledItem->setCheckState(cfg.enabled ? Qt::Checked : Qt::Unchecked);
		targetsTable->setItem(i, 3, enabledItem);
	}
}

void SrtlaMultistreamDialog::addTarget()
{
	MultistreamTargetConfig cfg;
	MultistreamTargetConfigDialog dlg(this, cfg);
	if (dlg.exec() == QDialog::Accepted) {
		MultistreamManager::instance().addTarget(dlg.config);
		reloadList();
	}
}

void SrtlaMultistreamDialog::editTarget()
{
	int row = targetsTable->currentRow();
	if (row < 0)
		return;

	QString id = targetsTable->item(row, 0)->data(Qt::UserRole).toString();
	MultistreamTarget *t = MultistreamManager::instance().getTarget(id);
	if (!t)
		return;

	MultistreamTargetConfigDialog dlg(this, t->getConfig());
	if (dlg.exec() == QDialog::Accepted) {
		MultistreamManager::instance().updateTarget(id, dlg.config);
		reloadList();
	}
}

void SrtlaMultistreamDialog::deleteTarget()
{
	int row = targetsTable->currentRow();
	if (row < 0)
		return;

	QString id = targetsTable->item(row, 0)->data(Qt::UserRole).toString();
	int ret = QMessageBox::question(this, "Confirm Delete", "Are you sure you want to delete this target?");
	if (ret == QMessageBox::Yes) {
		MultistreamManager::instance().deleteTarget(id);
		reloadList();
	}
}

void SrtlaMultistreamDialog::saveSettings()
{
	MultistreamManager::instance().setSyncWithObs(syncWithObsCheck->isChecked());


	for (int i = 0; i < targetsTable->rowCount(); i++) {
		QString id = targetsTable->item(i, 0)->data(Qt::UserRole).toString();
		MultistreamTarget *t = MultistreamManager::instance().getTarget(id);
		if (t) {
			auto cfg = t->getConfig();
			cfg.enabled = (targetsTable->item(i, 3)->checkState() == Qt::Checked);
			MultistreamManager::instance().updateTarget(id, cfg);
		}
	}
	accept();
}

SrtlaMultistreamDock::SrtlaMultistreamDock(QWidget *parent) : QWidget(parent)
{
	setObjectName("srtla_multistream_dock");
	setMinimumSize(170, 95);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(6, 6, 6, 6);
	layout->setSpacing(6);

	// Top Header Bar: Status summary + Quick All/Settings actions
	QWidget *headerWidget = new QWidget(this);
	QHBoxLayout *headerLayout = new QHBoxLayout(headerWidget);
	headerLayout->setContentsMargins(0, 0, 0, 0);
	headerLayout->setSpacing(4);

	summaryLabel = new QLabel(headerWidget);
	summaryLabel->setWordWrap(true);
	summaryLabel->setMinimumWidth(60);
	summaryLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
	summaryLabel->setTextFormat(Qt::RichText);
	summaryLabel->setStyleSheet(
		"QLabel { "
		"  background-color: #18191c; "
		"  border: 1px solid #2e3138; "
		"  border-left: 3px solid #555860; "
		"  border-radius: 5px; "
		"  padding: 4px 8px; "
		"  font-size: 11px; "
		"  color: #dcddde; "
		"}");
	summaryLabel->setText("<span style='color:#888888; font-weight:bold;'>○ Idle</span> &nbsp;&bull;&nbsp; <b>Active:</b> 0/0");
	headerLayout->addWidget(summaryLabel, 1);

	startAllBtn = new QPushButton("▶ All", headerWidget);
	stopAllBtn = new QPushButton("⏹ All", headerWidget);
	configBtn = new QPushButton("⚙", headerWidget);

	startAllBtn->setCursor(Qt::PointingHandCursor);
	stopAllBtn->setCursor(Qt::PointingHandCursor);
	configBtn->setCursor(Qt::PointingHandCursor);

	startAllBtn->setToolTip("Start all enabled multistream targets");
	stopAllBtn->setToolTip("Stop all running multistream targets");
	configBtn->setToolTip("Open Multistream Configuration");

	startAllBtn->setStyleSheet(
		"QPushButton { background-color: #238636; color: #ffffff; font-weight: 600; border: 1px solid #2ea043; border-radius: 4px; padding: 3px 7px; font-size: 11px; } "
		"QPushButton:hover { background-color: #2ea043; } "
		"QPushButton:disabled { background-color: #23252b; color: #555860; border: 1px solid #2b2e36; }");
	stopAllBtn->setStyleSheet(
		"QPushButton { background-color: #da3633; color: #ffffff; font-weight: 600; border: 1px solid #f85149; border-radius: 4px; padding: 3px 7px; font-size: 11px; } "
		"QPushButton:hover { background-color: #f85149; } "
		"QPushButton:disabled { background-color: #23252b; color: #555860; border: 1px solid #2b2e36; }");
	configBtn->setStyleSheet(
		"QPushButton { background-color: #2b2d34; color: #dcddde; border: 1px solid #3c3f49; border-radius: 4px; padding: 3px 7px; font-size: 11px; } "
		"QPushButton:hover { background-color: #383b44; color: #ffffff; border-color: #4f5461; }");

	connect(startAllBtn, &QPushButton::clicked, this, []() {
		MultistreamManager::instance().startAll();
	});
	connect(stopAllBtn, &QPushButton::clicked, this, []() {
		MultistreamManager::instance().stopAll();
	});
	connect(configBtn, &QPushButton::clicked, this, [this]() {
		SrtlaMultistreamDialog dlg(this);
		dlg.exec();
	});

	headerLayout->addWidget(startAllBtn);
	headerLayout->addWidget(stopAllBtn);
	headerLayout->addWidget(configBtn);
	layout->addWidget(headerWidget);

	// Styled Responsive Targets Table (3 columns: Target, Status & Bitrate, Controls)
	statusTable = new QTableWidget(this);
	statusTable->setColumnCount(3);
	statusTable->setHorizontalHeaderLabels(QStringList() << "Target" << "Status & Bitrate" << "Controls");
	statusTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
	statusTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	statusTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	statusTable->horizontalHeader()->setStretchLastSection(false);
	statusTable->horizontalHeader()->setMinimumSectionSize(44);
	statusTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
	statusTable->setSelectionMode(QAbstractItemView::NoSelection);
	statusTable->setFocusPolicy(Qt::NoFocus);
	statusTable->setAlternatingRowColors(true);
	statusTable->setShowGrid(false);
	statusTable->setWordWrap(false);
	statusTable->verticalHeader()->setVisible(false);
	statusTable->verticalHeader()->setDefaultSectionSize(32);
	statusTable->setMinimumHeight(45);
	statusTable->setStyleSheet(
		"QTableWidget { "
		"  background-color: #1b1c20; "
		"  alternate-background-color: #202226; "
		"  color: #dcddde; "
		"  border: 1px solid #2e3138; "
		"  border-radius: 5px; "
		"  font-size: 11px; "
		"  outline: none; "
		"} "
		"QTableWidget::item { padding: 2px 6px; border: none; } "
		"QTableWidget::item:hover { background-color: #282b30; } "
		"QHeaderView::section { "
		"  background-color: #23252b; "
		"  color: #b9bbbe; "
		"  padding: 4px 6px; "
		"  font-size: 10px; "
		"  font-weight: 600; "
		"  text-transform: uppercase; "
		"  border: none; "
		"  border-bottom: 1px solid #32353d; "
		"  border-right: 1px solid #2b2d33; "
		"} "
		"QScrollBar:vertical { background: #18191c; width: 7px; margin: 0px; border-radius: 3px; } "
		"QScrollBar::handle:vertical { background: #3a3d45; min-height: 20px; border-radius: 3px; } "
		"QScrollBar::handle:vertical:hover { background: #4e525c; } "
		"QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; } "
		"QScrollBar:horizontal { background: #18191c; height: 7px; margin: 0px; border-radius: 3px; } "
		"QScrollBar::handle:horizontal { background: #3a3d45; min-width: 20px; border-radius: 3px; } "
		"QScrollBar::handle:horizontal:hover { background: #4e525c; } "
		"QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; }");
	layout->addWidget(statusTable, 1);

	connect(&MultistreamManager::instance(), &MultistreamManager::targetsChanged, this,
		&SrtlaMultistreamDock::updateList);
	connect(&MultistreamManager::instance(), &MultistreamManager::targetStatusChanged, this,
		&SrtlaMultistreamDock::updateList);

	metricsTimer = new QTimer(this);
	connect(metricsTimer, &QTimer::timeout, this, &SrtlaMultistreamDock::updateMetrics);
	metricsTimer->start(1000);

	applyResponsiveLayout(width());
	updateList();
}

void SrtlaMultistreamDock::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	applyResponsiveLayout(event->size().width());
}

void SrtlaMultistreamDock::applyResponsiveLayout(int w)
{
	bool newCompact = (w < 380);
	bool newUltraCompact = (w < 275);

	if (layout()) {
		int m = newUltraCompact ? 3 : 6;
		layout()->setContentsMargins(m, m, m, m);
		layout()->setSpacing(newUltraCompact ? 4 : 6);
	}

	if (startAllBtn && stopAllBtn) {
		startAllBtn->setText(newCompact ? "▶" : "▶ All");
		stopAllBtn->setText(newCompact ? "⏹" : "⏹ All");
		startAllBtn->setVisible(!newUltraCompact);
		stopAllBtn->setVisible(!newUltraCompact);
	}

	if (statusTable && statusTable->horizontalHeader()) {
		statusTable->setColumnHidden(1, newUltraCompact);
		if (newUltraCompact) {
			statusTable->setHorizontalHeaderLabels(QStringList() << "Target" << "Status" << "Act");
		} else if (newCompact) {
			statusTable->setHorizontalHeaderLabels(QStringList() << "Target" << "Status" << "Controls");
		} else {
			statusTable->setHorizontalHeaderLabels(QStringList() << "Target" << "Status & Bitrate" << "Controls");
		}

		for (int r = 0; r < statusTable->rowCount(); ++r) {
			QWidget *actionWidget = statusTable->cellWidget(r, 2);
			if (!actionWidget) continue;
			QPushButton *startBtn = actionWidget->findChild<QPushButton *>("startBtn");
			QPushButton *stopBtn = actionWidget->findChild<QPushButton *>("stopBtn");
			if (startBtn) startBtn->setText(newCompact ? "▶" : "▶ Start");
			if (stopBtn) stopBtn->setText(newCompact ? "⏹" : "⏹ Stop");
		}
	}

	bool changed = (newCompact != isCompact) || (newUltraCompact != isUltraCompact);
	isCompact = newCompact;
	isUltraCompact = newUltraCompact;
	if (changed) {
		updateMetrics();
	}
}

void SrtlaMultistreamDock::updateList()
{
	statusTable->setRowCount(0);
	auto targets = MultistreamManager::instance().getTargets();
	int enabledCount = 0;

	for (int i = 0; i < targets.size(); i++) {
		auto cfg = targets[i]->getConfig();
		if (!cfg.enabled)
			continue;

		enabledCount++;
		int row = statusTable->rowCount();
		statusTable->insertRow(row);
		statusTable->setRowHeight(row, 32);

		QTableWidgetItem *nameItem = new QTableWidgetItem(cfg.name);
		nameItem->setData(Qt::UserRole, cfg.id);
		nameItem->setTextAlignment(Qt::AlignVCenter | Qt::AlignLeft);
		statusTable->setItem(row, 0, nameItem);

		QTableWidgetItem *statusItem = new QTableWidgetItem();
		statusItem->setTextAlignment(Qt::AlignVCenter | Qt::AlignLeft);
		statusTable->setItem(row, 1, statusItem);

		QWidget *actionWidget = new QWidget();
		actionWidget->setStyleSheet("background: transparent;");
		QHBoxLayout *actionLayout = new QHBoxLayout(actionWidget);
		actionLayout->setContentsMargins(isUltraCompact ? 2 : 4, 2, isUltraCompact ? 2 : 4, 2);
		actionLayout->setSpacing(isUltraCompact ? 2 : 4);

		QPushButton *startBtn = new QPushButton(isCompact ? "▶" : "▶ Start");
		QPushButton *stopBtn = new QPushButton(isCompact ? "⏹" : "⏹ Stop");
		startBtn->setObjectName("startBtn");
		stopBtn->setObjectName("stopBtn");
		startBtn->setProperty("targetId", cfg.id);
		stopBtn->setProperty("targetId", cfg.id);
		startBtn->setCursor(Qt::PointingHandCursor);
		stopBtn->setCursor(Qt::PointingHandCursor);
		startBtn->setToolTip(QString("Start streaming to %1").arg(cfg.name));
		stopBtn->setToolTip(QString("Stop streaming to %1").arg(cfg.name));

		startBtn->setStyleSheet(
			"QPushButton { background-color: #238636; color: #ffffff; font-weight: 600; border: 1px solid #2ea043; border-radius: 4px; padding: 2px 8px; font-size: 11px; min-width: 18px; } "
			"QPushButton:hover { background-color: #2ea043; } "
			"QPushButton:disabled { background-color: #23252b; color: #555860; border: 1px solid #2b2e36; }");
		stopBtn->setStyleSheet(
			"QPushButton { background-color: #da3633; color: #ffffff; font-weight: 600; border: 1px solid #f85149; border-radius: 4px; padding: 2px 8px; font-size: 11px; min-width: 18px; } "
			"QPushButton:hover { background-color: #f85149; } "
			"QPushButton:disabled { background-color: #23252b; color: #555860; border: 1px solid #2b2e36; }");

		connect(startBtn, &QPushButton::clicked, this, &SrtlaMultistreamDock::startTarget);
		connect(stopBtn, &QPushButton::clicked, this, &SrtlaMultistreamDock::stopTarget);

		actionLayout->addWidget(startBtn);
		actionLayout->addWidget(stopBtn);
		statusTable->setCellWidget(row, 2, actionWidget);
	}

	if (enabledCount == 0) {
		statusTable->insertRow(0);
		statusTable->setRowHeight(0, 36);
		QTableWidgetItem *emptyItem = new QTableWidgetItem("No enabled targets — click ⚙ to configure");
		emptyItem->setForeground(QBrush(QColor("#888888")));
		emptyItem->setTextAlignment(Qt::AlignCenter);
		statusTable->setItem(0, 0, emptyItem);
		statusTable->setSpan(0, 0, 1, 3);
	}

	updateMetrics();
}

void SrtlaMultistreamDock::updateMetrics()
{
	auto targets = MultistreamManager::instance().getTargets();
	int enabledCount = 0;
	int activeCount = 0;
	double totalKbps = 0.0;

	for (int r = 0; r < statusTable->rowCount(); ++r) {
		QTableWidgetItem *nameItem = statusTable->item(r, 0);
		if (!nameItem) continue;
		QString id = nameItem->data(Qt::UserRole).toString();
		if (id.isEmpty()) continue; // Empty placeholder row

		MultistreamTarget *t = MultistreamManager::instance().getTarget(id);
		if (!t) continue;

		enabledCount++;
		auto cfg = t->getConfig();
		auto status = t->getStatus();
		QJsonObject m = t->getMetrics();

		uint64_t bytes = m["bytes"].toVariant().toULongLong();
		int dropped = m["frames_dropped"].toInt();
		int totalFrames = m["total_frames"].toInt();

		uint64_t prevBytes = prevBytesMap.value(id, bytes);
		if (bytes < prevBytes) prevBytes = bytes;
		prevBytesMap[id] = bytes;

		double kbps = (status == MultistreamTarget::STREAMING)
			? (((bytes - prevBytes) * 8.0) / 1000.0)
			: 0.0;
		totalKbps += kbps;

		QString statusText;
		QString dot = "○";
		QColor color("#888888");

		if (status == MultistreamTarget::STREAMING) {
			activeCount++;
			dot = "●";
			color = (dropped > 0) ? QColor("#f0b232") : QColor("#3fb950");
			statusText = isCompact
				? QString("● %1k").arg(QString::number(kbps, 'f', 0))
				: QString("● Live • %1 Kbps").arg(QString::number(kbps, 'f', 0));
		} else if (status == MultistreamTarget::STARTING) {
			dot = "◐";
			color = QColor("#58a6ff");
			statusText = isCompact ? "◐ Start..." : "◐ Starting...";
		} else if (status == MultistreamTarget::RECONNECTING) {
			dot = "⟳";
			color = QColor("#f0b232");
			statusText = isCompact ? "⟳ Retry..." : "⟳ Reconnecting...";
		} else if (status == MultistreamTarget::STOPPING) {
			dot = "◑";
			color = QColor("#888888");
			statusText = isCompact ? "◑ Stop..." : "◑ Stopping...";
		} else {
			dot = "○";
			color = QColor("#888888");
			statusText = "○ Stopped";
		}

		QString tooltip = QString("Target: %1\nURL: %2\nStatus: %3\nBitrate: %4 Kbps\nDropped Frames: %5 / %6")
			.arg(cfg.name, cfg.url, statusText)
			.arg(QString::number(kbps, 'f', 1))
			.arg(dropped)
			.arg(totalFrames);

		if (isUltraCompact) {
			QString compactName = cfg.name;
			if (status == MultistreamTarget::STREAMING && kbps > 0) {
				compactName += QString(" (%1k)").arg(QString::number(kbps, 'f', 0));
			}
			nameItem->setText(QString("%1 %2").arg(dot, compactName));
			nameItem->setForeground(QBrush(status == MultistreamTarget::STOPPED ? QColor("#9ca3af") : color));
		} else {
			nameItem->setText(cfg.name);
			nameItem->setForeground(QBrush(QColor("#dcddde")));
		}
		nameItem->setToolTip(tooltip);

		QTableWidgetItem *statusItem = statusTable->item(r, 1);
		if (statusItem) {
			statusItem->setText(statusText);
			statusItem->setForeground(QBrush(color));
			statusItem->setToolTip(tooltip);
		}

		QWidget *actionWidget = statusTable->cellWidget(r, 2);
		if (actionWidget) {
			QPushButton *startBtn = actionWidget->findChild<QPushButton *>("startBtn");
			QPushButton *stopBtn = actionWidget->findChild<QPushButton *>("stopBtn");
			bool isStopped = (status == MultistreamTarget::STOPPED);
			if (startBtn && stopBtn) {
				if (isCompact) {
					// Show single contextual button when narrow
					startBtn->setVisible(isStopped);
					stopBtn->setVisible(!isStopped);
				} else {
					startBtn->setVisible(true);
					stopBtn->setVisible(true);
				}
				startBtn->setEnabled(isStopped);
				stopBtn->setEnabled(!isStopped);
			}
		}
	}

	if (startAllBtn && stopAllBtn) {
		startAllBtn->setEnabled(enabledCount > 0 && activeCount < enabledCount);
		stopAllBtn->setEnabled(activeCount > 0);
	}

	if (summaryLabel) {
		QString accent = (activeCount > 0) ? "#3fb950" : "#555860";
		summaryLabel->setStyleSheet(QString(
			"QLabel { "
			"  background-color: #18191c; "
			"  border: 1px solid #2e3138; "
			"  border-left: 3px solid %1; "
			"  border-radius: 5px; "
			"  padding: %2; "
			"  font-size: 11px; "
			"  color: #dcddde; "
			"}")
			.arg(accent)
			.arg(isUltraCompact ? "3px 6px" : "4px 8px"));

		QString summaryHtml;
		if (activeCount > 0) {
			if (isUltraCompact) {
				summaryHtml = QString("<span style='color:#3fb950; font-weight:bold;'>● %1/%2</span> &bull; <span style='color:#3fb950; font-weight:bold;'>%3k</span>")
					.arg(activeCount).arg(enabledCount).arg(QString::number(totalKbps, 'f', 0));
			} else if (isCompact) {
				summaryHtml = QString("<span style='color:#3fb950; font-weight:bold;'>● Live</span> &bull; <b>%1/%2</b> &bull; <span style='color:#3fb950; font-weight:bold;'>%3 Kbps</span>")
					.arg(activeCount).arg(enabledCount).arg(QString::number(totalKbps, 'f', 0));
			} else {
				summaryHtml = QString("<span style='color:#3fb950; font-weight:bold;'>● Streaming</span> &nbsp;&bull;&nbsp; <b>Active:</b> %1/%2 &nbsp;&bull;&nbsp; <b>Out:</b> <span style='color:#3fb950; font-weight:bold;'>%3 Kbps</span>")
					.arg(activeCount).arg(enabledCount).arg(QString::number(totalKbps, 'f', 0));
			}
		} else {
			if (isUltraCompact) {
				summaryHtml = QString("<span style='color:#888888; font-weight:bold;'>○ Idle</span> &bull; <b>0/%1</b>").arg(enabledCount);
			} else {
				summaryHtml = QString("<span style='color:#888888; font-weight:bold;'>○ Idle</span> &nbsp;&bull;&nbsp; <b>Active:</b> 0/%1").arg(enabledCount);
			}
		}
		summaryLabel->setText(summaryHtml);
	}
}

void SrtlaMultistreamDock::startTarget()
{
	QPushButton *btn = qobject_cast<QPushButton *>(sender());
	if (btn) {
		QString id = btn->property("targetId").toString();
		auto t = MultistreamManager::instance().getTarget(id);
		if (t)
			t->start();
	}
}

void SrtlaMultistreamDock::stopTarget()
{
	QPushButton *btn = qobject_cast<QPushButton *>(sender());
	if (btn) {
		QString id = btn->property("targetId").toString();
		auto t = MultistreamManager::instance().getTarget(id);
		if (t)
			t->stop();
	}
}
