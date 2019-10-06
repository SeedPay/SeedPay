// Copyright (c) 2011-2013 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_OVERVIEWPAGE_H
#define BITCOIN_QT_OVERVIEWPAGE_H

#include "amount.h"
#include "torrentclient.h"

#include <QWidget>

QT_BEGIN_NAMESPACE
class QModelIndex;
class QAction;
class QLabel;
class QProgressDialog;
class QSlider;
QT_END_NAMESPACE
class TorrentView;
class ClientModel;
class TransactionFilterProxy;
class TxViewDelegate;
class WalletModel;

namespace Ui
{
class OverviewPage;
}

/** Overview ("home") page widget */
class OverviewPage : public QWidget
{
    Q_OBJECT

public:
    explicit OverviewPage(QWidget* parent = 0);
    ~OverviewPage();

    void setClientModel(ClientModel* clientModel);
    void setWalletModel(WalletModel* walletModel);
    void showOutOfSyncWarning(bool fShow);
    void updateObfuscationProgress();
	QSize sizeHint() const Q_DECL_OVERRIDE;
	const TorrentClient *clientForRow(int row) const;
    void shutdownTorrents();
    QAction *newTorrentAction;

public slots:
    void obfuScationStatus();
    void setBalance(const CAmount& balance, const CAmount& unconfirmedBalance, const CAmount& immatureBalance, const CAmount& anonymizedBalance, const CAmount& watchOnlyBalance, const CAmount& watchUnconfBalance, const CAmount& watchImmatureBalance);

signals:
    void transactionClicked(const QModelIndex& index);

private:
    QTimer* timer;
    Ui::OverviewPage* ui;
    ClientModel* clientModel;
    WalletModel* walletModel;
    CAmount currentBalance;
    CAmount currentUnconfirmedBalance;
    CAmount currentImmatureBalance;
    CAmount currentAnonymizedBalance;
    CAmount currentWatchOnlyBalance;
    CAmount currentWatchUnconfBalance;
    CAmount currentWatchImmatureBalance;
    int nDisplayUnit;

    TxViewDelegate* txdelegate;
    TransactionFilterProxy* filter;
	
    int rowOfClient(TorrentClient *client) const;
    bool addTorrent(const QString &fileName, const QString &destinationFolder,
                    const QByteArray &resumeState = QByteArray());

    TorrentView *torrentView;
    QAction *pauseTorrentAction;
    QAction *removeTorrentAction;
    QAction *upActionTool;
    QAction *downActionTool;
    QSlider *uploadLimitSlider;
    QSlider *downloadLimitSlider;
    QLabel *uploadLimitLabel;
    QLabel *downloadLimitLabel;

    int uploadLimit;
    int downloadLimit;

    struct Job {
        TorrentClient *client;
        QString torrentFileName;
        QString destinationDirectory;
    };
    QList<Job> jobs;
    int jobsStopped;
    int jobsToStop;

    QString lastDirectory;
    QProgressDialog *quitDialog;

    bool saveChanges;

private slots:
    void toggleObfuscation();
    void obfuscationAuto();
    void obfuscationReset();
    void updateDisplayUnit();
    void handleTransactionClicked(const QModelIndex& index);
    void updateAlerts(const QString& warnings);
    void updateWatchOnlyLabels(bool showWatchOnly);
    void loadSettings();
    void saveSettings();

    bool addTorrent();
    void removeTorrent();
    void pauseTorrent();
    void moveTorrentUp();
    void moveTorrentDown();

    void torrentStopped();
    void torrentError(TorrentClient::Error error);

    void updateState(TorrentClient::State state);
    void updatePeerInfo();
    void updateProgress(int percent);
    void updateDownloadRate(int bytesPerSecond);
    void updateUploadRate(int bytesPerSecond);

    void setUploadLimit(int bytes);
    void setDownloadLimit(int bytes);

    void about();
    void setActionsEnabled();
};

#endif // BITCOIN_QT_OVERVIEWPAGE_H
