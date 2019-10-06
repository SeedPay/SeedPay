// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2018-2019 The Seedpay developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "overviewpage.h"
#include "ui_overviewpage.h"

#include "bitcoinunits.h"
#include "clientmodel.h"
#include "guiconstants.h"
#include "guiutil.h"
#include "init.h"
#include "obfuscation.h"
#include "obfuscationconfig.h"
#include "optionsmodel.h"
#include "transactionfilterproxy.h"
#include "transactiontablemodel.h"
#include "walletmodel.h"
#include "addtorrentdialog.h"
#include "ratecontroller.h"
#include "torrentclient.h"

#include <QAbstractItemDelegate>
#include <QtWidgets>
#include <QPainter>
#include <QSettings>
#include <QTimer>

#define DECORATION_SIZE 48
#define ICON_OFFSET 16
#define NUM_ITEMS 3

// TorrentViewDelegate is used to draw the progress bars.
class TorrentViewDelegate : public QItemDelegate
{
    Q_OBJECT
public:
    inline TorrentViewDelegate(OverviewPage *overviewPage) : QItemDelegate(overviewPage) {}

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index ) const Q_DECL_OVERRIDE
    {
        if (index.column() != 2) {
            QItemDelegate::paint(painter, option, index);
            return;
        }

        // Set up a QStyleOptionProgressBar to precisely mimic the
        // environment of a progress bar.
        QStyleOptionProgressBar progressBarOption;
        progressBarOption.state = QStyle::State_Enabled;
        progressBarOption.direction = QApplication::layoutDirection();
        progressBarOption.rect = option.rect;
        progressBarOption.fontMetrics = QApplication::fontMetrics();
        progressBarOption.minimum = 0;
        progressBarOption.maximum = 100;
        progressBarOption.textAlignment = Qt::AlignCenter;
        progressBarOption.textVisible = true;

        // Set the progress and text values of the style option.
        int progress = qobject_cast<OverviewPage *>(parent())->clientForRow(index.row())->progress();
        progressBarOption.progress = progress < 0 ? 0 : progress;
        progressBarOption.text = QString::asprintf("%d%%", progressBarOption.progress);

        // Draw the progress bar onto the view.
        QApplication::style()->drawControl(QStyle::CE_ProgressBar, &progressBarOption, painter);
    }
};

class TxViewDelegate : public QAbstractItemDelegate
{
    Q_OBJECT
public:
    TxViewDelegate() : QAbstractItemDelegate(), unit(BitcoinUnits::SEED)
    {
    }

    inline void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
    {
        painter->save();

        QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        QRect mainRect = option.rect;
        mainRect.moveLeft(ICON_OFFSET);
        QRect decorationRect(mainRect.topLeft(), QSize(DECORATION_SIZE, DECORATION_SIZE));
        int xspace = DECORATION_SIZE + 8;
        int ypad = 6;
        int halfheight = (mainRect.height() - 2 * ypad) / 2;
        QRect amountRect(mainRect.left() + xspace, mainRect.top() + ypad, mainRect.width() - xspace - ICON_OFFSET, halfheight);
        QRect addressRect(mainRect.left() + xspace, mainRect.top() + ypad + halfheight, mainRect.width() - xspace, halfheight);
        icon.paint(painter, decorationRect);

        QDateTime date = index.data(TransactionTableModel::DateRole).toDateTime();
        QString address = index.data(Qt::DisplayRole).toString();
        qint64 amount = index.data(TransactionTableModel::AmountRole).toLongLong();
        bool confirmed = index.data(TransactionTableModel::ConfirmedRole).toBool();
        QVariant value = index.data(Qt::ForegroundRole);
        QColor foreground = COLOR_BLACK;
        if (value.canConvert<QBrush>()) {
            QBrush brush = qvariant_cast<QBrush>(value);
            foreground = brush.color();
        }

        painter->setPen(foreground);
        QRect boundingRect;
        painter->drawText(addressRect, Qt::AlignLeft | Qt::AlignVCenter, address, &boundingRect);

        if (index.data(TransactionTableModel::WatchonlyRole).toBool()) {
            QIcon iconWatchonly = qvariant_cast<QIcon>(index.data(TransactionTableModel::WatchonlyDecorationRole));
            QRect watchonlyRect(boundingRect.right() + 5, mainRect.top() + ypad + halfheight, 16, halfheight);
            iconWatchonly.paint(painter, watchonlyRect);
        }

        if (amount < 0) {
            foreground = COLOR_NEGATIVE;
        } else if (!confirmed) {
            foreground = COLOR_UNCONFIRMED;
        } else {
            foreground = COLOR_BLACK;
        }
        painter->setPen(foreground);
        QString amountText = BitcoinUnits::formatWithUnit(unit, amount, true, BitcoinUnits::separatorAlways);
        if (!confirmed) {
            amountText = QString("[") + amountText + QString("]");
        }
        painter->drawText(amountRect, Qt::AlignRight | Qt::AlignVCenter, amountText);

        painter->setPen(COLOR_BLACK);
        painter->drawText(amountRect, Qt::AlignLeft | Qt::AlignVCenter, GUIUtil::dateTimeStr(date));

        painter->restore();
    }

    inline QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const
    {
        return QSize(DECORATION_SIZE, DECORATION_SIZE);
    }

    int unit;
};
#include "overviewpage.moc"

OverviewPage::OverviewPage(QWidget* parent) : QWidget(parent),
                                              ui(new Ui::OverviewPage),
                                              clientModel(0),
                                              walletModel(0),
                                              currentBalance(-1),
                                              currentUnconfirmedBalance(-1),
                                              currentImmatureBalance(-1),
                                              currentWatchOnlyBalance(-1),
                                              currentWatchUnconfBalance(-1),
                                              currentWatchImmatureBalance(-1),
                                              txdelegate(new TxViewDelegate()),
											  quitDialog(0),
											  saveChanges(false),
                                              filter(0)
{
    nDisplayUnit = 0; // just make sure it's not unitialized
    ui->setupUi(this);
    ui->frameObfuscation->setVisible(false);
    // Recent transactions
    ui->listTransactions->setItemDelegate(txdelegate);
    ui->listTransactions->setIconSize(QSize(DECORATION_SIZE, DECORATION_SIZE));
    ui->listTransactions->setMinimumHeight(NUM_ITEMS * (DECORATION_SIZE + 2));
    ui->listTransactions->setAttribute(Qt::WA_MacShowFocusRect, false);

    connect(ui->listTransactions, SIGNAL(clicked(QModelIndex)), this, SLOT(handleTransactionClicked(QModelIndex)));


    // init "out of sync" warning labels
    ui->labelWalletStatus->setText("(" + tr("out of sync") + ")");
    ui->labelObfuscationSyncStatus->setText("(" + tr("out of sync") + ")");
    ui->labelTransactionsStatus->setText("(" + tr("out of sync") + ")");

    if (fLiteMode) {
        ui->frameObfuscation->setVisible(false);
    } else {
        if (fMasterNode) {
            ui->toggleObfuscation->setText("(" + tr("Disabled") + ")");
            ui->obfuscationAuto->setText("(" + tr("Disabled") + ")");
            ui->obfuscationReset->setText("(" + tr("Disabled") + ")");
            ui->frameObfuscation->setEnabled(false);
        } else {
            if (!fEnableObfuscation) {
                ui->toggleObfuscation->setText(tr("Start Obfuscation"));
            } else {
                ui->toggleObfuscation->setText(tr("Stop Obfuscation"));
            }
            timer = new QTimer(this);
            connect(timer, SIGNAL(timeout()), this, SLOT(obfuScationStatus()));
            timer->start(1000);
        }
    }

    // start with displaying the "out of sync" warnings
    showOutOfSyncWarning(true);
	
	    // Initialize some static strings
    QStringList headers;
    headers << tr("Torrent") << tr("Peers/Seeds") << tr("Progress")
            << tr("Down rate") << tr("Up rate") << tr("Status");

    // Main torrent list
    torrentView = new TorrentView(this);
    torrentView->setItemDelegate(new TorrentViewDelegate(this));
    torrentView->setHeaderLabels(headers);
    torrentView->setSelectionBehavior(QAbstractItemView::SelectRows);
    torrentView->setAlternatingRowColors(true);
    torrentView->setRootIsDecorated(false);
    setCentralWidget(torrentView);

    // Set header resize modes and initial section sizes
    QFontMetrics fm = fontMetrics();
    QHeaderView *header = torrentView->header();
    header->resizeSection(0, fm.width("typical-name-for-a-torrent.torrent"));
    header->resizeSection(1, fm.width(headers.at(1) + "  "));
    header->resizeSection(2, fm.width(headers.at(2) + "  "));
    header->resizeSection(3, qMax(fm.width(headers.at(3) + "  "), fm.width(" 1234.0 KB/s ")));
    header->resizeSection(4, qMax(fm.width(headers.at(4) + "  "), fm.width(" 1234.0 KB/s ")));
    header->resizeSection(5, qMax(fm.width(headers.at(5) + "  "), fm.width(tr("Downloading") + "  ")));

    // Create common actions
    QAction *newTorrentAction = new QAction(QIcon(":/icons/bottom.png"), tr("Add &new torrent"), this);
    pauseTorrentAction = new QAction(QIcon(":/icons/player_pause.png"), tr("&Pause torrent"), this);
    removeTorrentAction = new QAction(QIcon(":/icons/player_stop.png"), tr("&Remove torrent"), this);

    // File menu
    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(newTorrentAction);
    fileMenu->addAction(pauseTorrentAction);
    fileMenu->addAction(removeTorrentAction);
    fileMenu->addSeparator();
    fileMenu->addAction(QIcon(":/icons/exit.png"), tr("E&xit"), this, SLOT(close()));

    // Help menu
    QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(tr("&About"), this, SLOT(about()));
    helpMenu->addAction(tr("About &Qt"), qApp, SLOT(aboutQt()));

    // Top toolbar
    QToolBar *topBar = new QToolBar(tr("Tools"));
    addToolBar(Qt::TopToolBarArea, topBar);
    topBar->setMovable(false);
    topBar->addAction(newTorrentAction);
    topBar->addAction(removeTorrentAction);
    topBar->addAction(pauseTorrentAction);
    topBar->addSeparator();
    downActionTool = topBar->addAction(QIcon(tr(":/icons/1downarrow.png")), tr("Move down"));
    upActionTool = topBar->addAction(QIcon(tr(":/icons/1uparrow.png")), tr("Move up"));

    // Bottom toolbar
    QToolBar *bottomBar = new QToolBar(tr("Rate control"));
    addToolBar(Qt::BottomToolBarArea, bottomBar);
    bottomBar->setMovable(false);
    downloadLimitSlider = new QSlider(Qt::Horizontal);
    downloadLimitSlider->setRange(0, 1000);
    bottomBar->addWidget(new QLabel(tr("Max download:")));
    bottomBar->addWidget(downloadLimitSlider);
    bottomBar->addWidget((downloadLimitLabel = new QLabel(tr("0 KB/s"))));
    downloadLimitLabel->setFixedSize(QSize(fm.width(tr("99999 KB/s")), fm.lineSpacing()));
    bottomBar->addSeparator();
    uploadLimitSlider = new QSlider(Qt::Horizontal);
    uploadLimitSlider->setRange(0, 1000);
    bottomBar->addWidget(new QLabel(tr("Max upload:")));
    bottomBar->addWidget(uploadLimitSlider);
    bottomBar->addWidget((uploadLimitLabel = new QLabel(tr("0 KB/s"))));
    uploadLimitLabel->setFixedSize(QSize(fm.width(tr("99999 KB/s")), fm.lineSpacing()));

#ifdef Q_OS_OSX
    setUnifiedTitleAndToolBarOnMac(true);
#endif

    // Set up connections
    connect(torrentView, SIGNAL(itemSelectionChanged()),
            this, SLOT(setActionsEnabled()));
    connect(torrentView, SIGNAL(fileDropped(QString)),
            this, SLOT(acceptFileDrop(QString)));
    connect(uploadLimitSlider, SIGNAL(valueChanged(int)),
            this, SLOT(setUploadLimit(int)));
    connect(downloadLimitSlider, SIGNAL(valueChanged(int)),
            this, SLOT(setDownloadLimit(int)));
    connect(newTorrentAction, SIGNAL(triggered()),
            this, SLOT(addTorrent()));
    connect(pauseTorrentAction, SIGNAL(triggered()),
            this, SLOT(pauseTorrent()));
    connect(removeTorrentAction, SIGNAL(triggered()),
            this, SLOT(removeTorrent()));
    connect(upActionTool, SIGNAL(triggered(bool)),
            this, SLOT(moveTorrentUp()));
    connect(downActionTool, SIGNAL(triggered(bool)),
            this, SLOT(moveTorrentDown()));

    // Load settings and start
    setWindowTitle(tr("Torrent Client"));
    setActionsEnabled();
    QMetaObject::invokeMethod(this, "loadSettings", Qt::QueuedConnection);
	
}

void OverviewPage::handleTransactionClicked(const QModelIndex& index)
{
    if (filter)
        emit transactionClicked(filter->mapToSource(index));
}

OverviewPage::~OverviewPage()
{
    if (!fLiteMode && !fMasterNode) disconnect(timer, SIGNAL(timeout()), this, SLOT(obfuScationStatus()));
    delete ui;
}

void OverviewPage::setBalance(const CAmount& balance, const CAmount& unconfirmedBalance, const CAmount& immatureBalance, const CAmount& anonymizedBalance, const CAmount& watchOnlyBalance, const CAmount& watchUnconfBalance, const CAmount& watchImmatureBalance)
{
    currentBalance = balance;
    currentUnconfirmedBalance = unconfirmedBalance;
    currentImmatureBalance = immatureBalance;
    currentAnonymizedBalance = anonymizedBalance;
    currentWatchOnlyBalance = watchOnlyBalance;
    currentWatchUnconfBalance = watchUnconfBalance;
    currentWatchImmatureBalance = watchImmatureBalance;


    ui->labelBalance->setText(BitcoinUnits::floorHtmlWithUnit(nDisplayUnit, balance - immatureBalance, false, BitcoinUnits::separatorAlways));
    ui->labelUnconfirmed->setText(BitcoinUnits::floorHtmlWithUnit(nDisplayUnit, unconfirmedBalance, false, BitcoinUnits::separatorAlways));
    ui->labelImmature->setText(BitcoinUnits::floorHtmlWithUnit(nDisplayUnit, immatureBalance, false, BitcoinUnits::separatorAlways));
    ui->labelAnonymized->setText(BitcoinUnits::floorHtmlWithUnit(nDisplayUnit, anonymizedBalance, false, BitcoinUnits::separatorAlways));
    ui->labelTotal->setText(BitcoinUnits::floorHtmlWithUnit(nDisplayUnit, balance + unconfirmedBalance, false, BitcoinUnits::separatorAlways));


    ui->labelWatchAvailable->setText(BitcoinUnits::floorHtmlWithUnit(nDisplayUnit, watchOnlyBalance, false, BitcoinUnits::separatorAlways));
    ui->labelWatchPending->setText(BitcoinUnits::floorHtmlWithUnit(nDisplayUnit, watchUnconfBalance, false, BitcoinUnits::separatorAlways));
    ui->labelWatchImmature->setText(BitcoinUnits::floorHtmlWithUnit(nDisplayUnit, watchImmatureBalance, false, BitcoinUnits::separatorAlways));
    ui->labelWatchTotal->setText(BitcoinUnits::floorHtmlWithUnit(nDisplayUnit, watchOnlyBalance + watchUnconfBalance + watchImmatureBalance, false, BitcoinUnits::separatorAlways));

    // only show immature (newly mined) balance if it's non-zero, so as not to complicate things
    // for the non-mining users
    bool showImmature = immatureBalance != 0;
    bool showWatchOnlyImmature = watchImmatureBalance != 0;

    // for symmetry reasons also show immature label when the watch-only one is shown
    ui->labelImmature->setVisible(showImmature || showWatchOnlyImmature);
    ui->labelImmatureText->setVisible(showImmature || showWatchOnlyImmature);
    ui->labelWatchImmature->setVisible(showWatchOnlyImmature); // show watch-only immature balance

    updateObfuscationProgress();

    static int cachedTxLocks = 0;

    if (cachedTxLocks != nCompleteTXLocks) {
        cachedTxLocks = nCompleteTXLocks;
        ui->listTransactions->update();
    }
}

// show/hide watch-only labels
void OverviewPage::updateWatchOnlyLabels(bool showWatchOnly)
{
    ui->labelSpendable->setVisible(showWatchOnly);      // show spendable label (only when watch-only is active)
    ui->labelWatchonly->setVisible(showWatchOnly);      // show watch-only label
    ui->lineWatchBalance->setVisible(showWatchOnly);    // show watch-only balance separator line
    ui->labelWatchAvailable->setVisible(showWatchOnly); // show watch-only available balance
    ui->labelWatchPending->setVisible(showWatchOnly);   // show watch-only pending balance
    ui->labelWatchTotal->setVisible(showWatchOnly);     // show watch-only total balance

    if (!showWatchOnly) {
        ui->labelWatchImmature->hide();
    } else {
        ui->labelBalance->setIndent(20);
        ui->labelUnconfirmed->setIndent(20);
        ui->labelImmature->setIndent(20);
        ui->labelTotal->setIndent(20);
    }
}

void OverviewPage::setClientModel(ClientModel* model)
{
    this->clientModel = model;
    if (model) {
        // Show warning if this is a prerelease version
        connect(model, SIGNAL(alertsChanged(QString)), this, SLOT(updateAlerts(QString)));
        updateAlerts(model->getStatusBarWarnings());
    }
}

void OverviewPage::setWalletModel(WalletModel* model)
{
    this->walletModel = model;
    if (model && model->getOptionsModel()) {
        // Set up transaction list
        filter = new TransactionFilterProxy();
        filter->setSourceModel(model->getTransactionTableModel());
        filter->setLimit(NUM_ITEMS);
        filter->setDynamicSortFilter(true);
        filter->setSortRole(Qt::EditRole);
        filter->setShowInactive(false);
        filter->sort(TransactionTableModel::Date, Qt::DescendingOrder);

        ui->listTransactions->setModel(filter);
        ui->listTransactions->setModelColumn(TransactionTableModel::ToAddress);

        // Keep up to date with wallet
        setBalance(model->getBalance(), model->getUnconfirmedBalance(), model->getImmatureBalance(), model->getAnonymizedBalance(),
            model->getWatchBalance(), model->getWatchUnconfirmedBalance(), model->getWatchImmatureBalance());
        connect(model, SIGNAL(balanceChanged(CAmount, CAmount, CAmount, CAmount, CAmount, CAmount, CAmount)), this, SLOT(setBalance(CAmount, CAmount, CAmount, CAmount, CAmount, CAmount, CAmount)));

        connect(model->getOptionsModel(), SIGNAL(displayUnitChanged(int)), this, SLOT(updateDisplayUnit()));

        connect(ui->obfuscationAuto, SIGNAL(clicked()), this, SLOT(obfuscationAuto()));
        connect(ui->obfuscationReset, SIGNAL(clicked()), this, SLOT(obfuscationReset()));
        connect(ui->toggleObfuscation, SIGNAL(clicked()), this, SLOT(toggleObfuscation()));
        updateWatchOnlyLabels(model->haveWatchOnly());
        connect(model, SIGNAL(notifyWatchonlyChanged(bool)), this, SLOT(updateWatchOnlyLabels(bool)));
    }

    // update the display unit, to not use the default ("SEED")
    updateDisplayUnit();
}

void OverviewPage::updateDisplayUnit()
{
    if (walletModel && walletModel->getOptionsModel()) {
        nDisplayUnit = walletModel->getOptionsModel()->getDisplayUnit();
        if (currentBalance != -1)
            setBalance(currentBalance, currentUnconfirmedBalance, currentImmatureBalance, currentAnonymizedBalance,
                currentWatchOnlyBalance, currentWatchUnconfBalance, currentWatchImmatureBalance);

        // Update txdelegate->unit with the current unit
        txdelegate->unit = nDisplayUnit;

        ui->listTransactions->update();
    }
}

void OverviewPage::updateAlerts(const QString& warnings)
{
    this->ui->labelAlerts->setVisible(!warnings.isEmpty());
    this->ui->labelAlerts->setText(warnings);
}

void OverviewPage::showOutOfSyncWarning(bool fShow)
{
    ui->labelWalletStatus->setVisible(fShow);
    ui->labelObfuscationSyncStatus->setVisible(fShow);
    ui->labelTransactionsStatus->setVisible(fShow);
}

void OverviewPage::updateObfuscationProgress()
{
    if (!masternodeSync.IsBlockchainSynced() || ShutdownRequested()) return;

    if (!pwalletMain) return;

    QString strAmountAndRounds;
    QString strAnonymizeSeedpayAmount = BitcoinUnits::formatHtmlWithUnit(nDisplayUnit, nAnonymizeSeedpayAmount * COIN, false, BitcoinUnits::separatorAlways);

    if (currentBalance == 0) {
        ui->obfuscationProgress->setValue(0);
        ui->obfuscationProgress->setToolTip(tr("No inputs detected"));

        // when balance is zero just show info from settings
        strAnonymizeSeedpayAmount = strAnonymizeSeedpayAmount.remove(strAnonymizeSeedpayAmount.indexOf("."), BitcoinUnits::decimals(nDisplayUnit) + 1);
        strAmountAndRounds = strAnonymizeSeedpayAmount + " / " + tr("%n Rounds", "", nObfuscationRounds);

        ui->labelAmountRounds->setToolTip(tr("No inputs detected"));
        ui->labelAmountRounds->setText(strAmountAndRounds);
        return;
    }

    CAmount nDenominatedConfirmedBalance;
    CAmount nDenominatedUnconfirmedBalance;
    CAmount nAnonymizableBalance;
    CAmount nNormalizedAnonymizedBalance;
    double nAverageAnonymizedRounds;

    {
        TRY_LOCK(cs_main, lockMain);
        if (!lockMain) return;

        nDenominatedConfirmedBalance = pwalletMain->GetDenominatedBalance();
        nDenominatedUnconfirmedBalance = pwalletMain->GetDenominatedBalance(true);
        nAnonymizableBalance = pwalletMain->GetAnonymizableBalance();
        nNormalizedAnonymizedBalance = pwalletMain->GetNormalizedAnonymizedBalance();
        nAverageAnonymizedRounds = pwalletMain->GetAverageAnonymizedRounds();
    }

    CAmount nMaxToAnonymize = nAnonymizableBalance + currentAnonymizedBalance + nDenominatedUnconfirmedBalance;

    // If it's more than the anon threshold, limit to that.
    if (nMaxToAnonymize > nAnonymizeSeedpayAmount * COIN) nMaxToAnonymize = nAnonymizeSeedpayAmount * COIN;

    if (nMaxToAnonymize == 0) return;

    if (nMaxToAnonymize >= nAnonymizeSeedpayAmount * COIN) {
        ui->labelAmountRounds->setToolTip(tr("Found enough compatible inputs to anonymize %1")
                                              .arg(strAnonymizeSeedpayAmount));
        strAnonymizeSeedpayAmount = strAnonymizeSeedpayAmount.remove(strAnonymizeSeedpayAmount.indexOf("."), BitcoinUnits::decimals(nDisplayUnit) + 1);
        strAmountAndRounds = strAnonymizeSeedpayAmount + " / " + tr("%n Rounds", "", nObfuscationRounds);
    } else {
        QString strMaxToAnonymize = BitcoinUnits::formatHtmlWithUnit(nDisplayUnit, nMaxToAnonymize, false, BitcoinUnits::separatorAlways);
        ui->labelAmountRounds->setToolTip(tr("Not enough compatible inputs to anonymize <span style='color:red;'>%1</span>,<br>"
                                             "will anonymize <span style='color:red;'>%2</span> instead")
                                              .arg(strAnonymizeSeedpayAmount)
                                              .arg(strMaxToAnonymize));
        strMaxToAnonymize = strMaxToAnonymize.remove(strMaxToAnonymize.indexOf("."), BitcoinUnits::decimals(nDisplayUnit) + 1);
        strAmountAndRounds = "<span style='color:red;'>" +
                             QString(BitcoinUnits::factor(nDisplayUnit) == 1 ? "" : "~") + strMaxToAnonymize +
                             " / " + tr("%n Rounds", "", nObfuscationRounds) + "</span>";
    }
    ui->labelAmountRounds->setText(strAmountAndRounds);

    // calculate parts of the progress, each of them shouldn't be higher than 1
    // progress of denominating
    float denomPart = 0;
    // mixing progress of denominated balance
    float anonNormPart = 0;
    // completeness of full amount anonimization
    float anonFullPart = 0;

    CAmount denominatedBalance = nDenominatedConfirmedBalance + nDenominatedUnconfirmedBalance;
    denomPart = (float)denominatedBalance / nMaxToAnonymize;
    denomPart = denomPart > 1 ? 1 : denomPart;
    denomPart *= 100;

    anonNormPart = (float)nNormalizedAnonymizedBalance / nMaxToAnonymize;
    anonNormPart = anonNormPart > 1 ? 1 : anonNormPart;
    anonNormPart *= 100;

    anonFullPart = (float)currentAnonymizedBalance / nMaxToAnonymize;
    anonFullPart = anonFullPart > 1 ? 1 : anonFullPart;
    anonFullPart *= 100;

    // apply some weights to them ...
    float denomWeight = 1;
    float anonNormWeight = nObfuscationRounds;
    float anonFullWeight = 2;
    float fullWeight = denomWeight + anonNormWeight + anonFullWeight;
    // ... and calculate the whole progress
    float denomPartCalc = ceilf((denomPart * denomWeight / fullWeight) * 100) / 100;
    float anonNormPartCalc = ceilf((anonNormPart * anonNormWeight / fullWeight) * 100) / 100;
    float anonFullPartCalc = ceilf((anonFullPart * anonFullWeight / fullWeight) * 100) / 100;
    float progress = denomPartCalc + anonNormPartCalc + anonFullPartCalc;
    if (progress >= 100) progress = 100;

    ui->obfuscationProgress->setValue(progress);

    QString strToolPip = ("<b>" + tr("Overall progress") + ": %1%</b><br/>" +
                          tr("Denominated") + ": %2%<br/>" +
                          tr("Mixed") + ": %3%<br/>" +
                          tr("Anonymized") + ": %4%<br/>" +
                          tr("Denominated inputs have %5 of %n rounds on average", "", nObfuscationRounds))
                             .arg(progress)
                             .arg(denomPart)
                             .arg(anonNormPart)
                             .arg(anonFullPart)
                             .arg(nAverageAnonymizedRounds);
    ui->obfuscationProgress->setToolTip(strToolPip);
}


void OverviewPage::obfuScationStatus()
{
    static int64_t nLastDSProgressBlockTime = 0;

    int nBestHeight = chainActive.Tip()->nHeight;

    // we we're processing more then 1 block per second, we'll just leave
    if (((nBestHeight - obfuScationPool.cachedNumBlocks) / (GetTimeMillis() - nLastDSProgressBlockTime + 1) > 1)) return;
    nLastDSProgressBlockTime = GetTimeMillis();

    if (!fEnableObfuscation) {
        if (nBestHeight != obfuScationPool.cachedNumBlocks) {
            obfuScationPool.cachedNumBlocks = nBestHeight;
            updateObfuscationProgress();

            ui->obfuscationEnabled->setText(tr("Disabled"));
            ui->obfuscationStatus->setText("");
            ui->toggleObfuscation->setText(tr("Start Obfuscation"));
        }

        return;
    }

    // check obfuscation status and unlock if needed
    if (nBestHeight != obfuScationPool.cachedNumBlocks) {
        // Balance and number of transactions might have changed
        obfuScationPool.cachedNumBlocks = nBestHeight;
        updateObfuscationProgress();

        ui->obfuscationEnabled->setText(tr("Enabled"));
    }

    QString strStatus = QString(obfuScationPool.GetStatus().c_str());

    QString s = tr("Last Obfuscation message:\n") + strStatus;

    if (s != ui->obfuscationStatus->text())
        LogPrintf("Last Obfuscation message: %s\n", strStatus.toStdString());

    ui->obfuscationStatus->setText(s);

    if (obfuScationPool.sessionDenom == 0) {
        ui->labelSubmittedDenom->setText(tr("N/A"));
    } else {
        std::string out;
        obfuScationPool.GetDenominationsToString(obfuScationPool.sessionDenom, out);
        QString s2(out.c_str());
        ui->labelSubmittedDenom->setText(s2);
    }
}

void OverviewPage::obfuscationAuto()
{
    obfuScationPool.DoAutomaticDenominating();
}

void OverviewPage::obfuscationReset()
{
    obfuScationPool.Reset();

    QMessageBox::warning(this, tr("Obfuscation"),
        tr("Obfuscation was successfully reset."),
        QMessageBox::Ok, QMessageBox::Ok);
}

void OverviewPage::toggleObfuscation()
{
    QSettings settings;
    // Popup some information on first mixing
    QString hasMixed = settings.value("hasMixed").toString();
    if (hasMixed.isEmpty()) {
        QMessageBox::information(this, tr("Obfuscation"),
            tr("If you don't want to see internal Obfuscation fees/transactions select \"Most Common\" as Type on the \"Transactions\" tab."),
            QMessageBox::Ok, QMessageBox::Ok);
        settings.setValue("hasMixed", "hasMixed");
    }
    if (!fEnableObfuscation) {
        CAmount balance = currentBalance;
        float minAmount = 14.90 * COIN;
        if (balance < minAmount) {
            QString strMinAmount(BitcoinUnits::formatWithUnit(nDisplayUnit, minAmount));
            QMessageBox::warning(this, tr("Obfuscation"),
                tr("Obfuscation requires at least %1 to use.").arg(strMinAmount),
                QMessageBox::Ok, QMessageBox::Ok);
            return;
        }

        // if wallet is locked, ask for a passphrase
        if (walletModel->getEncryptionStatus() == WalletModel::Locked) {
            WalletModel::UnlockContext ctx(walletModel->requestUnlock(false));
            if (!ctx.isValid()) {
                //unlock was cancelled
                obfuScationPool.cachedNumBlocks = std::numeric_limits<int>::max();
                QMessageBox::warning(this, tr("Obfuscation"),
                    tr("Wallet is locked and user declined to unlock. Disabling Obfuscation."),
                    QMessageBox::Ok, QMessageBox::Ok);
                if (fDebug) LogPrintf("Wallet is locked and user declined to unlock. Disabling Obfuscation.\n");
                return;
            }
        }
    }

    fEnableObfuscation = !fEnableObfuscation;
    obfuScationPool.cachedNumBlocks = std::numeric_limits<int>::max();

    if (!fEnableObfuscation) {
        ui->toggleObfuscation->setText(tr("Start Obfuscation"));
        obfuScationPool.UnlockCoins();
    } else {
        ui->toggleObfuscation->setText(tr("Stop Obfuscation"));

        /* show obfuscation configuration if client has defaults set */

        if (nAnonymizeSeedpayAmount == 0) {
            ObfuscationConfig dlg(this);
            dlg.setModel(walletModel);
            dlg.exec();
        }
    }
}

QSize OverviewPage::sizeHint() const
{
    const QHeaderView *header = torrentView->header();

    // Add up the sizes of all header sections. The last section is
    // stretched, so its size is relative to the size of the width;
    // instead of counting it, we count the size of its largest value.
    int width = fontMetrics().width(tr("Downloading") + "  ");
    for (int i = 0; i < header->count() - 1; ++i)
        width += header->sectionSize(i);

    return QSize(width, QOverviewPage::sizeHint().height())
        .expandedTo(QApplication::globalStrut());
}

const TorrentClient *OverviewPage::clientForRow(int row) const
{
    // Return the client at the given row.
    return jobs.at(row).client;
}

int OverviewPage::rowOfClient(TorrentClient *client) const
{
    // Return the row that displays this client's status, or -1 if the
    // client is not known.
    int row = 0;
    foreach (Job job, jobs) {
        if (job.client == client)
            return row;
        ++row;
    }
    return -1;
}

void OverviewPage::loadSettings()
{
    // Load base settings (last working directory, upload/download limits).
    QSettings settings("QtProject", "Torrent");
    lastDirectory = settings.value("LastDirectory").toString();
    if (lastDirectory.isEmpty())
        lastDirectory = QDir::currentPath();
    int up = settings.value("UploadLimit").toInt();
    int down = settings.value("DownloadLimit").toInt();
    uploadLimitSlider->setValue(up ? up : 170);
    downloadLimitSlider->setValue(down ? down : 550);

    // Resume all previous downloads.
    int size = settings.beginReadArray("Torrents");
    for (int i = 0; i < size; ++i) {
        settings.setArrayIndex(i);
        QByteArray resumeState = settings.value("resumeState").toByteArray();
        QString fileName = settings.value("sourceFileName").toString();
        QString dest = settings.value("destinationFolder").toString();

        if (addTorrent(fileName, dest, resumeState)) {
            TorrentClient *client = jobs.last().client;
            client->setDownloadedBytes(settings.value("downloadedBytes").toLongLong());
            client->setUploadedBytes(settings.value("uploadedBytes").toLongLong());
        }
    }
}

bool OverviewPage::addTorrent()
{
    // Show the file dialog, let the user select what torrent to start downloading.
    QString fileName = QFileDialog::getOpenFileName(this, tr("Choose a torrent file"),
                                                    lastDirectory,
                                                    tr("Torrents (*.torrent);;"
                                                       " All files (*.*)"));
    if (fileName.isEmpty())
        return false;
    lastDirectory = QFileInfo(fileName).absolutePath();

    // Show the "Add Torrent" dialog.
    AddTorrentDialog *addTorrentDialog = new AddTorrentDialog(this);
    addTorrentDialog->setTorrent(fileName);
    addTorrentDialog->deleteLater();
    if (!addTorrentDialog->exec())
        return false;

    // Add the torrent to our list of downloads
    addTorrent(fileName, addTorrentDialog->destinationFolder());
    if (!saveChanges) {
        saveChanges = true;
        QTimer::singleShot(1000, this, SLOT(saveSettings()));
    }
    return true;
}

void OverviewPage::removeTorrent()
{
    // Find the row of the current item, and find the torrent client
    // for that row.
    int row = torrentView->indexOfTopLevelItem(torrentView->currentItem());
    TorrentClient *client = jobs.at(row).client;

    // Stop the client.
    client->disconnect();
    connect(client, SIGNAL(stopped()), this, SLOT(torrentStopped()));
    client->stop();

    // Remove the row from the view.
    delete torrentView->takeTopLevelItem(row);
    jobs.removeAt(row);
    setActionsEnabled();

    saveChanges = true;
    saveSettings();
}

void OverviewPage::torrentStopped()
{
    // Schedule the client for deletion.
    TorrentClient *client = qobject_cast<TorrentClient *>(sender());
    client->deleteLater();

    // If the quit dialog is shown, update its progress.
    if (quitDialog) {
        if (++jobsStopped == jobsToStop)
            quitDialog->close();
    }
}

void OverviewPage::torrentError(TorrentClient::Error)
{
    // Delete the client.
    TorrentClient *client = qobject_cast<TorrentClient *>(sender());
    int row = rowOfClient(client);
    QString fileName = jobs.at(row).torrentFileName;
    jobs.removeAt(row);

    // Display the warning.
    QMessageBox::warning(this, tr("Error"),
                         tr("An error occurred while downloading %0: %1")
                         .arg(fileName)
                         .arg(client->errorString()));

    delete torrentView->takeTopLevelItem(row);
    client->deleteLater();
}

bool OverviewPage::addTorrent(const QString &fileName, const QString &destinationFolder,
                            const QByteArray &resumeState)
{
    // Check if the torrent is already being downloaded.
    foreach (Job job, jobs) {
        if (job.torrentFileName == fileName && job.destinationDirectory == destinationFolder) {
            QMessageBox::warning(this, tr("Already downloading"),
                                 tr("The torrent file %1 is "
                                    "already being downloaded.").arg(fileName));
            return false;
        }
    }

    // Create a new torrent client and attempt to parse the torrent data.
    TorrentClient *client = new TorrentClient(this);
    if (!client->setTorrent(fileName)) {
        QMessageBox::warning(this, tr("Error"),
                             tr("The torrent file %1 cannot not be opened/resumed.").arg(fileName));
        delete client;
        return false;
    }
    client->setDestinationFolder(destinationFolder);
    client->setDumpedState(resumeState);

    // Setup the client connections.
    connect(client, SIGNAL(stateChanged(TorrentClient::State)), this, SLOT(updateState(TorrentClient::State)));
    connect(client, SIGNAL(peerInfoUpdated()), this, SLOT(updatePeerInfo()));
    connect(client, SIGNAL(progressUpdated(int)), this, SLOT(updateProgress(int)));
    connect(client, SIGNAL(downloadRateUpdated(int)), this, SLOT(updateDownloadRate(int)));
    connect(client, SIGNAL(uploadRateUpdated(int)), this, SLOT(updateUploadRate(int)));
    connect(client, SIGNAL(stopped()), this, SLOT(torrentStopped()));
    connect(client, SIGNAL(error(TorrentClient::Error)), this, SLOT(torrentError(TorrentClient::Error)));

    // Add the client to the list of downloading jobs.
    Job job;
    job.client = client;
    job.torrentFileName = fileName;
    job.destinationDirectory = destinationFolder;
    jobs << job;

    // Create and add a row in the torrent view for this download.
    QTreeWidgetItem *item = new QTreeWidgetItem(torrentView);

    QString baseFileName = QFileInfo(fileName).fileName();
    if (baseFileName.toLower().endsWith(".torrent"))
        baseFileName.remove(baseFileName.size() - 8);

    item->setText(0, baseFileName);
    item->setToolTip(0, tr("Torrent: %1<br>Destination: %2")
                     .arg(baseFileName).arg(destinationFolder));
    item->setText(1, tr("0/0"));
    item->setText(2, "0");
    item->setText(3, "0.0 KB/s");
    item->setText(4, "0.0 KB/s");
    item->setText(5, tr("Idle"));
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    item->setTextAlignment(1, Qt::AlignHCenter);

    if (!saveChanges) {
        saveChanges = true;
        QTimer::singleShot(5000, this, SLOT(saveSettings()));
    }
    client->start();
    return true;
}

void OverviewPage::saveSettings()
{
    if (!saveChanges)
      return;
    saveChanges = false;

    // Prepare and reset the settings
    QSettings settings("QtProject", "Torrent");
    settings.clear();

    settings.setValue("LastDirectory", lastDirectory);
    settings.setValue("UploadLimit", uploadLimitSlider->value());
    settings.setValue("DownloadLimit", downloadLimitSlider->value());

    // Store data on all known torrents
    settings.beginWriteArray("Torrents");
    for (int i = 0; i < jobs.size(); ++i) {
        settings.setArrayIndex(i);
        settings.setValue("sourceFileName", jobs.at(i).torrentFileName);
        settings.setValue("destinationFolder", jobs.at(i).destinationDirectory);
        settings.setValue("uploadedBytes", jobs.at(i).client->uploadedBytes());
        settings.setValue("downloadedBytes", jobs.at(i).client->downloadedBytes());
        settings.setValue("resumeState", jobs.at(i).client->dumpedState());
    }
    settings.endArray();
    settings.sync();
}

void OverviewPage::updateState(TorrentClient::State)
{
    // Update the state string whenever the client's state changes.
    TorrentClient *client = qobject_cast<TorrentClient *>(sender());
    int row = rowOfClient(client);
    QTreeWidgetItem *item = torrentView->topLevelItem(row);
    if (item) {
        item->setToolTip(0, tr("Torrent: %1<br>Destination: %2<br>State: %3")
                         .arg(jobs.at(row).torrentFileName)
                         .arg(jobs.at(row).destinationDirectory)
                         .arg(client->stateString()));

        item->setText(5, client->stateString());
    }
    setActionsEnabled();
}

void OverviewPage::updatePeerInfo()
{
    // Update the number of connected, visited, seed and leecher peers.
    TorrentClient *client = qobject_cast<TorrentClient *>(sender());
    int row = rowOfClient(client);

    QTreeWidgetItem *item = torrentView->topLevelItem(row);
    item->setText(1, tr("%1/%2").arg(client->connectedPeerCount())
                  .arg(client->seedCount()));
}

void OverviewPage::updateProgress(int percent)
{
    TorrentClient *client = qobject_cast<TorrentClient *>(sender());
    int row = rowOfClient(client);

    // Update the progressbar.
    QTreeWidgetItem *item = torrentView->topLevelItem(row);
    if (item)
        item->setText(2, QString::number(percent));
}

void OverviewPage::setActionsEnabled()
{
    // Find the view item and client for the current row, and update
    // the states of the actions.
    QTreeWidgetItem *item = 0;
    if (!torrentView->selectedItems().isEmpty())
        item = torrentView->selectedItems().first();
    TorrentClient *client = item ? jobs.at(torrentView->indexOfTopLevelItem(item)).client : 0;
    bool pauseEnabled = client && ((client->state() == TorrentClient::Paused)
                                       ||  (client->state() > TorrentClient::Preparing));

    removeTorrentAction->setEnabled(item != 0);
    pauseTorrentAction->setEnabled(item != 0 && pauseEnabled);

    if (client && client->state() == TorrentClient::Paused) {
        pauseTorrentAction->setIcon(QIcon(":/icons/player_play.png"));
        pauseTorrentAction->setText(tr("Resume torrent"));
    } else {
        pauseTorrentAction->setIcon(QIcon(":/icons/player_pause.png"));
        pauseTorrentAction->setText(tr("Pause torrent"));
    }

    int row = torrentView->indexOfTopLevelItem(item);
    upActionTool->setEnabled(item && row != 0);
    downActionTool->setEnabled(item && row != jobs.size() - 1);
}

void OverviewPage::updateDownloadRate(int bytesPerSecond)
{
    // Update the download rate.
    TorrentClient *client = qobject_cast<TorrentClient *>(sender());
    int row = rowOfClient(client);
    const QString num = QString::asprintf("%.1f KB/s", bytesPerSecond / 1024.0);
    torrentView->topLevelItem(row)->setText(3, num);

    if (!saveChanges) {
        saveChanges = true;
        QTimer::singleShot(5000, this, SLOT(saveSettings()));
    }
}

void OverviewPage::updateUploadRate(int bytesPerSecond)
{
    // Update the upload rate.
    TorrentClient *client = qobject_cast<TorrentClient *>(sender());
    int row = rowOfClient(client);
    const QString num = QString::asprintf("%.1f KB/s", bytesPerSecond / 1024.0);
    torrentView->topLevelItem(row)->setText(4, num);

    if (!saveChanges) {
        saveChanges = true;
        QTimer::singleShot(5000, this, SLOT(saveSettings()));
    }
}

void OverviewPage::pauseTorrent()
{
    // Pause or unpause the current torrent.
    int row = torrentView->indexOfTopLevelItem(torrentView->currentItem());
    TorrentClient *client = jobs.at(row).client;
    client->setPaused(client->state() != TorrentClient::Paused);
    setActionsEnabled();
}

void OverviewPage::moveTorrentUp()
{
    QTreeWidgetItem *item = torrentView->currentItem();
    int row = torrentView->indexOfTopLevelItem(item);
    if (row == 0)
        return;

    Job tmp = jobs.at(row - 1);
    jobs[row - 1] = jobs[row];
    jobs[row] = tmp;

    QTreeWidgetItem *itemAbove = torrentView->takeTopLevelItem(row - 1);
    torrentView->insertTopLevelItem(row, itemAbove);
    setActionsEnabled();
}

void OverviewPage::moveTorrentDown()
{
    QTreeWidgetItem *item = torrentView->currentItem();
    int row = torrentView->indexOfTopLevelItem(item);
    if (row == jobs.size() - 1)
        return;

    Job tmp = jobs.at(row + 1);
    jobs[row + 1] = jobs[row];
    jobs[row] = tmp;

    QTreeWidgetItem *itemAbove = torrentView->takeTopLevelItem(row + 1);
    torrentView->insertTopLevelItem(row, itemAbove);
    setActionsEnabled();
}

static int rateFromValue(int value)
{
    int rate = 0;
    if (value >= 0 && value < 250) {
        rate = 1 + int(value * 0.124);
    } else if (value < 500) {
        rate = 32 + int((value - 250) * 0.384);
    } else if (value < 750) {
        rate = 128 + int((value - 500) * 1.536);
    } else {
        rate = 512 + int((value - 750) * 6.1445);
    }
    return rate;
}

void OverviewPage::setUploadLimit(int value)
{
    int rate = rateFromValue(value);
    uploadLimitLabel->setText(tr("%1 KB/s").arg(QString::asprintf("%4d", rate)));
    RateController::instance()->setUploadLimit(rate * 1024);
}

void OverviewPage::setDownloadLimit(int value)
{
    int rate = rateFromValue(value);
    downloadLimitLabel->setText(tr("%1 KB/s").arg(QString::asprintf("%4d", rate)));
    RateController::instance()->setDownloadLimit(rate * 1024);
}

void OverviewPage::about()
{
    QLabel *icon = new QLabel;
    icon->setPixmap(QPixmap(":/icons/peertopeer.png"));

    QLabel *text = new QLabel;
    text->setWordWrap(true);
    text->setText("<p>The <b>Torrent Client</b> example demonstrates how to"
                  " write a complete peer-to-peer file sharing"
                  " application using Qt's network and thread classes.</p>"
                  "<p>This feature complete client implementation of"
                  " the BitTorrent protocol can efficiently"
                  " maintain several hundred network connections"
                  " simultaneously.</p>");

    QPushButton *quitButton = new QPushButton("OK");

    QHBoxLayout *topLayout = new QHBoxLayout;
    topLayout->setMargin(10);
    topLayout->setSpacing(10);
    topLayout->addWidget(icon);
    topLayout->addWidget(text);

    QHBoxLayout *bottomLayout = new QHBoxLayout;
    bottomLayout->addStretch();
    bottomLayout->addWidget(quitButton);
    bottomLayout->addStretch();

    QVBoxLayout *mainLayout = new QVBoxLayout;
    mainLayout->addLayout(topLayout);
    mainLayout->addLayout(bottomLayout);

    QDialog about(this);
    about.setModal(true);
    about.setWindowTitle(tr("About Torrent Client"));
    about.setLayout(mainLayout);

    connect(quitButton, SIGNAL(clicked()), &about, SLOT(close()));

    about.exec();
}