#include "controller.h"

#include "api-key-store.h"
#include "core/keywords.h"
#include "plugin-settings.h"
#include "text-output.h"

#include <obs-module.h>
#include <obs.h>
#include <plugin-support.h>

namespace rts {

namespace {

bool source_exists(const std::string &name, bool *is_text = nullptr, bool *has_audio = nullptr)
{
	if (name.empty())
		return false;
	obs_source_t *source = obs_get_source_by_name(name.c_str());
	if (!source)
		return false;
	if (is_text) {
		const char *id = obs_source_get_unversioned_id(source);
		*is_text = id && is_supported_text_source_id(id);
	}
	if (has_audio)
		*has_audio = (obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) != 0;
	obs_source_release(source);
	return true;
}

} // namespace

Controller &Controller::instance()
{
	static Controller controller;
	return controller;
}

StartInputs Controller::collect_start_inputs() const
{
	const SourceSelection selection = PluginSettings::instance().selection();
	StartInputs inputs;
	inputs.api_key_saved = api_key_saved();
	inputs.audio_source = selection.audio_source;
	bool has_audio = false;
	inputs.audio_source_exists = source_exists(selection.audio_source, nullptr, &has_audio) && has_audio;
	for (size_t i = 0; i < 3; ++i) {
		inputs.targets[i].name = selection.targets[i];
		inputs.targets[i].exists = source_exists(selection.targets[i], &inputs.targets[i].is_text);
	}
	return inputs;
}

bool Controller::start()
{
	if (active_)
		return true;

	source_removed_ = false;
	last_status_ = Status();
	problems_ = check_start(collect_start_inputs());
	if (!problems_.empty())
		return false;

	SessionConfig config;
	if (load_saved_api_key(config.api_key) != ApiKeyStatus::Ok) {
		problems_.push_back(StartProblem::ApiKeyMissing);
		return false;
	}
	const SourceSelection selection = PluginSettings::instance().selection();
	const GlobalSettings global = PluginSettings::instance().global();
	config.audio_source = selection.audio_source;
	config.keywords = parse_keywords(global.keywords_text).keywords;
	config.summary_max_chars = global.summary_max_chars;
	config.summary_interval_seconds = global.summary_interval_seconds;

	active_targets_ = selection.targets;
	active_audio_ = selection.audio_source;
	write_targets(active_targets_, false);

	const uint64_t generation = ++generation_;
	const auto targets = active_targets_;
	active_ = true;
	session_.start(config, [this, generation, targets](RealtimeSession::Output output, const std::string &text) {
		// 作業のスレッドから呼ばれる。表示の更新はメインスレッドで行い、停止の後に届いた古い世代は捨てる
		run_on_main_thread([this, generation, targets, output, text] {
			if (!active_ || generation != generation_)
				return;
			set_source_text(targets[output], text);
		});
	});
	obs_log(LOG_INFO, "controller started");
	return true;
}

void Controller::stop()
{
	if (!active_)
		return;
	// セッションを止めてから、動作中として最後の状態を写す。メインスレッドで呼ぶため、この間に表示の更新は割り込まない
	session_.stop();
	last_status_ = status();
	active_ = false;
	++generation_;
	write_targets(active_targets_, true);
	obs_log(LOG_INFO, "controller stopped");
}

void Controller::poll()
{
	if (!active_)
		return;
	if (session_.state() == SessionState::Error) {
		stop();
		return;
	}
	const GlobalSettings global = PluginSettings::instance().global();
	if (auto_stop_due(global.auto_stop_enabled, global.auto_stop_minutes, session_.elapsed_seconds())) {
		obs_log(LOG_INFO, "controller auto stop minutes=%d", global.auto_stop_minutes);
		stop();
		last_status_.auto_stopped = true;
		last_status_.auto_stop_minutes = global.auto_stop_minutes;
	}
}

void Controller::on_source_removed(const std::string &name)
{
	if (!active_ || name.empty())
		return;
	bool used = name == active_audio_;
	for (const auto &target : active_targets_)
		used = used || name == target;
	if (!used)
		return;
	source_removed_ = true;
	stop();
	last_status_.source_removed = true;
}

void Controller::show_idle_on_selected_targets()
{
	if (!active_)
		write_targets(PluginSettings::instance().selection().targets, true);
}

void Controller::clear_target(const std::string &name)
{
	bool is_text = false;
	if (!active_ && source_exists(name, &is_text) && is_text)
		set_source_text(name, "");
}

// idle が真なら停止中の表示 (設定により仮の文字列か空)、偽なら空を書く。字幕の本文は残さない
void Controller::write_targets(const std::array<std::string, 3> &targets, bool idle)
{
	static constexpr const char *kPlaceholderKeys[3] = {"Placeholder.Japanese", "Placeholder.English",
							    "Placeholder.Summary"};
	const bool placeholders = idle && PluginSettings::instance().global().show_placeholders;
	for (size_t i = 0; i < targets.size(); ++i) {
		bool is_text = false;
		if (source_exists(targets[i], &is_text) && is_text)
			set_source_text(targets[i], placeholders ? obs_module_text(kPlaceholderKeys[i]) : "");
	}
}

Controller::Status Controller::status() const
{
	if (!active_) {
		Status stopped = last_status_;
		stopped.problems = problems_;
		if (stopped.state != SessionState::Error)
			stopped.state = SessionState::Stopped;
		stopped.muted = false;
		return stopped;
	}
	Status s;
	s.state = session_.state();
	s.error = session_.error();
	s.source_removed = source_removed_;
	s.elapsed_seconds = session_.elapsed_seconds();
	s.sent_audio_seconds = session_.sent_audio_seconds();
	s.input_tokens = session_.input_tokens();
	s.output_tokens = session_.output_tokens();
	s.muted = session_.muted();
	s.translate_failing = session_.translate_failing();
	s.summary_failing = session_.summary_failing();
	s.retry_attempt = session_.retry_attempt();
	s.max_retries = session_.max_retries();
	return s;
}

} // namespace rts
