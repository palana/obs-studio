#ifndef ADDSOURCEDIALOG_H
#define ADDSOURCEDIALOG_H

#include <QDialog>
#include <QPointer>
#include <QAbstractListModel>

#include <obs.hpp>

#include <memory>

namespace Ui {
class AddSourceDialog;
}

class AddSourceDialog : public QDialog
{
	Q_OBJECT

public:
	explicit AddSourceDialog(QWidget *parent=nullptr);
	~AddSourceDialog();

signals:
	void newSourceCreated(OBSSource source);

private:
	Ui::AddSourceDialog *ui;
	std::unique_ptr<QAbstractListModel> sourceTypes;
	std::unique_ptr<QAbstractListModel> existingSources;

	OBSSignal sourceDestroyed;
	OBSSignal sourceRenamed;
	OBSSignal sourceCreated;

	void AddNewSource();
	void AddExistingSource();

private slots:
	void SourceDestroyed(OBSWeakSource source);
	void SourceRenamed(OBSWeakSource source);
	void SourceCreated(OBSWeakSource source);
};

#endif // ADDSOURCEDIALOG_H
