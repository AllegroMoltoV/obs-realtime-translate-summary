#include "caption-router.h"

#include "text-util.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace rts {

namespace {

std::string string_field(const nlohmann::json &object, const char *key)
{
	const auto value = object.find(key);
	return value != object.end() && value->is_string() ? value->get<std::string>() : std::string();
}

} // namespace

std::optional<TranscriptionEvent> parse_transcription_event(const std::string &message)
{
	const nlohmann::json parsed = nlohmann::json::parse(message, nullptr, false);
	if (parsed.is_discarded() || !parsed.is_object())
		return std::nullopt;

	TranscriptionEvent event;
	event.type_name = string_field(parsed, "type");
	event.item_id = string_field(parsed, "item_id");
	const auto previous = parsed.find("previous_item_id");
	if (previous != parsed.end() && previous->is_string())
		event.previous_item_id = previous->get<std::string>();
	event.delta = string_field(parsed, "delta");
	event.transcript = string_field(parsed, "transcript");

	const auto error = parsed.find("error");
	if (error != parsed.end() && error->is_object()) {
		event.error_type = string_field(*error, "type");
		event.error_code = string_field(*error, "code");
		event.error_param = string_field(*error, "param");
	}

	if (event.type_name == "input_audio_buffer.committed")
		event.type = TranscriptionEvent::Type::Committed;
	else if (event.type_name == "conversation.item.input_audio_transcription.delta")
		event.type = TranscriptionEvent::Type::Delta;
	else if (event.type_name == "conversation.item.input_audio_transcription.completed")
		event.type = TranscriptionEvent::Type::Completed;
	else if (event.type_name == "conversation.item.input_audio_transcription.failed")
		event.type = TranscriptionEvent::Type::Failed;
	return event;
}

CaptionRouter::CaptionRouter(CaptionFn on_caption, CompletedFn on_completed, ClockFn clock)
	: on_caption_(std::move(on_caption)),
	  on_completed_(std::move(on_completed)),
	  clock_(clock),
	  turns_(clock)
{
}

TurnError CaptionRouter::handle(const TranscriptionEvent &event)
{
	switch (event.type) {
	case TranscriptionEvent::Type::Committed:
		return turns_.commit(event.item_id, event.previous_item_id);

	case TranscriptionEvent::Type::Delta: {
		const auto sequence = turns_.sequence_for(event.item_id);
		// 表示中より古い発話の差分で、新しい字幕を上書きしない
		if (!sequence || *sequence < latest_displayed_sequence_)
			return TurnError::None;
		std::string &partial = partials_[event.item_id];
		partial += event.delta;
		latest_displayed_sequence_ = *sequence;
		on_caption_(partial);
		return TurnError::None;
	}

	case TranscriptionEvent::Type::Completed: {
		std::vector<CompletedTurn> released;
		const TurnError error = turns_.complete(event.item_id, trim_unicode(event.transcript), released);
		partials_.erase(event.item_id);
		deliver(released);
		return error;
	}

	case TranscriptionEvent::Type::Failed: {
		std::vector<CompletedTurn> released;
		partials_.erase(event.item_id);
		const TurnError error = turns_.fail(event.item_id, released);
		deliver(released);
		return error;
	}

	case TranscriptionEvent::Type::Other:
		return TurnError::None;
	}
	return TurnError::None;
}

CaptionRouter::ResetCounts CaptionRouter::reset_session()
{
	ResetCounts counts;
	counts.pending_items = turns_.pending_count();
	counts.partial_captions = static_cast<int>(partials_.size());
	turns_ = TurnTracker(clock_);
	partials_.clear();
	latest_displayed_sequence_ = -1;
	return counts;
}

void CaptionRouter::deliver(const std::vector<CompletedTurn> &turns)
{
	for (const auto &turn : turns) {
		if (turn.transcript.empty())
			continue;
		latest_displayed_sequence_ = (std::max)(latest_displayed_sequence_, turn.sequence);
		on_caption_(turn.transcript);
		on_completed_(turn);
	}
}

} // namespace rts
