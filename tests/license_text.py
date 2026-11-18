#!/usr/bin/env python3
"""Assert the first-party Apache-2.0 grant, notice, and package metadata."""
import hashlib
import json
import sys
from pathlib import Path

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[1]
OFFICIAL_LICENSE_SHA256 = "cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30"
COPYRIGHT = "Copyright (c) 2026 iisacc"
SPDX = "SPDX-License-Identifier: Apache-2.0"
PACKAGE_SPDX = 'set(iiVirtualizer_SPDX_LICENSE "Apache-2.0")'


def fail(message):
    raise SystemExit(message)


def read(relative):
    return (ROOT / relative).read_text(encoding="utf-8")


license_bytes = (ROOT / "LICENSE").read_bytes()
if hashlib.sha256(license_bytes).hexdigest() != OFFICIAL_LICENSE_SHA256:
    fail("LICENSE is not the Apache License, Version 2.0")
license_text = license_bytes.decode("utf-8")
for phrase in (
    "Apache License",
    "Version 2.0, January 2004",
    "http://www.apache.org/licenses/",
    "END OF TERMS AND CONDITIONS",
    "APPENDIX: How to apply the Apache License to your work.",
):
    if phrase not in license_text:
        fail("LICENSE is missing the Apache-2.0 text: " + phrase)

notice = read("NOTICE")
if COPYRIGHT not in notice:
    fail("NOTICE is missing the copyright line")
if "Apache License, Version 2.0" not in notice:
    fail("NOTICE does not identify Apache-2.0")

readme = read("README.md")
if "## License" not in readme or SPDX not in readme:
    fail("README License section does not identify Apache-2.0")
if COPYRIGHT not in readme:
    fail("README is missing the copyright notice")

korean = read("docs/License.md")
if SPDX not in korean or COPYRIGHT not in korean:
    fail("docs/License.md does not identify Apache-2.0")

agent = read("AGENT.md")
if "Apache License 2.0" not in agent or COPYRIGHT not in agent:
    fail("AGENT.md does not state the Apache-2.0 grant")

for relative in ("CMakeLists.txt", "cmake/iiVirtualizerConfig.cmake.in"):
    if PACKAGE_SPDX not in read(relative):
        fail(relative + " does not export iiVirtualizer_SPDX_LICENSE")
cmake = read("CMakeLists.txt")
if "install(FILES LICENSE NOTICE DESTINATION ${CMAKE_INSTALL_DATADIR}/iiVirtualizer)" not in cmake:
    fail("CMake does not install LICENSE")

projections = {
    "README.md": "docs/entrypoints/README.agent.json",
    "AGENT.md": "docs/entrypoints/AGENT.agent.json",
    "docs/README.md": "docs/README.agent.json",
    "docs/License.md": "docs/License.agent.json",
}
for source, projection in projections.items():
    data = (ROOT / source).read_bytes()
    document = json.loads(read(projection))
    if document["source"]["sha256"] != hashlib.sha256(data).hexdigest():
        fail(projection + " does not match the current Markdown")
    if document["source"]["path"] != source:
        fail(projection + " records the wrong source path")
    if "".join(block["raw"] for block in document["blocks"]).encode("utf-8") != data:
        fail(projection + " does not round-trip its Markdown source")

index = json.loads(read("docs/agent-index.json"))
indexed = {entry["source"]: entry for entry in index["documents"]}
license_entry = indexed.get("docs/License.md")
if license_entry is None or license_entry["agent_json"] != "docs/License.agent.json":
    fail("agent index is missing docs/License.md")
if license_entry["source_sha256"] != hashlib.sha256((ROOT / "docs/License.md").read_bytes()).hexdigest():
    fail("agent index hash for docs/License.md is stale")
for source, projection in projections.items():
    if indexed[source]["source_sha256"] != hashlib.sha256((ROOT / source).read_bytes()).hexdigest():
        fail("agent index hash is stale for " + source)
    if indexed[source]["agent_json"] != projection:
        fail("agent index projection path is wrong for " + source)

license_document = json.loads(read("docs/License.agent.json"))
if SPDX not in "".join(block["raw"] for block in license_document["blocks"]):
    fail("docs/License.agent.json does not carry the SPDX identifier")

print("Apache-2.0 license text, notice, SPDX metadata, install rule, and agent mirrors passed")
