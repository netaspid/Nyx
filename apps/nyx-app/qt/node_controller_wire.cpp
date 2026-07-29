#include "node_controller.hpp"

#include "android_platform.hpp"
#include "node_controller_media.hpp"

#include "nyx/file_index.hpp"
#include "nyx/markdown_format.hpp"
#include "nyx/util.hpp"

#include <QDateTime>
#include <QMetaObject>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <string>
#include <vector>

void NodeController::wireCallbacks() {
  wireStatusCallbacks();
  wireChatCallbacks();
  wireSessionCallbacks();
  wireDiscoveryCallbacks();
  wireGroupCallbacks();
  wireCallCallbacks();
  wireFileCallbacks();
}

void NodeController::wireStatusCallbacks() {
  service_.set_on_status([this](const std::string& text) {
    QMetaObject::invokeMethod(
        this,
        [this, text]() {
          const QString q = QString::fromStdString(text);
          setStatus(q);
          const QString lower = q.toLower();
          if (lower.contains(QStringLiteral("вошёл")) || lower.contains(QStringLiteral("вошел"))) {
            refreshGroupList();
            if (main_view_mode_ == 1)
              refreshFileAccessLists();
          }

          const bool progress_noise = lower.contains(QStringLiteral("lookup")) ||
                                      lower.contains(QStringLiteral("rendezvous")) ||
                                      lower.contains(QStringLiteral("register")) ||
                                      lower.contains(QStringLiteral("handshake")) ||
                                      lower.contains(QStringLiteral("bind ")) ||
                                      lower.startsWith(QStringLiteral("hub «")) ||
                                      lower.startsWith(QStringLiteral("эфир «")) ||
                                      lower.contains(QStringLiteral("invite:")) ||
                                      lower.contains(QStringLiteral("подключение к hub")) ||
                                      lower.contains(QStringLiteral("подключение к эфиру")) ||
                                      lower.contains(QStringLiteral("подключение к ")) ||
                                      lower.contains(QStringLiteral("поиск на rendezvous")) ||
                                      lower.contains(QStringLiteral("повтор lookup")) ||
                                      lower.contains(QStringLiteral("пробить nat")) ||
                                      lower.contains(QStringLiteral("установить канал")) ||
                                      lower.contains(QStringLiteral("собеседник не найден")) ||
                                      lower.contains(QStringLiteral("эфир не найден")) ||
                                      lower.contains(QStringLiteral("поле недоступно")) ||
                                      lower.contains(QStringLiteral("владелец офлайн"));
          if (!progress_noise && (lower.contains(QStringLiteral("failed")) ||
                                  lower.contains(QStringLiteral("не удалось")) ||
                                  lower.contains(QStringLiteral("неверн")) ||
                                  lower.contains(QStringLiteral("отказ")) ||
                                  lower.contains(QStringLiteral("файл сохранён")) ||
                                  lower.contains(QStringLiteral("приём")) ||
                                  lower.contains(QStringLiteral("запрос файла")) ||
                                  lower.contains(QStringLiteral("timeout")) ||
                                  lower.contains(QStringLiteral("не найден")) ||
                                  lower.contains(QStringLiteral("не отвечает")))) {
            showToast(q,
                      lower.contains(QStringLiteral("отказ")) ||
                          lower.contains(QStringLiteral("failed")) ||
                          lower.contains(QStringLiteral("не удалось")) ||
                          lower.contains(QStringLiteral("не найден")) ||
                          lower.contains(QStringLiteral("не отвечает")));
          }
        },
        Qt::QueuedConnection);
  });

  service_.set_on_invite_token([this](const std::string& hex) {
    QMetaObject::invokeMethod(
        this,
        [this, hex]() {
          invite_token_ = QString::fromStdString(hex);
          emit inviteTokenChanged();
        },
        Qt::QueuedConnection);
  });
}

void NodeController::wireChatCallbacks() {
  service_.set_on_message([this](const nyx_app::UiMessage& msg) {
    if (!msg.outgoing) {
      const auto blocks = nyx::parse_markdown_blocks(msg.text);
      const QString chat_key = QString::fromStdString(msg.chat_key);
      const QString title = QString::fromStdString(msg.author);
      nyx::GroupId scope_id {};
      QString scope_hex;
      if (chat_key.startsWith(QLatin1String("group:"))) {
        scope_hex = chat_key.section(QLatin1Char(':'), 1).toLower();
        nyx::GroupStore::group_id_from_hex(scope_hex.toStdString(), scope_id);
      }
      const QString library_root =
          QString::fromStdString(nyx::FileIndex::library_root_path(scope_id));
      for (const auto& block : blocks) {
        if (block.type != nyx::MdBlockType::File || block.hash.empty())
          continue;
        const QString mime = QString::fromStdString(block.mime).toLower();
        const QString name = nyxSafeMediaPathPart(QString::fromStdString(block.caption));
        QString kind;
        if (mime.startsWith(QLatin1String("audio/")) &&
            (name.startsWith(QLatin1String("voice-message"), Qt::CaseInsensitive) ||
             name.compare(QLatin1String("voice.m4a"), Qt::CaseInsensitive) == 0)) {
          kind = QStringLiteral("voice");
        } else if (mime.startsWith(QLatin1String("video/")) &&
                   name.startsWith(QLatin1String("circle-message"), Qt::CaseInsensitive)) {
          kind = QStringLiteral("circle");
        } else {
          continue;
        }
        const QString relative_dir = nyxMediaRelativeDir(chat_key, title, kind);
        const QString dest_dir = QDir(library_root).filePath(relative_dir);
        QDir().mkpath(dest_dir);
        const QString destination = QDir(dest_dir).filePath(
            QString::fromStdString(block.hash).left(12) + QLatin1Char('-') + name);

        if (const auto local = service_.find_file_object(block.hash)) {
          service_.import_file_object(local->absolute_path(),
                                      name.toStdString(),
                                      mime.toStdString(),
                                      scope_hex.toStdString(),
                                      msg.author_user_id,
                                      relative_dir.toStdString());
        } else {
          service_.download_file(block.hash, destination.toStdString(), msg.session_id);
        }
      }
    }
    QMetaObject::invokeMethod(
        this,
        [this, msg]() {
          const QString chat_key = QString::fromStdString(msg.chat_key);
          const bool for_active = chat_key.isEmpty() || chat_key == active_chat_key_ ||
                                  (msg.session_id == service_.active_session_id());
          if (for_active) {
            if (msg.message_id != 0 && messages_.hasMessageId(msg.message_id)) {
              if (!msg.delivery.empty()) {
                messages_.setDelivery(msg.message_id, QString::fromStdString(msg.delivery));
              }
            } else {
              messages_.appendMessage(QString::fromStdString(msg.author),
                                      QString::fromStdString(msg.text),
                                      msg.outgoing,
                                      msg.timestamp_ms,
                                      msg.message_id,
                                      QString::fromStdString(msg.delivery),
                                      QString::fromStdString(msg.author_user_id));
            }
          } else if (!msg.outgoing && !chat_key.isEmpty()) {
            chat_list_.bumpUnread(chat_key);
          }
          refreshChatList();
          if (!msg.outgoing) {
            const QString author = QString::fromStdString(msg.author);
            const QString preview = QString::fromStdString(msg.text);
            emit incomingMessage(author, preview);
            if (!window_active_ && tray_icon_) {
              tray_icon_->showMessage(
                  author,
                  preview.length() > 120 ? preview.left(117) + QStringLiteral("...") : preview,
                  QSystemTrayIcon::Information,
                  4000);
            }
          }
        },
        Qt::QueuedConnection);
  });

  service_.set_on_delivery([this](const std::string&, uint64_t message_id, bool delivered) {
    QMetaObject::invokeMethod(
        this,
        [this, message_id, delivered]() {
          messages_.setDelivery(message_id,
                                delivered ? QStringLiteral("delivered") : QStringLiteral("failed"));
        },
        Qt::QueuedConnection);
  });
}

void NodeController::wireSessionCallbacks() {
  service_.set_on_chat_ready([this](const std::string& session_id,
                                    const std::string& peer_title,
                                    const std::string& conn_label,
                                    nyx::ConversationKind kind,
                                    const std::string& ref_id) {
    QMetaObject::invokeMethod(
        this,
        [this, session_id, peer_title, conn_label, kind, ref_id]() {
          const QString sid = QString::fromStdString(session_id);
          const QString ref = QString::fromStdString(ref_id);
          const QString list_key = kind == nyx::ConversationKind::Group
                                       ? (QStringLiteral("group:") + ref)
                                       : (ref.isEmpty() ? sid : QStringLiteral("dm:") + ref);

          chat_list_.setSessionState(list_key, QStringLiteral("live"));
          if (sid != list_key)
            chat_list_.setSessionState(sid, QStringLiteral("live"));

          const bool user_waiting = pending_field_join_notify_;
          const bool was_selected = !active_chat_key_.isEmpty() && active_chat_key_ == list_key;
          pending_field_join_notify_ = false;

          if (user_waiting || was_selected) {
            service_.set_active_session(session_id);
            active_chat_kind_ = static_cast<int>(kind);
            active_chat_ref_id_ = ref;
            enterChat(QString::fromStdString(peer_title),
                      QString::fromStdString(conn_label),
                      static_cast<int>(kind),
                      ref);
            if (kind == nyx::ConversationKind::Group) {
              loadStoredHistory(static_cast<int>(kind), ref, list_key);
              showToast(QStringLiteral("В поле «") + QString::fromStdString(peer_title) +
                        QStringLiteral("»"));
            }
          } else if (kind == nyx::ConversationKind::Group) {
            showToast(QStringLiteral("Эфир «") + QString::fromStdString(peer_title) +
                      QStringLiteral("» снова открыт"));
          } else {
            showToast(QStringLiteral("Собеседник снова на связи"));
          }

          refreshChatList();
          refreshGroupList();
          refreshContactList();
          emit sessionsChanged();
          emit chatChanged();
        },
        Qt::QueuedConnection);
  });

  service_.set_on_session_ended([this](const std::string& session_id) {
    QMetaObject::invokeMethod(
        this,
        [this, session_id]() {
          const QString sid = QString::fromStdString(session_id);
          if (!sid.isEmpty()) {
            chat_list_.setSessionState(sid, QStringLiteral("offline"));
          }
          const bool active_gone = !active_chat_key_.isEmpty() &&
                                   !service_.is_session_live(active_chat_key_.toStdString());
          const bool ended_active =
              !active_chat_key_.isEmpty() &&
              (sid == active_chat_key_ ||
               (active_gone && (sid.startsWith(QStringLiteral("group:join:")) ||
                                sid.startsWith(QStringLiteral("dm:")))));
          if (ended_active || (in_chat_ && active_gone)) {
            chat_list_.setSessionState(active_chat_key_, QStringLiteral("offline"));
            endLiveSession();
          }
          refreshGroupList();
          refreshChatList();
          emit sessionsChanged();
          emit chatChanged();
          if (pending_field_join_notify_) {
            pending_field_join_notify_ = false;
            const auto st = service_.session_state(session_id);
            if (st == nyx_app::SessionState::Offline) {
              showToast(QStringLiteral("Эфир закрыт — владелец не в сети"), false);
            }
          }

          if (sid.startsWith(QStringLiteral("group:")) &&
              !sid.startsWith(QStringLiteral("group:join:"))) {
            const auto st = service_.session_state(session_id);
            if (st == nyx_app::SessionState::Offline &&
                service_.is_session_intent_enabled(sid.toStdString())) {
              QTimer::singleShot(3000, this, [this, sid]() {
                if (!auto_start_owned_hub_)
                  return;
                if (!service_.is_session_intent_enabled(sid.toStdString()))
                  return;
                if (service_.is_session_up(sid.toStdString()))
                  return;
                service_.ensure_session(sid.toStdString());
                refreshChatList();
                emit sessionsChanged();
                emit chatChanged();
              });
            }
          }
        },
        Qt::QueuedConnection);
  });
}

void NodeController::wireDiscoveryCallbacks() {
  service_.set_on_sessions_changed([this]() {
    QMetaObject::invokeMethod(
        this,
        [this]() {
          refreshChatSessionStates();
          emit sessionsChanged();
          emit busyChanged();
          emit listeningChanged();
        },
        Qt::QueuedConnection);
  });

  service_.set_on_lan_peers([this](const std::vector<nyx::LanPeer>& peers) {
    QMetaObject::invokeMethod(
        this,
        [this, peers]() {
          QVariantList list;
          for (const auto& p : peers) {
            QVariantMap m;
            m.insert("instance", QString::fromStdString(p.instance));
            m.insert("host", QString::fromStdString(p.host));
            m.insert("port", static_cast<int>(p.port));
            m.insert("userId", QString::fromStdString(p.user_id_short));
            list.append(m);
          }
          lan_peers_.setPeers(list);
          emit listeningChanged();
          emit busyChanged();
        },
        Qt::QueuedConnection);
  });

  service_.set_on_mode([this]() {
    QMetaObject::invokeMethod(
        this,
        [this]() {
          emit listeningChanged();
          emit busyChanged();
        },
        Qt::QueuedConnection);
  });
}

void NodeController::wireGroupCallbacks() {
  service_.set_on_group_created([this](const std::string& gid, const std::string& invite) {
    QMetaObject::invokeMethod(
        this,
        [this, gid, invite]() {
          last_group_invite_ = QString::fromStdString(invite);
          setStatus(QString("поле создано\n  id: %1\n  invite: %2")
                        .arg(QString::fromStdString(gid), last_group_invite_));
          refreshChatList();
          refreshGroupList();
          if (service_.auto_start_owned_hub()) {
            startFieldHub(QString::fromStdString(gid));
            toast_ = QStringLiteral("Поле создано — hub запускается автоматически");
          } else {
            toast_ = QStringLiteral("Поле создано — нажмите «Запустить hub» в списке полей");
          }
          emit toastChanged();
        },
        Qt::QueuedConnection);
  });

  service_.set_on_group_meta_changed([this]() {
    QMetaObject::invokeMethod(
        this,
        [this]() {
          refreshGroupList();
          if (field_info_open_) {
            syncFieldInfoState();
            emit fieldInfoOpenChanged();
          }
        },
        Qt::QueuedConnection);
  });

  service_.set_on_avatars_changed([this]() {
    QMetaObject::invokeMethod(
        this,
        [this]() {
          refreshContactList();
          emit profilePhotosChanged();
          emit chatChanged();
        },
        Qt::QueuedConnection);
  });
}

void NodeController::wireCallCallbacks() {
  service_.set_on_call_changed([this]() {
    QMetaObject::invokeMethod(
        this,
        [this]() {
          emit callChanged();

          syncCallNotifications();
          syncCallAudio();
        },
        Qt::QueuedConnection);
  });
  service_.set_on_call_media(
      [this](nyx::CallMediaType type, const nyx::ByteBuffer& payload, const nyx::UserId& from) {
        QByteArray packet(reinterpret_cast<const char*>(payload.data()),
                          static_cast<int>(payload.size()));
        QString peer;
        const bool from_zero =
            std::all_of(from.begin(), from.end(), [](uint8_t b) { return b == 0; });
        if (!from_zero) {
          peer = QString::fromStdString(nyx::to_hex(from.data(), from.size()));
        }
        QMetaObject::invokeMethod(
            this,
            [this, type, packet, peer]() {
              if (type == nyx::CallMediaType::Opus) {
                call_audio_.onRemoteOpus(peer, packet);
              } else if (type == nyx::CallMediaType::Video) {
                call_video_.onRemoteVideo(peer, packet);
              }
            },
            Qt::QueuedConnection);
      });
}

void NodeController::wireFileCallbacks() {
  service_.set_on_file_progress([this](const std::string& label, int percent) {
    QMetaObject::invokeMethod(
        this,
        [this, label, percent]() {
          file_progress_label_ = QString::fromStdString(label);
          file_progress_percent_ = percent;
          file_progress_visible_ = percent > 0 && percent < 100;
          emit fileProgressChanged();
          if (percent >= 100) {
            QTimer::singleShot(1500, this, [this]() {
              file_progress_visible_ = false;
              emit fileProgressChanged();
            });
          }
        },
        Qt::QueuedConnection);
  });

  service_.set_on_file_index_progress(
      [this](const std::string& path, int files_scanned, bool finished) {
        QMetaObject::invokeMethod(
            this,
            [this, path, files_scanned, finished]() {
              file_index_files_scanned_ = files_scanned;
              if (finished)
                return;
              file_index_progress_visible_ = true;

              const int paced =
                  5 + static_cast<int>(
                          (95.0 * (1.0 - std::exp(-static_cast<double>(files_scanned) / 80.0))));
              file_index_progress_percent_ = qBound(5, paced, 95);
              const QString name = QString::fromStdString(path);
              file_index_progress_label_ =
                  name.isEmpty() ? QStringLiteral("Сканирование… %1 файлов").arg(files_scanned)
                                 : QStringLiteral("%1 · %2").arg(name).arg(files_scanned);
              emit fileIndexProgressChanged();
            },
            Qt::QueuedConnection);
      });
  service_.set_on_transfer_queue_changed([this]() {
    QMetaObject::invokeMethod(
        this,
        [this]() {
          transfer_queue_.clear();
          for (const auto& task : service_.transfer_queue()) {
            QVariantMap item;
            item.insert(QStringLiteral("hash"), QString::fromStdString(task.hash_hex));
            const QString dest = QString::fromStdString(task.dest_path);
            item.insert(QStringLiteral("name"),
                        QFileInfo(dest).fileName().isEmpty() ? dest : QFileInfo(dest).fileName());
            item.insert(QStringLiteral("path"), dest);
            item.insert(QStringLiteral("state"), QString::fromStdString(task.state));
            item.insert(QStringLiteral("progress"), task.progress);
            item.insert(QStringLiteral("paused"), task.paused);
            item.insert(QStringLiteral("error"), QString::fromStdString(task.error));
            item.insert(QStringLiteral("direction"), QString::fromStdString(task.direction));
            transfer_queue_.append(item);
          }
          emit filesChanged();
        },
        Qt::QueuedConnection);
  });

  service_.set_on_remote_files([this](const std::vector<nyx::FileEntry>& entries) {
    QMetaObject::invokeMethod(
        this,
        [this, entries]() {
          refreshRemoteFileModel(entries);
          emit filesChanged();
          const int n = static_cast<int>(remote_file_list_.size());
          if (file_resources_root_.isEmpty()) {
            showToast(n == 0 ? QStringLiteral("Ресурсы поля: папок нет")
                             : QStringLiteral("Ресурсы поля: %1 папок").arg(n));
          } else {
            showToast(QStringLiteral("Уровень каталога: %1").arg(n));
          }
        },
        Qt::QueuedConnection);
  });

  service_.set_on_file_access_sync([this]() {
    QMetaObject::invokeMethod(
        this,
        [this]() {
          refreshFileAccessLists();
          refreshRemoteFileModel();
          emit filesChanged();
        },
        Qt::QueuedConnection);
  });
}
