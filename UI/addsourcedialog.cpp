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

static std::string fixed_source_order[] = {
	"window_capture",   // windows + macos

	"game_capture",     // windows
	"syphon-input",     // macos

	"obs_browser",

	"dshow_input",      // windows
	"av_capture_input", // macos

	"image_source",
	"slideshow",
	"ffmpeg_source",

	"monitor_capture",  // windows
	"display_capture",  // macos
};

namespace {
	struct source_info {
		const char *id_;
		uint32_t caps;
		int8_t order;
		QString display_name;
		QImage image;
	};
}

struct SourceTypeModel : QAbstractListModel {
	std::vector<source_info> source_types;

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

			auto it = find(begin(fixed_source_order), end(fixed_source_order), id_);
			auto dist = static_cast<int8_t>(distance(begin(fixed_source_order), it));

			source_types.push_back({ id_, caps, dist, QString::fromUtf8(display_name), LoadSourceImage(source_icon_dir, id_) });
		}

		std::sort(begin(source_types), end(source_types), [](const source_info &a, const source_info &b)
		{
			if (a.order != b.order)
				return a.order < b.order;

			auto deprecated_a = (a.caps & OBS_SOURCE_DEPRECATED) != 0;
			auto deprecated_b = (b.caps & OBS_SOURCE_DEPRECATED) != 0;
			if (deprecated_a != deprecated_b)
				return deprecated_a;

			return a.display_name < b.display_name;
		});
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
		return static_cast<int>(source_types.size());
	}

	QVariant data(const QModelIndex &index, int role=Qt::DisplayRole) const override
	{
		if (role != Qt::DisplayRole && role != Qt::DecorationRole)
			return {};

		auto row = index.row();
		if (row < 0 || static_cast<size_t>(row) >= source_types.size())
			return {};

		auto &info = source_types[row];
		if (role == Qt::DecorationRole)
			return info.image;

		return info.display_name;
	}
};

Q_DECLARE_METATYPE(OBSWeakSource);

struct ExistingSourcesModel : QAbstractListModel {
	std::vector<OBSWeakSource> sources;
	std::string id_str;

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
		if (row < 0 || static_cast<size_t>(row) >= sources.size())
			return {};

		auto source = OBSGetStrongRef(sources[row]);
		if (!source)
			return {};

		return QString::fromUtf8(obs_source_get_name(source));
	}

	void SetSourceType(const char *id_)
	{
		beginResetModel();

		sources.clear();

		if (id_) {
			id_str = id_;
			auto enum_func = [&](obs_source_t *src)
			{
				auto id_ = obs_source_get_id(src);
				if (id_ && id_ == id_str)
					sources.push_back(OBSGetWeakRef(src));
			};
			using enum_func_t = decltype(enum_func);

			obs_enum_sources([](void *context, obs_source_t *src)
			{
				(*static_cast<enum_func_t*>(context))(src);
				return true;
			}, static_cast<void*>(&enum_func));

		} else {
			id_str.clear();
		}

		endResetModel();
	}

	decltype(cbegin(sources)) FindSource(OBSWeakSource src_) const
	{
		auto src = OBSGetStrongRef(src_);
		if (!src)
			return cend(sources);

		auto id_ = obs_source_get_id(src);
		if (!id_ || id_ != id_str)
			return cend(sources);

		return find_if(cbegin(sources), cend(sources), [&](OBSWeakSource source)
		{
			return obs_weak_source_references_source(source, src);
		});
	}

	void SourceDestroyed(OBSWeakSource src_)
	{
		auto it = FindSource(src_);
		if (it == end(sources))
			return;

		auto idx = static_cast<int>(std::distance(cbegin(sources), it));
		beginRemoveRows(createIndex(0, 0), idx, idx);
		sources.erase(it);
		endRemoveRows();
	}

	void SourceRenamed(OBSWeakSource src_)
	{
		auto it = FindSource(src_);
		if (it == end(sources))
			return;

		auto idx = static_cast<int>(std::distance(cbegin(sources), it));
		emit dataChanged(createIndex(idx, 0), createIndex(idx, 0));
	}

	void SourceCreated(OBSWeakSource src_)
	{
		auto it = FindSource(src_);
		if (it != end(sources))
			return;

		auto new_idx = static_cast<int>(sources.size());
		beginInsertRows(createIndex(0, 0), new_idx, new_idx);
		sources.push_back(src_);
		endInsertRows();
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


static AddSourceDialog *cast(void *ctx)
{
	return static_cast<AddSourceDialog*>(ctx);
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
		auto source_id = valid ? source_types->source_types[list.front().row()].id_ : nullptr;

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


	auto sig = obs_get_signal_handler();

	sourceDestroyed.Connect(sig, "source_destroy", [](void *ctx, calldata_t *data)
	{
		auto src = static_cast<obs_source_t*>(calldata_ptr(data, "source"));
		QMetaObject::invokeMethod(cast(ctx), "SourceDestroyed", Q_ARG(OBSWeakSource, OBSGetWeakRef(src)));
	}, this);
	sourceRenamed.Connect(sig, "source_rename", [](void *ctx, calldata_t *data)
	{
		auto src = static_cast<obs_source_t*>(calldata_ptr(data, "source"));
		QMetaObject::invokeMethod(cast(ctx), "SourceRenamed", Q_ARG(OBSWeakSource, OBSGetWeakRef(src)));
	}, this);
	sourceCreated.Connect(sig, "source_create", [](void *ctx, calldata_t *data)
	{
		auto src = static_cast<obs_source_t*>(calldata_ptr(data, "source"));
		QMetaObject::invokeMethod(cast(ctx), "SourceCreated", Q_ARG(OBSWeakSource, OBSGetWeakRef(src)));
	}, this);


	installEventFilter(CreateShortcutFilter());
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
	auto source_id = source_types->source_types[indexes.front().row()].id_;
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
	obs_source_t *source = OBSGetStrongRef(ex_sources->sources[indexes.front().row()]);
	if (!source)
		return;

	obs_enter_graphics();
	obs_scene_add(scene, source);
	obs_leave_graphics();

	obs_source_release(source);

	close();
}

void AddSourceDialog::SourceDestroyed(OBSWeakSource source)
{
	auto *ex_sources = static_cast<ExistingSourcesModel*>(existingSources.data());
	ex_sources->SourceRenamed(source);
}

void AddSourceDialog::SourceRenamed(OBSWeakSource source)
{
	auto *ex_sources = static_cast<ExistingSourcesModel*>(existingSources.data());
	ex_sources->SourceRenamed(source);
}

void AddSourceDialog::SourceCreated(OBSWeakSource source)
{
	auto *ex_sources = static_cast<ExistingSourcesModel*>(existingSources.data());
	ex_sources->SourceCreated(source);
}
