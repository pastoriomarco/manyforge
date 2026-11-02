# ManyForge content identity

This ROS-free lower library owns ManyForge's single RFC 8785 value codec,
exact lowercase SHA-256 rendering, and spec-210 domain/API framing primitive.

It is deliberately not a schema validator or semantic identity owner. Callers
must first validate a closed DTO/schema, normalize every collection, enforce
safe integer and text rules, materialize required defaults, and omit only the
field selected by their own normative contract. Deployment's schema codecs
and the logical-scene semantic owner built on Core DTOs remain responsible
for those decisions.

The library exists so Core, deployment verification, and future owner packages
do not carry independent canonical-JSON or hashing implementations.
