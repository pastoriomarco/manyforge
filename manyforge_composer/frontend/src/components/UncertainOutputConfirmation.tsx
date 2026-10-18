import {
  UNCERTAIN_OUTPUT_WARNING,
  uncertainOutputKey,
  type UncertainOutput,
} from '../utils/uncertainOutputs';

/**
 * The per-output acknowledgement of retained acceptance-unknown outputs
 * (contract 66): the warning and one explicit checkbox per output. The caller
 * enables its Reset only once every listed output is approved.
 */
export function UncertainOutputConfirmation({ outputs, approved, onToggle, disabled = false }: {
  outputs: readonly UncertainOutput[];
  approved: ReadonlySet<string>;
  onToggle: (key: string, approved: boolean) => void;
  disabled?: boolean;
}) {
  if (outputs.length === 0) return null;
  return (
    <fieldset className="uncertain-output-confirmation" aria-label="Outputs with unknown acceptance">
      <legend>Outputs with unknown acceptance</legend>
      <p className="inline-message severity-warning" role="alert">
        {UNCERTAIN_OUTPUT_WARNING} Reset releases each output below only after you confirm it; its device must have been disabled and enabled again first.
      </p>
      {outputs.map((output) => {
        const key = uncertainOutputKey(output);
        return (
          <label className="toggle-control" key={key}>
            <input type="checkbox" checked={approved.has(key)} disabled={disabled}
              aria-label={`Acknowledge ${output.label}`}
              onChange={(event) => onToggle(key, event.target.checked)} />
            <span>I accept that {output.label} may still act.</span>
          </label>
        );
      })}
    </fieldset>
  );
}
