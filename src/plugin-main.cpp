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

#include "verify-session.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QMainWindow>
#include <QTimer>

#include <curl/curl.h>

#include <cstdlib>
#include <memory>
#include <string>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {

std::unique_ptr<rts::VerifySession> g_session;

std::string env_value(const char *name)
{
	const char *value = std::getenv(name);
	return value ? std::string(value) : std::string();
}

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

// 段階 1 の検証だけの起動の経路。段階 2 と段階 4 でキーの保存とドックに置き換え、環境変数を読む処理を取り除く
void start_verification()
{
	if (g_session->running())
		return;

	const std::string source = env_value("RTS_VERIFY_SOURCE");
	if (source.empty()) {
		obs_log(LOG_ERROR, "VERIFY RTS_VERIFY_SOURCE is not set");
		return;
	}
	const auto mode = env_value("RTS_VERIFY_MODE") == "full" ? rts::VerifySession::Mode::Full
								  : rts::VerifySession::Mode::AudioOnly;
	const std::string key = env_value("OPENAI_API_KEY");
	if (mode == rts::VerifySession::Mode::Full && key.empty()) {
		obs_log(LOG_ERROR, "VERIFY OPENAI_API_KEY is not set");
		return;
	}
	g_session->start(source, key, mode);
}

void stop_verification()
{
	g_session->stop();
}

void on_frontend_event(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		if (env_value("RTS_VERIFY_AUTOSTART") == "1") {
			obs_log(LOG_INFO, "VERIFY autostart");
			start_verification();
		}

		const int seconds = std::atoi(env_value("RTS_VERIFY_AUTOQUIT_SECONDS").c_str());
		if (seconds <= 0)
			return;

		// 管理者として動く OBS には外のプロセスから終了を要求できないため、検証の終わりに OBS の中から閉じる
		auto *main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		obs_log(LOG_INFO, "VERIFY autoquit_seconds=%d", seconds);
		QTimer::singleShot(seconds * 1000, main_window, [main_window]() {
			stop_verification();
			obs_log(LOG_INFO, "VERIFY autoquit closing main window");
			main_window->close();
		});
	} else if (event == OBS_FRONTEND_EVENT_EXIT) {
		stop_verification();
	}
}

} // namespace

bool obs_module_load(void)
{
	curl_global_init(CURL_GLOBAL_DEFAULT);
	log_curl_runtime();

	g_session = std::make_unique<rts::VerifySession>();
	obs_log(LOG_INFO, "VERIFY env OPENAI_API_KEY present=%d RTS_VERIFY_SOURCE present=%d RTS_VERIFY_MODE=%s",
		env_value("OPENAI_API_KEY").empty() ? 0 : 1, env_value("RTS_VERIFY_SOURCE").empty() ? 0 : 1,
		env_value("RTS_VERIFY_MODE").c_str());

	obs_frontend_add_tools_menu_item(obs_module_text("VerifyStart"), [](void *) { start_verification(); },
					 nullptr);
	obs_frontend_add_tools_menu_item(obs_module_text("VerifyStop"), [](void *) { stop_verification(); },
					 nullptr);
	obs_frontend_add_event_callback(on_frontend_event, nullptr);

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);
	if (g_session) {
		g_session->stop();
		g_session.reset();
	}
	curl_global_cleanup();
	obs_log(LOG_INFO, "plugin unloaded");
}
