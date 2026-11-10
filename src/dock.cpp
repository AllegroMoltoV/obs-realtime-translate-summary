#include "dock.h"

#include "controller.h"
#include "core/start-check.h"
#include "plugin-settings.h"
#include "settings-dialog.h"

#include <obs-module.h>
#include <obs.h>

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <string>
#include <vector>

namespace rts {

namespace {

QString text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

struct SourceInfo {
	std::string name;
	bool audio = false;
	bool text = false;
};

std::vector<SourceInfo> list_sources()
{
	std::vector<SourceInfo> sources;
	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			auto *out = static_cast<std::vector<SourceInfo> *>(param);
			SourceInfo info;
			const char *name = obs_source_get_name(source);
			info.name = name ? name : "";
			info.audio = (obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) != 0;
			const char *id = obs_source_get_unversioned_id(source);
			info.text = id && is_supported_text_source_id(id);
			if (!info.name.empty())
				out->push_back(info);
			return true;
		},
		&sources);
	return sources;
}

// 一覧を作り直す。選択中のソースが無い場合は「(見つかりません)」を付けて残す
void fill_combo(QComboBox *combo, const std::vector<std::string> &names, const std::string &selected, bool allow_unused)
{
	combo->clear();
	if (allow_unused)
		combo->addItem(text("Dock.Unused"), QString());
	bool found = selected.empty();
	for (const auto &name : names) {
		combo->addItem(QString::fromStdString(name), QString::fromStdString(name));
		found = found || name == selected;
	}
	if (!found)
		combo->addItem(text("Dock.Missing") + " " + QString::fromStdString(selected),
			       QString::fromStdString(selected));
	const int index = combo->findData(QString::fromStdString(selected));
	combo->setCurrentIndex(index >= 0 ? index : 0);
}

QString format_duration(double seconds)
{
	const int total = static_cast<int>(seconds);
	return QString("%1:%2").arg(total / 60).arg(total % 60, 2, 10, QChar('0'));
}

const char *problem_key(StartProblem problem)
{
	switch (problem) {
	case StartProblem::ApiKeyMissing:
		return "Error.ApiKeyMissing";
	case StartProblem::AudioSourceNotSelected:
		return "Error.AudioSourceNotSelected";
	case StartProblem::AudioSourceMissing:
		return "Error.AudioSourceMissing";
	case StartProblem::NoTargetSelected:
		return "Error.NoTargetSelected";
	case StartProblem::TargetMissing:
		return "Error.TargetMissing";
	case StartProblem::TargetNotText:
		return "Error.TargetNotText";
	case StartProblem::DuplicateTarget:
		return "Error.DuplicateTarget";
	}
	return "Error.Internal";
}

const char *session_error_key(SessionError error)
{
	switch (error) {
	case SessionError::ConnectFailed:
		return "Error.ConnectFailed";
	case SessionError::AuthFailed:
		return "Error.AuthFailed";
	case SessionError::SessionRejected:
		return "Error.SessionRejected";
	case SessionError::AudioSourceMissing:
		return "Error.AudioSourceMissing";
	case SessionError::ConnectionLost:
		return "Error.ConnectionLost";
	case SessionError::Internal:
	case SessionError::None:
		break;
	}
	return "Error.Internal";
}

} // namespace

Dock::Dock(QWidget *parent) : QWidget(parent)
{
	status_label_ = new QLabel(this);
	detail_label_ = new QLabel(this);
	detail_label_->setWordWrap(true);
	detail_label_->hide();

	audio_combo_ = new QComboBox(this);
	for (auto &combo : target_combos_)
		combo = new QComboBox(this);

	auto *form = new QFormLayout();
	form->addRow(text("Dock.Audio"), audio_combo_);
	form->addRow(text("Dock.Japanese"), target_combos_[0]);
	form->addRow(text("Dock.English"), target_combos_[1]);
	form->addRow(text("Dock.Summary"), target_combos_[2]);

	toggle_button_ = new QPushButton(text("Dock.Start"), this);
	settings_button_ = new QPushButton(text("Dock.Settings"), this);
	auto *buttons = new QHBoxLayout();
	buttons->addWidget(toggle_button_, 1);
	buttons->addWidget(settings_button_);

	usage_label_ = new QLabel(this);
	usage_label_->setWordWrap(true);

	auto *layout = new QVBoxLayout(this);
	layout->addWidget(status_label_);
	layout->addWidget(detail_label_);
	layout->addLayout(form);
	layout->addLayout(buttons);
	layout->addWidget(usage_label_);
	layout->addStretch(1);

	connect(toggle_button_, &QPushButton::clicked, this, [this] { on_toggle(); });
	connect(settings_button_, &QPushButton::clicked, this, [this] {
		auto *dialog = new SettingsDialog(window());
		dialog->show();
		dialog->raise();
		dialog->activateWindow();
	});
	connect(audio_combo_, &QComboBox::currentIndexChanged, this, [this] { on_selection_changed(); });
	for (auto *combo : target_combos_)
		connect(combo, &QComboBox::currentIndexChanged, this, [this] { on_selection_changed(); });

	timer_ = new QTimer(this);
	connect(timer_, &QTimer::timeout, this, [this] {
		Controller::instance().poll();
		update_status();
	});
	timer_->start(500);

	refresh_sources();
	update_status();
}

Dock::~Dock() = default;

void Dock::refresh_sources()
{
	refreshing_ = true;
	const auto sources = list_sources();
	std::vector<std::string> audio_names;
	std::vector<std::string> text_names;
	for (const auto &source : sources) {
		if (source.audio)
			audio_names.push_back(source.name);
		if (source.text)
			text_names.push_back(source.name);
	}
	const SourceSelection selection = PluginSettings::instance().selection();
	fill_combo(audio_combo_, audio_names, selection.audio_source, false);
	if (selection.audio_source.empty())
		audio_combo_->setCurrentIndex(-1);
	for (size_t i = 0; i < 3; ++i)
		fill_combo(target_combos_[i], text_names, selection.targets[i], true);
	refreshing_ = false;
	update_status();
}

void Dock::on_selection_changed()
{
	if (refreshing_)
		return;
	SourceSelection selection;
	selection.audio_source = audio_combo_->currentData().toString().toStdString();
	for (size_t i = 0; i < 3; ++i)
		selection.targets[i] = target_combos_[i]->currentData().toString().toStdString();
	PluginSettings::instance().set_selection(selection);
}

void Dock::on_toggle()
{
	auto &controller = Controller::instance();
	if (controller.active())
		controller.stop();
	else
		controller.start();
	update_status();
}

void Dock::update_status()
{
	const auto &controller = Controller::instance();
	const auto status = controller.status();
	const bool active = controller.active();

	audio_combo_->setEnabled(!active);
	for (auto *combo : target_combos_)
		combo->setEnabled(!active);
	toggle_button_->setText(active ? text("Dock.Stop") : text("Dock.Start"));

	QString state;
	QStringList details;
	switch (status.state) {
	case SessionState::Stopped:
		state = text("Dock.State.Stopped");
		break;
	case SessionState::Connecting:
		state = text("Dock.State.Connecting");
		break;
	case SessionState::Running:
		state = text("Dock.State.Running") + "  " + format_duration(status.elapsed_seconds);
		break;
	case SessionState::Error:
		state = text("Dock.State.Error");
		details << text(session_error_key(status.error));
		break;
	}
	if (status.source_removed)
		details << text("Error.SourceRemoved");
	for (const auto problem : status.problems)
		details << text(problem_key(problem));
	if (active && status.muted)
		details << text("Dock.Muted");
	if (active && status.text_failures > 0)
		details << text("Dock.TextFailures").arg(status.text_failures);

	status_label_->setText(text("Dock.Status") + " " + state);
	detail_label_->setText(details.join("\n"));
	detail_label_->setVisible(!details.isEmpty());

	usage_label_->setText(text("Dock.Usage")
				      .arg(format_duration(status.sent_audio_seconds))
				      .arg(status.input_tokens)
				      .arg(status.output_tokens));
}

} // namespace rts
