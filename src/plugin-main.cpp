/*
Realtime Translate & Summary
Copyright (C) 2026 AllegroMoltoV

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "controller.h"
#include "dock.h"
#include "plugin-settings.h"
#include "text-output.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/config-file.h>

#include <QEvent>
#include <QMainWindow>
#include <QPointer>

#include <curl/curl.h>

#include <string>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {

constexpr const char *kDockId = "realtime-translate-summary-dock";

QPointer<rts::Dock> g_dock;
obs_hotkey_id g_hotkey_start = OBS_INVALID_HOTKEY_ID;
obs_hotkey_id g_hotkey_stop = OBS_INVALID_HOTKEY_ID;
bool g_refresh_pending = false;

// メインウィンドウの close の要求を、OBS の closeEvent より先に受け取る。
// OBS は終了の通知 (EXIT) より前にシーンコレクションを保存するため、ここで停止して表示を空にしておく
class CloseFilter : public QObject {
public:
	using QObject::QObject;

protected:
	bool eventFilter(QObject *watched, QEvent *event) override
	{
		if (event->type() == QEvent::Close)
			rts::Controller::instance().stop();
		return QObject::eventFilter(watched, event);
	}
};

// 実行時に読み込まれた libcurl の版と、WSS と HTTPS の対応をログに残す
void log_curl_runtime()
{
	const curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);
	if (!info) {
		obs_log(LOG_WARNING, "curl_version_info returned null");
		return;
	}
	bool has_https = false;
	bool has_wss = false;
	for (const char *const *p = info->protocols; p && *p; ++p) {
		const std::string name = *p;
		has_https = has_https || name == "https";
		has_wss = has_wss || name == "wss";
	}
	obs_log(LOG_INFO, "libcurl runtime version=%s ssl=%s https=%d wss=%d", info->version,
		info->ssl_version ? info->ssl_version : "none", has_https ? 1 : 0, has_wss ? 1 : 0);
}

// ソースの通知は呼び出したスレッドで届くため、ドックの更新はメインスレッドへ移し、続けて届いた分はまとめる
void request_dock_refresh()
{
	rts::run_on_main_thread([] {
		if (g_refresh_pending)
			return;
		g_refresh_pending = true;
		rts::post_to_main_thread([] {
			g_refresh_pending = false;
			if (g_dock)
				g_dock->refresh_sources();
		});
	});
}

void on_source_created(void *, calldata_t *)
{
	request_dock_refresh();
}

void on_source_removed(void *, calldata_t *data)
{
	obs_source_t *source = static_cast<obs_source_t *>(calldata_ptr(data, "source"));
	const char *name = source ? obs_source_get_name(source) : nullptr;
	const std::string removed = name ? name : "";
	rts::run_on_main_thread([removed] { rts::Controller::instance().on_source_removed(removed); });
	request_dock_refresh();
}

void on_source_renamed(void *, calldata_t *data)
{
	const char *new_name = calldata_string(data, "new_name");
	const char *prev_name = calldata_string(data, "prev_name");
	const std::string next = new_name ? new_name : "";
	const std::string prev = prev_name ? prev_name : "";
	rts::run_on_main_thread([next, prev] {
		// 名前を変えても選択が外れないよう、選択中の名前を追従させる
		auto selection = rts::PluginSettings::instance().selection();
		bool changed = false;
		if (selection.audio_source == prev) {
			selection.audio_source = next;
			changed = true;
		}
		for (auto &target : selection.targets) {
			if (target == prev) {
				target = next;
				changed = true;
			}
		}
		if (changed)
			rts::PluginSettings::instance().set_selection(selection);
	});
	request_dock_refresh();
}

void on_hotkey(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	const bool start = data != nullptr;
	rts::run_on_main_thread([start] {
		if (start)
			rts::Controller::instance().start();
		else
			rts::Controller::instance().stop();
	});
}

void save_hotkey(obs_data_t *obj, const char *key, obs_hotkey_id id)
{
	obs_data_array_t *array = obs_hotkey_save(id);
	obs_data_set_array(obj, key, array);
	obs_data_array_release(array);
}

void load_hotkey(obs_data_t *obj, const char *key, obs_hotkey_id id)
{
	obs_data_array_t *array = obs_data_get_array(obj, key);
	obs_hotkey_load(id, array);
	obs_data_array_release(array);
}

// 選択とホットキーの割り当てをシーンコレクションに保存し、読み込みで復元する
void on_save(obs_data_t *save_data, bool saving, void *)
{
	auto &settings = rts::PluginSettings::instance();
	if (saving) {
		settings.save_selection_to(save_data);
		obs_data_t *obj = obs_data_get_obj(save_data, rts::PluginSettings::kSaveKey);
		save_hotkey(obj, "hotkey_start", g_hotkey_start);
		save_hotkey(obj, "hotkey_stop", g_hotkey_stop);
		obs_data_release(obj);
		return;
	}

	settings.load_selection_from(save_data);
	obs_data_t *obj = obs_data_get_obj(save_data, rts::PluginSettings::kSaveKey);
	if (!obj)
		obj = obs_data_create();
	load_hotkey(obj, "hotkey_start", g_hotkey_start);
	load_hotkey(obj, "hotkey_stop", g_hotkey_stop);
	obs_data_release(obj);
	// 異常終了で残った本文を消す
	rts::Controller::instance().show_idle_on_selected_targets();
	request_dock_refresh();
}

void register_dock()
{
	auto *main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!main_window)
		return;

	main_window->installEventFilter(new CloseFilter(main_window));

	auto *dock = new rts::Dock();
	dock->setObjectName(kDockId);
	if (!obs_frontend_add_dock_by_id(kDockId, obs_module_text("Dock.Title"), dock)) {
		obs_log(LOG_ERROR, "failed to add dock");
		delete dock;
		return;
	}
	g_dock = dock;

	// OBS は保存済みのドックの配置をこの通知の前に復元し、新しく足したドックを隠すため、配置を再適用する
	config_t *user_config = obs_frontend_get_user_config();
	const char *saved_state = user_config ? config_get_string(user_config, "BasicWindow", "DockState") : nullptr;
	if (saved_state && *saved_state)
		main_window->restoreState(QByteArray::fromBase64(QByteArray(saved_state)));
	obs_log(LOG_INFO, "dock registered");
}

void on_frontend_event(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		register_dock();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
		// 切り替えの保存より前に停止して表示を空にする。続くソースの削除をエラーにしない
		rts::Controller::instance().stop();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		request_dock_refresh();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		rts::Controller::instance().stop();
		break;
	default:
		break;
	}
}

} // namespace

bool obs_module_load(void)
{
	curl_global_init(CURL_GLOBAL_DEFAULT);
	log_curl_runtime();
	rts::PluginSettings::instance().load_global();

	// data が非 null なら開始、null なら停止のホットキー
	static int start_marker = 0;
	g_hotkey_start = obs_hotkey_register_frontend("realtime_translate_summary.start",
						      obs_module_text("Hotkey.Start"), on_hotkey, &start_marker);
	g_hotkey_stop = obs_hotkey_register_frontend("realtime_translate_summary.stop", obs_module_text("Hotkey.Stop"),
						     on_hotkey, nullptr);

	signal_handler_t *handler = obs_get_signal_handler();
	signal_handler_connect(handler, "source_create", on_source_created, nullptr);
	signal_handler_connect(handler, "source_remove", on_source_removed, nullptr);
	signal_handler_connect(handler, "source_rename", on_source_renamed, nullptr);

	obs_frontend_add_save_callback(on_save, nullptr);
	obs_frontend_add_event_callback(on_frontend_event, nullptr);

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);
	obs_frontend_remove_save_callback(on_save, nullptr);

	signal_handler_t *handler = obs_get_signal_handler();
	signal_handler_disconnect(handler, "source_create", on_source_created, nullptr);
	signal_handler_disconnect(handler, "source_remove", on_source_removed, nullptr);
	signal_handler_disconnect(handler, "source_rename", on_source_renamed, nullptr);

	obs_hotkey_unregister(g_hotkey_start);
	obs_hotkey_unregister(g_hotkey_stop);

	rts::Controller::instance().stop();
	// ドックは OBS が破棄する
	g_dock = nullptr;
	curl_global_cleanup();
	obs_log(LOG_INFO, "plugin unloaded");
}
