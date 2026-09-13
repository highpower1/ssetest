# Open issues

The DLSS-NR/RR `commandList->Close()` device loss previously tracked here is
resolved on the user's machine. The debug layer identified a resource-flag
mismatch: a render-target-only guide was used as a UAV. The guide targets and
the direct NGX output now opt into UAV use. See [HANDOFF.md](../HANDOFF.md) for
the failure, fix, and successful-run evidence.

This does not close the separate DLSS-G stability risk documented in
[SAFETY.md](SAFETY.md), or the known feature gaps listed in the handoff.
