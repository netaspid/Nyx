#pragma once

#include "chat_list_model.hpp"
#include "lan_peer_model.hpp"
#include "message_model.hpp"
#include "../appcore/node_service.hpp"

#include "nyx/file_access.hpp"
#include "nyx/file_index.hpp"

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <thread>
#include <vector>

class QMenu;
class QSystemTrayIcon;

/** Qt-обёртка над NodeService: свойства и сигналы для QML. */
class NodeController : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString profileNickname READ profileNickname WRITE setNickname NOTIFY profileChanged)
  Q_PROPERTY(QString profileIdShort READ profileIdShort NOTIFY profileChanged)
  Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
  Q_PROPERTY(QString inviteToken READ inviteToken NOTIFY inviteTokenChanged)
  Q_PROPERTY(QString lastGroupInvite READ lastGroupInvite NOTIFY lastGroupInviteChanged)
  Q_PROPERTY(QString peerTitle READ peerTitle NOTIFY chatChanged)
  Q_PROPERTY(QString peerConnectionLabel READ peerConnectionLabel NOTIFY chatChanged)
  Q_PROPERTY(QString peerStatusText READ peerStatusText NOTIFY chatChanged)
  Q_PROPERTY(int activeChatKind READ activeChatKind NOTIFY chatChanged)
  Q_PROPERTY(QString activeChatRefId READ activeChatRefId NOTIFY chatChanged)
  Q_PROPERTY(bool activeFieldIsOwner READ activeFieldIsOwner NOTIFY chatChanged)
  Q_PROPERTY(bool autoStartOwnedHub READ autoStartOwnedHub WRITE setAutoStartOwnedHub
                 NOTIFY networkSettingsChanged)
  Q_PROPERTY(QString profileUserId READ profileUserIdHex NOTIFY profileChanged)
  Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
  Q_PROPERTY(bool listening READ listening NOTIFY listeningChanged)
  Q_PROPERTY(bool inChat READ inChat NOTIFY chatChanged)
  Q_PROPERTY(bool canSendMessage READ canSendMessage NOTIFY chatChanged)
  Q_PROPERTY(bool sessionUnlocked READ sessionUnlocked NOTIFY sessionUnlockedChanged)
  Q_PROPERTY(QVariantList accountList READ accountList NOTIFY accountGateChanged)
  Q_PROPERTY(QString accountGateError READ accountGateError NOTIFY accountGateChanged)
  Q_PROPERTY(bool legacyProfilePending READ legacyProfilePending NOTIFY accountGateChanged)
  Q_PROPERTY(QString pendingRecoveryPhrase READ pendingRecoveryPhrase NOTIFY accountGateChanged)
  Q_PROPERTY(QString lastAccountId READ lastAccountId NOTIFY accountGateChanged)
  Q_PROPERTY(bool needsRecoveryConfirm READ needsRecoveryConfirm NOTIFY accountGateChanged)
  Q_PROPERTY(bool needsOnboarding READ needsOnboarding NOTIFY profileChanged)
  Q_PROPERTY(MessageModel* messages READ messages CONSTANT)
  Q_PROPERTY(ChatListModel* chatList READ chatList CONSTANT)
  Q_PROPERTY(LanPeerModel* lanPeers READ lanPeers CONSTANT)
  Q_PROPERTY(QVariantList groupList READ groupList NOTIFY groupListChanged)
  Q_PROPERTY(QVariantList contactList READ contactList NOTIFY contactListChanged)
  Q_PROPERTY(QString rendezvous READ rendezvous WRITE setRendezvous NOTIFY rendezvousChanged)
  Q_PROPERTY(QString rendezvousList READ rendezvousList WRITE setRendezvousList NOTIFY rendezvousChanged)
  Q_PROPERTY(int discoveryMode READ discoveryMode WRITE setDiscoveryMode NOTIFY networkSettingsChanged)
  Q_PROPERTY(QString networkStatus READ networkStatus NOTIFY networkSettingsChanged)
  Q_PROPERTY(QString toast READ toast NOTIFY toastChanged)
  Q_PROPERTY(bool windowActive READ windowActive WRITE setWindowActive NOTIFY windowActiveChanged)
  Q_PROPERTY(QString fileProgressLabel READ fileProgressLabel NOTIFY fileProgressChanged)
  Q_PROPERTY(int fileProgressPercent READ fileProgressPercent NOTIFY fileProgressChanged)
  Q_PROPERTY(bool fileProgressVisible READ fileProgressVisible NOTIFY fileProgressChanged)
  Q_PROPERTY(QVariantList localFileList READ localFileList NOTIFY filesChanged)
  Q_PROPERTY(QVariantList remoteFileList READ remoteFileList NOTIFY filesChanged)
  Q_PROPERTY(QVariantList fileShareRoots READ fileShareRoots NOTIFY filesChanged)
  Q_PROPERTY(QString fileSelectedShareRoot READ fileSelectedShareRoot WRITE setFileSelectedShareRoot
                 NOTIFY filesChanged)
  Q_PROPERTY(QString fileBrowsePath READ fileBrowsePath NOTIFY filesChanged)
  Q_PROPERTY(QVariantList fileBrowseCrumbs READ fileBrowseCrumbs NOTIFY filesChanged)
  Q_PROPERTY(QString fileResourcesRoot READ fileResourcesRoot NOTIFY filesChanged)
  Q_PROPERTY(QVariantList fileRemoteBrowseCrumbs READ fileRemoteBrowseCrumbs NOTIFY filesChanged)
  Q_PROPERTY(int filesSection READ filesSection WRITE setFilesSection NOTIFY filesChanged)
  Q_PROPERTY(bool canFileList READ canFileList NOTIFY fileAccessChanged)
  Q_PROPERTY(bool canRemoveShareFolder READ canRemoveShareFolder NOTIFY fileAccessChanged)
  Q_PROPERTY(QString fileScopeGroupId READ fileScopeGroupId WRITE setFileScopeGroupId
                 NOTIFY filesChanged)
  Q_PROPERTY(QString fileScopeLabel READ fileScopeLabel NOTIFY filesChanged)
  Q_PROPERTY(bool fileExchangeReady READ fileExchangeReady NOTIFY filesChanged)
  Q_PROPERTY(QString fileExchangeHint READ fileExchangeHint NOTIFY filesChanged)
  Q_PROPERTY(bool fileIndexProgressVisible READ fileIndexProgressVisible NOTIFY fileIndexProgressChanged)
  Q_PROPERTY(int fileIndexProgressPercent READ fileIndexProgressPercent NOTIFY fileIndexProgressChanged)
  Q_PROPERTY(QString fileIndexProgressLabel READ fileIndexProgressLabel NOTIFY fileIndexProgressChanged)
  Q_PROPERTY(int mainViewMode READ mainViewMode WRITE setMainViewMode NOTIFY mainViewModeChanged)
  Q_PROPERTY(QVariantList fileRoleList READ fileRoleList NOTIFY fileAccessChanged)
  Q_PROPERTY(QVariantList filePermissionPresetList READ filePermissionPresetList NOTIFY fileAccessChanged)
  Q_PROPERTY(QVariantList fileMemberAccess READ fileMemberAccess NOTIFY fileAccessChanged)
  Q_PROPERTY(QVariantList filePathMemberAccess READ filePathMemberAccess NOTIFY fileAccessChanged)
  Q_PROPERTY(QString filePathRoleId READ filePathRoleId NOTIFY fileAccessChanged)
  Q_PROPERTY(QString filePathRoleInheritedFrom READ filePathRoleInheritedFrom NOTIFY fileAccessChanged)
  Q_PROPERTY(QString fileAccessTargetLabel READ fileAccessTargetLabel NOTIFY fileAccessChanged)
  Q_PROPERTY(QString fileAccessTargetRoot READ fileAccessTargetRoot NOTIFY fileAccessChanged)
  Q_PROPERTY(QString fileAccessTargetRel READ fileAccessTargetRel NOTIFY fileAccessChanged)
  Q_PROPERTY(bool toastIsError READ toastIsError NOTIFY toastChanged)
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
  Q_PROPERTY(bool connectionPanelOpen READ connectionPanelOpen WRITE setConnectionPanelOpen
                 NOTIFY connectionPanelOpenChanged)
  Q_PROPERTY(bool groupsDialogOpen READ groupsDialogOpen WRITE setGroupsDialogOpen
                 NOTIFY groupsDialogOpenChanged)
  Q_PROPERTY(bool fieldInfoOpen READ fieldInfoOpen WRITE setFieldInfoOpen
                 NOTIFY fieldInfoOpenChanged)
  Q_PROPERTY(QString fieldInfoGroupId READ fieldInfoGroupId NOTIFY fieldInfoOpenChanged)
  Q_PROPERTY(QString fieldInfoInvite READ fieldInfoInvite NOTIFY fieldInfoOpenChanged)
  Q_PROPERTY(bool fieldInfoIsOwner READ fieldInfoIsOwner NOTIFY fieldInfoOpenChanged)
  Q_PROPERTY(QString fieldInfoDescription READ fieldInfoDescription NOTIFY fieldInfoOpenChanged)
  Q_PROPERTY(QString fieldInfoDirection READ fieldInfoDirection NOTIFY fieldInfoOpenChanged)
  Q_PROPERTY(QString fieldInfoTags READ fieldInfoTags NOTIFY fieldInfoOpenChanged)
  Q_PROPERTY(bool fieldInfoPublicListed READ fieldInfoPublicListed NOTIFY fieldInfoOpenChanged)
  Q_PROPERTY(QVariantList fieldInfoMembers READ fieldInfoMembers NOTIFY fieldInfoOpenChanged)
  Q_PROPERTY(bool peerInfoOpen READ peerInfoOpen WRITE setPeerInfoOpen
                 NOTIFY peerInfoOpenChanged)
  Q_PROPERTY(QString peerInfoUserId READ peerInfoUserId NOTIFY peerInfoOpenChanged)
  Q_PROPERTY(bool trayAvailable READ trayAvailable CONSTANT)
  Q_PROPERTY(QString activeChatKey READ activeChatKey NOTIFY chatChanged)
  Q_PROPERTY(QString sessionSummary READ sessionSummary NOTIFY sessionsChanged)
  Q_PROPERTY(QString dmInboxToken READ dmInboxToken NOTIFY inviteTokenChanged)
  /** 0=чаты, 1=друзья, 2=поля — режим левого списка. */
  Q_PROPERTY(int sidebarMode READ sidebarMode WRITE setSidebarMode NOTIFY sidebarModeChanged)
  Q_PROPERTY(QString profileBio READ profileBio WRITE setProfileBio NOTIFY profileMetaChanged)
  Q_PROPERTY(QString profileInterests READ profileInterests WRITE setProfileInterests
                 NOTIFY profileMetaChanged)
  /** available | away | busy | invisible */
  Q_PROPERTY(QString profileAvailability READ profileAvailability WRITE setProfileAvailability
                 NOTIFY profileMetaChanged)
  Q_PROPERTY(QString profileAvailabilityLabel READ profileAvailabilityLabel
                 NOTIFY profileMetaChanged)
  Q_PROPERTY(QString profileAvatarPath READ profileAvatarPath NOTIFY profilePhotosChanged)
  Q_PROPERTY(QVariantList profilePhotoList READ profilePhotoList NOTIFY profilePhotosChanged)

 public:
  explicit NodeController(QObject* parent = nullptr);
  ~NodeController() override;

  QString profileNickname() const { return profile_nickname_; }
  QString profileIdShort() const { return profile_id_short_; }
  QString statusText() const { return status_text_; }
  QString inviteToken() const { return invite_token_; }
  QString lastGroupInvite() const { return last_group_invite_; }
  QString peerTitle() const { return peer_title_; }
  QString peerConnectionLabel() const { return peer_connection_label_; }
  QString peerStatusText() const { return peer_status_text_; }
  int activeChatKind() const { return active_chat_kind_; }
  QString activeChatRefId() const { return active_chat_ref_id_; }
  bool activeFieldIsOwner() const;
  bool autoStartOwnedHub() const { return auto_start_owned_hub_; }
  QString profileUserIdHex() const { return profile_user_id_hex_; }
  bool busy() const { return service_.busy(); }
  bool listening() const { return service_.mode() == nyx_app::NodeMode::Listening; }
  bool inChat() const { return in_chat_; }
  bool canSendMessage() const;
  bool sessionUnlocked() const { return session_unlocked_; }
  QVariantList accountList() const { return account_list_; }
  QString accountGateError() const { return account_gate_error_; }
  bool legacyProfilePending() const { return legacy_profile_pending_; }
  QString pendingRecoveryPhrase() const { return pending_recovery_phrase_; }
  QString lastAccountId() const { return last_account_id_; }
  bool needsRecoveryConfirm() const { return !pending_recovery_phrase_.isEmpty(); }
  bool needsOnboarding() const { return needs_onboarding_; }
  MessageModel* messages() { return &messages_; }
  ChatListModel* chatList() { return &chat_list_; }
  LanPeerModel* lanPeers() { return &lan_peers_; }
  QVariantList groupList() const { return group_list_; }
  QVariantList contactList() const { return contact_list_; }
  int sidebarMode() const { return sidebar_mode_; }
  void setSidebarMode(int mode);
  QString profileBio() const { return profile_bio_; }
  void setProfileBio(const QString& v);
  QString profileInterests() const { return profile_interests_; }
  void setProfileInterests(const QString& v);
  QString profileAvailability() const { return profile_availability_; }
  void setProfileAvailability(const QString& v);
  QString profileAvailabilityLabel() const;
  QString profileAvatarPath() const { return profile_avatar_path_; }
  QVariantList profilePhotoList() const { return profile_photo_list_; }
  QString rendezvous() const { return rendezvous_; }
  QString rendezvousList() const { return rendezvous_list_; }
  int discoveryMode() const { return discovery_mode_; }
  QString networkStatus() const { return network_status_; }
  QString profilePath() const { return profile_path_; }
  QString toast() const { return toast_; }
  bool windowActive() const { return window_active_; }
  QString fileProgressLabel() const { return file_progress_label_; }
  int fileProgressPercent() const { return file_progress_percent_; }
  bool fileProgressVisible() const { return file_progress_visible_; }
  QVariantList localFileList() const { return local_file_list_; }
  QVariantList remoteFileList() const { return remote_file_list_; }
  QVariantList fileShareRoots() const { return file_share_roots_; }
  QString fileSelectedShareRoot() const { return file_selected_share_root_; }
  QString fileBrowsePath() const { return file_browse_path_; }
  QVariantList fileBrowseCrumbs() const { return file_browse_crumbs_; }
  QString fileResourcesRoot() const { return file_resources_root_; }
  QVariantList fileRemoteBrowseCrumbs() const { return file_remote_browse_crumbs_; }
  int filesSection() const { return files_section_; }
  bool canFileList() const;
  bool canRemoveShareFolder() const;
  QString fileScopeGroupId() const { return file_scope_group_id_; }
  QString fileScopeLabel() const { return file_scope_label_; }
  bool fileExchangeReady() const;
  QString fileExchangeHint() const;
  bool fileIndexProgressVisible() const { return file_index_progress_visible_; }
  int fileIndexProgressPercent() const { return file_index_progress_percent_; }
  QString fileIndexProgressLabel() const { return file_index_progress_label_; }
  int mainViewMode() const { return main_view_mode_; }
  QVariantList fileRoleList() const { return file_role_list_; }
  QVariantList filePermissionPresetList() const { return file_permission_preset_list_; }
  QVariantList fileMemberAccess() const { return file_member_access_; }
  QVariantList filePathMemberAccess() const { return file_path_member_access_; }
  QString filePathRoleId() const { return file_path_role_id_; }
  QString filePathRoleInheritedFrom() const { return file_path_role_inherited_from_; }
  QString fileAccessTargetLabel() const { return file_access_target_label_; }
  QString fileAccessTargetRoot() const { return file_access_target_root_; }
  QString fileAccessTargetRel() const { return file_access_target_rel_; }
  bool toastIsError() const { return toast_is_error_; }
  bool canManageFileRoles() const;
  bool canFileUpload() const;
  bool canFileDownload() const;
  bool canFileDownloadAt(const QString& rootPath, const QString& relativePath) const;
  Q_INVOKABLE bool canDownloadFolderAt(const QString& rootPath,
                                       const QString& relativePath) const;
  bool canFileOpenRemote() const;
  bool canFileOpenRemoteAt(const QString& rootPath, const QString& relativePath) const;
  bool canManageFileShares() const;
  bool canAddShareFolder() const;
  int permFileList() const { return static_cast<int>(nyx::FilePermission::List); }
  int permFileDownload() const { return static_cast<int>(nyx::FilePermission::Download); }
  int permFileUpload() const { return static_cast<int>(nyx::FilePermission::Upload); }
  int permFileDelete() const { return static_cast<int>(nyx::FilePermission::Delete); }
  int permFileOpenRemote() const { return static_cast<int>(nyx::FilePermission::OpenRemote); }
  int permFileManageShares() const { return static_cast<int>(nyx::FilePermission::ManageShares); }
  int permFileManageRoles() const { return static_cast<int>(nyx::FilePermission::ManageRoles); }
  bool connectionPanelOpen() const { return connection_panel_open_; }
  bool groupsDialogOpen() const { return groups_dialog_open_; }
  bool fieldInfoOpen() const { return field_info_open_; }
  void setFieldInfoOpen(bool open);
  QString fieldInfoGroupId() const { return field_info_group_id_; }
  QString fieldInfoInvite() const { return field_info_invite_; }
  bool fieldInfoIsOwner() const { return field_info_is_owner_; }
  QString fieldInfoDescription() const { return field_info_description_; }
  QString fieldInfoDirection() const { return field_info_direction_; }
  QString fieldInfoTags() const { return field_info_tags_; }
  bool fieldInfoPublicListed() const { return field_info_public_listed_; }
  QVariantList fieldInfoMembers() const { return field_info_members_; }
  bool peerInfoOpen() const { return peer_info_open_; }
  void setPeerInfoOpen(bool open);
  QString peerInfoUserId() const { return peer_info_user_id_; }
  bool trayAvailable() const { return tray_icon_ != nullptr; }
  QString activeChatKey() const { return active_chat_key_; }
  QString sessionSummary() const;
  QString dmInboxToken() const;

  void setWindowActive(bool active);
  void setConnectionPanelOpen(bool open);
  void setGroupsDialogOpen(bool open);
  void setMainViewMode(int mode);
  void setFileScopeGroupId(const QString& groupIdHex);

  void setRendezvous(const QString& v);
  void setRendezvousList(const QString& v);
  void setDiscoveryMode(int mode);
  void setAutoStartOwnedHub(bool enabled);

  Q_INVOKABLE void saveNetworkSettings();
  Q_INVOKABLE bool testRendezvousServer(const QString& hostPort);
  void setProfilePath(const QString& v);
  void setNickname(const QString& v);

  Q_INVOKABLE void refreshAccountList();
  Q_INVOKABLE bool createAccount(const QString& nickname, const QString& password,
                                 const QString& confirmPassword, bool rememberMe = false);
  Q_INVOKABLE bool unlockAccount(const QString& accountId, const QString& password,
                                 bool rememberMe = false);
  Q_INVOKABLE bool tryUnlockRemembered(const QString& accountId);
  Q_INVOKABLE bool resetPasswordWithRecovery(const QString& accountId,
                                             const QString& recoveryPhrase,
                                             const QString& newPassword,
                                             const QString& confirmPassword);
  Q_INVOKABLE void confirmRecoveryPhraseSaved();
  Q_INVOKABLE void copyRecoveryPhrase();
  Q_INVOKABLE bool importLegacyProfile(const QString& password);
  Q_INVOKABLE void signOut();
  Q_INVOKABLE void refreshProfile();
  Q_INVOKABLE void completeOnboarding(const QString& nickname);
  Q_INVOKABLE void refreshChatList();
  Q_INVOKABLE void refreshGroupList();
  Q_INVOKABLE void refreshContactList();
  Q_INVOKABLE void refreshProfilePhotos();
  Q_INVOKABLE void pickAndSetProfilePhoto();
  Q_INVOKABLE void makeProfilePhotoCurrent(const QString& hashHex);
  Q_INVOKABLE void removeProfilePhoto(const QString& hashHex);
  Q_INVOKABLE QString peerAvatarPath(const QString& userIdHex) const;
  Q_INVOKABLE QVariantList peerAvatarHistory(const QString& userIdHex) const;
  Q_INVOKABLE void openContact(const QString& userIdHex);
  Q_INVOKABLE QString shortInviteCode(const QString& hex) const;
  Q_INVOKABLE void openGroupsDialog();
  Q_INVOKABLE void openFieldInfo(const QString& groupIdHex = {});
  Q_INVOKABLE void openPeerInfo(const QString& userIdHex = {});
  Q_INVOKABLE void openFilesView();
  Q_INVOKABLE void showChatView();
  Q_INVOKABLE void openFilesDialog();
  Q_INVOKABLE QString pickFolder();
  /** Диалог «Сохранить как»; suggestedFileName — исходное имя файла. */
  Q_INVOKABLE QString pickSaveFile(const QString& suggestedFileName);
  /** Выбор папки для сохранения нескольких файлов. */
  Q_INVOKABLE QString pickSaveFolder();
  Q_INVOKABLE void refreshFileLists();
  Q_INVOKABLE void refreshFileAccess();
  Q_INVOKABLE void refreshFieldRoster();
  Q_INVOKABLE bool hasFilePermission(int permissionBit) const;
  Q_INVOKABLE void setMemberFileRole(const QString& userIdHex, const QString& roleId);
  Q_INVOKABLE void createFileRole(const QString& name, int permissions);
  Q_INVOKABLE void updateFileRole(const QString& roleId, const QString& name, int permissions);
  Q_INVOKABLE void deleteFileRole(const QString& roleId);
  Q_INVOKABLE void openRemoteFile(const QString& hashHex, const QString& fileName = {},
                                  const QString& rootPath = {},
                                  const QString& relativePath = {});
  Q_INVOKABLE void addIndexedFolder(const QString& path);
  Q_INVOKABLE void setFileSelectedShareRoot(const QString& path);
  Q_INVOKABLE void setFilesSection(int section);
  Q_INVOKABLE void browseIntoFolder(const QString& navPath, const QString& itemRootPath = {});
  Q_INVOKABLE void browseUp();
  Q_INVOKABLE void browseToCrumb(int index);
  Q_INVOKABLE void toggleFileRolePermission(const QString& roleId, int permissionBit);
  Q_INVOKABLE bool canEditFileRolePermissions(const QString& roleId) const;
  Q_INVOKABLE void syncFileAccessTargetFromBrowse();
  Q_INVOKABLE void setFileAccessTarget(const QString& rootPath, const QString& relativePath);
  Q_INVOKABLE void openAccessForPath(const QString& rootPath, const QString& relativePath,
                                     const QString& label);
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
  Q_INVOKABLE void downloadFile(const QString& hashHex, const QString& fileName = {},
                                const QString& rootPath = {},
                                const QString& relativePath = {});
  Q_INVOKABLE void downloadRemoteFolder(const QString& rootPath, const QString& relativePath);
  Q_INVOKABLE bool canDownloadFileAt(const QString& rootPath, const QString& relativePath) const {
    return canFileDownloadAt(rootPath, relativePath);
  }
  Q_INVOKABLE bool canOpenRemoteFileAt(const QString& rootPath, const QString& relativePath) const {
    return canFileOpenRemoteAt(rootPath, relativePath);
  }
  Q_INVOKABLE void sendFileByHash(const QString& hashHex);
  Q_INVOKABLE void openConversation(const QString& key, int kind, const QString& refId,
                                    const QString& title, const QString& lastSeen);
  Q_INVOKABLE void searchMessages(const QString& query);
  Q_INVOKABLE void showWindow();
  Q_INVOKABLE void hideToTray();
  Q_INVOKABLE void startListen();
  Q_INVOKABLE void connectToken(const QString& tokenHex);
  Q_INVOKABLE void connectPeer(const QString& host, int port);
  Q_INVOKABLE void refreshLanPeers();
  Q_INVOKABLE void disconnectSession();
  Q_INVOKABLE void disconnectChat(const QString& key);
  Q_INVOKABLE QString sessionStateForKey(const QString& key) const;
  Q_INVOKABLE bool isChatSelectable(const QString& key) const;
  Q_INVOKABLE void sendMessage(const QString& text);
  /** Выбрать фото/видео → скопировать в chat_media → markdown `![…](nyx-media:hash)`. */
  Q_INVOKABLE QString pickChatMediaMarkdown();
  Q_INVOKABLE QString mediaLocalPath(const QString& hashHex) const;
  Q_INVOKABLE void ensureMediaAvailable(const QString& hashHex);
  Q_INVOKABLE bool isImageMedia(const QString& hashHex) const;
  Q_INVOKABLE void createGroup(const QString& name, const QString& description = {},
                               const QString& direction = {}, const QString& tags = {},
                               bool publicListed = false);
  Q_INVOKABLE void updateGroupMeta(const QString& groupIdHex, const QString& description,
                                   const QString& direction, const QString& tags,
                                   bool publicListed);
  Q_INVOKABLE QVariantMap contactInfo(const QString& userIdHex) const;
  Q_INVOKABLE void deleteGroup(const QString& groupIdHex);
  Q_INVOKABLE void removeFieldMember(const QString& groupIdHex, const QString& userIdHex);
  Q_INVOKABLE void startFieldHub(const QString& groupIdHex);
  Q_INVOKABLE void joinField(const QString& inviteHex);
  Q_INVOKABLE void connectActiveField();
  Q_INVOKABLE void copyToClipboard(const QString& text);
  Q_INVOKABLE void copyInviteToken();
  Q_INVOKABLE void copyDmInboxToken();
  Q_INVOKABLE void copyLastGroupInvite();
  Q_INVOKABLE void clearToast();
  /** Синхронизировать системный title bar (Windows) с темой UI. */
  Q_INVOKABLE void setNativeChromeDark(bool dark);

 signals:
  void sessionUnlockedChanged();
  void accountGateChanged();
  void profileChanged();
  void statusTextChanged();
  void inviteTokenChanged();
  void lastGroupInviteChanged();
  void contactListChanged();
  void sidebarModeChanged();
  void profileMetaChanged();
  void profilePhotosChanged();
  void chatChanged();
  void busyChanged();
  void listeningChanged();
  void rendezvousChanged();
  void networkSettingsChanged();
  void profilePathChanged();
  void toastChanged();
  void windowActiveChanged();
  void fileProgressChanged();
  void fileIndexProgressChanged();
  void filesChanged();
  void mainViewModeChanged();
  void fileAccessChanged();
  void connectionPanelOpenChanged();
  void groupsDialogOpenChanged();
  void fieldInfoOpenChanged();
  void peerInfoOpenChanged();
  void groupListChanged();
  void sessionsChanged();
  void incomingMessage(const QString& author, const QString& preview);
  void logLine(const QString& line);
  void requestCloseToTray();
  void showMainWindow();

 private:
  void wireCallbacks();
  void setStatus(const QString& text);
  void showToast(const QString& text, bool isError = false);
  QString normalizeInviteHex(const QString& hex) const;
  void enterChat(const QString& peerName, const QString& connectionLabel = {},
                 int kind = 0, const QString& refId = {});
  void endLiveSession();
  void leaveChat();
  void showGroupInView(const QString& groupIdHex);
  void loadStoredHistory(int kind, const QString& refId, const QString& convKey);
  void tickLanDiscovery();
  void beginMainSession();
  void updateOnboardingFlag();
  void syncNetworkSettingsFromService();
  bool applyRendezvousList(const QString& v);
  void maybeAutoReconnectSessions();
  void syncFileScopeLabel();
  void syncFileBrowseCrumbs();
  void syncRemoteBrowseCrumbs();
  std::vector<nyx::FileEntry> remoteRootsCatalog(const std::vector<nyx::FileEntry>& all) const;
  void resetFilesUiState();
  void resetFileBrowse();
  void syncFileScopeFromSavedOrRoots();
  QString normalizeShareRootPath(const QString& path) const;
  bool shareRootPathsEqual(const QString& a, const QString& b) const;
  QString scopeLabelForGroupId(const QString& groupIdHex) const;
  bool canRemoveShareRoot(const nyx::ShareRoot& root) const;
  bool isFileScopeOwner() const;
  void refreshFilePathMemberAccess();
  void refreshPathRoleState();
  void updateFileAccessTargetLabel();
  void refreshFileShareRoots();
  void refreshLocalFileModel();
  /** Без аргумента — из кэша NodeService; с вектором — как есть (в т.ч. пустой список). */
  void refreshRemoteFileModel();
  void refreshRemoteFileModel(const std::vector<nyx::FileEntry>& entries);
  /** Сбрасывает browse, если текущий remote-корень пропал из каталога. */
  void reconcileRemoteBrowsePath(const std::vector<nyx::FileEntry>& catalog);
  void runIndexJob(const QString& path, const QString& scopeGroupId, bool rescan);
  void refreshFileAccessLists();
  uint32_t currentFilePermissions() const;
  uint32_t filePermissionsAt(const QString& rootPath, const QString& relativePath) const;
  QString joinFileRelPath(const QString& browseRel, const QString& entryRel) const;
  QString resolveAccessRootPath(const QString& rootPath) const;
  QVariantList entriesToVariant(const std::vector<nyx::FileEntry>& entries, bool remote) const;

  nyx_app::NodeService service_;
  MessageModel messages_;
  ChatListModel chat_list_;
  LanPeerModel lan_peers_;
  QTimer lan_discovery_timer_;
  QTimer session_reconnect_timer_;

  QString profile_nickname_;
  QString profile_id_short_;
  QString profile_user_id_hex_;
  bool auto_start_owned_hub_ = false;
  QString status_text_;
  QString invite_token_;
  QString last_group_invite_;
  QString peer_title_;
  QString peer_connection_label_;
  QString peer_status_text_;
  QString active_chat_key_;
  QString active_chat_ref_id_;
  int active_chat_kind_ = 0;
  QString rendezvous_ = QStringLiteral("127.0.0.1:3478");
  QString rendezvous_list_;
  int discovery_mode_ = 0;
  QString network_status_;
  QString profile_path_;
  QString toast_;
  bool toast_is_error_ = false;
  bool in_chat_ = false;
  bool pending_field_join_notify_ = false;
  bool needs_onboarding_ = false;
  bool session_unlocked_ = false;
  bool legacy_profile_pending_ = false;
  QVariantList account_list_;
  QString account_gate_error_;
  QString pending_recovery_phrase_;
  QString last_account_id_;
  void finishAccountUnlock(bool begin_session);
  bool window_active_ = true;
  bool connection_panel_open_ = false;
  bool groups_dialog_open_ = false;
  bool field_info_open_ = false;
  QString field_info_group_id_;
  QString field_info_invite_;
  bool field_info_is_owner_ = false;
  QString field_info_description_;
  QString field_info_direction_;
  QString field_info_tags_;
  bool field_info_public_listed_ = false;
  QVariantList field_info_members_;
  bool peer_info_open_ = false;
  QString peer_info_user_id_;
  void syncFieldInfoState();
  QVariantList group_list_;
  QVariantList contact_list_;
  int sidebar_mode_ = 0;
  QString profile_bio_;
  QString profile_avatar_path_;
  QVariantList profile_photo_list_;
  QString profile_interests_;
  QString profile_availability_ = QStringLiteral("available");
  QString file_progress_label_;
  void loadProfileMeta();
  void persistProfileMeta();
  int file_progress_percent_ = 0;
  bool file_progress_visible_ = false;
  int main_view_mode_ = 0;
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
  int files_section_ = 0;
  bool file_index_progress_visible_ = false;
  int file_index_progress_percent_ = 0;
  QString file_index_progress_label_;
  int file_index_files_scanned_ = 0;
  std::atomic<bool> file_index_busy_{false};
  QVariantList file_role_list_;
  QVariantList file_permission_preset_list_;
  QVariantList file_member_access_;
  QVariantList file_path_member_access_;
  QString file_path_role_id_;
  QString file_path_role_inherited_from_;
  QString file_access_target_root_;
  QString file_access_target_rel_;
  QString file_access_target_label_;
  QSystemTrayIcon* tray_icon_ = nullptr;
  QMenu* tray_menu_ = nullptr;

  void ensureChatMediaRootIndexed();
  static QString chatMediaDir();
};
