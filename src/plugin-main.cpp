/*
 * OBS Docked Teleprompter — plugin entry point (phase 002.A scaffold)
 *
 * Registers a native Qt dock with OBS via the frontend API so the teleprompter
 * appears automatically in OBS → Docks once the plugin .so is installed — no
 * Custom Browser Dock, no URL, no WebSocket config (decision
 * `obs-plugin-native-installer`).
 *
 * Dock registration must happen on the Qt UI thread with the OBS main window
 * available. obs_module_load() runs before the frontend UI is fully up, so we
 * register the dock in an OBS_FRONTEND_EVENT_FINISHED_LOADING callback — the
 * standard, documented pattern for frontend docks.
 */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/base.h>

#include "teleprompter-dock.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-teleprompter", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Docked teleprompter with in-process recording control";
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return "OBS Docked Teleprompter";
}

static const char *kDockId = "obs_teleprompter_dock";
static const char *kDockTitle = "Teleprompter";

static void register_dock(void)
{
	// Ownership: obs_frontend_add_dock_by_id() takes ownership of the widget
	// and parents it to the dock host, so we do not delete it ourselves.
	auto *dock = new TeleprompterDock();
	if (!obs_frontend_add_dock_by_id(kDockId, kDockTitle, dock)) {
		blog(LOG_WARNING,
		     "[obs-teleprompter] failed to register dock '%s'",
		     kDockId);
		delete dock;
		return;
	}
	blog(LOG_INFO, "[obs-teleprompter] dock '%s' registered", kDockId);
}

static void on_frontend_event(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		register_dock();
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
		// Confirmed record start — the dock gates its scroll on this
		// (reliability-first; port of the 724aa91 race fix's intent).
		if (auto *d = TeleprompterDock::instance())
			d->onRecordingStarted();
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
		if (auto *d = TeleprompterDock::instance())
			d->onRecordingStopped();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		obs_frontend_remove_dock(kDockId);
		break;
	default:
		break;
	}
}

bool obs_module_load(void)
{
	blog(LOG_INFO, "[obs-teleprompter] loaded (version %s)",
	     PLUGIN_VERSION);
	obs_frontend_add_event_callback(on_frontend_event, nullptr);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);
	blog(LOG_INFO, "[obs-teleprompter] unloaded");
}
