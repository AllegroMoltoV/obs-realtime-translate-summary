#include "api-key-file.h"

#include "text-util.h"

#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dpapi.h>
#endif

namespace fs = std::filesystem;

namespace rts {

namespace {

constexpr const char *kTempSuffix = ".tmp";

#ifdef _WIN32
bool protect(const std::string &plain, std::vector<unsigned char> &out)
{
	DATA_BLOB in;
	in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(plain.data()));
	in.cbData = static_cast<DWORD>(plain.size());
	DATA_BLOB blob = {};
	if (!CryptProtectData(&in, L"realtime-translate-summary", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
			      &blob))
		return false;
	out.assign(blob.pbData, blob.pbData + blob.cbData);
	LocalFree(blob.pbData);
	return true;
}

bool unprotect(const std::vector<unsigned char> &cipher, std::string &out)
{
	DATA_BLOB in;
	in.pbData = const_cast<BYTE *>(cipher.data());
	in.cbData = static_cast<DWORD>(cipher.size());
	DATA_BLOB blob = {};
	if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &blob))
		return false;
	out.assign(reinterpret_cast<const char *>(blob.pbData), blob.cbData);
	LocalFree(blob.pbData);
	return true;
}

// バックアップを作らずに置き換える。ReplaceFileW のバックアップ付きの置き換えは、削除後も旧キーの暗号文を残すため使わない
bool replace_file(const fs::path &from, const fs::path &to)
{
	return MoveFileExW(from.wstring().c_str(), to.wstring().c_str(),
			   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
#endif

} // namespace

bool api_key_storage_supported()
{
#ifdef _WIN32
	return true;
#else
	return false;
#endif
}

std::string normalize_api_key(const std::string &input)
{
	const std::string key = trim_unicode(input);
	if (key.empty() || contains_line_break(key))
		return std::string();
	return key;
}

ApiKeyStatus save_api_key_file(const fs::path &dir, const std::string &key)
{
	const std::string normalized = normalize_api_key(key);
	if (normalized.empty())
		return ApiKeyStatus::InvalidKey;
#ifdef _WIN32
	std::vector<unsigned char> cipher;
	if (!protect(normalized, cipher))
		return ApiKeyStatus::IoError;

	std::error_code ec;
	fs::create_directories(dir, ec);
	if (ec)
		return ApiKeyStatus::IoError;

	const fs::path target = dir / kApiKeyFileName;
	const fs::path temp = dir / (std::string(kApiKeyFileName) + kTempSuffix);
	{
		std::ofstream out(temp, std::ios::binary | std::ios::trunc);
		if (!out)
			return ApiKeyStatus::IoError;
		out.write(reinterpret_cast<const char *>(cipher.data()), static_cast<std::streamsize>(cipher.size()));
		if (!out)
			return ApiKeyStatus::IoError;
	}
	if (!replace_file(temp, target)) {
		fs::remove(temp, ec);
		return ApiKeyStatus::IoError;
	}
	return ApiKeyStatus::Ok;
#else
	(void)dir;
	return ApiKeyStatus::Unsupported;
#endif
}

ApiKeyStatus load_api_key_file(const fs::path &dir, std::string &key_out)
{
	key_out.clear();
#ifdef _WIN32
	const fs::path target = dir / kApiKeyFileName;
	std::error_code ec;
	if (!fs::exists(target, ec))
		return ApiKeyStatus::NotFound;

	std::ifstream in(target, std::ios::binary);
	if (!in)
		return ApiKeyStatus::IoError;
	const std::vector<unsigned char> cipher((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	if (cipher.empty())
		return ApiKeyStatus::DecryptFailed;

	std::string plain;
	if (!unprotect(cipher, plain))
		return ApiKeyStatus::DecryptFailed;
	key_out = normalize_api_key(plain);
	return key_out.empty() ? ApiKeyStatus::DecryptFailed : ApiKeyStatus::Ok;
#else
	(void)dir;
	return ApiKeyStatus::Unsupported;
#endif
}

ApiKeyStatus remove_api_key_file(const fs::path &dir)
{
	std::error_code ec;
	const bool removed = fs::remove(dir / kApiKeyFileName, ec);
	if (ec)
		return ApiKeyStatus::IoError;
	fs::remove(dir / (std::string(kApiKeyFileName) + kTempSuffix), ec);
	return removed ? ApiKeyStatus::Ok : ApiKeyStatus::NotFound;
}

const char *api_key_status_name(ApiKeyStatus status)
{
	switch (status) {
	case ApiKeyStatus::Ok:
		return "ok";
	case ApiKeyStatus::NotFound:
		return "not_found";
	case ApiKeyStatus::InvalidKey:
		return "invalid_key";
	case ApiKeyStatus::DecryptFailed:
		return "decrypt_failed";
	case ApiKeyStatus::IoError:
		return "io_error";
	case ApiKeyStatus::Unsupported:
		return "unsupported";
	}
	return "unknown";
}

} // namespace rts
