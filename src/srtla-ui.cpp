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

extern "C" {
    void srtla_get_connection_stats(bool *is_listening, int *active_groups, int *active_connections);
    void srtla_get_connection_details(int *listen_port, int *failed_conns, char* out_buffer, int max_len);
    void srtla_get_all_receivers_json(char *out_buffer, int max_len);
    void srtla_force_start_by_name(const char *name);
    void srtla_force_stop_by_name(const char *name);
    void srtla_force_restart_by_name(const char *name);
}

SrtlaStatusWidget::SrtlaStatusWidget(QWidget *parent)
    : QDockWidget("SRTLA Status", parent)
{
    QWidget *centralWidget = new QWidget(this);
    QVBoxLayout *mainLayout = new QVBoxLayout(centralWidget);

    QFont boldFont;
    boldFont.setBold(true);

    auto addRow = [&](const QString& titleText, QLabel*& labelOut, QVBoxLayout* layout) {
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
    
    QFrame* line1 = new QFrame();
    line1->setFrameShape(QFrame::HLine);
    line1->setFrameShadow(QFrame::Sunken);
    mainLayout->addWidget(line1);

    addRow("Active Encoders (Groups):", encodersLabel, mainLayout);
    addRow("Active Bonded Connections:", connectionsLabel, mainLayout);
    addRow("Failed/Dropped Connections:", failedConnectionsLabel, mainLayout);

    QFrame* line2 = new QFrame();
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
    receiversTable->setStyleSheet("QTableWidget { background-color: #1e1e1e; color: #d4d4d4; } QHeaderView::section { background-color: #2d2d2d; color: white; padding: 4px; }");
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
    treeWidget->setStyleSheet("QTreeWidget { background-color: #1e1e1e; color: #d4d4d4; } QHeaderView::section { background-color: #2d2d2d; color: white; padding: 4px; }");
    
    mainLayout->addWidget(treeWidget, 1); // Expand to fill available space
    
    QPushButton *logButton = new QPushButton("View Detailed Plugin Logs", this);
    connect(logButton, &QPushButton::clicked, this, &SrtlaStatusWidget::openLogFolder);
    mainLayout->addWidget(logButton);
    
    setWidget(centralWidget);
    setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

    // Set up timer
    updateTimer = new QTimer(this);
    connect(updateTimer, &QTimer::timeout, this, &SrtlaStatusWidget::updateStatus);
    updateTimer->start(500); // 500ms
    
    updateStatus(); // initial update
}

SrtlaStatusWidget::~SrtlaStatusWidget()
{
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
                
                QObject::connect(startBtn, &QPushButton::clicked, [name]() {
                    srtla_force_start_by_name(name.toUtf8().constData());
                });
                QObject::connect(stopBtn, &QPushButton::clicked, [name]() {
                    srtla_force_stop_by_name(name.toUtf8().constData());
                });
                QObject::connect(restartBtn, &QPushButton::clicked, [name]() {
                    srtla_force_restart_by_name(name.toUtf8().constData());
                });
                
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
                QList<QTreeWidgetItem*> found = treeWidget->findItems(nodeName, Qt::MatchExactly, 0);
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
                        if (groupItem->child(k)->text(0) == ip && groupItem->child(k)->text(1) == port) {
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

extern "C" void setup_srtla_menu() {
    QMainWindow *mainWindow = (QMainWindow *)obs_frontend_get_main_window();
    if (!mainWindow) return;

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
    QObject::connect(startAction, &QAction::triggered, []() {
        srtla_force_start_all();
    });
    
    QAction *restartAction = srtlaMenu->addAction("Restart All Listeners");
    QObject::connect(restartAction, &QAction::triggered, []() {
        srtla_force_restart_all();
    });
    
    QAction *stopAction = srtlaMenu->addAction("Stop All Listeners");
    QObject::connect(stopAction, &QAction::triggered, []() {
        srtla_force_stop_all();
    });
}
