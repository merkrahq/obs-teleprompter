/*
 * OBS Docked Teleprompter — dock widget (phase 002.A scaffold)
 *
 * A native OBS dock, registered with the OBS frontend API, that appears
 * automatically in OBS → Docks once the plugin is installed. This scaffold is
 * intentionally empty of teleprompter logic: the script editor + smooth scroll
 * (002.B) and in-process recording control (002.C) are layered on top of this
 * QWidget later. Its only jobs right now are to build, load cleanly, and show
 * a placeholder so we can confirm the dock auto-appears in OBS.
 */
#pragma once

#include <QWidget>

class TeleprompterDock : public QWidget {
	Q_OBJECT

public:
	explicit TeleprompterDock(QWidget *parent = nullptr);
	~TeleprompterDock() override = default;
};
