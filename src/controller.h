#pragma once

#include "core/start-check.h"
#include "realtime-session.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace rts {

// ドック、ホットキー、OBS のイベントから使う、開始と停止の制御。すべての関数を Qt のメインスレッドで呼ぶ
class Controller {
public:
	static Controller &instance();

	// 開始の条件を満たさない場合は false を返し、problems() に理由を残す
	bool start();
	// 停止して、選んでいた表示先の text を空にする
	void stop();
	bool active() const { return active_; }

	// 動作中に通信の失敗などで止まったら、停止して表示を空にする。ドックのタイマーから呼ぶ
	void poll();

	// ソースの削除と名前の変更。動作中に使っているソースが消えたら止める
	void on_source_removed(const std::string &name);

	// 選択の読み込みの後に、異常終了で残った本文を消す
	void clear_selected_targets();

	struct Status {
		SessionState state = SessionState::Stopped;
		SessionError error = SessionError::None;
		bool source_removed = false;
		std::vector<StartProblem> problems;
		double elapsed_seconds = 0.0;
		double sent_audio_seconds = 0.0;
		long long input_tokens = 0;
		long long output_tokens = 0;
		bool muted = false;
		int text_failures = 0;
	};
	Status status() const;

private:
	Controller() = default;
	StartInputs collect_start_inputs() const;
	void clear_targets(const std::array<std::string, 3> &targets);

	RealtimeSession session_;
	bool active_ = false;
	uint64_t generation_ = 0;
	std::array<std::string, 3> active_targets_;
	std::string active_audio_;
	std::vector<StartProblem> problems_;
	bool source_removed_ = false;
	// 停止した後もドックに最後の利用量と原因を出すため、停止の時点の値を残す
	Status last_status_;
};

} // namespace rts
