import type { StateUpdate } from '../types/api';

type StateCallback = (update: StateUpdate) => void;

export class StateWebSocket {
  private ws: WebSocket | null = null;
  private callbacks: StateCallback[] = [];
  private reconnectTimer: number | null = null;

  connect(url?: string) {
    const wsUrl = url || `ws://${window.location.host}/ws/state`;
    this.ws = new WebSocket(wsUrl);
    this.ws.onmessage = (event) => {
      const data = JSON.parse(event.data) as StateUpdate;
      this.callbacks.forEach(cb => cb(data));
    };
    this.ws.onclose = () => {
      // Reconnect after 1s
      this.reconnectTimer = window.setTimeout(() => this.connect(url), 1000);
    };
  }

  subscribe(cb: StateCallback) {
    this.callbacks.push(cb);
    return () => {
      this.callbacks = this.callbacks.filter(c => c !== cb);
    };
  }

  disconnect() {
    if (this.reconnectTimer) clearTimeout(this.reconnectTimer);
    this.ws?.close();
  }
}
