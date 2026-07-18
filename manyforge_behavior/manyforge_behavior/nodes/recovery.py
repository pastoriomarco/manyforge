"""Deterministic recovery decorator nodes for ManyForge behavior tree (OP-6).

These nodes wrap a child and apply bounded deterministic recovery on failure.
Neither node involves model calls or external delegation — recovery is
program-only and never produces "skip" as an outcome.

py_trees Decorator.tick() already ticks the child before calling update().
In update(), read self.decorated.status — do NOT call self.decorated.tick() again.
"""

from __future__ import annotations

import logging
import time

import py_trees

from manyforge_behavior.blackboard_namespace import write_namespace_bundle

logger = logging.getLogger(__name__)


class FallbackOverrideNode(py_trees.decorators.Decorator):
    """Decorator that retries its child with overridden parameters on failure.

    On child FAILURE, applies ``fallback_params`` directly to the child node's
    ``_params`` dict (the convention used by parameterized action nodes)
    and retries the child up to ``max_retries`` times.  This ensures the child
    re-reads the overridden values in its ``initialise()``/``update()`` cycle.

    Fallback values are also mirrored to the blackboard for traceability.

    On child SUCCESS, returns SUCCESS immediately.  When retries are exhausted,
    returns FAILURE.

    Recovery metadata is recorded on the blackboard under the root namespace
    bundle ``_recovery/{node_name}``.
    """

    def __init__(
        self,
        name: str,
        child: py_trees.behaviour.Behaviour,
        *,
        fallback_params: dict,
        max_retries: int = 1,
    ):
        super().__init__(name=name, child=child)
        self._fallback_params = dict(fallback_params)
        self._max_retries = max(0, max_retries)
        self._retry_count = 0
        self._bb = py_trees.blackboard.Client(name=f"recovery_{name}")
        # Register keys lazily on first use to avoid ordering issues
        self._keys_registered = False
        # Snapshot original _params so we can restore on re-initialise
        self._original_params: dict | None = None

    def _ensure_bb_keys(self) -> None:
        if self._keys_registered:
            return
        self._bb.register_key(
            key=f"_recovery/{self.name}",
            access=py_trees.common.Access.WRITE,
        )
        self._keys_registered = True

    def _write_recovery_state(
        self,
        *,
        status: str,
        retry_count: int,
        applied_overrides: dict | None = None,
    ) -> None:
        value = {
            "retry_count": retry_count,
            "applied_overrides": dict(applied_overrides or {}),
        }
        write_namespace_bundle(
            self._bb,
            f"_recovery/{self.name}",
            value=value,
            status=status,
            timestamp_ns=time.monotonic_ns(),
        )

    def initialise(self):
        self._retry_count = 0
        # Snapshot original child params on first entry; restore on subsequent
        # entries so that fallback overrides from a previous run do not leak.
        child = self.decorated
        if hasattr(child, "_params") and isinstance(child._params, dict):
            if self._original_params is None:
                self._original_params = dict(child._params)
            else:
                child._params.update(self._original_params)

    def _apply_fallback_to_child(self) -> None:
        """Apply fallback_params to the child node's _params dict.

        Parameterized action nodes store their runtime parameters in
        ``self._params`` (captured from **kwargs at __init__).  Mutating that
        dict is the only reliable way to change the child's behaviour on
        retry, because nodes like MoveManipulatorAction build their
        ExecuteRequest from ``self._params``, not from the blackboard.
        """
        child = self.decorated
        if hasattr(child, "_params") and isinstance(child._params, dict):
            child._params.update(self._fallback_params)
        else:
            logger.warning(
                "%s: child '%s' has no _params dict; "
                "fallback overrides written to blackboard only",
                self.name,
                child.name,
            )

    def update(self) -> py_trees.common.Status:
        self._ensure_bb_keys()

        if self.decorated.status == py_trees.common.Status.SUCCESS:
            self._write_recovery_state(
                status="succeeded",
                retry_count=self._retry_count,
                applied_overrides=self._fallback_params if self._retry_count > 0 else {},
            )
            return py_trees.common.Status.SUCCESS

        if self.decorated.status == py_trees.common.Status.FAILURE:
            self._retry_count += 1
            if self._retry_count > self._max_retries:
                logger.warning(
                    "%s: fallback retries exhausted (%d/%d)",
                    self.name,
                    self._retry_count - 1,
                    self._max_retries,
                )
                self._write_recovery_state(
                    status="exhausted",
                    retry_count=self._retry_count - 1,
                    applied_overrides=self._fallback_params if self._retry_count > 1 else {},
                )
                return py_trees.common.Status.FAILURE

            # Apply fallback overrides to child's _params dict
            self._apply_fallback_to_child()
            # Mirror to blackboard for traceability
            logger.info(
                "%s: applying fallback overrides (attempt %d/%d): %s",
                self.name,
                self._retry_count,
                self._max_retries,
                list(self._fallback_params.keys()),
            )
            self._write_recovery_state(
                status="retrying",
                retry_count=self._retry_count,
                applied_overrides=self._fallback_params,
            )
            # Return RUNNING to re-tick the child on the next tree tick
            return py_trees.common.Status.RUNNING

        # RUNNING — child is still executing
        return py_trees.common.Status.RUNNING


class RecoverySkillNode(py_trees.decorators.Decorator):
    """Decorator that runs a named recovery skill when its child fails.

    On child FAILURE, the recovery skill subtree is built (via
    ``skill_expander``) and ticked.  If the recovery skill succeeds, the
    original child is retried.  If the recovery skill fails or retries are
    exhausted, the node returns FAILURE.

    The ``skill_expander`` callable is injected at build time by the runner.
    It takes ``(skill_id, params)`` and returns a ``py_trees.behaviour.Behaviour``
    subtree, or ``None`` if the skill is unknown.

    Recovery metadata is recorded on the blackboard under the root namespace
    bundle ``_recovery/{node_name}``.
    """

    def __init__(
        self,
        name: str,
        child: py_trees.behaviour.Behaviour,
        *,
        recovery_skill_id: str,
        recovery_params: dict | None = None,
        max_retries: int = 1,
        skill_expander=None,
    ):
        super().__init__(name=name, child=child)
        self._recovery_skill_id = recovery_skill_id
        self._recovery_params = dict(recovery_params or {})
        self._max_retries = max(0, max_retries)
        self._skill_expander = skill_expander
        self._retry_count = 0
        self._recovery_subtree: py_trees.behaviour.Behaviour | None = None
        self._running_recovery = False
        self._bb = py_trees.blackboard.Client(name=f"recovery_{name}")
        self._keys_registered = False

    def _ensure_bb_keys(self) -> None:
        if self._keys_registered:
            return
        self._bb.register_key(
            key=f"_recovery/{self.name}",
            access=py_trees.common.Access.WRITE,
        )
        self._keys_registered = True

    def _write_recovery_state(
        self,
        *,
        status: str,
        retry_count: int,
        recovery_skill_id: str | None = None,
    ) -> None:
        value = {
            "retry_count": retry_count,
            "recovery_skill_id": recovery_skill_id,
        }
        write_namespace_bundle(
            self._bb,
            f"_recovery/{self.name}",
            value=value,
            status=status,
            timestamp_ns=time.monotonic_ns(),
        )

    def initialise(self):
        self._retry_count = 0
        self._recovery_subtree = None
        self._running_recovery = False

    def update(self) -> py_trees.common.Status:
        self._ensure_bb_keys()

        # If we're currently running a recovery skill subtree
        if self._running_recovery and self._recovery_subtree is not None:
            self._recovery_subtree.tick_once()
            recovery_status = self._recovery_subtree.status
            if recovery_status == py_trees.common.Status.SUCCESS:
                # Recovery skill succeeded — retry the original child
                self._running_recovery = False
                self._recovery_subtree = None
                logger.info(
                    "%s: recovery skill '%s' succeeded, retrying child",
                    self.name,
                    self._recovery_skill_id,
                )
                self._write_recovery_state(
                    status="retrying",
                    retry_count=self._retry_count,
                    recovery_skill_id=self._recovery_skill_id,
                )
                return py_trees.common.Status.RUNNING
            elif recovery_status == py_trees.common.Status.FAILURE:
                # Recovery skill failed
                self._running_recovery = False
                self._recovery_subtree = None
                logger.warning(
                    "%s: recovery skill '%s' failed",
                    self.name,
                    self._recovery_skill_id,
                )
                self._write_recovery_state(
                    status="recovery_failed",
                    retry_count=self._retry_count,
                    recovery_skill_id=self._recovery_skill_id,
                )
                return py_trees.common.Status.FAILURE
            else:
                # Recovery skill is still running
                return py_trees.common.Status.RUNNING

        # Normal path: check child status
        if self.decorated.status == py_trees.common.Status.SUCCESS:
            self._write_recovery_state(
                status="succeeded",
                retry_count=self._retry_count,
                recovery_skill_id=self._recovery_skill_id,
            )
            return py_trees.common.Status.SUCCESS

        if self.decorated.status == py_trees.common.Status.FAILURE:
            self._retry_count += 1
            if self._retry_count > self._max_retries:
                logger.warning(
                    "%s: recovery retries exhausted (%d/%d)",
                    self.name,
                    self._retry_count - 1,
                    self._max_retries,
                )
                self._write_recovery_state(
                    status="exhausted",
                    retry_count=self._retry_count - 1,
                    recovery_skill_id=self._recovery_skill_id,
                )
                return py_trees.common.Status.FAILURE

            # Build and start recovery skill subtree
            if self._skill_expander is None:
                logger.error(
                    "%s: no skill_expander provided, cannot run recovery skill '%s'",
                    self.name,
                    self._recovery_skill_id,
                )
                self._write_recovery_state(
                    status="no_expander",
                    retry_count=self._retry_count,
                    recovery_skill_id=self._recovery_skill_id,
                )
                return py_trees.common.Status.FAILURE

            subtree = self._skill_expander(
                self._recovery_skill_id,
                self._recovery_params,
            )
            if subtree is None:
                logger.error(
                    "%s: recovery skill '%s' not found in catalog",
                    self.name,
                    self._recovery_skill_id,
                )
                self._write_recovery_state(
                    status="skill_not_found",
                    retry_count=self._retry_count,
                    recovery_skill_id=self._recovery_skill_id,
                )
                return py_trees.common.Status.FAILURE

            self._recovery_subtree = subtree
            self._running_recovery = True
            logger.info(
                "%s: starting recovery skill '%s' (attempt %d/%d)",
                self.name,
                self._recovery_skill_id,
                self._retry_count,
                self._max_retries,
            )
            self._write_recovery_state(
                status="recovering",
                retry_count=self._retry_count,
                recovery_skill_id=self._recovery_skill_id,
            )
            # Tick the recovery subtree once to start it
            self._recovery_subtree.tick_once()
            if self._recovery_subtree.status == py_trees.common.Status.SUCCESS:
                self._running_recovery = False
                self._recovery_subtree = None
                return py_trees.common.Status.RUNNING
            elif self._recovery_subtree.status == py_trees.common.Status.FAILURE:
                self._running_recovery = False
                self._recovery_subtree = None
                self._write_recovery_state(
                    status="recovery_failed",
                    retry_count=self._retry_count,
                    recovery_skill_id=self._recovery_skill_id,
                )
                return py_trees.common.Status.FAILURE
            return py_trees.common.Status.RUNNING

        # RUNNING — child is still executing
        return py_trees.common.Status.RUNNING
