#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <thread>

class NodeController;

class FilesUi : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString fileProgressLabel READ fileProgressLabel NOTIFY fileProgressChanged)
  Q_PROPERTY(int fileProgressPercent READ fileProgressPercent NOTIFY fileProgressChanged)
  Q_PROPERTY(bool fileProgressVisible READ fileProgressVisible NOTIFY fileProgressChanged)
  Q_PROPERTY(QVariantList localFileList READ localFileList NOTIFY filesChanged)
  Q_PROPERTY(QVariantList remoteFileList READ remoteFileList NOTIFY filesChanged)
  Q_PROPERTY(QVariantList transferQueue READ transferQueue NOTIFY filesChanged)
  Q_PROPERTY(bool inAppMediaOpen READ inAppMediaOpen NOTIFY inAppMediaChanged)
  Q_PROPERTY(QString inAppMediaPath READ inAppMediaPath NOTIFY inAppMediaChanged)
  Q_PROPERTY(QString inAppMediaMime READ inAppMediaMime NOTIFY inAppMediaChanged)
  Q_PROPERTY(QString inAppMediaTitle READ inAppMediaTitle NOTIFY inAppMediaChanged)
  Q_PROPERTY(QVariantList fileShareRoots READ fileShareRoots NOTIFY filesChanged)
  Q_PROPERTY(QString fileSelectedShareRoot READ fileSelectedShareRoot WRITE setFileSelectedShareRoot
                 NOTIFY filesChanged)
  Q_PROPERTY(QString fileBrowsePath READ fileBrowsePath NOTIFY filesChanged)
  Q_PROPERTY(QVariantList fileBrowseCrumbs READ fileBrowseCrumbs NOTIFY filesChanged)
  Q_PROPERTY(QString fileResourcesRoot READ fileResourcesRoot NOTIFY filesChanged)
  Q_PROPERTY(QString fileRemoteBrowsePath READ fileRemoteBrowsePath NOTIFY filesChanged)
  Q_PROPERTY(QVariantList fileRemoteBrowseCrumbs READ fileRemoteBrowseCrumbs NOTIFY filesChanged)
  Q_PROPERTY(int filesSection READ filesSection WRITE setFilesSection NOTIFY filesChanged)
  Q_PROPERTY(bool canFileList READ canFileList NOTIFY fileAccessChanged)
  Q_PROPERTY(
      QString fileScopeGroupId READ fileScopeGroupId WRITE setFileScopeGroupId NOTIFY filesChanged)
  Q_PROPERTY(QString fileScopeLabel READ fileScopeLabel NOTIFY filesChanged)
  Q_PROPERTY(bool fileExchangeReady READ fileExchangeReady NOTIFY filesChanged)
  Q_PROPERTY(QString fileExchangeHint READ fileExchangeHint NOTIFY filesChanged)
  Q_PROPERTY(
      bool fileIndexProgressVisible READ fileIndexProgressVisible NOTIFY fileIndexProgressChanged)
  Q_PROPERTY(
      int fileIndexProgressPercent READ fileIndexProgressPercent NOTIFY fileIndexProgressChanged)
  Q_PROPERTY(
      QString fileIndexProgressLabel READ fileIndexProgressLabel NOTIFY fileIndexProgressChanged)
  Q_PROPERTY(QVariantList fileRoleList READ fileRoleList NOTIFY fileAccessChanged)
  Q_PROPERTY(
      QVariantList filePermissionPresetList READ filePermissionPresetList NOTIFY fileAccessChanged)
  Q_PROPERTY(QVariantList fileMemberAccess READ fileMemberAccess NOTIFY fileAccessChanged)
  Q_PROPERTY(QVariantList filePathMemberAccess READ filePathMemberAccess NOTIFY fileAccessChanged)
  Q_PROPERTY(QString filePathRoleId READ filePathRoleId NOTIFY fileAccessChanged)
  Q_PROPERTY(
      QString filePathRoleInheritedFrom READ filePathRoleInheritedFrom NOTIFY fileAccessChanged)
  Q_PROPERTY(QString fileAccessTargetLabel READ fileAccessTargetLabel NOTIFY fileAccessChanged)
  Q_PROPERTY(QString fileAccessTargetRoot READ fileAccessTargetRoot NOTIFY fileAccessChanged)
  Q_PROPERTY(QString fileAccessTargetRel READ fileAccessTargetRel NOTIFY fileAccessChanged)
  Q_PROPERTY(bool canManageFileRoles READ canManageFileRoles NOTIFY fileAccessChanged)
  Q_PROPERTY(bool canFileUpload READ canFileUpload NOTIFY fileAccessChanged)
  Q_PROPERTY(bool canFileDownload READ canFileDownload NOTIFY fileAccessChanged)
  Q_PROPERTY(bool canFileOpenRemote READ canFileOpenRemote NOTIFY fileAccessChanged)
  Q_PROPERTY(bool canManageFileShares READ canManageFileShares NOTIFY fileAccessChanged)
  Q_PROPERTY(bool canAddShareFolder READ canAddShareFolder NOTIFY fileAccessChanged)
  Q_PROPERTY(int permFileList READ permFileList CONSTANT)
  Q_PROPERTY(int permFileDownload READ permFileDownload CONSTANT)
  Q_PROPERTY(int permFileUpload READ permFileUpload CONSTANT)
  Q_PROPERTY(int permFileDelete READ permFileDelete CONSTANT)
  Q_PROPERTY(int permFileOpenRemote READ permFileOpenRemote CONSTANT)
  Q_PROPERTY(int permFileManageShares READ permFileManageShares CONSTANT)
  Q_PROPERTY(int permFileManageRoles READ permFileManageRoles CONSTANT)

public:
  explicit FilesUi(QObject* parent = nullptr) : QObject(parent) {}

  void setHost(NodeController* host) { host_ = host; }

  QString fileProgressLabel() const { return file_progress_label_; }
  int fileProgressPercent() const { return file_progress_percent_; }
  bool fileProgressVisible() const { return file_progress_visible_; }
  QVariantList localFileList() const { return local_file_list_; }
  QVariantList remoteFileList() const { return remote_file_list_; }
  QVariantList transferQueue() const { return transfer_queue_; }
  bool inAppMediaOpen() const { return in_app_media_open_; }
  QString inAppMediaPath() const { return in_app_media_path_; }
  QString inAppMediaMime() const { return in_app_media_mime_; }
  QString inAppMediaTitle() const { return in_app_media_title_; }
  QVariantList fileShareRoots() const { return file_share_roots_; }
  QString fileSelectedShareRoot() const { return file_selected_share_root_; }
  QString fileBrowsePath() const { return file_browse_path_; }
  QVariantList fileBrowseCrumbs() const { return file_browse_crumbs_; }
  QString fileResourcesRoot() const { return file_resources_root_; }
  QString fileRemoteBrowsePath() const { return file_remote_browse_path_; }
  QVariantList fileRemoteBrowseCrumbs() const { return file_remote_browse_crumbs_; }
  int filesSection() const { return files_section_; }
  QString fileScopeGroupId() const { return file_scope_group_id_; }
  QString fileScopeLabel() const { return file_scope_label_; }
  bool fileIndexProgressVisible() const { return file_index_progress_visible_; }
  int fileIndexProgressPercent() const { return file_index_progress_percent_; }
  QString fileIndexProgressLabel() const { return file_index_progress_label_; }
  QVariantList fileRoleList() const { return file_role_list_; }
  QVariantList filePermissionPresetList() const { return file_permission_preset_list_; }
  QVariantList fileMemberAccess() const { return file_member_access_; }
  QVariantList filePathMemberAccess() const { return file_path_member_access_; }
  QString filePathRoleId() const { return file_path_role_id_; }
  QString filePathRoleInheritedFrom() const { return file_path_role_inherited_from_; }
  QString fileAccessTargetLabel() const { return file_access_target_label_; }
  QString fileAccessTargetRoot() const { return file_access_target_root_; }
  QString fileAccessTargetRel() const { return file_access_target_rel_; }

  bool canFileList() const;
  bool fileExchangeReady() const;
  QString fileExchangeHint() const;
  bool canManageFileRoles() const;
  bool canFileUpload() const;
  bool canFileDownload() const;
  bool canFileOpenRemote() const;
  bool canManageFileShares() const;
  bool canAddShareFolder() const;
  int permFileList() const;
  int permFileDownload() const;
  int permFileUpload() const;
  int permFileDelete() const;
  int permFileOpenRemote() const;
  int permFileManageShares() const;
  int permFileManageRoles() const;

  void setFileSelectedShareRoot(const QString& path);
  void setFilesSection(int section);
  void setFileScopeGroupId(const QString& groupIdHex);

  Q_INVOKABLE QString pickFolder();
  Q_INVOKABLE QString pickSaveFile(const QString& suggestedFileName);
  Q_INVOKABLE QString pickSaveFolder();
  Q_INVOKABLE void refreshFileLists();
  Q_INVOKABLE bool hasFilePermission(int permissionBit) const;
  Q_INVOKABLE void setMemberFileRole(const QString& userIdHex, const QString& roleId);
  Q_INVOKABLE void createFileRole(const QString& name, int permissions);
  Q_INVOKABLE void updateFileRole(const QString& roleId, const QString& name, int permissions);
  Q_INVOKABLE void deleteFileRole(const QString& roleId);
  Q_INVOKABLE void openRemoteFile(const QString& hashHex,
                                  const QString& fileName = {},
                                  const QString& rootPath = {},
                                  const QString& relativePath = {});
  Q_INVOKABLE void addIndexedFolder(const QString& path);
  Q_INVOKABLE void browseIntoFolder(const QString& navPath, const QString& itemRootPath = {});
  Q_INVOKABLE void browseUp();
  Q_INVOKABLE void browseToCrumb(int index);
  Q_INVOKABLE void toggleFileRolePermission(const QString& roleId, int permissionBit);
  Q_INVOKABLE bool canEditFileRolePermissions(const QString& roleId) const;
  Q_INVOKABLE void setFileAccessTarget(const QString& rootPath, const QString& relativePath);
  Q_INVOKABLE void setPathRole(const QString& roleId);
  Q_INVOKABLE void clearPathRole();
  Q_INVOKABLE void createPermissionPreset(const QString& name, int permissions);
  Q_INVOKABLE void deletePermissionPreset(const QString& presetId);
  Q_INVOKABLE void togglePermissionPresetBit(const QString& presetId, int permissionBit);
  Q_INVOKABLE void applyPresetToRole(const QString& presetId, const QString& roleId);
  Q_INVOKABLE void setPathMemberFileRole(const QString& userIdHex, const QString& roleId);
  Q_INVOKABLE void setPathGrantDirect(const QString& userIdHex);
  Q_INVOKABLE void clearPathMemberGrant(const QString& userIdHex);
  Q_INVOKABLE void togglePathDirectPermission(const QString& userIdHex, int permissionBit);
  Q_INVOKABLE void addDroppedUrls(const QVariantList& urls);
  Q_INVOKABLE void removeIndexedFolder(const QString& path);
  Q_INVOKABLE void rescanIndexedFolder(const QString& path);
  Q_INVOKABLE void refreshRemoteFileList();
  Q_INVOKABLE void downloadFile(const QString& hashHex,
                                const QString& fileName = {},
                                const QString& rootPath = {},
                                const QString& relativePath = {});
  Q_INVOKABLE void downloadRemoteFolder(const QString& rootPath, const QString& relativePath);
  Q_INVOKABLE void sendFileByHash(const QString& hashHex);
  Q_INVOKABLE void
  openInAppMedia(const QString& path, const QString& mime = {}, const QString& title = {});
  Q_INVOKABLE void closeInAppMedia();
  Q_INVOKABLE QString mediaLocalPath(const QString& hashHex) const;
  Q_INVOKABLE void ensureMediaAvailable(const QString& hashHex);
  Q_INVOKABLE bool isImageMedia(const QString& hashHex) const;
  Q_INVOKABLE QString fileLocalPath(const QString& hashHex) const;
  Q_INVOKABLE void ensureFileAvailable(const QString& hashHex, const QString& fileName);
  Q_INVOKABLE QString fileTextPreview(const QString& hashHex) const;
  Q_INVOKABLE bool openLocalFile(const QString& path, const QString& mime = {});
  Q_INVOKABLE void openFileByHash(const QString& hashHex,
                                  const QString& fileName = {},
                                  const QString& mime = {},
                                  const QString& rootPath = {},
                                  const QString& relativePath = {});
  Q_INVOKABLE void linkFileToChat(const QString& hashHex,
                                  const QString& fileName,
                                  const QString& mime,
                                  qulonglong size);
  Q_INVOKABLE void linkFolderToChat(const QString& hashHex,
                                    const QString& folderName,
                                    const QString& rootPath,
                                    const QString& relativePath,
                                    qulonglong size);
  Q_INVOKABLE void openFolderInResources(const QString& hashHex,
                                         const QString& rootPath = {},
                                         const QString& relativePath = {});
  Q_INVOKABLE int fileSyncState(const QString& hashHex) const;
  Q_INVOKABLE void pauseFileTransfer(const QString& hashHex, bool paused);
  Q_INVOKABLE void cancelFileTransfer(const QString& hashHex);
  Q_INVOKABLE void retryFileTransfer(const QString& hashHex);
  Q_INVOKABLE void moveFileTransfer(const QString& hashHex, int delta);
  Q_INVOKABLE void importFiles();
  Q_INVOKABLE void exportFile(const QString& hashHex, const QString& fileName, const QString& mime);
  Q_INVOKABLE bool canDownloadFolderAt(const QString& rootPath, const QString& relativePath) const;

  void notifyFileProgressChanged() { emit fileProgressChanged(); }
  void notifyFilesChanged() { emit filesChanged(); }
  void notifyInAppMediaChanged() { emit inAppMediaChanged(); }
  void notifyFileIndexProgressChanged() { emit fileIndexProgressChanged(); }
  void notifyFileAccessChanged() { emit fileAccessChanged(); }

signals:
  void fileProgressChanged();
  void filesChanged();
  void inAppMediaChanged();
  void fileIndexProgressChanged();
  void fileAccessChanged();

public:
  QString file_progress_label_;
  int file_progress_percent_ = 0;
  bool file_progress_visible_ = false;
  QString file_scope_group_id_;
  QString file_scope_label_;
  QVariantList file_share_roots_;
  QString file_selected_share_root_;
  QString file_browse_path_;
  QVariantList file_browse_crumbs_;
  QString file_resources_root_;
  QString file_remote_browse_path_;
  QVariantList file_remote_browse_crumbs_;
  QVariantList local_file_list_;
  QVariantList remote_file_list_;
  QVariantList transfer_queue_;
  bool in_app_media_open_ = false;
  QString in_app_media_path_;
  QString in_app_media_mime_;
  QString in_app_media_title_;
  int files_section_ = 0;
  bool file_index_progress_visible_ = false;
  int file_index_progress_percent_ = 0;
  QString file_index_progress_label_;
  int file_index_files_scanned_ = 0;
  std::atomic<bool> file_index_busy_ {false};
  std::thread file_index_thread_;
  QVariantList file_role_list_;
  QVariantList file_permission_preset_list_;
  QVariantList file_member_access_;
  QVariantList file_path_member_access_;
  QString file_path_role_id_;
  QString file_path_role_inherited_from_;
  QString file_access_target_root_;
  QString file_access_target_rel_;
  QString file_access_target_label_;

private:
  NodeController* host_ = nullptr;
};
