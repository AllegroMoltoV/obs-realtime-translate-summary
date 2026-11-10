#include "verify-session.h"

#include "base64.h"
#include "pcm-converter.h"
#include "realtime-client.h"
#include "text-client.h"

#include <obs.h>
#include <plugin-support.h>

#include <nlohmann/json.hpp>

#include <algorithm>

using json = nlohmann::json;

namespace rts {

namespace {

constexpr const char *kRealtimeUrl = "wss://api.openai.com/v1/realtime?intent=transcription";
constexpr const char *kTextModel = "gpt-6-luna";
constexpr size_t kChunkSamples = PcmConverter::kOutputRate / 10; // 100 ms
constexpr int kSummaryMaxChars = 30;
constexpr auto kSummaryWindow = std::chrono::minutes(5);

// 元のツールの text_processing.py の指示文をそのまま移した
constexpr const char *kTranslateInstructions =
	"日本語の配信字幕を、意味を保った簡潔で自然な英語字幕へ翻訳する。説明や引用符を付けず、翻訳だけを返す。";

std::string summary_instructions(int max_chars)
{
	const int line_limit = max_chars - 2;
	// Windows の min マクロとの衝突を避けるため、かっこで囲んで呼ぶ
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

json summary_schema()
{
	return json{{"type", "object"},
		    {"properties",
		     {{"display_lines",
		       {{"type", "array"}, {"items", {{"type", "string"}}}, {"minItems", 3}, {"maxItems", 3}}}}},
		    {"required", json::array({"display_lines"})},
		    {"additionalProperties", false}};
}

json session_update()
{
	return json{{"type", "session.update"},
		    {"session",
		     {{"type", "transcription"},
		      {"audio",
		       {{"input",
			 {{"format", {{"type", "audio/pcm"}, {"rate", 24000}}},
			  {"transcription",
			   {{"model", "gpt-transcribe"},
			    {"prompt", "日本語の一人語り。固有名詞、数字、英単語を正確に転写する。"}}},
			  {"turn_detection",
			   {{"type", "server_vad"},
			    {"threshold", 0.5},
			    {"prefix_padding_ms", 300},
			    {"silence_duration_ms", 500}}}}}}}}}};
}

size_t utf8_length(const std::string &text)
{
	size_t count = 0;
	for (const unsigned char c : text) {
		if ((c & 0xC0) != 0x80)
			++count;
	}
	return count;
}

std::string json_string(const json &object, const char *key)
{
	const auto value = object.find(key);
	return value != object.end() && value->is_string() ? value->get<std::string>() : std::string();
}

void log_response(const char *kind, const ResponseResult &r, bool temperature_sent)
{
	obs_log(LOG_INFO,
		"VERIFY %s http=%ld ok=%d elapsed_ms=%.1f temperature_sent=%d input_tokens=%lld cached_tokens=%lld "
		"output_tokens=%lld reasoning_tokens=%lld output_chars=%zu error_type=%s error_code=%s error_param=%s "
		"transport=%s",
		kind, r.http_status, r.ok ? 1 : 0, r.elapsed_ms, temperature_sent ? 1 : 0, r.input_tokens,
		r.cached_tokens, r.output_tokens, r.reasoning_tokens, utf8_length(r.output_text), r.error_type.c_str(),
		r.error_code.c_str(), r.error_param.c_str(), r.transport_error.c_str());
}

} // namespace

VerifySession::~VerifySession()
{
	stop();
}

bool VerifySession::start(const std::string &source_name, const std::string &api_key, Mode mode)
{
	if (running_.load())
		return false;
	stop();

	source_name_ = source_name;
	api_key_ = api_key;
	mode_ = mode;
	running_.store(true);
	network_thread_ = std::thread(&VerifySession::network_loop, this);
	if (mode_ == Mode::Full)
		text_thread_ = std::thread(&VerifySession::text_loop, this);
	return true;
}

void VerifySession::stop()
{
	// 通信の失敗で running_ が先に false になっていても、スレッドは必ず合流させる
	running_.store(false);
	text_cv_.notify_all();
	const bool had_threads = network_thread_.joinable() || text_thread_.joinable();
	if (network_thread_.joinable())
		network_thread_.join();
	if (text_thread_.joinable())
		text_thread_.join();
	if (had_threads)
		obs_log(LOG_INFO, "VERIFY stopped");
}

void VerifySession::network_loop()
{
	PcmConverter converter;
	const uint32_t input_rate = audio_output_get_sample_rate(obs_get_audio());
	if (!converter.open(input_rate)) {
		obs_log(LOG_ERROR, "VERIFY resampler open failed input_rate=%u", input_rate);
		running_.store(false);
		return;
	}

	RealtimeClient client;
	std::string error;
	if (mode_ == Mode::Full) {
		if (!client.connect(kRealtimeUrl, api_key_, error)) {
			obs_log(LOG_ERROR, "VERIFY realtime connect failed %s", error.c_str());
			running_.store(false);
			text_cv_.notify_all();
			return;
		}
		if (!client.send_text(session_update().dump(), error)) {
			obs_log(LOG_ERROR, "VERIFY session.update send failed %s", error.c_str());
			running_.store(false);
			text_cv_.notify_all();
			return;
		}
		network_alive_ = true;
	}

	if (!capture_.attach(source_name_)) {
		obs_log(LOG_ERROR, "VERIFY audio source not found or has no audio");
		running_.store(false);
		text_cv_.notify_all();
		return;
	}

	started_ = Clock::now();
	last_metrics_ = started_;
	total_frames_at_start_ = obs_get_total_frames();
	lagged_frames_at_start_ = obs_get_lagged_frames();
	obs_log(LOG_INFO, "VERIFY started mode=%s input_rate=%u", mode_ == Mode::Full ? "full" : "audio", input_rate);

	std::vector<int16_t> pcm;
	MonoFrame frame;
	while (running_.load()) {
		bool worked = false;

		while (capture_.pop(frame)) {
			worked = true;
			const size_t before = pcm.size();
			converter.convert(frame.samples, frame.frames, pcm);
			produced_samples_ += pcm.size() - before;
		}

		while (pcm.size() >= kChunkSamples) {
			if (network_alive_) {
				const std::string audio = base64_encode(reinterpret_cast<const uint8_t *>(pcm.data()),
									kChunkSamples * sizeof(int16_t));
				const json append = {{"type", "input_audio_buffer.append"}, {"audio", audio}};
				if (client.send_text(append.dump(), error)) {
					++sent_chunks_;
				} else {
					obs_log(LOG_ERROR, "VERIFY audio send failed %s", error.c_str());
					network_alive_ = false;
				}
			}
			pcm.erase(pcm.begin(), pcm.begin() + kChunkSamples);
		}

		while (network_alive_) {
			std::string message;
			const auto result = client.receive(message, error);
			if (result == RealtimeClient::RecvResult::Nothing)
				break;
			worked = true;
			if (result == RealtimeClient::RecvResult::Message) {
				handle_event(message);
				continue;
			}
			obs_log(LOG_ERROR, "VERIFY realtime %s close_code=%d elapsed_s=%.1f",
				result == RealtimeClient::RecvResult::Closed ? "closed" : "error", client.close_code(),
				std::chrono::duration<double>(Clock::now() - started_).count());
			obs_log(LOG_ERROR, "VERIFY realtime detail %s", error.c_str());
			network_alive_ = false;
		}

		if (Clock::now() - last_metrics_ >= std::chrono::seconds(10))
			log_metrics();

		if (!worked)
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}

	capture_.detach();
	log_metrics();
	client.close();
}

void VerifySession::handle_event(const std::string &message)
{
	const json event = json::parse(message, nullptr, false);
	if (event.is_discarded() || !event.is_object()) {
		obs_log(LOG_WARNING, "VERIFY event invalid json bytes=%zu", message.size());
		return;
	}

	const std::string type = json_string(event, "type");
	if (type == "conversation.item.input_audio_transcription.delta") {
		++delta_events_;
		return;
	}

	if (type == "conversation.item.input_audio_transcription.completed") {
		++completed_events_;
		const std::string transcript = json_string(event, "transcript");
		obs_log(LOG_INFO, "VERIFY event %s chars=%zu", type.c_str(), utf8_length(transcript));
		if (!transcript.empty()) {
			std::lock_guard<std::mutex> lock(text_mutex_);
			pending_transcripts_.push_back(transcript);
		}
		text_cv_.notify_one();
		return;
	}

	if (type == "error" || type == "conversation.item.input_audio_transcription.failed") {
		const auto error = event.find("error");
		const json detail = error != event.end() && error->is_object() ? *error : json::object();
		obs_log(LOG_ERROR, "VERIFY event %s error_type=%s error_code=%s error_param=%s", type.c_str(),
			json_string(detail, "type").c_str(), json_string(detail, "code").c_str(),
			json_string(detail, "param").c_str());
		return;
	}

	obs_log(LOG_INFO, "VERIFY event %s", type.c_str());
}

void VerifySession::log_metrics()
{
	const auto now = Clock::now();
	const double elapsed = std::chrono::duration<double>(now - started_).count();
	const double produced = static_cast<double>(produced_samples_) / PcmConverter::kOutputRate;
	obs_log(LOG_INFO,
		"VERIFY metrics elapsed_s=%.1f produced_s=%.2f ratio=%.4f queued=%zu dropped_frames=%llu "
		"muted_callbacks=%llu sent_chunks=%llu delta=%llu completed=%llu total_frames=%llu lagged_frames=%llu",
		elapsed, produced, elapsed > 0 ? produced / elapsed : 0.0, capture_.queued(),
		static_cast<unsigned long long>(capture_.dropped_frames()),
		static_cast<unsigned long long>(capture_.muted_callbacks()),
		static_cast<unsigned long long>(sent_chunks_), static_cast<unsigned long long>(delta_events_),
		static_cast<unsigned long long>(completed_events_),
		static_cast<unsigned long long>(obs_get_total_frames() - total_frames_at_start_),
		static_cast<unsigned long long>(obs_get_lagged_frames() - lagged_frames_at_start_));
	last_metrics_ = now;
}

void VerifySession::text_loop()
{
	for (;;) {
		std::string transcript;
		{
			std::unique_lock<std::mutex> lock(text_mutex_);
			text_cv_.wait(lock, [this] { return !running_.load() || !pending_transcripts_.empty(); });
			if (!running_.load())
				return;
			transcript = std::move(pending_transcripts_.front());
			pending_transcripts_.pop_front();
			recent_transcripts_.emplace_back(Clock::now(), transcript);
		}
		translate(transcript);
		summarize();
	}
}

void VerifySession::translate(const std::string &transcript)
{
	TextClient client(api_key_);
	json request = {{"model", kTextModel},      {"instructions", kTranslateInstructions},
			{"input", transcript},      {"reasoning", {{"effort", "none"}}},
			{"max_output_tokens", 256}, {"store", false}};
	const bool with_temperature = !omit_temperature_.load();
	if (with_temperature)
		request["temperature"] = 0.2;

	ResponseResult result = client.create(request);
	log_response("translate", result, with_temperature);
	if (with_temperature && result.http_status == 400 && result.error_param == "temperature") {
		omit_temperature_.store(true);
		request.erase("temperature");
		result = client.create(request);
		log_response("translate-retry", result, false);
	}
}

void VerifySession::summarize()
{
	json transcripts = json::array();
	{
		std::lock_guard<std::mutex> lock(text_mutex_);
		const auto cutoff = Clock::now() - kSummaryWindow;
		std::vector<std::pair<Clock::time_point, std::string>> kept;
		for (auto &entry : recent_transcripts_) {
			if (entry.first >= cutoff)
				kept.push_back(std::move(entry));
		}
		recent_transcripts_.swap(kept);
		for (const auto &entry : recent_transcripts_)
			transcripts.push_back(entry.second);
	}

	TextClient client(api_key_);
	json request = {
		{"model", kTextModel},
		{"instructions", summary_instructions(kSummaryMaxChars)},
		{"input", json{{"recent_transcripts", transcripts}}.dump()},
		{"reasoning", {{"effort", "none"}}},
		{"max_output_tokens", 512},
		{"store", false},
		{"text",
		 {{"format",
		   {{"type", "json_schema"}, {"name", "summary"}, {"strict", true}, {"schema", summary_schema()}}}}}};
	const bool with_temperature = !omit_temperature_.load();
	if (with_temperature)
		request["temperature"] = 0.0;

	ResponseResult result = client.create(request);
	log_response("summary", result, with_temperature);
	if (with_temperature && result.http_status == 400 && result.error_param == "temperature") {
		omit_temperature_.store(true);
		request.erase("temperature");
		result = client.create(request);
		log_response("summary-retry", result, false);
	}
	if (!result.ok)
		return;

	const json parsed = json::parse(result.output_text, nullptr, false);
	const auto lines = parsed.is_object() ? parsed.find("display_lines") : parsed.end();
	if (parsed.is_discarded() || !parsed.is_object() || lines == parsed.end() || !lines->is_array()) {
		obs_log(LOG_WARNING, "VERIFY summary display_lines missing");
		return;
	}
	std::string counts;
	for (const auto &line : *lines) {
		if (!counts.empty())
			counts += ",";
		counts += line.is_string() ? std::to_string(utf8_length(line.get<std::string>())) : "non-string";
	}
	obs_log(LOG_INFO, "VERIFY summary display_lines size=%zu line_chars=%s transcripts=%zu", lines->size(),
		counts.c_str(), transcripts.size());
}

} // namespace rts
