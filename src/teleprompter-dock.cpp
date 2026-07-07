/*
 * OBS Docked Teleprompter — dock widget implementation (phase 002.A scaffold)
 *
 * Placeholder UI only. Ports of the index.html teleprompter (editor, scroll,
 * countdown, controls) land in 002.B; recording wiring in 002.C.
 */
#include "teleprompter-dock.hpp"

#include <QLabel>
#include <QVBoxLayout>

TeleprompterDock::TeleprompterDock(QWidget *parent) : QWidget(parent)
{
	setObjectName(QStringLiteral("TeleprompterDockRoot"));

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(12, 12, 12, 12);

	auto *title = new QLabel(QStringLiteral("Teleprompter"), this);
	QFont titleFont = title->font();
	titleFont.setPointSize(titleFont.pointSize() + 2);
	titleFont.setBold(true);
	title->setFont(titleFont);

	auto *hint = new QLabel(
		QStringLiteral(
			"Native OBS plugin scaffold loaded.\n"
			"Script editor, scrolling, and recording arrive next (002.B / 002.C)."),
		this);
	hint->setWordWrap(true);

	layout->addWidget(title);
	layout->addWidget(hint);
	layout->addStretch(1);
}
