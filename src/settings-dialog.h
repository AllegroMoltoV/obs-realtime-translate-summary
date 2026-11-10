#pragma once

#include <QDialog>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

namespace rts {

// 設定画面。docs/design/user-interface.md の設定画面の仕様にしたがう。
// API キーの保存と削除はボタンを押した時点で反映し、ほかの欄は OK で保存して次の開始から反映する
class SettingsDialog : public QDialog {
public:
	explicit SettingsDialog(QWidget *parent = nullptr);

private:
	void refresh_status();
	void on_save_key();
	void on_delete_key();
	void on_accept();
	void show_key_error(const char *text_key);

	QLabel *status_label_ = nullptr;
	QLabel *key_message_label_ = nullptr;
	QLineEdit *key_edit_ = nullptr;
	QPushButton *save_button_ = nullptr;
	QPushButton *delete_button_ = nullptr;

	QPlainTextEdit *keywords_edit_ = nullptr;
	QLabel *keywords_message_label_ = nullptr;
	QSpinBox *max_chars_spin_ = nullptr;
	QDoubleSpinBox *interval_spin_ = nullptr;
	QCheckBox *placeholders_check_ = nullptr;
	QCheckBox *auto_stop_check_ = nullptr;
	QSpinBox *auto_stop_spin_ = nullptr;
};

} // namespace rts
