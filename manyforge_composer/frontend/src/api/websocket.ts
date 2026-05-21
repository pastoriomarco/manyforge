import type { StateUpdate } from '../types/api';

type StateCallback = (update: StateUpdate) => void;

export class StateWebSocket {
  private ws: WebSocket | null = null;
  private callbacks: StateCallback[] = [];
  private reconnectTimer: number | null = null;
  private pingTimer: number | null = null;
  private shouldReconnect = false;
  private missedPongs = 0;
  private _stale = false;

  get stale(): boolean {
    return this._stale;
  }

  connect(url?: string) {
    this.shouldReconnect = true;
    if (this.reconnectTimer !== null) {
      window.clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
    const proto = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const wsUrl = url || `${proto}//${window.location.host}/ws/state`;
    this.ws = new WebSocket(wsUrl);
    this.ws.onmessage = (event) => {
      const text = event.data as string;
      if (text === 'pong') {
        this.missedPongs = 0;
        this._stale = false;
        return;
      }
      let data: StateUpdate;
      try {
        data = JSON.parse(text) as StateUpdate;
      } catch {
        return;
      }
      if (!data || typeof data !== 'object') {
        return;
      }
      this.callbacks.forEach(cb => cb(data));
    };
    this.ws.onopen = () => {
      this.missedPongs = 0;
      this._stale = false;
      this.startPing();
    };
    this.ws.onclose = () => {
      this.stopPing();
      if (!this.shouldReconnect) {
        return;
      }
      // Reconnect after 1s
      this.reconnectTimer = window.setTimeout(() => this.connect(url), 1000);
    };
  }

  private startPing() {
    this.stopPing();
    this.pingTimer = window.setInterval(() => {
      if (this.ws?.readyState === WebSocket.OPEN) {
        this.ws.send('ping');
        this.missedPongs++;
        if (this.missedPongs >= 3) {
          this._stale = true;
        }
      }
    }, 5000);
  }

  private stopPing() {
    if (this.pingTimer !== null) {
      window.clearInterval(this.pingTimer);
      this.pingTimer = null;
    }
  }

  subscribe(cb: StateCallback) {
    this.callbacks.push(cb);
    return () => {
      this.callbacks = this.callbacks.filter(c => c !== cb);
    };
  }

  disconnect() {
    this.shouldReconnect = false;
    this.stopPing();
    if (this.reconnectTimer !== null) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
    const ws = this.ws;
    this.ws = null;
    ws?.close();
  }
}
