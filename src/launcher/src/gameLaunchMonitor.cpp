#include "gameLaunchMonitor.h"

#include "configuration.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QWidget>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {
class GameLaunchMonitor final: public QObject {
public:
	GameLaunchMonitor(QProcess* process, const QString& interpreter, const QStringList& arguments,
	                  const Configuration& config, QWidget* parent)
	    : QObject(process), m_process(process), m_parent(parent) {
		const QDir runtime(QFileInfo(interpreter).absolutePath());
		auto title = config.title_id.isEmpty() ? QStringLiteral("game") : config.title_id;
		title.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]")), QStringLiteral("_"));
		m_log.setFileName(runtime.filePath(QStringLiteral("_Logs/%1-%2-%3.log")
		                                     .arg(title, QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"),
		                                          QUuid::createUuid().toString(QUuid::Id128).left(8))));
		if (!runtime.mkpath(QStringLiteral("_Logs")) || !m_log.open(QIODevice::WriteOnly)) {
			QMessageBox::warning(parent, tr("Launch log unavailable"),
			                     tr("The game will start, but its launch log cannot be saved:\n%1\n%2")
			                         .arg(m_log.fileName(), m_log.errorString()));
		}

		auto env = QProcessEnvironment::systemEnvironment();
		const auto set = [&env](const char* key, bool enabled) {
			env.insert(QString::fromLatin1(key), enabled ? QStringLiteral("1") : QStringLiteral("0"));
		};
		// Always record new variants, even when startup preparation is disabled.
		set("KYTY_PROGRAM_CACHE", true);
		set("KYTY_PROGRAM_CACHE_SAVE", true);
		set("KYTY_SHADER_PRECOMPILE", true);
		set("KYTY_SHADER_PRECOMPILE_REPLAY", config.shader_warmup_enabled);
		set("KYTY_SHADER_PRECOMPILE_WAIT", config.shader_warmup_enabled);
		set("KYTY_PIPELINE_JOURNAL", true);
		set("KYTY_PIPELINE_JOURNAL_REPLAY", config.shader_warmup_enabled);
		set("KYTY_PIPELINE_JOURNAL_WAIT", config.shader_warmup_enabled);
		set("KYTY_PIPELINE_JOURNAL_FORCE", config.shader_warmup_enabled);
		set("KYTY_PIPELINE_PREWARM_COMPUTE", config.shader_warmup_enabled);
		set("KYTY_PIPELINE_PREWARM_WAIT", config.shader_warmup_enabled);
		set("KYTY_PIPELINE_CACHE_SAVE", true);
		set("KYTY_PIPELINE_KNOWN", true);
		set("KYTY_PIPELINE_FAST_FIRST", config.pipeline_acceleration_enabled);
		set("KYTY_PIPELINE_FAST_FIRST_PROBE", config.pipeline_acceleration_enabled);
		if (config.pipeline_acceleration_enabled) set("KYTY_PIPELINE_LIBRARY", false);
		env.insert("KYTY_PIPELINE_RESIDENT_MAX", QString::number(config.pipeline_resident_limit));
		set("KYTY_FRAME_STATS", config.performance_diagnostics_enabled);
		set("KYTY_PIPELINE_STATS", config.performance_diagnostics_enabled);
		set("KYTY_THREAD_CPU_STATS", config.performance_diagnostics_enabled);
		set("KYTY_CP_GAP_STATS", config.performance_diagnostics_enabled);
		set("KYTY_PROFILE_LOADING", config.performance_diagnostics_enabled);
		set("KYTY_LOADING_SUMMARY", config.performance_diagnostics_enabled);
		if (config.performance_diagnostics_enabled) {
			env.insert("KYTY_LOADING_LOG", QFileInfo(m_log.fileName()).absolutePath() + "/" +
			    QFileInfo(m_log.fileName()).completeBaseName() + ".loading.csv");
		} else {
			env.remove("KYTY_LOADING_LOG");
		}
		env.insert("KYTY_FAULT_COST_LOG", config.performance_diagnostics_enabled ? "10" : "60");
		// These are startup workers; background optimization keeps its own bounded pool.
		const int automatic = qBound(1, QThread::idealThreadCount() / 4, 4);
		env.insert("KYTY_SHADER_PRECOMPILE_THREADS",
		           QString::number(config.shader_warmup_threads == 0 ? automatic : config.shader_warmup_threads));
		env.insert("KYTY_PIPELINE_JOURNAL_THREADS",
		           QString::number(config.pipeline_warmup_threads == 0 ? automatic : config.pipeline_warmup_threads));
		env.insert("KYTY_PIPELINE_PREWARM_THREADS", QStringLiteral("2"));
		if (!config.pipeline_list_path.isEmpty()) {
			env.insert("KYTY_PIPELINE_LIST", QFileInfo(config.pipeline_list_path).absoluteFilePath());
		} else if (QFileInfo::exists(runtime.filePath("PipelineLists/" + title + ".kyplst"))) {
			env.insert("KYTY_PIPELINE_LIST", QStringLiteral("1"));
		} else {
			env.remove("KYTY_PIPELINE_LIST");
		}
		// The launcher starts a game; the separate list preparation tool may request exit-only mode.
		env.remove("KYTY_PIPELINE_LIST_PREPARE");
		process->setProcessEnvironment(env);
		process->setProgram(interpreter);
		process->setArguments(arguments);
		process->setWorkingDirectory(runtime.absolutePath());
		process->setProcessChannelMode(QProcess::MergedChannels);
#if defined(_WIN32)
		process->setNativeArguments({});
		process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
			args->flags |= static_cast<DWORD>(CREATE_NO_WINDOW);
		});
#endif
		if (config.shader_warmup_enabled) {
			m_progress = new QProgressDialog(parent);
			m_progress->setWindowFlag(Qt::WindowStaysOnTopHint);
			m_progress->setWindowTitle(tr("Preparing shaders and pipelines"));
			m_progress->setLabelText(tr("Reading this game's cache…\nNew areas may still need a first compile."));
			m_progress->setCancelButtonText(tr("Cancel launch"));
			m_progress->setRange(0, 0);
			m_progress->setMinimumDuration(0);
			m_progress->setAutoClose(false);
			m_progress->setAutoReset(false);
			// Non-modal avoids QProgressDialog::setValue processing nested events while parsing output.
			m_progress->setWindowModality(Qt::NonModal);
			m_progress->setMinimumWidth(480);
			connect(m_progress, &QProgressDialog::canceled, this, [this] {
				if (m_ready) return;
				m_canceled = true;
				m_process->terminate();
				QTimer::singleShot(3000, this, [this] {
					if (m_canceled && m_process->state() != QProcess::NotRunning) m_process->kill();
				});
			});
			m_progress->show();
		}
		connect(process, &QProcess::readyReadStandardOutput, this, [this] { Drain(); });
		connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
			if (error == QProcess::FailedToStart) {
				Drain();
				CloseProgress();
				QMessageBox::critical(m_parent, tr("Game could not start"), m_process->errorString());
				deleteLater();
			}
		});
		connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
		        [this](int code, QProcess::ExitStatus status) {
			        Drain();
			        CloseProgress();
			        m_log.flush();
			        if (!m_canceled && (code != 0 || status == QProcess::CrashExit)) {
				        QMessageBox::critical(
				            m_parent, tr("Game stopped"),
				            tr("The emulator stopped (exit code %1).\nLog: %2\n\n%3")
				                .arg(code).arg(m_log.fileName(), QString::fromUtf8(m_tail.right(1800))));
			        }
			        if (m_log_failed) {
				        QMessageBox::warning(m_parent, tr("Incomplete launch log"),
				                             tr("Writing the log failed. Check free space and folder permissions.\n%1")
				                                 .arg(m_log.fileName()));
			        }
			        deleteLater();
		        });
		process->start();
	}

	~GameLaunchMonitor() override { CloseProgress(); }

private:
	void CloseProgress() {
		if (m_progress) {
			m_progress->disconnect(this);
			m_progress->hide();
			m_progress->deleteLater();
			m_progress = nullptr;
		}
	}

	void Drain() {
		const auto data = m_process->readAllStandardOutput();
		if (data.isEmpty()) return;
		if (m_log.isOpen() && !m_log_failed) {
			if (m_log.write(data) != data.size() || !m_log.flush()) m_log_failed = true;
		}
		m_tail.append(data);
		if (m_tail.size() > 32768) m_tail = m_tail.right(32768);
		if (!m_progress) return;
		m_pending.append(data);
		qsizetype end;
		while ((end = m_pending.indexOf('\n')) >= 0) {
			Parse(QString::fromUtf8(m_pending.first(end)));
			m_pending.remove(0, end + 1);
		}
		// A malformed or very long guest printf must not grow the startup reader indefinitely.
		if (m_pending.size() > 32768) m_pending = m_pending.right(32768);
	}

	void Parse(QString line) {
		static const QRegularExpression ansi(QStringLiteral("\x1b\\[[0-9;]*[mK]"));
		static const QRegularExpression shader(QStringLiteral("Shader precompile: (?:done )?(\\d+)/(\\d+)"));
		static const QRegularExpression pipeline(QStringLiteral("Pipeline replay: (\\d+)/(\\d+)"));
		line.remove(ansi);
		if (line.contains(QStringLiteral("Initialized: Graphics"))) {
			m_ready = true;
			CloseProgress();
			return;
		}
		if (!m_progress) return;
		const auto shaders = shader.match(line);
		const auto pipelines = pipeline.match(line);
		if (shaders.hasMatch() || pipelines.hasMatch()) {
			const auto& match = shaders.hasMatch() ? shaders : pipelines;
			const auto total = match.captured(2).toULongLong();
			const auto done = qMin(match.captured(1).toULongLong(), total);
			m_progress->setRange(0, total == 0 ? 0 : 1000);
			if (total != 0) m_progress->setValue(static_cast<int>(done * 1000 / total));
			m_progress->setLabelText((shaders.hasMatch() ? tr("Shaders: %1 / %2") : tr("Pipelines: %1 / %2"))
			                            .arg(done).arg(total) + tr("\nOnly previously recorded, usable variants can be prepared."));
		} else if (line.contains("Pipeline journal:") && line.contains("pipelines loaded")) {
			m_progress->setRange(0, 0);
			m_progress->setLabelText(tr("Preparing recorded graphics and compute pipelines…"));
		} else if (line.contains("Pipeline journal: replay done")) {
			m_progress->setRange(0, 0);
			m_progress->setLabelText(tr("Finishing graphics initialization…"));
		} else if (line.contains("Pipeline warmup: waiting")) {
			m_progress->setRange(0, 0);
			m_progress->setLabelText(tr("Preparing compute and ray-tracing pipelines…\nA cold driver cache can take several minutes."));
		}
	}

	QProcess* m_process;
	QPointer<QWidget> m_parent;
	QPointer<QProgressDialog> m_progress;
	QFile m_log;
	QByteArray m_tail, m_pending;
	bool m_ready = false, m_canceled = false, m_log_failed = false;
};
} // namespace

void StartGameWithPreparation(QProcess* process, const QString& interpreter,
                              const QStringList& arguments, const Configuration& config,
                              QWidget* parent) {
	new GameLaunchMonitor(process, interpreter, arguments, config, parent);
}
