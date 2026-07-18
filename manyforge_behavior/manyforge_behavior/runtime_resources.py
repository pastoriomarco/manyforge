"""Deployment-owned runtime resource factories and immutable node handles."""

from __future__ import annotations

from dataclasses import dataclass
import threading
from types import MappingProxyType
from typing import Any, Callable, Mapping


class RuntimeResourceError(ValueError):
    pass


RuntimeResourceFactory = Callable[[Mapping[str, Any]], Any]


class RuntimeResourceFactoryRegistry:
    def __init__(self, factories: Mapping[str, RuntimeResourceFactory] | None = None):
        self._factories = MappingProxyType(dict(factories or {}))

    def create(self, provider: str, config: Mapping[str, Any]) -> Any:
        try:
            factory = self._factories[provider]
        except KeyError as exc:
            raise RuntimeResourceError(
                f"runtime resource provider '{provider}' is not registered"
            ) from exc
        return factory(config)


@dataclass
class RuntimeResourceRegistry:
    resources: Mapping[str, Mapping[str, Any]]

    def __post_init__(self) -> None:
        self._close_lock = threading.Lock()
        self._closed = False

    @classmethod
    def empty(cls) -> "RuntimeResourceRegistry":
        return cls(MappingProxyType({}))

    def get(self, resource_type: str, alias: str = "default") -> Any:
        with self._close_lock:
            if self._closed:
                raise RuntimeResourceError("runtime resource registry is closed")
            aliases = self.resources.get(resource_type)
            if aliases is None:
                aliases = self.resources.get(f"{resource_type}s")
            if aliases is not None and alias in aliases:
                return aliases[alias]
        raise RuntimeResourceError(
            f"runtime resource '{resource_type}:{alias}' is unavailable"
        )

    def get_plc_client(self, alias: str = "default") -> Any:
        return self.get("plc_client", alias)

    def close(self) -> None:
        with self._close_lock:
            if self._closed:
                return
            self._closed = True
        seen: set[int] = set()
        errors: list[str] = []
        for aliases in self.resources.values():
            for handle in aliases.values():
                if id(handle) in seen:
                    continue
                seen.add(id(handle))
                close = getattr(handle, "close", None)
                if callable(close):
                    try:
                        close()
                    except (Exception, SystemExit) as exc:
                        errors.append(f"{type(exc).__name__}: {exc}")
        if errors:
            raise RuntimeResourceError(
                "runtime resource close failed after all handles were attempted: "
                + "; ".join(errors)
            )


def build_runtime_resource_registry(
    raw: Mapping[str, Any],
    factories: RuntimeResourceFactoryRegistry,
) -> RuntimeResourceRegistry:
    built: dict[str, Mapping[str, Any]] = {}
    opened: list[Any] = []
    try:
        for resource_type, aliases_raw in raw.items():
            if not isinstance(resource_type, str) or not resource_type:
                raise RuntimeResourceError("runtime resource type must be non-empty")
            if not isinstance(aliases_raw, Mapping):
                raise RuntimeResourceError(
                    f"runtime_resources.{resource_type} must be a mapping of aliases"
                )
            aliases: dict[str, Any] = {}
            for alias, declaration in aliases_raw.items():
                if not isinstance(alias, str) or not alias:
                    raise RuntimeResourceError(
                        f"runtime_resources.{resource_type} alias must be non-empty"
                    )
                if not isinstance(declaration, Mapping):
                    raise RuntimeResourceError(
                        f"runtime_resources.{resource_type}.{alias} must be a mapping"
                    )
                provider = declaration.get("provider")
                if not isinstance(provider, str) or not provider:
                    raise RuntimeResourceError(
                        f"runtime_resources.{resource_type}.{alias}.provider is required"
                    )
                # Provider and capability declarations are ManyForge routing /
                # admission metadata, not provider configuration.
                config = {
                    key: value
                    for key, value in declaration.items()
                    if key not in {"provider", "capabilities"}
                }
                handle = factories.create(provider, config)
                opened.append(handle)
                aliases[alias] = handle
            built[resource_type] = MappingProxyType(aliases)
    except (Exception, SystemExit) as exc:
        close_errors: list[str] = []
        closed: set[int] = set()
        for handle in reversed(opened):
            if id(handle) in closed:
                continue
            closed.add(id(handle))
            close = getattr(handle, "close", None)
            if callable(close):
                try:
                    close()
                except (Exception, SystemExit) as close_exc:
                    close_errors.append(
                        f"{type(close_exc).__name__}: {close_exc}"
                    )
        if close_errors:
            exc.add_note(
                "runtime-resource rollback close errors: " + "; ".join(close_errors)
            )
        raise
    return RuntimeResourceRegistry(MappingProxyType(built))
