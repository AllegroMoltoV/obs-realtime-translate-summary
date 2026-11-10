#include "settings-dialog.h"

#include "api-key-store.h"

#include <obs-module.h>

#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace rts {

namespace {

QString text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

} // namespace

SettingsDialog::SettingsDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(text("Settings.Title"));
	setAttribute(Qt::WA_DeleteOnClose);

	auto *group = new QGroupBox(text("Settings.ApiKey"), this);
	status_label_ = new QLabel(group);

	key_edit_ = new QLineEdit(group);
	// 入力は伏せ字にし、保存済みのキーは表示しない
	key_edit_->setEchoMode(QLineEdit::Password);
	key_edit_->setMinimumWidth(320);
	save_button_ = new QPushButton(text("Settings.ApiKey.Save"), group);
	delete_button_ = new QPushButton(text("Settings.ApiKey.Delete"), group);

	auto *input_row = new QHBoxLayout();
	input_row->addWidget(key_edit_, 1);
	input_row->addWidget(save_button_);
	input_row->addWidget(delete_button_);

	message_label_ = new QLabel(group);
	message_label_->setStyleSheet("color: #d9534f;");
	message_label_->hide();

	auto *issue_label = new QLabel(text("Settings.ApiKey.Issue"), group);
	issue_label->setTextFormat(Qt::RichText);
	issue_label->setOpenExternalLinks(true);

	auto *group_layout = new QVBoxLayout(group);
	group_layout->addWidget(status_label_);
	group_layout->addLayout(input_row);
	group_layout->addWidget(message_label_);
	group_layout->addWidget(issue_label);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	auto *layout = new QVBoxLayout(this);
	layout->addWidget(group);
	layout->addWidget(buttons);

	connect(save_button_, &QPushButton::clicked, this, [this] { on_save(); });
	connect(delete_button_, &QPushButton::clicked, this, [this] { on_delete(); });
	connect(key_edit_, &QLineEdit::returnPressed, this, [this] { on_save(); });

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

void SettingsDialog::on_save()
{
	const ApiKeyStatus status = save_api_key(key_edit_->text().toStdString());
	if (status == ApiKeyStatus::InvalidKey) {
		show_error("Settings.ApiKey.Invalid");
		return;
	}
	if (status != ApiKeyStatus::Ok) {
		show_error("Settings.ApiKey.SaveFailed");
		return;
	}
	key_edit_->clear();
	message_label_->hide();
	refresh_status();
}

void SettingsDialog::on_delete()
{
	const ApiKeyStatus status = delete_saved_api_key();
	if (status != ApiKeyStatus::Ok && status != ApiKeyStatus::NotFound) {
		show_error("Settings.ApiKey.DeleteFailed");
		return;
	}
	message_label_->hide();
	refresh_status();
}

void SettingsDialog::show_error(const char *text_key)
{
	message_label_->setText(text(text_key));
	message_label_->show();
}

} // namespace rts
