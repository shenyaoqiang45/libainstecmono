// Minimal typings for the parts of roslib used by this app.
// roslib ships no TypeScript types; keep these pragmatic.
declare module 'roslib' {
  export class Ros {
    constructor(options?: { url?: string });
    isConnected: boolean;
    on(event: 'connection' | 'close' | 'error', callback: (...args: any[]) => void): void;
    close(): void;
    // rosapi returns node-qualified names ("<node>:<param>"); filter client-side.
    getParams(callback: (params: string[]) => void, failedCallback?: (error: any) => void): void;
  }

  export class Topic {
    constructor(options: {
      ros: Ros;
      name: string;
      messageType: string;
      throttle_rate?: number;
      queue_size?: number;
      queue_length?: number;
      compression?: string;
    });
    subscribe(callback: (message: any) => void): void;
    unsubscribe(callback?: (message: any) => void): void;
  }

  export class Service {
    constructor(options: { ros: Ros; name: string; serviceType: string });
    callService(
      request: ServiceRequest,
      success: (response: any) => void,
      error?: (error: any) => void
    ): void;
  }

  export class ServiceRequest {
    constructor(values?: Record<string, any>);
  }

  export class Param {
    constructor(options: { ros: Ros; name: string });
    name: string;
    get(callback: (value: any) => void, errorCallback?: (error: any) => void): void;
    set(value: any, callback?: (response: any) => void, errorCallback?: (error: any) => void): void;
  }
}
