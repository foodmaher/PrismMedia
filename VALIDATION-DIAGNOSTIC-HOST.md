# Validation status — extensible 4.0.0 source revision

Completed in the authoring workspace:

- Compiled and ran the portable C++17 option parser tests with GCC, warnings
  treated as errors. Invalid, overflowing, duplicate, unknown and boundary
  options are covered.
- Parsed project XML, checked referenced C++ source/header paths, parsed the
  Windows workflow YAML, and confirmed the plugin version remains 4.0.0.
- Reviewed the modified source against the prior revision; media capture,
  audio and the existing game-specific branch implementation are unchanged.
- Added a bounded, console-selectable addon isolation experiment. It can only
  select a high-count candidate whose captured return value was RCX on every
  sample, expires automatically, and leaves unmatched addon calls native.
- Replaced the fixed `DrawIndexed`/slot-6 router with bounded one-hop
  compositor tracing across all seven hooked D3D11 draw variants. Active mode
  retains only the learned draw hook and restores the native SRV immediately.
- Verified source ZIP integrity before delivery.

Added as required checks before the Windows workflow packages its artifact:

- MSVC compilation of the parser test and runtime capture module.
- WARP test of a texture bound before capture, repeated-state aggregation,
  resource filtering, capture budgets/deadlines, JSON output and COM reference
  balance. Hook installation is stubbed in this unit test; actual interception
  is not validated by WARP.
- PowerShell parsing of every test script and a real named-pipe exchange with
  a response larger than one read buffer and an acknowledgement.
- Full x64 Release solution compilation.

Windows SDK compilation, the Windows tests, script execution under the user's
Windows policy, and actual ETS2 interception have NOT been run in this Linux
workspace. The delivered archive is source. Run the included Windows workflow
before installing its binaries. Successful Windows checks still do not prove
the smartphone GPS issue is fixed; use the live A/B script to collect evidence.
