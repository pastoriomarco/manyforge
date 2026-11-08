"""Validation for the shared bounded Program reference refusal shape."""

from typing import Any


def validate_reference_refusal(value: object, *, allow_empty: bool = False) -> dict[str, Any] | None:
    if not isinstance(value, dict) or set(value) != {"diagnostics", "total_count", "truncated"}:
        return None
    rows, total, truncated = value["diagnostics"], value["total_count"], value["truncated"]
    if (not isinstance(rows, list) or type(total) is not int
            or total < (0 if allow_empty else 1)
            or type(truncated) is not bool or total < len(rows)
            or truncated != (total > len(rows))):
        return None
    for row in rows:
        if (not isinstance(row, dict) or set(row) != {"code", "location", "payload"}
                or not isinstance(row["code"], str) or not row["code"]
                or not isinstance(row["payload"], dict)):
            return None
        location = row["location"]
        if (not isinstance(location, dict) or set(location) != {
                "document", "path", "node_instance_id", "parameter"
            } or location["document"] != "program"
                or not isinstance(location["path"], str)
                or (location["path"] != "" and not location["path"].startswith("/"))
                or any(value is not None and not isinstance(value, str)
                       for value in (location["node_instance_id"], location["parameter"]))):
            return None
    return {"diagnostics": rows, "total_count": total, "truncated": truncated}
