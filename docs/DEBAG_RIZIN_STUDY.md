# Debag Rizin Study

## Ported

The top recommendations from this study have been implemented in the
2O9 `--static-db` REPL. Each entry below lists the recommendation, the
commit that shipped it, and any deviations from the original plan.
Recommendations 3 and 6+ remain future work.

| # | Recommendation | Commit | Notes |
|---|---|---|---|
| 1 | PLT/GOT relocation parsing + import address resolution | `debag: parse PLT/GOT relocations for imports in static-db` | Parses `DT_JMPREL`/`DT_REL`/`DT_RELA` from `.dynamic` and walks each reloc table; `ii` now prints `idx got_addr type name` (e.g. `R_X86_64_JUMP_SLOT  printf`). `s <import_name>` resolves to the GOT slot via the reloc table. PLT calls in `pd`/`pdd` are annotated with `; -> 0xGOT (name)` by reading the PLT entry's `jmp [rip+disp32]` and matching the resulting GOT slot to a reloc. ~170 LOC across `static_analysis.c` (reloc parsing) and `static_db.c` (`ii`, `resolve_expr`, PLT annotation). Clean-room reimplementation of `elf_relocs.c:184-240` + `elf_imports.c:400-418`; no rizin code copied. |
| 3 | Seek history `u`/`U` | `debag: add seek history (u/U/sh) to static-db REPL` | 32-entry history stack + 32-entry redo stack on `repl_t`. `s <addr>` pushes prior offset (no-op seeks don't touch history) and clears redo. `u` pops history -> restores, `U` pops redo -> restores, `sh` prints the stack. Added `sh` for printing the stack (rizin's `s-` equivalent). ~90 LOC in `static_db.c`. Clean-room reimplementation of `cmd_seek.c:154-176` + `rz_core_seek_undo` semantics. |

### Items not yet ported

2. Jump arrows + call-target annotation in `pd` (PLT call annotation
   shipped as part of item 1; full reflines are not implemented).
4. Sparse-mode hex dump (three-row `...` collapse).
5. `pxr` pointer-chase dump.
6. UTF-16LE string detection.
7. `izz` whole-binary scan.
8. `axt <addr>` xref-to.
9. Basic `af` walk-to-RET.
10. `$$`/`$s` tokens.

---

A surgical study of the rizin source tree (cloned at commit `ff4d660`,
master branch, `https://github.com/rizinorg/rizin`) with the goal of
telling the 2O9 `--static-db` REPL what to steal and what to leave
alone. Each section cites specific rizin files and line numbers and
ends with a concrete recommendation. The 2O9 code under review is
`src/debag/static_db.c` (787 lines), `src/debag/static_analysis.c`
(658 lines), and `src/debag/debag.h` (185 lines) at HEAD `6fc4231`.

## What 2O9's --static-db has today

`debag_static_db_repl()` in `src/debag/static_db.c` is a small, working
ELF REPL: longest-prefix command dispatch over a static table of 16
commands (`i`, `iS`, `iSS`, `is`, `ie`, `iz`, `ii`, `px`, `pxw`,
`pxq`, `ps`, `s`, `pd`, `pdd`, `?`, `q`), a single `uint64_t offset`
cursor, `pread()` straight off the binary's fd for every read, no IO
layer, no block cache, no expression evaluator, no analysis. ELF data
comes pre-parsed in `debag_analysis_t` (sections, segments, symbols,
entry point, arch, bits, endianness) from `debag_analyze()` in
`static_analysis.c`. Disassembly is gated on `HAVE_CAPSTONE`; without
it `pd`/`pdd` print a hint and return. Hex dump mirrors rizin's
`rz_print_hexdump_str` byte layout (`0x%016llx` address + 16 bytes
grouped in pairs + ASCII column). Symbol resolution order is
entry0 → section name → symbol name → hex/dec number, mirroring rizin's
`num_callback`. The whole thing is ~800 LOC in one file. It works.

What it does not do: dynamic relocations, PLT/GOT resolution, version
info, build-id, DWARF, jump arrows, call-target annotation, string
xrefs, UTF-16 strings, sparse-mode hex dumps, comments column, seek
history, `izz` (whole-binary string scan), `pxr` (resolve pointer
dereferences), `ii` showing import GOT addresses, `is` filtering by
section, function detection, or any analysis at all. Everything below
is about closing those gaps without taking on rizin's full complexity.

## What rizin does better, with file:line citations

### 1. ELF parsing

2O9's `static_analysis.c:412-471` parses `.dynsym` and `.symtab` and
calls it a day. The `.dynamic` section is read only for `DT_NEEDED`
library names (`static_analysis.c:541-583`); DT_REL, DT_RELA, DT_JMPREL
are never read. This means 2O9's `ii` lists import *names* but has no
idea which GOT/PLT slot each import lands in, and `is` cannot show the
resolved PLT thunks. Rizin does four things 2O9 doesn't:

**(a) Parse the dynamic section into a hashtable keyed by d_tag.**
`librz/bin/format/elf/elf_dynamic.c:67-118` (`init_dt_dynamic`) walks
`PT_DYNAMIC`, reads each `Elf_Dyn` entry (`get_dynamic_entry` at
line 22), and inserts into an `HtUU` keyed by `d_tag`. `DT_NEEDED`
entries go into a separate `RzVector` because there can be many.
2O9's `static_analysis.c:541-583` re-scans the dynamic array for each
field it wants; rizin's one-pass approach is simpler and lets every
later lookup be `O(1)` via `Elf_(rz_bin_elf_get_dt_info)(bin, key, &out)`
(`elf_dynamic.c:120-128`).

**(b) Read relocations from both DT_DYNAMIC and SHT_REL/SHT_RELA
sections.** `librz/bin/format/elf/elf_relocs.c:184-216`
(`get_relocs_entry_from_dt_dynamic`) reads three relocation sources:
`DT_JMPREL`/`DT_PLTRELSZ` (PLT relocations, mode set by `DT_PLTREL`),
`DT_REL`/`DT_RELSZ`, and `DT_RELA`/`DT_RELASZ`. Then
`elf_relocs.c:222-240` (`get_relocs_entry_from_sections`) walks
`SHT_REL` and `SHT_RELA` sections to catch relocations not declared in
`DT_DYNAMIC` (common in `.o` files and certain stripped binaries). Each
relocation's `r_info` is split via `ELF_R_SYM`/`ELF_R_TYPE`
(`elf_relocs.c:125-126`) and the symbol index is used to look up the
target. Rizin dedups via an `HtUU` set keyed on file offset
(`elf_relocs.c:135-164`).

**(c) Resolve import addresses by walking relocations.**
`librz/bin/format/elf/elf_imports.c:400-418` (`get_import_addr`) walks
all relocations looking for `reloc->sym == symbol_ordinal`; the matching
reloc's `r_offset` (after arch-specific fixups in `get_import_addr_aux`
at line 362) is the import's GOT slot address. This is how rizin's `ii`
can show `printf` at `0x404018` rather than just "printf, undefined".
2O9's `static_analysis.c:443` marks imports with `is_import=1` but
stores `vaddr=0`, so the REPL can never resolve `s printf` to a useful
address — `resolve_expr` in `static_db.c:187-194` skips symbols with
`vaddr == 0`.

**(d) Symbol version info (`DT_GNU_VERSION`, `.gnu.version_r`,
`.gnu.version_d`).** `librz/bin/format/elf/elf_info.c:1594-1700` parses
`Elf_Verdef`, `Elf_Verneed`, `Elf_Versym` records. With it, rizin's
symbols list can show `printf@GLIBC_2.2.5` rather than bare `printf`.
2O9 has the section-type names (`static_analysis.c:188-196`) but never
parses the records.

**Concrete recommendation.** Add a `debag_elf_reloc_t` array to
`debag_analysis_t` (fields: `vaddr`, `sym_idx`, `type`, `addend`,
`is_plt`). In `debag_analyze()`, after the existing `.dynsym` walk,
parse `PT_DYNAMIC` once into a tag→value map (mirror
`elf_dynamic.c:67-118` — about 30 lines of C), then walk `DT_JMPREL`,
`DT_REL`, `DT_RELA` and the `SHT_REL`/`SHT_RELA` sections to fill the
reloc array (mirror `elf_relocs.c:184-240` — about 60 lines). In
`cmd_info_imports` (`static_db.c:357-370`), for each undefined symbol,
scan the reloc array for `reloc.sym == i` and print
`name  got_addr=0x404018  type=R_X86_64_JUMP_SLOT`. In
`resolve_expr` (`static_db.c:187-194`), if a symbol has `vaddr==0` but
a reloc with matching index exists, return the reloc's `r_offset` as
the address — that makes `s printf` seek to the GOT slot, just like
rizin. Skip version info for now; it's polish, not leverage. Skip
DWARF/PDB entirely (rizin has 18 files under `librz/bin/dwarf/` — way
out of scope).

### 2. Hex dump

2O9's `print_hexdump` (`static_db.c:230-282`) is a faithful copy of
rizin's basic layout but is missing four features that materially
affect readability: sparse-mode collapse, configurable row width, the
comment column, and `pxr` (pointer-chasing).

**Sparse mode.** `librz/util/print.c:763-782` (`checkSparse` loop in
`rz_print_hexdump_str`): if three consecutive rows are identical, the
middle ones are replaced with a single `...\n` line. This is the
difference between dumping `.bss`-adjacent pages and seeing 4000 lines
of `0x00 00 00 00 ...` vs. 3 lines + `...`. Gated on the
`RZ_PRINT_FLAGS_SPARSE` flag. Trivial to port: keep a `last_row[16]`
buffer and a `last_sparse` counter in `print_hexdump`; emit `...\n`
when `memcmp(buf+i, buf+i+16, 16) == 0 && memcmp(buf+i, buf+i+32, 16)
== 0`.

**Configurable row width.** Rizin's `p->cols` (set by
`hex.cols` config, default 16) drives the row width; 2O9 hardcodes 16
at `static_db.c:238,263`. Not strictly necessary, but a `set cols 8`
would be one line.

**Comment column.** `librz/util/print.c:725-733` reserves a `comment`
header in the hex dump, populated by per-address metadata from
`RzAnalysisMetaItem`. 2O9 has no analysis metadata, but it could show
section-name annotations ("`.text`", "`.rodata`") at section
boundaries and string-preview annotations ("`string: "/bin/sh"\n"`")
where `iz` found a string at that offset. This is the single biggest
visual win for `px` readability on real binaries.

**`pxr` — pointer-chase the hex dump.** `librz/util/print.c:749,
863-883` (`isPxr` path): in `pxr` mode, each 8-byte (or 4-byte) word
is treated as a pointer; if it points at a known symbol, the symbol
name is printed alongside the value, and zero-valued words are
suppressed. This is *enormously* useful for dumping `.got.plt`,
`.data.rel.ro`, and vtables — you see not just the bytes but what they
point at. Implementation: for each word, look it up in the symbol
table (linear scan is fine; 2O9's symbol_count is typically < 2000)
and append ` -> sym.main` if it hits. About 30 lines.

**Concrete recommendation.** In `print_hexdump` (`static_db.c:230`),
add a `sparse` flag (always on for now) and the three-row collapse
check. Add a new `cmd_print_hex_refs` command (`pxr`) that reuses
`do_print_hex` with `word_size=8` and, after printing each word,
appends ` -> name` when the value matches a known symbol vaddr. Add a
`pc` (print comments) variant of `px` that, for each row address,
checks if it falls in a named section and prepends `[.rodata]` etc.
Skip the `pxe` emoji mode, `pxb` bit mode, `pxd` signed-integer mode,
`pxA` op-analysis color map — all rizin chrome.

### 3. Disassembly

2O9's `do_disasm` (`static_db.c:582-630`) is a tight Capstone loop:
read `count*16+64` bytes, `cs_disasm` once with a count limit, print
`0xADDR  hex  mnemonic operands` per instruction. It is correct but
bare. Rizin's full `pd` (`rz_core_print_disasm` at
`librz/core/disasm.c:5332`, ~1500 lines) is a beast — flag
annotations, meta items, basic-block boundaries, color, ESIL,
comments, xrefs, jump arrows, function labels — and most of it is not
worth porting. But three things are:

**(a) Call/jump target annotation.** When `pd` hits a `call 0x401020`
or `jne 0x4012a0`, rizin resolves the target to a symbol name and
appends it in a comment. `librz/core/disasm.c` does this via
`ds_print_comments_right` (line 5066) which calls
`rz_core_get_xref_comment` (line 1411). 2O9 doesn't need xrefs — just
a symbol lookup. With Capstone in `CS_OPT_DETAIL` mode, the
instruction's operands are exposed; for x86 a `call`/`jmp` with
immediate operand gives the target directly. Without detail mode (the
current 2O9 setup, faster), parse the op_str for `0x[0-9a-f]+` tokens
and look each up in the symbol table. About 25 lines.

**(b) Jump arrows (reflines).** `librz/arch/reflines.c:88-285`
(`rz_analysis_reflines_get`): during a pre-pass over the disasm window,
identify every JMP/CJMP/CALL with an in-window target, store
`(from, to)` pairs, sort, allocate "levels" by greedy lowest-free
slot, then render the arrows in `rz_analysis_reflines_str` (line 367)
as `:==`/`|`/`->` characters. The full rizin implementation is ~200
lines and handles wide chars, mid-bb corners, both-direction jumps.
For 2O9 a stripped-down version is enough: pre-pass with Capstone in
detail mode, collect `(from, to, is_call)` triples where `to` is
within the current disasm window, render a `<= 8`-column-wide ASCII
arrow column left of the mnemonic. About 80 lines.

**(c) Basic-block boundaries.** Rizin draws a blank line or `;-- `
separator at the start of each basic block (gated on `asm.bb.middle`,
see `disasm.c:6295-6296`). 2O9 doesn't have BB analysis, but a
*heuristic* version is cheap: any `JMP`/`RET`/`HLT` instruction ends a
block; any instruction that is the target of a `JMP`/`CJMP`/`CALL`
starts one. A single pass over the disasm window can mark these and
the renderer can insert a blank line. About 20 lines on top of (b).

**Concrete recommendation.** In `do_disasm`, after `cs_disasm`,
iterate the resulting `cs_insn` array twice. First pass: for each
instruction, extract its jump/call target (parse `op_str` for `0x...`,
or enable `CS_OPT_DETAIL` and read `detail->x86.operands[0].imm` for
`X86_OP_IMM` operands on `call`/`jmp`/`jcc` mnemonics); build a
hashtable of in-window target addresses. Second pass: print each
instruction with an optional arrow column ("  -> " if this insn is a
jump target, "====" if jumping forward within window, "  <= " if a
back-edge), the existing hex/mnemonic columns, and a trailing
"`  ; sym.name`" comment if the target resolves to a symbol. Skip
ESIL, color, full function analysis, mid-bb flag handling — all
rizin-specific complexity with marginal value for a read-only ELF
REPL.

### 4. Strings

2O9's `cmd_info_strings` (`static_db.c:382-422`) walks `SHF_ALLOC` &&
`!SHF_EXECINSTR` sections, reads each into a malloc'd buffer, scans
for printable-ASCII runs `>= 4` chars, prints `vaddr  length  "str"`.
That is the 1990s `strings(1)` algorithm. Rizin does substantially
more in `librz/bin/bfile_string.c` and `librz/util/str_search.c`:

**(a) Multi-encoding support.** `librz/util/str_search.c:303-342`
(`process_one_string`) dispatches on `RzStrEnc`: `RZ_STRING_ENC_8BIT`
(plain ASCII), `RZ_STRING_ENC_UTF8` (validated UTF-8 with multi-byte
decoding via `rz_utf8_decode`), `RZ_STRING_ENC_UTF16LE`/`UTF16BE`
(`rz_utf16le_decode`), `RZ_STRING_ENC_UTF32LE`/`UTF32BE`,
`RZ_STRING_ENC_EBCDIC_*`, `RZ_STRING_ENC_IBM037`. The detected
encoding is recorded per-string in `RzBinString.type`
(`librz/include/rz_bin.h:820`). 2O9 is 8-bit-only.

**(b) BOM detection.** `librz/util/str_search.c:185-228`
(`adjust_offset`): if the bytes immediately before a detected UTF-16
string are `\xff\xfe`, or before a UTF-32 string are `\xff\xfe\x00\x00`,
the string's reported start address is backed up to include the BOM.
Sensible.

**(c) `iz` vs `izz` distinction.** `librz/bin/bfile_string.c:471-564`
(`gen_intervals`): in `RZ_BIN_STRING_SEARCH_MODE_AUTO` (the default for
`iz`), only `is_data_section` sections are scanned
(`bfile_string.c:50-58` — `section->has_strings || section->is_data`).
`izz` (whole-binary scan) splits the file into `bf->size / pool_size`
chunks and scans everything. 2O9's `iz` is closer to rizin's `izz` —
it scans all `SHF_ALLOC && !SHF_EXECINSTR` sections including `.text`,
which rizin's `iz` doesn't.

**(d) `izx` — strings with xrefs.**
`librz/core/cmd/cmd_info.c:752-760` (`rz_cmd_info_xrefs_strings_handler`)
filters the strings list to only those with at least one xref. This
requires the analysis layer; 2O9 doesn't have one. But a poor-man's
version — scan `.text` for 4-byte / 8-byte little-endian values that
point into the string list — would catch direct `lea reg, [rip+str]`
references without a full xref database.

**(e) False-positive reduction.** `librz/util/str_search.c:389-398`
calls `reduce_false_positives` which checks ASCII frequency
(`check_ascii_freq` flag in `RzUtilStrScanOptions`, line 236) — a
string of all-uppercase consonants is likely junk. 2O9 just checks
`isprint(c)`.

**Concrete recommendation.** Two changes, both small. (1) Add
UTF-16LE detection in `cmd_info_strings`: alongside the printable-ASCII
run, scan for runs of ` printable 00 printable 00 ... ` with length
`>= 4` (i.e. 8 bytes). Print these as `vaddr  length  L"str"`. This
catches Windows-PE strings embedded in Linux binaries and the
wide-character constants some Rust binaries use. About 30 lines.
(2) Add `izz` (whole-binary scan, including non-ALLOC sections like
`.comment`, `.note.gnu.build-id`) as a separate command. The current
`iz` already excludes `SHT_NOBITS` and `SHF_EXECINSTR`; `izz` would
just drop the `SHF_ALLOC` filter. One-line change in
`static_db.c:389`. Skip UTF-32, EBCDIC, IBM037, false-positive
reduction, and `izx` (needs analysis layer).

### 5. REPL infrastructure

2O9's `dispatch_line` (`static_db.c:687-743`) is a longest-prefix
matcher over a flat `cmds[]` table. It is correct and small. Rizin's
equivalent (`librz/core/cmd/cmd_api.c:342-378`, `cmd_get_desc_best`)
chops one character at a time off the end of the input string and
hash-table-looks-up each suffix — same algorithm, different data
structure. Three things rizin does that 2O9 should consider:

**(a) Char-chop dispatch with a hashtable.** Rizin's `ht_sp_find` on
each prefix is `O(1)` per chop; 2O9's linear scan over the table is
`O(N*M)` per dispatch. With 16 commands the difference is invisible.
But the char-chop trick has one real advantage: it lets the user type
just `i` and get a sub-menu (`librz/core/cmd_descs/cmd_info.yaml:1-7`
shows `i` as a `RZ_CMD_DESC_TYPE_GROUP` whose subcommands are `iS`,
`is`, `ie`, etc.). 2O9's current matcher already does this implicitly
because `iS` is longer than `i` and `strncmp("iS", "iS", 2) == 0`
wins over `strncmp("i", "iS", 1) == 0` (longest-match). No change
needed for correctness; the only reason to switch to char-chop would
be if 2O9 grew to >100 commands.

**(b) Seek history (`u`/`U` undo/redo).** `librz/core/seek.c:116-131`
just sets `core->offset`; the history tracking lives in
`rz_core_seek_and_save` / `rz_core_seek_undo` (called from
`librz/core/cmd/cmd_seek.c:154-176`'s `rz_seek_handler`). For 2O9
this is a 32-entry ring buffer of `(uint64_t offset, char *hint)` and
two commands (`u` to undo, `U` to redo). About 50 lines, high
usability payoff — every rizin user uses `u` constantly.

**(c) `$`-variables and `@@`-iterator.** `librz/core/core.c:593-692`
(`num_callback`) resolves `$`-prefixed tokens (`$$` = current seek,
`$s` = file size, `$SS` = section start) and `@@` is a per-iteration
seek. This is rizin's secret sauce for one-liners like
`/x 90 @@ .text`. For 2O9 this is overkill — the REPL is read-only
and there's no `/` search command yet. Defer.

**Concrete recommendation.** Keep the current linear-scan dispatcher.
Add `u`/`U` seek history (50 lines: ring buffer + two handlers).
Optionally add `$$` (= current seek) and `$s` (= file size) as
special tokens in `resolve_expr` — 5 lines, pays off in `px $$+0x10
16`-style commands. Do not port the YAML-driven command descriptor
system (`librz/core/cmd_descs/cmd_descs.yaml`, ~2000 lines of
generated C) — it exists because rizin has 600+ commands; 2O9 has 16.

### 6. Analysis

2O9 has no analysis layer. No functions, no basic blocks, no xrefs, no
call graph. Rizin's `librz/anal/` (now under `librz/arch/`) is
thousands of files; the entry points are `rz_analysis_op`
(`librz/arch/op.c:108`) for single-instruction analysis and
`rz_analysis_fcn` (`librz/arch/fcn.c:1672`) for recursive function
discovery. Full analysis is way out of scope. But three bits are
cheap and high-leverage:

**(a) Single-instruction operand analysis.**
`librz/arch/op.c:108-154` (`rz_analysis_op`) takes a buffer and
returns an `RzAnalysisOp` with `.type` (`RZ_ANALYSIS_OP_TYPE_CALL`,
`_JMP`, `_CJMP`, `_RET`, etc.), `.jump` (target for direct
jumps/calls), `.fail` (fall-through for conditional jumps),
`.ptr` (resolved memory operand for `mov rax, [rip+0x...]`).
2O9 gets all of this from Capstone's `CS_OPT_DETAIL` mode — `cs_insn`
already has `detail->x86.operands[]` with type, reg, imm, and mem
fields. No separate analysis layer needed. This is the data that
powers recommendation 3(a) (call-target annotation) and 3(b) (jump
arrows).

**(b) Function detection at entry.** Rizin's `rz_analysis_fcn`
(`librz/arch/fcn.c:1672-1746`) does a recursive-descent walk from a
function entry, following calls and conditional jumps, building basic
blocks, stopping at `RET`/`HLT`/`UNIMP`. A full port is ~500 lines.
But the *minimum viable* version — "starting at entry0, walk forward
instruction-by-instruction until I hit a `RET`, mark that range as
`function entry0`" — is 20 lines on top of Capstone and gives `pd`
the ability to print `;-- entry0:` at the start.

**(c) Xref storage.** `librz/arch/xrefs.c:108-205`:
`rz_analysis_xrefs_set(analysis, from, to, type)` records a reference;
`rz_analysis_xrefs_get_to(analysis, addr)` (line 164) returns all
`from`s pointing at `addr`. For 2O9 this is a flat `RzList` of
`(from, to, type)` triples, populated lazily by a `axt <addr>` command
that scans `.text` for 4-byte LE values equal to `addr` and reports
each hit. About 60 lines. Useful for `axt sym.printf` to find every
call site.

**Concrete recommendation.** Enable `CS_OPT_DETAIL` in `do_disasm`
(`static_db.c:597-600`) — one `cs_option(handle, CS_OPT_DETAIL,
CS_OPT_ON)` call. Add `axt <addr>` (xref-to) that linear-scans
`.text` for 4-byte LE matches. Add `af` (analyze function) that walks
forward from current seek until a `RET`, sets an in-memory
`function_start`/`function_end` pair, and lets `pd` print `;-- name:`
at the function start. Skip ESIL, full recursive-descent analysis,
call-graph construction, type inference, BB-level analysis, RzIL —
all multi-thousand-line investments.

## Recommended port priority

Ranked by leverage-per-LOC. Items 1-4 are the highest-leverage and
should land first; items 5-8 are nice-to-have; items 9-10 are
stretch goals.

1. **PLT/GOT relocation parsing + import address resolution.**
   Parse `PT_DYNAMIC` once into a tag→value map, walk `DT_JMPREL` /
   `DT_REL` / `DT_RELA` into a reloc array, look up import GOT slots
   in `ii` and `resolve_expr`. Mirrors
   `librz/bin/format/elf/elf_relocs.c:184-240` and
   `librz/bin/format/elf/elf_imports.c:400-418`. ~100 LOC, transforms
   `ii` from "list of names" into "list of names with GOT addresses"
   and makes `s printf` actually work.

2. **Jump arrows + call-target annotation in `pd`.**
   Enable `CS_OPT_DETAIL`, do a two-pass print in `do_disasm` with an
   ASCII arrow column and a `; sym.name` comment on resolved targets.
   Mirrors `librz/arch/reflines.c:88-285` (simplified) and the
   comment logic in `librz/core/disasm.c:5066`. ~80 LOC, makes `pd`
   output actually navigable.

3. **Seek history (`u`/`U`).**
   32-entry ring buffer in `repl_t`, two new commands. Mirrors
   `librz/core/cmd/cmd_seek.c:154-176` + `rz_core_seek_and_save` /
   `rz_core_seek_undo`. ~50 LOC, huge usability win.

4. **Sparse-mode hex dump.**
   Three-row collapse check in `print_hexdump`. Mirrors
   `librz/util/print.c:763-782`. ~15 LOC, makes `px` on `.bss` and
   large `.rodata` actually readable.

5. **`pxr` pointer-chase dump.**
   New command, reuses word-mode hex layout, looks each 8-byte value
   up in the symbol table and appends ` -> name`. Mirrors the `isPxr`
   path in `librz/util/print.c:749, 863-883`. ~30 LOC, great for
   dumping `.got.plt` and `.data.rel.ro`.

6. **UTF-16LE string detection in `iz`.**
   Alongside the ASCII run, scan for `c 00 c 00 ...` patterns.
   Mirrors a tiny slice of `librz/util/str_search.c:303-342`. ~30
   LOC, catches wide-char strings in mixed-encoding binaries.

7. **`izz` (whole-binary string scan).**
   Drop the `SHF_ALLOC` filter from `cmd_info_strings`. One-line
   change in `static_db.c:389`, plus a new command-table entry. Catches
   strings in `.comment`, `.note.*`, overlay.

8. **`axt <addr>` (xref-to).**
   Linear-scan `.text` for 4-byte LE values matching `addr`. Mirrors
   `librz/arch/xrefs.c:164` (read-only variant). ~60 LOC, useful for
   finding callers of a function.

9. **Basic function detection (`af`).**
   Walk forward from current seek until a `RET`, mark range, print
   `;-- entry0:` label in `pd`. Mirrors a stripped-down
   `rz_analysis_fcn` from `librz/arch/fcn.c:1672`. ~20 LOC, gives
   `pd` function-level framing.

10. **`$$` and `$s` tokens in `resolve_expr`.**
    `$s` = file size from `fstat`. ~5 LOC, minor convenience.

## What NOT to port

Deliberate omissions with reasons.

- **YAML command descriptors** (`librz/core/cmd_descs/*.yaml` +
  `cmd_descs_generate.py`, ~2000 generated LOC). Rizin autogenerates
  dispatch for 600+ commands; 2O9 has 16 hand-written entries. Pure
  overhead.

- **RzConfig eval system** (`librz/config/`, `librz/core/cconfig.c`).
  ~500 config vars (`asm.bytes`, `hex.cols`, `scr.color`, ...) drive
  every print routine. 2O9 hardcodes defaults; no user base is asking
  for `e asm.lines.call=false` toggles.

- **RzIO layer** (`librz/io/`). Abstracts reads through `RzBuffer` /
  `RzIODesc` so the same code works on files, memory, gdb-remote,
  archives. 2O9 calls `pread(fd, ...)`; correct for a single-file
  read-only REPL.

- **RzShell parser** (`librz/core/cmd/rz-shell-parser-cmds.inc`).
  Tree-sitter-generated; handles pipes, `;`, `@@`, `>`, `~grep`,
  `$.script`. 2O9's `strtok(s, " \t")` is the right tool at this
  scale.

- **Full function recovery** (`librz/arch/fcn.c`, `block.c`,
  `jmptbl.c`). Thousands of lines for jump-table reconstruction,
  switch analysis, BB reachability. The recommendation-#9 minimum
  `af` (walk-to-RET) gives 80% of the practical value at 1% of the
  code.

- **ESIL / RzIL emulation** (`librz/arch/esil/`, `librz/il/`).
  Stack-based and SSA-based IRs for taint analysis, deobfuscation,
  constraint solving. A read-only REPL doesn't need this.

- **DWARF / PDB debug-info** (`librz/bin/dwarf/` 18 files,
  `librz/bin/pdb/` 12 files). DWARF line tables, var locations, type
  info, PDB streams for Windows binaries. 2O9 targets Linux ELF; if
  stripped, no DWARF helps, and if not, `.symtab` (already walked in
  `static_analysis.c:455-471`) gets the symbols.

- **Visual modes (`V`, `VV`, `Vv`)** — 17k LOC of curses TUI across
  `librz/core/tui/visual.c` (3931 lines), `panels.c` (7085),
  `agraph.c` (6026). 2O9's REPL is intentionally a line-oriented
  prompt; users who want visual mode should install rizin.

- **Symbol demangling** (`librz/demangler/`). ~5000 LOC of C++/Rust/
  Swift/Java demanglers. 2O9's symbols come out of `.dynsym` raw;
  pipe through `c++filt` if needed.

- **RzBin plugin architecture** (`librz/bin/p/bin_*.c`, ~80 plugins
  for PE, Mach-O, Java, Dex, WASM, COFF, MZ, NE, LE, console ROMs).
  2O9 is ELF-only on Linux; correct scope for a sandboxing tool.

- **Color support** (`librz/cons/`). Full ANSI palette system. 2O9
  output is plain text; `less -R` is sufficient. Adding 16-color
  codes would roughly double the print code for marginal benefit.

---

*Source: rizin commit `ff4d660`, cloned to `/tmp/rizin-src`. 2O9 HEAD
`6fc4231`. Total rizin files read for this study: 22. Report length:
~2600 words.*
