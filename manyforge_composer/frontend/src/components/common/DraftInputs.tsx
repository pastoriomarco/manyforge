import { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState } from 'react';
import type {
  InputHTMLAttributes,
  KeyboardEvent,
  ReactNode,
  TextareaHTMLAttributes,
} from 'react';

type DraftInputResetKey = string | number;
type DraftInputResetScope = 'scenario' | 'program' | 'deployment';
type DraftInputResetKeys = Record<DraftInputResetScope, DraftInputResetKey> & {
  activeScope: DraftInputResetScope;
};

const DEFAULT_DRAFT_INPUT_RESET_KEYS: DraftInputResetKeys = {
  scenario: 0, program: 0, deployment: 0, activeScope: 'scenario',
};
const DraftInputResetContext = createContext(DEFAULT_DRAFT_INPUT_RESET_KEYS);

export function useDraftInputResetSignal(): {
  scenarioResetKey: DraftInputResetKey;
  scopedResetKey: DraftInputResetKey;
} {
  const resetKeys = useContext(DraftInputResetContext);
  const scenarioResetKey = resetKeys.scenario;
  const scopedResetKey = resetKeys[resetKeys.activeScope];
  return useMemo(() => ({ scenarioResetKey, scopedResetKey }), [scenarioResetKey, scopedResetKey]);
}

/** A successful Draft reset/replacement rebases pending text across the workbench. */
export function DraftInputResetProvider({ scope = 'scenario', resetKey, children }: {
  scope?: DraftInputResetScope;
  resetKey: DraftInputResetKey;
  children: ReactNode;
}) {
  const parent = useContext(DraftInputResetContext);
  const keys = useMemo(() => ({ ...parent, [scope]: resetKey, activeScope: scope }), [parent, resetKey, scope]);
  return (
    <DraftInputResetContext.Provider value={keys}>
      {children}
    </DraftInputResetContext.Provider>
  );
}

function formatDraftValue(value: string | number | null | undefined): string {
  return value === null || value === undefined ? '' : String(value);
}

type DraftTextInputProps = Omit<
  InputHTMLAttributes<HTMLInputElement>,
  'value' | 'defaultValue' | 'onChange'
> & {
  value: string | number | null | undefined;
  onCommit: (value: string) => void;
  validate?: (value: string) => string | null;
  onInvalidChange?: (message: string | null) => void;
  onPendingChange?: (pending: boolean) => void;
  resetKey?: string | number;
  shiftStepMultiplier?: number;
  commitNativeStep?: boolean;
  validateOnInput?: boolean;
  dirtyClassName?: string;
};

export function DraftTextInput({
  value,
  onCommit,
  validate,
  onInvalidChange,
  onPendingChange,
  resetKey,
  shiftStepMultiplier,
  commitNativeStep,
  validateOnInput,
  dirtyClassName,
  onBlur,
  onFocus,
  onKeyDown,
  ...props
}: DraftTextInputProps) {
  const { scenarioResetKey, scopedResetKey } = useDraftInputResetSignal();
  const normalizedValue = formatDraftValue(value);
  const [draft, setDraft] = useState(normalizedValue);
  const [focused, setFocused] = useState(false);
  const [invalidMessage, setInvalidMessage] = useState<string | null>(null);
  const skipNextCommit = useRef(false);
  const inputRef = useRef<HTMLInputElement | null>(null);
  const userEdited = useRef(false);

  useEffect(() => {
    if (!userEdited.current || (!focused && invalidMessage === null)) {
      userEdited.current = false;
      setDraft(normalizedValue);
    } else if (draft === normalizedValue && invalidMessage === null) {
      userEdited.current = false;
    }
  }, [draft, focused, invalidMessage, normalizedValue]);

  useEffect(() => {
    onPendingChange?.(invalidMessage !== null || (userEdited.current && draft !== normalizedValue));
  }, [draft, invalidMessage, normalizedValue, onPendingChange]);

  const currentValue = useRef(normalizedValue);
  currentValue.current = normalizedValue;
  const appliedReset = useRef<{
    localKey: typeof resetKey;
    scenarioKey: typeof scenarioResetKey;
    scopedKey: typeof scopedResetKey;
  } | null>(null);
  useEffect(() => {
    // StrictMode can replay effects when a keyed row moves. Either explicit
    // reset identity can rebase text; an effect replay is not another reset.
    if (appliedReset.current !== null
      && appliedReset.current.localKey === resetKey
      && appliedReset.current.scenarioKey === scenarioResetKey
      && appliedReset.current.scopedKey === scopedResetKey) return;
    appliedReset.current = { localKey: resetKey, scenarioKey: scenarioResetKey, scopedKey: scopedResetKey };
    userEdited.current = false;
    setDraft(currentValue.current);
    setInvalidMessage(null);
    onInvalidChange?.(null);
    onPendingChange?.(false);
    // Only an explicit local or global rebase resets even an invalid/focused
    // input. Ordinary owner observations must not overwrite what is being typed.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [resetKey, scenarioResetKey, scopedResetKey]);

  const validateDraft = useCallback(() => {
    const message = validate?.(draft) ?? null;
    setInvalidMessage(message);
    onInvalidChange?.(message);
    return message;
  }, [draft, onInvalidChange, validate]);
  const commit = useCallback(() => {
    if (!userEdited.current) return;
    const message = validateDraft();
    if (message === null && draft !== normalizedValue) onCommit(draft);
  }, [draft, normalizedValue, onCommit, validateDraft]);
  const reset = useCallback(() => {
    userEdited.current = false;
    setDraft(normalizedValue);
    setInvalidMessage(null);
    onInvalidChange?.(null);
    onPendingChange?.(false);
  }, [normalizedValue, onInvalidChange, onPendingChange]);

  useEffect(() => {
    const input = inputRef.current;
    if (!commitNativeStep || input === null) return undefined;
    const onInput = (event: Event) => {
      const inputEvent = event as InputEvent;
      // Firefox's number spinner emits insertReplacementText without focusing
      // the input; Chromium emits a plain input event. Neither is an unfinished
      // typed draft. Text, paste and IME edits still commit on Enter/blur.
      const nativeStep = !inputEvent.isComposing
        && (!inputEvent.inputType || inputEvent.inputType === 'insertReplacementText');
      let candidate = input.value;
      userEdited.current = true;
      if (!nativeStep) {
        const message = validate?.(candidate) ?? null;
        setDraft(candidate);
        if (validateOnInput) {
          setInvalidMessage(message);
          onInvalidChange?.(message);
        }
        onPendingChange?.(candidate !== normalizedValue || message !== null);
        return;
      }
      if (candidate.trim() && Number.isFinite(Number(candidate))) {
        candidate = String(Math.min(
          props.max === undefined ? Infinity : Number(props.max),
          Math.max(props.min === undefined ? -Infinity : Number(props.min), Number(candidate)),
        ));
      }
      const message = validate?.(candidate) ?? null;
      setDraft(candidate);
      setInvalidMessage(message);
      onInvalidChange?.(message);
      if (message === null) {
        userEdited.current = false;
        onPendingChange?.(false);
        if (candidate !== normalizedValue) onCommit(candidate);
      }
    };
    input.addEventListener('input', onInput);
    return () => {
      input.removeEventListener('input', onInput);
    };
  }, [commitNativeStep, normalizedValue, onCommit, onInvalidChange, onPendingChange,
    props.min, props.max, validate, validateOnInput]);

  return (
    <input
      ref={inputRef}
      {...props}
      // Native stepping must be relative to the displayed value, not an HTML
      // grid anchored to a radian-to-degree converted limit. Firefox can round
      // back to the same grid point indefinitely. Validate typed bounds and
      // clamp native/keyboard steps above instead (as in the former panel).
      min={commitNativeStep ? undefined : props.min}
      max={commitNativeStep ? undefined : props.max}
      className={`${props.className ?? ''}${
        dirtyClassName && draft !== normalizedValue ? ` ${dirtyClassName}` : ''
      }${invalidMessage ? ' draft-input-invalid' : ''}`.trim()}
      aria-invalid={invalidMessage ? true : props['aria-invalid']}
      title={invalidMessage ?? props.title}
      value={draft}
      onFocus={(event) => {
        setFocused(true);
        onFocus?.(event);
      }}
      onChange={(event) => {
        // One native input writer for numeric controls; do not let React's
        // delegated change restore the pre-commit text or mark it pending.
        if (commitNativeStep) return;
        const next = event.target.value;
        userEdited.current = true;
        setDraft(next);
        const message = validate?.(next) ?? null;
        if (validateOnInput) {
          setInvalidMessage(message);
          onInvalidChange?.(message);
        }
        onPendingChange?.(next !== normalizedValue || message !== null);
      }}
      onBlur={(event) => {
        if (skipNextCommit.current) skipNextCommit.current = false;
        else commit();
        setFocused(false);
        onBlur?.(event);
      }}
      onKeyDown={(event: KeyboardEvent<HTMLInputElement>) => {
        if (
          shiftStepMultiplier !== undefined
          && (event.key === 'ArrowUp' || event.key === 'ArrowDown')
          && props.type === 'number'
        ) {
          const current = Number(draft);
          const baseStep = typeof props.step === 'number'
            ? props.step
            : Number(props.step ?? 1);
          if (Number.isFinite(current) && Number.isFinite(baseStep)) {
            const direction = event.key === 'ArrowUp' ? 1 : -1;
            const minimum = props.min === undefined ? -Infinity : Number(props.min);
            const maximum = props.max === undefined ? Infinity : Number(props.max);
            const next = String(Math.min(maximum, Math.max(
              minimum,
              current + direction * baseStep * (
                event.shiftKey ? shiftStepMultiplier : 1
              ),
            )));
            userEdited.current = true;
            setDraft(next);
            onPendingChange?.(next !== normalizedValue || (validate?.(next) ?? null) !== null);
            if (commitNativeStep) {
              const message = validate?.(next) ?? null;
              setInvalidMessage(message);
              onInvalidChange?.(message);
              if (message === null) {
                userEdited.current = false;
                onPendingChange?.(false);
                if (next !== normalizedValue) onCommit(next);
              }
            }
            event.preventDefault();
            return;
          }
        }
        if (event.key === 'Enter' || event.key === 'Tab') {
          if (validateDraft() !== null) {
            event.preventDefault();
            return;
          }
        }
        if (event.key === 'Enter') {
          event.preventDefault();
          event.currentTarget.blur();
          return;
        }
        if (event.key === 'Escape') {
          event.preventDefault();
          skipNextCommit.current = true;
          reset();
          event.currentTarget.blur();
          return;
        }
        onKeyDown?.(event);
      }}
    />
  );
}

type DraftTextareaProps = Omit<
  TextareaHTMLAttributes<HTMLTextAreaElement>,
  'value' | 'defaultValue' | 'onChange'
> & {
  value: string | number | null | undefined;
  onCommit: (value: string) => void;
  validate?: (value: string) => string | null;
  onInvalidChange?: (message: string | null) => void;
};

export function DraftTextarea({
  value,
  onCommit,
  validate,
  onInvalidChange,
  onBlur,
  onFocus,
  onKeyDown,
  ...props
}: DraftTextareaProps) {
  const { scenarioResetKey, scopedResetKey } = useDraftInputResetSignal();
  const normalizedValue = formatDraftValue(value);
  const [draft, setDraft] = useState(normalizedValue);
  const [focused, setFocused] = useState(false);
  const [invalidMessage, setInvalidMessage] = useState<string | null>(null);
  const skipNextCommit = useRef(false);
  const currentValue = useRef(normalizedValue);
  const currentOnInvalidChange = useRef(onInvalidChange);
  const appliedReset = useRef<{
    scenarioKey: typeof scenarioResetKey;
    scopedKey: typeof scopedResetKey;
  } | null>(null);

  useEffect(() => {
    if (!focused && invalidMessage === null) setDraft(normalizedValue);
  }, [focused, invalidMessage, normalizedValue]);

  useEffect(() => {
    currentValue.current = normalizedValue;
    currentOnInvalidChange.current = onInvalidChange;
  }, [normalizedValue, onInvalidChange]);

  useEffect(() => {
    if (appliedReset.current !== null
      && appliedReset.current.scenarioKey === scenarioResetKey
      && appliedReset.current.scopedKey === scopedResetKey) return;
    appliedReset.current = { scenarioKey: scenarioResetKey, scopedKey: scopedResetKey };
    setDraft(currentValue.current);
    setInvalidMessage(null);
    currentOnInvalidChange.current?.(null);
  }, [scenarioResetKey, scopedResetKey]);

  const validateDraft = useCallback(() => {
    const message = validate?.(draft) ?? null;
    setInvalidMessage(message);
    onInvalidChange?.(message);
    return message;
  }, [draft, onInvalidChange, validate]);
  const commit = useCallback(() => {
    const message = validateDraft();
    if (message === null && draft !== normalizedValue) onCommit(draft);
  }, [draft, normalizedValue, onCommit, validateDraft]);
  const reset = useCallback(() => {
    setDraft(normalizedValue);
    setInvalidMessage(null);
    onInvalidChange?.(null);
  }, [normalizedValue, onInvalidChange]);

  return (
    <textarea
      {...props}
      className={`${props.className ?? ''}${invalidMessage ? ' draft-input-invalid' : ''}`.trim()}
      aria-invalid={invalidMessage ? true : props['aria-invalid']}
      title={invalidMessage ?? props.title}
      value={draft}
      onFocus={(event) => {
        setFocused(true);
        onFocus?.(event);
      }}
      onChange={(event) => setDraft(event.target.value)}
      onBlur={(event) => {
        if (skipNextCommit.current) skipNextCommit.current = false;
        else commit();
        setFocused(false);
        onBlur?.(event);
      }}
      onKeyDown={(event: KeyboardEvent<HTMLTextAreaElement>) => {
        if (event.key === 'Tab' && validateDraft() !== null) {
          event.preventDefault();
          return;
        }
        if ((event.ctrlKey || event.metaKey) && event.key === 'Enter') {
          if (validateDraft() !== null) {
            event.preventDefault();
            return;
          }
          event.preventDefault();
          event.currentTarget.blur();
          return;
        }
        if (event.key === 'Escape') {
          event.preventDefault();
          skipNextCommit.current = true;
          reset();
          event.currentTarget.blur();
          return;
        }
        onKeyDown?.(event);
      }}
    />
  );
}
