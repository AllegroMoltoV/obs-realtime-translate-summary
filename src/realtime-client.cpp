#include "realtime-client.h"

// util/base.h の EXPORT 付きの宣言を、plugin-support.h の宣言より先に読み込む
#include <util/base.h>
#include <plugin-support.h>

#include <chrono>
#include <thread>

namespace rts {

RealtimeClient::~RealtimeClient()
{
	close();
}

namespace {

constexpr auto kHandshakeLimit = std::chrono::seconds(20);

} // namespace

// ハンドシェイクの間だけ呼ばれる。0 以外を返すと libcurl が中断する
int RealtimeClient::on_connect_progress(void *user, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
	const auto *self = static_cast<const RealtimeClient *>(user);
	if (self->keep_running_ && !self->keep_running_->load())
		return 1;
	return std::chrono::steady_clock::now() > self->connect_deadline_ ? 1 : 0;
}

bool RealtimeClient::connect(const std::string &url, const std::string &api_key, std::string &error,
			     const std::atomic<bool> *keep_running)
{
	close();

	curl_ = curl_easy_init();
	if (!curl_) {
		error = "curl_easy_init";
		return false;
	}

	const std::string auth = "Authorization: Bearer " + api_key;
	headers_ = curl_slist_append(headers_, auth.c_str());

	curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers_);
	// 2 は WebSocket のハンドシェイクだけを行い、以後の送受信を curl_ws_send と curl_ws_recv に任せる
	curl_easy_setopt(curl_, CURLOPT_CONNECT_ONLY, 2L);
	curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 15L);
	// CURLOPT_TIMEOUT は接続の後の送受信にも効く恐れがあるため使わず、ハンドシェイクの間だけ自前の期限で中断する
	keep_running_ = keep_running;
	connect_deadline_ = std::chrono::steady_clock::now() + kHandshakeLimit;
	curl_easy_setopt(curl_, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl_, CURLOPT_XFERINFOFUNCTION, &RealtimeClient::on_connect_progress);
	curl_easy_setopt(curl_, CURLOPT_XFERINFODATA, this);

	const CURLcode rc = curl_easy_perform(curl_);
	// 接続の後の curl_ws_send と curl_ws_recv では進捗のコールバックを使わない
	curl_easy_setopt(curl_, CURLOPT_NOPROGRESS, 1L);
	long status = 0;
	curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &status);
	last_http_status_ = status;
	if (rc != CURLE_OK) {
		error = std::string("connect curl=") + curl_easy_strerror(rc) + " http=" + std::to_string(status);
		close();
		return false;
	}
	obs_log(LOG_INFO, "realtime connected http=%ld", status);
	return true;
}

void RealtimeClient::close()
{
	if (curl_) {
		size_t sent = 0;
		// 1000 は正常な終了を示す終了コード
		const unsigned char normal_close[2] = {0x03, 0xE8};
		curl_ws_send(curl_, normal_close, sizeof(normal_close), &sent, 0, CURLWS_CLOSE);
		curl_easy_cleanup(curl_);
		curl_ = nullptr;
	}
	if (headers_) {
		curl_slist_free_all(headers_);
		headers_ = nullptr;
	}
	partial_.clear();
	last_pong_ = {};
	pongs_received_ = 0;
	keep_running_ = nullptr;
}

bool RealtimeClient::send_text(const std::string &payload, std::string &error)
{
	if (!curl_) {
		error = "not connected";
		return false;
	}

	size_t offset = 0;
	int again = 0;
	while (offset < payload.size()) {
		size_t sent = 0;
		const CURLcode rc =
			curl_ws_send(curl_, payload.data() + offset, payload.size() - offset, &sent, 0, CURLWS_TEXT);
		offset += sent;
		if (rc == CURLE_AGAIN) {
			if (++again > 2000) {
				error = "send timeout";
				return false;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			continue;
		}
		if (rc != CURLE_OK) {
			error = std::string("send curl=") + curl_easy_strerror(rc);
			return false;
		}
		if (offset < payload.size())
			obs_log(LOG_WARNING, "realtime partial send offset=%zu size=%zu", offset, payload.size());
	}
	return true;
}

bool RealtimeClient::send_ping(std::string &error)
{
	if (!curl_) {
		error = "not connected";
		return false;
	}
	size_t sent = 0;
	const CURLcode rc = curl_ws_send(curl_, "", 0, &sent, 0, CURLWS_PING);
	if (rc != CURLE_OK && rc != CURLE_AGAIN) {
		error = std::string("ping curl=") + curl_easy_strerror(rc);
		return false;
	}
	return true;
}

RealtimeClient::RecvResult RealtimeClient::receive(std::string &message, std::string &error)
{
	if (!curl_) {
		error = "not connected";
		return RecvResult::Error;
	}

	char buffer[65536];
	for (;;) {
		size_t received = 0;
		const curl_ws_frame *meta = nullptr;
		const CURLcode rc = curl_ws_recv(curl_, buffer, sizeof(buffer), &received, &meta);
		if (rc == CURLE_AGAIN)
			return RecvResult::Nothing;
		if (rc == CURLE_GOT_NOTHING) {
			error = "connection closed without close frame";
			return RecvResult::Closed;
		}
		if (rc != CURLE_OK) {
			error = std::string("recv curl=") + curl_easy_strerror(rc);
			return RecvResult::Error;
		}
		if (!meta)
			continue;

		if (meta->flags & CURLWS_CLOSE) {
			close_code_ = received >= 2 ? ((static_cast<unsigned char>(buffer[0]) << 8) |
						       static_cast<unsigned char>(buffer[1]))
						    : 0;
			error = "close frame";
			return RecvResult::Closed;
		}
		if (meta->flags & CURLWS_PONG) {
			last_pong_ = std::chrono::steady_clock::now();
			++pongs_received_;
			continue;
		}
		// サーバーからの PING には libcurl が自動で PONG を返す
		if (meta->flags & CURLWS_PING)
			continue;

		partial_.append(buffer, received);
		if (meta->bytesleft == 0 && (meta->flags & CURLWS_CONT) == 0) {
			message.swap(partial_);
			partial_.clear();
			return RecvResult::Message;
		}
	}
}

} // namespace rts
