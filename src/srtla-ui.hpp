#pragma once

#include <QDockWidget>
#include <QLabel>
#include <QTimer>
#include <QTreeWidget>
#include <QMap>

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
