#include "api-key-store.h"

#include <obs-module.h>
#include <plugin-support.h>

#include <filesystem>

namespace rts {

namespace {

std::filesystem::path config_dir()
{
	// obs_module_config_path はパスを組み立てるだけでフォルダーを作らない。作成は保存の側で行う
	char *path = obs_module_config_path("");
	if (!path)
		return std::filesystem::path();
	const std::filesystem::path dir = std::filesystem::u8path(path);
	bfree(path);
	return dir;
}

ApiKeyStatus logged(const char *operation, ApiKeyStatus status)
{
	if (status != ApiKeyStatus::Ok && status != ApiKeyStatus::NotFound)
		obs_log(LOG_WARNING, "api key %s failed: %s", operation, api_key_status_name(status));
	return status;
}

} // namespace

bool api_key_saved()
{
	std::string key;
	return load_saved_api_key(key) == ApiKeyStatus::Ok;
}

ApiKeyStatus load_saved_api_key(std::string &key_out)
{
	const auto dir = config_dir();
	if (dir.empty())
		return logged("load", ApiKeyStatus::IoError);
	return logged("load", load_api_key_file(dir, key_out));
}

ApiKeyStatus save_api_key(const std::string &key)
{
	const auto dir = config_dir();
	if (dir.empty())
		return logged("save", ApiKeyStatus::IoError);
	const ApiKeyStatus status = save_api_key_file(dir, key);
	if (status == ApiKeyStatus::Ok)
		obs_log(LOG_INFO, "api key saved");
	return logged("save", status);
}

ApiKeyStatus delete_saved_api_key()
{
	const auto dir = config_dir();
	if (dir.empty())
		return logged("delete", ApiKeyStatus::IoError);
	const ApiKeyStatus status = remove_api_key_file(dir);
	if (status == ApiKeyStatus::Ok)
		obs_log(LOG_INFO, "api key deleted");
	return logged("delete", status);
}

} // namespace rts
