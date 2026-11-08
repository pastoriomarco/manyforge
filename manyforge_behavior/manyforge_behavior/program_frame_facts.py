"""Immutable direct-frame evidence captured with one Applied Program."""
from __future__ import annotations

from dataclasses import dataclass
from types import MappingProxyType
from typing import Mapping


@dataclass(frozen=True, slots=True)
class ProgramFrameFacts:
    components: frozenset[str]
    frames: Mapping[tuple[str, str], int]

    @classmethod
    def from_capture(cls, value: object, identity: Mapping[str, object]) -> "ProgramFrameFacts":
        fields = {"applied_record_digest", "program_binding_id", "program_revision",
                  "retained_bundle_id", "graph_revision", "components", "frames"}
        if not isinstance(value, dict) or set(value) != fields:
            raise ValueError("frame facts have an invalid closed shape")
        for key in fields - {"components", "frames"}:
            if not isinstance(value[key], str) or value[key] != identity.get(key):
                raise ValueError(f"frame facts {key} differs from captured Applied identity")
        components = value["components"]
        if (not isinstance(components, list)
                or not all(isinstance(c, str) and c for c in components)
                or components != sorted(set(components))):
            raise ValueError("frame facts components are not sorted and unique")
        rows = value["frames"]
        if not isinstance(rows, list):
            raise ValueError("frame facts frames must be an array")
        frames: dict[tuple[str, str], int] = {}
        order: list[tuple[str, str]] = []
        for row in rows:
            if not isinstance(row, dict) or set(row) != {"reference", "match_count"}:
                raise ValueError("frame fact row has an invalid closed shape")
            ref = row["reference"]
            if (not isinstance(ref, dict) or set(ref) != {"component_id", "frame_id"}
                    or not all(isinstance(ref[k], str) and ref[k] for k in ref)):
                raise ValueError("frame fact reference is invalid")
            key = (ref["component_id"], ref["frame_id"])
            count = row["match_count"]
            if key[0] not in components or type(count) is not int or count not in (1, 2):
                raise ValueError("frame fact count or component is invalid")
            order.append(key)
            frames[key] = count
        if order != sorted(set(order)):
            raise ValueError("frame facts are not sorted and unique")
        return cls(frozenset(components), MappingProxyType(frames))

    def match_count(self, component_id: str, frame_id: str) -> int:
        if component_id not in self.components:
            return 0
        return self.frames.get((component_id, frame_id), 0)
