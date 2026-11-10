#include "openai-text-generator.h"

#include "core/text-util.h"
#include "text-client.h"

#include <obs.h>
#include <plugin-support.h>

namespace rts {

OpenAiTextGenerator::OpenAiTextGenerator(std::string api_key, ModelProfile translate_profile,
					 ModelProfile summary_profile, int summary_max_chars)
	: api_key_(std::move(api_key)),
	  translate_profile_(std::move(translate_profile)),
	  summary_profile_(std::move(summary_profile)),
	  summary_max_chars_(summary_max_chars)
{
}

GenerationResult OpenAiTextGenerator::translate(const std::string &transcript)
{
	return send("translate", translate_request(translate_profile_, transcript));
}

GenerationResult OpenAiTextGenerator::summarize(const std::vector<std::string> &transcripts)
{
	return send("summary", summary_request(summary_profile_, transcripts, summary_max_chars_));
}

GenerationResult OpenAiTextGenerator::send(const char *kind, nlohmann::json request)
{
	if (omit_temperature_.load())
		request.erase("temperature");
	const bool temperature_sent = request.contains("temperature");

	TextClient client(api_key_, &cancel_);
	ResponseResult result = client.create(request);
	if (temperature_sent && result.http_status == 400 && result.error_param == "temperature" && !cancel_.load()) {
		obs_log(LOG_WARNING, "%s temperature rejected; retrying without temperature", kind);
		omit_temperature_.store(true);
		request.erase("temperature");
		result = client.create(request);
	}

	obs_log(LOG_INFO,
		"%s http=%ld ok=%d elapsed_ms=%.1f input_tokens=%lld cached_tokens=%lld output_tokens=%lld "
		"reasoning_tokens=%lld output_chars=%zu error_type=%s error_code=%s error_param=%s transport=%s",
		kind, result.http_status, result.ok ? 1 : 0, result.elapsed_ms, result.input_tokens,
		result.cached_tokens, result.output_tokens, result.reasoning_tokens,
		codepoint_length(result.output_text), result.error_type.c_str(), result.error_code.c_str(),
		result.error_param.c_str(), result.transport_error.c_str());

	return GenerationResult{result.ok, result.output_text};
}

} // namespace rts
