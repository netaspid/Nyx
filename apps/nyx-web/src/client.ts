export type RpcResult = {
  type: string;
  id?: string;
  ok?: boolean;
  result?: unknown;
  error?: string;
  name?: string;
  payload?: unknown;
};

export class NyxClient {
  token = "";
  sessionId = "";
  private ws: WebSocket | null = null;
  private pending = new Map<string, { resolve: (v: RpcResult) => void; reject: (e: Error) => void }>();
  private seq = 0;
  onEvent: ((name: string, payload: unknown) => void) | null = null;

  async createSession(adminToken = ""): Promise<void> {
    const headers: Record<string, string> = { "Content-Type": "application/json" };
    if (adminToken) headers.Authorization = `Bearer ${adminToken}`;
    const res = await fetch("/api/v1/session", { method: "POST", headers });
    if (!res.ok) throw new Error(await res.text());
    const body = await res.json();
    this.token = body.token;
    this.sessionId = body.sessionId;
    await this.connectWs();
  }

  private connectWs(): Promise<void> {
    return new Promise((resolve, reject) => {
      const proto = location.protocol === "https:" ? "wss" : "ws";
      this.ws = new WebSocket(`${proto}://${location.host}/api/v1/ws?token=${this.token}`);
      this.ws.onopen = () => resolve();
      this.ws.onerror = () => reject(new Error("ws failed"));
      this.ws.onmessage = (ev) => {
        let msg: RpcResult;
        try {
          msg = JSON.parse(String(ev.data));
        } catch {
          return;
        }
        if (msg.type === "rpc_result" && msg.id && this.pending.has(msg.id)) {
          const p = this.pending.get(msg.id)!;
          this.pending.delete(msg.id);
          p.resolve(msg);
          return;
        }
        if (msg.type === "event" && msg.name) this.onEvent?.(msg.name, msg.payload);
      };
    });
  }

  async rpc<T = unknown>(op: string, args: Record<string, unknown> = {}): Promise<T> {
    const id = String(++this.seq);
    const payload = JSON.stringify({ type: "rpc", id, op, args });
    if (this.ws && this.ws.readyState === WebSocket.OPEN) {
      const result = await new Promise<RpcResult>((resolve, reject) => {
        this.pending.set(id, { resolve, reject });
        this.ws!.send(payload);
        setTimeout(() => {
          if (this.pending.has(id)) {
            this.pending.delete(id);
            reject(new Error("rpc timeout"));
          }
        }, 30000);
      });
      if (!result.ok) throw new Error(result.error || "rpc failed");
      return result.result as T;
    }
    const res = await fetch("/api/v1/rpc", {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        Authorization: `Bearer ${this.token}`,
      },
      body: payload,
    });
    const result = (await res.json()) as RpcResult;
    if (!result.ok) throw new Error(result.error || "rpc failed");
    return result.result as T;
  }
}
