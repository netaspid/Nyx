#include "document_viewer.hpp"

#include "host_env.hpp"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QMimeDatabase>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>
#include <QtGlobal>

#include <cmath>
#include <thread>

namespace {

QProcessEnvironment tool_env_for(const QString& program) {
  QProcessEnvironment env = nyx_app::host_process_environment();
  const QFileInfo fi(program);
  const QString tools_dir = fi.absolutePath();

  if (tools_dir.endsWith(QStringLiteral("/tools")) ||
      tools_dir.endsWith(QStringLiteral("\\tools"))) {
    const QString lib = QDir(tools_dir).filePath(QStringLiteral("lib"));
#if defined(Q_OS_WIN)
    const QString path = env.value(QStringLiteral("PATH"));
    env.insert(QStringLiteral("PATH"), tools_dir + QLatin1Char(';') + path);
#else
    if (QDir(lib).exists()) {
      env.insert(QStringLiteral("LD_LIBRARY_PATH"), lib);
    }
#endif
  }
  return env;
}

void ensure_bundled_tools_executable() {
  const QString tools =
      QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("tools"));
  if (!QDir(tools).exists())
    return;
  const QStringList names = {QStringLiteral("mutool"),
                             QStringLiteral("pdfinfo"),
                             QStringLiteral("pdftoppm"),
                             QStringLiteral("mutool.exe"),
                             QStringLiteral("pdfinfo.exe"),
                             QStringLiteral("pdftoppm.exe")};
  for (const QString& name : names) {
    QFile f(QDir(tools).filePath(name));
    if (!f.exists())
      continue;
    f.setPermissions(f.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeUser |
                     QFileDevice::ExeGroup | QFileDevice::ExeOther);
  }
}

QByteArray
run_tool_capture(const QString& program, const QStringList& args, int timeout_ms, int* exit_code) {
  if (exit_code)
    *exit_code = -1;
  if (program.isEmpty() || !QFileInfo::exists(program))
    return {};
  QProcess proc;
  proc.setProcessEnvironment(tool_env_for(program));
  proc.setProgram(program);
  proc.setArguments(args);
  proc.start();
  if (!proc.waitForStarted(3000))
    return {};
  if (!proc.waitForFinished(timeout_ms)) {
    proc.kill();
    proc.waitForFinished(1500);
    return {};
  }
  if (exit_code)
    *exit_code = proc.exitCode();
  return proc.readAllStandardOutput() + proc.readAllStandardError();
}

QStringList text_extensions() {
  return {QStringLiteral("txt"), QStringLiteral("md"),   QStringLiteral("markdown"),
          QStringLiteral("rst"), QStringLiteral("log"),  QStringLiteral("csv"),
          QStringLiteral("tsv"), QStringLiteral("json"), QStringLiteral("xml"),
          QStringLiteral("yml"), QStringLiteral("yaml"), QStringLiteral("toml"),
          QStringLiteral("ini"), QStringLiteral("cfg"),  QStringLiteral("conf"),
          QStringLiteral("c"),   QStringLiteral("h"),    QStringLiteral("cpp"),
          QStringLiteral("hpp"), QStringLiteral("cc"),   QStringLiteral("hh"),
          QStringLiteral("py"),  QStringLiteral("js"),   QStringLiteral("ts"),
          QStringLiteral("qml"), QStringLiteral("java"), QStringLiteral("go"),
          QStringLiteral("rs"),  QStringLiteral("sh"),   QStringLiteral("bash"),
          QStringLiteral("zsh"), QStringLiteral("sql"),  QStringLiteral("html"),
          QStringLiteral("htm"), QStringLiteral("css"),  QStringLiteral("svg")};
}

QStringList office_extensions() {
  return {QStringLiteral("doc"),
          QStringLiteral("docx"),
          QStringLiteral("odt"),
          QStringLiteral("rtf"),
          QStringLiteral("xls"),
          QStringLiteral("xlsx"),
          QStringLiteral("ods"),
          QStringLiteral("ppt"),
          QStringLiteral("pptx"),
          QStringLiteral("odp")};
}

QString ext_of(const QString& path) {
  return QFileInfo(path).suffix().trimmed().toLower();
}

#if defined(Q_OS_WIN)
QString windows_soffice_path() {
  auto usable = [](const QString& path) -> bool {
    if (path.isEmpty())
      return false;
    const QFileInfo fi(path);
    return fi.exists() && fi.isFile();
  };

  const QString from_path = QStandardPaths::findExecutable(QStringLiteral("soffice"));
  if (usable(from_path))
    return from_path;

  for (const QString& root :
       {QStringLiteral("HKEY_LOCAL_MACHINE"), QStringLiteral("HKEY_CURRENT_USER")}) {
    QSettings app_paths(
        root + QStringLiteral(
                   "\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\soffice.exe"),
        QSettings::NativeFormat);
    const QString def = app_paths.value(QStringLiteral(".")).toString();
    if (usable(def))
      return def;
  }

  const QStringList candidates = {
      QDir(QString::fromLocal8Bit(qgetenv("ProgramFiles")))
          .filePath(QStringLiteral("LibreOffice/program/soffice.exe")),
      QDir(QString::fromLocal8Bit(qgetenv("ProgramFiles(x86)")))
          .filePath(QStringLiteral("LibreOffice/program/soffice.exe")),
      QDir(QString::fromLocal8Bit(qgetenv("LOCALAPPDATA")))
          .filePath(QStringLiteral("Programs/LibreOffice/program/soffice.exe")),
  };
  for (const QString& path : candidates) {
    if (usable(path))
      return path;
  }
  return {};
}
#endif

}

DocumentViewer::DocumentViewer(QObject* parent) : QObject(parent) {
  ensure_bundled_tools_executable();
}

DocumentViewer::~DocumentViewer() {
  killActiveProcess();
}

bool DocumentViewer::canHandle(const QString& path, const QString& mime) {
#if defined(Q_OS_ANDROID)
  Q_UNUSED(path);
  Q_UNUSED(mime);
  return false;
#else
  if (path.trimmed().isEmpty() || !QFileInfo::exists(path))
    return false;
  if (isTextLike(path, mime) || isPdf(path, mime) || isOffice(path, mime))
    return true;
  return false;
#endif
}

bool DocumentViewer::isTextLike(const QString& path, const QString& mime) {
  const QString m = mime.trimmed().toLower();
  if (m.startsWith(QLatin1String("text/")))
    return true;
  if (m == QLatin1String("application/json") || m == QLatin1String("application/xml") ||
      m == QLatin1String("application/x-yaml") || m == QLatin1String("application/javascript"))
    return true;
  return text_extensions().contains(ext_of(path));
}

bool DocumentViewer::isPdf(const QString& path, const QString& mime) {
  const QString m = mime.trimmed().toLower();
  if (m == QLatin1String("application/pdf") || m == QLatin1String("application/x-pdf"))
    return true;
  return ext_of(path) == QLatin1String("pdf");
}

bool DocumentViewer::isOffice(const QString& path, const QString& mime) {
  const QString m = mime.trimmed().toLower();
  if (m.contains(QLatin1String("officedocument")) || m.contains(QLatin1String("msword")) ||
      m.contains(QLatin1String("ms-excel")) || m.contains(QLatin1String("ms-powerpoint")) ||
      m.contains(QLatin1String("opendocument")))
    return true;
  return office_extensions().contains(ext_of(path));
}

DocumentViewer::Kind DocumentViewer::classify(const QString& path, const QString& mime) const {
  if (isPdf(path, mime))
    return Kind::Pdf;
  if (isOffice(path, mime))
    return Kind::Office;
  return Kind::Text;
}

QString DocumentViewer::findTool(const QStringList& names) {
  const QString app_dir = QCoreApplication::applicationDirPath();
  const QString tools = QDir(app_dir).filePath(QStringLiteral("tools"));

  auto usable = [](const QString& path) -> bool {
    if (path.isEmpty())
      return false;
    QFileInfo fi(path);
    return fi.exists() && fi.isFile() && fi.isExecutable();
  };

  for (const QString& name : names) {
#if defined(Q_OS_WIN)
    if (name == QLatin1String("soffice") || name == QLatin1String("libreoffice")) {
      const QString soffice = windows_soffice_path();
      if (!soffice.isEmpty() && QFileInfo::exists(soffice))
        return soffice;
    }
#else
    const QString abs = QStringLiteral("/usr/bin/") + name;
    if (usable(abs))
      return abs;
#endif
    const QString found = QStandardPaths::findExecutable(name);
    if (usable(found))
      return found;
  }
  for (const QString& name : names) {
#if defined(Q_OS_WIN)
    const QString bundled = QDir(tools).filePath(name + QStringLiteral(".exe"));
#else
    const QString bundled = QDir(tools).filePath(name);
#endif
    if (usable(bundled))
      return bundled;
  }
  return {};
}

void DocumentViewer::killActiveProcess() {
  if (!active_)
    return;
  active_->disconnect(this);
  if (active_->state() != QProcess::NotRunning) {
    active_->kill();
    active_->waitForFinished(1500);
  }
  active_->deleteLater();
  active_ = nullptr;
}

void DocumentViewer::resetState() {
  killActiveProcess();
  open_ = false;
  path_.clear();
  mime_.clear();
  title_.clear();
  mode_.clear();
  text_.clear();
  status_.clear();
  error_.clear();
  source_path_.clear();
  pdf_path_.clear();
  page_count_ = 0;
  page_ = 1;
  page_url_.clear();
  zoom_ = 1.0;
  cache_dir_.clear();
  temp_dir_.reset();
  ++render_gen_;
}

void DocumentViewer::emitChanged() {
  emit changed();
}

void DocumentViewer::setBusy(const QString& status) {
  mode_ = QStringLiteral("busy");
  status_ = status;
  error_.clear();
  emitChanged();
}

void DocumentViewer::setError(const QString& message) {
  mode_ = QStringLiteral("error");
  error_ = message;
  status_.clear();
  emitChanged();
  emit toast(message, true);
}

void DocumentViewer::close() {
  if (!open_ && path_.isEmpty())
    return;
  resetState();
  emitChanged();
}

bool DocumentViewer::openExternally() {
  const QString local = source_path_.isEmpty() ? path_ : source_path_;
  if (local.isEmpty() || !QFileInfo::exists(local))
    return false;
#if defined(Q_OS_LINUX)
  QProcess proc;
  proc.setProcessEnvironment(tool_env_for(QStringLiteral("/usr/bin/xdg-open")));
  proc.setProgram(QStringLiteral("/usr/bin/xdg-open"));
  proc.setArguments({QFileInfo(local).absoluteFilePath()});
  if (proc.startDetached())
    return true;
#endif
  return QDesktopServices::openUrl(QUrl::fromLocalFile(local));
}

bool DocumentViewer::openDocument(const QString& path, const QString& mime, const QString& title) {
#if defined(Q_OS_ANDROID)
  Q_UNUSED(path);
  Q_UNUSED(mime);
  Q_UNUSED(title);
  return false;
#else
  const QString local = path.trimmed();
  if (local.isEmpty() || !QFileInfo::exists(local)) {
    emit toast(QStringLiteral("Файл не найден"), true);
    return false;
  }
  QString use_mime = mime.trimmed();
  if (use_mime.isEmpty()) {
    use_mime = QMimeDatabase().mimeTypeForFile(local, QMimeDatabase::MatchExtension).name();
  }
  if (!canHandle(local, use_mime))
    return false;

  resetState();
  open_ = true;
  path_ = local;
  source_path_ = local;
  mime_ = use_mime;
  title_ = title.trimmed().isEmpty() ? QFileInfo(local).fileName() : title.trimmed();
  temp_dir_ = std::make_unique<QTemporaryDir>();
  if (!temp_dir_->isValid()) {
    setError(QStringLiteral("Не удалось создать временный каталог"));
    return true;
  }
  cache_dir_ = temp_dir_->path();

  const Kind kind = classify(local, use_mime);
  if (kind == Kind::Text) {
    if (!openText(local))
      setError(QStringLiteral("Не удалось прочитать файл"));
    return true;
  }
  if (kind == Kind::Pdf) {
    beginPdf(local);
    return true;
  }
  beginOfficeConvert(local);
  return true;
#endif
}

bool DocumentViewer::openText(const QString& path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
    return false;
  constexpr qint64 kMax = 2 * 1024 * 1024;
  QByteArray raw = f.read(kMax + 1);
  const bool truncated = raw.size() > kMax;
  if (truncated)
    raw.resize(kMax);
  text_ = QString::fromUtf8(raw);
  if (text_.contains(QChar::ReplacementCharacter)) {
    text_ = QString::fromLocal8Bit(raw);
  }
  if (truncated) {
    text_ += QStringLiteral("\n\n… файл обрезан (показаны первые 2 МБ)");
  }
  mode_ = QStringLiteral("text");
  status_.clear();
  error_.clear();
  emitChanged();
  return true;
}

void DocumentViewer::beginOfficeConvert(const QString& path) {
  const QString soffice = findTool({QStringLiteral("soffice"), QStringLiteral("libreoffice")});
  if (soffice.isEmpty()) {
    setError(QStringLiteral("Для Office-файлов нужен LibreOffice (soffice). Установите его или "
                            "откройте во внешней программе."));
    return;
  }
  setBusy(QStringLiteral("Конвертация в PDF…"));

  killActiveProcess();
  active_ = new QProcess(this);
  active_->setProgram(soffice);
  active_->setProcessEnvironment(tool_env_for(soffice));
  active_->setArguments({QStringLiteral("--headless"),
                         QStringLiteral("--nologo"),
                         QStringLiteral("--nofirststartwizard"),
                         QStringLiteral("--convert-to"),
                         QStringLiteral("pdf"),
                         QStringLiteral("--outdir"),
                         cache_dir_,
                         QFileInfo(path).absoluteFilePath()});
  connect(active_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
    QProcess* proc = active_;
    active_ = nullptr;
    if (proc)
      proc->deleteLater();
    if (!open_)
      return;
    if (status != QProcess::NormalExit || code != 0) {
      setError(QStringLiteral("LibreOffice не смог конвертировать файл"));
      return;
    }
    const QString base = QFileInfo(source_path_).completeBaseName() + QStringLiteral(".pdf");
    const QString out = QDir(cache_dir_).filePath(base);
    if (!QFileInfo::exists(out)) {

      const auto pdfs = QDir(cache_dir_).entryList({QStringLiteral("*.pdf")}, QDir::Files);
      if (pdfs.isEmpty()) {
        setError(QStringLiteral("PDF после конвертации не найден"));
        return;
      }
      beginPdf(QDir(cache_dir_).filePath(pdfs.first()));
      return;
    }
    beginPdf(out);
  });
  connect(active_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
    if (!open_ || !active_)
      return;
    killActiveProcess();
    setError(QStringLiteral("Не удалось запустить LibreOffice"));
  });
  active_->start();
}

void DocumentViewer::beginPdf(const QString& pdfPath) {
  pdf_path_ = pdfPath;
  path_ = source_path_.isEmpty() ? pdfPath : source_path_;
  setBusy(QStringLiteral("Чтение PDF…"));
  queryPageCount();
}

void DocumentViewer::queryPageCount() {
  ensure_bundled_tools_executable();
  const QString pdfinfo = findTool({QStringLiteral("pdfinfo")});
  const QString mutool = findTool({QStringLiteral("mutool")});
  if (pdfinfo.isEmpty() && mutool.isEmpty()) {
    setError(
        QStringLiteral("Нужен pdfinfo (poppler-utils) или mutool (mupdf-tools) для просмотра PDF"));
    return;
  }

  const QString program = !pdfinfo.isEmpty() ? pdfinfo : mutool;
  const QStringList args = !pdfinfo.isEmpty() ? QStringList {pdf_path_}
                                              : QStringList {QStringLiteral("info"), pdf_path_};
  const int gen = ++render_gen_;
  const QString pdf = pdf_path_;


  std::thread([this, program, args, gen, pdf]() {
    int code = -1;
    const QByteArray out = run_tool_capture(program, args, 15000, &code);
    QMetaObject::invokeMethod(
        this,
        [this, gen, out, code, pdf]() {
          if (!open_ || gen != render_gen_ || pdf_path_ != pdf)
            return;
          if (code != 0 || out.isEmpty()) {
            setError(QStringLiteral("Не удалось прочитать PDF"));
            return;
          }
          const QRegularExpression re(QStringLiteral("Pages:\\s*(\\d+)"),
                                      QRegularExpression::CaseInsensitiveOption);
          const auto m = re.match(QString::fromUtf8(out));
          if (!m.hasMatch()) {
            setError(QStringLiteral("Не удалось определить число страниц"));
            return;
          }
          page_count_ = m.captured(1).toInt();
          if (page_count_ < 1) {
            setError(QStringLiteral("Пустой PDF"));
            return;
          }
          page_ = 1;
          mode_ = QStringLiteral("pdf");
          status_.clear();
          error_.clear();
          emitChanged();
          renderCurrentPage();
        },
        Qt::QueuedConnection);
  }).detach();
}

void DocumentViewer::renderCurrentPage() {
  if (!open_ || pdf_path_.isEmpty() || page_ < 1 || page_ > page_count_)
    return;

  const int gen = ++render_gen_;
  const int dpi = qBound(0, static_cast<int>(std::lround(120.0 * zoom_)), 400);
  const int page = page_;
  const QString pdf = pdf_path_;
  const QString out =
      QDir(cache_dir_).filePath(QStringLiteral("page-%1-z%2.png").arg(page).arg(dpi));
  if (QFileInfo::exists(out)) {
    page_url_ = QUrl::fromLocalFile(out).toString();
    mode_ = QStringLiteral("pdf");
    status_.clear();
    emitChanged();
    return;
  }

  ensure_bundled_tools_executable();
  const QString mutool = findTool({QStringLiteral("mutool")});
  const QString pdftoppm = findTool({QStringLiteral("pdftoppm")});
  if (mutool.isEmpty() && pdftoppm.isEmpty()) {
    setError(QStringLiteral("Нужен mutool или pdftoppm для отрисовки страниц"));
    return;
  }

  status_ = QStringLiteral("Рендер страницы %1…").arg(page);
  emitChanged();

  const QString program = !mutool.isEmpty() ? mutool : pdftoppm;
  QStringList args;
  QString stem;
  if (!mutool.isEmpty()) {
    args = {QStringLiteral("draw"),
            QStringLiteral("-q"),
            QStringLiteral("-o"),
            out,
            QStringLiteral("-r"),
            QString::number(dpi),
            QStringLiteral("-F"),
            QStringLiteral("png"),
            pdf,
            QString::number(page)};
  } else {
    stem = QDir(cache_dir_).filePath(QStringLiteral("ppm-%1-z%2").arg(page).arg(dpi));
    args = {QStringLiteral("-png"),
            QStringLiteral("-f"),
            QString::number(page),
            QStringLiteral("-l"),
            QString::number(page),
            QStringLiteral("-r"),
            QString::number(dpi),
            pdf,
            stem};
  }

  std::thread([this, program, args, gen, out, stem, page, pdf, use_mutool = !mutool.isEmpty()]() {
    int code = -1;
    run_tool_capture(program, args, 30000, &code);
    QString page_file = out;
    if (!use_mutool) {
      const QString produced = stem + QStringLiteral("-") +
                               QStringLiteral("%1").arg(page, 2, 10, QChar('0')) +
                               QStringLiteral(".png");
      QString src = produced;
      if (!QFileInfo::exists(src)) {
        const auto pngs =
            QDir(QFileInfo(stem).absolutePath())
                .entryList({QFileInfo(stem).fileName() + QStringLiteral("*.png")}, QDir::Files);
        if (!pngs.isEmpty())
          src = QDir(QFileInfo(stem).absolutePath()).filePath(pngs.first());
      }
      if (QFileInfo::exists(src)) {
        if (!QFileInfo::exists(out))
          QFile::copy(src, out);
        page_file = QFileInfo::exists(out) ? out : src;
      } else {
        page_file.clear();
      }
    } else if (!QFileInfo::exists(out)) {
      page_file.clear();
    }

    QMetaObject::invokeMethod(
        this,
        [this, gen, page_file, code, pdf]() {
          if (!open_ || gen != render_gen_ || pdf_path_ != pdf)
            return;
          if (code != 0 || page_file.isEmpty() || !QFileInfo::exists(page_file)) {
            setError(QStringLiteral("Не удалось отрисовать страницу"));
            return;
          }
          page_url_ = QUrl::fromLocalFile(page_file).toString();
          mode_ = QStringLiteral("pdf");
          status_.clear();
          emitChanged();
        },
        Qt::QueuedConnection);
  }).detach();
}

void DocumentViewer::setPage(int page) {
  if (!open_ || mode_ == QStringLiteral("text"))
    return;
  const int p = qBound(1, page, qMax(1, page_count_));
  if (p == page_ && !page_url_.isEmpty())
    return;
  page_ = p;
  page_url_.clear();
  emitChanged();
  renderCurrentPage();
}

void DocumentViewer::nextPage() {
  setPage(page_ + 1);
}

void DocumentViewer::prevPage() {
  setPage(page_ - 1);
}

void DocumentViewer::setZoom(double zoom) {
  const double z = qBound(0.5, zoom, 3.0);
  if (std::fabs(z - zoom_) < 0.01)
    return;
  zoom_ = z;
  page_url_.clear();
  emitChanged();
  if (mode_ == QStringLiteral("pdf") || (!pdf_path_.isEmpty() && open_))
    renderCurrentPage();
}

void DocumentViewer::zoomIn() {
  setZoom(zoom_ + 0.25);
}

void DocumentViewer::zoomOut() {
  setZoom(zoom_ - 0.25);
}
