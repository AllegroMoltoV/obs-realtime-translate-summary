#pragma once

#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;

namespace rts {

// 設定画面。docs/design/user-interface.md の設定画面の仕様にしたがう。
// 段階 2 では API キーの欄だけを置き、固有名詞のヒントと概要の値の欄は段階 4 で足す
class SettingsDialog : public QDialog {
public:
	explicit SettingsDialog(QWidget *parent = nullptr);

private:
	void refresh_status();
	void on_save();
	void on_delete();
	void show_error(const char *text_key);

	QLabel *status_label_ = nullptr;
	QLabel *message_label_ = nullptr;
	QLineEdit *key_edit_ = nullptr;
	QPushButton *save_button_ = nullptr;
	QPushButton *delete_button_ = nullptr;
};

} // namespace rts
