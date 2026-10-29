# Contributors

Thank you to everyone who has contributed code, fixes, testing, or a hard-won
debugging insight. This file is the canonical record of who did what; the
CHANGELOG tells the story change by change, but credit lives here. The tools
and published work pcrecomp builds on are credited in the README's
[Credits](README.md#credits).

If you've contributed and aren't listed (or a line is wrong), open a PR against
this file. We want every name right.

---

## Maintainer

### Ned Heller ([@sp00nznet](https://github.com/sp00nznet))
Project creator and maintainer. The disassemblers and lifters (x86-16, -32 and
-64), the PE, NE and Mach-O tooling, the native32 and win32hle runtimes, the DRM
unwrappers, and the game projects that built them.

---

## Contributors

### Chris Pressland ([@cpressland](https://github.com/cpressland))
**native32 under Wine** (#55): found why lifted programs could not take a
callback under Wine. Wine leaves DEP off unless a process asks for it, so the
first fetch from the guest's code made the page executable and ran the original
machine code, silently; and under Rosetta (CrossOver on an Apple Silicon Mac)
Wine reports an instruction fetch as a read. `native32_init` now turns DEP on,
and the fault handler takes a read of the faulting instruction's own address as
a fetch. This is what lets native32 games run on macOS and Linux under Wine; it
was found and proved with Red Alert 2 on a Mac (sp00nznet/redalert2-recomp#1).

### Haukur Rósinkranz ([@hauxir](https://github.com/hauxir))
**win32hle in the browser** (#148): an Emscripten/WebAssembly host for the
win32hle layer, so a lifted game runs in a web page. The guest image sits in
linear memory, DirectSound plays through a shared-memory ring drained by an
AudioWorklet, frames go to the page's WebGL presenter, and Bink movies are
played by the page. Fixes the web port turned up apply to every host: a Bink
stub that reads the real `.bik` header instead of failing (Yuri's Revenge
crashed without it), INI, `_lcreat` and docfile writes routed to the mod
overlay, and kept directory listings for the case-insensitive path match
(on the browser's storage, listing on every miss took minutes). Built and proved
with Red Alert 2 (sp00nznet/redalert2-recomp).

### Daniel Nylander ([@yeager](https://github.com/yeager))
**Malformed input, and free disk space** (#145, #146, #147): `unlzexe.py`
bounds-checks the header, the relocation tables and every back-reference
(before, a bad stream could read bytes from the end of the output through
Python's negative indexing); the 16-bit MZ analyzer validates the header
against the file before using it; and win32hle gained
`GetDiskFreeSpaceExA`, with lexical `..` now stopping at a mapped drive's
root, as it does on Windows. Each came with tests.
