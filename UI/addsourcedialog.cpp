#include "addsourcedialog.hpp"
#include "ui_AddSourceDialog.h"

#include "obs-app.hpp"
#include "qt-wrappers.hpp"
#include "window-basic-main.hpp"

#include <qdir.h>
#include <qfileinfo.h>

#include <obs.hpp>
#include <util/dstr.hpp>

#include <algorithm>
#include <map>
#include <utility>
#include <vector>

struct SourceTypeModel : QAbstractListModel {
	std::vector<const char*> source_ids;
	std::map<const char*, std::pair<const char*, QImage>> source_info;

	QWidget *target = nullptr;

	SourceTypeModel(QWidget *target) : target(target)
	{
		QFileInfo info(App()->styleSheet().mid(8));
		QDir source_icon_dir = info.dir().filePath(info.baseName() + "/source_icons/");

		const char *id_ = nullptr;
		for(size_t i = 0; obs_enum_input_types(i, &id_); i++) {
			auto display_name = obs_source_get_display_name(id_);
			if (!display_name)
				continue;

			auto caps = obs_get_source_output_flags(id_);
			if ((caps & OBS_SOURCE_CAP_DISABLED) != 0)
				continue;

			source_ids.push_back(id_);

			source_info[id_] = std::make_pair(display_name,
					LoadSourceImage(source_icon_dir, id_));
		}
	}

	QImage LoadSourceImage(const QDir &dir, const char *source_id)
	{
		DStr source_filename;
		dstr_printf(source_filename, "%s.png", source_id);

		if (!dir.exists(source_filename->array))
			return{};

		blog(LOG_INFO, "loaded");
		return QImage(dir.filePath(source_filename->array));
	}

	int rowCount(const QModelIndex &/*parent*/=QModelIndex()) const override
	{
		return static_cast<int>(source_ids.size());
	}

	QVariant data(const QModelIndex &index, int role=Qt::DisplayRole) const override
	{
		if (role != Qt::DisplayRole && role != Qt::DecorationRole)
			return {};

		auto row = index.row();
		if (row < 0 || static_cast<size_t>(row) >= source_ids.size())
			return {};

		auto it = source_info.find(source_ids[row]);
		if (it == std::end(source_info))
			return {};

		if (role == Qt::DecorationRole)
			return it->second.second;

		return QString(it->second.first);
	}
};

struct ExistingSourcesModel : QAbstractListModel {
	std::vector<OBSSource> sources;

	QWidget *target = nullptr;

	ExistingSourcesModel(QWidget *target) : target(target)
	{
	}

	int rowCount(const QModelIndex &/*parent*/=QModelIndex()) const override
	{
		return static_cast<int>(sources.size());
	}

	QVariant data(const QModelIndex &index, int role=Qt::DisplayRole) const override
	{
		if (role != Qt::DisplayRole)
			return {};

		auto row = index.row();
		if (row < 0 || static_cast<size_t>(row) >= sources.size()) {
			return {};
		}

		auto &source = sources[row];
		return QString(obs_source_get_name(source));
	}

	void SetSourceType(const char *id_)
	{
		beginResetModel();

		sources.clear();

		if (id_) {
			std::string id_str = id_;
			auto enum_func = [&](obs_source_t *src)
			{
				auto id_ = obs_source_get_id(src);
				if (id_ && id_ == id_str)
					sources.push_back(src);
			};
			using enum_func_t = decltype(enum_func);

			obs_enum_sources([](void *context, obs_source_t *src)
			{
				(*static_cast<enum_func_t*>(context))(src);
				return true;
			}, static_cast<void*>(&enum_func));
		}

		endResetModel();
	}
};




static DStr get_new_source_name(const char *name)
{
	DStr new_name;
	int inc = 0;

	dstr_copy(new_name, name);

	for (;;) {
		obs_source_t *existing_source = obs_get_source_by_name(
				new_name);
		if (!existing_source)
			break;

		obs_source_release(existing_source);

		dstr_printf(new_name, "%s %d", name, ++inc + 1);
	}

	return new_name;
}


AddSourceDialog::AddSourceDialog(QWidget *parent) :
	QDialog(parent),
	ui(new Ui::AddSourceDialog),
	sourceTypes(new SourceTypeModel(ui->sourceTypes)),
	existingSources(new ExistingSourcesModel(ui->existingSources))
{
	ui->setupUi(this);

	ui->sourceTypes->setModel(sourceTypes);
	ui->existingSources->setModel(existingSources);

	auto *source_types = static_cast<SourceTypeModel*>(sourceTypes.data());
	auto *ex_sources = static_cast<ExistingSourcesModel*>(existingSources.data());

	connect(ui->sourceTypes->selectionModel(), &QItemSelectionModel::selectionChanged,
			[=](const QItemSelection &selected, const QItemSelection &/*deselected*/)
	{
		auto list = selected.indexes();
		auto valid = !list.empty();
		auto source_id = valid ? source_types->source_ids[list.front().row()] : nullptr;

		ex_sources->SetSourceType(source_id);
		ui->addNewSource->setEnabled(valid);
		ui->sourceNameLineEdit->setEnabled(valid);

		if (valid && ui->addExistingSource->isDefault())
			ui->addExistingSource->setDefault(false);
		ui->addNewSource->setDefault(valid);

		if (valid) {
			auto display_name = obs_source_get_display_name(source_id);
			ui->existingSourcesLabel->setText(QTStr("AddSource.SelectExisting.ExistingSources").arg(display_name));
			ui->addNewSource->setText(QTStr("AddSource.AddNewButton.AddType").arg(display_name));
			ui->sourceNameLineEdit->setText(get_new_source_name(obs_source_get_display_name(source_id))->array);
		} else {
			ui->existingSourcesLabel->setText(QTStr("AddSource.SelectExisting.SelectType"));
			ui->addNewSource->setText(QTStr("AddSource.AddNewButton.SelectType"));
			ui->addExistingSource->setText(QTStr("AddSource.AddExistingButton.SelectExisting"));
			ui->existingSourcesLabel->setText(QTStr("AddSource.SelectExisting.SelectType"));
		}
	});

	connect(ui->existingSources->selectionModel(), &QItemSelectionModel::selectionChanged,
			[=](const QItemSelection &selected, const QItemSelection &/*deselected*/)
	{
		auto list = selected.indexes();
		auto valid = !list.empty();

		ui->addExistingSource->setEnabled(valid);

		if (valid && ui->addNewSource->isDefault())
			ui->addNewSource->setDefault(false);
		ui->addExistingSource->setDefault(valid);

		if (valid)
			ui->addExistingSource->setText(QTStr("AddSource.AddExistingButton.AddExisting"));
		else
			ui->addExistingSource->setText(QTStr("AddSource.AddExistingButton.SelectExisting"));
	});

	connect(ui->sourceNameLineEdit, &QLineEdit::returnPressed,
			[=]
	{
		AddNewSource();
	});

	connect(ui->addNewSource, &QAbstractButton::clicked,
			[=](bool /*checked*/)
	{
		AddNewSource();
	});

	connect(ui->addExistingSource, &QAbstractButton::clicked,
			[=](bool /*checked*/)
	{
		AddExistingSource();
	});
}

AddSourceDialog::~AddSourceDialog()
{
	delete ui;
}

void AddSourceDialog::AddNewSource()
{
	OBSBasic *main = reinterpret_cast<OBSBasic*>(App()->GetMainWindow());
	OBSScene scene = main->GetCurrentScene();
	if (!scene)
		return;

	auto indexes = ui->sourceTypes->selectionModel()->selectedIndexes();
	if (indexes.empty())
		return;

	auto *source_types = static_cast<SourceTypeModel*>(sourceTypes.data());
	auto source_id = source_types->source_ids[indexes.front().row()];
	if (!source_id)
		return;

	auto name = ui->sourceNameLineEdit->text();

	OBSSource source = obs_get_source_by_name(QT_TO_UTF8(name));
	obs_source_release(source);
	if (source) {
		OBSMessageBox::information(main,
				QTStr("NameExists.Title"),
				QTStr("NameExists.Text"));
		return;
	}

	source = obs_source_create(source_id, QT_TO_UTF8(name), NULL, nullptr);
	if (!source)
		return;

	obs_source_release(source);

	obs_enter_graphics();
	obs_scene_add(scene, source);
	obs_leave_graphics();

	emit newSourceCreated(source);

	close();
}

void AddSourceDialog::AddExistingSource()
{
	OBSBasic *main = reinterpret_cast<OBSBasic*>(App()->GetMainWindow());
	OBSScene scene = main->GetCurrentScene();
	if (!scene)
		return;

	auto indexes = ui->existingSources->selectionModel()->selectedIndexes();
	if (indexes.empty())
		return;

	auto *ex_sources = static_cast<ExistingSourcesModel*>(existingSources.data());
	obs_source_t *source = ex_sources->sources[indexes.front().row()];
	if (!source)
		return;

	obs_enter_graphics();
	obs_scene_add(scene, source);
	obs_leave_graphics();

	obs_source_release(source);

	close();
}
