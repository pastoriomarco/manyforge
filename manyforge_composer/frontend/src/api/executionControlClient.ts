/** Thin dispatch ordering shared by every execution-control surface. */

export type ExecutionControlPriority = 'stop' | 'ordinary';

export class ExecutionControlClient {
  private dispatch<T>(
    _priority: ExecutionControlPriority,
    request: () => Promise<T>,
    afterDispatch: () => void,
  ): Promise<T> {
    // Calling the API function starts fetch() synchronously. Only then may the
    // UI publish pending state or trigger any local projection work.
    const response = request();
    afterDispatch();
    return response;
  }

  stop<T>(request: () => Promise<T>, afterDispatch: () => void): Promise<T> {
    return this.dispatch('stop', request, afterDispatch);
  }

  ordinary<T>(request: () => Promise<T>, afterDispatch: () => void): Promise<T> {
    return this.dispatch('ordinary', request, afterDispatch);
  }
}

export const executionControlClient = new ExecutionControlClient();
