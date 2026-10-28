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

## Review scope and limitations

The review covered tracked source and scripts, including runtime and hardware
agent file/network handling, disc extraction, and build/test tooling. This was
a static review, not a proof that the repository contains no other
vulnerabilities. Generated game code and data were not reviewed as source;
the bundled `ftpd.3dsx` is binary-only and could not receive a source-level
review.

These findings describe existing repository code and are not necessarily
introduced by the current worktree changes.
