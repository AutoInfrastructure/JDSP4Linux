#include "AeqPackageManager.h"
#include "AeqSelector.h"
#include "HttpException.h"
#include "ui_AeqSelector.h"

#include "AeqListDelegates.h"
#include "AeqMeasurementItem.h"
#include "AeqMeasurementModel.h"

#include <QListWidget>
#include <QMessageBox>
#include <QBitmap>
#include <QSortFilterProxyModel>
#include <QtConcurrent/QtConcurrent>

AeqSelector::AeqSelector(QWidget *parent) :
	QDialog(parent),
    ui(new Ui::AeqSelector),
    pkgManager(new AeqPackageManager(this)),
    model(new AeqMeasurementModel(this)),
    proxyModel(new QSortFilterProxyModel(this))
{
    ui->setupUi(this);

	ui->buttonBox->button(QDialogButtonBox::StandardButton::Ok)->setEnabled(false);
    ui->stackedWidget->setCurrentIndex(0);
    ui->previewStack->setCurrentIndex(0);

    proxyModel->setFilterCaseSensitivity(Qt::CaseInsensitive);
    proxyModel->setDynamicSortFilter(false);
    proxyModel->setSourceModel(model);

    ui->list->setEmptyViewEnabled(true);
    ui->list->setEmptyViewTitle("No measurements found");
    ui->list->setModel(proxyModel);
    ui->list->setItemDelegate(new AeqItemDelegate(ui->list));

    connect(ui->searchInput,    &QLineEdit::textChanged, proxyModel, &QSortFilterProxyModel::setFilterWildcard);
    connect(ui->manageDatabase, &QPushButton::clicked,  this,        &AeqSelector::switchPane);
    connect(ui->updateButton,   &QPushButton::clicked,  this,        &AeqSelector::updateDatabase);
    connect(ui->deleteButton,   &QPushButton::clicked,  this,        &AeqSelector::deleteDatabase);

    connect(ui->list->selectionModel(), &QItemSelectionModel::selectionChanged, this, &AeqSelector::onSelectionChanged);

    updateDatabaseInfo();
}

AeqSelector::~AeqSelector()
{
    delete ui;
}

void AeqSelector::showEvent(QShowEvent *ev)
{
    if(!pkgManager->isPackageInstalled())
    {
        this->setEnabled(false);
        auto res = QMessageBox::question(this, "AutoEQ database",
                      "Before using the AutoEQ integration, you need to download a minified version of their headphone compensation database (~50MB) to your hard drive.\n"
                      "An internet connection is required during this step.\n"
                      "Do you want to continue and enable this feature?",
                      QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Ok);

        if(res == QMessageBox::Cancel)
        {
            this->reject();
            return;
        }

        pkgManager->getRepositoryVersion().then([=](AeqVersion remote)
        {
            pkgManager->installPackage(remote, this).then([=]()
            {
                this->setEnabled(true);
                updateDatabaseInfo();
            }).fail([this](){
                this->reject();
                return;
            });

        }).fail([this](const HttpException& ex){
            QMessageBox::critical(this, "Failed to retrieve version information", QString("Failed to retrieve package information from the remote repository:\n\n"
                                                                                          "Status code: %0\nReason: %1").arg(ex.statusCode()).arg(ex.reasonPhrase()));
            this->reject();
            return;
        });
    }


    QDialog::showEvent(ev);
}

void AeqSelector::switchPane()
{
    ui->manageDatabase->setText(!ui->stackedWidget->currentIndex() ? "Return to database" : "Manage database");
    ui->stackedWidget->setCurrentIndex(!ui->stackedWidget->currentIndex());
}

void AeqSelector::updateDatabase()
{
    this->setEnabled(false);

    auto doInstallation = [this](AeqVersion remote){
        pkgManager->installPackage(remote, this).then([this]()
        {
            updateDatabaseInfo();
        });
    };


    pkgManager->isUpdateAvailable().then([&](AeqVersion remote)
    {
        doInstallation(remote);

    }).fail([this](const HttpException& ex){
        QMessageBox::critical(this, "Failed to retrieve version information", QString("Failed to retrieve package information from the remote repository:\n\n"
                                                                                      "Status code: %0\nReason: %1").arg(ex.statusCode()).arg(ex.reasonPhrase()));
    }).fail([this, doInstallation](const AeqVersion& remote){
        auto button = QMessageBox::question(this, "No new updates available", QString("The local database is currently up-to-date; no new updates are available at this time.\n\n"
                                                                                      "It may take up to 24 hours for new changes in the AutoEQ upstream repo to become available for download here. "
                                                                                      "Packages are generated at 4am UTC daily.\n\n"
                                                                                      "Do you want to re-install the latest database update anyway?"));

        if(button == QMessageBox::Yes)
        {
            doInstallation(remote);
        }
    }).finally([this]{
        this->setEnabled(true);
    });
}

void AeqSelector::deleteDatabase()
{
    pkgManager->uninstallPackage();
    QMessageBox::information(this, "Database cleared", "The database has been removed from your hard disk");
    reject();
}

void AeqSelector::updateDatabaseInfo()
{
    ui->searchInput->setText("");
    ui->list->selectionModel()->clearSelection();

    pkgManager->getLocalVersion().then([=](AeqVersion local){
        ui->db_commit->setText(local.commit.left(7));
        ui->db_committime->setText(local.commitTime.toString("yy/MM/dd HH:mm:ss") + " UTC");
        ui->db_uploadtime->setText(local.packageTime.toString("yy/MM/dd HH:mm:ss") + " UTC");
    }).fail([=]{
        ui->db_commit->setText("N.A.");
        ui->db_committime->setText("N.A.");
        ui->db_uploadtime->setText("N.A.");
    });

    pkgManager->getLocalIndex().then([=](const QVector<AeqMeasurement>& items){
        model->import(items);
    });
}

void AeqSelector::onSelectionChanged(const QItemSelection &selected, const QItemSelection &deselected)
{
    Q_UNUSED(deselected)
    ui->previewStack->setCurrentIndex(!selected.isEmpty());
    ui->buttonBox->button(QDialogButtonBox::StandardButton::Ok)->setEnabled(!selected.isEmpty());

    if(selected.isEmpty())
    {
        return;
    }
}

QString AeqSelector::selection(DataFormat format)
{
    if (ui->list->selectionModel()->selectedRows().isEmpty())
	{
        return QString();
	}

    auto item = proxyModel->data(ui->list->selectionModel()->selectedRows().first(), Qt::UserRole).value<AeqMeasurement>();

    QString name;
    switch(format)
    {
    case AeqSelector::dGraphicEq:
        name = "graphic.txt";
        break;
    case AeqSelector::dCsv:
        name = "raw.csv";
        break;
    }

    QFile file(item.path(pkgManager->databaseDirectory(), name));
    if(!file.exists())
    {
        QMessageBox::critical(this, "Error", "Unable to retrieve corresponding file from database. Please update the local database as it appears to be incomplete.");
        return QString();
    }

    auto guard = qScopeGuard([&]{ file.close(); });
    return file.readAll();
}