import type { DocumentStates } from '../state/documentStates';

/**
 * D / S / A letters for one document (Program or Deployment) beside the
 * workspace switcher. All three show while any of them differs; green
 * matches, amber differs. Each explains itself on hover.
 */
export function DocumentStateLetters({
  states,
  owner,
  side,
}: {
  states: DocumentStates;
  owner: 'Program' | 'Deployment';
  side: 'left' | 'right';
}) {
  if (!states.pending) return null;
  return (
    <span className={`document-state-letters ${side}`} aria-label={`${owner} state`}>
      {states.letters.map((letter) => (
        <span
          key={letter.letter}
          className={`document-state document-state-letter state-${letter.state}`}
          title={letter.title}
        >{letter.letter}</span>
      ))}
    </span>
  );
}
