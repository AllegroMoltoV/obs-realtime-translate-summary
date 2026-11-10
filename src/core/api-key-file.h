#pragma once

#include <filesystem>
#include <string>

namespace rts {

// API キーを OS の機能で暗号化してファイルに保存する。libobs に依存させず、保存先のフォルダーを引数に取る。
// Windows では DPAPI (CryptProtectData) を使う。ほかの OS では未対応を返す
enum class ApiKeyStatus {
	Ok,
	NotFound,      // 保存されていない
	InvalidKey,    // 空、または改行を含む
	DecryptFailed, // ファイルの破損や、別の利用者・PC で暗号化されたファイル
	IoError,
	Unsupported,
};

constexpr const char *kApiKeyFileName = "api-key.bin";

bool api_key_storage_supported();

// 前後の空白を除いたキーを返す。空や改行を含む場合は空を返す
std::string normalize_api_key(const std::string &input);

ApiKeyStatus save_api_key_file(const std::filesystem::path &dir, const std::string &key);
ApiKeyStatus load_api_key_file(const std::filesystem::path &dir, std::string &key_out);
// 一時ファイルの残りも消す
ApiKeyStatus remove_api_key_file(const std::filesystem::path &dir);

const char *api_key_status_name(ApiKeyStatus status);

} // namespace rts
