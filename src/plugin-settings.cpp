#include "plugin-settings.h"

#include "core/start-check.h"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

namespace rts {

namespace {

constexpr const char *kTargetKeys[3] = {"japanese_target", "english_target", "summary_target"};

} // namespace

PluginSettings &PluginSettings::instance()
{
	static PluginSettings settings;
	return settings;
}

SourceSelection PluginSettings::selection() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return selection_;
}

void PluginSettings::set_selection(const SourceSelection &selection)
{
	std::lock_guard<std::mutex> lock(mutex_);
	selection_ = selection;
}

GlobalSettings PluginSettings::global() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return global_;
}

void PluginSettings::set_global(const GlobalSettings &settings)
{
	std::lock_guard<std::mutex> lock(mutex_);
	global_ = settings;
	save_global_locked();
}

void PluginSettings::load_global()
{
	char *path = obs_module_config_path("settings.json");
	if (!path)
		return;
	obs_data_t *data = obs_data_create_from_json_file(path);
	bfree(path);
	if (!data)
		return;

	GlobalSettings loaded;
	loaded.keywords_text = obs_data_get_string(data, "keywords");
	if (obs_data_has_user_value(data, "summary_max_chars")) {
		const int value = static_cast<int>(obs_data_get_int(data, "summary_max_chars"));
		if (valid_summary_max_chars(value))
			loaded.summary_max_chars = value;
	}
	if (obs_data_has_user_value(data, "summary_interval_seconds")) {
		const double value = obs_data_get_double(data, "summary_interval_seconds");
		if (valid_summary_interval(value))
			loaded.summary_interval_seconds = value;
	}
	if (obs_data_has_user_value(data, "show_placeholders"))
		loaded.show_placeholders = obs_data_get_bool(data, "show_placeholders");
	obs_data_release(data);

	std::lock_guard<std::mutex> lock(mutex_);
	global_ = loaded;
}

void PluginSettings::save_global_locked() const
{
	char *dir = obs_module_config_path("");
	if (!dir)
		return;
	os_mkdirs(dir);
	bfree(dir);

	char *path = obs_module_config_path("settings.json");
	if (!path)
		return;
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "keywords", global_.keywords_text.c_str());
	obs_data_set_int(data, "summary_max_chars", global_.summary_max_chars);
	obs_data_set_double(data, "summary_interval_seconds", global_.summary_interval_seconds);
	obs_data_set_bool(data, "show_placeholders", global_.show_placeholders);
	if (!obs_data_save_json_safe(data, path, "tmp", "bak"))
		obs_log(LOG_WARNING, "settings.json save failed");
	obs_data_release(data);
	bfree(path);
}

void PluginSettings::save_selection_to(obs_data_t *save_data) const
{
	const SourceSelection current = selection();
	obs_data_t *obj = obs_data_create();
	obs_data_set_string(obj, "audio_source", current.audio_source.c_str());
	for (size_t i = 0; i < 3; ++i)
		obs_data_set_string(obj, kTargetKeys[i], current.targets[i].c_str());
	obs_data_set_obj(save_data, kSaveKey, obj);
	obs_data_release(obj);
}

void PluginSettings::load_selection_from(obs_data_t *save_data)
{
	SourceSelection loaded;
	obs_data_t *obj = obs_data_get_obj(save_data, kSaveKey);
	if (obj) {
		loaded.audio_source = obs_data_get_string(obj, "audio_source");
		for (size_t i = 0; i < 3; ++i)
			loaded.targets[i] = obs_data_get_string(obj, kTargetKeys[i]);
		obs_data_release(obj);
	}
	set_selection(loaded);
}

} // namespace rts
