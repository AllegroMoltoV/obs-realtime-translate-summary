#include "text-client.h"

#include <curl/curl.h>

#include <chrono>

namespace rts {

namespace {

constexpr const char *kResponsesUrl = "https://api.openai.com/v1/responses";

size_t append_body(char *data, size_t size, size_t count, void *user)
{
	static_cast<std::string *>(user)->append(data, size * count);
	return size * count;
}

long long token_count(const nlohmann::json &usage, const char *key, const char *detail_key = nullptr,
		      const char *detail_field = nullptr)
{
	if (detail_key) {
		const auto details = usage.find(detail_key);
		if (details == usage.end() || !details->is_object())
			return -1;
		const auto value = details->find(detail_field);
		return value != details->end() && value->is_number_integer() ? value->get<long long>() : -1;
	}
	const auto value = usage.find(key);
	return value != usage.end() && value->is_number_integer() ? value->get<long long>() : -1;
}

std::string string_field(const nlohmann::json &object, const char *key)
{
	const auto value = object.find(key);
	return value != object.end() && value->is_string() ? value->get<std::string>() : std::string();
}

} // namespace

ResponseResult TextClient::create(const nlohmann::json &request) const
{
	ResponseResult result;

	CURL *curl = curl_easy_init();
	if (!curl) {
		result.transport_error = "curl_easy_init";
		return result;
	}

	const std::string body = request.dump();
	std::string response;
	curl_slist *headers = nullptr;
	headers = curl_slist_append(headers, "Content-Type: application/json");
	headers = curl_slist_append(headers, ("Authorization: Bearer " + api_key_).c_str());

	curl_easy_setopt(curl, CURLOPT_URL, kResponsesUrl);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &append_body);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

	const auto started = std::chrono::steady_clock::now();
	const CURLcode rc = curl_easy_perform(curl);
	result.elapsed_ms =
		std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.http_status);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	if (rc != CURLE_OK) {
		result.transport_error = curl_easy_strerror(rc);
		return result;
	}

	const nlohmann::json parsed = nlohmann::json::parse(response, nullptr, false);
	if (parsed.is_discarded() || !parsed.is_object()) {
		result.transport_error = "invalid json";
		return result;
	}

	const auto error = parsed.find("error");
	if (error != parsed.end() && error->is_object()) {
		result.error_type = string_field(*error, "type");
		result.error_code = string_field(*error, "code");
		result.error_param = string_field(*error, "param");
	}

	const auto usage = parsed.find("usage");
	if (usage != parsed.end() && usage->is_object()) {
		result.input_tokens = token_count(*usage, "input_tokens");
		result.cached_tokens = token_count(*usage, nullptr, "input_tokens_details", "cached_tokens");
		result.output_tokens = token_count(*usage, "output_tokens");
		result.reasoning_tokens = token_count(*usage, nullptr, "output_tokens_details", "reasoning_tokens");
	}

	// output_text は SDK だけの便利なプロパティのため、output の message から本文を集める
	const auto output = parsed.find("output");
	if (output != parsed.end() && output->is_array()) {
		for (const auto &item : *output) {
			if (string_field(item, "type") != "message")
				continue;
			const auto content = item.find("content");
			if (content == item.end() || !content->is_array())
				continue;
			for (const auto &part : *content) {
				if (string_field(part, "type") == "output_text")
					result.output_text += string_field(part, "text");
			}
		}
	}

	result.ok = result.http_status == 200 && !result.output_text.empty();
	return result;
}

} // namespace rts
