#include "text-output.h"

#include <obs.h>

#include <QCoreApplication>
#include <QMetaObject>
#include <QThread>

namespace rts {

void set_source_text(const std::string &source_name, const std::string &text)
{
	if (source_name.empty())
		return;
	obs_source_t *source = obs_get_source_by_name(source_name.c_str());
	if (!source)
		return;
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "text", text.c_str());
	obs_source_update(source, settings);
	obs_data_release(settings);
	obs_source_release(source);
}

bool on_main_thread()
{
	auto *app = QCoreApplication::instance();
	return app && QThread::currentThread() == app->thread();
}

void run_on_main_thread(std::function<void()> fn)
{
	auto *app = QCoreApplication::instance();
	if (!app)
		return;
	if (on_main_thread()) {
		fn();
		return;
	}
	QMetaObject::invokeMethod(app, std::move(fn), Qt::QueuedConnection);
}

void post_to_main_thread(std::function<void()> fn)
{
	if (auto *app = QCoreApplication::instance())
		QMetaObject::invokeMethod(app, std::move(fn), Qt::QueuedConnection);
}

} // namespace rts
