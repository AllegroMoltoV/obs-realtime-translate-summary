#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace rts {

// OpenAI Responses API の 1 回の呼び出しの結果。本文は呼び出し元が使い、ログには書かない
struct ResponseResult {
	long http_status = 0;
	double elapsed_ms = 0.0;
	bool ok = false;
	std::string output_text;
	long long input_tokens = -1;
	long long cached_tokens = -1;
	long long output_tokens = -1;
	long long reasoning_tokens = -1;
	std::string error_type;
	std::string error_code;
	std::string error_param;
	std::string transport_error;
};

// v1/responses へ同期で要求を送る。呼び出し元のスレッドで待つ
class TextClient {
public:
	explicit TextClient(std::string api_key) : api_key_(std::move(api_key)) {}

	ResponseResult create(const nlohmann::json &request) const;

private:
	std::string api_key_;
};

} // namespace rts
