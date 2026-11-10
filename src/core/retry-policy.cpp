#include "retry-policy.h"

#include <algorithm>
#include <cmath>

namespace rts {

double RetryPolicy::delay_seconds(int attempt, double jitter) const
{
	const int exponent = (std::max)(0, attempt - 1);
	const double ceiling = (std::min)(max_delay_seconds, base_delay_seconds * std::pow(2.0, exponent));
	const double clamped = (std::min)(1.0, (std::max)(0.0, jitter));
	return ceiling * (0.5 + 0.5 * clamped);
}

bool RetryCounter::on_disconnect(double connected_seconds)
{
	if (connected_seconds >= policy_.stable_connection_seconds)
		retries_ = 0;
	if (retries_ >= policy_.max_retries)
		return false;
	++retries_;
	return true;
}

} // namespace rts
