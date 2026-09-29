import { Fragment } from 'react';

/**
 * An identifier that may wrap only after its underscores, dots or colons,
 * never in the middle of a word.
 */
export function BreakableId({ text }: { text: string }) {
  const parts = text.split(/(?<=[_.:])/);
  return (
    <>
      {parts.map((part, index) => (
        <Fragment key={index}>{part}{index < parts.length - 1 && <wbr />}</Fragment>
      ))}
    </>
  );
}
