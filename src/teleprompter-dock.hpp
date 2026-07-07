/*
 * OBS Docked Teleprompter — dock widget (phases 002.B + 002.C)
 *
 * A native OBS dock that ports the TODO 001 teleprompter UX from `index.html`
 * into Qt widgets, and drives OBS recording in-process via the OBS frontend API
 * (no OBS WebSocket, no host/port/password). The dock:
 *
 *   - hosts a script editor + a smooth-scrolling prompter stage with adjustable
 *     font size / scroll speed / line height, a dark theme, an optional center
 *     guide line, and an animated countdown overlay;
 *   - runs the Start → countdown → record → scroll state machine, gating the
 *     scroll on the confirmed OBS_FRONTEND_EVENT_RECORDING_STARTED event
 *     (reliability-first — never scroll on an unconfirmed record start);
 *   - persists all settings to a JSON file under the plugin config path so they
 *     survive OBS restarts.
 *
 * Recording frontend events arrive on the Qt UI thread in OBS, so the event
 * hooks below touch widgets directly. plugin-main.cpp forwards the relevant
 * frontend events to the single live dock instance via the static hooks.
 */
#pragma once

#include <QWidget>

class QTimer;
class QElapsedTimer;
class QPlainTextEdit;
class QScrollArea;
class QLabel;
class QPushButton;
class QSlider;
class QComboBox;
class QCheckBox;
class QFrame;

// Prompter viewport: a scrollable stage that paints the script and the optional
// center guide. Kept minimal — the dock drives its scroll offset each tick.
class PrompterStage;

class TeleprompterDock : public QWidget {
	Q_OBJECT

public:
	explicit TeleprompterDock(QWidget *parent = nullptr);
	~TeleprompterDock() override;

	// Forwarded from the OBS frontend event callback (UI thread). These drive
	// the recording-gated state machine in 002.C.
	void onRecordingStarted();
	void onRecordingStopped();

	// The single live dock instance, so plugin-main.cpp can forward frontend
	// recording events. Set in the constructor, cleared in the destructor.
	static TeleprompterDock *instance();

protected:
	bool eventFilter(QObject *obj, QEvent *ev) override;

private:
	// ── control state machine ────────────────────────────────────────────
	enum class Mode { Idle, Counting, WaitingForRecord, Running };

	void buildUi();
	void applyStyle();
	void wireSignals();

	// settings + persistence
	void loadSettings();
	void saveSettings();      // debounced
	void saveSettingsNow();
	QString settingsPath() const;

	// prompter rendering / read-time
	void renderPrompter();
	void updateReadTime();
	void applyPrompterFont();

	// scroll engine
	void tick();              // ~60 Hz timer callback
	double maxScroll() const;
	void beginScroll();
	void finishScroll();      // reached end naturally
	void resetScroll();

	// countdown
	void startCountdown(int seconds);
	void countdownStep();
	void cancelCountdown();

	// orchestration
	void startSession();
	void pauseResume();
	void stopAll(const char *reason);

	// recording (in-process frontend API)
	void requestRecordingStart();   // start or reuse an active recording
	void requestRecordingStop();

	// control enable/disable per mode
	void setControls(Mode mode);
	void flashStatus(const QString &text);
	void setRecordingIndicator(bool on);

	// ── settings model (ports index.html `defaults`) ─────────────────────
	QString m_script;
	int m_fontSize = 48;      // px
	int m_speed = 60;         // px / second
	double m_lineHeight = 1.5;
	int m_countdownLen = 3;   // seconds (0 = none)
	bool m_guide = false;
	bool m_editorOpen = false;
	bool m_settingsOpen = false;

	// ── runtime state ────────────────────────────────────────────────────
	Mode m_mode = Mode::Idle;
	double m_scrollPos = 0.0; // sub-pixel accumulator
	bool m_paused = false;
	int m_countRemaining = 0;
	// Stop pressed while a record-start is in flight clears this; the
	// RECORDING_STARTED handler checks it before beginning the scroll
	// (port of the 724aa91 Stop-during-record-start race fix).
	bool m_startPending = false;

	QTimer *m_scrollTimer = nullptr;
	QElapsedTimer *m_frameClock = nullptr;
	QTimer *m_countdownTimer = nullptr;
	QTimer *m_saveTimer = nullptr;
	QTimer *m_flashTimer = nullptr;

	// ── widgets ──────────────────────────────────────────────────────────
	QPushButton *m_startBtn = nullptr;
	QPushButton *m_pauseBtn = nullptr;
	QPushButton *m_stopBtn = nullptr;
	QPushButton *m_resetBtn = nullptr;
	QPushButton *m_scriptToggle = nullptr;
	QPushButton *m_settingsToggle = nullptr;

	QFrame *m_settingsPanel = nullptr;
	QFrame *m_editorPanel = nullptr;
	QPlainTextEdit *m_editor = nullptr;

	QSlider *m_fontSlider = nullptr;
	QLabel *m_fontVal = nullptr;
	QSlider *m_speedSlider = nullptr;
	QLabel *m_speedVal = nullptr;
	QSlider *m_lineSlider = nullptr;
	QLabel *m_lineVal = nullptr;
	QComboBox *m_countdownCombo = nullptr;
	QCheckBox *m_guideCheck = nullptr;
	QLabel *m_readTime = nullptr;

	PrompterStage *m_stage = nullptr;
	QLabel *m_countdownOverlay = nullptr;

	QLabel *m_recDot = nullptr;
	QLabel *m_recLabel = nullptr;
	QLabel *m_statusMsg = nullptr;
};
