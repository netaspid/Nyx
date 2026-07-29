import { NyxClient } from "./client";
import "./styles.css";

const client = new NyxClient();
const app = document.getElementById("app")!;

type Acc = { id: string; nickname: string; rememberActive?: boolean };
type Conv = { key: string; title: string; kind: number; refId: string; lastPreview?: string };
type Msg = { id?: number; messageId?: number; ts: number; author: string; text: string; outgoing: boolean };

let accounts: Acc[] = [];
let conversations: Conv[] = [];
let messages: Msg[] = [];
let unlocked = false;
let nickname = "";
let activeConv: Conv | null = null;
let view: "gate" | "chats" | "files" | "settings" | "call" = "gate";
let callState: Record<string, unknown> = { state: "idle" };
type FileRow = { hash: string; name: string; size: number; mime?: string; directory?: boolean };
let localFiles: FileRow[] = [];
let remoteFiles: FileRow[] = [];
let contacts: { userId: string; nickname: string }[] = [];
let toast = "";

const media = {
  stream: null as MediaStream | null,
  audioCtx: null as AudioContext | null,
  processor: null as ScriptProcessorNode | null,
  pc: null as RTCPeerConnection | null,
};

function showToast(t: string) {
  toast = t;
  render();
  setTimeout(() => {
    toast = "";
    render();
  }, 2500);
}

async function refreshAccounts() {
  accounts = await client.rpc<Acc[]>("listAccounts");
}

async function refreshChats() {
  conversations = await client.rpc<Conv[]>("listConversations");
  try {
    await client.rpc("listGroups");
  } catch {
    /* ignore */
  }
}

async function loadHistory(c: Conv) {
  activeConv = c;
  messages = await client.rpc<Msg[]>("loadHistory", { kind: c.kind, refId: c.refId });
  render();
}

async function unlock(id: string, password: string, remember: boolean) {
  const r = await client.rpc<{ nickname: string }>("unlockAccount", {
    accountId: id,
    password,
    rememberMe: remember,
  });
  nickname = r.nickname;
  unlocked = true;
  view = "chats";
  await refreshChats();
  render();
}

function el(html: string) {
  app.innerHTML = html;
}

function render() {
  if (!client.token) {
    el(`<div class="shell gate">
      <h1>Nyx Web</h1>
      <p class="muted">Hosted client — keys stay on the server.</p>
      <label>Admin token (if required)</label>
      <input id="admin" placeholder="optional" />
      <button id="boot">Connect session</button>
      ${toast ? `<div class="toast">${toast}</div>` : ""}
    </div>`);
    document.getElementById("boot")!.onclick = async () => {
      try {
        const admin = (document.getElementById("admin") as HTMLInputElement).value.trim();
        await client.createSession(admin);
        await refreshAccounts();
        render();
      } catch (e) {
        showToast(String(e));
      }
    };
    return;
  }

  if (!unlocked) {
    const rows = accounts
      .map(
        (a) => `<option value="${a.id}">${a.nickname} (${a.id.slice(0, 8)}…)</option>`
      )
      .join("");
    el(`<div class="shell gate">
      <h1>Nyx Web</h1>
      <p class="muted">Session ${client.sessionId.slice(0, 8)}…</p>
      <h2>Unlock</h2>
      <select id="acc">${rows}</select>
      <input id="pass" type="password" placeholder="Password" />
      <label class="row"><input id="rem" type="checkbox" /> Remember</label>
      <button id="unlock">Unlock</button>
      <h2>Create account</h2>
      <input id="nick" placeholder="Nickname" />
      <input id="npass" type="password" placeholder="Password" />
      <button id="create">Create</button>
      ${toast ? `<div class="toast">${toast}</div>` : ""}
    </div>`);
    document.getElementById("unlock")!.onclick = async () => {
      try {
        const id = (document.getElementById("acc") as HTMLSelectElement).value;
        const pass = (document.getElementById("pass") as HTMLInputElement).value;
        const rem = (document.getElementById("rem") as HTMLInputElement).checked;
        await unlock(id, pass, rem);
      } catch (e) {
        showToast(String(e));
      }
    };
    document.getElementById("create")!.onclick = async () => {
      try {
        const nick = (document.getElementById("nick") as HTMLInputElement).value;
        const pass = (document.getElementById("npass") as HTMLInputElement).value;
        const r = await client.rpc<{ accountId: string; recoveryPhrase: string }>("createAccount", {
          nickname: nick,
          password: pass,
        });
        alert("Recovery phrase (save it):\n" + r.recoveryPhrase);
        await client.rpc("confirmRecoveryPhraseSaved");
        await refreshAccounts();
        render();
      } catch (e) {
        showToast(String(e));
      }
    };
    return;
  }

  const nav = `<nav>
    <button data-v="chats" class="${view === "chats" ? "on" : ""}">Chats</button>
    <button data-v="files" class="${view === "files" ? "on" : ""}">Files</button>
    <button data-v="settings" class="${view === "settings" ? "on" : ""}">Settings</button>
    <button data-v="call" class="${view === "call" ? "on" : ""}">Call</button>
    <span class="grow"></span>
    <span class="muted">${nickname}</span>
    <button id="out">Sign out</button>
  </nav>`;

  let body = "";
  if (view === "chats") {
    const list = conversations
      .map(
        (c) =>
          `<button class="chat-item ${activeConv?.key === c.key ? "on" : ""}" data-k="${c.key}">
            <strong>${escapeHtml(c.title)}</strong>
            <span class="muted">${escapeHtml(c.lastPreview || "")}</span>
          </button>`
      )
      .join("");
    const msgs = messages
      .map(
        (m) =>
          `<div class="bubble ${m.outgoing ? "out" : "in"}"><div class="meta">${escapeHtml(
            m.author
          )}</div><div>${escapeHtml(m.text)}</div></div>`
      )
      .join("");
    body = `<div class="split">
      <aside>
        <div class="tools">
          <button id="inbox">Start DM inbox</button>
          <input id="token" placeholder="Invite / DM token hex" />
          <button id="connect">Connect</button>
          <input id="gname" placeholder="New field name" />
          <button id="mkfield">Create field</button>
          <button id="hub">Start field hub</button>
          <input id="ginvite" placeholder="Join field invite" />
          <button id="joinfield">Join</button>
        </div>
        ${list || '<p class="muted">No conversations</p>'}
      </aside>
      <main class="chat">
        <div class="msgs">${msgs || '<p class="muted">Select a chat</p>'}</div>
        <div class="composer">
          <input id="msg" placeholder="Message" ${activeConv ? "" : "disabled"} />
          <button id="send" ${activeConv ? "" : "disabled"}>Send</button>
          <button id="callAudio" ${activeConv ? "" : "disabled"}>Audio</button>
          <button id="callVideo" ${activeConv ? "" : "disabled"}>Video</button>
        </div>
      </main>
    </div>`;
  } else if (view === "files") {
    const row = (f: FileRow, remote: boolean) =>
      `<div class="file-row">
        <div><strong>${escapeHtml(f.name)}</strong><div class="muted">${f.size} B · ${escapeHtml(
        f.hash.slice(0, 12)
      )}…</div></div>
        <div class="row">
          ${
            remote
              ? `<button data-dl="${f.hash}" data-name="${escapeHtml(f.name)}">Download</button>`
              : `<a href="/api/v1/files/blob?token=${encodeURIComponent(
                  client.token
                )}&hash=${encodeURIComponent(f.hash)}" target="_blank">Open</a>`
          }
        </div>
      </div>`;
    body = `<div class="panel">
      <h2>Files</h2>
      <div class="tools">
        <input id="folder" placeholder="/path/to/share" />
        <button id="index">Index folder</button>
        <input id="updest" placeholder="Upload dest path on host" />
        <input id="upfile" type="file" />
        <button id="upload">Upload</button>
        <button id="refLoc">Refresh local</button>
        <button id="refRem">Refresh remote</button>
        <button id="queue">Transfer queue</button>
      </div>
      <h3>Local</h3>
      <div class="file-list">${
        localFiles.length ? localFiles.map((f) => row(f, false)).join("") : '<p class="muted">Empty</p>'
      }</div>
      <h3>Remote</h3>
      <div class="file-list">${
        remoteFiles.length ? remoteFiles.map((f) => row(f, true)).join("") : '<p class="muted">Empty</p>'
      }</div>
      <pre id="queueOut"></pre>
    </div>`;
  } else if (view === "settings") {
    const contactRows = contacts
      .map((c) => `<li><strong>${escapeHtml(c.nickname)}</strong> <span class="muted">${c.userId.slice(0, 12)}…</span></li>`)
      .join("");
    body = `<div class="panel">
      <h2>Settings</h2>
      <label>Rendezvous list</label>
      <input id="rv" placeholder="host:port,host:port" />
      <button id="saveNet">Save network</button>
      <label>Bio</label>
      <input id="bio" />
      <label>Interests</label>
      <input id="interests" />
      <label>Availability</label>
      <select id="avail">
        <option value="available">available</option>
        <option value="away">away</option>
        <option value="busy">busy</option>
        <option value="invisible">invisible</option>
      </select>
      <button id="saveMeta">Save profile</button>
      <h3>Contacts</h3>
      <ul id="contacts">${contactRows || '<li class="muted">No contacts</li>'}</ul>
      <button id="lan">Browse LAN peers</button>
      <pre id="lanOut"></pre>
    </div>`;
  } else if (view === "call") {
    const st = String(callState.state || "idle");
    body = `<div class="panel call">
      <h2>Call</h2>
      <p>State: <strong>${escapeHtml(st)}</strong> ${escapeHtml(String(callState.title || ""))}</p>
      <div class="call-controls">
        <button class="round green" id="answer" title="Ответить" aria-label="Answer"></button>
        <button class="round" id="mic" title="Mute" aria-label="Mute"></button>
        <button class="round" id="cam" title="Camera" aria-label="Camera"></button>
        <button class="round" id="reject" title="Сбросить" aria-label="Decline"></button>
        <button class="round red" id="hang" title="Сбросить" aria-label="Hang up"></button>
      </div>
      <video id="localVid" autoplay muted playsinline></video>
      <audio id="remoteAud" autoplay></audio>
      <p class="muted">Media bridge: getUserMedia → PCM over WS → CallMesh (+ WebRTC stub)</p>
    </div>`;
  }

  el(`<div class="shell app">${nav}${body}${toast ? `<div class="toast">${toast}</div>` : ""}</div>`);

  app.querySelectorAll("nav [data-v]").forEach((b) => {
    (b as HTMLButtonElement).onclick = () => {
      view = (b as HTMLElement).dataset.v as typeof view;
      render();
      if (view === "files") void refreshFiles();
      if (view === "settings") void loadSettings();
      if (view === "call") void setupCallUi();
    };
  });
  const out = document.getElementById("out");
  if (out)
    out.onclick = async () => {
      await client.rpc("signOut");
      unlocked = false;
      view = "gate";
      render();
    };

  if (view === "chats") wireChats();
  if (view === "files") wireFiles();
  if (view === "settings") wireSettings();
  if (view === "call") void setupCallUi();
}

function escapeHtml(s: string) {
  return s.replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]!));
}

function wireChats() {
  app.querySelectorAll(".chat-item").forEach((b) => {
    (b as HTMLButtonElement).onclick = () => {
      const key = (b as HTMLElement).dataset.k!;
      const c = conversations.find((x) => x.key === key);
      if (c) void loadHistory(c);
    };
  });
  document.getElementById("inbox")!.onclick = async () => {
    try {
      await client.rpc("startDmInbox");
      showToast("DM inbox listening");
    } catch (e) {
      showToast(String(e));
    }
  };
  const connect = document.getElementById("connect");
  if (connect)
    connect.onclick = async () => {
      try {
        const token = (document.getElementById("token") as HTMLInputElement).value.trim();
        await client.rpc("connectToken", { token });
        showToast("Connecting…");
        await refreshChats();
        render();
      } catch (e) {
        showToast(String(e));
      }
    };
  document.getElementById("mkfield")!.onclick = async () => {
    try {
      const name = (document.getElementById("gname") as HTMLInputElement).value.trim();
      await client.rpc("createGroup", { name });
      showToast("Field created");
      await refreshChats();
      render();
    } catch (e) {
      showToast(String(e));
    }
  };
  document.getElementById("hub")!.onclick = async () => {
    try {
      const groups = await client.rpc<{ id: string; name: string }[]>("listGroups");
      const gid = groups[0]?.id;
      if (!gid) {
        showToast("No field yet");
        return;
      }
      await client.rpc("startFieldHub", { groupId: gid });
      showToast("Field hub starting");
    } catch (e) {
      showToast(String(e));
    }
  };
  document.getElementById("joinfield")!.onclick = async () => {
    try {
      const invite = (document.getElementById("ginvite") as HTMLInputElement).value.trim();
      await client.rpc("joinField", { invite });
      showToast("Joining…");
    } catch (e) {
      showToast(String(e));
    }
  };
  document.getElementById("send")!.onclick = async () => {
    if (!activeConv) return;
    const text = (document.getElementById("msg") as HTMLInputElement).value;
    try {
      await client.rpc("sendMessage", { text });
      (document.getElementById("msg") as HTMLInputElement).value = "";
      await loadHistory(activeConv);
    } catch (e) {
      showToast(String(e));
    }
  };
  document.getElementById("callAudio")!.onclick = async () => {
    try {
      await client.rpc("startCall", { video: false });
      view = "call";
      render();
    } catch (e) {
      showToast(String(e));
    }
  };
  document.getElementById("callVideo")!.onclick = async () => {
    try {
      await client.rpc("startCall", { video: true });
      view = "call";
      render();
    } catch (e) {
      showToast(String(e));
    }
  };
}

async function refreshFiles() {
  try {
    localFiles = await client.rpc<FileRow[]>("listLocalFiles", { scopeGroupId: "" });
    remoteFiles = await client.rpc<FileRow[]>("listRemoteFiles");
  } catch (e) {
    showToast(String(e));
  }
  render();
}

function wireFiles() {
  document.getElementById("index")!.onclick = async () => {
    try {
      const path = (document.getElementById("folder") as HTMLInputElement).value;
      await client.rpc("addIndexedFolder", { path, scopeGroupId: "" });
      await refreshFiles();
    } catch (e) {
      showToast(String(e));
    }
  };
  document.getElementById("upload")!.onclick = async () => {
    try {
      const dest = (document.getElementById("updest") as HTMLInputElement).value.trim();
      const fileInput = document.getElementById("upfile") as HTMLInputElement;
      const file = fileInput.files?.[0];
      if (!dest || !file) {
        showToast("dest + file required");
        return;
      }
      const buf = await file.arrayBuffer();
      const res = await fetch(
        `/api/v1/files/upload?token=${encodeURIComponent(client.token)}&dest=${encodeURIComponent(dest)}`,
        {
          method: "POST",
          headers: { Authorization: `Bearer ${client.token}` },
          body: buf,
        }
      );
      if (!res.ok) throw new Error(await res.text());
      showToast("Uploaded");
      await refreshFiles();
    } catch (e) {
      showToast(String(e));
    }
  };
  document.getElementById("refLoc")!.onclick = () => void refreshFiles();
  document.getElementById("refRem")!.onclick = async () => {
    try {
      await client.rpc("requestRemoteFiles", { rootPath: "", relativePath: "", scopeGroupId: "" });
      await refreshFiles();
    } catch (e) {
      showToast(String(e));
    }
  };
  document.getElementById("queue")!.onclick = async () => {
    try {
      const q = await client.rpc("transferQueue");
      (document.getElementById("queueOut") as HTMLElement).textContent = JSON.stringify(q, null, 2);
    } catch (e) {
      showToast(String(e));
    }
  };
  app.querySelectorAll("[data-dl]").forEach((b) => {
    (b as HTMLButtonElement).onclick = async () => {
      try {
        const hash = (b as HTMLElement).dataset.dl!;
        const name = (b as HTMLElement).dataset.name || hash;
        await client.rpc("downloadFile", { hash, destPath: `/tmp/nyx-web-dl-${name}` });
        showToast("Download queued");
      } catch (e) {
        showToast(String(e));
      }
    };
  });
}

async function loadSettings() {
  try {
    const net = await client.rpc<{ rendezvousList: string }>("getNetworkConfig");
    const meta = await client.rpc<{ bio: string; interests: string; availability: string }>(
      "getProfileMeta"
    );
    contacts = await client.rpc("listContacts");
    render();
    const rv = document.getElementById("rv") as HTMLInputElement | null;
    const bio = document.getElementById("bio") as HTMLInputElement | null;
    const interests = document.getElementById("interests") as HTMLInputElement | null;
    const avail = document.getElementById("avail") as HTMLSelectElement | null;
    if (rv) rv.value = net.rendezvousList || "";
    if (bio) bio.value = meta.bio || "";
    if (interests) interests.value = meta.interests || "";
    if (avail && meta.availability) avail.value = meta.availability;
  } catch {
    /* ignore */
  }
}

function wireSettings() {
  document.getElementById("saveNet")!.onclick = async () => {
    try {
      const rendezvousList = (document.getElementById("rv") as HTMLInputElement).value;
      await client.rpc("saveNetworkSettings", { rendezvousList, discoveryMode: 0 });
      showToast("Network saved");
    } catch (e) {
      showToast(String(e));
    }
  };
  document.getElementById("saveMeta")!.onclick = async () => {
    try {
      await client.rpc("setProfileMeta", {
        bio: (document.getElementById("bio") as HTMLInputElement).value,
        interests: (document.getElementById("interests") as HTMLInputElement).value,
        availability: (document.getElementById("avail") as HTMLSelectElement).value,
      });
      showToast("Profile saved");
    } catch (e) {
      showToast(String(e));
    }
  };
  document.getElementById("lan")!.onclick = async () => {
    try {
      await client.rpc("refreshLanPeers");
      showToast("LAN scan started — watch events");
      (document.getElementById("lanOut") as HTMLElement).textContent = "scan requested";
    } catch (e) {
      showToast(String(e));
    }
  };
}

async function setupCallUi() {
  const answer = document.getElementById("answer");
  const hang = document.getElementById("hang");
  const reject = document.getElementById("reject");
  const mic = document.getElementById("mic");
  const cam = document.getElementById("cam");
  if (answer)
    answer.onclick = async () => {
      try {
        await client.rpc("acceptCall");
        await startMediaBridge(!!callState.video);
      } catch (e) {
        showToast(String(e));
      }
    };
  if (reject)
    reject.onclick = async () => {
      stopMediaBridge();
      try {
        await client.rpc("rejectCall");
      } catch (e) {
        showToast(String(e));
      }
    };
  if (hang)
    hang.onclick = async () => {
      stopMediaBridge();
      try {
        await client.rpc("hangupCall");
      } catch (e) {
        showToast(String(e));
      }
    };
  if (mic)
    mic.onclick = async () => {
      const muted = !callState.micMuted;
      await client.rpc("setCallMicMuted", { muted });
      callState.micMuted = muted;
    };
  if (cam)
    cam.onclick = async () => {
      const on = !callState.cameraOn;
      await client.rpc("setCallCameraOn", { on });
      callState.cameraOn = on;
    };

  // WebRTC peer connection stub toward host (ICE optional for same-machine)
  if (!media.pc) {
    media.pc = new RTCPeerConnection({ iceServers: [] });
  }
  if (callState.state === "active" || callState.state === "outgoing") {
    await startMediaBridge(!!callState.video);
  }
}

async function startMediaBridge(video: boolean) {
  try {
    media.stream = await navigator.mediaDevices.getUserMedia({ audio: true, video });
    const vid = document.getElementById("localVid") as HTMLVideoElement | null;
    if (vid) vid.srcObject = media.stream;
    if (media.pc && media.stream) {
      for (const track of media.stream.getTracks()) media.pc.addTrack(track, media.stream);
    }
    // PCM uplink via Web Audio → base64 frames (host encodes/forwards to CallMesh)
    media.audioCtx = new AudioContext({ sampleRate: 48000 });
    const src = media.audioCtx.createMediaStreamSource(media.stream);
    media.processor = media.audioCtx.createScriptProcessor(4096, 1, 1);
    media.processor.onaudioprocess = async (ev) => {
      const input = ev.inputBuffer.getChannelData(0);
      const pcm = new Int16Array(input.length);
      for (let i = 0; i < input.length; ++i) pcm[i] = Math.max(-32768, Math.min(32767, input[i] * 32767));
      const b64 = btoa(String.fromCharCode(...new Uint8Array(pcm.buffer)));
      try {
        await client.rpc("sendCallMedia", { mediaType: 1, payloadBase64: b64, audioLevel: 0 });
      } catch {
        /* ignore burst errors */
      }
    };
    src.connect(media.processor);
    media.processor.connect(media.audioCtx.destination);
  } catch (e) {
    showToast("Media: " + e);
  }
}

function stopMediaBridge() {
  media.processor?.disconnect();
  media.audioCtx?.close();
  media.stream?.getTracks().forEach((t) => t.stop());
  media.processor = null;
  media.audioCtx = null;
  media.stream = null;
}

client.onEvent = (name, payload) => {
  if (name === "message" && activeConv) {
    messages.push(payload as Msg);
    render();
  }
  if (name === "sessionsChanged" || name === "chatReady" || name === "groupCreated") {
    void refreshChats().then(render);
  }
  if (name === "callChanged") {
    callState = payload as Record<string, unknown>;
    if (view === "call") render();
  }
  if (name === "status") showToast(String(payload));
  if (name === "lanPeers") {
    const out = document.getElementById("lanOut");
    if (out) out.textContent = JSON.stringify(payload, null, 2);
  }
  if (name === "callMedia") {
    // Downlink: host forwards CallMesh frames as base64; decode when codec bridge is wired.
  }
  if (name === "remoteFilesChanged" || name === "transferQueueChanged") {
    if (view === "files") void refreshFiles();
  }
};

render();
