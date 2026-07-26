# L4Ka::Pistachio — C++ → C Migration Plan

Status: **plan / not started** · Target: `x86-x64-p4-smp` · Author-assisted design, 2026-07

## 1. Goal and scope

- **Goal:** eliminate the C++ toolchain dependency — build the kernel (and userland) with a
  pure C compiler. Behavior and the exported L4 binary ABI stay identical.
- **Fidelity:** faithful 1:1 translation. `class` → `struct` + free functions; same control
  flow, same struct layouts, same KIP/UTCB/TCB ABI. No idiomatic reshaping.
- **Scope:** the kernel *and* userland for the active `x86-x64-p4-smp` configuration.
  PowerPC / PowerPC64 / x32-compat / HVM trees are **out of scope** (not compiled by this
  config; left as-is, optionally deleted later).
- **Strategy:** staged — the build compiles and boots at every step (mixed C/C++ during the
  transition via `extern "C"`).

## 2. Why this is tractable (findings)

The active build (`ARCH=x86 SUBARCH=x64 CPU=p4 PLATFORM=pc99 API=v4`, `CONFIG_DEBUG=y`)
compiles **72 kernel TUs + ~48 userland TUs**, and the linked kernel contains **zero
vtables** (`nm x86-kernel | grep -c _ZTV` → 0). Every genuinely hard C++ construct is either
absent from the codebase or disabled by this config:

| Hard construct | Where it lives | Status in this build |
|---|---|---|
| Virtual dispatch | `mdb_t` / `mdb_mem_t` / `vrt_t` (mapping-DB drivers) | **not compiled** (`CONFIG_NEW_MDB` off) — 0 vtables |
| `vmcs_field<index,T>` VMX magic-registers | `arch/x86/vmx.h`, `glue/v4-x86/x32/hvm-vmx.cc` | **not compiled** (`CONFIG_X_X86_HVM` off) |
| Pointer-to-member ctrlxfer tables | `api/v4/tcb.h`, `glue/v4-x86/x32/ktcb.h` | **not compiled** (`CONFIG_X_CTRLXFER_MSG` off) |
| Namespaces (`x32::` / `x64::` UTCB) | `glue/v4-x86/utcb.h`, `.../x64/x32comp/*` | **not compiled** (compat-mode off) |
| Exceptions / RTTI / destructors | — | **none anywhere** (`-fno-exceptions -fno-rtti` already set) |
| Multiple inheritance, modern C++ (auto/lambda/constexpr/STL) | — | **none anywhere** (pre-C++11 "C with classes") |

The IPC fast path is hand-written assembly (`glue/v4-x86/x64/trap.S`, `CONFIG_IPC_FASTPATH`)
plus non-virtual inline methods — there is **no dispatch cost at risk**. The asm↔C++ boundary
is already C-ABI-compatible: entry points are `extern "C"` and method calls from asm are
`__asm__("name")`-pinned (e.g. `tcb_resources_save`/`load` in `glue/v4-x86/resources.h`,
called from `trap.S`).

What actually remains is **mechanical and uniform** (see §4).

## 3. The core mechanic — how the build stays green

The binding constraint is *not* the classes. It is that **a `.c` file cannot `#include` a
header containing C++ class definitions**, and that during the transition C-compiled and
C++-compiled objects must still link despite name mangling.

Solution: a **C/C++ common subset**, applied in three passes so the kernel never stops
building.

### Pass A — rewrite in the common subset, still compiled as C++

```
    // before
    class foo_t { word_t x; public: int m(int); bool operator==(foo_t o); };

    // after — valid as BOTH C and C++
    struct foo_t { word_t x; };
    BEGIN_DECLS
        int  foo_m (foo_t *self, int);
        bool foo_eq(foo_t a, foo_t b);
    END_DECLS
```

- Method bodies in `.cc` → free functions taking an explicit `foo_t *self` first argument.
- Operators → named functions (`operator==` → `foo_eq`); implicit conversions made explicit
  (usually just `.raw` on the backing union).
- Single inheritance → struct embedding, base as first member (§4).
- `BEGIN_DECLS` / `END_DECLS` expand to `extern "C" {` / `}` under `__cplusplus`, and to
  nothing in C. This gives every free function **C linkage**, so the symbol a C++ TU emits
  matches what a C TU will emit later.
- The kernel is **still C++**, still compiled with `gcc -x c++`, still boots identically.
  This pass is ~90% of the labor and every step is independently verifiable.

### Pass B — flip files `.cc` → `.c`, leaf-first

- Rename `foo.cc` → `foo.c` (or add a per-file `CFLAGS_foo = -x c` override; the `.c` build
  rule in `Mk/Makeconf` already works — the disassemblers `amd64-dis.c` compile today).
- Because the shared decls are `extern "C"`, the C-compiled symbols match what the remaining
  `.cc` TUs still reference. C and C++ objects interoperate across a stable ABI boundary.
- Flip in dependency order (leaves first). Build stays green through **every** file flip.

### Pass C — remove the C++ path entirely

- `#define INLINE extern inline` → `static inline` (avoids GNU89-vs-C99 `extern inline`
  semantic flip; preserves "inline-only, no standalone symbol" intent).
- Set `-std=gnu11` (GNU extensions are needed: `__builtin_*`, statement exprs, anon unions).
- Replace the prioritized-global-ctor machinery (`ctors.ldi`, `call_{cpu,node,global}_ctors`)
  with explicit init-function calls (§4).
- `Mk/Makefile.voodoo` `tcb_layout.h` generation: `-x c++` → `-x c` (offsetof works in both).
- Drop the C++ compile rule. **Pure C toolchain — goal met.**

## 4. Conversion rulebook (faithful 1:1)

- **Methods:** `tcb_t::get_global_id()` → `tcb_get_global_id(tcb_t *self)`. Call sites
  `t->get_global_id()` → `tcb_get_global_id(t)`. Type names unchanged (`tcb_t` stays `tcb_t`).
- **Header accessors (`INLINE`):** → `static inline` free functions.
- **Inheritance:** `class space_t : public x86_space_t` →
  `struct space_t { x86_space_t base; /*…*/ };` with base as the first member so pointer
  casts stay valid; base-method calls → `x86_space_m(&sp->base, …)`. All static — fully
  resolvable, no vtable needed.
- **Word-wrapper types** (`msg_tag_t`, `threadid_t`, `time_t`, KIP fields): already
  `union { struct { BITFIELDn(…) } x; word_t raw; }` — already valid C. Class → struct;
  operators/conversions → functions or explicit `.raw`.
- **Templates** (all trivial): `min`/`max` → macros; `local_apic_t<base>` / `rtc_t<0x70>` /
  `i8259_pic_t<base>` → base as a macro/argument; `bitmask_t<T>` / `ringlist_t<T>` → X-macros
  or concrete structs; `asid_manager_t<T,SIZE>` → concrete instantiation;
  `virt_to_phys<T>`/`phys_to_virt<T>` → macros. (`vmcs_field<>` — the one hard template — is
  not in this build.)
- **`new` / `delete`:** only class-scoped overloads redirecting to `mdb_alloc_buffer` / `kmem`;
  `new (radix) mdb_table_t` → `mdb_table_alloc(radix)`. Minimal (mdb not compiled here).
- **References:** `T&` → `T*`, add `&` at call sites. Rare.
- **Global ctors** (`idt`, `tss`, `boot_cpu_ft`, `cpu_kdb`): → explicit `idt_init()`,
  `tss_init()`, `cpu_features_probe()` … called from `init.cc` in the same CPU / NODE / GLOBAL
  order the linker-generated `__ctors_*__[]` arrays enforced. Note `x86_x64_cpu_features_t`'s
  ctor runs live `CPUID` probing — order-critical.
- **ABI invariants:** keep KIP / UTCB / TCB struct layouts byte-identical. Preserve the
  `TCB_START_MARKER` / `TCB_END_MARKER` comments so `Makefile.voodoo`'s member-name scraping
  still matches.

## 5. Conversion order (bottom-up by dependency)

1. **Scaffolding:** add `BEGIN_DECLS`/`END_DECLS` to `src/generic/macros.h`; wire per-file
   `CFLAGS_*`; confirm `-x c` compiles a trivial converted TU and links.
2. **Leaf value types:** `types.h`, `thread.h` (`threadid_t`), `ipc.h` wrappers,
   `kernelinterface.h` (KIP). Included nearly everywhere → convert first.
3. **Utilities / allocators:** `bitmask`, `lib`, `sync`/`atomic` (spinlocks), `kmem`,
   `mapping_alloc`.
4. **Core objects:** `space_t` / `x86_space_t`, `tcb_t`, `scheduler_t` / `rr_scheduler_t`.
5. **Glue / arch / platform:** `init`, `idt`, `exception`, `resources`, `syscalls`,
   `platform/pc99`, `intctrl-{apic,pic}`.
6. **kdb** (separate track, required because `CONFIG_DEBUG=y`): redesign `DECLARE_CMD` to
   register a plain `cmd_ret_t (*)(cmd_group_t*)` function pointer instead of the
   `&kdb_t::method` pointer-to-member stored in a linker set; change `kdb/Makeconf`'s
   grep-codegen (which parses `Class::method` syntax to emit prototypes) to emit plain
   prototypes. May be deferred by temporarily building `CONFIG_DEBUG=n`.
7. **Userland** (independent — decoupled from the kernel by the L4 binary ABI): same three-pass
   method over ~48 TUs (`serv/sigma0`, `util/kickstart`, `apps/l4test`, `lib/l4`, `lib/io`).
   Can run in parallel with the kernel track.
8. **Pass C cleanup** (§3): flip dialect, drop C++ machinery.

## 6. Verification

- **After each Pass-A change:** rebuild (still C++) + boot `l4test` / `pingpong` under QEMU —
  behavior must be unchanged (same compiled language).
- **After each Pass-B flip:** rebuild (mixed C/C++) + boot test.
- **ABI safety net:** `_Static_assert` on `sizeof` / `offsetof` of KIP, UTCB, TCB; diff the
  generated `tcb_layout.h` before/after each core-struct conversion.

Build command (from repo top):

```sh
tools/autobuild x86-x64-p4-smp
# equivalently, in build/x86-x64-p4-smp/kernel: make batchconfig && make   → ./x86-kernel
```

## 7. Risk register

Low-risk overall (repetitive, mechanical). Risk concentrates in three narrow places:

1. **kdb command-dispatch codegen** — the only real redesign (pointer-to-member + grep codegen).
2. **Global-ctor ordering** — mis-ordering `boot_cpu_ft` / `tss` / `idt` init yields silent
   boot/SMP bugs; the CPUID-probing ctor is the sharpest edge.
3. **Byte-exact struct layout** — any drift in KIP/UTCB/TCB breaks the userland ABI and the
   asm offset scraping. Guarded by the `_Static_assert` / `tcb_layout.h` diff net above.

## 8. Open decisions (with recommendations)

1. **PowerPC / x32 / HVM trees:** leave untouched (not compiled by this config); optionally
   delete later. *Recommended: leave.*
2. **C dialect:** `-std=gnu11`. *Recommended.*
3. **kdb timing:** land the core kernel first with `CONFIG_DEBUG=n` to reach a pure-C boot
   fast, then re-enable and convert kdb. *Recommended.*

## 9. Pilot slice — DONE (2026-07-25)

The first pilot proved the **Pass-B mechanic end-to-end** on a real leaf, `src/generic/lib.cc`
(freestanding `memcpy`/`memset`). Changes:

- **Scaffolding in `src/generic/macros.h`** (foundation for the whole migration):
  - `BEGIN_DECLS` / `END_DECLS` → `extern "C" {`/`}` under C++, nothing under C.
  - `INLINE` made C-safe: `extern inline` for C++, `static inline` for C (defuses the
    C99/gnu11 `extern inline` "emits an external definition" landmine).
  - These recompile every C++ TU with **zero behavior change** — full clean rebuild.
- **`src/generic/lib.h`**: switched to `BEGIN_DECLS`/`END_DECLS` (now C-includable).
- **`src/generic/lib.cc` → `src/generic/lib.c`** (`git mv`): dropped the dead `#include
  <debug.h>` and the `extern "C"` wrappers; include `<generic/lib.h>` for decl/def consistency.
- **`src/generic/Makeconf`**: `lib.cc` → `lib.c` in `SOURCES`.

Verification:

- `lib.o` compiles via the `.c` rule (now with `-ffreestanding`), emits **weak, unmangled C
  symbols** (`W memcpy`, `W memset`), **zero `_Z` mangling**, and links with the 60+ C++
  objects into a working `x86-kernel` (build exit 0).
- **Machine code for `memcpy`/`memset` is byte-identical** between the C and C++ compiles
  (`objdump -d` diff empty) — behavior provably unchanged; only symbol linkage differs.
- After changing `SOURCES`, the aggregated `.depend` must be regenerated (it cached
  `lib.o ← lib.cc`): `rm .depend && make .depend`. Fold this into the flip procedure.

### Lessons that refine the plan

1. **Include-closure coupling is the real gating factor, not the class count.** A file can
   only flip to `.c` once its entire transitive `#include` closure is in the C subset. The
   force-included closure (`-include types.h`, `-imacros macros.h/config.h`) is already
   C-clean (`types.h` guards its C++-only bits with `#ifdef __cplusplus`).
2. **`debug.h` is a high-priority early conversion.** It sits in almost every file's closure
   (like `types.h`) but is *not* yet C-safe: `glue/v4-x86/debug.h` has `class` forward-decls,
   default arguments on `INLINE` helpers, a `debug_param_t` data class, and pulls in
   `atomic.h`/`trapgate.h`. Converting it early unblocks the widest set of downstream flips.
3. **`threadid_t` is a migration *chunk*, not a pilot** — it's referenced in **38 files /
   ~375 call sites**. Small value types with operator overloads have huge blast radius;
   schedule them as their own dedicated slices, not warm-ups.
4. **Automated boot testing needs a serial console first.** The `x86-x64-p4-smp` kdb console
   is keyboard/VGA (`CONFIG_KDB_CONS_KBD=y`, no `CONS_COM`), so `qemu -serial` captures
   nothing. The native `grubdisk/bootdisk.img` path is also broken (`grub.img` is fetched
   from a dead URL). **Next infra task:** enable a COM/serial kdb console (or a small custom
   multiboot+serial harness) so `qemu-system-x86_64 -kernel kickstart -initrd
   "kernel,sigma0,l4test"` yields capturable pass/fail output. Until then, the byte-identical
   `objdump` check is the strongest per-slice equivalence signal.

### Recommended next slices

1. **Boot/serial test harness** (infra) — so every later slice can be boot-verified.
2. ~~`debug.h` closure → C subset~~ **DONE — see §10.**
3. Then proceed with §5 order (value types, allocators, core objects, …).

## 10. `debug.h` closure → C subset — DONE (2026-07-25)

`#include <debug.h>` now parses cleanly from a C translation unit (verified: a probe
`.c` including `<debug.h>` and using `printf`/`ASSERT`/`panic` compiles with `-Wall -Wextra`,
zero warnings), and the full C++ kernel still builds byte-size-identically (358112).

Technique — **single-definition dual-language**: convert `class`→`struct` (no C++ ABI/mangling
change), guard genuinely-C++-only syntax (methods, operators, inheritance, default args,
`extern "C"`, nested class-scoped enums) behind `#ifdef __cplusplus`, and expose to C only the
data layout + the few symbols C actually needs. One layout definition, so no drift.

Files changed:

- **`src/generic/types.h`** — C `bool`/`true`/`false` (via `_Bool`), guarded `!__cplusplus`.
  Foundational: `bool` is a C++ keyword used across nearly every header.
- **`src/arch/x86/atomic.h`** — `class atomic_t`→`struct atomic_t`; the operator/`cmpxchg`
  methods guarded `#ifdef __cplusplus`; `val` now a plain member; added
  `typedef struct atomic_t atomic_t;`. C sees `struct atomic_t { word_t val; }`.
- **`src/arch/x86/x64/trapgate.h`** — `class x86_exceptionregs_t`→`struct`; the class-scoped
  `num_regs`/`reg_e`/`num_dbgregs` enums guarded `#ifdef __cplusplus`; the register-array
  dimension uses a new plain macro `X86_EXCEPTIONREGS_NUM_REGS`. (gcc's C frontend warns
  "declaration does not declare anything" on *any* nested type decl inside a struct — hence
  the guard + macro rather than a shared enum.) C sees the register frame as a real struct,
  which is the point (the `X86_EXCWITH_ERRORCODE` macro is designed to allow C handlers).
- **`src/arch/x86/trapgate.h`** — `printf` decl wrapped in `BEGIN_DECLS`; `x86_exceptionframe_t`
  (which *inherits* `x86_exceptionregs_t`) uses the dual-definition form: C++ keeps the class
  with inheritance + methods; C gets `struct { struct x86_exceptionregs_t __base; }` (identical
  layout, since the class adds only statics/methods).
- **`src/glue/v4-x86/debug.h`** — guarded the C++-only region (`spin`/`spin_forever` with
  default args, `debug_param_t`, `class space_t/tcb_t` fwd-decls, kdb breakpoint decls) and the
  kdb-internal `#include <kdb/tracepoints.h>` behind `#ifdef __cplusplus`; left the
  `enter_kdebug` asm macro visible to C.
- **`src/generic/debug.h`** — `printf` via `BEGIN_DECLS`; the `tcb_t*`-returning
  `get_kdebug_tcb()` guarded `#ifdef __cplusplus`.

Deliberately **deferred** (guarded out of C's view, not converted): the kdb tracepoint machinery
(`kdb/tracepoints.h` → `linker_set.h`, `tracebuffer.h`) — debugger-internal, no generic C file
needs it. Convert when the kdb subsystem itself is tackled (§5 step 6).

Out of scope for this config (unchanged, would need the same treatment if ever built):
`src/arch/x86/x32/trapgate.h` still uses `static const word_t num_regs` (x32 not compiled).

Reusable lesson: `class`↔`struct` is free in C++ (no ABI change); the friction is nested
class-scoped enums/`static const` members and default arguments — guard those, macro-ise any
value C needs (e.g. array bounds).

## 11. Boot-test harness + early-boot faults (2026-07-25)

Built `tools/boottest` (serial capture under QEMU) to boot-verify conversion slices.
Getting a boot to work uncovered several pre-existing, modern-toolchain bugs **unrelated
to the C++→C conversion** (they live in files the conversion never touches):

**kickstart free-memory search (FIXED, committed).** `kip_manager_t::is_mem_region_free()`
only skipped the whole-space *shared* descriptor; QEMU's multiboot map also yields a
whole-space *arch-specific* descriptor whose 64-bit base has stray high bits, so the loader
found no free memory and panicked before launching the kernel. Fixed by skipping any
descriptor covering the whole 32-bit space (compared in 32-bit terms). See commit
"kickstart: ignore whole-address-space descriptors in free-memory search".

**Kernel fault #1 — long-mode helper not inlined (FIXED, committed).** The 32-bit trampoline
`init_paging()` runs at low physical with paging off, so every callee must be inlined. Modern
gcc stopped inlining `x86_mmu_t::has_long_mode()`, emitting it at the kernel's high virtual
address; the call from low-physical code triple-faulted. Fixed with `always_inline` on
`has_long_mode` + `x86_x64_has_cpuid` + `x86_cpuid`. (If more `init.init32` callees are found
non-inlined later, they need the same treatment.) Commit "Force-inline the long-mode
detection helpers used by init_paging".

**Kernel fault #2 — trampoline page tables/GDT clobbered by dropped relocation
addends (FIXED, committed).** Root cause: init32 is compiled `-m32` then converted
with `objcopy -O elf64-x86-64` (`arch/x86/x64/Makeconf`); current binutils drops the
field addends of the object's absolute REL relocations during the REL→RELA conversion,
so every *statically-indexed* store into `init_pml4`/`init_pdp`/`init32_gdt` lost its
offset and collapsed onto the array base — `init_pml4[0]`'s high dword (+4) zeroed the
entry (→ CR3 walks a null table → triple fault the instant paging is enabled), and
`init32_gdt[1]` (+8) landed on `init32_gdt[0]` (→ selector 0x08 invalid → #GP on the
`%ds` load). Found with a live gdb hardware watchpoint on physical `0xd1d000` (low dword
written, then immediately overwritten by the high dword at the *same* address). Fixed by
laundering the base pointers through an empty `asm` so element offsets become instruction
displacements, not relocation addends (the pdir loop was already immune — register-scaled
addressing). Commit "Fix init_paging page tables/GDT clobbered by dropped relocation
addends". With this the trampoline completes and **the kernel reaches 64-bit mode and
prints its banner.**

**Kernel fault #3 — tcb_t::init faults in the KTCB area (open).** Now that boot gets
this far, `tcb_t::init` (`0xffffffffc060c926`) faults writing to the KTCB area
(`0xfffffffe8...`, `#PF` write/not-present). This is 64-bit kernel code (not affected by
the objcopy bug) — a genuine early-init sequencing/mapping issue to chase next.

*(historical, superseded)* Fault #2 first looked like: CR3 points at an empty top-level page table.
Once #1 is fixed, boot reaches `enable_paging()` and the very next instruction fetch
`#PF`-not-present → `#DF` → triple fault. Traced with raw serial markers (COM1 is live from
kickstart) + QEMU `-d int` + reading physical memory and CR3 from the trampoline:
  - CR3 = `(u32_t)init_pml4` = `0xd1d000`; `&init_pml4`, the array access, and CR3 all agree.
  - Physical `0xd1d000` (and the whole pml4) reads **all-zero** at runtime, so the walk fails.
  - The `init32.o` stores populating the tables are **correct** (verified pre-link: pml4[0]
    low→+0x0, high→+0x4; pml4[511]→0xff8/0xffc). PDP_IDX=PML4_IDX=511, PDIR_IDX=0.
  - Ruled out: dead-store elimination (tried `volatile` — no change), store miscompilation,
    address divergence, linker VMA/LMA mismatch (`.init` is identity-mapped: vaddr=paddr=
    0xd19000 per the program headers; `AT(ADDR(.init))` in `glue/v4-x86/x64/linker.lds`).
  - **Open:** why correct absolute writes to `0xd1d000` don't manifest in the physical memory
    CR3 walks. Next step: live QEMU+gdb (`-s -S`) with a watchpoint on physical `0xd1d000`
    to catch what writes/clears it during the trampoline.

Net: the harness + serial path work and reach `Launching kernel ...`; a full boot to userland
is blocked on fault #2. Per-slice verification meanwhile relies on the `objdump` byte-identity
check and the clean C++ rebuild (binary size unchanged).

## 12. Boot progress after the objcopy fix (2026-07-25)

With the addend fix (§11) the kernel boots **all the way through init** (verified with
CONFIG_VERBOSE_INIT): CPU features, kernel space, TCBs (the KTCB `#PF`s are the *designed*
on-demand allocation via the page-fault handler — "fault #3" was a non-issue), IDT, KIP,
ACPI/APIC/IOAPIC, timer, per-CPU bring-up (3494 MHz), threading, "Idle thread started",
and it creates sigma0 + the root task. sigma0 and l4test run in user mode (cpl=3).

**Next blocker — sigma0 crashes right after L4_KernelInterface().** sigma0's `lock; nop`
(user/include/l4/amd64/syscalls.h:57) is the L4 KernelInterface magic; the kernel emulates
it in exc_invalid_opcode (glue/v4-x86/exception.cc:481), returning the KIP base in %rax via
`space->get_kip_page_area().get_base()`. Immediately afterwards sigma0 does a user read of
`CR2=0x8` (near-null) → i.e. the returned KIP base is 0, so sigma0's space appears to have no
KIP area set. Next step: check how sigma0's space / kip_area is initialised (space_t::init /
the root-server creation path) — a genuine kernel/space-init issue, unrelated to the
trampoline fixes and to the C++→C conversion.

## 13. Full boot achieved — l4test runs (2026-07-25)

After the crt0 alignment fix the whole chain works end to end: kernel boots, sigma0 pages
the root task, and **l4test reaches its menu and runs the suite**. Driving it (send "8" =
All tests over the serial console once booted) runs Kernel-Interface-Page tests (all pass),
memtest Page-touch OK, and the Simple IPC test — From parameter / Send / ReplyWait / Send
timeout / Receive timeout all **OK** — then **"Local destination Id: FAILED"**, which drops
into kdb. So: from "triple-faults on the first instruction" to a booting kernel running the
test suite, mostly passing.

Fixes that got here (all pre-existing modern-toolchain bugs, none related to the C++→C
conversion): kickstart free-memory search; long-mode helper inlining; the objcopy addend
drop in init_paging's page tables + GDT (the big one, found via a live gdb watchpoint on
physical 0xd1d000); and sigma0's crt0 stack-alignment (`andq $-16,%rsp`) so modern-gcc SSE
in userland doesn't #GP.

**Open — the one failing subtest:** "Local destination Id" in the inter-AS untyped-word IPC
test (l4test eip ~0x1000295). Local-thread-ID IPC addressing; next thing to chase if a green
test run is wanted. Note the kernel build currently has CONFIG_KDB_CONS_COM and
CONFIG_VERBOSE_INIT enabled in the (gitignored) build config for testing.

## 14. "Local destination Id" subtest — investigation (2026-07-25, unresolved)

The one failing l4test subtest is "Local destination Id" in Simple IPC's *local* variant
(setup_ipc_threads(..., true, true) — SAME address space, so intra-space local-ID IPC).
Two threads: t2 (0x3d00000001) does `L4_Send(L4_LocalId(ipc_t1))` then `L4_Receive(ipc_t1)`;
t1 (0x3e00000001) does `L4_Receive(ipc_t2, 5s)` — which times out → FAILED → drops to kdb.

Traced (kernel printf on the send/exregs paths, reverted afterwards):
- The send reaches sys_ipc with `to=0x3e00000001` (t1's global id), `is_local=0`, resolves to
  a valid tcb, no error — so the *destination* is correct (kernel handles local IDs via
  `#define HANDLE_LOCAL_IDS`, converting before `get_tcb`; receive-side matching at
  ipc.cc:556/714 uses `get_local_id()`).
- L4_LocalId(ipc_t1) never calls ExchangeRegisters (0 exreg traces before the fail), i.e.
  userland `L4_IsGlobalId(ipc_t1)` returned false — surprising, since 0x3e00000001 has its low
  bit set and the L4_ThreadId_t bitfield/endian macros (types.h) look correctly configured for
  amd64 (L4_64BIT + L4_LITTLE_ENDIAN).

So the destination resolves to t1 but the **send does not rendezvous** with t1's receive.
Root cause is in the local-ID rendezvous/matching semantics (or a subtle threadid
interpretation), not destination resolution — needs two-thread state-level tracing to finish.
Everything ELSE in the suite passes; this is one subtest in an otherwise working kernel.


## 15. `kmem_t` allocator → C (Pass A + Pass B) — DONE (2026-07-25)

Converted the kernel memory allocator, the first real subsystem after the scaffolding
work, and took it all the way to a compiled-as-C translation unit.

**Pass A** (`class kmem_t` → `struct kmem_t` + free functions, commit 41c464f):
- Methods → free functions under `BEGIN_DECLS`: `kmem.alloc(g,sz)` → `kmem_alloc(&kmem,g,sz)`,
  likewise `kmem_free` / `kmem_init` / `kmem_add`; the three internal routines become
  `kmem_do_alloc` / `kmem_do_alloc_aligned` / `kmem_do_free`.  Group-aware wrappers stay
  `INLINE` forwarders in the header.  Bodies take an explicit `kmem_t *self`.
- Dropped `friend class kdb_t` (struct members are public; kdb reads them directly).
- 73 call sites across 24 files (incl. out-of-scope powerpc/x32, kept consistent).

**Pass B** (`kmemory.cc` → `kmemory.c`, commit 0d4842f) required a small header closure:
- `spinlock_t` (arch/x86/sync.h): dual-representation, same as atomic.h — struct + free
  functions (`spinlock_lock/unlock/init/is_locked`) with the C++ methods kept as
  `#ifdef __cplusplus` one-line forwarders, so **every existing `x.lock()` call site is
  unchanged**; only the flipped C file uses the free functions.  Blast radius = 1 file.
- `generic/sync.h` (`lockstate_t`, non-SMP `spinlock_t`), `kdb/linker_set.h`
  (`linker_set_t`, `linker_set_entry_t`), `kdb/tracepoints.h` (`tracepoint_t`,
  `tracepoint_list_t`): classes → structs, methods/wrappers guarded under `__cplusplus`.
  With `CONFIG_TRACEPOINTS` / `CONFIG_TRACEBUFFER` off these reduce to plain C
  (`DECLARE_TRACEPOINT` = aggregate init, `TRACEPOINT` = nothing).  `tracebuffer.h` was
  already C-safe (everything under `CONFIG_TRACEBUFFER`); `init.h` is trivial.
- In-body C++-isms in kmemory: `self->spinlock.lock()` → `spinlock_lock(&self->spinlock)`;
  the `max()` template (types.h, `__cplusplus`-only) → local `KMEM_MAX` macro (operands
  side-effect free); one `void*`/`word_t*` comparison needed an explicit cast in C.

Both passes boot-verified (kmem drives the whole boot: TCBs, spaces, sigma0/l4test load).
Size drifted +8 bytes at Pass B (C vs C++ codegen) — expected; a non-leaf flip is not
byte-identical the way lib.c was.

**Technique confirmed for the rest of the migration:** dual-representation makes a widely
used primitive (spinlock_t: 57 lock / 70 unlock sites) C-includable with a *one-file* blast
radius, because C++ callers keep the forwarding methods.  Convert the primitive once, then
flip consumer files one at a time.

### Recommended next slices
- More Pass-B flips of allocator-adjacent generic files that now have a C-clean closure
  (e.g. `mapping_alloc.cc`) — each just needs its own includes checked + a boottest.
- Value types next: `threadid_t` / `spaceid_t` (many consumers, but pure data + methods →
  dual-representation keeps call sites intact), then work up toward `tcb_t` / `space_t`.


## 16. `mapping_alloc.cc` → C (Pass B) — DONE (2026-07-25, commit ce22043)

Third `.c` file.  The lesson here: **a file's difficulty is its include closure, not its
own code.**  `mapping_alloc` is a plain buffer allocator, but it includes `mapping.h`, the
MDB *type* header, which `#include`s the entire page-table header stack
(`pgent.h` → `ptab.h` → `x86.h`, plus `fpage.h`).  Converting all of that (pgent_t /
fpage_t alone are ~120 methods) would have been a huge slice.

**What kept it bounded:** `mapping_alloc` uses only the MDB node structs (via `sizeof`),
`mdb_buflist_t`, and the `MDB_NUM_PGSIZES` constant — never `pgent_t` / `fpage_t`.  So
`mapping.h` now guards `#include pgent.h` / `#include fpage.h` under `__cplusplus` and pulls
in `ptab.h` directly for the constant.  `ptab.h`'s one class (`x86_pgent_t`) is guarded;
`x86.h` was already class-free.  The 120-method page-table classes were **not** touched.

- `mapnode_t` / `rootnode_t` / `dualnode_t` / `mdb_buflist_t`: class → struct, methods +
  nested `pgsize_e` enum + the namespace-scope `pgsize_e` operator overloads guarded under
  `__cplusplus`.  class↔struct is ABI-neutral (no vtables).  Their bitfield-union data stays
  C-visible so `sizeof` is correct.
- Linkage: `mdb_alloc_buffer` / `mdb_free_buffer` (defined in the flipped C file) moved under
  `BEGIN_DECLS` in **both** `mapping.h` and `mdb.h` so every TU agrees on C linkage;
  `mapping.cc`'s file-scope forward decl of `mdb_buflist_init` became `extern "C"`.
  *Gotcha:* a linkage spec (`extern "C"`) is illegal at block scope — it has to sit at file
  scope, not inside the function that calls it.
- In-file: the two private structs (`mdb_link_t`, `mdb_mng_t`) needed `struct` tags on their
  self-referential pointer members + trailing typedefs; one `~(int)` mask got a `word_t`
  cast for `-Wconversion`.

Forward-decl tag rule reconfirmed: a type still defined as `class` elsewhere (`space_t`,
`pgent_t`) must keep a `class` forward-decl for C++ and a `struct` one for C (guarded), or
the C++ front end warns on the tag mismatch.  Types converted in-file (`mapnode_t` etc.) use
`struct` in both.

Boot-verified: the MDB allocator is driven by every page mapping, so reaching l4test with no
"Illegal MDB buffer allocation" panic confirms it.  Three generic C files now: `lib.c`,
`kmemory.c`, `mapping_alloc.c`.


## 17. `threadid_t` → struct (dual-representation) — DONE (2026-07-25, commit 3e4e6a7)

First **value type**.  `class threadid_t` → `struct threadid_t` in `api/v4/thread.h`: the
full C++ API (static factories, `is_*`/`get_*`/`set_*` accessors, `==`/`!=` operators, the
out-of-line `set_global_id`, the `threadid(rawid)` helper) is guarded under `__cplusplus`;
the bitfield union (`raw` / `local` / `global`) stays C-visible.  The original
`public:`/`private:` is preserved *inside* the guard:

    struct threadid_t {
    #if defined(__cplusplus)
    public:
        ... methods / statics / operators ...
    private:
    #endif
        union { word_t raw; struct {...} local; struct {...} global; };
    } __attribute__((packed));
    typedef struct threadid_t threadid_t;

ABI-neutral (no vtables) — the linked kernel came out **byte-for-byte identical** and all ~38
call sites are untouched.  Verified additionally by a standalone C compile of the C-visible
struct (parses, word-sized, union members reachable).

**Honest status:** this makes the *type* C-ready, but it does not yet let any file flip,
because `thread.h` pulls in `kernelinterface.h` (the KIP) at the top, and `is_interrupt()`
needs `get_kip()`.  So the value-type layer and the KIP are entangled: the natural next
domino is the **`kernelinterface.h` / KIP** closure, after which thread.h becomes a candidate
for C-includability and threadid_t's C free-function API (raw/equality/predicates/factories,
minus the KIP-dependent `is_interrupt`) can be added and exercised by a real C consumer.

Pattern note: for a value type, dual-representation with the access specifiers preserved
inside the `__cplusplus` guard keeps the C++ side literally identical (byte-identical proof),
which is the safest possible way to land the struct.  Free functions follow the first
consumer, never speculatively (they'd be untestable until then).


## 18. `kernelinterface.h` / KIP closure — SCOPED + DONE (2026-07-25, commit 0f38291)

**Outcome:** executed exactly as scoped below, in one slice.  All ~20 classes across the 4
headers became dual-representation structs; the linked kernel is **byte-for-byte identical**
(362368) and boots l4test.  Milestone proven: a standalone C compile that `#include`s
`thread.h` reaches `threadid_t`, `get_kip()`, the nested info structs, bitfields, the
`char[4]/word_t` unions, and `mem_region_t`-by-value — all from C.  `thread.h` + the KIP are
now C-includable.  The scope below matched reality with no surprises (the operator/overload
constructs all sat cleanly inside the `__cplusplus` guard; no call-site churn).  Reconfirmed
that anonymous `struct`-in-`union` (memory_info_t) and `union`-in-`struct` compile under
`-std=gnu99`.  Next: threadid_t's C free-function API, and hunting `.cc` files whose only
non-C-safe include was `thread.h`/KIP.

---

### Original scope (2026-07-25)

Goal: make the Kernel Interface Page header C-includable via dual-representation, which in
turn makes **`thread.h` fully C-includable** (its only non-C-safe include).  This is the
gate blocking the thread/value-type layer.

### Closure — exactly 4 headers, ~20 classes, and it does NOT cascade
- `api/v4/kernelinterface.h` (449 lines, 18 classes)
- `generic/memregion.h` (1 class: `mem_region_t`)
- `api/v4/memdesc.h` (1 class: `memdesc_t`)
- `api/v4/procdesc.h` (1 class: `procdesc_t`)

The three sub-includes pull in nothing further.  `thread.h`'s other include,
`api/v4/config.h`, is **already C-safe** (79 lines of `#define`, 0 classes).  Blast radius is
49 files, but only 2 headers include the KIP directly (`thread.h`, `glue/v4-x86/schedule.h`);
the rest are `.cc`.  `tcb.h` / `space.h` do **not** embed KIP classes by value.

### Difficulty: LOW.  It is ~20 plain-POD classes, mechanical to dual-represent
The whole closure has **none** of the genuinely hard constructs: no ctors/dtors, no virtual,
no inheritance, no templates, no `friend`, no static data members, no reference *members*, no
default arguments, no member initializers, no true nested classes (`memdesc_t::type_e` is the
lone nested *enum*).  Every class's data is plain C — `word_t` / bitfield-unions (`BITFIELD*`
macros are C-safe) / char[4] arrays / by-value sub-structs / one flexible array member.

The only constructs that a bare `class→struct` can't express in C — and all are handled the
spinlock/threadid way, by **guarding them under `#if defined(__cplusplus)` (zero call-site
changes, since C++ keeps them)**:
- 3 operator overloads: `api_flags_t::operator word_t()`, `api_version_t::operator word_t()`,
  `mem_region_t::operator+=`.
- 3 overloaded methods: `memory_info_t::insert` ×2, `memdesc_t::set` ×2.
- reference *parameters* on `mem_region_t` / `memdesc_t` methods — fine, the methods are
  guarded out for C.

### Conversion order (inner-first — `kernel_interface_page_t` embeds ~20 classes BY VALUE)
1. Leaf value types: `mem_region_t`, `procdesc_t`, `memdesc_t`, `magic_word_t`, and the
   bitfield info classes (`utcb_info_t`, `kip_area_info_t`, `clock_info_t`, `thread_info_t`,
   `page_info_t`, `processor_info_t`, `api_flags_t`, `api_version_t`, `memory_info_t`,
   `kernel_id_t`, `kernel_gen_date_t`, `kernel_version_t`, `kernel_supplier_t`).
2. Aggregates: `root_server_t` (embeds `mem_region_t`), `kernel_descriptor_t` (embeds the 4
   kernel_* value types).
3. `kernel_interface_page_t` (embeds nearly all of the above by value) + the trivial
   `get_kip()` (`return &KIP`; `KIP` is **already** `extern "C"`, so C sees the same symbol).

Because it's one header (plus 3 tiny ones), the whole thing can land as a **single
dual-representation slice** — there are no `.cc` flips here, so no per-file staging.

### Verification
- C++ regression build must come out **byte-identical** (class→struct is ABI-neutral, and the
  KIP layout is load-bearing — it is emitted into a fixed linker section and read by userland,
  so byte-identity is both the correctness proof and a hard requirement).
- boottest (the KIP is read constantly during boot).
- standalone C-parse of `kernelinterface.h` (and then of `thread.h`) to prove C-includability.

### Payoff / what it unblocks
- `thread.h` becomes C-includable → `threadid_t` can grow its real C free-function API,
  including `is_interrupt()` (now expressible: `get_kip()->thread_info.get_system_base()`
  becomes `thread_info_get_system_base(&get_kip()->thread_info)`).
- Any `.cc` whose only C++-header dependency was `thread.h` / the KIP becomes a flip candidate.

### Risks / watch-items
- **Byte layout is ABI**: keep every field, every `#if CONFIG_*` field variant, the anonymous
  `char[4]/word_t` unions, and the `version_parts[0]` flexible array exactly as-is.  The
  byte-identical build is the guardrail.
- Forward-decl tag rule (seen in §16): `kernel_descriptor_t` is forward-declared then defined;
  use `struct` consistently, and guard any `class`-defined-elsewhere forward decls.
- `kdb/api/v4/kernelinterface.cc` and other kdb files read KIP fields — they stay C++, so
  dual-representation (methods retained under the guard) leaves them untouched.

### Estimate
One focused slice.  ~20 mechanical class→struct + guard edits in one main header plus 3 tiny
ones, no call-site churn, no `.cc` flips.  Comparable in effort to the `mapping.h` conversion
(§16) but lower-risk (no include-guarding gymnastics, no linkage changes — pure
dual-representation), gated on the byte-identical build passing.


## 19. `threadid_t` C free-function API — DONE (2026-07-25, commit 1266a41)

With the KIP now C-includable (§18), `threadid_t` got the full C API promised in §17, and its
~20 C++ methods became one-line forwarders — the **spinlock_t pattern applied to a value
type, second stage** (§17 landed the struct; this landed the functions).

- Logic moved out of the in-class method bodies into `threadid_*` free functions (factories,
  accessors, predicates, compare) available to both C and C++.  `is_interrupt()` now reads
  `get_kip()->thread_info.system_base` directly (KIP is C-visible) instead of the
  `__cplusplus`-guarded `get_system_base()` accessor — same value.
- C++ methods: declared in the struct, defined out-of-line as forwarders under `__cplusplus`.
  `NILTHREAD`/`ANYTHREAD`/`ANYLOCALTHREAD`/`IDLETHREAD` macros made dual so they resolve in C.
- The data union was made **public** — the free functions are non-members, so in C++ they
  can't touch a private union.  Access control is compile-time only, so still ABI-neutral.

**Two-layer verification worth repeating for value types:**
1. The forwarding refactor came out **byte-for-byte identical** (362368).  That is the proof
   the free functions are behaviour-equivalent to the original methods — every one of the ~38
   C++ call sites now routes through them, and the codegen didn't move.
2. A standalone **C** program linked the free functions and asserted values at runtime
   (nilthread==0, anythread==~0, idlethread==0x1d1e…, raw round-trip, global-id accessors,
   equality, the `NILTHREAD` macro).  This is the piece boot alone can't give: it exercises
   the *C* compilation of the API, not just the C++ forwarders.

Gotcha logged: non-member free functions can't read a `private` union in C++ — either make the
data public (done here) or the functions would need friendship.  For a value type whose data
is now the shared C/C++ contract, public data is the right call.

Pattern crystallised (value type, two stages): §17 land the struct dual-representation
(byte-identical, call sites untouched); later, once the type's own dependencies are C-visible,
§19 move the bodies into free functions + forward (byte-identical again) and value-test the C
side.  Free functions still never land before their dependencies are C-ready.


## 20. Flip-candidate hunt + `ctors.cc` → C (2026-07-25, commit 7d28cb1)

Ran a systematic sweep to find `.cc` files whose include closure is *already* C-clean (so the
only remaining work is the body).  Method: for each of the 33 compiled `src/*.cc`, extract its
`#include` lines into a stub `.c` and `gcc -fsyntax-only` it as C with the real kernel flags.
A stub that compiles ⇒ that file's whole header closure is C-includable.  (Script lives in the
scratchpad; rerun after each header conversion to re-scan.)

**Result: exactly one fully-clean leaf — `glue/v4-x86/ctors.cc` — now flipped.**  It includes
only `debug.h` and is already plain C (func_ptr typedef, three extern ctor tables, three loops).
`ctors.h` prototypes moved under `BEGIN_DECLS`; C linkage now matches the C++ callers.

### The real payoff: a ranked blocker map (what to convert next)

Every other file is gated on a *small* set of header closures.  First-blocker tally (a file
may hit more than one; converting the top blocker reveals the next), with the header's size:

| Blocker header | # files gated (first) | size | shape |
|---|---|---|---|
| `glue/v4-x86/hwspace.h` | 9 | 45 ln | **0 classes** — some non-class C++ syntax at line 35 (ref/inline/operator?); cheap once identified |
| `api/v4/cpu.h` | 9 | 76 ln | 1 class — straight dual-representation |
| `api/v4/types.h` | 5 | 147 ln | 2 classes, foundational (widely included) |
| `arch/x86/x64/cpu.h` | 5 | 272 ln | 1 class (+ `arch/x86/cpu.h` likely in the family) |
| `api/v4/user.h` | 2 | — | gates kernelinterface.cc, x64/user.cc |
| `arch/x86/x64/syscalls.h` | 1 | — | gates exregs.cc |
| `api/v4/threadstate.h` | 1 | — | gates asmsyms.cc |

Highest leverage next: **`hwspace.h`** (9 files, tiny) and **`api/v4/cpu.h`** (9 files, one
class).  Converting the `cpu.h` family (`api/v4/cpu.h` + `arch/x86/x64/cpu.h` + `arch/x86/cpu.h`)
together would clear the largest CPU-gated cluster.  `api/v4/types.h` is the foundational one —
converting it early pays off transitively.  Note these tallies are *first*-blocker only; the
true unblock of a file needs its whole closure done, so expect to convert a small connected
group before the next `.cc` actually flips.


## 21. `hwspace.h` → macros + re-scan (2026-07-25, commit 87d317c)

`glue/v4-x86/hwspace.h`: the `virt_to_phys<T>` / `phys_to_virt<T>` function templates became
`__typeof__` macros (plan §4).  Byte-size-identical (362352), boots.  ~112 call sites
unchanged (all plain calls).

Re-ran the closure scan afterward.  **Lesson confirmed: clearing a first-blocker rarely flips
a file by itself** — hwspace.h gated 9 files, but all 9 also need the `cpu.h` family, so 0 new
CLEAN files.  What changed is the tally: `arch/x86/x64/cpu.h` jumped from 5 → **13** first-block
hits (the ex-hwspace files surfaced it as their next gate).  Updated ranking:

| Blocker | # gated (first) | note |
|---|---|---|
| `arch/x86/x64/cpu.h` | 13 | now the dominant gate |
| `api/v4/cpu.h` | 9 | 1 class |
| `api/v4/types.h` | 5 | foundational |
| `api/v4/user.h` | 2 | |
| `generic/acpi.h`, `x64/syscalls.h`, `threadstate.h` | 1 each | |

**Takeaway for sequencing:** stop chasing individual first-blockers; convert the **`cpu.h`
family as a unit** (`arch/x86/x64/cpu.h` + `api/v4/cpu.h` + `arch/x86/cpu.h`) plus
`api/v4/types.h`.  That cluster gates ~22 of the 32 compiled `.cc` and is the thing standing
between here and the first wave of actual `.cc` flips.


## 22. cpu.h family + `api/v4/types.h` → structs (2026-07-25, commit f547360)

Converted the dominant blocker cluster (dual-representation, byte-identical 362352, boots):
`api/v4/types.h` (`time_t`, `timeout_t`), `api/v4/cpu.h` (`cpu_t`), `arch/x86/x64/cpu.h`
(`x86_x64_cpu_features_t`).  `arch/x86/cpu.h` needed nothing (all INLINE asm, 0 classes).

**New wrinkle: two classes carry real C++ constructors** — `cpu_t::cpu_t()` and the
SEC_INIT `x86_x64_cpu_features_t::x86_x64_cpu_features_t()` (the order-critical CPUID prober).
For *dual-representation* they guard fine (`#if __cplusplus`), and the C++ build stays
byte-identical.  But it means: **a clean closure is necessary, not sufficient, for a flip.**
`cpu.cc` is now closure-clean, yet its body defines that constructor + the `boot_cpu_ft`
global, so flipping it needs the plan-§4 "ctor → explicit `*_init(self)` + wire into the init
order" work, not just a rename.  Same will hold for cpu_t's `descriptors[]`/`count` statics.

Re-scan after this cluster:

| | |
|---|---|
| **newly CLEAN closure** | `arch/x86/x64/cpu.cc` (needs ctor→init to actually flip) |
| next gates | `api/v4/queuestate.h` (11), `arch/x86/mmu.h` (9), `x64/segdesc.h` (3), `generic-archfpage.h` (3), `user.h` (2) |

**Refined mental model of the frontier:** the remaining work splits into (a) *header*
dual-representation (cheap, byte-identical, unblocks closures) — `queuestate.h`, `mmu.h`,
`segdesc.h`, `generic-archfpage.h`, `user.h` are the next batch; and (b) *body* flips, where
files with constructors/global-ctors (cpu.cc, and later the scheduler/space/tcb layer) need
the ctor→init conversion.  Keep clearing header closures until a *constructor-free* .cc goes
CLEAN — that's the next actually-trivial flip — while treating cpu.cc as the first
"needs-ctor-work" flip when we choose to take it.


## 23. queuestate.h + mmu.h → structs (2026-07-25, commit 7c97221)

Two more header gates, dual-representation, byte-identical (362352), boots.
- `api/v4/queuestate.h`: `queue_state_t` -> struct (`word_t state` visible; nested `state_e`
  enum + methods + OOL defs guarded).
- `arch/x86/mmu.h`: `x86_mmu_t` is static-methods-only with **no instances anywhere** — whole
  class + OOL methods guarded out of C entirely (a C struct would be an empty size-0-vs-1
  mismatch; nothing to represent).  New sub-pattern: *static-only "namespace" classes get
  guarded wholesale, not turned into empty structs.*

Frontier after this: still only `arch/x86/x64/cpu.cc` CLEAN (needs ctor→init).  Next gates:
`api/v4/threadstate.h` (12), `arch/x86/pgent.h` (8).  Note `pgent.h` is the 86-method
page-table-entry class deferred in §16 — the first genuinely *large* header conversion, and
the wall between here and the mapping/space `.cc` cluster.  `threadstate.h` (12 files, likely
small like queuestate) is the cheaper next step.


## 24. `arch/x86/x64/cpu.cc` → C via ctor→init — DONE (2026-07-25, commit 5493ad1)

The campaign's **first .cc unblocked and flipped**, and the first constructor conversion —
establishes the reusable mechanic for the core-object layer (scheduler/space/tcb next).

**The ctor→init mechanic (staged form):**
1. Ctor body → a C free function `T_init(T *self)` in the flipped .c; every `this->`/implicit
   member ref becomes `self->`.  (dump_features() → `T_dump(self)` the same way.)
2. In the header, keep the C++ ctor + method as **thin forwarders** guarded under
   `__cplusplus`: `T::T() { T_init(this); }`.  This means the CTORPRIO/`__ctors_*__`
   global-construction machinery and all `.method()` call sites keep working with **zero**
   changes to init.cc — the compiler-emitted global ctor just inlines the forwarder.
3. `SEC_INIT` moves from the ctor onto the free init function (an inline forwarding ctor
   can't carry a `section` alias — GCC: "section of alias must match section of its target").
4. Actually deleting the C++ ctor machinery (CTORPRIO globals → explicit init calls) is a
   Pass-C global sweep, not per-file.  Forwarding keeps Pass B green and low-risk.

**Two verification lessons:**
- *A clean C compile proves the member sweep is complete.* With no shadowing locals, any
  member missed by the `\bmember\b -> self->member` sed is an undeclared identifier and fails
  to compile. `apm_features` (missing from my member list) was caught exactly this way — so
  once it builds, every member ref is provably rewritten.  No need to eyeball 100+ refs.
- *sed rewrites member names inside string literals too.* Three labels at the start of printf
  formats (`"family` → `"self->family`) got clobbered — invisible to the compiler (still
  valid strings), caught only by reading the runtime dump.  Fix: `s/"self->/"/`.  **Lesson:
  for value-carrying flips, run the code and read the output, not just the build log.**
  (Verified: probe prints AuthenticAMD / "QEMU Virtual CPU" / family 15 / sse2 / lm, correct
  labels.)

Not byte-identical (362352 → 362280) — expected for a .cc→.c flip.  6 C files now: lib,
kmemory, mapping_alloc, ctors, hwspace(hdr), cpu.


## 25. threadstate.h → struct (2026-07-25, commit 32ce00f)

`thread_state_t` -> struct, byte-identical (362280), boots.  New sub-pattern worth naming:
**an enum-typed member whose enum must stay C++-only is stored as the enum's word-wide backing
type.**  Here `thread_state_e state` -> `word_t state` — and because `waiting_forever =
BLOCKED_STATE(~0UL)` forces the enum to 64 bits, the swap is byte-identical.  Enum + 3
implicit-conversion ctors + methods + `==`/`!=`/`word_t` operators guarded.

Frontier: no new CLEAN .cc.  Top gates now `api/v4/generic-archfpage.h` (**15**, the generic
flexpage header — jumped to #1) and `arch/x86/pgent.h` (8).  `generic-archfpage.h` is the
cheaper next step; `pgent.h` remains the 86-method wall.

Running pattern catalogue (for the core-object layer ahead):
- plain POD class -> struct + guard methods (KIP, most info types)
- static-only "namespace" class -> guard wholesale, no C struct (x86_mmu_t)
- value type -> struct dual-rep, then free-fn API + forwarders when deps are C-ready
  (spinlock_t, threadid_t)
- template fn -> __typeof__ macro (hwspace virt/phys)
- enum-typed member -> word-wide backing type (thread_state_t)
- constructor -> free `*_init(self)` + forwarding ctor; SEC_INIT moves to the free fn (cpu.cc)


## 26. generic-archfpage.h → struct (2026-07-25, commit ba40f47)

`arch_fpage_t` (generic stub flexpage: `word_t raw` + stub accessors) -> struct, byte-identical
(362280), boots.  fpage_t/tcb_t forward decls made dual (class in C++, struct in C).

Frontier: clearing this sub-header exposed its **parent** `api/v4/fpage.h` (`fpage_t`, ~34
methods / 2 classes) as the new top gate at **15** files; `arch/x86/pgent.h` (8) still the
other wall.  Pattern seen repeatedly now: peeling an included sub-header just surfaces the
including header (generic-archfpage.h -> fpage.h, like queuestate->threadstate->archfpage).
Two medium/large headers -- `fpage.h` (15) and `pgent.h` (8) -- now stand between here and the
first big wave of mapping/space/api flips.  `fpage.h` is the next step; both were the classes
guarded-off in the mapping.h work (§16), now coming due.


## 27. fpage.h → structs; pgent.h is now THE gate (2026-07-25, commit 7b7a85b)

`mempage_t` + `fpage_t` -> structs (unions C-visible; ~30 methods, static factories, and the
free `base_mask`/`address` helpers guarded).  Byte-identical (362280), boots.

**Frontier consolidation:** clearing fpage.h pushed its 15 files onto `arch/x86/pgent.h`, which
jumped **8 -> 21** and is now the single dominant gate over the ~31 remaining compiled `.cc`.
Everything else is a rounding error (segdesc.h 4, x64/syscalls.h 3, user.h 2, acpi.h 1).

So the header-clearing campaign has done its job: it funneled the whole remaining frontier onto
**one wall -- `pgent.h`**, the 86-method page-table-entry class deferred in §16.  This is the
next real job and a big one; converting it should cascade the mapping/space/glue cluster to
CLEAN in one shot.  Headers cleared to get here: KIP closure, cpu-family+types, hwspace,
queuestate, mmu, threadstate, generic-archfpage, fpage.  Value-type/dual-rep muscle is well
warmed up; pgent.h is where it pays off.


## 28. pgent.h + x86_pgent_t → structs; next wall is mdb.h (2026-07-25, commit 6218518)

The page-table-entry wall, done.  Two coupled classes dual-represented, byte-identical (362280),
boots:
- `x86_pgent_t` (ptab.h): the §16 whole-class `__cplusplus` guard replaced by dual-rep -- the
  pg4k/pg2m/raw bitfield union is now C-visible; enum + ~24 methods + `friend pgent_t` guarded.
- `pgent_t` (pgent.h): struct; `union{x86_pgent_t pgent; word_t raw}` C-visible; pgsize_e enum +
  ~50 methods guarded; mapnode_t/space_t forward decls dual.

**Next wall: `generic/mdb.h` -- and it's a different kind of wall.**  The same 21 files now
block on it.  It is 961 lines, **7 classes, zero CONFIG_NEW_MDB guards** (all unconditional),
and `mdb_t` has **18 virtual functions** -- the virtual-dispatch class the plan (§2) flagged as
genuinely hard.  BUT: mdb_t is never instantiated in this config (NEW_MDB off -> 0 vtables in
the linked kernel), and the mapping/space cluster needs exactly **one** thing from the header:
the nested value type `mdb_t::ctrl_t` (space.h: `fpage_t mapctrl(fpage_t, mdb_t::ctrl_t, ...)`,
"Even if new MDB is not used we need the mdb_t::ctrl_t").

So mdb.h is NOT a mechanical dual-rep like the last dozen headers.  Strategy to decide before
touching it:
- guard the virtual `mdb_t` body + the NEW_MDB-only classes (mdb_node_t/tableent_t/table_t,
  vrt_t, etc.) under `__cplusplus` -- C never needs mdb_t's layout since nothing instantiates it;
- expose only `ctrl_t` to C, which means hoisting the nested `mdb_t::ctrl_t` to a top-level
  C struct (`mdb_ctrl_t`?) with dual-rep, and teaching space.h's `mdb_t::ctrl_t` usage to
  resolve in both C and C++ (typedef / macro bridge).
This is a design call (nested-type hoist + a virtual class guarded wholesale), not a
copy-the-pattern job -- worth deciding deliberately rather than barreling in.


## 29. mdb.h C-includable (hoist ctrl_t/range_t, guard virtual mdb_t) — DONE (2026-07-25, commit e537b70)

The first *hard* header (virtual mdb_t) and the first *design* conversion rather than
copy-a-pattern.  mdb_t (18 virtuals) is never instantiated here (NEW_MDB off, 0 vtables), and
the mapping/space layer needs only the nested value type `mdb_t::ctrl_t`.

**New pattern -- nested-value-type hoist:** move a nested type out to a top-level dual-rep
struct (`mdb_t::ctrl_t` -> `mdb_ctrl_t`), then re-alias it inside the guarded parent with
`typedef mdb_ctrl_t ctrl_t;`.  C sees the top-level struct; C++ keeps `mdb_t::ctrl_t` working
via the typedef, so the ~18 call sites don't change and the C++ side is behaviourally
unchanged.  The whole virtual parent is then guarded wholesale (nothing in C needs its layout).
Cost: -8 bytes (the hoisted type's static methods/ctors mangle differently -- string table,
not codegen).  This is how the virtual/never-instantiated classes get handled generally.

Frontier: 21-file cluster moved mdb.h -> `glue/v4-x86/x64/space.h` (space_t + its own
`mdb_t::ctrl_t` use).  space.h is next; the bridge is now easy -- `mdb_t::ctrl_t` -> `mdb_ctrl_t`
where a C path needs it (space.h and the ~18 call sites, when those files flip).  Minor
independent gates unchanged: segdesc.h (4), x64/syscalls.h (3), user.h (2), acpi.h (1).

Header conversions this campaign (13): KIP closure, cpu-family+types, hwspace, queuestate, mmu,
threadstate, generic-archfpage, fpage, pgent+x86_pgent, mdb.  Plus 2 .cc flips (cpu, ctors).
The mapping/space core is now one header deep (space.h) from the big cluster flip.


## 30. x86_space_t (address-space base) → struct — DONE (2026-07-25, commit 6dfa1c3)

The kernel's most layout-critical class, and the first of the space_t chain.  x64/space.h's
x86_space_t dual-represented via the nested-value-type-hoist pattern (§29), **byte-identical**
(362272), boots.  kernel_pdp_t/top_pdir_t hoisted to x86_kernel_pdp_t/x86_top_pdir_t (needed
because `data` holds a `top_pdir_t*`); re-aliased with typedefs so external
`sizeof(top_pdir_t)` / `space_t::top_pdir_t` keep working.  `data` struct C-visible (all members
already C: x86_top_pdir_t*, atomic_t, fpage_t, word_t).

**Two gotchas for the C forward-decl bridge (both hit here):**
- `class X; #else struct X;` is not enough -- a bare `X *` in C needs the typedef too:
  `struct X; typedef struct X X;`.  (space_t used as `space_t *` in x86_top_pdir_t.)
- Missed forward decls (tcb_t/utcb_t) show up as `unknown type name 'class'` and cascade into
  spurious downstream errors; fix the first, re-scan.

Byte-identity is the proof for paging-layout classes: if the struct came out identical, the
hoist+re-alias preserved the exact ABI.

Frontier: x64/space.h done; 21-file cluster now at parent `glue/v4-x86/space.h`
(`class space_t : public x86_space_t`).  Next slice is the **single-inheritance -> embedding**
step (plan §4: base as first member), on top of a space_t class that itself has the mapctrl /
mdb_t::ctrl_t methods.  The space chain: x86_space_t (done) -> space_t [glue] -> generic
space_t [api] -- 2 headers left in the core.


## 31. glue space_t (inheritance -> embedding) — DONE (2026-07-25, commit 84e0d01)

Second link of the space chain.  `class space_t : public x86_space_t` -> dual: C++ keeps the
inheritance; C gets `struct space_t { x86_space_t base; }`.  space_t adds **no data of its own**
(pure methods), so single-inheritance -> base-as-first-member (plan §4) is trivial and
byte-identical (362272), boots.  Everything else (methods, ~15 OOL inline bodies, the SMP
`active_cpu_space_t` helper + extern, and reference-param/method-calling free functions) guarded
wholesale.

**Pattern -- single inheritance, no new data:** the whole class body splits with `#if/#else`
(C++ inheritance vs C `{ base_t base; }`); nothing else to model since the derived class only
adds behaviour.  When a derived class *does* add data, the C struct is `{ base_t base; <own
fields>; }`.

Frontier: 21-file cluster split onto `api/v4/space.h` (generic space_t, **14**) and
`generic/linear_ptab.h` (**7**).  Space chain: x86_space_t -> glue space_t (both done) ->
api/v4/space.h next (the generic/API space layer).  Down to ~2 core headers before the cluster
finally goes CLEAN.


## 32. api/v4/space.h done — space wall fully down; frontier is now TEMPLATES (2026-07-25, commit 4dba989)

Final link of the space chain: api/v4/space.h (extern space_t* globals + is_*_space free
predicates [kept C-visible, pointer compares] + OOL space_t:: method bodies [guarded]).
Byte-identical (362272), boots.  **The entire space_t chain -- x86_space_t -> glue space_t ->
api/v4/space.h -- is now C-includable.** This was the hardest, most layout-critical part of the
migration, landed across §30-32 all byte-identical.

New frontier, and it's a different animal -- **templates** (plan §4: templates -> concrete
structs / X-macros, not class dual-rep):
- `generic/bitmask.h` (**14**): `template<typename T> class bitmask_t` -- a T-wrapper with
  ctors + operators.  Only 3 instantiations in the tree: bitmask_t<u16_t>, <u32_t>, <word_t>.
  Approach: guard the template under __cplusplus, emit the 3 concrete C structs (X-macro or
  hand-written), typedef-bridge where used as struct members.
- `generic/linear_ptab.h` (**7**): a `template<typename T>` (the linear page-table walker) --
  needs the same treatment.
Minor independent gates unchanged: segdesc.h (4), x64/syscalls.h (3), user.h (2), acpi.h (1).

Milestone: 16 header conversions + 2 .cc flips this campaign; every value/KIP/space header now
C-includable.  The remaining walls (bitmask, linear_ptab) are templates -- mechanically
different but small (3 instantiations / 1 walker), and they're the last thing between here and
the first mapping/space/api .cc flips.


## 33. bitmask.h (template -> concrete structs) — DONE (2026-07-25, commit 86d3d26)

First template conversion (plan §4).  `template<typename T> class bitmask_t` guarded wholesale;
concrete aliases bitmask_word_t / bitmask_u32_t / bitmask_u16_t added -- in C++ a
`typedef bitmask_t<T>` (byte-identical, full operators), in C a plain `struct { T maskvalue; }`.
Two in-scope members bridged `bitmask_t<word_t>` -> `bitmask_word_t` (tcb_t::flags,
resources_t::resource_bits).  Byte-identical (362272), boots.

**Pattern -- class template used as a member:** guard the template; for each instantiation that
appears as a struct member, emit a concrete alias (typedef-to-template in C++, backing struct in
C) and switch the member declaration to the alias.  Cheap when instantiations are few (here 3,
one in-scope).

Frontier: bitmask.h done; 14 files now at `api/v4/resources.h` (resources_t), plus
`generic/linear_ptab.h` (7, the linear-ptab walker template) and the minor gates (segdesc 4,
syscalls 3, user 2, acpi 1).  Two template walls left in the core path: resources.h is a plain
class; linear_ptab.h is the second template.


## 34. resources.h (empty-base EBO) — DONE (2026-07-25, commit 71f6ee9)

resources_t layer, byte-identical (362272), boots.  generic_thread_resources_t (empty, methods
only) guarded wholesale; thread_resources_t (`: public generic_thread_resources_t` + data)
dual: C++ inheritance, C `struct { <fields> }` with the empty base dropped.  resource_bits_t
dual-rep (bitmask_word_t member C-visible).

**New sub-pattern -- empty base:** an empty base (no data) is 0 bytes by EBO, so the C struct
must *omit* it (not embed it as a first member -- an empty C struct would add padding and break
layout).  Contrast §31 (non-empty base -> first member).  Rule: non-empty base -> `{ base_t
base; ... }`; empty base -> just the derived's own fields, base guarded away.

Frontier: 14-file cluster (the tcb.h include chain) advances resources.h -> `api/v4/preempt.h`;
`generic/linear_ptab.h` (7) unchanged.  The cluster is walking down tcb.h's includes one class
per slice (bitmask -> resources -> preempt -> ...); tcb_t itself is the eventual big one.


## 35. Batched header pass toward the .cc flips (2026-07-25, commits 11e4657, 5dcdacf)

Switched from one-header-per-slice to batching: convert all currently-visible blocker headers,
build once, re-scan, repeat.  All byte-identical (362272), boots.

Headers this pass: linear_ptab.h (guard pgsize_e operators + readmem<T> template), api/v4/user.h
+ glue x64/syscalls.h + traphandler.h (extern "C" -> BEGIN_DECLS; static-only x86_exc_reg_t
guarded), glue/v4-x86/ipc.h (arch_ctrlxfer_item_t guarded), api/v4/ipc.h (msg_tag_t / msg_item_t
/ acceptor_t dual-rep; ctrlxfer_item_t is config-off), api/v4/syscalls.h (extern "C" block ->
BEGIN_DECLS; exregs_ctrl_t/schedule_ctrl_t guarded wholesale for now), x64/segdesc.h (3
descriptor classes dual-rep).

**Payoff reached: the first .cc closures went CLEAN** -- `api/v4/kernelinterface.cc` and
`api/v4/processor.cc` now have fully C-includable header closures (their bodies still need the
Pass-B flip: rename + body C-isms + Makeconf + linkage).

Remaining gates on the tcb.h cluster (23 files): `api/v4/sched-rr/ktcb.h` -- has a
`ringlist_t<tcb_t>` **template member** (needs the §33 bitmask-style concrete-type treatment);
`arch/x86/segdesc.h` (the parent, its own descriptor classes); `acpi.h` (1 file, ~10 ACPI-table
classes -- can be guarded wholesale, only acpi.cc uses them).  perl (not the Edit tool) is the
right tool for these tab-heavy multi-class headers -- anchor subs on class names / distinctive
trailing content, never on whitespace.

Tally so far: ~23 headers converted + 2 .cc flipped (cpu, ctors); 2 more .cc are CLEAN-closure
and ready to flip.  Next: ringlist_t concrete type -> ktcb.h -> the cluster cascades, then flip
kernelinterface.cc / processor.cc to actually bank .c files.


## 36. Flip kernelinterface.cc -> C (the KIP) — DONE (2026-07-25, commit 4165a6e)

First API-layer .cc flipped, and the ABI-critical one: the global Kernel Interface Page
definition + init.  Compiled by the C frontend, boots l4test (userland reads the KIP).  Not
byte-identical (362272 -> 362184).  7 C files now.

Mechanic (extends the cpu.cc ctor->init pattern to a whole file of methods):
- 4 OOL methods -> C free functions (processor_info_get_procdesc / memory_info_get_memdesc /
  memory_info_insert / kernel_interface_page_init), `this` -> `self`; C++ methods kept as thin
  forwarders in the header so all `.method()` call sites are unchanged.  A guarded enum param
  (memdesc_t::type_e) becomes word_t at the forward boundary.
- Rather than cascade into a memdesc_set free function, memory_info_insert **inlines** the one
  `md->set(...)` it needs (the _type/_low/... bitfields are C-visible).  Inline a single
  cross-class method call instead of converting the callee when it's used exactly once.
- `extern "C" { }` blocks -> BEGIN_DECLS/END_DECLS.  The KIP's GNU colon-designated aggregate
  initializer (`{string:{...}}`, `{raw: ...}`, SHUFFLEn) compiles unchanged under gcc's C
  frontend (gnu17) -- no need to rewrite it to C99 `.field =`.

**Linkage lesson (new):** flipping a file to C surfaces every C++ symbol it references by name.
kdebug_init / kdebug_entry (kdb .cc definitions, and kdebug_entry's debug.h declaration) had to
become `extern "C"` so the C file's unmangled references resolve.  Pattern: when a flipped .c
gets "undefined reference to <name>", make that symbol's definition + header decl extern "C"
(BEGIN_DECLS).

Remaining: processor.cc is the other CLEAN-closure file ready to flip; the 23-file tcb cluster
still gated on sched-rr/ktcb.h (ringlist_t<tcb_t> template member) + arch/x86/segdesc.h + acpi.h.


## 37. Flip processor.cc -> C — DONE (2026-07-25, commit 79f3dc1)

Second API-layer .cc (both CLEAN-closure files now flipped).  init_cpu becomes C: spinlock
method -> spinlock_lock/unlock free fns; get_procdesc -> processor_info_get_procdesc (the free
fn from §36); the two procdesc freq setters inlined to direct field writes.  init_cpu wrapped in
BEGIN_DECLS in cpu.h.  Boots ("Registering processor 0 in KIP (1000MHz, 3494MHz)").  8 C files.

Confirms the API-layer flip recipe is now routine: (1) header closure CLEAN, (2) body -> free
functions / inlined one-use method calls / spinlock+get_procdesc free fns, (3) BEGIN_DECLS the
functions this file *defines* that C++ still calls.  Next .cc flips wait on the tcb cluster
(ktcb.h ringlist template) or can pick any file whose closure is already CLEAN.


## 38. ktcb/utcb chain -> tcb.h (2026-07-25, commits bbc9e8d, fcc5d44)

Walked the 23-file tcb cluster down its include chain, all byte-identical (362168):
ringlist_t (types.h, concrete ringlist_tcb_t) -> rr_sched_ktcb_t -> sched_ktcb_t (sktcb.h) ->
arch_ktcb_t (empty member -> 1-byte C struct) -> utcb_t + generic-utcb.h.  Each was a routine
dual-rep / inheritance-split / template-concrete-type.

**Arrived at the final wall: api/v4/tcb.h (837 lines, the tcb_t thread control block).**  It's
the biggest and most ABI-critical class -- the TCB layout is read by hand-written asm and
scraped by Makefile.voodoo (tcb_layout.h) between TCB_START/END_MARKER.  Its data section
(lines ~300-356) has **8 interspersed private:/public: labels** and members of every type
converted so far (threadid_t, thread_state_t, resource_bits_t, queue_state_t, ringlist_tcb_t,
sched_ktcb_t, spinlock_t, lockstate_t, bitmask_word_t, arch_ktcb_t, thread_resources_t,
misc_tcb_t, utcb_t*, space_t*, tcb_t*) -- all now C-visible.  So the conversion is bounded but
intricate: guard the enums+methods block, guard each of the 8 access labels in the data section,
keep the data C-visible, guard the friends/static; byte-identity is the ABI proof.  Worth a
dedicated careful pass (no inheritance -- tcb_t is not derived).

Cluster is one header from cascading: tcb.h -> then re-scan should flip many api/glue .cc to
CLEAN.  segdesc.h (5, arch/x86 parent) and acpi.h (1) remain independent.


## 39. tcb.h (the thread control block) -- the keystone, cluster cascades (2026-07-25, commit 6965aff)

The biggest and most ABI-critical class in the kernel, converted byte-identical (362168),
boots.  Converting it **broke the 23-file logjam**: 5 files went CLEAN at once (mapping.cc,
asmsyms.cc, x64 exception/syscalls/user), the rest split onto small gates (smp.h 7, schedule.h
5, segdesc.h 5, intctrl.h/acpi.h 2, mdb_mem.h/tss.h/fpu.h 1).

**Tooling lesson: for a large intricate class, script it, don't hand-edit.**  tcb.h has ~180
in-class method decls, a nested typedef, 8 interspersed data access labels, in-class + trailing
friends, and ~90 out-of-line inline methods after the class.  A line-by-line state machine
(scratchpad/conv_tcb.py: pre -> class-open -> methods -> data -> regionB[friends] ->
regionC[tail]) did it cleanly and reproducibly; ad-hoc perl/Edit would have been error-prone.
Byte-identity confirmed the whole thing preserved the layout.

Sub-points worth keeping:
- A `typedef ... X;` nested inside a class is illegal in C -> hoist to file scope (perl move,
  no retype, to preserve layout exactly).
- Guard the data-section access labels *individually* (between the TCB markers); guard the
  enums+methods as one block before the markers; guard the trailing tail (externs, OOL methods,
  glue include, prototypes) wholesale after the class `};`.

Milestone: the entire value-type / KIP / space / tcb layer is now C-includable.  ~32 headers
converted + 4 .cc flipped.  The remaining gates are small and mostly independent (smp.h,
schedule.h, segdesc.h, intctrl.h, acpi.h, ...) -- the hard structural work is behind us; what's
left is a longer tail of routine header dual-reps and then the bulk of the .cc flips.


## 40. Gate-clearing after tcb.h (2026-07-25, commits a4801d7, and idt.h)

Post-keystone gate-clearing, all byte-identical (362168), boots.  Cleared: glue schedule.h
(method-call -> free-fn, no guard), fpu.h (static-only -> wholesale), intctrl.h (methods-only ->
wholesale), mdb_mem.h (NEW_MDB-off -> wholesale), tss.h + arch segdesc.h + idt.h (dual-rep;
data C-visible for their `extern` globals).

**.cc CLEAN count: 9** (init32, linear_ptab_walker, mapping, asmsyms, glue thread, x64
exception/syscalls/user, + more).  The tcb.h keystone plus this gate batch has the cluster
cascading steadily.

Bug caught: a perl close-guard that captured `};\n\n#endif` and re-emitted it duplicated the
class `}` (fpu.h) -- C++ build error, not a C-scan error.  Lesson: for wholesale guards, anchor
the closing `#endif` on the *include-guard* line and insert before it, rather than matching and
re-emitting the class close.

Remaining gates (larger, multi-class/template): api/v4/smp.h (cpu_mb_t/entry/sync + get_on_cpu<T>
template), api/v4/schedule.h (schedule_req_t + scheduler_t : policy_scheduler_t), pc99/82093.h
(IO-APIC classes), acpi.h.  Then the bulk of the .cc flips.

## 41. All header gates cleared -- every .cc has a C-clean closure (2026-07-26, commits 9cba3ef 5006c7c 881cf6c d0c2c81)

Finished the gate-clearing phase.  All byte-identical (362168), boots.  Guarded wholesale (all
C++-only, no C-visible declaration references them):
- api/v4/smp.h (SMP central-handler block + get_on_cpu<T>), api/v4/schedule.h (schedule_req_t /
  schedule_request_queue_t / scheduler_t : policy_scheduler_t -- depend on wholesale-guarded
  schedule_ctrl_t / cpu_mb_entry_t; the enum sched_flags_e + const sched_* flags stay C-visible).
- APIC/IO-APIC driver stack: apic.h (local_apic_t<base> template), 82093.h (ioapic_redir_t +
  i82093_t; ioapic_version_t stays C-visible), intctrl-apic.h (intctrl_t), glue intctrl.h
  (get_interrupt_ctrl).
- Misc driver/firmware overlays: generic-archmap.h (acceptor_t OOL method only), generic timer.h
  (base classes), glue timer.h (timer_t), amdhwcr.h (x86_amdhwcr_t static-only), rtc.h (rtc_t
  template), nmi.h (nmi_t), generic acpi.h (all ACPI table classes; acpi_remap/unmap externs stay
  C-visible), pc99 acpi.h (acpi_rsdp_t::locate), resource_functions.h (thread_resources_t OOL
  methods).

Pattern confirmed: a driver/overlay class used *only* via pointer or only inside .cc files needs
no dual-rep -- guard it wholesale.  Dual-rep (C-visible data) is required only when the type is a
by-value member or `extern` global that a C translation unit must see the layout of.

**Re-scan bug fixed (important):** the stub previously grepped every raw `#include` line, ignoring
`#if` context, so it pulled in x32comp headers that live behind `#if defined(CONFIG_X86_COMPATIBILITY_MODE)`
(OFF) -- a false blocker.  Fix: the stub now also carries `#if/#ifdef/#ifndef/#else/#elif/#endif`
directives, so CONFIG-gated includes evaluate correctly (CONFIG_* come from -imacros config.h).
x32comp is dead code in this config and needs no conversion.

**State: 0 blockers.** All 29 remaining .cc files (of 40 objects; the other 11 are already-flipped
.c + assembly) have a fully C-includable header closure.  The structural header layer is done.
Next phase is Pass B: flipping .cc bodies to .c -- the CLEAN list is the ready queue.

## 42. Pass B begins: first .cc flips -- asmsyms.c and idt.c (2026-07-26, commits 3d1da93 efe7cfb)

First two .cc->.c body flips.  Boots clean.  Byte-identity NO LONGER the universal proof:
a flip that changes the constructor mechanism necessarily changes the binary, so from here
correctness is proven by **boot test** (+ byte-identity where it still applies, e.g. headers).

**asmsyms.c** (mechanical): the body referenced class-scoped enum values
(`thread_state_t::polling`, `queue_state_t::wakeup`) whose enums are guarded C++-only.
Pattern for "class-scoped enumerator needed in C": hoist each value to a named macro
(`THREAD_STATE_*`, `QUEUE_STATE_*`) as the single source of truth, and have the C++ enum
*alias* the macros -- values byte-identical, both spellings work (`thread_state_t::polling`
in C++, `THREAD_STATE_POLLING` in C).  Build-system gotcha: `asmsyms` is built via SYMSRCS
in Mk/Makefile.voodoo, which globbed only `asmsyms.cc`; after the rename the flipped file was
silently NOT compiled (stale asmsyms.h reused -> falsely "byte-identical").  Fixed the glob to
match `asmsyms.c` too.  Lesson: after renaming a .cc that a Makefile references by explicit
name (Makeconf `SOURCES+=`, voodoo `SYMSRCS`), update that reference and force-regenerate.

**idt.c** (first CTORPRIO static-ctor flip -- user chose "explicit init"):
- `idt_t idt CTORPRIO(CTORPRIO_GLOBAL,3)` used a C++ static constructor (emitted as
  `.init_array.55532`, run by `call_global_ctors()`).  C has no ctors.  Chosen strategy:
  drop the static ctor, rename the ctor body to `idt_init(idt_t*)`, and call it **explicitly**
  from the boot path -- placed right after `call_global_ctors()/call_node_ctors()` in
  `startup_system()`, exactly where the ctor used to run (traced: BSP builds the IDT there,
  before the first `idt.activate()`; APs reuse the BSP-built global).
- Class methods -> C free functions (`idt_add_gate`, `idt_activate`) declared in BEGIN_DECLS;
  the C++ methods become INLINE **forwarders** (`void activate(){ idt_activate(this); }`) so
  unflipped C++ callers (init.cc, intctrl-apic.cc) keep `idt.activate()` working.
- Nested descriptor types needed C APIs: added `x86_idtdesc_set()` (x64 segdesc.h) and
  `x86_descreg_set/setdescreg()` (segdesc.h) as `#if !defined(__cplusplus)` static inlines,
  plus value macros (`X86_IDTDESC_*`, `X86_DESCREG_*`, `IDT_TYPE_*`).  The C++ methods are left
  UNTOUCHED (their C++ users -- init32.cc, x64/init.cc -- stay byte-identical); only NEW C-only
  free functions are added alongside.  Pattern: "C-only free-fn API beside the C++ methods"
  when the type still has live C++ callers you don't want to perturb.
- Makeconf `SOURCES+=` list updated idt.cc->idt.c.
- Size 362168 -> 361608 (expected: the synthesized static-ctor wrapper + .init_array entry gone).
- **-Wconversion is stricter in C than C++**: the identical bitfield/index code was clean as
  C++ but warned as C.  Fixed faithfully: `word_t` loop index instead of `int`; cast masked
  operands to `u64_t` (`((u64_t)ist)&0x7`) so GCC sees the value provably fits the bitfield;
  split chained `res0=res1=0`.  Always diff warnings against the pre-flip C++ file
  (`git show HEAD:...cc | gcc -x c++ -fsyntax-only`) to tell regressions from pre-existing.

Ready queue now 27 .cc CLEAN (asmsyms/idt flipped out of the set).  0 blockers.

## 43. tcb C-API foundation + more Pass B flips (2026-07-26, commits 08aa098 ee8acd9 8c88382)

Flipped x64/user.cc (pure extern-"C" inline-asm syscall stubs; drop extern "C", add explicit
#include <tcb_layout.h> since glue tcb.h -- which used to pull it in -- is now C++-guarded in
api tcb.h; byte-identical).

Then the foundational piece the rest of the queue needs: **expose the glue tcb layer to C**.
Nearly every remaining .cc calls get_current_tcb() and tcb accessors, but the glue tcb headers
were reachable only in C++ (api/v4/tcb.h wrapped `#include INC_GLUE(tcb.h)` inside the big
__cplusplus tail-guard from the keystone work, and glue tcb.h + glue x64 tcb.h are almost all
tcb_t:: OOL methods).
- api/v4/tcb.h: split the tail guard so INC_GLUE(tcb.h) is included in BOTH C and C++ (close the
  guard before the include, reopen after -- a no-op for C++, so byte-identical).
- glue/v4-x86/tcb.h + glue/v4-x86/x64/tcb.h: guard the tcb_t:: OOL methods (and the
  kdb/tracebuffer.h include, which has C++ classes) behind __cplusplus; leave get_current_tcb(),
  tcb_layout.h, initial_switch_to, and the C-safe includes visible to C.
Diagnostic trick used: `gcc -E` the header as C vs C++ and grep for a body-only token (e.g.
`leaq -8` from get_current_tcb's asm) to prove whether a header's body actually reaches C.

Then flipped x64/syscalls.cc (the dispatcher) as the first tcb-C-API consumer:
- threadid_t X.set_raw(v) -> threadid_set_raw(&X,v) (threadid_t's C API already existed); sys_*
  handlers are in BEGIN_DECLS and take threadid_t by value -> C calls them directly.
- get_current_tcb()->get_local_id().get_raw() -> tcb_get_local_id() temp + threadid_get_raw().
  Added tcb_get_local_id() as the first tcb_t C accessor free function (mirrors the method,
  which stays for C++; add more accessors here as C files need them).
- C-only -Wsign-conversion on `uip & ~(SYSCALL_ALIGN-1)` fixed with a `~(word_t)` cast.

**Pattern for adding a tcb accessor to C**: add `INLINE T tcb_<name>(const tcb_t*self){return
self-><member>;}` right after INC_GLUE(tcb.h) in api tcb.h (C-visible), leave the C++ method
alone (byte-identity). The big files (ipc.cc 166 tcb-calls, thread.cc 154, ...) will each need a
batch of these accessors as free functions before they can flip.

State: 24 .cc remain. All byte-identical / boot-verified. The tcb accessor free-fn API is now the
incremental unblock path for the core queue.

## 44. api/v4/smp.cc -> smp.c: a wider cascade (2026-07-26, commit b449c26)

smp.cc looked small (OOL=3) but cascaded across 6 files -- a good example of how a Pass B flip's
cost is set by how many wholesale-guarded types and C++-only helpers its body touches, not by line
count.

- Two config surprises: CONFIG_SMP_SYNC_REQUEST is defined in glue/v4-x86/x64/config.h (NOT the
  main config.h), so the "dead" synchronous XCPU half is actually LIVE. Always grep the glue
  config.h too, not just build/.../config.h.
- Dual-repped cpu_mb_entry_t, cpu_mb_t, and sync_entry_t (all were wholesale-guarded in smp.h from
  the §41 pass). sync_entry_t is single-inheritance from cpu_mb_entry_t -> whole-class #if/#else
  split with `cpu_mb_entry_t base;` first member in C; inherited-member access becomes `.base.x`,
  and an upcast `&e` (sync->mb) becomes `&e.base`.
- Method -> free-function rules of thumb: if a method has NO C++ callers (walk_mailbox except via
  process_xcpu_mailbox; all of sync_entry_t's), make it a pure C free function and delete the
  method. If it DOES (dump_mailbox via xcpu_request; cpu_mb_entry_t::set via cpu_mb_t::enter),
  keep the C++ method (or a forwarder) AND add the C free function beside it.
- Default-arg decls can't go in BEGIN_DECLS as-is: sync_xcpu_request needs its C++ default-arg
  declaration for space.cc, so split `#if __cplusplus <defaults> #else <no defaults> #endif`
  inside BEGIN_DECLS.
- Two more C-linkage exposures the body forced: get_kdebug_tcb() (moved from a __cplusplus block
  into debug.h's BEGIN_DECLS -- had to write `struct tcb_t *` since the tcb_t typedef isn't
  visible that early, only the types.h forward `struct tcb_t;`), and spin() (glue debug.h; added a
  C branch without the default arg, C callers pass spin(pos, 0)). Both are non-byte-identical
  (get_kdebug_tcb's symbol demangles).

Result non-byte-identical (361608 -> 361816). Boots, AP startup exercised. 24 .cc remain.

## 45. x64/exception.cc -> exception.c: static-member arrays + enum hoist (2026-07-26, commit dfbc440)

A static-data file (exception-frame register tables + one asm trap stub). Two reusable patterns:

- **Class-scoped enum needed in both C and C++** (again -- cf. threadstate/queuestate §42): hoist
  each enumerator to a namespaced macro (X86_EXC_*), C++ enum aliases them. Watch case-collisions:
  reg_e had Dreg/dreg and Breg/breg (differ only by case) -- macros are case-sensitive but the
  confusion is real, so those pairs use register-name macros (X86_EXC_RDIREG vs X86_EXC_RDXREG)
  instead of case tricks.
- **Static class-member array defined in the .cc**: a C file can't define a C++ `Class::member`.
  Convert it to a plain file-scope global (`x86_exc_reg_mr2reg`, `x86_exceptionframe_{name,dbgreg}`),
  declare it `extern const ...` in the header (the `extern` also gives it external linkage in C++,
  which a bare file-scope `const` would NOT have), and repoint the consuming C++ inline methods
  (mr()/reg(), dump()) at the global. The .c then defines the global. Callers of the methods are
  unchanged.

Non-byte-identical (table symbols change): 361816 -> 361808. Boots; the CONFIG_DEBUG dump() path
(kdb) links against the new globals. Also mirrored the definition-header changes into the unbuilt
x32/exception.cc for consistency. 23 .cc remain.

## 46. glue/v4-x86/cpu.cc -> cpu.c: the deepest cascade yet (2026-07-26, commit a96dc75)

Three foundational pieces in one flip; each is a reusable unlock:

1. **EXTERN_C helper (macros.h)**: a macro that embeds `extern "C"` is a C syntax error. Added
   `#define EXTERN_C extern "C"` (C++) / empty (C) -- the single-declaration companion to
   BEGIN_DECLS/END_DECLS (which are block form and can't sit inside another macro). Switched the
   shared exception-handler macros X86_EXCNO_ERRORCODE / X86_EXCWITH_ERRORCODE (x64 + x32
   trapgate.h) to it. C++ expansion is unchanged, so all existing handlers are byte-identical.

2. **Static data member used tree-wide -> global**: cpu_t::descriptors/count are read as
   `cpu_t::count` in ~8 files (src + kdb). A C file can't define `Class::member`, so convert to
   globals (cpu_descriptors/cpu_count) and sweep ALL callers -- including kdb/ (missed it the
   first pass -> a compile error) and the unbuilt powerpc/sched-hs/x32 for consistency, since
   api/v4/cpu.h is shared. Watch the constructor: cpu_t() set id=~0UL (invalid marker); the C
   global array must reproduce that with a designated init `{[0 ... N-1] = { ~0UL }}`, else the
   zero-initialised ids read as valid.
   - Keep methods with NO C caller (add_cpu) as inline C++ *with their body* (using the globals),
     NOT as a forwarder to a C function -- a forwarder would force every arch's cpu.cc to define
     that C function (would break powerpc's link). Only give a method a C free function when a C
     file actually calls it (get/get_id here, for cpu.c).

3. **Template instance -> C API**: cpu.c used a stateless `local_apic_t<APIC_MAPPINGS_START> apic;`
   (apic.EOI(), apic.send_ipi()). Templates can't exist in C. Added C functions (local_apic_eoi,
   local_apic_send_ipi) in apic.h that hardcode the fixed mapping and replicate the register
   writes (offsets/bit layout copied from the template's regno_t/command_reg_t, with a comment).

Also: C exception-frame access is `frame->__base.regs[X86_EXC_*]` (the C x86_exceptionframe_t has
a `__base` member where C++ inherits); init_xcpu_handling moved to glue smp.h's BEGIN_DECLS.

Non-byte-identical (361760 -> 361728). Boots; AP startup + XCPU IPI exercised. 22 .cc remain.

## 47. glue/v4-x86/timer-apic.cc -> timer-apic.c: thin C entry points for big classes (2026-07-26, commit cc5e9d0)

Eighth Pass-B flip. The APIC timer touches four subsystems (timer_t, the local APIC template,
the scheduler, and the interrupt controller), but none of the big classes had to be dual-repped:
each got one narrow C entry point instead.

- **Empty-base dual-rep**: timer_t derives from generic_periodic_timer_t, which has NO data
  members. Empty-base optimisation means the derived layout is just {bus_freq, proc_freq}, so the
  C `struct timer_t` needs no `base` member -- unlike sync_entry_t (§44), whose base carried data.
  init_global/init_cpu -> C free functions timer_init_global/timer_init_cpu(+self) with INLINE
  C++ forwarders; get_timer stays C++-only (no C caller).

- **Wrapper-in-.cc, not dual-rep, when the class is huge and only one method is needed**:
  scheduler_t and intctrl_t are large orchestrator classes. Rather than dual-rep them for a
  single call site, add a C-linkage wrapper *defined in an existing C++ TU* and declared in the
  header's BEGIN_DECLS:
    - sched_handle_timer_interrupt() in api/v4/schedule.cc wraps
      get_current_scheduler()->handle_timer_interrupt().
    - intctrl_has_pmtimer()/intctrl_pmtimer_wait() in platform/generic/intctrl-apic.cc wrap the
      intctrl_t methods.
  Behaviour is identical (handle_timer_interrupt was already an out-of-line call; get_current_*
  was a trivial cpulocal-global read now folded into the wrapper). Cost: one non-inlined call on
  the timer IRQ path -- negligible. This is the cheaper dual of "grow the header C-API": use it
  when the class stays C++ and the C side needs just a verb, not the layout.

- **Template method reuse**: the local-APIC C API from §46 (cpu.c) was extended in place with the
  timer registers (local_apic_timer_get/set/setup/set_divisor), each replicating one
  local_apic_t<base>::timer_* body against the fixed APIC_MAPPINGS_START. Second consumer of the
  same C API -- the pattern compounds.

- **Template-free rewrite instead of a wrapper**: wait_for_second_tick used rtc_t<0x70>. Rather
  than wrap it (no rtc.cc exists to host a C-linkage symbol), rewrite it with direct in_u8/out_u8
  on ports 0x70/0x71 and move it OUT of the __cplusplus guard -> a single definition valid in
  both languages. Prefer this when the templated helper is thin and self-contained.

- **file-scope const collision (the link error this flip surfaced)**: api/v4/schedule.h defines
  `const sched_flags_t sched_default = ...` etc. at file scope. C++ gives file-scope const
  INTERNAL linkage, so no symbol escaped; in C the same line has EXTERNAL linkage, so once TWO C
  TUs include schedule.h (smp.c + timer-apic.c) the linker sees duplicate `sched_rplywt` &c. Fix:
  add `static` -- a no-op for C++ (already internal), forces internal linkage in C. GCC does not
  warn about unused static const at file scope, so no fallout. General rule for headers going
  C-visible: any bare file-scope `const X y = ...;` must become `static const` before a second C
  TU includes it.

Non-byte-identical (361728 -> 361496). Boots (SMP, idle thread up). 21 .cc remain.

## 48. glue/v4-x86/resources.cc -> resources.c: __asm__-label method flip + accessor batch (2026-07-26, commit b9e41a4)

Ninth Pass-B flip. thread_resources_t (FPU state + IPC copy areas) was already
dual-repped and save/load already carried __asm__("tcb_resources_save"/"...load")
labels because trap.S calls them by name. Two lessons stand out.

- **__asm__ label = zero-forwarder method->C conversion.** For a non-virtual
  method with C++ callers, give its *declaration* an __asm__("c_symbol") label,
  then define a plain C function `c_symbol(ClassT *self, ...args)`. Every C++
  `obj.method(args)` compiles to a call of `c_symbol` with `&obj` as the leading
  argument -- which is exactly the C function's first parameter. No forwarder, no
  mangled-name mismatch, and asm callers (trap.S) resolve to the same symbol. Did
  this for all six methods here (save/load already had labels; added purge/init/
  free/x86_no_math_exception/release_copy_area). Cheaper than the inline-forwarder
  pattern when the class is dual-repped and the method leaves the header anyway.
  Caveat: it de-inlines (release_copy_area was INLINE) -- kernel grew 361496->361720.

- **tcb_t data members are already C-visible; only its methods are guarded.** The
  keystone tcb_t isn't flipped, but its struct body declares all data members
  outside `#if __cplusplus` (only methods/enums/friends are inside). So a C file
  reads tcb->resource_bits, ->partner, ->misc.saved_state[l].partner, ->space,
  ->cpu, ->pdir_cache, ->resources.fpu_state directly -- no accessor needed for
  plain field reads. Only *behaviour* (get_tcb address math, space_t methods)
  needs C entry points. This is why "flip a leaf that uses tcb_t" is tractable
  well before tcb_t itself flips.

- **Accessor batch (reused by the coming thread.cc/space.cc):** resource_bits_*
  (poke bitmask_word_t::maskvalue, param typed word_t not resource_type_e to
  dodge the api<->glue resources.h include cycle that defines the enum *after*
  including the accessors); x86_fpu_* and x86_mmu_* (C mirrors of the static-only
  holder classes, same asm bodies); tcb_get_tcb (INLINE, dynamic-KTCB branch);
  and space_{populate,delete}_copy_area / {get_top_pdir_phys,alloc_cpu_top_pdir,
  has_cpu_top_pdir} as BEGIN_DECLS wrappers in space.cc (space_t stays C++;
  get_top_pdir_phys returns word_t so C needn't know x86_pgent_t).

- **Include-cycle gotcha:** api/v4/resources.h includes glue resources.h, which
  includes api/v4/resources.h *before* defining `enum resource_type_e`. So an
  INLINE in api/v4/resources.h must not name resource_type_e (it isn't defined on
  the glue-first include path). Typing the param word_t sidesteps it -- the enum
  constants (FPU/COPY_AREA/...) pass through as ints fine.

- **Dead-branch discipline:** CONFIG_X86_SMALL_SPACES / FPU_REENABLE /
  CONFIG_X_X86_HVM are off here; their bodies are dropped-with-a-note rather than
  ported (HVM's tcb->get_arch()->disable_hvm() has no C form yet). IS_SPACE_GLOBAL
  is a C++-only macro that is `false` in this config, so release_copy_area flushes
  the TLB with a literal false and a comment.

Non-byte-identical (361496 -> 361720; de-inlining). Boots; save/load exercised on
every context switch via trap.S. 20 .cc remain.

## 49. linear_ptab_walker.cc -> C: the mapping engine, staged (2026-07-26, commits 1a28bba 97cab43 e9919a8)

The hardest file in the kernel (map_fpage calls itself "the single most
algorithmically complex part of the kernel"). Done in three staged commits so
each was independently verifiable -- the pattern to reuse for any high-risk flip.

- **Step 1 (1a28bba) -- pgent_t C-API + pgsize_e hoist.** Hoisted the pgsize_e
  enumerators to X86_PGSIZE_* macros (byte-identical: linear_ptab_walker.o
  rebuilt bit-for-bit against HEAD) and added a 16-function pgent_t C API as thin
  delegating wrappers in space.cc. space_t/mapnode_t have no C typedef in
  pgent.h, so wrapper decls use elaborated `struct` pointers.

- **Step 2a (97cab43) -- the rest of the C-API batch (~40 entry points).** fpage_t
  C API + base_mask/address; the linear_ptab.h page-geometry helpers reimplemented
  as C inlines (word_t pgsize) in a !__cplusplus branch; space_t wrappers (pgent,
  begin/end_update, is_mappable, get_kip/utcb_page_area, sigma0_*, flush_tlb*,
  readmem_phys, release_kernel_mapping); mdb_map_c/mdb_flush_c C-linkage wrappers.
  Again byte-identical for existing code; only new unused symbols. Gotchas:
  mapping.h is included by plain C files (mapping_alloc.c) and only forward-decls
  `struct pgent_t` for C, so the mdb wrapper decls must use elaborated structs and
  pass fpage_t by pointer (no complete type needed); addr_offset/addr_mask already
  had C forms.

- **Step 2b (e9919a8) -- the translation.** map_fpage/mapctrl/readmem -> C free
  functions via __asm__ labels on their space.h declarations (fpage_t/mdb_ctrl_t
  pass by value, ABI identical). Key hazard: lookup_mapping's out-param is a 4-byte
  pgent_t::pgsize_e; an asm-labelled C symbol writing word_t (8 bytes) would
  corrupt its many external callers' stacks. So lookup_mapping stays C++ (moved to
  space.cc) with a word_t-bridging wrapper (space_lookup_mapping_c) for the C
  readmem. pgsize_e locals become word_t and the enum ++/--/+/- operators become
  plain unsigned arithmetic (loops are bounded -> no underflow). mdb_ctrl_t is
  already dual-repped so mapctrl takes it by value in C; its string() method is
  reimplemented as a small static C helper for the tracepoint.

- **Verification beyond boot.** For a page-table file, "it boots" is weak proof
  (silent corruption). Drove l4test interactively under QEMU (-serial stdio, feed
  menu digits): Memory (Page touch), Sigma0 (memory request) and IPC (untyped
  transfers) all OK. The lone FAILED subtest -- "Local destination Id", about
  thread-local IDs, not page tables -- reproduces identically on the pre-flip 2a
  kernel, proving no regression. This "run the app's own test suite and diff the
  failure set against the pre-flip build" is the right bar for risky flips.

Non-byte-identical (367864 -> 363752). Boots + mapping suites pass. 19 .cc remain.

## 50. mapping.cc -> C: the mapping database, staged (2026-07-26, commits b425449 b405766)

The old MDB (mapping-database) core -- the other half of the mapping engine
(linear_ptab_walker §49 is the page-table half). Same silent-corruption risk,
same staged approach.

- **Self-contained flip.** mapping.cc is the ONLY built consumer of the
  mapnode_t/rootnode_t methods -- mdb.cc/mdb_mem.cc are CONFIG_NEW_MDB (off,
  unbuilt) and mdb_io.cc is CONFIG_X86_IO_FLEXPAGES (off). And its external
  entry points (mdb_map/mdb_flush/init_mdb/sigma0_mapnode) were already bridged
  by the walker work. So the whole flip touches only mapping.{cc,h} plus a
  handful of new pgent/fpage wrappers.

- **Step A (b425449) -- node methods as C inlines.** The ~34 mapnode_t/
  rootnode_t methods are small bit/pointer ops on C-visible packed bitfields,
  reimplemented in mapping.h's !__cplusplus branch. Byte-identical for C++
  (mapping.o unchanged vs HEAD). The C++ overloads (get_pgent, set_backlink,
  set_next, set_ptr) become distinctly-named C functions. **Critical trap:**
  the packed pointers live in wide bitfields (prev_ptr:63, next_ptr:62,
  space:53); in C, GCC promotes those to a *narrower int* before the shift and
  truncates the pointer (C++ integral promotion keeps the 64-bit declared
  type). Fix: cast the bitfield to word_t before shifting -- caught by
  -Wint-to-pointer-cast, which is exactly the review signal to watch for on any
  wide-bitfield-to-pointer reimplementation.

- **Step B (b405766) -- the translation.** init_mdb/mdb_map/mdb_flush/helpers/
  remove_map/create_* to C. mdb_map/mdb_flush become file-static; the 2a
  mdb_map_c/mdb_flush_c wrappers become trivial pass-throughs (keeps the
  verified walker.c untouched). init_mdb moves to BEGIN_DECLS so init.cc (C++)
  resolves the C symbol (a C++ mangled-name link error flagged this). Overloads
  are chosen by argument type at each call site. New wrappers: pgent_vaddr/
  reference_bits/reset_reference_bits/update_reference_bits/revoke_rights/flush,
  fpage_is_rwx.

- **Verification.** mapping.c -Wconversion clean; boots; l4test Memory + Sigma0
  pass; and "All tests" yields a failure set byte-identical to the pre-flip
  build (same pre-existing, page-table-unrelated "Local destination Id"
  failure on both). Diffing the test-suite failure set against the pre-flip
  kernel is the standing bar for these mapping-engine flips.

Non-byte-identical (363752 -> 363712). 18 .cc remain.

## 51. api/v4/space.cc -> C: the first orchestrator, staged A/B1/B2 (2026-07-26, commits 1ccc1fd ffb549a 19bad20)

The first tcb-coupled orchestrator. Unlike the self-contained mapping files, it
drives ~18 tcb_t methods, the scheduler, xcpu, time_t, the KIP and the syscall
machinery -- so it needed a real accessor batch first. Three commits.

- **Step A (1ccc1fd) -- reusable tcb_t + scheduler C-API.** tcb_t data members
  are private in C++ but plain fields in C, so member reads (cpu, myself_global,
  partner, space, thread_state.state, flags) become C-only INLINE accessors in
  tcb.h; the non-trivial methods (get_mr, notify, send_pagefault_ipc, ...) are
  wrappers in thread.cc (the tcb implementation file, still C++). scheduler ->
  sched_schedule/sched_get_current_time in schedule.cc. This batch is what
  thread.cc/ipc.cc/schedule.cc will all reuse.

- **Step B1 (ffb549a) -- the space-specific glue.** ~12 space_t method wrappers,
  fpage extras, xcpu_request_c, time_t C-forms (is_zero/is_never inline;
  get_microseconds/operator< wrapped in C++), KIP size accessors. All additive
  and byte-identical for C++ (space.o unchanged vs HEAD).

- **Step B2 (19bad20) -- the translation.** handle_pagefault/free via __asm__
  labels; the two syscalls stay syscall-shaped because SYSCALL_ATTR is empty on
  x64 and sys_space_control/sys_unmap are ALREADY called from the C syscall
  dispatch (x64/syscalls.c) -- flipping the definitions to C aligns the linkage
  rather than breaking it.

Key lessons (reusable for thread/ipc/schedule):
- **enum out-params / enum-typed params need the exact width.** access_e is a
  *signed* enum (readwrite=-1); handle_pagefault takes it as `int`, not word_t,
  so the ABI matches its C++ callers -- same trap as lookup_mapping's pgsize_e.
- **hoist the C++ enums the file names.** access_e -> SPACE_ACCESS_*,
  thread_state_t::X was already THREAD_STATE_* (asmsyms era); the C side uses the
  macros.
- **overloaded no-arg vs word_t methods** need distinct wrappers: fpage set_rwx()
  (sets mem.x.r/w/x bits) is NOT set_rwx(7), so fpage_set_rwx_all is separate.
- **dual-repped value types pay off:** time_t/mdb_ctrl_t/thread_state_t all carry
  C-visible data, so predicates reimplement in C and only the arithmetic-heavy
  methods (time_t::get_microseconds/operator<) need C++ wrappers.

Verified: handle_pagefault runs on every demand-paged page during boot;
l4test "All tests" failure set identical to the pre-flip kernel. 17 .cc remain.

## 52. api/v4/thread.cc -> C: the last keystone, staged over many turns (2026-07-26, commits ab27644..995b205)

The largest and hardest file in the tree: 1658 lines, defines 19 tcb_t methods,
calls ~90 distinct methods, assembly-coupled (initial stack, notify, xcpu). Done
as a big C-API buildout (Stage A, ~8 commits) then one atomic translation.

Why it had to be atomic (learned the hard way): the moment you add the __asm__
labels the build breaks -- the asm-named methods collide with the hosted C-API
wrappers -- and it stays broken until thread.c + wrapper-migration + linkage all
land together. There is no partial commit. A first attempt was reverted after
hitting this; the second did the whole coordinated change in one run.

Stage A -- the C-API (reused by every api/v4 file):
- Data types are almost all dual-repped/C-visible (tcb_t members, utcb_t,
  arch_ktcb_t, thread_state_t, sched_ktcb_t, msg_tag_t, acceptor_t, timeout_t,
  queue_state_t, time_t), so the C form accesses members directly and needs C
  forms only for *methods*. Predicates on dual-repped value types reimplement as
  C inlines (thread_state_is_*, msg_tag_*, queue_state_*, time_is_*); the
  arithmetic-heavy or arch/utcb methods get wrappers in glue/v4-x86/thread.cc
  (do_ipc, get/set_tag, user_ip/sp, return_from_ipc, ...).

Stage B -- the translation. Key gotchas, each a reusable lesson:
- **friend-declared free functions** (handle_ipc_error) can't be naively
  extern-C'd: declare the extern "C" prototype *before* the class so the friend
  decl refers to it; then C++ callers (exregs.cc) and the C definition agree.
- **enum tags need a typedef in C** (sktcb_type_e): `typedef enum X X;` -- C,
  unlike C++, has no implicit type name for an enum tag.
- **enums the file uses must be hoisted to macros** (unwind_reason_e ->
  TCB_UNWIND_*, access_e -> SPACE_ACCESS_*); watch identifier collisions
  (`abort`/`timeout` are also std names -- word-boundary replace only).
- **C++-only helper macros** need C forms: TID = `(x).get_raw()` -> redefine as
  `threadid_get_raw(&(x))`; `max()` was C++-only -> inline it.
- **C++-only inline free funcs** used bare (get_idle_tcb/get_dummy_tcb) need the
  _c wrappers even inside their own former file.
- **whole_tcb_t** (the padding union for KTCB allocation) had to be hoisted out
  of the __cplusplus guard.
- thread.c carries C prototypes for every asm-named function it defines or calls
  (the C++ method decls are invisible to C).

Verified beyond boot: creates sigma0 + root task (the full activate/create/
schedule/arch-init lifecycle), and l4test "All tests" matches the pre-flip
failure set exactly. Non-byte-identical; kernel 363192. 16 .cc remain.

## 53. api/v4/interrupt.cc -> C: first post-keystone orchestrator (2026-07-26, commits d906962 81f0e17)

interrupt.cc (360 lines: handle_interrupt, thread_control_interrupt,
irq_thread, migrate_interrupt_start/end, init_interrupt_threads). First flip
after the thread keystone, and notably *smooth* precisely because it defines
only free functions -- no tcb_t methods -- so there is NO asm-name atomic-flip
constraint. Reused the broad tcb/scheduler/threadid/thread_state/msg_tag C-API
from thread.cc; only a thin step-A foundation was new.

Step A foundation (d906962, pure additions, boots):
  - intctrl.h + intctrl-apic.cc: 8 C entry points wrapping the intctrl_t
    methods the interrupt path uses (get_number_irqs, is_irq_available, mask,
    unmask, enable, disable, is_pending, set_cpu), beside the pmtimer wrappers.
  - schedule.{h,cc}: sched_schedule_interrupt(irq, handler).
  - tcb.h: tcb_set_partner / tcb_set_irq_handler / tcb_get_irq_handler C
    inlines. NB: the IRQ handler tid is stored in the sched-ktcb *scheduler*
    field (set_irq_handler == sched_state.set_scheduler), not a dedicated slot.
  - ipc.h: msg_tag_irq_tag(); kernelinterface.h: thread_info_set_system_base()
    (12-bit field, base & 0xfff).

Step B flip (81f0e17):
  - Mechanical method->accessor translation. by-value threadid_t returns
    (get_global_id/get_partner/get_irq_handler) spilled to a local before
    taking &addr for threadid_equals/threadid_get_irqno.
  - scheduler->schedule(t) default arg is sched_default -> sched_schedule(t,
    sched_default); the sched_handoff/sched_default consts are already
    C-visible (u8_t, outside the __cplusplus guard).

Three reusable gotchas:

  (a) TRACE args are never compiled when the tracebuffer is off. TRACE_IRQ /
      TRACEPOINT ultimately expand through tbuf_record_event(args...), which
      CONFIG_TRACEBUFFER=off defines as an EMPTY function-like macro. A
      function-like macro that expands to nothing discards its argument tokens
      without rescanning them -- so C++-only args like
      `irq_tcb->get_state().string()` inside a TRACE_IRQ are never parsed as C.
      => keep all TRACE*/TRACEPOINT lines verbatim in the C translation; do not
      spend effort C-ifying trace-only expressions. (Same applies to TID(x) in
      dead trace args.)

  (b) A callback defined in a still-C++ file, referenced from the new C file:
      do_xcpu_send_irq lives in schedule.cc. Giving its schedule.h decl C
      linkage (BEGIN_DECLS) makes the C++ *definition* C-linkage -- good -- but
      that decl sits inside schedule.h's big `#if __cplusplus` block, so C
      can't see it. Fix: carry a plain C forward decl in the consumer
      (interrupt.c) after its own smp.h include (for cpu_mb_entry_t). Header
      decl drives the definition's linkage; local decl feeds the C caller.

  (c) Duplicate prototype with the wrong linkage. handle_interrupt is declared
      in BOTH api/v4/interrupt.h and generic/intctrl.h. Flipping only the
      former to extern "C" made ipc.cc (which pulls in intctrl.h first) see a
      C++-linkage decl then a C-linkage one -> "conflicting declaration ... with
      'C' linkage". Any header re-declaring a now-C symbol must also move to
      BEGIN_DECLS. grep every header for the symbol before flipping.

Verified: builds 363400, boots (interrupt threads init, KIP system_base set),
l4test All-tests region byte-identical to the pre-flip reference (diff clean;
only the pre-existing "Local destination Id" FAILED). 15 .cc remain.

## 54. api/v4/exregs.cc -> C: ExchangeRegisters(), staged A/B (2026-07-26, commits ba08e80 9fbe95e)

exregs.cc (455 lines; ~70 dead behind CONFIG_X_CTRLXFER_MSG=off). SMP xcpu
request/reply/remote handlers + perform_exregs + the SYS_EXCHANGE_REGISTERS
syscall. Reused the tcb/scheduler/threadid/thread_state C-API.

Step A (ba08e80): exregs_ctrl_t class -> dual-rep struct (EXREGS_CTRL_*_FLAG
macros; union C-visible; methods under __cplusplus; exregs_ctrl_is_set/set C
inlines). 4 new tcb wrappers (get/set_user_flags, get/set_user_handle).
xcpu_request7 (7-param C wrapper -- the exregs handlers pass up to 7 mailbox
params, past xcpu_request_many's 4).

Step B (9fbe95e): mechanical translation. misc.exregs.* accessed directly
(C-visible union member). exregs_ctrl_t(word_t) ctor -> `c.raw = r;`.

Two gotchas worth keeping:

  (a) Restructuring a class that shared an outer `#if __cplusplus` block.
      exregs_ctrl_t opened a `#if __cplusplus` that ALSO wrapped the following
      schedule_ctrl_t etc. (one guard, matching #endif far below). Dropping the
      opening `#if` to expose exregs_ctrl_t to C silently un-guarded everything
      after it. Fix: re-open `#if __cplusplus` right after the exregs_ctrl_t C
      block so the trailing types stay C++-only and the distant #endif still
      pairs. When you split a shared guard, always re-close/re-open around the
      one type you're exposing.

  (b) A return-path macro with C++-isms, taking the address of its arg. The
      return_exchange_registers macro (x64/syscalls.h) is expanded ONLY in
      exregs, so it was made language-neutral (pager.get_raw() ->
      threadid_get_raw(&(pager)); current->get_user_*() -> tcb_get_user_*()).
      Because it now does &(pager), the pager arg must be an LVALUE -- the
      error path passed threadid_t::nilthread() (an rvalue), so spill it:
      `threadid_t nil = threadid_nilthread(); return_exchange_registers(...,
      nil, ...);`. Same for the result word: pass threadid_get_raw(&local).

Verified: builds 363128, boots, l4test region byte-identical. 14 .cc remain.

## 55. api/v4/ipcx.cc -> C: extended IPC transfer, staged A/B (2026-07-26, commits 250266e 00efced)

ipcx.cc (459 lines): the non-untyped IPC transfer path -- string items (with
compound/substring copy loops) and map/grant items. Mostly free functions
(ipc_copy, copy_mr, extended_transfer). CONFIG_X_CTRLXFER_MSG off -> the
ctrlxfer branches are dead-#if'd. Broadest foundation so far because IPC touches
mapping + copy-areas; each new form is a one-liner though.

Step A (250266e): msg_item_t/acceptor_t already dual-rep structs, so just C
accessors over their unions (msg_item_is_*/get_*; acceptor_accept_strings/
get_rcv_window) + msg_tag_get_typed. Wrappers: space_get_copy_limit,
tcb_adjust_for_copy_area, arch_map_fpage_c, acceptor_get_arch_specific_
rcvwindow. space_t::map_fpage is already asm-named space_map_fpage (C prototype,
no wrapper).

Two points worth keeping:

  (a) Which arch-map definitions are active. ipcx pulls arch_map_fpage /
      get_arch_specific_rcvwindow via INC_GLUE(map.h) -> io_space.h. With
      CONFIG_X86_IO_FLEXPAGES OFF, io_space.h just #includes generic-archmap.h
      -- the nil/empty INLINE versions. glue thread.cc (the arch-wrapper home)
      also includes generic-archmap.h, so hosting the wrappers there resolves
      the SAME definitions => behavior-identical. Always confirm which of two
      competing definitions the target's include chain actually selects before
      picking a wrapper home.

  (b) goto across C block-scope declarations. extended_transfer's overflow
      handling is `goto message_overflow` from deep in nested blocks. This is
      valid C: the label is at function (outermost) scope, so every goto jumps
      OUTWARD, exiting the inner scopes -- C only forbids jumping INTO a scope
      past a VLA, which never happens here. No restructuring needed.

Also fixed a latent bug this surfaced: interrupt.c (committed earlier) called
two asm-named tcb methods with no C prototype -> implicit declarations that
linked only by SysV-AMD64 luck. The earlier flip's warning grep hadn't included
"implicit declaration"; added the prototypes (commit a5d0c0a) and widened the
warning check.

Verified: builds 359168 (warning-clean), boots, l4test region byte-identical.
13 .cc remain.

## 56. api/v4/ipc.cc -> C: the IPC fast path (SYS_IPC), staged A/B (2026-07-26, commits 555c4d5 ed026bd)

ipc.cc (724 lines): transfer_message + 4 SMP xcpu handlers + the SYS_IPC
send/receive state machine -- the hottest path in the kernel (every IPC).
Mostly free functions, so no atomic asm-name constraint; the biggest foundation
so far but all one-liner forms.

Step A (555c4d5): msg_tag C inlines (get_label, is_propagated, set_propagated,
set_xcpu, clear_receive_flags); timeout_get_rcv/snd; lock_state_is_enabled/
is_active (over lockstate_t's C-visible flags union, generic/sync.h); tcb
wrappers (enqueue_send, copy_mrs, get_saved_partner); scheduler wrappers
(sched_remote_schedule, sched_schedule_two). tcb_sched_set_timeout(time_t)
already existed.

Step B (ed026bd) translation notes:

  (a) Cached-scheduler local dropped. The C++ cached `scheduler =
      get_current_scheduler()` and called `scheduler->schedule(...)` many times.
      The current scheduler never changes mid-syscall, so every use became the
      sched_* current-scheduler wrapper (schedule/schedule_two/remote_schedule/
      get_current_time) -- behavior-identical, no local needed.

  (b) Repeated by-value getters -> spill once. get_partner()/get_global_id()
      appear several times per condition; each returns threadid_t by value and
      C can't take &(rvalue) for threadid_equals. Spill to a local once before
      the condition. Two conditions needed a small `{ ... }` brace scope purely
      to hold the spill locals (gnu99 would allow mid-block decls without them,
      but the brace keeps the spill's lifetime obvious). Balance carefully --
      the fall-through send-completion code lives after the block.

  (c) return_ipc macro, rvalue args. Used only by ipc, so made language-neutral,
      but several call sites pass rvalues -- return_ipc(from_tcb->get_local_id())
      , return_ipc(current->get_partner()), return_ipc(NILTHREAD). The macro
      takes &from, so spill `from` to a local threadid_t inside the macro body
      first; that also collapses the original's double-evaluation of `from`.

Verified: builds 359208 (warning-clean), boots, l4test region byte-identical --
the Simple-IPC/Send/ReplyWait/Send-timeout/Receive-timeout subtests exercise
this state machine directly. 12 .cc remain.

## 57. glue/v4-x86/exception.cc -> C: x86 exception handling, staged A/B (2026-07-26, commits be8d1c9 61c3965)

exception.cc (551 lines, ~250 live): send_exception_ipc, the instruction-decode
fault handler, and the #GP/#UD/#NM/catch-all trap handlers. First flip of an
arch file that leans hard on the exception-frame internals -- but the plumbing
was already done (glue/v4-x86/x64/exception.c is C; x86_exceptionframe_t is
dual-repped; register indices are X86_EXC_* macros). Most config blocks are dead
(IO_FLEXPAGES/SMALL_SPACES/COMPAT/SUBARCH_X32/CTRLXFER/KDB), so the live surface
is small.

Key points:

  (a) Composition instead of inheritance in the C rep. The C++
      x86_exceptionframe_t : public x86_exceptionregs_t becomes
      `struct { x86_exceptionregs_t __base; }` in C, so every frame->regs[...] /
      frame->error becomes frame->__base.regs[...] / frame->__base.error. The
      register-name enums (ipreg, creg, ...) alias X86_EXC_* macros, so
      x86_exceptionframe_t::ipreg -> X86_EXC_IPREG. Two-step rewrite: field
      access AND enum name -- easy to do the first and forget the second.

  (b) A C++ template used as a free function. readmem<T>(space, vaddr, T*) has
      no C form; reimplemented as a local readmem_u8 over the asm-named
      space_readmem (direct access for kernel memory, checked read + byte
      truncate for user memory), matching the template body.

  (c) A conversion operator with no .raw. api_version_t/api_flags_t are bitfield
      structs whose only word_t form is `operator word_t()` (C++-only). Added
      api_version_to_word/api_flags_to_word C inlines reproducing the operator
      bodies; placed AFTER the struct typedefs in the header (a forward INLINE
      referencing the not-yet-defined type is an "unknown type name" error).

  (d) A symbol defined in the kdb subsystem. kdebug_check_interrupt lives in
      kdb/api/v4/tcb.cc (C++, mangled). Wrapping its debug.h decl in BEGIN_DECLS
      gives the definition C linkage (kdb tcb.cc includes debug.h) and keeps the
      other C++ callers consistent -- the same dup-decl-linkage lesson as
      handle_interrupt, now across the kdb boundary.

Verified: builds 359384 (warning-clean), boots, l4test region byte-identical
(the KIP-read lock;nop path through exc_invalid_opcode is exercised). 11 .cc
remain (schedule.cc + init.cc/debug.cc, plus the thread.cc/space.cc
wrapper-hosts that stay C++).

## 58. glue/v4-x86/debug.cc -> C: KDB glue, and converting a C++ static ctor (2026-07-26, commit 30aa92a)

debug.cc (164 lines, all under CONFIG_DEBUG): the per-CPU KDB control block
(class cpu_kdb_t), do_enter_kdebug/do_return_from_kdb, sync_debug, and the
#BP/#DB/#NMI KDB trap handlers.

The one hard part: a prioritized C++ static constructor.

  cpu_kdb was `cpu_kdb_t cpu_kdb CTORPRIO(CTORPRIO_CPU, 1);`. CTORPRIO expands
  to __attribute__((init_priority(65535-(30000+1)))) = init_priority(35534).
  This project's ctor machinery: the linker (generic/ctors.ldi) groups
  *(SORT(.ctors.3*)) -> __ctors_CPU__, .ctors.2* -> NODE, and .ctors.1* / .ctors
  / .init_array.* -> __ctors_GLOBAL__; call_{cpu,node,global}_ctors() walk those
  arrays. Modern GCC actually emits init_priority objects into
  .init_array.NNNNN (confirmed: the C++ debug.o had .init_array.35534), so in
  THIS build every init_priority ctor lands in __ctors_GLOBAL__ regardless of
  the CPU/NODE class -- the class only affects the numeric priority, i.e. the
  run order within call_global_ctors.

  C has no object constructors. The fix: turn the ctor body into a function
  cpu_kdb_ctor() with __attribute__((constructor(35534))). GCC emits a C
  constructor-attribute function into the SAME .init_array.<prio> section with
  the same priority number, so objdump shows cpu_kdb_ctor in
  .init_array.35534 -- byte-for-byte the same slot the C++ object used. It
  therefore registers into __ctors_GLOBAL__ at the identical position and runs
  at the identical point. cpu_kdb becomes a zero-initialised cpulocal struct.

  Verify empirically: objdump -h the old .cc object to read the exact
  .init_array.<N> section, then confirm the new .c object reproduces the same N.
  Don't trust the CTORPRIO arithmetic alone -- the section is the contract.

Everything else was routine: class -> struct + free functions on the cpulocal
global; a local debug_param_t mirror of debug.h's C++-only class (same layout,
passed by address to the kdb entry which owns its own copy); spinlock_unlock;
sync_debug drops extern "C" (already BEGIN_DECLS in arch/x86/sync.h). And KDB
still fully works (renders the prompt, runs commands), which is the real
end-to-end test of the ctor timing. 10 .cc remain.

## 59. glue/v4-x86/init.cc -> C: the boot orchestrator, staged A1-A3 + B (2026-07-26, commits b1fc872 055faf1 d07d4b7 2e6235c)

init.cc (578 lines): startup_system + init_cpu + the SMP AP-startup path -- the
boot code that drives every subsystem. The largest foundation of the migration
(~30 C forms across ~12 subsystems), so it was staged: A1 KIP value types, A2
space/scheduler/tcb, A3 arch (mmu/cpu/timer/intctrl/apic/tss/gdt/meminfo), then
B the flip. Most config blocks are dead here.

Four things specific to a broad boot orchestrator:

  (a) Choose the wrapper-host by what's still C++. Many subsystems are already C
      (cpu.c/timer-apic.c/resources.c/idt.c/kernelinterface.c), so their method
      wrappers can't live there. The C++ hosts are the remaining .cc: the
      local_apic_t<> TEMPLATE wrappers went in intctrl-apic.cc (which already
      instantiates it), and the tss-global + setup_gdt(x86_tss_t&) REFERENCE
      wrappers went in x64/init.cc (which owns both). A C++ template/reference
      never has to appear in the C file -- it's hidden behind a C wrapper in a
      file that still has the C++ type in scope.

  (b) The C-overload trap. C++ overloaded glue init_cpu(void) and api/v4
      init_cpu(cpuid,freq,freq); the latter is already C (processor.c), so once
      the glue one becomes C they collide on the symbol `init_cpu`. Renamed the
      file-internal glue one to static init_cpu_local(). Always scan a
      to-be-flipped file's own function names against existing C symbols.

  (c) Cross-file callee linkage is atomic with the flip. init.c calls several
      boot functions defined in x64/init.cc + kdb console.cc that were
      C++-mangled; giving them C linkage (BEGIN_DECLS) breaks the still-C++
      init.cc immediately, so the linkage edits + the .cc->.c rename + Makeconf
      must land in one build (same lesson as thread.cc's asm-name flip).

  (d) -Warray-bounds on absolute-address MMIO. The BIOS warm-reset-vector writes
      `*((volatile unsigned short*)0x469)` trip a GCC false positive in C
      (0-length array at a constant address) that C++ didn't emit; pointer
      variables don't help (constant-folded), so a targeted
      `#pragma GCC diagnostic ignored "-Warray-bounds"` around the two writes.

Verified: builds 359952 (warning-clean, zero implicit declarations kernel-wide),
boots (full init sequence), boottest PASS, l4test region byte-identical. 9 .cc
remain (schedule.cc + the thread.cc/space.cc wrapper-hosts).

## 60. api/v4/schedule.cc -> C: the scheduler, staged A1-A4 + B (2026-07-26, commits 03350bc c474a19 9230d40 6c673d1 da13b30)

schedule.cc (474 -> 401 lines after wrapper migration): the scheduler
orchestrator -- SYS_SCHEDULE, SYS_THREAD_SWITCH, the request queue, idle_thread,
scheduler_t::init/start.  The last real keystone, and the one whose difficulty
was most over-estimated.

The pivotal finding (Step-A planning): schedule.cc reads NO scheduler_t
instance data -- only the static request queue and method calls.  So
scheduler_t : public policy_scheduler_t (= rr_scheduler_t) stays an opaque C++
class with its inheritance intact; this is a normal opaque-class wrapper/asm-name
flip like tcb.cc/space.cc, NOT the inheritance-flattening de-classing first
feared.  Lesson: before assuming a C++ inheritance hierarchy must be flattened,
check whether the file being flipped actually touches instance data or only
calls methods / uses statics.

Staged: A1 request/control data types dual-repped; A2 storage types
(prio_queue_t/rr_scheduler_t/scheduler_t) dual-repped so the `scheduler` global
can be *defined* in C -- scheduler_t's C rep is struct { policy_scheduler_t
__base; } (empty derived class has the base layout); A3 the 6 policy-method
wrappers in the permanent C++ host sched-rr/schedule.cc; A4 migrated the 24
sched_* wrappers there too and added the request-queue C forms; B the flip.

Flip-specific points:
  - Only the two methods with external callers (init/start, reached via
    sched_init/sched_start) got asm-names; add/process_schedule_requests have no
    external callers and became plain file-static C functions -- no asm-name
    needed.
  - static class member -> global: scheduler_t::schedule_request_queue became a
    plain extern global; the C++ schedule_requests_pending inline still finds it
    by unqualified name.  Its zero-only ctor => C zero-init.
  - protected method inlined: policy_scheduler_init (2 lines) inlined into
    init() against the C-visible __base rather than exposed.
  - a class value type with no C ctor (schedule_req_t): set the fields that the
    C++ default ctor would have (req.valid = false), since C won't run it.
  - a method defined in the flipped file but used elsewhere (time_t::operator<):
    move its definition to a C++ host (glue thread.cc, beside its time_lt
    wrapper) rather than trying to express operator< in C.

Verified: builds 355888 (warning-clean, zero implicit declarations kernel-wide),
boots (scheduler init/start/idle all run), boottest PASS, l4test byte-identical
-- every subtest exercises the scheduler.

MILESTONE: the entire api/v4 layer is now C (thread, ipc, ipcx, exregs,
interrupt, schedule, space, mapping, kernelinterface, smp, processor).  The only
built C++ that remains is glue/v4-x86/x64/init.cc (sub-arch GDT/TSS init) and the
two wrapper-hosts glue/v4-x86/thread.cc + space.cc, which bridge the still-C++
tcb_t/space_t/scheduler_t methods and would flip only by de-classing those types.

## 61. glue/v4-x86/x64/init.cc -> C: sub-arch GDT/TSS init, staged A1/A2/B (2026-07-26, commits 817244e cdad54e 99d041d)

x64/init.cc (301 lines): x86-64 segment-descriptor / GDT / TSS init + CPU
features + MSRs.  Dense arch C++ (segdesc/tssdesc/descreg methods) but the
descriptor types were already dual-repped (unions C-visible), so only their
methods needed C forms.

Foundation: A1 x64/segdesc.h -- X86_SEGDESC_* enum macros + x86_segdesc_set_seg
(5-arg u64) + x86_tssdesc_set_seg + arch/x86/segdesc.h x86_descreg_set_sel /
setselreg; A2 x86_tss_setup (tss.h) + x86_fpu_enable_osfxsr (fpu.h).

Flip points specific to arch/descriptor code:
  - descriptor "local ctors" -> declare + init-call: `x86_descreg_t r(a,b)`
    becomes `x86_descreg_t r; x86_descreg_set(&r, a, b);`.  set_seg's default
    args have no C equivalent, so the 4-arg calls pass X86_SEGDESC_MSR_NONE
    explicitly.
  - a reference parameter defined-and-used only locally: setup_gdt(x86_tss_t&)
    -> (x86_tss_t*); its only caller (setup_gdt_c) passes &tss.
  - the second of two CTORPRIO objects was a no-op: objdump showed only ONE
    .init_array entry (boot_cpu_ft), because x86_x64_tss_t has no constructor --
    so tss just becomes zero-init and only boot_cpu_ft needed the
    __attribute__((constructor(55534))) conversion (same slot, verified).
  - member read instead of getter: boot_cpu_ft.get_l1_cache().d... -> the
    C-visible boot_cpu_ft.l1_cache.d... directly (the getter only returned it).
  - C++ function-style cast unsigned(x) -> (unsigned)(x).

Verified: builds 355864 (warning-clean, zero implicit declarations kernel-wide),
boots through GDT/TSS setup + the segment reload (a wrong descriptor field would
triple-fault at the lretq), boottest PASS, l4test byte-identical.

MILESTONE: this was the last genuinely-flippable standalone file.  The only
built C++ that remains is the three wrapper-hosts -- glue/v4-x86/thread.cc,
glue/v4-x86/space.cc, glue/v4-x86/x64/space.cc -- which hold the C wrappers /
asm-named bodies bridging the still-C++ tcb_t / space_t / scheduler_t methods.
Flipping them is the final "de-classing" phase: converting those class methods
themselves to C.  (io_space/mdb_io/timer/vrt_io.cc are not built in this config.)
