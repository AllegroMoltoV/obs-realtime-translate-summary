#pragma once

#include <curl/curl.h>

#include <string>

namespace rts {

// OpenAI Realtime API の WebSocket の接続。libcurl の easy ハンドルを 1 本のスレッドだけが使う前提で、
// スレッドの同期をしない
class RealtimeClient {
public:
	enum class RecvResult { Message, Nothing, Closed, Error };

	RealtimeClient() = default;
	~RealtimeClient();

	RealtimeClient(const RealtimeClient &) = delete;
	RealtimeClient &operator=(const RealtimeClient &) = delete;

	// 失敗した場合は error に原因の種類だけを入れる。キーは入れない
	bool connect(const std::string &url, const std::string &api_key, std::string &error);
	void close();

	bool send_text(const std::string &payload, std::string &error);

	// 1 つのメッセージが揃うまで受信する。届いていなければ Nothing を返す
	RecvResult receive(std::string &message, std::string &error);

	int close_code() const { return close_code_; }

private:
	CURL *curl_ = nullptr;
	curl_slist *headers_ = nullptr;
	std::string partial_;
	int close_code_ = 0;
};

} // namespace rts
