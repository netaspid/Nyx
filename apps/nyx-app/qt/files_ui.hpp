#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

#include <atomic>
#include <thread>

class FilesUi : public QObject {
  Q_OBJECT
public:
  explicit FilesUi(QObject* parent = nullptr) : QObject(parent) {}

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
};
