#include "settings-dialog.h"

#include "api-key-store.h"
#include "controller.h"
#include "core/keywords.h"
#include "core/start-check.h"
#include "core/summary-parser.h"
#include "plugin-settings.h"

#include <obs-module.h>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace rts {

namespace {

QString text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QLabel *error_label(QWidget *parent)
{
	auto *label = new QLabel(parent);
	label->setStyleSheet("color: #d9534f;");
	label->setWordWrap(true);
	label->hide();
	return label;
}

} // namespace

SettingsDialog::SettingsDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(text("Settings.Title"));
	setAttribute(Qt::WA_DeleteOnClose);

	// API キー
	auto *key_group = new QGroupBox(text("Settings.ApiKey"), this);
	status_label_ = new QLabel(key_group);
	key_edit_ = new QLineEdit(key_group);
	// 入力は伏せ字にし、保存済みのキーは表示しない
	key_edit_->setEchoMode(QLineEdit::Password);
	key_edit_->setMinimumWidth(320);
	save_button_ = new QPushButton(text("Settings.ApiKey.Save"), key_group);
	delete_button_ = new QPushButton(text("Settings.ApiKey.Delete"), key_group);
	auto *input_row = new QHBoxLayout();
	input_row->addWidget(key_edit_, 1);
	input_row->addWidget(save_button_);
	input_row->addWidget(delete_button_);
	key_message_label_ = error_label(key_group);
	auto *issue_label = new QLabel(text("Settings.ApiKey.Issue"), key_group);
	issue_label->setTextFormat(Qt::RichText);
	issue_label->setOpenExternalLinks(true);
	// プラグインは無料で、OpenAI API の利用料金は利用者の負担であることを示す
	auto *cost_label = new QLabel(text("Settings.ApiKey.Cost"), key_group);
	cost_label->setWordWrap(true);
	auto *key_layout = new QVBoxLayout(key_group);
	key_layout->addWidget(status_label_);
	key_layout->addLayout(input_row);
	key_layout->addWidget(key_message_label_);
	key_layout->addWidget(issue_label);
	key_layout->addWidget(cost_label);

	// 固有名詞のヒントと概要の値
	const GlobalSettings global = PluginSettings::instance().global();
	auto *options_group = new QGroupBox(text("Settings.Options"), this);
	keywords_edit_ = new QPlainTextEdit(options_group);
	keywords_edit_->setPlainText(QString::fromStdString(global.keywords_text));
	keywords_edit_->setMinimumHeight(90);
	keywords_message_label_ = error_label(options_group);
	max_chars_spin_ = new QSpinBox(options_group);
	max_chars_spin_->setRange(kMinSummaryMaxChars, kMaxSummaryMaxChars);
	max_chars_spin_->setValue(global.summary_max_chars);
	interval_spin_ = new QDoubleSpinBox(options_group);
	interval_spin_->setRange(0.5, 3600.0);
	interval_spin_->setDecimals(1);
	interval_spin_->setSuffix(text("Settings.Seconds"));
	interval_spin_->setValue(global.summary_interval_seconds);
	auto *options_layout = new QFormLayout(options_group);
	options_layout->addRow(text("Settings.Keywords"), keywords_edit_);
	auto *keywords_help = new QLabel(text("Settings.Keywords.Help"), options_group);
	keywords_help->setWordWrap(true);
	options_layout->addRow(QString(), keywords_help);
	options_layout->addRow(QString(), keywords_message_label_);
	options_layout->addRow(text("Settings.MaxChars"), max_chars_spin_);
	options_layout->addRow(text("Settings.Interval"), interval_spin_);
	placeholders_check_ = new QCheckBox(text("Settings.Placeholders"), options_group);
	placeholders_check_->setChecked(global.show_placeholders);
	options_layout->addRow(QString(), placeholders_check_);
	options_layout->addRow(QString(), new QLabel(text("Settings.AppliesNext"), options_group));

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	connect(buttons, &QDialogButtonBox::accepted, this, [this] { on_accept(); });
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	auto *layout = new QVBoxLayout(this);
	layout->addWidget(key_group);
	layout->addWidget(options_group);
	layout->addWidget(buttons);

	connect(save_button_, &QPushButton::clicked, this, [this] { on_save_key(); });
	connect(delete_button_, &QPushButton::clicked, this, [this] { on_delete_key(); });
	connect(key_edit_, &QLineEdit::returnPressed, this, [this] { on_save_key(); });

	refresh_status();
}

void SettingsDialog::refresh_status()
{
	if (!api_key_storage_supported()) {
		status_label_->setText(text("Settings.ApiKey.Unsupported"));
		key_edit_->setEnabled(false);
		save_button_->setEnabled(false);
		delete_button_->setEnabled(false);
		return;
	}

	std::string key;
	const ApiKeyStatus status = load_saved_api_key(key);
	if (status == ApiKeyStatus::Ok)
		status_label_->setText(text("Settings.ApiKey.Saved"));
	else if (status == ApiKeyStatus::DecryptFailed)
		status_label_->setText(text("Settings.ApiKey.DecryptFailed"));
	else
		status_label_->setText(text("Settings.ApiKey.NotSaved"));
	delete_button_->setEnabled(status == ApiKeyStatus::Ok || status == ApiKeyStatus::DecryptFailed);
}

void SettingsDialog::on_save_key()
{
	const ApiKeyStatus status = save_api_key(key_edit_->text().toStdString());
	if (status == ApiKeyStatus::InvalidKey) {
		show_key_error("Settings.ApiKey.Invalid");
		return;
	}
	if (status != ApiKeyStatus::Ok) {
		show_key_error("Settings.ApiKey.SaveFailed");
		return;
	}
	key_edit_->clear();
	key_message_label_->hide();
	refresh_status();
}

void SettingsDialog::on_delete_key()
{
	const ApiKeyStatus status = delete_saved_api_key();
	if (status != ApiKeyStatus::Ok && status != ApiKeyStatus::NotFound) {
		show_key_error("Settings.ApiKey.DeleteFailed");
		return;
	}
	key_message_label_->hide();
	refresh_status();
}

void SettingsDialog::on_accept()
{
	const std::string keywords = keywords_edit_->toPlainText().toStdString();
	const KeywordParseResult parsed = parse_keywords(keywords);
	if (!parsed.ok()) {
		QStringList lines;
		for (const int line : parsed.invalid_lines)
			lines << QString::number(line);
		keywords_message_label_->setText(text("Settings.Keywords.Invalid").arg(lines.join(", ")));
		keywords_message_label_->show();
		return;
	}

	GlobalSettings settings;
	settings.keywords_text = keywords;
	settings.summary_max_chars = max_chars_spin_->value();
	settings.summary_interval_seconds = interval_spin_->value();
	settings.show_placeholders = placeholders_check_->isChecked();
	PluginSettings::instance().set_global(settings);
	// 仮の文字列の表示の切り替えを、停止中ならすぐ表示先に反映する
	Controller::instance().show_idle_on_selected_targets();
	accept();
}

void SettingsDialog::show_key_error(const char *text_key)
{
	key_message_label_->setText(text(text_key));
	key_message_label_->show();
}

} // namespace rts
