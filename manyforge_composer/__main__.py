"""python -m manyforge_composer --catalog-path <path> [--host 0.0.0.0] [--port 9000]"""
from __future__ import annotations

import argparse
import uvicorn
from manyforge_behavior.kernel_client import MockKernelClient
from manyforge_composer.backend.logging_config import setup_logging
from manyforge_composer.backend.state import ComposerState
from manyforge_composer.backend.app import create_app


def main():
    parser = argparse.ArgumentParser(description="ManyForge Composer Backend")
    parser.add_argument("--catalog-path", required=True, help="Path to node_catalog.yaml")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--log-level", default="INFO", choices=["DEBUG", "INFO", "WARNING", "ERROR"])
    args = parser.parse_args()

    setup_logging(args.log_level)

    kernel = MockKernelClient()
    state = ComposerState(catalog_path=args.catalog_path, kernel=kernel)
    app = create_app(state)
    uvicorn.run(app, host=args.host, port=args.port)


if __name__ == "__main__":
    main()
