#ifndef ADDSOURCEDIALOG_H
#define ADDSOURCEDIALOG_H

#include <QDialog>
#include <QPointer>
#include <QAbstractListModel>

#include <obs.hpp>

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
	QPointer<QAbstractListModel> sourceTypes;
	QPointer<QAbstractListModel> existingSources;

	void AddNewSource();
	void AddExistingSource();
};

#endif // ADDSOURCEDIALOG_H
