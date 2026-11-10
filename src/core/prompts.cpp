#include "prompts.h"

#include <algorithm>

namespace rts {

std::string translate_instructions()
{
	return "日本語の配信字幕を、意味を保った簡潔で自然な英語字幕へ翻訳する。説明や引用符を付けず、翻訳だけを返す。";
}

std::string summary_instructions(int max_chars)
{
	const int line_limit = max_chars - 2;
	// 元のツールの min(round(N * 0.8), N - 2)。整数の N では N * 0.8 の端数が 0.5 にならないため、四捨五入で一致する
	const int target = (std::min)(static_cast<int>(max_chars * 0.8 + 0.5), line_limit);

	return std::string(
		       "日本語の一人語り配信を要約する。recent_transcriptsは直近5分間に確定した発言を古い順に並べたもの。"
		       "この発言に含まれる内容だけを使い、発言にない配信内容、予定、募集、視聴者への呼びかけを加えない。"
		       "例えば「今日はピアノを弾きます」だけなら、演奏することだけを1行に書き、リクエスト募集は書かない。"
		       "過去に終えた行は過去形、今している行は進行中の形、これからする行は予定や意向を示す形で述語まで書く。"
		       "発言の時点と時制を変えず、条件付きの予定を断定しない。"
		       "後の発言で話題や予定が変わった場合は新しい発言を優先する。"
		       "display_linesはOBSへ表示する日本語で、見出しや単語の羅列ではなく、発言した対象、動作、順序が伝わる文にする。"
		       "まず同じ対象について述べた内容を1つの話題にまとめ、話題ごとに1行で最大3行にする。"
		       "例えば果物としてリンゴ、バナナ、ミカンを挙げた場合は種類を1行に列挙し、果物の行を増やさない。"
		       "同じ話題の列挙が1行の上限内に収まる場合は、挙げられた種類を省かず1行にまとめる。"
		       "列挙された固有の種類は「など」「等」で省略せず、説明語を短くして全種類を先に残す。"
		       "同じ話題を分割して3行を埋めない。各行の最大文字数は") +
	       std::to_string(line_limit) +
	       "文字とし、必ずこの上限以内に収める。"
	       "文字数はUTF-8のバイト数や英単語数ではなく、漢字、ひらがな、カタカナ、英数字、記号をそれぞれ1文字として数える。"
	       "情報が十分な行は" +
	       std::to_string(target) +
	       "文字前後を目指す。"
	       "短い要点だけに圧縮せず、発言された時点、対象、動作に加え、方法または理由などの関連情報を一つ程度含める。"
	       "目安を超える細部は重要度に応じて省く。時制と具体的な内容を残す。情報が足りない行は無理に伸ばさない。"
	       "文字数や行数を満たすために内容を水増し・反復しない。返答前に各行を確認し、" +
	       std::to_string(line_limit) +
	       "文字を超えた行は、事実と時制を保って短く書き直す。"
	       "配列は必ず3要素とする。表示する話題が2件なら3行目は空文字、1件なら2行目と3行目は空文字にする。";
}

nlohmann::json summary_schema()
{
	return nlohmann::json{
		{"type", "object"},
		{"properties",
		 {{"display_lines",
		   {{"type", "array"}, {"items", {{"type", "string"}}}, {"minItems", 3}, {"maxItems", 3}}}}},
		{"required", nlohmann::json::array({"display_lines"})},
		{"additionalProperties", false}};
}

nlohmann::json session_update(const std::vector<std::string> &keywords)
{
	nlohmann::json transcription = {{"model", "gpt-transcribe"},
					{"prompt", "日本語の一人語り。固有名詞、数字、英単語を正確に転写する。"}};
	if (!keywords.empty())
		transcription["keywords"] = keywords;

	return nlohmann::json{{"type", "session.update"},
			      {"session",
			       {{"type", "transcription"},
				{"audio",
				 {{"input",
				   {{"format", {{"type", "audio/pcm"}, {"rate", 24000}}},
				    {"transcription", transcription},
				    {"turn_detection",
				     {{"type", "server_vad"},
				      {"threshold", 0.5},
				      {"prefix_padding_ms", 300},
				      {"silence_duration_ms", 500}}}}}}}}}};
}

namespace {

nlohmann::json base_request(const ModelProfile &profile, const std::string &instructions, int max_output_tokens)
{
	return nlohmann::json{{"model", profile.model},
			      {"instructions", instructions},
			      {"reasoning", {{"effort", profile.reasoning_effort}}},
			      {"max_output_tokens", max_output_tokens},
			      {"store", false}};
}

} // namespace

nlohmann::json translate_request(const ModelProfile &profile, const std::string &transcript)
{
	nlohmann::json request = base_request(profile, translate_instructions(), 256);
	request["input"] = transcript;
	if (profile.send_temperature)
		request["temperature"] = 0.2;
	return request;
}

nlohmann::json summary_request(const ModelProfile &profile, const std::vector<std::string> &transcripts, int max_chars)
{
	nlohmann::json request = base_request(profile, summary_instructions(max_chars), 512);
	request["input"] = nlohmann::json{{"recent_transcripts", transcripts}}.dump();
	request["text"] = {
		{"format",
		 {{"type", "json_schema"}, {"name", "summary"}, {"strict", true}, {"schema", summary_schema()}}}};
	if (profile.send_temperature)
		request["temperature"] = 0.0;
	return request;
}

} // namespace rts
