#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace rts {

// 英訳と概要に使うモデルと推論の設定
struct ModelProfile {
	std::string model;
	std::string reasoning_effort = "none";
	bool send_temperature = true;
};

// 2026-11-10 の比較で決めたモデル。根拠は docs/reports/2026-11-10-model-comparison.md
inline ModelProfile default_translate_profile()
{
	return ModelProfile{"gpt-5.6-luna", "none", true};
}

inline ModelProfile default_summary_profile()
{
	return ModelProfile{"gpt-6-luna", "none", true};
}

// 元のツールの text_processing.py と transcription.py の指示文とスキーマを移した
std::string translate_instructions();
std::string summary_instructions(int max_chars);
nlohmann::json summary_schema();

nlohmann::json session_update(const std::vector<std::string> &keywords);

nlohmann::json translate_request(const ModelProfile &profile, const std::string &transcript);
nlohmann::json summary_request(const ModelProfile &profile, const std::vector<std::string> &transcripts, int max_chars);

} // namespace rts
