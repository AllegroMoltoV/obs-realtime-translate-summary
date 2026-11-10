// API キーの保存の単体テスト。Windows の DPAPI を使い、一時フォルダーだけに書く。本物の plugin_config には触れない

#include "core/api-key-file.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace rts;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                    \
	do {                                                                           \
		++g_checks;                                                            \
		if (!(cond)) {                                                         \
			++g_failures;                                                  \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
		}                                                                      \
	} while (0)

// 存在しないフォルダーのパスを返す。保存がフォルダーを作ることも確かめるため
fs::path fresh_dir()
{
	const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	return fs::temp_directory_path() / ("rts-api-key-test-" + std::to_string(stamp)) / "plugin_config";
}

size_t file_count(const fs::path &dir)
{
	size_t count = 0;
	std::error_code ec;
	for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); ++it)
		++count;
	return count;
}

const std::string kKey = "sk-test-0123456789abcdef";

void test_round_trip_creates_directory()
{
	const fs::path dir = fresh_dir();
	CHECK(!fs::exists(dir));
	CHECK(save_api_key_file(dir, "  " + kKey + "\t") == ApiKeyStatus::Ok);
	std::string loaded;
	CHECK(load_api_key_file(dir, loaded) == ApiKeyStatus::Ok);
	CHECK(loaded == kKey);

	// 暗号化したファイルに平文のキーが含まれない
	std::ifstream in(dir / kApiKeyFileName, std::ios::binary);
	const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	CHECK(!bytes.empty());
	CHECK(bytes.find(kKey) == std::string::npos);
	in.close();

	// 上書きでもバックアップと一時ファイルが残らない
	CHECK(save_api_key_file(dir, kKey + "-2") == ApiKeyStatus::Ok);
	CHECK(file_count(dir) == 1);
	CHECK(load_api_key_file(dir, loaded) == ApiKeyStatus::Ok && loaded == kKey + "-2");
	fs::remove_all(dir.parent_path());
}

void test_delete_leaves_nothing()
{
	const fs::path dir = fresh_dir();
	CHECK(save_api_key_file(dir, kKey) == ApiKeyStatus::Ok);
	std::ofstream(dir / (std::string(kApiKeyFileName) + ".tmp")) << "leftover";
	CHECK(remove_api_key_file(dir) == ApiKeyStatus::Ok);
	CHECK(file_count(dir) == 0);
	std::string loaded;
	CHECK(load_api_key_file(dir, loaded) == ApiKeyStatus::NotFound);
	CHECK(remove_api_key_file(dir) == ApiKeyStatus::NotFound);
	fs::remove_all(dir.parent_path());
}

void test_corrupted_file_is_treated_as_unset()
{
	const fs::path dir = fresh_dir();
	fs::create_directories(dir);
	std::ofstream(dir / kApiKeyFileName, std::ios::binary) << "not a dpapi blob";
	std::string loaded = "previous";
	CHECK(load_api_key_file(dir, loaded) == ApiKeyStatus::DecryptFailed);
	CHECK(loaded.empty());
	fs::remove_all(dir.parent_path());
}

void test_invalid_keys_are_rejected()
{
	const fs::path dir = fresh_dir();
	CHECK(save_api_key_file(dir, "") == ApiKeyStatus::InvalidKey);
	CHECK(save_api_key_file(dir, "   ") == ApiKeyStatus::InvalidKey);
	CHECK(save_api_key_file(dir, "sk-a\nsk-b") == ApiKeyStatus::InvalidKey);
	CHECK(!fs::exists(dir / kApiKeyFileName));
	std::error_code ec;
	fs::remove_all(dir.parent_path(), ec);
}

} // namespace

int main()
{
	const struct {
		const char *name;
		void (*fn)();
	} tests[] = {
		{"round_trip_creates_directory", test_round_trip_creates_directory},
		{"delete_leaves_nothing", test_delete_leaves_nothing},
		{"corrupted_file_is_treated_as_unset", test_corrupted_file_is_treated_as_unset},
		{"invalid_keys_are_rejected", test_invalid_keys_are_rejected},
	};
	for (const auto &test : tests) {
		const int before = g_failures;
		test.fn();
		std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", test.name);
	}
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
