#pragma once

#include "chat_list_model.hpp"
#include "lan_peer_model.hpp"
#include "message_model.hpp"
#include "../appcore/node_service.hpp"

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

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
  Q_PROPERTY(bool needsOnboarding READ needsOnboarding NOTIFY profileChanged)
  Q_PROPERTY(MessageModel* messages READ messages CONSTANT)
  Q_PROPERTY(ChatListModel* chatList READ chatList CONSTANT)
  Q_PROPERTY(LanPeerModel* lanPeers READ lanPeers CONSTANT)
  Q_PROPERTY(QVariantList groupList READ groupList NOTIFY groupListChanged)
  Q_PROPERTY(QString rendezvous READ rendezvous WRITE setRendezvous NOTIFY rendezvousChanged)
  Q_PROPERTY(QString rendezvousList READ rendezvousList WRITE setRendezvousList NOTIFY rendezvousChanged)
  Q_PROPERTY(int discoveryMode READ discoveryMode WRITE setDiscoveryMode NOTIFY networkSettingsChanged)
  Q_PROPERTY(QString networkStatus READ networkStatus NOTIFY networkSettingsChanged)
  Q_PROPERTY(QString toast READ toast NOTIFY toastChanged)
  Q_PROPERTY(bool windowActive READ windowActive WRITE setWindowActive NOTIFY windowActiveChanged)
  Q_PROPERTY(QString fileProgressLabel READ fileProgressLabel NOTIFY fileProgressChanged)
  Q_PROPERTY(int fileProgressPercent READ fileProgressPercent NOTIFY fileProgressChanged)
  Q_PROPERTY(bool fileProgressVisible READ fileProgressVisible NOTIFY fileProgressChanged)
  Q_PROPERTY(bool connectionPanelOpen READ connectionPanelOpen WRITE setConnectionPanelOpen
                 NOTIFY connectionPanelOpenChanged)
  Q_PROPERTY(bool groupsDialogOpen READ groupsDialogOpen WRITE setGroupsDialogOpen
                 NOTIFY groupsDialogOpenChanged)
  Q_PROPERTY(bool trayAvailable READ trayAvailable CONSTANT)

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
  bool canSendMessage() const { return in_chat_; }
  bool sessionUnlocked() const { return session_unlocked_; }
  QVariantList accountList() const { return account_list_; }
  QString accountGateError() const { return account_gate_error_; }
  bool legacyProfilePending() const { return legacy_profile_pending_; }
  bool needsOnboarding() const { return needs_onboarding_; }
  MessageModel* messages() { return &messages_; }
  ChatListModel* chatList() { return &chat_list_; }
  LanPeerModel* lanPeers() { return &lan_peers_; }
  QVariantList groupList() const { return group_list_; }
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
  bool connectionPanelOpen() const { return connection_panel_open_; }
  bool groupsDialogOpen() const { return groups_dialog_open_; }
  bool trayAvailable() const { return tray_icon_ != nullptr; }

  void setWindowActive(bool active);
  void setConnectionPanelOpen(bool open);
  void setGroupsDialogOpen(bool open);

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
                                 const QString& confirmPassword);
  Q_INVOKABLE bool unlockAccount(const QString& accountId, const QString& password);
  Q_INVOKABLE bool importLegacyProfile(const QString& password);
  Q_INVOKABLE void signOut();
  Q_INVOKABLE void refreshProfile();
  Q_INVOKABLE void completeOnboarding(const QString& nickname);
  Q_INVOKABLE void refreshChatList();
  Q_INVOKABLE void refreshGroupList();
  Q_INVOKABLE void openGroupsDialog();
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
  Q_INVOKABLE void sendMessage(const QString& text);
  Q_INVOKABLE void indexFolder(const QString& path);
  Q_INVOKABLE void requestRemoteFiles();
  Q_INVOKABLE void downloadFile(const QString& hashHex);
  Q_INVOKABLE void createGroup(const QString& name);
  Q_INVOKABLE void deleteGroup(const QString& groupIdHex);
  Q_INVOKABLE void removeFieldMember(const QString& groupIdHex, const QString& userIdHex);
  Q_INVOKABLE void startFieldHub(const QString& groupIdHex);
  Q_INVOKABLE void joinField(const QString& inviteHex);
  Q_INVOKABLE void connectActiveField();
  Q_INVOKABLE void copyToClipboard(const QString& text);
  Q_INVOKABLE void copyInviteToken();
  Q_INVOKABLE void copyLastGroupInvite();
  Q_INVOKABLE void clearToast();

 signals:
  void sessionUnlockedChanged();
  void accountGateChanged();
  void profileChanged();
  void statusTextChanged();
  void inviteTokenChanged();
  void lastGroupInviteChanged();
  void chatChanged();
  void busyChanged();
  void listeningChanged();
  void rendezvousChanged();
  void networkSettingsChanged();
  void profilePathChanged();
  void toastChanged();
  void windowActiveChanged();
  void fileProgressChanged();
  void connectionPanelOpenChanged();
  void groupsDialogOpenChanged();
  void groupListChanged();
  void incomingMessage(const QString& author, const QString& preview);
  void logLine(const QString& line);
  void requestCloseToTray();
  void showMainWindow();

 private:
  void wireCallbacks();
  void setStatus(const QString& text);
  void showToast(const QString& text);
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
  void maybeAutoStartOwnedHub();

  nyx_app::NodeService service_;
  MessageModel messages_;
  ChatListModel chat_list_;
  LanPeerModel lan_peers_;
  QTimer lan_discovery_timer_;

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
  bool in_chat_ = false;
  bool needs_onboarding_ = false;
  bool session_unlocked_ = false;
  bool legacy_profile_pending_ = false;
  QVariantList account_list_;
  QString account_gate_error_;
  bool window_active_ = true;
  bool connection_panel_open_ = false;
  bool groups_dialog_open_ = false;
  QVariantList group_list_;
  QString file_progress_label_;
  int file_progress_percent_ = 0;
  bool file_progress_visible_ = false;
  QSystemTrayIcon* tray_icon_ = nullptr;
  QMenu* tray_menu_ = nullptr;
};
