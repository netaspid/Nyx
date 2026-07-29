#pragma once

#include <QObject>
#include <QString>
#include <QTemporaryDir>

#include <memory>

class QProcess;

/** Desktop in-app document viewer (text / PDF / office→PDF). No WebView. */
class DocumentViewer : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool open READ isOpen NOTIFY changed)
  Q_PROPERTY(QString path READ path NOTIFY changed)
  Q_PROPERTY(QString mime READ mime NOTIFY changed)
  Q_PROPERTY(QString title READ title NOTIFY changed)
  /** text | pdf | busy | error */
  Q_PROPERTY(QString mode READ mode NOTIFY changed)
  Q_PROPERTY(QString text READ text NOTIFY changed)
  Q_PROPERTY(QString status READ status NOTIFY changed)
  Q_PROPERTY(QString error READ error NOTIFY changed)
  Q_PROPERTY(int pageCount READ pageCount NOTIFY changed)
  Q_PROPERTY(int page READ page WRITE setPage NOTIFY changed)
  Q_PROPERTY(QString pageUrl READ pageUrl NOTIFY changed)
  Q_PROPERTY(double zoom READ zoom WRITE setZoom NOTIFY changed)
  Q_PROPERTY(bool canPrev READ canPrev NOTIFY changed)
  Q_PROPERTY(bool canNext READ canNext NOTIFY changed)

public:
  explicit DocumentViewer(QObject* parent = nullptr);
  ~DocumentViewer() override;

  bool isOpen() const { return open_; }
  QString path() const { return path_; }
  QString mime() const { return mime_; }
  QString title() const { return title_; }
  QString mode() const { return mode_; }
  QString text() const { return text_; }
  QString status() const { return status_; }
  QString error() const { return error_; }
  int pageCount() const { return page_count_; }
  int page() const { return page_; }
  QString pageUrl() const { return page_url_; }
  double zoom() const { return zoom_; }
  bool canPrev() const { return page_ > 1; }
  bool canNext() const { return page_ < page_count_; }

  Q_INVOKABLE bool
  openDocument(const QString& path, const QString& mime = {}, const QString& title = {});
  Q_INVOKABLE void close();
  Q_INVOKABLE void setPage(int page);
  Q_INVOKABLE void nextPage();
  Q_INVOKABLE void prevPage();
  Q_INVOKABLE void setZoom(double zoom);
  Q_INVOKABLE void zoomIn();
  Q_INVOKABLE void zoomOut();
  Q_INVOKABLE bool openExternally();

  /** True when desktop in-app viewer should handle this file. */
  static bool canHandle(const QString& path, const QString& mime);

signals:
  void changed();
  void toast(const QString& message, bool isError);

private:
  enum class Kind { Text, Pdf, Office };

  void resetState();
  void setBusy(const QString& status);
  void setError(const QString& message);
  void emitChanged();

  Kind classify(const QString& path, const QString& mime) const;
  bool openText(const QString& path);
  void beginPdf(const QString& pdfPath);
  void beginOfficeConvert(const QString& path);
  void queryPageCount();
  void renderCurrentPage();
  void killActiveProcess();

  static QString findTool(const QStringList& names);
  static bool isTextLike(const QString& path, const QString& mime);
  static bool isPdf(const QString& path, const QString& mime);
  static bool isOffice(const QString& path, const QString& mime);

  bool open_ = false;
  QString path_;
  QString mime_;
  QString title_;
  QString mode_;
  QString text_;
  QString status_;
  QString error_;
  QString source_path_;
  QString pdf_path_;
  int page_count_ = 0;
  int page_ = 1;
  QString page_url_;
  double zoom_ = 1.0;
  QString cache_dir_;
  std::unique_ptr<QTemporaryDir> temp_dir_;
  QProcess* active_ = nullptr;
  int render_gen_ = 0;
};
