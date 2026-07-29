#include "files_ui.hpp"

#include "node_controller.hpp"

#include "nyx/file_access.hpp"

bool FilesUi::canFileList() const {
  return host_ ? host_->canFileList() : false;
}
bool FilesUi::fileExchangeReady() const {
  return host_ ? host_->fileExchangeReady() : false;
}
QString FilesUi::fileExchangeHint() const {
  return host_ ? host_->fileExchangeHint() : QString();
}
bool FilesUi::canManageFileRoles() const {
  return host_ ? host_->canManageFileRoles() : false;
}
bool FilesUi::canFileUpload() const {
  return host_ ? host_->canFileUpload() : false;
}
bool FilesUi::canFileDownload() const {
  return host_ ? host_->canFileDownload() : false;
}
bool FilesUi::canFileOpenRemote() const {
  return host_ ? host_->canFileOpenRemote() : false;
}
bool FilesUi::canManageFileShares() const {
  return host_ ? host_->canManageFileShares() : false;
}
bool FilesUi::canAddShareFolder() const {
  return host_ ? host_->canAddShareFolder() : false;
}
int FilesUi::permFileList() const {
  return static_cast<int>(nyx::FilePermission::List);
}
int FilesUi::permFileDownload() const {
  return static_cast<int>(nyx::FilePermission::Download);
}
int FilesUi::permFileUpload() const {
  return static_cast<int>(nyx::FilePermission::Upload);
}
int FilesUi::permFileDelete() const {
  return static_cast<int>(nyx::FilePermission::Delete);
}
int FilesUi::permFileOpenRemote() const {
  return static_cast<int>(nyx::FilePermission::OpenRemote);
}
int FilesUi::permFileManageShares() const {
  return static_cast<int>(nyx::FilePermission::ManageShares);
}
int FilesUi::permFileManageRoles() const {
  return static_cast<int>(nyx::FilePermission::ManageRoles);
}

void FilesUi::setFileSelectedShareRoot(const QString& path) {
  if (host_)
    host_->setFileSelectedShareRoot(path);
}
void FilesUi::setFilesSection(int section) {
  if (host_)
    host_->setFilesSection(section);
}
void FilesUi::setFileScopeGroupId(const QString& groupIdHex) {
  if (host_)
    host_->setFileScopeGroupId(groupIdHex);
}

QString FilesUi::pickFolder() {
  return host_ ? host_->pickFolder() : QString();
}
QString FilesUi::pickSaveFile(const QString& suggestedFileName) {
  return host_ ? host_->pickSaveFile(suggestedFileName) : QString();
}
QString FilesUi::pickSaveFolder() {
  return host_ ? host_->pickSaveFolder() : QString();
}
void FilesUi::refreshFileLists() {
  if (host_)
    host_->refreshFileLists();
}
bool FilesUi::hasFilePermission(int permissionBit) const {
  return host_ ? host_->hasFilePermission(permissionBit) : false;
}
void FilesUi::setMemberFileRole(const QString& userIdHex, const QString& roleId) {
  if (host_)
    host_->setMemberFileRole(userIdHex, roleId);
}
void FilesUi::createFileRole(const QString& name, int permissions) {
  if (host_)
    host_->createFileRole(name, permissions);
}
void FilesUi::updateFileRole(const QString& roleId, const QString& name, int permissions) {
  if (host_)
    host_->updateFileRole(roleId, name, permissions);
}
void FilesUi::deleteFileRole(const QString& roleId) {
  if (host_)
    host_->deleteFileRole(roleId);
}
void FilesUi::openRemoteFile(const QString& hashHex,
                             const QString& fileName,
                             const QString& rootPath,
                             const QString& relativePath) {
  if (host_)
    host_->openRemoteFile(hashHex, fileName, rootPath, relativePath);
}
void FilesUi::addIndexedFolder(const QString& path) {
  if (host_)
    host_->addIndexedFolder(path);
}
void FilesUi::browseIntoFolder(const QString& navPath, const QString& itemRootPath) {
  if (host_)
    host_->browseIntoFolder(navPath, itemRootPath);
}
void FilesUi::browseUp() {
  if (host_)
    host_->browseUp();
}
void FilesUi::browseToCrumb(int index) {
  if (host_)
    host_->browseToCrumb(index);
}
void FilesUi::toggleFileRolePermission(const QString& roleId, int permissionBit) {
  if (host_)
    host_->toggleFileRolePermission(roleId, permissionBit);
}
bool FilesUi::canEditFileRolePermissions(const QString& roleId) const {
  return host_ ? host_->canEditFileRolePermissions(roleId) : false;
}
void FilesUi::setFileAccessTarget(const QString& rootPath, const QString& relativePath) {
  if (host_)
    host_->setFileAccessTarget(rootPath, relativePath);
}
void FilesUi::setPathRole(const QString& roleId) {
  if (host_)
    host_->setPathRole(roleId);
}
void FilesUi::clearPathRole() {
  if (host_)
    host_->clearPathRole();
}
void FilesUi::createPermissionPreset(const QString& name, int permissions) {
  if (host_)
    host_->createPermissionPreset(name, permissions);
}
void FilesUi::deletePermissionPreset(const QString& presetId) {
  if (host_)
    host_->deletePermissionPreset(presetId);
}
void FilesUi::togglePermissionPresetBit(const QString& presetId, int permissionBit) {
  if (host_)
    host_->togglePermissionPresetBit(presetId, permissionBit);
}
void FilesUi::applyPresetToRole(const QString& presetId, const QString& roleId) {
  if (host_)
    host_->applyPresetToRole(presetId, roleId);
}
void FilesUi::setPathMemberFileRole(const QString& userIdHex, const QString& roleId) {
  if (host_)
    host_->setPathMemberFileRole(userIdHex, roleId);
}
void FilesUi::setPathGrantDirect(const QString& userIdHex) {
  if (host_)
    host_->setPathGrantDirect(userIdHex);
}
void FilesUi::clearPathMemberGrant(const QString& userIdHex) {
  if (host_)
    host_->clearPathMemberGrant(userIdHex);
}
void FilesUi::togglePathDirectPermission(const QString& userIdHex, int permissionBit) {
  if (host_)
    host_->togglePathDirectPermission(userIdHex, permissionBit);
}
void FilesUi::addDroppedUrls(const QVariantList& urls) {
  if (host_)
    host_->addDroppedUrls(urls);
}
void FilesUi::removeIndexedFolder(const QString& path) {
  if (host_)
    host_->removeIndexedFolder(path);
}
void FilesUi::rescanIndexedFolder(const QString& path) {
  if (host_)
    host_->rescanIndexedFolder(path);
}
void FilesUi::refreshRemoteFileList() {
  if (host_)
    host_->refreshRemoteFileList();
}
void FilesUi::downloadFile(const QString& hashHex,
                           const QString& fileName,
                           const QString& rootPath,
                           const QString& relativePath) {
  if (host_)
    host_->downloadFile(hashHex, fileName, rootPath, relativePath);
}
void FilesUi::downloadRemoteFolder(const QString& rootPath, const QString& relativePath) {
  if (host_)
    host_->downloadRemoteFolder(rootPath, relativePath);
}
void FilesUi::sendFileByHash(const QString& hashHex) {
  if (host_)
    host_->sendFileByHash(hashHex);
}
void FilesUi::openInAppMedia(const QString& path, const QString& mime, const QString& title) {
  if (host_)
    host_->openInAppMedia(path, mime, title);
}
void FilesUi::closeInAppMedia() {
  if (host_)
    host_->closeInAppMedia();
}
QString FilesUi::mediaLocalPath(const QString& hashHex) const {
  return host_ ? host_->mediaLocalPath(hashHex) : QString();
}
void FilesUi::ensureMediaAvailable(const QString& hashHex) {
  if (host_)
    host_->ensureMediaAvailable(hashHex);
}
bool FilesUi::isImageMedia(const QString& hashHex) const {
  return host_ ? host_->isImageMedia(hashHex) : false;
}
QString FilesUi::fileLocalPath(const QString& hashHex) const {
  return host_ ? host_->fileLocalPath(hashHex) : QString();
}
void FilesUi::ensureFileAvailable(const QString& hashHex, const QString& fileName) {
  if (host_)
    host_->ensureFileAvailable(hashHex, fileName);
}
QString FilesUi::fileTextPreview(const QString& hashHex) const {
  return host_ ? host_->fileTextPreview(hashHex) : QString();
}
bool FilesUi::openLocalFile(const QString& path, const QString& mime) {
  return host_ ? host_->openLocalFile(path, mime) : false;
}
void FilesUi::openFileByHash(const QString& hashHex,
                             const QString& fileName,
                             const QString& mime,
                             const QString& rootPath,
                             const QString& relativePath) {
  if (host_)
    host_->openFileByHash(hashHex, fileName, mime, rootPath, relativePath);
}
void FilesUi::linkFileToChat(const QString& hashHex,
                             const QString& fileName,
                             const QString& mime,
                             qulonglong size) {
  if (host_)
    host_->linkFileToChat(hashHex, fileName, mime, size);
}
void FilesUi::linkFolderToChat(const QString& hashHex,
                               const QString& folderName,
                               const QString& rootPath,
                               const QString& relativePath,
                               qulonglong size) {
  if (host_)
    host_->linkFolderToChat(hashHex, folderName, rootPath, relativePath, size);
}
void FilesUi::openFolderInResources(const QString& hashHex,
                                    const QString& rootPath,
                                    const QString& relativePath) {
  if (host_)
    host_->openFolderInResources(hashHex, rootPath, relativePath);
}
int FilesUi::fileSyncState(const QString& hashHex) const {
  return host_ ? host_->fileSyncState(hashHex) : 0;
}
void FilesUi::pauseFileTransfer(const QString& hashHex, bool paused) {
  if (host_)
    host_->pauseFileTransfer(hashHex, paused);
}
void FilesUi::cancelFileTransfer(const QString& hashHex) {
  if (host_)
    host_->cancelFileTransfer(hashHex);
}
void FilesUi::retryFileTransfer(const QString& hashHex) {
  if (host_)
    host_->retryFileTransfer(hashHex);
}
void FilesUi::moveFileTransfer(const QString& hashHex, int delta) {
  if (host_)
    host_->moveFileTransfer(hashHex, delta);
}
void FilesUi::importFiles() {
  if (host_)
    host_->importFiles();
}
void FilesUi::exportFile(const QString& hashHex, const QString& fileName, const QString& mime) {
  if (host_)
    host_->exportFile(hashHex, fileName, mime);
}
bool FilesUi::canDownloadFolderAt(const QString& rootPath, const QString& relativePath) const {
  return host_ ? host_->canDownloadFolderAt(rootPath, relativePath) : false;
}
