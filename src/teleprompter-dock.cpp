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
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPlainTextEdit>
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
const char *kAccent = "#3ea6ff";
const char *kBad = "#ff5b5b";

TeleprompterDock *g_instance = nullptr;
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
	m_speedSlider->setValue(m_speed);
	m_lineSlider->setValue(int(m_lineHeight * 100));
	m_countdownCombo->setCurrentIndex(
		m_countdownCombo->findData(m_countdownLen));
	m_guideCheck->setChecked(m_guide);
	m_settingsPanel->setVisible(m_settingsOpen);
	m_editorPanel->setVisible(m_editorOpen);
	m_settingsToggle->setProperty("active", m_settingsOpen);
	m_scriptToggle->setProperty("active", m_editorOpen);

	m_fontVal->setText(QString::number(m_fontSize) + "px");
	m_speedVal->setText(QString::number(m_speed));
	m_lineVal->setText(QString::number(m_lineHeight, 'f', 2));

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
	return b;
}

static QFrame *hline()
{
	auto *f = new QFrame();
	f->setFrameShape(QFrame::HLine);
	f->setFrameShadow(QFrame::Plain);
	return f;
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

	m_startBtn = mkButton(QStringLiteral("▶ Start"),
			      QStringLiteral("Start (Ctrl+Enter)"));
	m_startBtn->setProperty("kind", "primary");
	m_pauseBtn = mkButton(QStringLiteral("⏸ Pause"),
			      QStringLiteral("Pause / Resume (Space)"));
	m_stopBtn = mkButton(QStringLiteral("⏹ Stop"),
			     QStringLiteral("Stop (Esc)"));
	m_stopBtn->setProperty("kind", "danger");
	m_resetBtn = mkButton(QStringLiteral("⟲ Reset"),
			      QStringLiteral("Reset to top"));
	m_scriptToggle = mkButton(QStringLiteral("✎ Script"),
				  QStringLiteral("Show/hide script editor"));
	m_settingsToggle = mkButton(QStringLiteral("⚙ Settings"),
				    QStringLiteral("Show/hide settings"));

	barLay->addWidget(m_startBtn);
	barLay->addWidget(m_pauseBtn);
	barLay->addWidget(m_stopBtn);
	barLay->addWidget(m_resetBtn);
	barLay->addStretch(1);
	barLay->addWidget(m_scriptToggle);
	barLay->addWidget(m_settingsToggle);
	root->addWidget(bar);

	// ── settings panel ──
	m_settingsPanel = new QFrame();
	m_settingsPanel->setObjectName("panel");
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
		m_fontSlider->setRange(18, 120);
		setLay->addWidget(m_fontSlider);
	}
	// scroll speed
	{
		auto *row = new QHBoxLayout();
		auto *lab = mkFieldLabel(QStringLiteral("Scroll speed (px/s)"));
		m_speedVal = new QLabel(QStringLiteral("60"));
		m_speedVal->setObjectName("val");
		row->addWidget(lab);
		row->addStretch(1);
		row->addWidget(m_speedVal);
		setLay->addLayout(row);
		m_speedSlider = new QSlider(Qt::Horizontal);
		m_speedSlider->setRange(10, 400);
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
		m_lineSlider->setRange(100, 300); // /100 → 1.0–3.0
		m_lineSlider->setSingleStep(5);
		setLay->addWidget(m_lineSlider);
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
		m_readTime = new QLabel(QStringLiteral("~0:00 reading time"));
		m_readTime->setObjectName("sub");
		setLay->addWidget(m_readTime);
	}
	m_settingsPanel->setVisible(false);
	root->addWidget(m_settingsPanel);
	root->addWidget(hline());

	// ── script editor panel ──
	m_editorPanel = new QFrame();
	m_editorPanel->setObjectName("panel");
	auto *edLay = new QVBoxLayout(m_editorPanel);
	edLay->setContentsMargins(10, 10, 10, 10);
	m_editor = new QPlainTextEdit();
	m_editor->setPlaceholderText(
		QStringLiteral("Paste or type your script here…"));
	m_editor->setMinimumHeight(120);
	edLay->addWidget(m_editor);
	m_editorPanel->setVisible(false);
	root->addWidget(m_editorPanel);

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
			"QPushButton {"
			"  background:%6; color:%3; border:1px solid %7;"
			"  border-radius:6px; padding:7px 12px;"
			"  font-size:13px; font-weight:600; }"
			"QPushButton:hover { border-color:%5; }"
			"QPushButton:disabled { color:%4; }"
			"QPushButton[kind=\"primary\"] {"
			"  background:%5; color:#06121e; border-color:%5; }"
			"QPushButton[kind=\"danger\"] {"
			"  color:%8; border-color:%8; }"
			"QPushButton[active=\"true\"] {"
			"  background:%5; color:#06121e; border-color:%5; }"
			"QPlainTextEdit {"
			"  background:%6; color:%3; border:1px solid %7;"
			"  border-radius:8px; padding:8px;"
			"  font-family:monospace; font-size:14px; }"
			"QComboBox {"
			"  background:%6; color:%3; border:1px solid %7;"
			"  border-radius:6px; padding:5px 8px; }"
			"QComboBox QAbstractItemView {"
			"  background:%6; color:%3;"
			"  selection-background-color:%5; }"
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
			.arg(kBg, kPanel, kText, kMuted, kAccent, kPanel2,
			     kBorder, kBad));
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
		m_editorOpen = !m_editorOpen;
		m_editorPanel->setVisible(m_editorOpen);
		m_scriptToggle->setProperty("active", m_editorOpen);
		m_scriptToggle->style()->unpolish(m_scriptToggle);
		m_scriptToggle->style()->polish(m_scriptToggle);
		if (m_editorOpen)
			m_editor->setFocus();
		saveSettings();
	});
	connect(m_settingsToggle, &QPushButton::clicked, this, [this]() {
		m_settingsOpen = !m_settingsOpen;
		m_settingsPanel->setVisible(m_settingsOpen);
		m_settingsToggle->setProperty("active", m_settingsOpen);
		m_settingsToggle->style()->unpolish(m_settingsToggle);
		m_settingsToggle->style()->polish(m_settingsToggle);
		saveSettings();
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
		saveSettings();
	});
	connect(m_speedSlider, &QSlider::valueChanged, this, [this](int v) {
		m_speed = v;
		m_speedVal->setText(QString::number(v));
		saveSettings();
	});
	connect(m_lineSlider, &QSlider::valueChanged, this, [this](int v) {
		m_lineHeight = v / 100.0;
		m_lineVal->setText(QString::number(m_lineHeight, 'f', 2));
		applyPrompterFont();
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
	if (o.contains("speed"))
		m_speed = o.value("speed").toInt(m_speed);
	if (o.contains("lineHeight"))
		m_lineHeight = o.value("lineHeight").toDouble(m_lineHeight);
	if (o.contains("countdownLen"))
		m_countdownLen = o.value("countdownLen").toInt(m_countdownLen);
	if (o.contains("guide"))
		m_guide = o.value("guide").toBool(m_guide);
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
	o["speed"] = m_speed;
	o["lineHeight"] = m_lineHeight;
	o["countdownLen"] = m_countdownLen;
	o["guide"] = m_guide;
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
		// countdown finished → request recording, then gate scroll on
		// the confirmed RECORDING_STARTED event.
		if (m_mode != Mode::Counting)
			return; // a Stop cancelled us mid-count
		m_mode = Mode::WaitingForRecord;
		requestRecordingStart();
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
	// reset to top before counting
	resetScroll();

	const int len = m_countdownLen;
	if (len <= 0) {
		// no countdown — go straight to record request
		m_mode = Mode::WaitingForRecord;
		setControls(Mode::Counting); // Stop enabled, Start disabled
		requestRecordingStart();
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
	m_pauseBtn->setText(m_paused ? QStringLiteral("▶ Resume")
				     : QStringLiteral("⏸ Pause"));
	m_pauseBtn->setProperty("active", m_paused);
	m_pauseBtn->style()->unpolish(m_pauseBtn);
	m_pauseBtn->style()->polish(m_pauseBtn);
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

	// Stop the recording we (may have) started. Safe to call even if the
	// start hasn't been confirmed yet — a backstop against the record-start
	// winning the race after this point.
	if (wasActive)
		requestRecordingStop();
}

// ── recording (in-process OBS frontend API) ─────────────────────────────────
void TeleprompterDock::requestRecordingStart()
{
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
		if (!m_startPending)
			requestRecordingStop();
		return;
	}
	m_startPending = false;
	beginScroll();
}

void TeleprompterDock::onRecordingStopped()
{
	setRecordingIndicator(false);
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
		m_pauseBtn->setText(QStringLiteral("⏸ Pause"));
		m_pauseBtn->setProperty("active", false);
		m_pauseBtn->style()->unpolish(m_pauseBtn);
		m_pauseBtn->style()->polish(m_pauseBtn);
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
	if (ev->type() != QEvent::KeyPress)
		return QWidget::eventFilter(obj, ev);

	// Only act when the keyboard focus is inside this dock, so we don't
	// hijack keys meant for the rest of OBS.
	QWidget *focus = qApp->focusWidget();
	const bool focusInDock =
		isVisible() && focus && (focus == this || isAncestorOf(focus));
	if (!focusInDock)
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
