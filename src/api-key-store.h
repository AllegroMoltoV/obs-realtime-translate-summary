#pragma once

#include "core/api-key-file.h"

#include <string>

namespace rts {

// プラグインの設定フォルダー (obs_module_config_path) に API キーを保存する。
// キーの値はログに書かず、失敗の種類だけを書く
bool api_key_saved();
ApiKeyStatus load_saved_api_key(std::string &key_out);
ApiKeyStatus save_api_key(const std::string &key);
ApiKeyStatus delete_saved_api_key();

} // namespace rts
