#include "start-check.h"

#include "summary-parser.h"

#include <cmath>
#include <set>

namespace rts {

std::vector<StartProblem> check_start(const StartInputs &inputs)
{
	std::vector<StartProblem> problems;
	if (!inputs.api_key_saved)
		problems.push_back(StartProblem::ApiKeyMissing);

	if (inputs.audio_source.empty())
		problems.push_back(StartProblem::AudioSourceNotSelected);
	else if (!inputs.audio_source_exists)
		problems.push_back(StartProblem::AudioSourceMissing);

	bool any_target = false;
	bool missing = false;
	bool not_text = false;
	bool duplicate = false;
	std::set<std::string> seen;
	for (const auto &target : inputs.targets) {
		if (target.name.empty())
			continue;
		any_target = true;
		if (!seen.insert(target.name).second)
			duplicate = true;
		if (!target.exists)
			missing = true;
		else if (!target.is_text)
			not_text = true;
	}
	if (!any_target)
		problems.push_back(StartProblem::NoTargetSelected);
	if (missing)
		problems.push_back(StartProblem::TargetMissing);
	if (not_text)
		problems.push_back(StartProblem::TargetNotText);
	if (duplicate)
		problems.push_back(StartProblem::DuplicateTarget);
	return problems;
}

bool is_supported_text_source_id(const std::string &unversioned_id)
{
	return unversioned_id == "text_gdiplus" || unversioned_id == "text_ft2_source";
}

bool valid_summary_max_chars(int value)
{
	return value >= kMinSummaryMaxChars && value <= kMaxSummaryMaxChars;
}

bool valid_summary_interval(double seconds)
{
	return std::isfinite(seconds) && seconds > 0.0;
}

bool valid_auto_stop_minutes(int minutes)
{
	return minutes >= kMinAutoStopMinutes && minutes <= kMaxAutoStopMinutes;
}

bool auto_stop_due(bool enabled, int minutes, double elapsed_seconds)
{
	return enabled && valid_auto_stop_minutes(minutes) && elapsed_seconds >= minutes * 60.0;
}

} // namespace rts
