<!--
  THIS FILE IS GENERATED — do not hand-edit. Source of truth: code.
  Regenerate with:
    cd <repository-root>
    python3 scripts/regen-public-contracts.py structural-rest
  Last regenerated: 2026-08-09 09:44:40 UTC
  Generator source: scripts/regen-public-contracts.py:gen_rest_api
-->

# Composer REST API

This is the structural and authoring-only Composer surface retained during the composable-cell runtime cutover. Catalog reads, sandboxed artifact browsing, structural URDF assets, health, and the strict revision-fenced Program Draft/Saved workflow remain available. Program Apply and execution remain unavailable. The catch-all `/api/{unavailable_path}` operations return the typed runtime-unavailable response for former product routes.

The live schema is available at `/openapi.json` and `/docs` while the API process is running.

OpenAPI version: `3.1.0`. Title: `ManyForge Composer (replaceable client)`.

## Routes

| Method | Path | Tag | Summary |
|---|---|---|---|
| `GET` | `/api/catalog` | catalog | Get Catalog |
| `GET` | `/api/catalog/{entry_id}` | catalog | Get Catalog Entry |
| `GET` | `/api/files/browse` | files | Browse Filesystem |
| `GET` | `/api/health` |  | Health |
| `GET` | `/api/nodes/environment` | node-environment | Get Node Environment |
| `GET` | `/api/program/draft` | composer-draft | Read Draft |
| `POST` | `/api/program/draft/mutations` | composer-draft | Mutate Draft |
| `POST` | `/api/program/open-draft` | composer-draft | Open Draft |
| `POST` | `/api/program/save` | composer-draft | Save Draft |
| `POST` | `/api/program/save-as` | composer-draft | Save Draft As |
| `GET` | `/api/program/workspace` | composer-draft | Read Workspace |
| `GET` | `/api/scene/urdf/manifest` | structural-urdf | Get Manifest |
| `GET` | `/api/scene/urdf/models/{model_key}/collision-meshes/{link_name}/{collision_index}` | structural-urdf | Get Model Collision Mesh |
| `GET` | `/api/scene/urdf/models/{model_key}/meshes/{link_name}/{visual_index}` | structural-urdf | Get Model Mesh |
| `DELETE` | `/api/{unavailable_path}` |  | Unavailable Api |
| `GET` | `/api/{unavailable_path}` |  | Unavailable Api |
| `PATCH` | `/api/{unavailable_path}` |  | Unavailable Api |
| `POST` | `/api/{unavailable_path}` |  | Unavailable Api |
| `PUT` | `/api/{unavailable_path}` |  | Unavailable Api |
