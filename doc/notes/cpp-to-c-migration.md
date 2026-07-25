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


## 18. SCOPE — `kernelinterface.h` / KIP closure (2026-07-25, planned)

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
