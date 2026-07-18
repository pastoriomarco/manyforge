"""Immutable executable registry paired with an effective node catalog."""

from __future__ import annotations

import importlib
import inspect
import logging
import sys
from dataclasses import dataclass, field
from importlib.machinery import ModuleSpec
from importlib.metadata import PackageNotFoundError, version
from types import MappingProxyType, ModuleType
from typing import Any, Callable, Mapping

import py_trees

from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.node_descriptor import NodeDescriptor


class NodeRegistryError(ValueError):
    """Catalog runtime metadata cannot produce a coherent registry."""


@dataclass(frozen=True)
class NodeFactoryContext:
    name: str
    params: Mapping[str, Any]
    children: tuple[py_trees.behaviour.Behaviour, ...] = ()
    services: Mapping[str, Any] = field(default_factory=dict)


NodeFactory = Callable[[NodeFactoryContext], py_trees.behaviour.Behaviour]

logger = logging.getLogger(__name__)


class _ExternalNodeBoundary(py_trees.decorators.Decorator):
    """Convert trusted extension exceptions into an observable tree failure."""

    def __init__(self, *, node_id: str, child: py_trees.behaviour.Behaviour):
        super().__init__(name=child.name, child=child)
        self._node_id = node_id
        self._manyforge_failure_code: str | None = None
        self._manyforge_failure_details: dict[str, str] | None = None
        child._manyforge_internal_status_node = True

    def update(self) -> py_trees.common.Status:
        return self.decorated.status

    def initialise(self) -> None:
        # A retry is a new lifecycle. Do not let an exception from an earlier
        # attempt relabel a later, ordinary FAILURE in cycle diagnostics.
        self._manyforge_failure_code = None
        self._manyforge_failure_details = None
        self.feedback_message = ""

    def _record_exception(self, exc: BaseException) -> None:
        self._manyforge_failure_code = "external_node_exception"
        self._manyforge_failure_details = {
            "node_id": self._node_id,
            "exception_type": type(exc).__name__,
            "message": str(exc),
        }
        self.feedback_message = (
            f"external_node_exception:{type(exc).__name__}: {exc}"
        )

    def _fence_decorated_after_cleanup_failure(self) -> None:
        self.decorated.status = py_trees.common.Status.INVALID
        self.decorated.iterator = self.decorated.tick()

    def tick(self):
        try:
            yield from super().tick()
        except (Exception, SystemExit) as exc:
            logger.exception("external node '%s' tick failed", self._node_id)
            self._record_exception(exc)
            try:
                self.decorated.stop(py_trees.common.Status.INVALID)
            except (Exception, SystemExit):
                logger.exception(
                    "external node '%s' cleanup after failure also failed",
                    self._node_id,
                )
                self._fence_decorated_after_cleanup_failure()
            py_trees.behaviour.Behaviour.stop(
                self, py_trees.common.Status.FAILURE
            )
            yield self

    def stop(self, new_status: py_trees.common.Status) -> None:
        """Contain extension cleanup failures at the trusted-code boundary."""
        try:
            super().stop(new_status)
            return
        except (Exception, SystemExit) as exc:
            logger.exception(
                "external node '%s' termination failed", self._node_id
            )
            self._record_exception(exc)

        # A terminate() implementation that raised may have left the child in
        # RUNNING. It must not be ticked again after its parent invalidated it.
        self._fence_decorated_after_cleanup_failure()
        effective_status = (
            py_trees.common.Status.INVALID
            if new_status is py_trees.common.Status.INVALID
            else py_trees.common.Status.FAILURE
        )
        py_trees.behaviour.Behaviour.stop(self, effective_status)


def _load_symbol(reference: str) -> Any:
    module_name, separator, attribute = reference.partition(":")
    if not separator:
        module_name, separator, attribute = reference.rpartition(".")
    if not module_name or not attribute:
        raise NodeRegistryError(f"invalid import reference '{reference}'")
    try:
        module = importlib.import_module(module_name)
        return getattr(module, attribute)
    except (ImportError, AttributeError) as exc:
        raise NodeRegistryError(f"cannot import '{reference}': {exc}") from exc


def _class_factory(cls: type) -> NodeFactory:
    if not issubclass(cls, py_trees.behaviour.Behaviour):
        raise NodeRegistryError(
            f"{cls.__module__}.{cls.__qualname__} is not a py_trees Behaviour"
        )

    def factory(context: NodeFactoryContext) -> py_trees.behaviour.Behaviour:
        return cls(name=context.name, **dict(context.params))

    return factory


def _validate_name_kwargs_constructor(cls: type, *, node_id: str) -> None:
    signature = inspect.signature(cls.__init__)
    parameters = list(signature.parameters.values())[1:]
    accepts_name = any(
        parameter.name == "name"
        and parameter.kind
        in {parameter.POSITIONAL_OR_KEYWORD, parameter.KEYWORD_ONLY}
        for parameter in parameters
    )
    accepts_kwargs = any(
        parameter.kind is parameter.VAR_KEYWORD for parameter in parameters
    )
    unsupported = [
        parameter.name
        for parameter in parameters
        if parameter.name != "name"
        and parameter.kind is not parameter.VAR_KEYWORD
    ]
    if not accepts_name or not accepts_kwargs or unsupported:
        suffix = (
            f"; unsupported explicit parameters: {unsupported}"
            if unsupported
            else ""
        )
        raise NodeRegistryError(
            f"external node '{node_id}' constructor must be name and **params only"
            f"{suffix}"
        )


def _import_external_class(descriptor: NodeDescriptor) -> type:
    try:
        installed = version("py_trees")
    except PackageNotFoundError as exc:
        raise NodeRegistryError("py_trees is not installed") from exc
    if installed != "2.4.0":
        raise NodeRegistryError(
            f"external node '{descriptor.node_id}' requires py_trees 2.4.0; "
            f"process has {installed}"
        )
    digest = descriptor.content_hash.removeprefix("sha256:")[:16]
    module_name = f"manyforge_external_{descriptor.node_id}_{digest}"
    module = ModuleType(module_name)
    module.__file__ = str(descriptor.source_path)
    module.__package__ = ""
    module.__spec__ = ModuleSpec(
        module_name,
        loader=None,
        origin=str(descriptor.source_path),
    )
    sys.modules[module_name] = module
    try:
        source_identity = (
            f"{descriptor.source_path}@{descriptor.content_hash}"
        )
        code = compile(
            descriptor.source_bytes,
            source_identity,
            "exec",
        )
        exec(code, module.__dict__)
        cls = getattr(module, descriptor.runtime.class_name)
    except (Exception, SystemExit) as exc:
        sys.modules.pop(module_name, None)
        raise NodeRegistryError(
            f"external node '{descriptor.node_id}' import failed: {exc}"
        ) from exc
    if not isinstance(cls, type) or not issubclass(
        cls, py_trees.behaviour.Behaviour
    ):
        sys.modules.pop(module_name, None)
        raise NodeRegistryError(
            f"external node '{descriptor.node_id}' runtime class must subclass "
            "py_trees.behaviour.Behaviour"
        )
    try:
        _validate_name_kwargs_constructor(cls, node_id=descriptor.node_id)
    except Exception:
        sys.modules.pop(module_name, None)
        raise
    # The class and its functions retain the executed module globals. Keeping
    # every content-addressed reload in sys.modules would create an unbounded
    # process-lifetime module cache, while external_node.v0 deliberately does
    # not support import-by-module-name or pickling as part of its contract.
    sys.modules.pop(module_name, None)
    return cls


def load_external_node_factory(descriptor: NodeDescriptor) -> NodeFactory:
    """Import and validate one descriptor without publishing a registry."""
    try:
        return _class_factory(_import_external_class(descriptor))
    except NodeRegistryError:
        raise
    except (Exception, SystemExit) as exc:
        raise NodeRegistryError(
            f"external node '{descriptor.node_id}' runtime validation failed: {exc}"
        ) from exc


class NodeRegistry:
    """Read-only map from catalog node ID to executable factory."""

    def __init__(
        self,
        factories: Mapping[str, NodeFactory],
        *,
        external_node_ids: set[str] | frozenset[str] = frozenset(),
    ):
        self._factories = MappingProxyType(dict(factories))
        self._external_node_ids = frozenset(external_node_ids)

    def instantiate(
        self,
        node_id: str,
        context: NodeFactoryContext,
    ) -> py_trees.behaviour.Behaviour:
        try:
            factory = self._factories[node_id]
        except KeyError as exc:
            raise NodeRegistryError(f"node '{node_id}' has no runtime factory") from exc
        try:
            result = factory(context)
        except (Exception, SystemExit) as exc:
            raise NodeRegistryError(
                f"factory for '{node_id}' failed: {exc}"
            ) from exc
        if not isinstance(result, py_trees.behaviour.Behaviour):
            raise NodeRegistryError(
                f"factory for '{node_id}' returned {type(result).__name__}, not Behaviour"
            )
        if node_id in self._external_node_ids:
            return _ExternalNodeBoundary(node_id=node_id, child=result)
        return result

    def contains(self, node_id: str) -> bool:
        return node_id in self._factories

    @property
    def node_ids(self) -> tuple[str, ...]:
        return tuple(self._factories)


def build_node_registry(
    catalog: NodeCatalog,
    descriptors: Mapping[str, NodeDescriptor],
    *,
    external_factories: Mapping[str, NodeFactory] | None = None,
) -> NodeRegistry:
    """Build all factories before publishing a catalog/registry pair."""
    factories: dict[str, NodeFactory] = {}
    for entry in catalog.entries:
        if entry.kind == "authoring_macro":
            continue
        runner = entry.runners.get("py_trees")
        if not isinstance(runner, dict):
            raise NodeRegistryError(f"node '{entry.id}' has invalid py_trees metadata")
        implementation_kind = runner.get("implementation_kind")
        if implementation_kind == "external_python":
            descriptor = descriptors.get(entry.id)
            if descriptor is None:
                raise NodeRegistryError(
                    f"external node '{entry.id}' has no accepted descriptor"
                )
            if external_factories is not None:
                try:
                    factory = external_factories[entry.id]
                except KeyError as exc:
                    raise NodeRegistryError(
                        f"external node '{entry.id}' has no validated factory"
                    ) from exc
            else:
                factory = load_external_node_factory(descriptor)
        elif "factory" in runner:
            factory = _load_symbol(str(runner["factory"]))
            if not callable(factory):
                raise NodeRegistryError(
                    f"factory for '{entry.id}' is not callable"
                )
        elif implementation_kind == "plugin" and "class" in runner:
            cls = _load_symbol(str(runner["class"]))
            if not isinstance(cls, type):
                raise NodeRegistryError(f"class for '{entry.id}' is not a type")
            factory = _class_factory(cls)
        else:
            raise NodeRegistryError(
                f"node '{entry.id}' must declare a registry factory or plugin class"
            )
        factories[entry.id] = factory
    return NodeRegistry(factories, external_node_ids=set(descriptors))
