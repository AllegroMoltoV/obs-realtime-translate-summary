#pragma once

#include <QWidget>

#include <array>

class QComboBox;
class QLabel;
class QPushButton;
class QTimer;

namespace rts {

// docs/design/user-interface.md のドック。開始と停止は Controller に任せる
class Dock : public QWidget {
public:
	explicit Dock(QWidget *parent = nullptr);
	~Dock() override;

	// ソースの追加・削除・名前の変更、シーンコレクションの切り替えのあとに一覧を作り直す。メインスレッドで呼ぶ
	void refresh_sources();

private:
	void on_toggle();
	void on_selection_changed();
	void update_status();

	QLabel *status_label_ = nullptr;
	QLabel *detail_label_ = nullptr;
	QComboBox *audio_combo_ = nullptr;
	std::array<QComboBox *, 3> target_combos_{};
	QPushButton *toggle_button_ = nullptr;
	QPushButton *settings_button_ = nullptr;
	QLabel *usage_label_ = nullptr;
	QTimer *timer_ = nullptr;
	bool refreshing_ = false;
};

} // namespace rts
