/** System signals are supplied by the generation-qualified Deployment Draft candidate. */
export const CARRIED_OBJECT_LOST_SIGNAL_PREFIX = 'carried_object_lost.';

export interface CarriedObjectLostSignalChoice {
  controllableComponentId: string;
  commandTargetId: string;
  signalName: string;
  direction: 'input';
  valueType: 'bool';
}
