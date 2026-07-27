import { useSyncExternalStore } from 'react';
import { Ros } from 'roslib';

export type RosStatus = 'disconnected' | 'connecting' | 'connected';

export interface RosState {
  status: RosStatus;
  ros: Ros | null;
}

const DEFAULT_URL = 'ws://localhost:9090';
const RETRY_MS = 2000;

export function defaultRosUrl(): string {
  const fromQuery = new URLSearchParams(window.location.search).get('ros');
  return fromQuery && fromQuery.trim() !== '' ? fromQuery : DEFAULT_URL;
}

let state: RosState = { status: 'disconnected', ros: null };
let currentUrl = defaultRosUrl();
let manualClose = false;
let retryTimer: number | null = null;
const listeners = new Set<() => void>();

function setState(next: RosState): void {
  state = next;
  listeners.forEach((listener) => listener());
}

function subscribe(listener: () => void): () => void {
  listeners.add(listener);
  return () => {
    listeners.delete(listener);
  };
}

function getSnapshot(): RosState {
  return state;
}

function clearRetry(): void {
  if (retryTimer !== null) {
    window.clearTimeout(retryTimer);
    retryTimer = null;
  }
}

export function connect(url?: string): void {
  if (url) currentUrl = url;
  manualClose = false;
  clearRetry();
  const prev = state.ros;
  if (prev) {
    state = { status: 'disconnected', ros: null };
    try {
      prev.close();
    } catch {
      /* already closed */
    }
  }
  const ros = new Ros({ url: currentUrl });
  setState({ status: 'connecting', ros });

  ros.on('connection', () => {
    if (state.ros === ros) setState({ status: 'connected', ros });
  });
  ros.on('error', () => {
    // rosbridge emits 'error' before 'close'; the close handler schedules the retry.
  });
  ros.on('close', () => {
    if (state.ros !== ros) return; // stale socket from a previous connect()
    setState({ status: 'disconnected', ros: null });
    if (!manualClose) {
      clearRetry();
      retryTimer = window.setTimeout(() => {
        retryTimer = null;
        connect();
      }, RETRY_MS);
    }
  });
}

export function disconnect(): void {
  manualClose = true;
  clearRetry();
  const prev = state.ros;
  setState({ status: 'disconnected', ros: null });
  if (prev) {
    try {
      prev.close();
    } catch {
      /* already closed */
    }
  }
}

export function getUrl(): string {
  return currentUrl;
}

export function useRosState(): RosState {
  return useSyncExternalStore(subscribe, getSnapshot);
}
