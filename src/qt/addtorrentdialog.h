#ifndef ADDTORRENTDIALOG_H
#define ADDTORRENTDIALOG_H

#include <QDialog>

#include "ui_addtorrentform.h"

class AddTorrentDialog : public QDialog
{
    Q_OBJECT

public:
    AddTorrentDialog(QWidget *parent = 0);

    QString torrentFileName() const;
    QString destinationFolder() const;

public slots:
    void setTorrent(const QString &torrentFile);

private slots:
    void selectTorrent();
    void selectDestination();
    void enableOkButton();

private:
    Ui_AddTorrentFile ui;
    QString destinationDirectory;
    QString lastDirectory;
    QString lastDestinationDirectory;
};

#endif
