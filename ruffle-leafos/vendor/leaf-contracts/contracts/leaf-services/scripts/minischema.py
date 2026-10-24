"""Minimal JSON Schema (draft-07 subset) validator.

Supports exactly the keywords used by the schemas in this directory:
type, const, enum, pattern, not, properties, additionalProperties,
required, items, minItems, maxItems, minLength, maxLength, minimum,
maximum, allOf, anyOf, $ref (local, "#/definitions/<name>").

This is not a general-purpose validator. It exists so the fixtures in
this directory are actually machine-checked against the schema files
without pulling in a third-party dependency for a docs-only repo. If a
schema file starts using a keyword this module does not implement, that
is a bug in the fixture harness, not a silent pass -- unimplemented
keywords are ignored, so add support here before relying on one.
"""
from __future__ import annotations

import re
from typing import Any


class ValidationError(Exception):
    def __init__(self, path: str, message: str):
        super().__init__(f"{path or '$'}: {message}")
        self.path = path
        self.message = message


def _json_eq(a: Any, b: Any) -> bool:
    """JSON-type-aware equality: JSON Schema's `const`/`enum` distinguish
    the boolean type from the number type, but Python's == does not
    (True == 1, False == 0). Without this, {"v": true} would satisfy
    {"const": 1}, and {"schema": true} would pass a service.schema
    check meant to require the integer 1."""
    if isinstance(a, bool) != isinstance(b, bool):
        return False
    return a == b


def _type_ok(value: Any, type_name: str) -> bool:
    if type_name == "object":
        return isinstance(value, dict)
    if type_name == "array":
        return isinstance(value, list)
    if type_name == "string":
        return isinstance(value, str)
    if type_name == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if type_name == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    if type_name == "boolean":
        return isinstance(value, bool)
    if type_name == "null":
        return value is None
    raise ValueError(f"unknown type {type_name!r}")


def _resolve_ref(ref: str, root: dict) -> dict:
    assert ref.startswith("#/"), f"only local refs supported, got {ref!r}"
    node = root
    for part in ref[2:].split("/"):
        node = node[part]
    return node


def validate(instance: Any, schema: dict, root: dict | None = None, path: str = "") -> None:
    """Raise ValidationError on the first violation found."""
    if root is None:
        root = schema

    if "$ref" in schema:
        validate(instance, _resolve_ref(schema["$ref"], root), root, path)
        return

    if "allOf" in schema:
        for sub in schema["allOf"]:
            validate(instance, sub, root, path)

    if "anyOf" in schema:
        errors = []
        for sub in schema["anyOf"]:
            try:
                validate(instance, sub, root, path)
                errors = []
                break
            except ValidationError as e:
                errors.append(str(e))
        else:
            if errors:
                raise ValidationError(path, "matched none of anyOf: " + " | ".join(errors))

    if "const" in schema:
        if not _json_eq(instance, schema["const"]):
            raise ValidationError(path, f"expected const {schema['const']!r}, got {instance!r}")

    if "enum" in schema:
        if not any(_json_eq(instance, e) for e in schema["enum"]):
            raise ValidationError(path, f"{instance!r} not in enum {schema['enum']!r}")

    if "type" in schema:
        t = schema["type"]
        types = t if isinstance(t, list) else [t]
        if not any(_type_ok(instance, tn) for tn in types):
            raise ValidationError(path, f"expected type {t!r}, got {type(instance).__name__}")

    if "not" in schema:
        try:
            validate(instance, schema["not"], root, path)
        except ValidationError:
            pass
        else:
            raise ValidationError(path, f"must not match schema {schema['not']!r}")

    if isinstance(instance, str):
        if "pattern" in schema and not re.search(schema["pattern"], instance):
            raise ValidationError(path, f"{instance!r} does not match pattern {schema['pattern']!r}")
        if "minLength" in schema and len(instance) < schema["minLength"]:
            raise ValidationError(path, f"length {len(instance)} < minLength {schema['minLength']}")
        if "maxLength" in schema and len(instance) > schema["maxLength"]:
            raise ValidationError(path, f"length {len(instance)} > maxLength {schema['maxLength']}")

    if isinstance(instance, (int, float)) and not isinstance(instance, bool):
        if "minimum" in schema and instance < schema["minimum"]:
            raise ValidationError(path, f"{instance} < minimum {schema['minimum']}")
        if "maximum" in schema and instance > schema["maximum"]:
            raise ValidationError(path, f"{instance} > maximum {schema['maximum']}")

    if isinstance(instance, list):
        if "minItems" in schema and len(instance) < schema["minItems"]:
            raise ValidationError(path, f"{len(instance)} items < minItems {schema['minItems']}")
        if "maxItems" in schema and len(instance) > schema["maxItems"]:
            raise ValidationError(path, f"{len(instance)} items > maxItems {schema['maxItems']}")
        if "items" in schema:
            for i, item in enumerate(instance):
                validate(item, schema["items"], root, f"{path}[{i}]")

    if isinstance(instance, dict):
        props = schema.get("properties", {})
        for req in schema.get("required", []):
            if req not in instance:
                raise ValidationError(path, f"missing required property {req!r}")
        if schema.get("additionalProperties") is False:
            unknown = set(instance) - set(props)
            if unknown:
                raise ValidationError(path, f"unknown properties {sorted(unknown)!r}")
        for key, sub in props.items():
            if key in instance:
                validate(instance[key], sub, root, f"{path}.{key}" if path else key)


def is_valid(instance: Any, schema: dict, root: dict | None = None) -> tuple[bool, str | None]:
    """root: pass the full schema document when `schema` is a sub-node
    (e.g. schema["definitions"]["foo"]) that may contain a local $ref --
    $refs resolve against `root`, not against `schema` itself."""
    try:
        validate(instance, schema, root)
    except ValidationError as e:
        return False, str(e)
    return True, None
