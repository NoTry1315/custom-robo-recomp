# Security review

Review date: 2026-10-28

This report records confirmed security findings from a repository-wide static
review. Severity describes the impact under the stated prerequisites; it does
not imply that exploitation has been observed. The review did not execute
malicious inputs or perform network testing.

## Findings

### 1. Unauthenticated hardware-agent access to SD files and homebrew execution

- **Severity:** High
- **Confidence:** 9/10
- **Category:** Broken access control
- **Location:** `tools/hw/agent/source/main.c:128-134, 138-179, 212-229, 236-263, 282-301`
- **Prerequisite:** The hardware agent is running and reachable by the attacker
  over the network.

The agent binds its listener to all network interfaces and dispatches accepted
clients' commands without authentication. A client can use `GET` to read SD
paths, `PUT` to write most SD paths, and `RUN` to launch a caller-selected
existing `.3dsx` through `hb:ldr`. The save-path restriction does not prevent
reading other paths or launching programs. This can expose SD data, persist
modified files, and execute uploaded homebrew with the privileges of the
running application.

**Recommended remediation:** Disable the listener by default and require
explicit opt-in. Authenticate and authorize each command, protect the
connection, restrict file operations to a narrow allowlist, and remove
arbitrary `RUN` or require a separate trusted physical confirmation.

**Status:** Mitigated in the shared repository, but not fixed for existing
local copies or deployed binaries. The vulnerable agent source
(`tools/hw/agent/source/main.c`) and client (`tools/hw_loop.py`) were removed
from the tracked tree, and `.gitignore` now prevents those maintainer-local
files from being accidentally added again. The unattended-run documentation
was replaced with a warning and manual-test instructions. A fresh checkout
therefore cannot build or run this service. Existing local copies and SD-card
binaries still use the unauthenticated protocol and remain vulnerable; do not
run them on an untrusted network. The unattended workflow remains unavailable
until a secure replacement is designed.

#### Remediation plan

1. Remove the agent source and client from the shared repository, ignore the
   local copies, and document the limitation. **Done; this contains the issue
   in shared checkouts but does not secure existing copies.**
2. Replace the unauthenticated protocol before using an agent on a network.
   **Pending; authentication, protected transport, least-privilege file
   access, and removal or trusted confirmation for arbitrary `RUN` are still
   required.**

### 2. Disc extractor permits Windows path traversal from FST names

- **Severity:** Medium
- **Confidence:** 9/10
- **Category:** Path traversal
- **Location:** `tools/extract_disc.py:12-27, 59-63`
- **Prerequisite:** The victim runs the extractor on a crafted ISO on Windows.

The extractor uses names from the ISO filesystem table (FST) to construct
output paths. Its checks reject `/` and the exact names `.` and `..`, but do
not reject backslashes. On Windows, a FST name containing `..\..\` can
traverse outside the selected extraction directory. The subsequent write can
overwrite files that the current user is permitted to modify.

**Recommended remediation:** Reject Windows path separators, rooted paths,
and drive-qualified names. Resolve each destination and verify it remains
within the extraction root before creating or writing the file.

**Status:** Fixed and regression-tested. `walk_fst` rejects path separators,
empty/dot components, and Windows rooted or drive-qualified FST names.
`output_path` rejects rooted, drive-qualified, and traversal paths, resolves
the destination, and verifies it remains under the resolved extraction root
before directory creation or file writes. This containment check also blocks
symlink escapes. Focused tests cover crafted Windows names, valid nested
paths, traversal, and symlink escapes; `python -m unittest discover -s
tools/tests -p test_extract_disc.py` passed all four tests.

#### Remediation plan

1. Validate each FST name as a single safe path component. **Done.**
2. Resolve destinations and enforce extraction-root containment before writes.
   **Done.**
3. Add focused traversal and valid-path tests. **Done; run in this branch.**

## Review scope and limitations

The review covered tracked source and scripts, including runtime and hardware
agent file/network handling, disc extraction, and build/test tooling. This was
a static review, not a proof that the repository contains no other
vulnerabilities. Generated game code and data were not reviewed as source;
the bundled `ftpd.3dsx` is binary-only and could not receive a source-level
review.

Finding 1 remains applicable to maintainer-local copies and previously built
agent binaries, which are outside the shared repository. Finding 2 describes
the pre-fix extractor; the current tracked extractor includes the remediation
described above.
