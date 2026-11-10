#pragma once

#include <functional>
#include <string>

namespace rts {

// テキストソースの text だけを更新する。フォントや色などの既存の設定は残る。
// OBS のソースの更新は Qt のメインスレッドで行う。作業のスレッドからは run_on_main_thread で移す

void set_source_text(const std::string &source_name, const std::string &text);

// 呼び出し元がメインスレッドならその場で、そうでなければキューに入れて実行する
void run_on_main_thread(std::function<void()> fn);

// 呼び出し元のスレッドによらず、メインスレッドのキューに入れる
void post_to_main_thread(std::function<void()> fn);

bool on_main_thread();

} // namespace rts
