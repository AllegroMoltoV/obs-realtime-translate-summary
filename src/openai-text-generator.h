#pragma once

#include "core/prompts.h"
#include "core/text-pipeline.h"

#include <atomic>
#include <functional>
#include <string>

namespace rts {

// OpenAI の Responses API で英訳と概要を作る。ログには処理時間とトークン数だけを書き、本文を書かない
class OpenAiTextGenerator : public TextGenerator {
public:
	OpenAiTextGenerator(std::string api_key, ModelProfile translate_profile, ModelProfile summary_profile,
			    int summary_max_chars);

	GenerationResult translate(const std::string &transcript) override;
	GenerationResult summarize(const std::vector<std::string> &transcripts) override;
	void cancel() override { cancel_.store(true); }

	// 入力と出力のトークン数を受け取る。作業のスレッドから呼ばれる
	std::function<void(long long input_tokens, long long output_tokens)> on_usage;
	// 要求の結果を受け取る。停止による中断は呼ばない。is_summary が偽なら英訳。作業のスレッドから呼ばれる
	std::function<void(bool is_summary, bool ok)> on_result;

private:
	GenerationResult send(const char *kind, nlohmann::json request);

	std::string api_key_;
	ModelProfile translate_profile_;
	ModelProfile summary_profile_;
	int summary_max_chars_;
	std::atomic<bool> cancel_{false};
	// temperature を拒否されたら、以後は付けずに送る
	std::atomic<bool> omit_temperature_{false};
};

} // namespace rts
