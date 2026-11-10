#pragma once

namespace rts {

// 文字起こしの接続の再接続の規則。元のツールの transcription.py の RetryPolicy を移した
struct RetryPolicy {
	int max_retries = 5;
	double base_delay_seconds = 1.0;
	double max_delay_seconds = 16.0;
	double stable_connection_seconds = 60.0;

	// attempt は 1 から数える。jitter は 0 以上 1 以下で、待機は上限の 0.5 から 1.0 倍になる
	double delay_seconds(int attempt, double jitter) const;
};

// 切断のたびに呼び、連続の再試行の回数を進める
class RetryCounter {
public:
	explicit RetryCounter(RetryPolicy policy = RetryPolicy()) : policy_(policy) {}

	// connected_seconds はその接続が続いた秒数。接続に失敗した場合は 0 を渡す。
	// 安定した接続のあとの切断では回数を 0 に戻してから数える。再試行できるなら true を返す
	bool on_disconnect(double connected_seconds);

	int retries() const { return retries_; }
	const RetryPolicy &policy() const { return policy_; }

private:
	RetryPolicy policy_;
	int retries_ = 0;
};

} // namespace rts
