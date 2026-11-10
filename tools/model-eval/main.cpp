// 英訳と概要のモデルを比べる。OBS を使わず、架空の発話の例を Responses API に送る。
// キーは環境変数 OPENAI_API_KEY から読み、出力にもログにも書かない。
// 使い方: rts-model-eval <samples のフォルダー> <出力のフォルダー> [effort] [all|summary] [モデル,モデル]
//   effort を省略すると、各モデルが受け付ける最も低い値を確かめて使う。
//   effort が none 以外のときは、公式のガイドにしたがい temperature を送らない

#include "core/prompts.h"
#include "core/summary-parser.h"
#include "core/text-util.h"
#include "text-client.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;
using namespace rts;

namespace {

constexpr int kMaxChars = 30;
constexpr double kSummaryIntervalSeconds = 10.0;
constexpr double kSummaryWindowSeconds = 300.0;
// INIT.md 3.6 節の元のツールの実績 (1,704.396 秒で英訳 77 回、概要 31 回) を 1 時間へ換算した回数
constexpr double kTranslationsPerHour = 77.0 * 3600.0 / 1704.396;
constexpr double kSummariesPerHour = 31.0 * 3600.0 / 1704.396;

struct ModelSpec {
	std::string name;
	double price_input;  // 100 万トークンあたりの米ドル
	double price_cached; // 100 万トークンあたりの米ドル
	double price_output; // 100 万トークンあたりの米ドル。推論のトークンは出力に含まれる
	ModelProfile profile;
	bool temperature_rejected = false;
};

struct Utterance {
	double t = 0.0;
	std::string text;
};

struct Scene {
	std::string file;
	std::string title;
	std::vector<Utterance> utterances;
};

struct Call {
	std::string model;
	std::string kind; // translate または summary
	std::string scene;
	int index = 0;
	ResponseResult result;
	bool temperature_sent = false;
	std::vector<std::string> flags;
	std::string rendered; // 出力の md に載せる本文
};

std::vector<Scene> load_scenes(const fs::path &dir)
{
	std::vector<fs::path> files;
	for (const auto &entry : fs::directory_iterator(dir)) {
		if (entry.path().extension() == ".json")
			files.push_back(entry.path());
	}
	std::sort(files.begin(), files.end());

	std::vector<Scene> scenes;
	for (const auto &file : files) {
		std::ifstream in(file, std::ios::binary);
		const json parsed = json::parse(in, nullptr, false);
		if (parsed.is_discarded() || !parsed.is_object()) {
			std::fprintf(stderr, "invalid sample: %s\n", file.string().c_str());
			continue;
		}
		Scene scene;
		scene.file = file.filename().string();
		scene.title = parsed.value("scene", scene.file);
		for (const auto &u : parsed.value("utterances", json::array()))
			scene.utterances.push_back({u.value("t", 0.0), u.value("text", std::string())});
		scenes.push_back(std::move(scene));
	}
	return scenes;
}

bool starts_or_ends_with_quote(const std::string &text)
{
	static const std::vector<std::string> quotes = {"\"",           "'",
							"\xE2\x80\x9C", "\xE2\x80\x9D",
							"\xE2\x80\x98", "\xE2\x80\x99",
							"\xE3\x80\x8C", "\xE3\x80\x8D",
							"\xE3\x80\x8E", "\xE3\x80\x8F"};
	for (const auto &q : quotes) {
		if (text.rfind(q, 0) == 0)
			return true;
		if (text.size() >= q.size() && text.compare(text.size() - q.size(), q.size(), q) == 0)
			return true;
	}
	return false;
}

bool contains_japanese(const std::string &text)
{
	// ひらがな、カタカナ、CJK 統合漢字の UTF-8 の先頭バイトは 0xE3 から 0xE9
	for (const unsigned char c : text) {
		if (c >= 0xE3 && c <= 0xE9)
			return true;
	}
	return false;
}

std::vector<std::string> translation_flags(const std::string &text)
{
	std::vector<std::string> flags;
	if (text.empty())
		flags.push_back("empty");
	if (starts_or_ends_with_quote(text))
		flags.push_back("quote");
	if (contains_line_break(text))
		flags.push_back("newline");
	if (contains_japanese(text))
		flags.push_back("japanese");
	std::string lower = text;
	std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
	if (lower.rfind("translation", 0) == 0 || lower.rfind("english", 0) == 0)
		flags.push_back("prefix");
	return flags;
}

// reasoning.effort を受け付けない 400 か判定する
bool effort_rejected(const ResponseResult &r)
{
	return r.http_status == 400 && r.error_param.find("reasoning") != std::string::npos;
}

ResponseResult send(TextClient &client, ModelSpec &model, json request, bool &temperature_sent)
{
	if (model.temperature_rejected)
		request.erase("temperature");
	temperature_sent = request.contains("temperature");
	ResponseResult result = client.create(request);
	if (temperature_sent && result.http_status == 400 && result.error_param == "temperature") {
		model.temperature_rejected = true;
		request.erase("temperature");
		temperature_sent = false;
		result = client.create(request);
	}
	return result;
}

// 各モデルが受け付ける最も低い reasoning.effort を、短い英訳で確かめる
void choose_effort(TextClient &client, ModelSpec &model)
{
	for (const char *effort : {"none", "minimal", "low"}) {
		model.profile.reasoning_effort = effort;
		bool temperature_sent = false;
		const ResponseResult r =
			send(client, model, translate_request(model.profile, "テストです。"), temperature_sent);
		if (r.http_status == 200) {
			std::printf("%s: reasoning.effort=%s temperature=%s\n", model.name.c_str(), effort,
				    model.temperature_rejected ? "rejected" : "accepted");
			return;
		}
		if (!effort_rejected(r)) {
			std::printf("%s: probe failed http=%ld type=%s param=%s transport=%s\n", model.name.c_str(),
				    r.http_status, r.error_type.c_str(), r.error_param.c_str(),
				    r.transport_error.c_str());
			return;
		}
	}
}

double percentile(std::vector<double> values, double p)
{
	if (values.empty())
		return 0.0;
	std::sort(values.begin(), values.end());
	const size_t index = static_cast<size_t>(std::ceil(p * static_cast<double>(values.size()))) - 1;
	return values[(std::min)(index, values.size() - 1)];
}

double median(std::vector<double> values)
{
	if (values.empty())
		return 0.0;
	std::sort(values.begin(), values.end());
	const size_t n = values.size();
	return n % 2 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2.0;
}

std::string escape_cell(const std::string &text)
{
	std::string out;
	for (const char c : text) {
		if (c == '|')
			out += "\\|";
		else if (c == '\n' || c == '\r')
			out += "<br>";
		else
			out += c;
	}
	return out;
}

std::string join(const std::vector<std::string> &items, const char *sep)
{
	std::string out;
	for (const auto &item : items) {
		if (!out.empty())
			out += sep;
		out += item;
	}
	return out;
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 3) {
		std::fprintf(stderr, "usage: rts-model-eval <samples-dir> <output-dir>\n");
		return 2;
	}
	const char *key = std::getenv("OPENAI_API_KEY");
	if (!key || !*key) {
		std::fprintf(stderr, "OPENAI_API_KEY is not set\n");
		return 2;
	}

	const fs::path samples_dir = argv[1];
	const fs::path out_dir = argv[2];
	fs::create_directories(out_dir);
	const auto scenes = load_scenes(samples_dir);

	curl_global_init(CURL_GLOBAL_DEFAULT);
	TextClient client(key);

	const std::string effort_arg = argc > 3 ? argv[3] : "";
	const bool summary_only = argc > 4 && std::string(argv[4]) == "summary";
	const std::string model_filter = argc > 5 ? argv[5] : "";

	std::vector<ModelSpec> all_models = {
		{"gpt-6-luna", 0.10, 0.01, 0.50, {}},
		{"gpt-5.6-luna", 0.20, 0.02, 1.20, {}},
		{"gpt-5.4-nano", 0.20, 0.02, 1.25, {}},
	};
	std::vector<ModelSpec> models;
	for (auto &model : all_models) {
		if (model_filter.empty() ||
		    ("," + model_filter + ",").find("," + model.name + ",") != std::string::npos)
			models.push_back(model);
	}
	for (auto &model : models) {
		model.profile.model = model.name;
		if (effort_arg.empty()) {
			choose_effort(client, model);
		} else {
			model.profile.reasoning_effort = effort_arg;
			model.profile.send_temperature = effort_arg == "none";
			std::printf("%s: reasoning.effort=%s temperature=%s\n", model.name.c_str(), effort_arg.c_str(),
				    model.profile.send_temperature ? "sent" : "not sent");
		}
	}

	std::vector<Call> calls;
	for (const auto &scene : scenes) {
		std::printf("scene %s\n", scene.file.c_str());
		double last_summary_t = -1e9;
		for (size_t i = 0; i < scene.utterances.size(); ++i) {
			const Utterance &u = scene.utterances[i];
			const bool summary_due = u.t - last_summary_t >= kSummaryIntervalSeconds;
			std::vector<std::string> window;
			if (summary_due) {
				last_summary_t = u.t;
				for (size_t k = 0; k <= i; ++k) {
					if (scene.utterances[k].t >= u.t - kSummaryWindowSeconds)
						window.push_back(scene.utterances[k].text);
				}
			}

			// 回線の変動が特定のモデルに偏らないよう、送る順番を発話ごとにずらす
			for (size_t m = 0; m < models.size(); ++m) {
				ModelSpec &model = models[(i + m) % models.size()];

				if (!summary_only) {
					Call t;
					t.model = model.name;
					t.kind = "translate";
					t.scene = scene.file;
					t.index = static_cast<int>(i);
					t.result = send(client, model, translate_request(model.profile, u.text),
							t.temperature_sent);
					t.rendered = trim_unicode(t.result.output_text);
					t.flags = t.result.ok ? translation_flags(t.rendered)
							      : std::vector<std::string>{"failed"};
					calls.push_back(std::move(t));
				}

				if (!summary_due)
					continue;
				Call s;
				s.model = model.name;
				s.kind = "summary";
				s.scene = scene.file;
				s.index = static_cast<int>(i);
				s.result = send(client, model, summary_request(model.profile, window, kMaxChars),
						s.temperature_sent);
				const auto parsed = s.result.ok
							    ? parse_summary_response(s.result.output_text, kMaxChars)
							    : std::nullopt;
				if (!s.result.ok) {
					s.flags.push_back("failed");
				} else if (!parsed) {
					s.flags.push_back("invalid");
					s.rendered = s.result.output_text;
				} else {
					std::vector<std::string> lines;
					for (size_t k = 0; k < 3; ++k) {
						if (parsed->display_lines[k].empty())
							continue;
						lines.push_back(parsed->display_lines[k] + " (" +
								std::to_string(parsed->model_line_chars[k]) + ")");
						if (parsed->model_line_chars[k] > static_cast<size_t>(kMaxChars - 2))
							s.flags.push_back("over" + std::to_string(kMaxChars - 2));
					}
					s.rendered = join(lines, "<br>");
				}
				calls.push_back(std::move(s));
			}
		}
	}

	// 生成した本文を入力と並べる。入力は架空の発話のため、本文を残してよい
	std::ofstream outputs(out_dir / "outputs.md", std::ios::binary);
	outputs << "# モデルの比較の出力\n\n入力は tools/model-eval/samples/ の架空の発話です。"
		   "概要の各行の末尾の数字は、切り詰める前の文字数です。\n";
	for (const auto &scene : scenes) {
		outputs << "\n## " << scene.title << " (" << scene.file << ")\n\n### 英訳\n\n| # | 入力";
		for (const auto &model : models)
			outputs << " | " << model.name;
		outputs << " |\n| ---: | ---";
		for (size_t m = 0; m < models.size(); ++m)
			outputs << " | ---";
		outputs << " |\n";
		for (size_t i = 0; i < scene.utterances.size(); ++i) {
			outputs << "| " << i << " | " << escape_cell(scene.utterances[i].text);
			for (const auto &model : models) {
				for (const auto &call : calls) {
					if (call.scene == scene.file && call.kind == "translate" &&
					    call.index == static_cast<int>(i) && call.model == model.name) {
						outputs << " | " << escape_cell(call.rendered);
						if (!call.flags.empty())
							outputs << " [" << join(call.flags, ",") << "]";
					}
				}
			}
			outputs << " |\n";
		}

		outputs << "\n### 概要\n\n| 時点の発話 # | 秒";
		for (const auto &model : models)
			outputs << " | " << model.name;
		outputs << " |\n| ---: | ---:";
		for (size_t m = 0; m < models.size(); ++m)
			outputs << " | ---";
		outputs << " |\n";
		for (size_t i = 0; i < scene.utterances.size(); ++i) {
			bool any = false;
			for (const auto &call : calls)
				any = any || (call.scene == scene.file && call.kind == "summary" &&
					      call.index == static_cast<int>(i));
			if (!any)
				continue;
			outputs << "| " << i << " | " << scene.utterances[i].t;
			for (const auto &model : models) {
				for (const auto &call : calls) {
					if (call.scene == scene.file && call.kind == "summary" &&
					    call.index == static_cast<int>(i) && call.model == model.name) {
						outputs << " | " << escape_cell(call.rendered);
						if (!call.flags.empty())
							outputs << " [" << join(call.flags, ",") << "]";
					}
				}
			}
			outputs << " |\n";
		}
	}

	// 集計
	std::ofstream stats(out_dir / "stats.md", std::ios::binary);
	stats << "# モデルの比較の集計\n\n"
	      << "1 時間あたりの費用は、元のツールの実績から換算した英訳 " << kTranslationsPerHour << " 回、概要 "
	      << kSummariesPerHour << " 回と、各モデルの 1 回あたりの平均トークン数から計算しました。\n\n"
	      << "| モデル | 処理 | reasoning.effort | temperature | 件数 | 失敗 | 規則の違反 | 中央値 ms | p95 ms | 最大 ms "
		 "| 平均入力 | 平均出力 | 平均推論 | 1 時間あたり米ドル |\n"
	      << "| --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n";
	char buffer[512];
	for (const auto &model : models) {
		double hourly_total = 0.0;
		for (const char *kind : {"translate", "summary"}) {
			std::vector<double> elapsed;
			int n = 0;
			int failed = 0;
			int violations = 0;
			double in = 0, out = 0, reasoning = 0;
			std::map<std::string, int> flag_counts;
			for (const auto &call : calls) {
				if (call.model != model.name || call.kind != kind)
					continue;
				++n;
				if (!call.result.ok)
					++failed;
				else
					elapsed.push_back(call.result.elapsed_ms);
				for (const auto &f : call.flags) {
					if (f != "failed") {
						++violations;
						++flag_counts[f];
					}
				}
				in += (std::max)(0LL, call.result.input_tokens);
				out += (std::max)(0LL, call.result.output_tokens);
				reasoning += (std::max)(0LL, call.result.reasoning_tokens);
			}
			const double avg_in = n ? in / n : 0.0;
			const double avg_out = n ? out / n : 0.0;
			const double avg_reasoning = n ? reasoning / n : 0.0;
			const double per_call = (avg_in * model.price_input + avg_out * model.price_output) / 1e6;
			const double hourly = per_call * (std::string(kind) == "translate" ? kTranslationsPerHour
											   : kSummariesPerHour);
			hourly_total += hourly;
			std::string flags_text;
			for (const auto &fc : flag_counts)
				flags_text +=
					(flags_text.empty() ? "" : ", ") + fc.first + "=" + std::to_string(fc.second);
			std::snprintf(
				buffer, sizeof(buffer),
				"| %s | %s | %s | %s | %d | %d | %d%s | %.1f | %.1f | %.1f | %.1f | %.1f | %.1f | %.5f |\n",
				model.name.c_str(), kind, model.profile.reasoning_effort.c_str(),
				model.temperature_rejected ? "拒否" : "受付", n, failed, violations,
				flags_text.empty() ? "" : (" (" + flags_text + ")").c_str(), median(elapsed),
				percentile(elapsed, 0.95),
				elapsed.empty() ? 0.0 : *std::max_element(elapsed.begin(), elapsed.end()), avg_in,
				avg_out, avg_reasoning, hourly);
			stats << buffer;
		}
		std::snprintf(buffer, sizeof(buffer), "| %s | 英訳と概要の合計 | | | | | | | | | | | | %.5f |\n",
			      model.name.c_str(), hourly_total);
		stats << buffer;
	}

	curl_global_cleanup();
	std::printf("wrote %s and %s\n", (out_dir / "outputs.md").string().c_str(),
		    (out_dir / "stats.md").string().c_str());
	return 0;
}
