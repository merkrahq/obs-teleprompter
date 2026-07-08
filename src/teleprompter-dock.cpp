/*
 * OBS Docked Teleprompter — dock implementation (phases 002.B + 002.C)
 *
 * Ports the index.html teleprompter (editor, sub-pixel smooth scroll, animated
 * countdown, Start/Pause/Stop/Reset, keyboard shortcuts, dark theme, center
 * guide) into Qt widgets, and wires recording in-process via the OBS frontend
 * API. See teleprompter-dock.hpp for the design overview.
 */
#include "teleprompter-dock.hpp"

#include <obs-frontend-api.h>

#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPen>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QPolygonF>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSlider>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

// obs_module_config_path() lives in obs-module.h; obs-frontend-api.h does not
// pull it in.
#include <obs-module.h>

// ── colour palette (ports the index.html :root variables) ───────────────────
namespace {
const char *kBg = "#0b0d10";
const char *kPanel = "#14181d";
const char *kPanel2 = "#1b2027";
const char *kBorder = "#2a323c";
const char *kText = "#f2f5f8";
const char *kMuted = "#8a97a6";
// White-only UI (TODO 00008.b): every control accent is white/neutral — no
// blue anywhere. kAccent was the old #3ea6ff blue; repointing it to the bright
// text white turns the value readouts, slider handles, hover/active borders and
// the centre-guide line/carets white in one place. The recording indicator
// (kBad, a status signal not a control) keeps its own colour.
const char *kAccent = "#f2f5f8";
const char *kBad = "#ff5b5b";

TeleprompterDock *g_instance = nullptr;

enum class TransportIcon { Play, Pause, Stop, Reset };

QIcon makeTransportIcon(TransportIcon icon)
{
	QPixmap pixmap(18, 18);
	pixmap.fill(Qt::transparent);

	QPainter p(&pixmap);
	p.setRenderHint(QPainter::Antialiasing, true);
	p.setBrush(QColor(kText));

	if (icon == TransportIcon::Play) {
		p.setPen(Qt::NoPen);
		p.drawPolygon(QPolygonF({QPointF(6, 4), QPointF(14, 9),
					 QPointF(6, 14)}));
	} else if (icon == TransportIcon::Pause) {
		p.setPen(Qt::NoPen);
		p.drawRoundedRect(QRectF(5, 4, 3.5, 10), 1.2, 1.2);
		p.drawRoundedRect(QRectF(10, 4, 3.5, 10), 1.2, 1.2);
	} else if (icon == TransportIcon::Stop) {
		p.setPen(Qt::NoPen);
		p.drawRoundedRect(QRectF(5, 5, 8, 8), 1.0, 1.0);
	} else {
		QPen pen(QColor(kText), 2.0, Qt::SolidLine, Qt::RoundCap,
			 Qt::RoundJoin);
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		p.drawArc(QRectF(4, 4, 10, 10), 30 * 16, 285 * 16);
		p.drawLine(QPointF(4.8, 5.5), QPointF(4.8, 2.4));
		p.drawLine(QPointF(4.8, 5.5), QPointF(7.9, 5.5));
	}

	return QIcon(pixmap);
}

// Font-derived baseline scroll speed. We hold LINES PER SECOND constant so the
// perceived reading cadence stays roughly the same across font sizes: a smaller
// font scrolls proportionally slower. kLinesPerSec is chosen so the historical
// defaults (48px font, 1.5 line-height) yield the historical ~60 px/s baseline
// (48 * 1.5 * 0.8333 ≈ 60). The speed slider multiplies this baseline.
constexpr double kLinesPerSec = 0.83333;
double fontBaseSpeed(int fontPx, double lineHeight)
{
	return double(fontPx) * lineHeight * kLinesPerSec;
}

// Convert a "#rrggbb" palette colour + an alpha (0..1) into a Qt-stylesheet
// "rgba(r,g,b,a)" string, so the dock chrome can be painted translucent for the
// docked-state opacity (TODO 00006.b). Alpha is the 0..255 form Qt expects.
QString rgbaFill(const char *hex, double alpha)
{
	const QColor c(hex);
	return QStringLiteral("rgba(%1,%2,%3,%4)")
		.arg(c.red())
		.arg(c.green())
		.arg(c.blue())
		.arg(int(qRound(qBound(0.0, alpha, 1.0) * 255)));
}
} // namespace

// ────────────────────────────────────────────────────────────────────────────
// PrompterStage — the scrolling prompter viewport.
//
// A QScrollArea would work, but a purpose-built widget lets us: keep a black
// stage, paint the optional center guide as an overlay, add the big top/bottom
// padding (index.html's `padding: 55vh 6vw`) so the first/last lines can reach
// the vertical center, and expose a clean double-precision scroll offset that
// the dock's tick() drives directly.
// ────────────────────────────────────────────────────────────────────────────
class PrompterStage : public QWidget {
public:
	explicit PrompterStage(QWidget *parent = nullptr) : QWidget(parent)
	{
		setAutoFillBackground(true);
		QPalette pal = palette();
		pal.setColor(QPalette::Window, QColor("#000000"));
		setPalette(pal);

		m_label = new QLabel(this);
		m_label->setWordWrap(true);
		m_label->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
		// Rich text so we can apply CSS line-height (QLabel/QFont has no
		// line-height); font size + colour also come from the HTML.
		m_label->setTextFormat(Qt::RichText);
		m_label->setAttribute(Qt::WA_TransparentForMouseEvents);
	}

	// Set prompter text + whether it is the empty-placeholder state (muted).
	void setContent(const QString &text, bool placeholder)
	{
		m_text = text;
		m_placeholder = placeholder;
		rebuildHtml();
	}

	void setFontPx(int px, double lineHeight)
	{
		m_fontPx = px;
		m_lineSpacing = lineHeight;
		rebuildHtml();
	}

	void setGuide(bool on)
	{
		m_guide = on;
		update();
	}

	// Vertical padding so first/last lines can reach centre. index.html uses
	// 55vh; we mirror that with ~55% of the viewport height.
	int topPad() const { return int(height() * 0.55); }

	// Total scrollable content height (label height + top & bottom pad).
	int contentHeight() const
	{
		return m_label->height() + 2 * topPad();
	}

	double maxOffset() const
	{
		return qMax(0, contentHeight() - height());
	}

	void setOffset(double off)
	{
		m_offset = qBound(0.0, off, maxOffset());
		positionLabel();
	}
	double offset() const { return m_offset; }

protected:
	void resizeEvent(QResizeEvent *) override { relayout(); }

	void paintEvent(QPaintEvent *) override
	{
		if (!m_guide)
			return;
		QPainter p(this);
		const int y = height() / 2;
		QColor line(kAccent);
		line.setAlphaF(0.55);
		p.setPen(QPen(line, 2));
		p.drawLine(0, y, width(), y);
		// left/right caret triangles
		p.setBrush(QColor(kAccent));
		p.setPen(Qt::NoPen);
		const int s = 8;
		p.drawPolygon(QPolygon()
			      << QPoint(0, y - s) << QPoint(s, y)
			      << QPoint(0, y + s));
		p.drawPolygon(QPolygon()
			      << QPoint(width(), y - s)
			      << QPoint(width() - s, y)
			      << QPoint(width(), y + s));
	}

private:
	// Build the prompter HTML: font size, weight, colour, centering, and a
	// per-line line-height (Qt honours `line-height` in rich text blocks).
	void rebuildHtml()
	{
		const QString colour =
			m_placeholder ? QString(kMuted) : QStringLiteral("#ffffff");
		const int weight = m_placeholder ? 500 : 700;
		const int lhPct = int(m_lineSpacing * 100);
		QString body = m_text.toHtmlEscaped();
		body.replace('\n', QStringLiteral("<br/>"));
		m_label->setText(
			QStringLiteral(
				"<div style='font-size:%1px; font-weight:%2; "
				"color:%3; line-height:%4%; text-align:center; "
				"letter-spacing:0.3px;'>%5</div>")
				.arg(m_fontPx)
				.arg(weight)
				.arg(colour)
				.arg(lhPct)
				.arg(body));
		relayout();
	}

	void relayout()
	{
		const int sideMargin = int(width() * 0.06); // ~6vw
		const int w = qMax(1, width() - 2 * sideMargin);
		m_label->setFixedWidth(w);
		// height driven by content at that width
		m_label->adjustSize();
		positionLabel();
		update();
	}

	void positionLabel()
	{
		const int sideMargin = int(width() * 0.06);
		m_label->move(sideMargin, int(topPad() - m_offset));
	}

	QLabel *m_label = nullptr;
	QString m_text;
	bool m_placeholder = true;
	int m_fontPx = 48;
	double m_offset = 0.0;
	double m_lineSpacing = 1.5;
	bool m_guide = false;
};

// ────────────────────────────────────────────────────────────────────────────
// TeleprompterDock
// ────────────────────────────────────────────────────────────────────────────
TeleprompterDock *TeleprompterDock::instance()
{
	return g_instance;
}

TeleprompterDock::TeleprompterDock(QWidget *parent) : QWidget(parent)
{
	g_instance = this;
	setObjectName(QStringLiteral("TeleprompterDockRoot"));

	loadSettings();
	buildUi();
	applyStyle();
	wireSignals();

	// timers
	m_scrollTimer = new QTimer(this);
	m_scrollTimer->setTimerType(Qt::PreciseTimer);
	m_scrollTimer->setInterval(16); // ~60 Hz
	connect(m_scrollTimer, &QTimer::timeout, this,
		&TeleprompterDock::tick);
	m_frameClock = new QElapsedTimer();

	m_countdownTimer = new QTimer(this);
	m_countdownTimer->setInterval(1000);
	connect(m_countdownTimer, &QTimer::timeout, this,
		&TeleprompterDock::countdownStep);

	m_saveTimer = new QTimer(this);
	m_saveTimer->setSingleShot(true);
	m_saveTimer->setInterval(300);
	connect(m_saveTimer, &QTimer::timeout, this,
		&TeleprompterDock::saveSettingsNow);

	m_flashTimer = new QTimer(this);
	m_flashTimer->setSingleShot(true);
	m_flashTimer->setInterval(3500);
	connect(m_flashTimer, &QTimer::timeout, this,
		[this]() { m_statusMsg->setText(QString()); });

	// reflect loaded settings into the widgets + prompter
	m_editor->setPlainText(m_script);
	m_fontSlider->setValue(m_fontSize);
	m_speedSlider->setValue(int(m_speedMult * 100));
	m_lineSlider->setValue(int(m_lineHeight * 100));
	m_opacitySlider->setValue(int(qRound(m_opacity * 100)));
	m_countdownCombo->setCurrentIndex(
		m_countdownCombo->findData(m_countdownLen));
	m_guideCheck->setChecked(m_guide);
	m_autoRecordCheck->setChecked(m_autoRecord);
	setSettingsOpen(m_settingsOpen, false);
	setEditorOpen(m_editorOpen, false);

	m_fontVal->setText(QString::number(m_fontSize) + "px");
	m_lineVal->setText(QString::number(m_lineHeight, 'f', 2));
	applyScrollSpeed(); // derives m_speed + sets m_speedVal from font/mult
	applyOpacity();     // reflects loaded opacity into chrome + readout

	applyPrompterFont();
	m_stage->setGuide(m_guide);
	renderPrompter();
	updateReadTime();

	// reflect an already-active OBS recording in the indicator
	setRecordingIndicator(obs_frontend_recording_active());

	setControls(Mode::Idle);

	// keyboard shortcuts: dock-wide filter
	qApp->installEventFilter(this);
}

TeleprompterDock::~TeleprompterDock()
{
	saveSettingsNow();
	qApp->removeEventFilter(this);
	if (m_editorPanel)
		m_editorPanel->hide();
	if (m_settingsPanel)
		m_settingsPanel->hide();
	if (m_frameClock) {
		delete m_frameClock;
		m_frameClock = nullptr;
	}
	if (g_instance == this)
		g_instance = nullptr;
}

// ── UI construction ─────────────────────────────────────────────────────────
static QPushButton *mkButton(const QString &text, const QString &tip)
{
	auto *b = new QPushButton(text);
	b->setToolTip(tip);
	b->setCursor(Qt::PointingHandCursor);
	b->setIconSize(QSize(16, 16));
	b->setMinimumHeight(34);
	b->setMinimumWidth(82);
	return b;
}

void TeleprompterDock::buildUi()
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(0, 0, 0, 0);
	root->setSpacing(0);

	// ── prompter stage (+ countdown overlay) ──
	// The stage lives at the TOP of the dock (TODO 00004): the reading text
	// wants to sit nearest a camera mounted above the screen, so everything
	// else (controls, settings, editor, status) is placed below it.
	m_stage = new PrompterStage();
	m_stage->setMinimumHeight(160);
	m_countdownOverlay = new QLabel(m_stage);
	m_countdownOverlay->setAlignment(Qt::AlignCenter);
	m_countdownOverlay->setObjectName("countdown");
	m_countdownOverlay->setVisible(false);
	m_countdownOverlay->setAttribute(Qt::WA_TransparentForMouseEvents);
	root->addWidget(m_stage, 1);

	// ── controls bar ──
	auto *bar = new QWidget();
	bar->setObjectName("bar");
	auto *barLay = new QHBoxLayout(bar);
	barLay->setContentsMargins(8, 8, 8, 8);
	barLay->setSpacing(6);

	// Transport controls use painted QIcons with a fixed iconSize (TODO 00008
	// item #6). This avoids Windows font/emoji glyph metrics entirely; the pause
	// bars now have the same stable 16x16 box as neighbouring transport icons.
	m_startBtn = mkButton(QStringLiteral("Start"),
			      QStringLiteral("Start (Ctrl+Enter)"));
	m_startBtn->setIcon(makeTransportIcon(TransportIcon::Play));
	m_pauseBtn = mkButton(QStringLiteral("Pause"),
			      QStringLiteral("Pause / Resume (Space)"));
	m_pauseBtn->setIcon(makeTransportIcon(TransportIcon::Pause));
	m_stopBtn = mkButton(QStringLiteral("Stop"),
			     QStringLiteral("Stop (Esc)"));
	m_stopBtn->setIcon(makeTransportIcon(TransportIcon::Stop));
	m_resetBtn = mkButton(QStringLiteral("Reset"),
			      QStringLiteral("Reset to top"));
	m_resetBtn->setIcon(makeTransportIcon(TransportIcon::Reset));
	m_scriptToggle = mkButton(QStringLiteral("✎ Script"),
				  QStringLiteral("Open script editor"));
	m_settingsToggle = mkButton(QStringLiteral("⚙ Settings"),
				    QStringLiteral("Open settings"));

	barLay->addWidget(m_startBtn);
	barLay->addWidget(m_pauseBtn);
	barLay->addWidget(m_stopBtn);
	barLay->addWidget(m_resetBtn);
	barLay->addStretch(1);
	barLay->addWidget(m_scriptToggle);
	barLay->addWidget(m_settingsToggle);
	root->addWidget(bar);

	// ── settings tool window ──
	const Qt::WindowFlags popupFlags = Qt::Tool | Qt::WindowTitleHint |
					  Qt::WindowCloseButtonHint;
	m_settingsPanel = new QFrame(this, popupFlags);
	m_settingsPanel->setObjectName("panel");
	m_settingsPanel->setWindowTitle(QStringLiteral("Teleprompter Settings"));
	m_settingsPanel->setAttribute(Qt::WA_QuitOnClose, false);
	m_settingsPanel->installEventFilter(this);
	auto *setLay = new QVBoxLayout(m_settingsPanel);
	setLay->setContentsMargins(10, 10, 10, 10);
	setLay->setSpacing(10);

	auto mkFieldLabel = [](const QString &t) {
		auto *l = new QLabel(t);
		l->setObjectName("fieldLabel");
		return l;
	};

	// font size
	{
		auto *row = new QHBoxLayout();
		auto *lab = mkFieldLabel(QStringLiteral("Font size"));
		m_fontVal = new QLabel(QStringLiteral("48px"));
		m_fontVal->setObjectName("val");
		row->addWidget(lab);
		row->addStretch(1);
		row->addWidget(m_fontVal);
		setLay->addLayout(row);
		m_fontSlider = new QSlider(Qt::Horizontal);
		m_fontSlider->setRange(12, 120);
		setLay->addWidget(m_fontSlider);
	}
	// scroll speed
	{
		auto *row = new QHBoxLayout();
		// Speed slider is a MULTIPLIER on the font-derived baseline
		// (0.25×–3.0×, stored /100). The readout shows the effective px/s
		// and the multiplier so the coupling is legible.
		auto *lab = mkFieldLabel(QStringLiteral("Scroll speed"));
		m_speedVal = new QLabel(QStringLiteral("60 px/s"));
		m_speedVal->setObjectName("val");
		row->addWidget(lab);
		row->addStretch(1);
		row->addWidget(m_speedVal);
		setLay->addLayout(row);
		m_speedSlider = new QSlider(Qt::Horizontal);
		m_speedSlider->setRange(25, 300); // /100 → 0.25×–3.0×
		m_speedSlider->setSingleStep(5);
		setLay->addWidget(m_speedSlider);
	}
	// line height
	{
		auto *row = new QHBoxLayout();
		auto *lab = mkFieldLabel(QStringLiteral("Line height"));
		m_lineVal = new QLabel(QStringLiteral("1.50"));
		m_lineVal->setObjectName("val");
		row->addWidget(lab);
		row->addStretch(1);
		row->addWidget(m_lineVal);
		setLay->addLayout(row);
		m_lineSlider = new QSlider(Qt::Horizontal);
		m_lineSlider->setRange(50, 300); // /100 → 0.50–3.0 (00008.a)
		m_lineSlider->setSingleStep(5);
		setLay->addWidget(m_lineSlider);
	}
	// dock opacity (TODO 00006.b): translucent dock background. Real
	// see-through when the dock is FLOATED (setWindowOpacity); while docked
	// inside OBS it degrades to a translucent chrome fill (see applyOpacity).
	{
		auto *row = new QHBoxLayout();
		auto *lab = mkFieldLabel(QStringLiteral("Dock opacity"));
		m_opacityVal = new QLabel(QStringLiteral("100%"));
		m_opacityVal->setObjectName("val");
		row->addWidget(lab);
		row->addStretch(1);
		row->addWidget(m_opacityVal);
		setLay->addLayout(row);
		m_opacitySlider = new QSlider(Qt::Horizontal);
		m_opacitySlider->setRange(30, 100); // /100 → 0.30–1.00
		m_opacitySlider->setSingleStep(5);
		setLay->addWidget(m_opacitySlider);
		auto *hint = new QLabel(QStringLiteral(
			"Full see-through requires floating the dock"));
		hint->setObjectName("sub");
		setLay->addWidget(hint);
	}
	// countdown
	{
		auto *row = new QHBoxLayout();
		row->addWidget(mkFieldLabel(QStringLiteral("Countdown")));
		m_countdownCombo = new QComboBox();
		m_countdownCombo->addItem(QStringLiteral("None"), 0);
		m_countdownCombo->addItem(QStringLiteral("3 seconds"), 3);
		m_countdownCombo->addItem(QStringLiteral("5 seconds"), 5);
		m_countdownCombo->addItem(QStringLiteral("10 seconds"), 10);
		row->addStretch(1);
		row->addWidget(m_countdownCombo);
		setLay->addLayout(row);
	}
	// guide + read time
	{
		m_guideCheck = new QCheckBox(QStringLiteral("Center guide line"));
		setLay->addWidget(m_guideCheck);
		m_autoRecordCheck = new QCheckBox(
			QStringLiteral("Auto-start OBS recording"));
		setLay->addWidget(m_autoRecordCheck);
		m_readTime = new QLabel(QStringLiteral("~0:00 reading time"));
		m_readTime->setObjectName("sub");
		setLay->addWidget(m_readTime);
	}
	m_settingsPanel->setVisible(false);
	m_settingsPanel->resize(380, 520);

	// ── script editor tool window ──
	m_editorPanel = new QFrame(this, popupFlags);
	m_editorPanel->setObjectName("panel");
	m_editorPanel->setWindowTitle(QStringLiteral("Teleprompter Script"));
	m_editorPanel->setAttribute(Qt::WA_QuitOnClose, false);
	m_editorPanel->installEventFilter(this);
	auto *edLay = new QVBoxLayout(m_editorPanel);
	edLay->setContentsMargins(10, 10, 10, 10);
	m_editor = new QPlainTextEdit();
	m_editor->setPlaceholderText(
		QStringLiteral("Paste or type your script here..."));
	m_editor->setMinimumHeight(120);
	edLay->addWidget(m_editor);
	m_editorPanel->setVisible(false);
	m_editorPanel->resize(560, 360);

	// ── status bar ──
	auto *status = new QWidget();
	status->setObjectName("status");
	auto *stLay = new QHBoxLayout(status);
	stLay->setContentsMargins(10, 7, 10, 7);
	stLay->setSpacing(10);
	m_recDot = new QLabel();
	m_recDot->setObjectName("dot");
	m_recDot->setFixedSize(10, 10);
	m_recLabel = new QLabel(QStringLiteral("Not recording"));
	m_recLabel->setObjectName("sub");
	m_statusMsg = new QLabel();
	m_statusMsg->setObjectName("sub");
	auto *keys = new QLabel(
		QStringLiteral("Ctrl+Enter start · Space pause · Esc stop"));
	keys->setObjectName("sub");
	stLay->addWidget(m_recDot);
	stLay->addWidget(m_recLabel);
	stLay->addWidget(m_statusMsg);
	stLay->addStretch(1);
	stLay->addWidget(keys);
	root->addWidget(status);
}

void TeleprompterDock::applyStyle()
{
	// Dock chrome is painted with an rgba() alpha driven by m_opacity so the
	// docked dock background is translucent (TODO 00006.b). The prompter stage
	// keeps its own opaque black fill (PrompterStage palette) so reading text
	// stays legible regardless of this setting. Full see-through-to-desktop is
	// only possible when the dock is floated (window opacity, applyOpacity()).
	const QString rootBg = rgbaFill(kBg, m_opacity);
	const QString chromeBg = rgbaFill(kPanel, m_opacity);
	setStyleSheet(
		QStringLiteral(
			"#TeleprompterDockRoot { background:%1; color:%3; }"
			"QWidget#bar, QFrame#panel, QWidget#status {"
			"  background:%2; }"
			"QLabel { color:%3; font-size:13px; }"
			"QLabel#fieldLabel { color:%4; font-size:11px;"
			"  text-transform:uppercase; }"
			"QLabel#val { color:%5; font-weight:600; }"
			"QLabel#sub { color:%4; font-size:11px; }"
			"QLabel#dot { border-radius:5px; background:%4; }"
			// Transport controls: uniform white glyphs on the neutral
			// panel fill — no colored play/pause/stop (TODO 00006.a).
			"QPushButton {"
			"  background:%6; color:%3; border:1px solid %7;"
			"  border-radius:6px; padding:7px 12px;"
			"  font-size:13px; font-weight:600; }"
			"QPushButton:hover { border-color:%5; }"
			"QPushButton:disabled { color:%4; }"
			// Active/pressed toggle: a subtle lighter neutral fill,
			// keeping the icon white (no colored accent fill).
			"QPushButton[active=\"true\"] {"
			"  background:%7; color:%3; border-color:%5; }"
			"QPlainTextEdit {"
			"  background:%6; color:%3; border:1px solid %7;"
			"  border-radius:8px; padding:8px;"
			"  font-family:monospace; font-size:14px; }"
			"QComboBox {"
			"  background:%6; color:%3; border:1px solid %7;"
			"  border-radius:6px; padding:5px 8px; }"
			"QComboBox QAbstractItemView {"
			"  background:%6; color:%3;"
			// neutral (not %5=white) so selected text stays legible
			"  selection-background-color:%7; }"
			"QCheckBox { color:%3; font-size:13px; }"
			"QFrame[frameShape=\"4\"] { color:%7; }" // HLine
			"QSlider::groove:horizontal {"
			"  height:4px; background:%7; border-radius:2px; }"
			"QSlider::handle:horizontal {"
			"  background:%5; width:14px; margin:-6px 0;"
			"  border-radius:7px; }"
			"QLabel#countdown {"
			"  background:rgba(0,0,0,180); color:#ffffff;"
			"  font-size:120px; font-weight:800; }")
			// Bind %1..%7 via chained single-arg arg() (not the
			// multi-arg overload) so the QString rgba fills mix with
			// the const char* palette colours cleanly. There is no %8:
			// the danger (kBad) rule was dropped in the white-icon
			// restyle (00006.a) — kBad now lives only in
			// setRecordingIndicator's own stylesheet.
			.arg(rootBg)
			.arg(chromeBg)
			.arg(QLatin1String(kText))
			.arg(QLatin1String(kMuted))
			.arg(QLatin1String(kAccent))
			.arg(QLatin1String(kPanel2))
			.arg(QLatin1String(kBorder)));
}

void TeleprompterDock::setButtonActive(QPushButton *button, bool active)
{
	if (!button)
		return;
	button->setProperty("active", active);
	button->style()->unpolish(button);
	button->style()->polish(button);
}

void TeleprompterDock::showToolWindow(QWidget *window, const QSize &fallbackSize)
{
	if (!window)
		return;
	if (window->size().isEmpty())
		window->resize(fallbackSize);
	if (!window->isVisible()) {
		const QPoint pos =
			mapToGlobal(QPoint(qMax(0, width() - window->width()),
					   height() + 8));
		window->move(pos);
	}
	window->show();
	window->raise();
	window->activateWindow();
}

void TeleprompterDock::setEditorOpen(bool open, bool persist)
{
	m_editorOpen = open;
	if (open) {
		showToolWindow(m_editorPanel, QSize(560, 360));
		if (m_editor)
			m_editor->setFocus();
	} else if (m_editorPanel) {
		m_editorPanel->hide();
	}
	setButtonActive(m_scriptToggle, open);
	if (persist)
		saveSettings();
}

void TeleprompterDock::setSettingsOpen(bool open, bool persist)
{
	m_settingsOpen = open;
	if (open)
		showToolWindow(m_settingsPanel, QSize(380, 520));
	else if (m_settingsPanel)
		m_settingsPanel->hide();
	setButtonActive(m_settingsToggle, open);
	if (persist)
		saveSettings();
}

void TeleprompterDock::wireSignals()
{
	connect(m_startBtn, &QPushButton::clicked, this,
		&TeleprompterDock::startSession);
	connect(m_pauseBtn, &QPushButton::clicked, this,
		&TeleprompterDock::pauseResume);
	connect(m_stopBtn, &QPushButton::clicked, this,
		[this]() { stopAll("stop button"); });
	connect(m_resetBtn, &QPushButton::clicked, this,
		&TeleprompterDock::resetScroll);

	connect(m_scriptToggle, &QPushButton::clicked, this, [this]() {
		setEditorOpen(!m_editorOpen, true);
	});
	connect(m_settingsToggle, &QPushButton::clicked, this, [this]() {
		setSettingsOpen(!m_settingsOpen, true);
	});

	connect(m_editor, &QPlainTextEdit::textChanged, this, [this]() {
		m_script = m_editor->toPlainText();
		renderPrompter();
		updateReadTime();
		saveSettings();
	});

	connect(m_fontSlider, &QSlider::valueChanged, this, [this](int v) {
		m_fontSize = v;
		m_fontVal->setText(QString::number(v) + "px");
		applyPrompterFont();
		applyScrollSpeed(); // baseline scales with font size
		saveSettings();
	});
	connect(m_speedSlider, &QSlider::valueChanged, this, [this](int v) {
		m_speedMult = v / 100.0; // manual override multiplier
		applyScrollSpeed();
		saveSettings();
	});
	connect(m_lineSlider, &QSlider::valueChanged, this, [this](int v) {
		m_lineHeight = v / 100.0;
		m_lineVal->setText(QString::number(m_lineHeight, 'f', 2));
		applyPrompterFont();
		applyScrollSpeed(); // baseline scales with line height too
		saveSettings();
	});
	connect(m_opacitySlider, &QSlider::valueChanged, this, [this](int v) {
		m_opacity = v / 100.0; // 0.30–1.00 dock-background opacity
		applyOpacity();
		saveSettings();
	});
	connect(m_countdownCombo,
		QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		[this](int) {
			m_countdownLen = m_countdownCombo->currentData().toInt();
			saveSettings();
		});
	connect(m_guideCheck, &QCheckBox::toggled, this, [this](bool on) {
		m_guide = on;
		m_stage->setGuide(on);
		saveSettings();
	});
	connect(m_autoRecordCheck, &QCheckBox::toggled, this, [this](bool on) {
		m_autoRecord = on;
		saveSettings();
	});
}

// ── settings persistence ────────────────────────────────────────────────────
QString TeleprompterDock::settingsPath() const
{
	// obs_module_config_path() returns <config>/obs-teleprompter/<file>.
	char *p = obs_module_config_path("settings.json");
	QString path = QString::fromUtf8(p ? p : "");
	bfree(p);
	return path;
}

void TeleprompterDock::loadSettings()
{
	const QString path = settingsPath();
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
		return; // defaults already set as member initialisers
	const QByteArray data = f.readAll();
	f.close();
	const QJsonObject o = QJsonDocument::fromJson(data).object();
	if (o.contains("script"))
		m_script = o.value("script").toString();
	if (o.contains("fontSize"))
		m_fontSize = o.value("fontSize").toInt(m_fontSize);
	if (o.contains("lineHeight"))
		m_lineHeight = o.value("lineHeight").toDouble(m_lineHeight);
	// Scroll speed is persisted as a relative MULTIPLIER (speedMult). For
	// back-compat with pre-0.1.4 settings that stored absolute px/s ("speed"),
	// derive the multiplier from that value against the font-derived baseline.
	if (o.contains("speedMult")) {
		m_speedMult = o.value("speedMult").toDouble(m_speedMult);
	} else if (o.contains("speed")) {
		const double base = fontBaseSpeed(m_fontSize, m_lineHeight);
		const int legacy = o.value("speed").toInt(m_speed);
		if (base > 0.0)
			m_speedMult = legacy / base;
	}
	m_speedMult = qBound(0.25, m_speedMult, 3.0);
	if (o.contains("opacity"))
		m_opacity = o.value("opacity").toDouble(m_opacity);
	m_opacity = qBound(0.30, m_opacity, 1.0);
	if (o.contains("countdownLen"))
		m_countdownLen = o.value("countdownLen").toInt(m_countdownLen);
	if (o.contains("guide"))
		m_guide = o.value("guide").toBool(m_guide);
	if (o.contains("autoRecord"))
		m_autoRecord = o.value("autoRecord").toBool(m_autoRecord);
	if (o.contains("editorOpen"))
		m_editorOpen = o.value("editorOpen").toBool(m_editorOpen);
	if (o.contains("settingsOpen"))
		m_settingsOpen = o.value("settingsOpen").toBool(m_settingsOpen);
}

void TeleprompterDock::saveSettings()
{
	if (m_saveTimer)
		m_saveTimer->start(); // debounce (300 ms)
}

void TeleprompterDock::saveSettingsNow()
{
	// Ensure the config dir exists: obs_module_config_path gives a file
	// path under a dir OBS does not create for us.
	char *dirp = obs_module_config_path("");
	if (dirp) {
		QDir().mkpath(QString::fromUtf8(dirp));
		bfree(dirp);
	}

	QJsonObject o;
	o["script"] = m_script;
	o["fontSize"] = m_fontSize;
	o["speedMult"] = m_speedMult; // relative multiplier (see loadSettings)
	o["lineHeight"] = m_lineHeight;
	o["opacity"] = m_opacity; // dock-background opacity (TODO 00006.b)
	o["countdownLen"] = m_countdownLen;
	o["guide"] = m_guide;
	o["autoRecord"] = m_autoRecord;
	o["editorOpen"] = m_editorOpen;
	o["settingsOpen"] = m_settingsOpen;

	QFile f(settingsPath());
	if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
		f.close();
	}
}

// ── prompter rendering ──────────────────────────────────────────────────────
void TeleprompterDock::renderPrompter()
{
	const QString trimmed = m_script.trimmed();
	if (trimmed.isEmpty()) {
		m_stage->setContent(
			QStringLiteral("Your script will appear here. Paste "
				       "text under ✎ Script, then press ▶ "
				       "Start."),
			true);
	} else {
		m_stage->setContent(m_script, false);
	}
}

void TeleprompterDock::applyPrompterFont()
{
	m_stage->setFontPx(m_fontSize, m_lineHeight);
}

void TeleprompterDock::applyScrollSpeed()
{
	// Effective px/s = font-derived baseline * manual multiplier. Recomputed
	// whenever font size, line height, or the multiplier changes so the reading
	// cadence stays consistent while the slider override survives font changes.
	const double base = fontBaseSpeed(m_fontSize, m_lineHeight);
	m_speed = qMax(1, int(qRound(base * m_speedMult)));
	if (m_speedVal)
		m_speedVal->setText(QStringLiteral("%1 px/s  ×%2")
					    .arg(m_speed)
					    .arg(m_speedMult, 0, 'f', 2));
}

void TeleprompterDock::applyOpacity()
{
	// Two mechanisms, best-effort by dock state (TODO 00006.b):
	//
	//  1. FLOATED — the dock is its own top-level window, so setWindowOpacity
	//     on window() yields true see-through-to-desktop transparency (this is
	//     the only path to real transparency for a docked Qt widget). window()
	//     is OBS's QDockWidget when floated; it is OBS's main window when
	//     docked, where per-widget opacity does not apply — so we only lower
	//     window opacity when floated, never dimming all of OBS.
	//
	//  2. DOCKED — the widget is composited into OBS's opaque main window, so
	//     true see-through is impossible; we instead paint the dock chrome
	//     (root background + bars/panels) with a translucent rgba() alpha for a
	//     visible translucency. The prompter stage keeps its own opaque black
	//     fill (set in PrompterStage) so the reading text stays fully legible.
	if (m_opacityVal)
		m_opacityVal->setText(
			QString::number(int(qRound(m_opacity * 100))) + "%");

	// Find the hosting QDockWidget (OBS wraps our widget in one). If it is
	// FLOATING, it is its own top-level window and setWindowOpacity gives true
	// desktop see-through. If it is DOCKED, window() is OBS's main window — we
	// must NOT lower its opacity (that would dim all of OBS), so we leave the
	// floating dock at full and reset any prior floating opacity to 1.0.
	QDockWidget *host = nullptr;
	for (QWidget *w = parentWidget(); w; w = w->parentWidget()) {
		if (auto *dw = qobject_cast<QDockWidget *>(w)) {
			host = dw;
			break;
		}
	}
	if (host) {
		if (host->isFloating())
			host->setWindowOpacity(m_opacity);
		else
			host->setWindowOpacity(1.0);
	}

	// Re-apply the stylesheet so the rgba() chrome alpha (built from
	// m_opacity in applyStyle) refreshes for the docked-state translucency.
	applyStyle();
}

void TeleprompterDock::updateReadTime()
{
	const QStringList words = m_script.split(QRegularExpression("\\s+"),
						 Qt::SkipEmptyParts);
	const int n = words.size();
	const int secs = int((n / 150.0) * 60.0); // ~150 wpm
	const int m = secs / 60, s = secs % 60;
	m_readTime->setText(QStringLiteral("~%1:%2 reading time · %3 words")
				    .arg(m)
				    .arg(s, 2, 10, QChar('0'))
				    .arg(n));
}

// ── scroll engine (QTimer + sub-pixel accumulation) ─────────────────────────
double TeleprompterDock::maxScroll() const
{
	return m_stage->maxOffset();
}

void TeleprompterDock::tick()
{
	if (m_mode != Mode::Running)
		return;
	if (m_paused) {
		m_frameClock->restart();
		return;
	}
	const double dt = m_frameClock->nsecsElapsed() / 1e9;
	m_frameClock->restart();
	m_scrollPos += m_speed * dt;
	const double max = maxScroll();
	if (m_scrollPos >= max) {
		m_scrollPos = max;
		m_stage->setOffset(m_scrollPos);
		finishScroll();
		return;
	}
	m_stage->setOffset(m_scrollPos);
}

void TeleprompterDock::beginScroll()
{
	m_mode = Mode::Running;
	m_paused = false;
	m_scrollPos = m_stage->offset();
	m_frameClock->restart();
	m_scrollTimer->start();
	setControls(Mode::Running);
}

void TeleprompterDock::finishScroll()
{
	stopAll("end of script");
}

void TeleprompterDock::resetScroll()
{
	m_scrollPos = 0;
	m_stage->setOffset(0);
}

// ── countdown ───────────────────────────────────────────────────────────────
void TeleprompterDock::startCountdown(int seconds)
{
	m_countRemaining = seconds;
	m_countdownOverlay->setGeometry(m_stage->rect());
	m_countdownOverlay->setText(QString::number(seconds));
	m_countdownOverlay->raise();
	m_countdownOverlay->setVisible(true);
	m_countdownTimer->start();
}

void TeleprompterDock::countdownStep()
{
	m_countRemaining -= 1;
	if (m_countRemaining <= 0) {
		m_countdownTimer->stop();
		m_countdownOverlay->setVisible(false);
		// Countdown finished. With auto-record enabled, request recording and
		// gate scroll on the confirmed RECORDING_STARTED event. With it
		// disabled, begin scrolling immediately without touching OBS recording.
		if (m_mode != Mode::Counting)
			return; // a Stop cancelled us mid-count
		if (m_autoRecord) {
			m_mode = Mode::WaitingForRecord;
			requestRecordingStart();
		} else {
			m_startPending = false;
			beginScroll();
		}
		return;
	}
	m_countdownOverlay->setGeometry(m_stage->rect());
	m_countdownOverlay->setText(QString::number(m_countRemaining));
}

void TeleprompterDock::cancelCountdown()
{
	m_countdownTimer->stop();
	m_countdownOverlay->setVisible(false);
}

// ── orchestration ───────────────────────────────────────────────────────────
void TeleprompterDock::startSession()
{
	if (m_mode != Mode::Idle)
		return;
	if (m_script.trimmed().isEmpty()) {
		flashStatus(QStringLiteral("Add a script first (✎ Script)."));
		return;
	}
	m_startPending = true;
	m_recordingManagedThisSession = false;
	// reset to top before counting
	resetScroll();

	const int len = m_countdownLen;
	if (len <= 0) {
		if (m_autoRecord) {
			// no countdown — go straight to record request
			m_mode = Mode::WaitingForRecord;
			setControls(Mode::Counting); // Stop enabled, Start disabled
			requestRecordingStart();
		} else {
			m_startPending = false;
			beginScroll();
		}
		return;
	}
	m_mode = Mode::Counting;
	setControls(Mode::Counting);
	startCountdown(len);
}

void TeleprompterDock::pauseResume()
{
	if (m_mode != Mode::Running)
		return;
	m_paused = !m_paused;
	if (!m_paused)
		m_frameClock->restart();
	m_pauseBtn->setText(m_paused ? QStringLiteral("Resume")
				     : QStringLiteral("Pause"));
	m_pauseBtn->setIcon(makeTransportIcon(m_paused ? TransportIcon::Play
						       : TransportIcon::Pause));
	setButtonActive(m_pauseBtn, m_paused);
}

void TeleprompterDock::stopAll(const char *reason)
{
	(void)reason;
	const bool wasActive = (m_mode != Mode::Idle);

	// cancel a mid-count countdown
	if (m_mode == Mode::Counting)
		cancelCountdown();

	// Clear the pending flag so an in-flight RECORDING_STARTED handler that
	// arrives after this Stop does NOT begin scrolling (724aa91 race fix).
	m_startPending = false;
	m_scrollTimer->stop();
	m_paused = false;
	m_mode = Mode::Idle;
	setControls(Mode::Idle);

	// Stop only the recording this Start flow managed. In auto-record mode this
	// preserves the existing start/reuse behavior and keeps the record-start
	// race backstop. With auto-record disabled, Stop only stops scrolling.
	if (wasActive && m_recordingManagedThisSession)
		requestRecordingStop();
}

// ── recording (in-process OBS frontend API) ─────────────────────────────────
void TeleprompterDock::requestRecordingStart()
{
	m_recordingManagedThisSession = true;
	if (obs_frontend_recording_active()) {
		// already recording — reuse it; the RECORDING_STARTED event
		// won't fire, so gate the scroll here directly.
		setRecordingIndicator(true);
		onRecordingStarted();
		return;
	}
	obs_frontend_recording_start();
	// scroll begins in onRecordingStarted() when OBS confirms the start.
}

void TeleprompterDock::requestRecordingStop()
{
	if (obs_frontend_recording_active())
		obs_frontend_recording_stop();
	// indicator flips in onRecordingStopped().
}

void TeleprompterDock::onRecordingStarted()
{
	setRecordingIndicator(true);
	// Only begin scrolling if a Start is still pending (not cancelled by a
	// Stop during the record-start window) and we are still waiting for it.
	if (!m_startPending || m_mode != Mode::WaitingForRecord) {
		// A Stop landed while the start was in flight → ensure nothing
		// is left recording, and do not scroll.
		if (!m_startPending && m_recordingManagedThisSession)
			requestRecordingStop();
		return;
	}
	m_startPending = false;
	beginScroll();
}

void TeleprompterDock::onRecordingStopped()
{
	setRecordingIndicator(false);
	m_recordingManagedThisSession = false;
}

// ── control state / status ──────────────────────────────────────────────────
void TeleprompterDock::setControls(Mode mode)
{
	const bool running = (mode == Mode::Running);
	const bool busy = (mode == Mode::Counting ||
			   mode == Mode::WaitingForRecord || running);
	m_startBtn->setEnabled(!busy);
	m_pauseBtn->setEnabled(running);
	m_stopBtn->setEnabled(busy);
	if (mode == Mode::Idle) {
		m_pauseBtn->setText(QStringLiteral("Pause"));
		m_pauseBtn->setIcon(makeTransportIcon(TransportIcon::Pause));
		setButtonActive(m_pauseBtn, false);
	}
}

void TeleprompterDock::setRecordingIndicator(bool on)
{
	m_recDot->setStyleSheet(
		on ? QStringLiteral("border-radius:5px; background:%1;")
			     .arg(kBad)
		   : QStringLiteral("border-radius:5px; background:%1;")
			     .arg(kMuted));
	m_recLabel->setText(on ? QStringLiteral("● Recording")
			       : QStringLiteral("Not recording"));
}

void TeleprompterDock::flashStatus(const QString &text)
{
	m_statusMsg->setText(text);
	m_flashTimer->start();
}

// ── keyboard shortcuts ──────────────────────────────────────────────────────
bool TeleprompterDock::eventFilter(QObject *obj, QEvent *ev)
{
	if (ev->type() == QEvent::Close) {
		if (obj == m_editorPanel) {
			m_editorOpen = false;
			setButtonActive(m_scriptToggle, false);
			saveSettings();
		} else if (obj == m_settingsPanel) {
			m_settingsOpen = false;
			setButtonActive(m_settingsToggle, false);
			saveSettings();
		}
		return QWidget::eventFilter(obj, ev);
	}

	if (ev->type() != QEvent::KeyPress)
		return QWidget::eventFilter(obj, ev);

	// Only act when the keyboard focus is inside this dock, so we don't
	// hijack keys meant for the rest of OBS.
	QWidget *focus = qApp->focusWidget();
	const bool focusInMainDock =
		isVisible() && focus && (focus == this || isAncestorOf(focus));
	const bool focusInScript =
		m_editorPanel && m_editorPanel->isVisible() && focus &&
		(focus == m_editorPanel || m_editorPanel->isAncestorOf(focus));
	const bool focusInSettings =
		m_settingsPanel && m_settingsPanel->isVisible() && focus &&
		(focus == m_settingsPanel ||
		 m_settingsPanel->isAncestorOf(focus));
	if (!focusInMainDock && !focusInScript && !focusInSettings)
		return QWidget::eventFilter(obj, ev);

	auto *ke = static_cast<QKeyEvent *>(ev);
	const bool ctrl = ke->modifiers() & (Qt::ControlModifier |
					     Qt::MetaModifier);
	const bool typing = (qApp->focusWidget() == m_editor);

	if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
		if (ctrl) {
			startSession();
			return true;
		}
	}
	if (typing)
		return QWidget::eventFilter(obj, ev); // don't hijack in editor

	if (ke->key() == Qt::Key_Space) {
		if (m_mode == Mode::Running)
			pauseResume();
		else if (m_mode == Mode::Idle)
			startSession();
		return true;
	}
	if (ke->key() == Qt::Key_Escape) {
		stopAll("esc");
		return true;
	}
	return QWidget::eventFilter(obj, ev);
}
