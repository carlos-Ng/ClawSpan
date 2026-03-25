#!/usr/bin/env python3
from __future__ import annotations

import os
import sys

import mcp_server


def main() -> int:
	transport = os.environ.get("CLAWSPAN_CHANNEL3_TRANSPORT", "legacy")
	argv = [
		"--channel3-transport",
		transport,
		*sys.argv[1:],
	]
	return mcp_server.main(argv)


if __name__ == "__main__":
	raise SystemExit(main())
