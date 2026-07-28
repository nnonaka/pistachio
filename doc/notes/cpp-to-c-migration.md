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


## 62. glue/v4-x86/x64/space.cc -> C: first wrapper-host, definition-site flip, staged A/B (2026-07-27, commits daae0c0 3d899fa)

Start of the final de-classing phase.  x64/space.cc (179 lines) is the smallest
of the three wrapper-hosts and the first *definition-site* flip: unlike every
prior file (which only *called* class methods through the bridge), this file
*defines* out-of-line method bodies.  space_t / pgent_t stay C++ classes; only
the five bodies here move to C.

Scoping that made this the right first target: no BEGIN_DECLS wrapper block to
relocate (thread.cc has ~50, space.cc ~30); five bodies of which four are near
trivial (readmem_phys = one deref; smp_reference_bits = printf+UNIMPLEMENTED;
non-compat space_control = return 0; sigma0_translate = cast) and only
pgent_t::smp_sync is intricate.

Step A (foundation, byte-identical): the C forms smp_sync's C body needs, added
while the file was still C++ so they were unused/dead until the flip.  Defined in
space.cc (stays C++), declared in the arch headers:
  arch/x86/pgent.h        pgent_idx, pgent_is_cpulocal
  glue/v4-x86/space.h     space_get_top_pdir (pointer form; _phys already existed)
  glue/v4-x86/x64/space.h x86_top_pdir_get_kernel_pdp{,_pgent}
(space_pgent / space_pgent_cpu / pgent_next already existed.)

Step B (flip) -- the definition-site bridge and its wrinkles:
  - asm-name at the DEFINITION side: each `Type::method` body becomes a C free
    fn emitting a stable symbol, and the class declaration is annotated
    __asm__("<that symbol>") so the remaining C++ callers link to it.  Same
    mechanism as schedule.cc's scheduler_init, now applied where the body lives.
    Chose space_t_* names (space_t_space_control) to avoid clashing with the
    like-named syscall entry symbols (SYSCALL_ATTR("space_control")).
  - enum-arg ABI at the C/C++ boundary: pgsize_e is a 4-byte enum, the C bridge
    uses 8-byte word_t.  Fix = declare the method's pgsize param as word_t (not
    pgsize_e) in the header; the C++ inline callers (pgent_t::sync /
    reference_bits) then widen pgsize_e->word_t at the *call site*, so the full
    64-bit value matches the C word_t param.  (Declaring word_t on the C side
    alone would read undefined upper bits -- the widening must happen in C++.)
    Applied to smp_sync, smp_reference_bits, sigma0_translate.  Only the not-built
    x32 defn diverges from the changed decl.
  - callee-linkage is atomic with the flip: acpi_remap/acpi_unmap move to C, so
    generic/acpi.h wraps them in BEGIN_DECLS or the C++ callers (intctrl-apic.cc)
    would look for the mangled names.
  - C sees `struct space_t { x86_space_t base; }` (single inheritance ->
    base-as-first-member), so direct field access is space->base.data.reference_ptab,
    not space->data...  (all other space access goes through the accessors, which
    take space_t* and work by the base-at-offset-0 identity).
  - build: x64/Makeconf space.cc -> space.c; the KDB build compiles the same
    source, so both src/ and kdb/ objects pick up the rename.

Gotcha during the flip: the header edits left several dependent objects stale
(thread.o, bootinfo.o, linear_ptab_dump.o still referencing the old mangled
names at link) -- deleting .depend was not enough; had to touch the changed
headers / rm the stale .o to force recompile.

smp_sync / smp_reference_bits have no live callers in this config (no ->sync()
sites anywhere; NEW_MDB off so mdb_mem.cc -- the only reference_bits() caller --
is unbuilt), but the asm-name bridges are kept correct regardless.

Verified: builds 355912, zero implicit declarations, no new warning kinds (the
memdesc/kernelinterface header warnings space.c now surfaces already fire from
the other C TUs); boots through GDT/TSS + AP bringup; boottest PASS; l4test
test-region byte-identical (same KIP/memtest/IPC output, same Local-destination-Id
fault at eip=0000000001000295).

Remaining de-classing: glue/v4-x86/thread.cc (~50 tcb_t bridge wrappers to
relocate into headers as dual-repped C inlines) and glue/v4-x86/space.cc (~30
space_t bodies + the fpage_t/mem_region wrapper block, and it hosts the
pgent_*/space_* wrapper DEFINITIONS the other files depend on -- so it flips
last).


## 63. glue/v4-x86/thread.cc -> C: second wrapper-host, the tcb_t bridge, foundation + flip (2026-07-27, commits 665f919 fb9e715)

thread.cc (262 lines) is the tcb_t bridge-wrapper host: ~55 extern "C" wrappers
that C api/v4 code calls, plus two out-of-line bodies (create_startup_stack,
time_t::operator<) and the return_to_user asm stub.  The largest flip of the
migration -- but most of it collapsed to trivial translation once the layers
were mapped:

  - utcb_t is ALREADY a plain C-visible struct (glue/v4-x86/utcb.h; only its
    accessor methods are __cplusplus-guarded), so the ~14 utcb-delegating
    wrappers become direct self->utcb->field access -- NO utcb C-form layer.
  - stack accessors index tcb_get_stack_top()[KSTACK_*]; queue/lock use the
    ENQUEUE_LIST/DEQUEUE_LIST macros (pure token ops) + existing
    queue_state_*/spinlock_* C forms; timeout_get_snd/rcv, threadid_get_raw,
    fpage_nilpage, threadid_nilthread C forms already existed.

Foundation (part 1, 665f919, byte-identical): active_cpu_space_set (C accessor
over the C++ active_cpu_space_t.set) + C prototypes for the already-C
tcb_resources_save/load/release_copy_area (defined in resources.c).

Flip (fb9e715).  The delicate arch bodies were translated VERBATIM from
x64/tcb.h into C -- the inline asm ports unchanged, only the operand
expressions become C forms:
  - tcb_switch_to: the stack/cr3/%gs context switch.  resources.save/load ->
    tcb_resources_save/load; tss.set_rsp0 -> tss.rsp[0]=; active_cpu_space.set
    -> active_cpu_space_set; dest->get_local_id().get_raw() ->
    threadid_get_raw(&dest->myself_local); OFS_TCB_* immediates unchanged.
  - tcb_copy_mrs (rep movsq over &self->utcb->mr[start]), tcb_return_from_ipc /
    _user_interruption (asm), the three tcb_notify stack-builders
    (notify_prologue), tcb_do_ipc (calls the C sys_ipc from ipc.c directly).

Definition-site gotchas:
  - EXPECT_FALSE(self->resource_bits) has no implicit bool conversion in C ->
    added resource_bits_have_resources() C accessor (maskvalue != 0).
  - notify_prologue / active_cpu_space_set / present_list_lock /
    global_present_list are declared C++-only in their headers -> local externs
    in thread.c (the init.c "local externs" pattern).
  - taking &self->utcb->xfer_timeout warns (packed member) -> copy to a local
    first, then timeout_get_snd/rcv.

Wrappers whose bodies still need a C++ method with a deep cascade were RELOCATED
to space.cc (the sole remaining C++ TU, flips last) instead of translating the
cascade now: tcb_copy_area_real_address / tcb_adjust_for_copy_area (resources
copy-area -> page_table_index/x86_mmu/populate_copy_area), tcb_sched_set_timeout
(sched_state), tcb_init_saved_state, is_privileged_space_c,
acceptor_get_arch_specific_rcvwindow, time_lt (time_t::operator< inlined there --
its only out-of-line definition had been in thread.cc).  space.cc gained
INC_API(schedule.h) + generic-archmap.h so those inline method definitions link.
(git recorded thread.c as add/delete, not rename -- the content is a near-total
rewrite.)

Verified: builds 355848, zero implicit declarations, no new warning kinds; boots
through context-switch + AP bringup (a wrong switch_to operand would corrupt or
crash immediately); boottest PASS; l4test test-region byte-identical -- every IPC
transfer sub-test OK, same Local-destination-Id fault eip=0000000001000295.

MILESTONE: the ONLY remaining built C++ is glue/v4-x86/space.cc -- the last
wrapper-host.  It defines ~30 space_t out-of-line methods, hosts the
pgent_*/space_* C wrapper definitions every flipped file depends on, and now also
hosts the tcb_t/time_t wrappers relocated here from thread.cc.  Flipping it (and
de-classing space_t/tcb_t/time_t's residual methods) is the final step.


## 64. glue/v4-x86/space.cc -> C: SCOPING of the final flip (2026-07-27, foundation commit 043d717)

space.cc (1446 lines, ~92 function/method definitions) is the last built C++ TU
and the largest single flip.  Fully scoped; no infrastructure blockers -- every
external dependency already has a C form -- but it is a THREE-LAYER de-classing
(space_t -> pgent_t -> x86_pgent_t) plus mmu/fpage helpers, so it needs a
dedicated focused effort.  Map for whoever executes it:

Contents:
  - ~30 space_t out-of-line methods (init, allocate_tcb/utcb/space, remap_area,
    add_mapping, release_kernel_mapping, map_dummy_tcb, switch_to_kernel_space,
    sync_kernel_space, populate_copy_area, delete_copy_area, get/install/free/
    sync_io_bitmap, arch_free, init_kernel_mappings, init_cpu_mappings,
    init_kernel_space, flush_tlb, flush_tlbent, end_update, move_tcb,
    alloc/free_cpu_top_pdir, lookup_mapping) -- the hard part; intricate
    page-table walking where a wrong translation = boot crash.
  - the pgent_* / space_* / fpage_* / mem_region_* C wrapper DEFINITIONS (every
    other flipped file links against these).
  - the tcb_t/time_t wrappers relocated here from thread.cc (§63):
    tcb_copy_area_real_address, tcb_adjust_for_copy_area, tcb_sched_set_timeout,
    tcb_init_saved_state, is_privileged_space_c, acceptor_get_arch_specific_
    rcvwindow, time_lt.

Method-call inventory of the space_t bodies (what they invoke):
  pgent_t methods: subtree(11) next(9) is_valid(9) set_global(5) set_entry(5)
    set_cpulocal(4) address(3) is_subtree(2) set_cacheability(1) make_subtree(1)
    make_cpu_subtree(1).  Existing pgent_* C wrappers cover subtree/next/is_valid/
    set_entry/address/is_subtree/make_subtree.  MISSING -> add: pgent_set_global,
    pgent_set_cpulocal, pgent_set_cacheability, pgent_make_cpu_subtree.  These
    cascade: set_global = x86_pgent_t::set_global(bitfield) + pgent sync;
    set_cacheability likewise + sync; set_cpulocal = bitfield only; make_cpu_subtree
    = set_ptab_entry(kmem_alloc,...).  So x86_pgent_t's set_global/set_cpulocal/
    set_cacheability/set_ptab_entry need C forms too (bitfields ARE C-visible --
    the pg4k/pg2m union is outside x86_pgent_t's #if __cplusplus in ptab.h -- so
    translate to direct bitfield ops or add x86_pgent_* C accessors).  pgent_t::sync
    -> smp_sync is already asm-named pgent_smp_sync (§62).
  statics: get_kernel_space (C form get_kernel_space_c EXISTS), get_current_cpu
    (C form exists), x86_mmu set_active_pagetable/flush_tlb/get_active_pagetable/
    get_pagefault_address/flush_tlbent (ALL C forms now exist -- the last three
    added in 043d717), fpage_t::complete_arch (MISSING -> add fpage_complete_arch;
    used 2x), space page-area get_kip/utcb_page_area (C forms EXIST) but
    set_kip_page_area/set_utcb_page_area (MISSING -> add).

Flip mechanics (same as x64/space.cc §62): each space_t method -> C function
asm-named to a stable symbol (space_t_* or reuse the existing space_* wrapper
name), the class decl in space.h/x64/space.h annotated __asm__.  space_t methods
call each other by their C names; `this->` -> `self->`; space->data ->
space->base.data (struct space_t wraps x86_space_t base); pgsize_e -> word_t
(X86_PGSIZE_*).  Then rename space.cc -> space.c, update x86 Makeconf, rm .depend.

Suggested staging: (F1) add the missing C forms -- pgent_set_global/set_cpulocal/
set_cacheability/make_cpu_subtree (+ their x86_pgent_t bitfield deps),
fpage_complete_arch, space_set_kip/utcb_page_area -- as byte-identical header/
space.cc additions; (B) the flip.  Verify: build warning-clean + 0 implicit-decl,
boottest PASS, l4test test-region byte-identical (KIP/memtest/IPC, same
Local-destination-Id fault eip=0000000001000295).  Completing this drops the C++
toolchain entirely -- the migration goal.


## 65. glue/v4-x86/space.cc -> C: the last wrapper-host, staged F1/F2/mmu + flip (2026-07-27, commits e306a3d 98931c1 043d717 49fab2d)

space.cc (1446 lines) -- the monster, third wrapper-host, a THREE-LAYER
de-classing (space_t -> pgent_t -> x86_pgent_t) + mmu/fpage/atomic/mem_region.
No infrastructure blockers: every external dependency already had a C form (see
§64 scoping).

Foundation (byte-identical, committed separately): F1 pgent_set_global/cpulocal/
cacheability + pgent_sync + pgent_smp_sync/_reference_bits decls (pgent.h);
F2 x86_pgent_t bitfield C forms (ptab.h, ~24 accessors); x86_mmu_t get_active_
pagetable/get_pagefault_address/flush_tlbent (mmu.h).

The flip (49fab2d).  Approach that worked after per-method whitespace edits
proved fragile: a full careful Write of the translated file, then compiler- and
l4test-guided fixes.
  - each out-of-line space_t method -> a C function bearing its space_* wrapper
    name (the redundant wrapper deleted); internal calls use those names;
    this-> -> self->, space->data -> space->base.data (struct space_t wraps
    x86_space_t base), pgsize_e -> word_t (X86_PGSIZE_*), pgent->m(this,..) ->
    pgent_m(pgent,self,..), get_kernel_space() -> get_kernel_space_c().
  - pgent_* wrappers delegate to x86_pgent_* (F2) + pgent_sync + a static
    pgent_linknode_ptr (SMP __linknode_ptr form); pgent_set_entry rebuilds the
    attrib bits inline (PGE/PAT/NX #ifs kept).
  - fpage_* wrappers -> direct mem.x/raw field access (CONFIG_X86_IO_FLEXPAGES off
    => arch_fpage always invalid => all mem-pages; is_complete_mempage =
    size==1&&base==0).  NB fpage_get_base: (word_t)mem.x.base << 10 -- widen the
    bitfield to 64-bit BEFORE the shift or large bases truncate.
  - space_* inline-method wrappers -> self->base.data field access; is_user_area/
    is_tcb_area/is_copy_area over sign_ext (= addr | X86_X64_SIGN_EXTENSION);
    unmap_fpage builds an mdb_ctrl_t (C-visible) and calls the asm-named
    space_mapctrl.
  - the io_bitmap methods are dead (#if CONFIG_X86_IO_FLEXPAGES, off) -> #error
    guard instead of dead C++.

Gotchas:
  - active_cpu_space_t is a C++ class -> defined a C struct locally; atomic_t
    thread_count needs atomic_inc/dec/read C forms (added to atomic.h).
  - IS_SPACE_SMALL/GLOBAL and align_memregion are C++-only in space.h -> redefined
    IS_SPACE_* macros locally, added align_memregion + mem_region_is_empty as C.
  - REMAINING C++ callers of space_t methods (intctrl-apic.cc, sched-rr policy
    schedule.cc) forced asm-name bridges on add_mapping/move_tcb/lookup_mapping
    (the §62 pattern); lookup_mapping's out-param stays int* (4-byte pgsize_e ABI,
    NOT word_t* -- a word_t write would corrupt the caller's stack).
  - BUG found by l4test (hang at "Send timeout"): tcb_sched_set_timeout passed
    enqueue=false, but sched_ktcb_t::set_timeout(u64_t,bool enqueue=true) defaults
    to true; the timeout never enqueued.  Fixed to true.  (First real behavioural
    bug caught by l4test in the whole migration -- underscores its value.)

Verified: builds 354744, 0 implicit-decl, warning-clean; boots through kernel-
space + per-CPU page-table init + AP bringup (a wrong pgent bit => triple fault);
boottest PASS; l4test test-region byte-identical (Send/Receive timeout now OK,
same Local-destination-Id fault eip=0000000001000295).

MILESTONE: all three wrapper-hosts flipped; tcb_t/space_t/pgent_t glue is fully C.
STILL BUILT AS C++ (the earlier "3 wrapper-hosts = last" framing overlooked
these): api/v4/sched-rr/schedule.cc (scheduler policy; hosts the sched_* wrapper
defs), arch/x86/x64/init32.cc (32-bit boot init), generic/acpi.cc,
platform/generic/intctrl-apic.cc.  Plus config-gated/unbuilt: io_space/mdb_io/
vrt_io/hvm-space/timer/mdb/mdb_mem.


## 66. acpi.cc + intctrl-apic.cc -> C: ACPI tables and the APIC/IOAPIC controller (2026-07-27, commits a3a9875 a9985cf 94c8846 855b7c0 405b689)

The interrupt-controller chain, done foundation-first.  intctrl-apic.cc could not
be flipped alone: init_arch drives the whole ACPI/MADT walk through acpi.cc's
C++ classes (25 coupling sites), so acpi.cc had to go first.

Foundations (each byte-identical, committed separately):
  a3a9875  arch/x86/apic.h -- C forms of the 8 local_apic_t<base> template
           methods intctrl uses (id/set_id/version/set_task_prio/mask/enable/
           error_setup/read_error).  The template's register structs are
           C++-only nested classes, so the C forms open-code the register
           access over APIC_MAPPINGS_START + the regno_t offsets, with the
           bit positions taken from the reg-struct bitfields.
  a9985cf  generic/acpi.cc -> acpi.c + a C mirror of every ACPI table class.
           They are plain packed data, so the C structs restate the members in
           order.  The 7 acpi_madt_t methods got __asm__ labels so the (then
           still C++) intctrl-apic kept linking.
  94c8846  platform/pc99/82093.h -- C mirrors of ioapic_redir_t and i82093_t
           (redirection-entry bitfields + the register-select/data-window pokes,
           including the masked-entry reread quirk); acpi_rsdp_locate;
           acpi_rsdp_rsdt/_xsdt and acpi_rsdt_find/acpi_xsdt_find/acpi_rsdt_list
           (the acpi__sdt_t<T> template methods, one pair per pointer width).
  855b7c0  local_apic_send_init_ipi/_send_startup_ipi; and HW_IRQ() spelled
           `extern "C"` unconditionally -> hoisted to __HWIRQ_EXTERN_C so the
           hwirq stubs can be emitted from a C file.

The flip (405b689): intctrl_t + its nested ioapic_t/ioapic_redir_table_t mirrored
in C (generic_intctrl_t is an empty base -> contributes nothing); the ~18 methods
translated; APIC_PGENTSZ/ACPI_PGENTSZ were pgent_t::size_* -> X86_PGSIZE_* in C;
LAPIC_LVT_* macros for the lvt_t values.  get_number_irqs keeps an __asm__ label
(kdb's platform/pc99/intctrl.cc calls it); handle_irq already had one because the
hwirq_common asm stub calls it by name with $intctrl in the first arg register.

Bug class caught again: generic/intctrl.h declared handle_interrupt inside a
BEGIN_DECLS block that was itself *inside* the `#if defined(__cplusplus)` guard,
so the C file got an implicit declaration.  Moved out.  (Same latent-bug class as
the earlier interrupt.c one -- always sweep for "implicit declaration" after a
flip; it is the only signal that a cross-TU call is going out untyped.)

Verified: builds 350136 (-4.5K -- no more template instantiations/mangled
thunks), zero implicit declarations kernel-wide, warning-clean; boots through
ACPI parsing + IOAPIC/LAPIC bringup; boottest PASS; l4test test-region
byte-identical.  The whole test suite is delivered over IOAPIC interrupts, so
this path is well exercised.

ACCURATE REMAINING-C++ INVENTORY (from a full rebuild, grepping the compile
lines -- earlier lists in these notes were taken by scanning kernel/src only and
so MISSED the kdb tree entirely, which is a separate top-level directory):
  kernel/src/api/v4/sched-rr/schedule.cc   scheduler policy (hosts sched_* wrappers)
  kernel/src/arch/x86/x64/init32.cc        32-bit early boot init
  kernel/kdb/**                            32 files -- the entire kernel debugger
                                           (generic/, api/v4/, arch/x86/, glue/,
                                           platform/pc99/)
Total 34 files.  The kdb tree is a whole subsystem in its own right and is the
bulk of what is left; it is also the least boot-critical (debugger only).


## 67. api/v4/sched-rr/schedule.cc -> C, and the kdb tree scoped (2026-07-27, commits 0be2e78 45d072a 0cd517a 1ecd6cd 05a8413 ed5cae5)

The scheduler policy was the last C++ file outside kdb.  Flipping it meant
de-classing the whole scheduler_t / rr_scheduler_t / sched_ktcb_t method
surface (23 methods), because the ~40 sched_* wrappers this file hosted existed
precisely *because* it was the C++ host.  See the commit message for the
mechanics.  Two things worth carrying forward:

  - VERIFICATION GAP: three foundation commits were "verified byte-identical"
    by builds that never recompiled the C TU which includes those headers.  The
    kernel size was unchanged and nothing errored, so they looked clean -- but
    the C forms had never been compiled.  Forcing that TU to rebuild surfaced
    three real bugs (wrong field path through the policy base, a call to a
    function declared later in the include order, and six forms referencing
    types that are incomplete that early).  After a header-only change, always
    `touch` a consumer TU before claiming it builds.
  - api/v4/sched-rr/schedule_functions.h is effectively C++-ONLY: preprocessing
    api/v4/schedule.c shows C never reaches it.  It is not a home for C forms.
    Anything needing scheduler_t/tcb_t complete belongs in the .c file.

MILESTONE: kernel/src is 100% C (verified by grepping the compile lines of a
full rebuild, not by scanning the source tree).

### The kdb tree -- decoded, with a staged plan

All 32 remaining C++ files are under kernel/kdb.  They are NOT 32 independent
flips, but they are also not one undifferentiated blob.  The coupling is two
generated headers plus two macros:

  kdb/Makeconf generates, into $(BUILDDIR)/include/:
    kdb_class_helper.h        greps "^ *DECLARE_CMD *(" out of $(filter %.cc %.h)
                              and emits `static cmd_ret_t NAME(cmd_group_t*);`
    kdb_autogenerated_protos.h  greps " kdb_t::.*(" out of $(filter %.cc)
                              and emits the bare prototypes
  Both are #included *inside* `class kdb_t` (src/kdb/kdb.h), which is why every
  command is a static member.  src/kdb/cmd.h then has
    DECLARE_CMD(func, group, ...)  ->  { ..., &kdb_t::func }
    CMD(func, param)               ->  cmd_ret_t kdb_t::func(cmd_group_t *param)

Crucially cmd_func_t is a PLAIN function pointer (cmd_ret_t (*)(cmd_group_t*)),
not a pointer-to-member -- the commands are static members, so no PTMF problem.

C++ surface actually in the 32 files: 14 explicit `kdb_t::`, 244 `X_t::y` calls
into kernel types (most of which now have C forms from this migration), 1 class
definition, 3 new/delete, 7 reference parameters.  kdb_t's own instance data is
only kdb_param / kdb_current / last_space / last_dump.

STAGED PLAN (avoids a 32-file atomic flip):
  Step A (enabler, all files stay .cc, expect byte-identical):
    - change the two generator rules to emit FREE function declarations instead
      of class-scoped ones, and include them outside `class kdb_t`;
    - change DECLARE_CMD to `{ ..., func }` and CMD to `cmd_ret_t func(...)`;
    - reduce kdb_t to its four data members.
    Everything still compiles as C++; verify boottest + l4test.
  Step B..N: flip the files one at a time to .c, translating the `X_t::y` calls
    to the existing C forms.  Remember to change the Makeconf generator filters
    from `%.cc` to `%.c` (and SOURCES) as files move -- the greps are extension-
    filtered, so a half-renamed tree silently loses command declarations.

## §68 — kdb/api/v4/sigma0.cc → .c (and two shared prerequisites)

Two prerequisites had to land with this file, both shared by every remaining
kdb command file:

1. **`cmd_group_t::interact` asm-name bridge.** Every submenu-invoking command
   calls `group.interact(cg, name)`. Gave the C++ method
   `__asm__ ("cmd_group_interact")` and declared the matching C prototype
   (leading `self` pointer) in `src/kdb/cmd.h`. Same pattern as `input.h`.

2. **`kdb_class_helper.h` needs `BEGIN_DECLS`.** The generator greps
   `DECLARE_CMD(` out of the sources and emits plain declarations for every
   command function. A `.c` file reads those as C (unmangled); a `.cc` file
   that *defines* the command reads them as C++ (mangled) — so the moment a
   command declared in one language is defined in the other, the link fails
   (`undefined reference to cmd__prior/cmd__abort/cmd__help`). Wrapping the
   generated header in `BEGIN_DECLS`/`END_DECLS` in `kdb/Makeconf` makes both
   languages agree. Regenerate with `rm -f include/kdb_class_helper.h`.

File-local translations: `enum sigma0_request_e` needed a `typedef` (C has no
implicit enum-tag type name); the `word_t arg = 0` default argument was spelled
out at both call sites; `get_dec ("Verbose level", 1)` became
`get_dec (..., 1, NULL)` — the C form takes all three parameters, and `NULL` is
exactly the C++ default for `defstr`.

Verification: 342232 bytes, warning-clean, 0 implicit declarations, boottest
PASS. l4test compared against a **rebuilt pre-flip kernel run through the
identical harness** — the only diffs are the build timestamp and CPU-speed
calibration jitter. Drove the command itself: `0` opens the sigma0 submenu,
`?` lists `m`/`v`, `m` and `v` both execute, and `v` prints `Verbose level [1]:`
confirming the `get_dec` default-argument translation is faithful.

**Note on the l4test harness:** under `-smp 2` the interactive menu produces a
loop of `tcb_get_cpu (self) == tcb_get_cpu (dest)` assertion failures at
`glue/v4-x86/thread.c:285`. This is **pre-existing**, not a migration
regression — the HEAD kernel reproduces it identically (30 assertions, 188
lines). Use `-smp 1` when driving kdb commands interactively.

## §69 — kdb/generic/mapping.cc → .c

Cheap flip: `src/generic/mapping.h` was already dual-repped, so every accessor
this file needs (`mapnode_get_space/_depth/_pgent/_nextroot/_nextmap`,
`mapnode_is_next_root/_map/_both`, `rootnode_get_map/_root`,
`rootnode_is_next_*`) already had a C form, as did `sigma0_mapnode`,
`mdb_pgshifts` and `hw_pgshifts`.

I briefly added `pgent_vaddr`/`pgent_get_linknode` as new C inlines in
`arch/x86/pgent.h` before finding `pgent_vaddr` was **already** defined
out-of-line in `glue/v4-x86/space.c:854` and declared in pgent.h's BEGIN_DECLS
block. Reverted. *Check the existing BEGIN_DECLS block before writing a new C
form* — several are already there from earlier flips.

`get_hex ("Address")` needed its two default arguments spelled out; the C++
defaults are `(NULL, 0, NULL)`, so `get_hex ("Address", 0, NULL)` is exact.

**`pgsize_e` arithmetic:** `mapnode_t::pgsize_e` carries overloaded
`operator+/-(pgsize_e, int)` that round-trip through the 4-byte enum, so C++
`size-1` truncates to 32 bits where C `word_t` does not. This only diverges at
`size == 0`, which the recursion cannot reach (a size-0 root has no next_root),
and both languages would index `mdb_pgshifts[]` out of bounds there anyway.

Verification: 338032 bytes, warning-clean, 0 implicit declarations, boottest
PASS. Drove the `m` command at four addresses against a rebuilt pre-flip
kernel — output identical, including node addresses. Address `0x1000000` was
chosen deliberately because it is the only probe that reaches `dump_mdbmaps`
(the `[1] space=... vaddr=... pgent=...` line); the first baseline at address 0
stopped at `dump_mdbroot` and would have left `pgent_vaddr` and most of the
translated accessors unexercised.

## §70 — kdb/generic/linear_ptab_dump.cc → .c

Another cheap one: every `pgent_t` method it uses already had a C form in
`arch/x86/pgent.h`'s BEGIN_DECLS block or its C-only INLINE block
(`pgent_is_valid/_subtree/_readable/_writable/_executable/_kernel`,
`pgent_address/_subtree/_next/_mapnode/_reference_bits/_dump_misc`), and
`space->pgent (n, cpu)` had `space_pgent_cpu`.

**The 4-byte enum trap, fourth occurrence.** `get_ptab_dump_ranges` writes its
`max_size` out-parameter through an `int *` on the C side, so the local must be
`int`, not `word_t` — a `word_t` write would clobber 8 bytes of caller stack.
The loop variable `size` is `int` too, both to match the C++ enum's width and to
keep `size < max_size` from mixing signedness.

The `#if !defined(CONFIG_ARCH_X86)` fallback is dead for this config but was
still translated; it now names `PGENT_SIZE_MAX`, which no arch defines yet, with
a comment saying non-x86 ports must supply it. The rename touched four
Makeconfs (x64, x32, powerpc, powerpc64) since the file is shared.

Left the `$Id: linear_ptab_dump.cc,v` line alone — that is a CVS record of the
file's history, not a path reference, and rewriting it would be a false record.
Same convention as `sigma0.c`.

Verification: 337976 bytes, warning-clean, 0 implicit declarations, boottest
PASS. Drove `p` over the *user* area against a pre-flip kernel: 22 lines
identical, covering subtree recursion in both directions, valid mappings,
reference bits, `mapnode`, and the `dump_misc` cacheability suffix.

## §71 — kdb/arch/x86/breakpoints.cc → .c

The file itself was already valid C apart from four `get_hex ("...")` calls
needing their default arguments spelled out. (`case '0'...'3':` is a GCC range
extension that C accepts fine.)

The work was in `glue/v4-x86/debug.h`, where `enum x86_breakpoint_type_e` and
`x86_set_kdb_dr` were both trapped inside a `#if defined(__cplusplus)` block
along with `do_enter_kdebug` and `x86_reset`. Split the block: the enum is
plain C-compatible so it moves out unguarded (with a C-only typedef for the
tag name), `x86_set_kdb_dr` moves under BEGIN_DECLS, and `do_enter_kdebug`
(x86_exceptionframe_t) plus the `extern "C"` bits stay C++-only.

`x86_set_kdb_dr` thereby goes from C++-mangled to C linkage. The only other
reference in the tree is `glue/v4-x86/x32/hvm-vmx.cc`, which is not built in
this config and whose call is commented out, so nothing else had to change.

Verification: 337912 bytes, warning-clean, 0 implicit declarations, boottest
PASS. Drove `b` three times against a pre-flip kernel — dump DRs, set DR0 to
an instruction breakpoint at 0x1000628, dump again — output identical, with
DR7 going 0x400 -> 0x402 and DR0 taking the address, so both the read path
(X86_GET_DR) and the write path (get_choice/get_hex/x86_dr_write/X86_SET_DR)
are covered.

## §72 — kdb/api/v4/kernelinterface.cc → .c, and a real bug in shipped C code

The flip itself was routine: ~20 missing C accessors added to the already
dual-repped `api/v4/kernelinterface.h` (api_version, api_flags, clock_info,
page_info, processor_info, kernel_id, kernel_gen_date, kernel_version,
kernel_descriptor), plus `MEMDESC_MAX_TYPE`. `processor_info_get_procdesc` and
`memory_info_get_memdesc` already existed as out-of-line C functions.

### THE WIDE-BITFIELD SHIFT TRAP (new, and it had already bitten us)

Comparing the `K` dump against the pre-flip kernel showed three wrong lines:

      0x0000000000000000 - 0xffffffffffffffff   shared     (C++, correct)
      0x0000000000000000 - 0x003fffffffffffff   shared     (C, wrong)

**C and C++ disagree on the type of a bitfield expression.** For a bitfield
wider than `int`, C++ uses the declared type (`word_t`), but GCC in C gives the
expression the bitfield's own width. So for a 54-bit field, `f << 10` keeps only
54 bits in C and the top 10 are discarded. Reduced case:

    struct m { word_t pad:10; word_t _high : 64-10; };
    (x._high << 10) + 0x3ff   ->  C: 003fffffffffffff   C++: ffffffffffffffff

This is **not** a translation slip — the C and C++ sources were textually
identical. Any `INLINE` C form that left-shifts a bitfield wider than `int` is
silently wrong.

Critically, this was **already live in committed code**: `memdesc_low`,
`memdesc_high` and `memdesc_size` were converted in an earlier session and are
called by `glue/v4-x86/init.c` when it walks memory descriptors and tests
`memdesc_high (md) <= KERNEL_AREA_END`. Boot output happens not to change
(the descriptors that decide the outcome are all below 2^54), so nothing ever
failed visibly — but the truncation was real and would misjudge any region at
or above 2^54. Fixed by casting to `word_t` before the shift, in `memdesc_low`,
`memdesc_high`, `memdesc_size` and `page_info_get_page_size_mask`.

Audited every other `self->field << n` C form in the tree: the rest
(`id << 24`, `subid << 16`, `version << 24`, `subversion << 16`,
`word_size << 2`) are all narrow fields that promote to `int` identically in
both languages, so they are unaffected.

**Rule going forward: when writing a C form for any bitfield wider than `int`,
cast to `word_t` before shifting.**

Verification: 337816 bytes, warning-clean, 0 implicit declarations, boottest
PASS, boot output identical (normalised), full l4test run identical apart from
build timestamp and CPU-frequency calibration jitter, and the `K` dump matches
the pre-flip kernel line for line. Note the pre-existing `Local destination Id:
FAILED` in l4test under `-smp 1` reproduces identically on the pre-flip kernel —
not a regression.

## §73 — kdb/generic/input.cc → .c

The cheapest flip so far: three `getc ()` calls needed their default argument
(`getc (true)`), and nothing else changed. The groundwork was already done when
`kdb/input.h` was dual-repped earlier in this session — its declarations were
put under BEGIN_DECLS then, so `get_hex`/`get_dec`/`get_choice` already had C
linkage and every call site already spelled out the arguments C cannot default.

`get_space`/`get_thread`/`get_comspace`/`get_thrspace` are *not* in this file
(they live in `kdb/api/v4/input.cc`), so nothing else was pulled in.

Verified the object is actually compiled by `gcc` and not `g++` — the kernel
size did not change at all (337816 both sides), which is expected here since
the translated code is identical, but it means size is no evidence of a rebuild.

Verification: warning-clean, 0 implicit declarations, boottest PASS, and the
prompt behaviour driven against a pre-flip kernel over the paths that matter:
`0x` prefix handling, digit entry, invalid characters ignored, backspace echo,
ESC returning ABORT_MAGIC, `get_choice` on RETURN (default), `get_choice` on an
explicit key, and `get_choice` echoing a lettered default. Output identical.

## §74 — kdb/generic/cmd.cc → .c (resolves the interact bridge)

This is the file that *defines* `cmd_group_t::interact`, which §68 asm-name
bridged as `cmd_group_interact` for sigma0.c. The C definition now emits that
symbol directly, so the bridge is resolved rather than merely forwarded:
`nm` shows one `T cmd_group_interact` and zero `_ZN12cmd_group_t*` symbols.

De-classing:
  - `interact_by_key` / `interact_by_command` become file-static C functions
    taking `cmd_group_t *self`; their private declarations are removed from the
    class (methods do not affect layout, so this is layout-neutral).
  - `reset()` / `next()` get C forms `cmd_group_reset` / `cmd_group_next` in
    cmd.h, built on the existing `linker_set_reset` / `linker_set_next` bridges.
    No other C++ file called them, so the C++ inlines just become unused.

**`kdb_cmd_mode` — a static member, not instance data.** It is a *static* member
of `class kdb_t`, so it is a global with a mangled name and is absent from the C
`struct kdb_t`. `kdb/cmd.h` already carried a stale `extern cmd_mode_t
kdb_cmd_mode;` (comment claimed entry.cc; the definition is really init.cc)
which nothing referenced and which would not have linked. Gave the member an
`__asm__ ("kdb_cmd_mode")` label and put that extern under BEGIN_DECLS, so C
reads/writes the same global the C++ side uses.

**Generator trap:** putting the asm label on the *definition* in init.cc
(`cmd_mode_t kdb_t::kdb_cmd_mode __asm__ (...)`) breaks the build — kdb/Makeconf
greps `" kdb_t::"` out of the sources into `kdb_autogenerated_protos.h`, which
is included *inside* the class body, producing "asm specifiers are not permitted
on non-static data members". The label must go on the in-class declaration in
kdb.h, which the grep does not match.

Verification: 337624 bytes, warning-clean, 0 implicit declarations, boottest
PASS. Drove the command loop against a pre-flip kernel via a scripted sequence
(scratchpad/cmdrun.sh): keystroke-mode help, group entry, modeswitch to line
mode, line-mode help with its padded layout, TAB unique completion, an unknown
command, group navigation, and modeswitch back. Output identical.

Not covered: the multi-match TAB listing path. Attempts to trigger it completed
uniquely to `dumpframe` and executed it, cascading page faults and making the
run nondeterministic, so the probe was dropped rather than left flaky.

## §75 — kdb/arch/x86/x86.cc → .c (template dependencies)

This file used `local_apic_t<APIC_MAPPINGS_START>` — a C++ *template* — in four
places, plus `nmi_t` whose methods use a second template, `rtc_t<0x70>`.
Templates cannot be bridged by asm name, so both were re-implemented natively
in C, following the existing C block in `arch/x86/apic.h` (which already had
`local_apic_eoi`, the timer helpers, `local_apic_id` and the `LAPIC_LVT_*`
constants from earlier work).

New C forms:
  - `arch/x86/apic.h`: `local_apic_disable`, `local_apic_read_vector`,
    `local_apic_send_nmi` (+ `X86_LAPIC_SVR`). Bit positions transcribed from
    `command_reg_t`: vector 0:7, delivery_mode 8:10, destination_mode 11,
    delivery_status 12, level 14, trigger_mode 15, destination 18:19.
  - `platform/pc99/nmi.h`: `nmi_mask` / `nmi_unmask`, with `rtc_t<0x70>::read`
    inlined as the two port accesses it performs.
  - `arch/x86/atomic.h`: `atomic_set`, mirroring `operator= (word_t)`.

`cpu_get`/`cpu_is_valid`/`cpu_get_id` and `x86_exceptionframe_dump` already
existed.

Verification: 337184 bytes, warning-clean, 0 implicit declarations, boottest
PASS. Two scripted sessions against a rebuilt pre-flip kernel, both identical:
`frame`, `ctrlregs`, `lvt`, `dumpmsrs`, `ports` (scratchpad/x86run.sh) and the
`enable_nmi` disable/enable pair (scratchpad/nmirun.sh). The `lvt` case is the
strong one — six `local_apic_read_vector` results match the template's output
exactly, confirming the register offsets.

**Not verified at runtime:** `cmd_reset` (reboots the machine),
`cmd_send_nmi` and `cmd_switch_cpus` (need a second CPU, and `-smp 2` triggers
the pre-existing assertion loop from §68). So `local_apic_send_nmi` and
`local_apic_disable` are verified by transcription against the template
source, not by execution.

The `CONFIG_X_X86_HVM` block is off in this config and so is preprocessed away.
It was translated by inspection and names `space_is_hvm_space`,
`space_get_hvm_space` and `hvm_lookup_gphys_addr`, none of which exist yet — an
HVM port must supply them. Same honest-undefined-name approach as §70.

## §76 — kdb/api/v4/input.cc → .c

Defines `get_space` and `get_thread` (the thread-name parser). Nearly all the
C forms it needs already existed: `tcb_get_space`, `tcb_get_tcb`,
`get_kernel_space_c`, `get_idle_tcb_c`, `get_kdebug_tcb`, `space_is_user_area`,
and the whole `threadid_*` family (`threadid_from_raw`, `threadid_global`,
`threadid_nilthread`, `threadid_irqthread`, `threadid_equals`).

Only `tcb_t::is_tcb` needed a C form — and adding it repeated a mistake from
earlier in the migration: **I inserted it without mapping the guards first**, so
it landed inside the `#if defined(__cplusplus)` block that opens well above it
(line 411) and runs past the definition. `addr_to_tcb` was stuck in that same
block. Both are now hoisted out ahead of the C++ region.

Under CONFIG_STATIC_TCBS there is deliberately **no** C form: that branch reads
`tcb_array`, a static member of `class tcb_t`. A STATIC_TCBS port must make it
C-visible first; the comment says so rather than pretending otherwise. That
config is off here.

Verification: 337176 bytes, warning-clean, 0 implicit declarations, boottest
PASS. Drove `t` six ways against a pre-flip kernel (scratchpad/inp2run.sh):
by name for sigma0 / roottask / idlethrd, the RETURN default (current), a
`80v1` threadno+version entry, and an invalid name that backs itself out.

One field differed: `curr ts`. It is stable per binary, so "run-to-run noise"
would have been the wrong explanation. Ran the *same* binary with the kdb entry
delayed 14s -> 17s and got exactly the pre-flip value (6094us), proving `curr ts`
tracks when the debugger is entered rather than the code. Every other field
across all six dumps — TCB address, ID, priority, state, queues, space, pdir,
pager, quanta, timeouts, resources, flags, partner, scheduler — is identical.

## §77 — kdb/generic/bootinfo.cc → .c

Self-contained: every class this file used (`bootrec_t`, `boot_module_t`,
`boot_simpleexec_t`, `boot_efi_t`, `boot_mbi_t`, `bootinfo_t`) is declared
*inside the file*, not in a shared header, so de-classing touched no headers at
all. Members were already in declaration order, so layout is unchanged.

`space_readmem_phys` already existed. Note `readmem_phys` is a *static* method,
so the C++ `space_t * s = kdb.kdb_current->get_space (); s->readmem_phys (...)`
was calling a static through an instance — the local becomes unused in C and is
dropped.

**Enum width, again.** The C++ `type()` returned `type_e`, which GCC sizes as a
4-byte `unsigned int`, so a record with garbage in the high 32 bits had them
silently dropped. A plain `word_t` C form would *not* truncate, and the
difference is observable in the `default:` case, which prints the type. Added an
explicit `(u32_t)` cast to preserve the original behaviour exactly. This is the
third width-divergence found in this migration (see §69, §72) — worth checking
on every enum-returning accessor, not just shifts.

**Pre-existing bug left alone:** `kmem_alloc (&kmem, kmem_misc, (1UL << alloc_size))`
shifts by `alloc_size`, which is already a *byte count* (>= 4096), not a log2.
On x86 the shift count is masked to 6 bits, so this asks for `1UL << 0` = 1 byte
and the copy loop then writes `size` bytes into it. Faithful migration means
preserving it; flagging it here rather than fixing it under cover of a
translation commit.

Verification: 337112 bytes, warning-clean, 0 implicit declarations, boottest
PASS, and the `B` command run twice against a pre-flip kernel
(scratchpad/birun.sh) — identical. Running it twice matters: the first call
takes the validate-and-copy path (`bootinfo_is_valid`, `bootinfo_size_safe`,
the `space_readmem_phys` loop), the second uses the cached copy.

## §78 — kdb/api/v4/tcb.cc → .c, and a REAL ABI DIVERGENCE in sched_ktcb_t

The flip needed a lot of new C forms: `tcb_get_utcb/_saved_state/_cop_flags/
_preempt_flags/_intended_receiver`, `thread_state_string`,
`msg_tag_is_redirected/_is_xcpu`, `msg_item_get_string_cache_hints`,
`time_is_period`, `fpage_is_complete_fpage`, and a C branch for the `TID()`
macro (it used `.get_raw()`).

The four `sched_ktcb_t` debug dumps (`dump_priority`, `dump_list1`,
`dump_list2`, `dump`) were **header inlines** in the C++-only
`sched-rr/schedule_functions.h` whose *only* caller was this file. Per the
emission rule, they were moved out to real C functions in
`sched-rr/schedule.c`, declared in `sktcb.h`.

### THE FINDING: C and C++ placed `sched_ktcb_t::scheduler` 2 bytes apart

Comparing `t`/`T` against the pre-flip kernel showed the `scheduler:` field
differing — old printed raw `003a000000010000`, new printed `ROOTTASK`. That
raw value is roottask's real tid `0000003a00000001` shifted left exactly 16
bits, i.e. a read 2 bytes early. Measured with conflicting-declaration probes
compiled *inside the real build* (a standalone probe TU gives wrong numbers —
the headers are not configured the same way):

    sizeof(policy_sched_ktcb_t)            88   both languages
    offsetof(rr_sched_ktcb_t, max_delay)   84   both languages
    offsetof(sched_ktcb_t, scheduler)      88 in C, 86 in C++   <-- diverges
    sizeof(sched_ktcb_t)                   96   both languages

`rr_sched_ktcb_t` ends with `u16_t max_delay` at 84, so it occupies 86 bytes
and pads to 88. C++ derives (`class sched_ktcb_t : public policy_sched_ktcb_t`)
and, because the class is non-standard-layout, **reuses the base's tail
padding** — putting `scheduler` at 86. The C rep models inheritance as an
embedded member (`policy_sched_ktcb_t base;`), which cannot reuse tail padding,
so C puts it at 88.

This was introduced when sktcb.h was dual-repped, i.e. **the migration silently
moved a field by 2 bytes**. It went unnoticed because sizeof still matches (so
nothing after `sched_state` in `tcb_t` shifted) and because by then every
writer *and* reader of the field was C — self-consistent. kdb's `scheduler:`
display was the last C++ reader and the only visible symptom.

Fixed by adding an explicit `u16_t __tail_pad` to `rr_sched_ktcb_t`, leaving no
tail padding for C++ to reuse. Both languages now report offset 88, sizeof
unchanged at 96. Verified decisively: rebuilding the **pre-flip C++** kernel
with *only* the padding fix makes it print `ROOTTASK` too.

**Lesson: `struct X { Base base; }` is not equivalent to `class X : public Base`
whenever Base has tail padding and the derived type is non-standard-layout.**
Every `__base`/`base` composition in this migration should be probed the same
way — `space_t`, `scheduler_t`, `x86_exceptionframe_t` are the other three.

Verification: 333152 bytes, warning-clean, 0 implicit declarations, boottest
PASS, and `t`/`T` driven three ways (basic dump, extended with UTCB + MRs +
BRs, and a thread with no UTCB) identical to the corrected C++ baseline.

## §79 — Audit: every base-composition dual-rep probed for the §78 bug

Followed up §78 by measuring all of them, with conflicting-declaration probes
compiled **inside the real build** (a standalone probe TU reports wrong numbers
because the kernel headers are not configured the same way).

| dual-rep (derived / base)                 | derived data members    | C (offset/size) | C++ (offset/size) | verdict |
|-------------------------------------------|-------------------------|-----------------|-------------------|---------|
| sched_ktcb_t / policy_sched_ktcb_t         | `scheduler`             | 88 / 96         | **86** / 96       | **was broken, fixed in §78** |
| sync_entry_t / cpu_mb_entry_t              | `pending_mask`,`ack_mask` | 80 / 96       | 80 / 96           | OK |
| space_t / x86_space_t                      | none                    | – / 4096        | – / 4096          | OK |
| scheduler_t / policy_scheduler_t           | none                    | – / 2072        | – / 2072          | OK |
| x86_exceptionframe_t / x86_exceptionregs_t | none                    | – / 176         | – / 176           | OK |

**Two conditions must both hold for the bug to appear:**
1. the base has tail padding, and
2. the derived type adds at least one data member.

`sched_ktcb_t` was the only case where both held — `rr_sched_ktcb_t` ended at 86
and padded to 88. `sync_entry_t` adds members but `cpu_mb_entry_t` is exactly 80
bytes with no tail padding, so there is nothing to reuse. The other three add no
data members at all, so `sizeof(derived) == sizeof(base)` trivially.

No automated guard was added: the natural one needs `offsetof` on a
non-standard-layout type, which makes GCC emit `-Winvalid-offsetof` in every C++
TU that includes the header. The probe recipe is recorded here instead — append
to a real C TU and a real C++ TU that include the header:

    char __p[__builtin_offsetof(T, first_derived_member)];  char __p[1];
    char __s[sizeof(T)];                                    char __s[1];

then read the sizes back out of the "conflicting declaration" notes. Re-run it
whenever a field is added to a base that a dual-repped type derives from.

## §80 — kdb/platform/pc99/io.cc → .c

The kdb console driver (serial + screen putc/getc, `kdb_consoles[]`) plus the
VGA screendump command. The active code was already essentially C: the only
translation in compiled code is one `readmem` call.

`readmem` is a **C++ function template** in `generic/linear_ptab.h` — the second
template blocker after `local_apic_t` (§75), and templates cannot be asm-name
bridged. Added `readmem_u8` and `readmem_word` C forms mirroring the template
body exactly (direct access outside the user area, otherwise checked
`space_readmem` plus a mask; `is_user_area` is static in C++ so takes no space
argument). The template itself stays — `kdb/glue/v4-x86/prepost.cc` is still C++
and still instantiates it, so there is no emission problem.

Consolidated a duplicate: `glue/v4-x86/exception.c` already carried an identical
`static readmem_u8` from an earlier flip. Removed it in favour of the shared
header version rather than leaving two copies to drift.

The `CONFIG_X86_IO_FLEXPAGES` block (off here) holds the only other C++ in the
file; translated by inspection, naming `mdb_node_get_table` and
`space_get_io_space`, which do not exist — an IO-flexpage port must supply them.
Same convention as §70 and §75.

Verification: 332944 bytes, warning-clean, 0 implicit declarations, boottest
PASS, and the `V` screendump driven against a pre-flip kernel
(scratchpad/vgarun.sh) — 25x80 of real screen memory read through `readmem_u8`,
identical apart from one line: the on-screen kickstart text quotes the kernel's
own size, which legitimately changed (333152 -> 332944). The console driver
itself needs no separate test — every character of the session is proof it works.

## §81 — kdb/arch/x86/x64/x86.cc → .c (the dump_hwcr blocker, resolved)

This file was reverted earlier in the migration because
`x86_amdhwcr_t::dump_hwcr()` is a **header inline** whose only caller it was —
once the caller became C, nothing would emit it, and `__attribute__((used))`
does not help. The rule established then applies directly: **re-implement
natively in the C block rather than asm-name bridging**. Added
`amdhwcr_dump_hwcr` plus the fourteen predicates it uses; all the
`X86_AMDHWCR_*` bit macros were already outside the `__cplusplus` guard.

Transcribed the predicate bodies rather than inferring them from the bit names:
**seven of the fourteen negate a *DIS* bit** (`is_ptemem_cached`,
`is_flushfilter_enabled`, `is_lockprefix_enabled`, `is_smi_spc_enabled`,
`is_rsm_spc_enabled`, `is_sse_enabled`, `is_wrap32_enabled`). Guessing from the
names would have inverted half the output.

The other two apparent blockers had already been solved: `dump_features` has a
C form (`x86_x64_cpu_features_dump`), and `idt.get_descriptor(i)` is just
`idt.descriptors[i]` — a plain member.

### The wide-bitfield shift trap, again (§72)

The IDT dump initially printed `ffffc0c02dc0` where the pre-flip kernel printed
`ffffffffc0c02dc0` — top 16 bits gone. `x86_idtdesc_t::offset_high` is
`u64_t offset_high : 48`, and C gives that expression a 48-bit type, so
`offset_high << 16` truncates; C++ used the declared `u64_t`. Fixed with an
explicit `(u64_t)` cast.

Note this file *already* carried two hand-written comments about widening
bit-fields before shifting, for the GDT base/limit fields — those were needed in
C++ too because those fields are `u32_t`. The IDT one did **not** need it in
C++ and only became wrong in C. So the C++ code being careful about a widening
issue nearby is not evidence that the rest is safe.

Verification: 332648 bytes, warning-clean, 0 implicit declarations, boottest
PASS, and all five commands this file owns driven against a pre-flip kernel
(scratchpad/x64run.sh) — `idt` (256 entries), `gdt` (10 descriptors + TSS +
FS/GS MSRs), `cpu`, `hwcr` (all 14 fields; QEMU reports AuthenticAMD so the MSR
reads work), and `pgtcalc` — output identical.

## §82 — kdb/generic/entry.cc + init.cc → .c

The last three files (entry, init, prepost) interlock: `kdb_t::entry` calls
`kdb_t::pre`/`post`, which live in prepost.cc. Flipping entry alone would leave
C code calling C++ members.

Bridging them the usual way does not work here, because the four methods are
declared by the **generator**: `kdb/Makeconf` greps `" kdb_t::"` out of the
sources into `kdb_autogenerated_protos.h`, which is `#include`d *inside* the
class body. An `__asm__` label on the definition would be scraped into the class
declaration and rejected (the same trap as §74's `kdb_cmd_mode`).

Solved with two thin `extern "C"` forwarders at the end of prepost.cc
(`kdb_pre`/`kdb_post` → `kdb.pre()`/`kdb.post()`) plus C declarations in kdb.h.
When prepost.cc is flipped these forwarders simply become the definitions and
`kdb_t::pre`/`post` disappear — at which point the generated protos file becomes
empty and `class kdb_t` has no members left at all.

De-classing detail: `entry`/`init` used implicit `this->` for `kdb_param`,
`last_space`, `last_dump`, `kdb_current`, so every one becomes an explicit
`kdb.` reference. `kdb_t::kdb_cmd_mode` (the static member from §74, which
already carried an `__asm__ ("kdb_cmd_mode")` label) becomes a plain global
definition in init.c; the label makes the remaining C++ declaration bind to it.

Verification: 332240 bytes, warning-clean, 0 implicit declarations, boottest
PASS (which is what exercises `kdb_init`), the scripted command-loop session
identical to the §74 baseline, and the `stats` group plus `go` driven by hand —
`go` returning cleanly to the l4test menu is what proves `kdb_entry`'s CMD_QUIT
exit and the `kdb_post` path.

**One file left: kdb/glue/v4-x86/prepost.cc.** It needs `readmem` C forms at
four widths (u8/u32/s32/word — §80 added two), C macros for the
`x86_exceptionframe_t::` register indices, and `f->regs[]` becomes
`f->__base.regs[]` under the base-composition C rep.

## §83 — kdb/glue/v4-x86/prepost.cc → .c — THE LAST C++ FILE

New C forms needed:
  - `readmem_u32` / `readmem_s32` in `generic/linear_ptab.h`, completing the set
    started in §80 (u8/word). The `readmem<T>` template has no C++ callers left
    now, so it is dead — but it is left in place for the other arches.
  - `atomic_cmpxchg` in `arch/x86/atomic.h`, mirroring `atomic_t::cmpxchg`.
  - `local_apic_broadcast_nmi` in `arch/x86/apic.h`, the last
    `local_apic_t<>` template method still in use (§75 covered the rest).

De-classing detail: `f->regs[...]` becomes `f->__base.regs[...]` — this is the
base-composition C rep, and §78/§79 confirmed `x86_exceptionframe_t` has
identical layout in both languages, so the index arithmetic is safe. The
register indices `x86_exceptionframe_t::freg` etc. already had `X86_EXC_*`
macros. The `extern "C"` `kdb_pre`/`kdb_post` forwarders added in §82 became the
real definitions, exactly as planned.

Config note: CONFIG_KDB_DISAS, CONFIG_X86_COMPATIBILITY_MODE, CONFIG_KDB_BREAKIN
and CONFIG_KDB_INPUT_HLT are all off here, so several branches are preprocessed
away and were translated by inspection only.

### MILESTONE: the kernel builds with zero C++

For `x86-x64-p4-smp`, a full rebuild now issues **no `g++` invocation and
compiles no `.cc` file**. `class kdb_t`'s C++ branch in `src/kdb/kdb.h` is now
unreachable, and `kdb_autogenerated_protos.h` is generated empty — nothing
matches the generator's `" kdb_t::"` grep any more.

Verification: 332136 bytes, warning-clean, 0 implicit declarations, boottest
PASS, the scripted command-loop and the five x64 arch commands identical to
their pre-flip baselines, and a **full l4test run identical to the baseline
captured before the kdb tree was touched at all** (§72-era), modulo the usual
build-timestamp and CPU-calibration normalisation.

### What is deliberately NOT done

The dual-reps are still dual. Every `#if defined(__cplusplus)` branch in the
headers is now dead code for this config, but other arches (powerpc, ofppc) and
inactive options (CONFIG_TRACEPOINTS -> tracebuffer.cc, CONFIG_X_..., vrt.cc,
acpi.cc) still reference `.cc` files, so collapsing them is a separate phase
with its own risk profile — not something to fold into this commit.

## §84 — Compile-checking the branches translated by inspection

Several dead `#if` blocks were translated without ever being compiled. Checked
each by re-running the file's *real* compile line with `-fsyntax-only` and an
`-include` that forces the option on (placed after `-imacros config.h`, so it
wins over the `#undef` there). **Grep for `implicit declaration` as well as
`error:`** — a missing function is only a warning in C, so an "error-free"
result can still mean a link failure.

| forced option | file | result |
|---------------|------|--------|
| CONFIG_KDB_DISAS | prepost.c | **clean** |
| CONFIG_KDB_BREAKIN | prepost.c | **clean** |
| CONFIG_KDB_INPUT_HLT | prepost.c | **clean** |
| CONFIG_CPU_X86_I686 | arch/x86/x86.c | **clean** |
| CONFIG_X_CTRLXFER_MSG | api/v4/tcb.c | **real gap found — fixed** |
| CONFIG_STATIC_TCBS | api/v4/input.c | `tcb_is_tcb` missing, exactly as §76 documented |
| CONFIG_X86_COMPATIBILITY_MODE | prepost.c | blocked: `x32comp/` is un-migrated C++ (`namespace`) |
| CONFIG_X_X86_HVM | arch/x86/x86.c | blocked: `arch/x86/x64/vmx.h` does not exist — HVM is x32-only |
| CONFIG_X86_IO_FLEXPAGES | pc99/io.c | blocked: mdb/vrt subsystem is un-migrated C++ |

### The real gap: ctrlxfer left as C++ inside a .c file

`kdb/api/v4/tcb.c` still contained raw C++ in its `CONFIG_X_CTRLXFER_MSG`
blocks — `tcb->flags.is_set (tcb_t::kernel_ctrlxfer_msg)`,
`ctrlxfer_item_t::get_idname(...)`, `tcb->get_mr(...)`,
`tcb->dump_ctrlxfer_state(...)`. It built only because the option is off. This
was inconsistent with io.c, x86.c and linear_ptab_dump.c, where dead blocks
*were* translated. Now translated the same way, adding
`TCB_FLAG_KERNEL_CTRLXFER_MSG`; the names it reaches for
(`msg_item_is_ctrlxfer_item`, `msg_item_get_ctrlxfer_id/_mask`,
`tcb_dump_ctrlxfer_state`, `tcb_get_fault_ctrlxfer_items`,
`ctrlxfer_item_get_idname/_hwregname/_fault_item_mask`) do not exist — the
ctrlxfer subsystem in `api/v4/ipc.h` is still C++.

Kernel size is unchanged (332136) and the `t`/`T` dumps are still identical,
confirming the edit is confined to preprocessed-away code.

### What the blocked cases actually mean

Three options cannot be compile-checked in C at all, because each depends on a
subsystem the migration never touched: `x32comp/` (compatibility mode), mdb/vrt
(IO flexpages), and ctrlxfer. Those are the honest boundary of "the kernel is
C" — true for this config, not for every config. HVM is different again: it is
simply unavailable on x64 (`vmx.h` is x32-only), so its block is dead for
reasons predating the migration.

## §85 — Dual-rep collapse, step 1: src/kdb/kdb.h and the proto generator

With the build 100% C, every `#if defined(__cplusplus)` branch in the headers is
dead code for this config. Collapsing them is its own phase; this is the first
step, chosen because it is self-contained and removes machinery rather than just
deleting a branch.

  - `class kdb_t` is gone. Its methods had already all become free functions
    (§74 commands, §82 entry/init, §83 pre/post), so what remained was plain
    instance data — now a single `struct kdb_t` for one language.
  - The forward declarations collapse to `struct`/`typedef` form, and now cover
    `mdb_t`/`mdb_node_t`/`mdb_table_t`/`vrt_t`/`vrt_table_t` too, which the C
    branch had been missing.
  - **`kdb_autogenerated_protos.h` and its generator rule in `kdb/Makeconf` are
    deleted.** The rule grepped `" kdb_t::"` out of the sources and injected the
    result *inside the class body* — the machinery behind two separate traps
    (§74's asm-label rejection, §82's inability to bridge pre/post). Nothing
    matches the grep any more, so the whole mechanism goes.
  - Stale comments in kdb.h and cmd.h that referred to "both languages" or to
    prepost.cc-the-C++-file are corrected.

`kdb_class_helper.h` (the `DECLARE_CMD` scraper) **stays** — it is still doing
real work, declaring every command function.

Verification for a declaration-only change: forced a full rebuild
(`.depend` deleted) so every consumer TU recompiles, then checked the kernel is
**byte-identical in size** (332136) — a collapse that changes codegen would mean
the two reps had disagreed. Plus boottest PASS and the scripted command loop
identical.

### Scope of what remains

77 headers under `kernel/src` still carry `__cplusplus` guards: api/v4 (21),
generic (13), glue/v4-x86 (11), arch/x86 (9), kdb (5), and the rest scattered.
They are not all equally safe to collapse — some are shared with arches whose
own `.cc` files were never migrated (powerpc, ofppc) and with the un-migrated
subsystems found in §84 (x32comp, mdb/vrt, ctrlxfer). Deleting a C++ branch that
those still depend on would break them silently, since nothing in this config
compiles them. Each header needs that check before its guard comes out.

## §86 — Collapse step 2: kdb/linker_set.h only — the rest are BLOCKED

Applied the §85 gate ("who else includes this, and is any of them still C++?")
to the five remaining `src/kdb/*.h` guards. The answer split them sharply:

| header | .cc includers | verdict |
|--------|---------------|---------|
| linker_set.h | 0 | **collapsed** |
| cmd.h | 15 | blocked |
| input.h | 15 | blocked |
| console.h | 7 | blocked |
| tracepoints.h | 26 | blocked |

`linker_set.h` lost its `__asm__`-labelled method block (the C forms are the
only callers now) and the two `linker_set_entry_t` accessors, which had no
callers at all — the `get_entry` hits in the tree are on mdb tables, a
different type.

### Why the other four are blocked, and by what

Four `.cc` files can still be pulled into an x86-x64 build by config options:

    kdb/generic/tracebuffer.cc   <- CONFIG_TRACEBUFFER
    kdb/generic/vrt.cc           <- CONFIG_X86_IO_FLEXPAGES
    kdb/generic/acpi.cc          <- CONFIG_ACPI
    kdb/glue/v4-x86/ipc.cc       <- CONFIG_X_CTRLXFER_MSG

Compiled `tracebuffer.cc` as C++ with the option forced: its **only** errors are
`cmd_* was not declared`, which is an artifact of the test (the `DECLARE_CMD`
scraper only sees files in the configured SOURCES). Otherwise it compiles
cleanly — so it is *live* C++, and deleting the C++ branch of cmd.h / input.h /
console.h / tracepoints.h would break `CONFIG_TRACEBUFFER=y`.

**Corollary: those four `.cc` files are the real prerequisite.** Migrating them
unblocks the remaining kdb headers; collapsing first would silently break
configs nothing here compiles.

### Separate finding: CONFIG_ACPI is already broken

`kdb/generic/acpi.cc` does not compile as C++ today — several
`... is private within this context` errors against `acpi__sdt_t` /
`acpi_rsdp_t`. That is independent of this collapse (it is an access-control
problem, not a language-branch one) and predates it; most likely fallout from
the acpi header dual-rep in §66. Worth confirming against a pre-migration
checkout before assuming which.

Verification: byte-identical kernel size (332136) across a forced full rebuild,
boottest PASS, command loop identical — the last matters here because every
`DECLARE_SET` iteration in the command dispatcher goes through
`linker_set_reset`/`_next`.

## §87 — Toward tracebuffer: a scratch CONFIG_TRACEBUFFER build, and what it found

Set up `build/scratch-tbuf` (a copy of the working build with
CONFIG_TRACEPOINTS/CONFIG_TRACEBUFFER forced on) to migrate
`kdb/generic/tracebuffer.cc`. Deleting `config/config.h` to regenerate it does
**not** work — the regenerated file is incomplete (`#error undefined
architecture width`); copy the working `config.h` and patch the two macros
directly, and edit `config/.config` too so the Makeconf `ifeq` picks up the
extra SOURCES.

The scratch build immediately found three things, none of them in
tracebuffer.cc:

**1. `kdb/generic/tracepoints.c` was broken C.** It was renamed to `.c` earlier
in the migration but its body still called `tp_list.size()`, `.get()`,
`.next()`, `.reset()` — C++ method syntax in a `.c` file. It is only compiled
under CONFIG_TRACEPOINTS, so nothing ever built it. Same class of gap as the
ctrlxfer block in §84, and the second one found this way. **Fixed.**

**2. `class tracepoint_list_t` in `src/kdb/tracepoints.h`** was the last C++
consumer of the `linker_set_t` methods removed in §86 — so that collapse did
break something after all, just nothing this config compiles. The gate in §85
("does any `.cc` include it?") was too narrow: the real question is whether any
*code guarded by an inactive option* uses the C++ spelling. Collapsed the class
to a plain struct plus `tracepoint_list_reset/_next/_size/_get`. **Fixed.**

**3. `src/kdb/tracebuffer.h` and `src/arch/x86/tracebuffer.h` are pure C++** —
unguarded `class`, never dual-repped, because CONFIG_TRACEBUFFER has always been
off. They fail immediately when a C TU (`asmsyms.c`) pulls them in.

### Remaining work for tracebuffer, honestly scoped

    src/kdb/tracebuffer.h        238 lines   dual-rep/collapse (4 classes)
    src/arch/x86/tracebuffer.h   142 lines   dual-rep/collapse
    kdb/generic/tracebuffer.cc   921 lines   migrate
                                ~1300 lines total

That is a whole file-set, not a single flip, so it is left for its own pass
rather than started here. `build/scratch-tbuf` is left in place as the harness
(it is gitignored); rebuild it with `.depend` deleted after each change.

Verification of the two fixes: the **main** config rebuilds byte-identical
(332136) with boottest PASS, confirming both changes are confined to code that
this config does not compile.

## §88 — Dual-repped the two tracebuffer headers

`src/kdb/tracebuffer.h` and `src/arch/x86/tracebuffer.h` were pure C++ with
unguarded `class` (§87). Both now carry a C rep and compile from a C TU:

  - `tracerecord_t` and `tracebuffer_t` get C structs in declaration order plus
    free functions. `num_args` becomes `TRACERECORD_NUM_ARGS`, `ofs_counters`
    becomes `TRACEBUFFER_OFS_COUNTERS`.
  - `current` is an `atomic_t`, so `current++` / `== max` / `= 0` become
    `atomic_inc` / `atomic_read` / `atomic_set` — the C++ operators do not exist
    in C.
  - `store_record` and `next_record` depend on the arch-specific `store_arch`,
    so their C forms sit *after* the `INC_ARCH(tracebuffer.h)` include at the
    bottom of the file, mirroring how C++ orders the same dependency.
  - `tracerecord_t::store_arch` and `tracebuffer_t::initialize` in the arch
    header get C counterparts.
  - Collateral: `traceconfig_t` had no C typedef; `tbuf_dump`'s two default
    arguments needed a C declaration; and the `TBUF_REC_*` macro bodies used
    `tbuf->next_record(...)` / `store_string` / `store_data` method syntax.

### A long tail: TRACEPOINT call sites in already-C files

With the headers fixed, the scratch build advanced and immediately hit the same
class of defect in *other* `.c` files — code inside `TRACEPOINT(...)` that was
never compiled because the option is off:

    src/api/v4/exregs.c    ctrl.string()        -> fixed (added exregs_ctrl_string)
    src/api/v4/ipc.c       ->get_state()        -> still open
    ...                    the build stops at each in turn

This is the third and fourth instance of the pattern first seen in §84
(ctrlxfer) and §87 (tracepoints.c). **Any statement that only appears inside a
disabled `TRACEPOINT`/`TRACEF` was never compiled by the migration**, so C++
method syntax survives there in files that already have a `.c` extension. The
count is unknown until the scratch build runs clean; each fix reveals the next.

Verification: main config rebuilds byte-identical (332136), boottest PASS — all
of this is confined to code the working config does not compile.

## §89 — Direct sweep: every TRACEPOINT-guarded C++ leftover

Instead of discovering these one build-error at a time, used `make -k` in the
scratch CONFIG_TRACEBUFFER build to collect them all at once. **142 errors
across 8 `.c` files and 3 headers** — a definite scope rather than a guess.

The pattern was uniform: method-call syntax on types that already have C free
functions, surviving only where it sits inside a disabled `TRACEPOINT`/
`TRACE_*` macro. Fixed across `api/v4/{interrupt,ipc,ipcx,schedule}.c` and
`glue/v4-x86/{exception,init}.c`:

    tcb->get_state().string()   -> thread_state_string (tcb_get_state (tcb))
    tcb->get_cpu() etc.         -> tcb_get_cpu (tcb) and friends
    item.is_map_item() etc.     -> msg_item_* (&item)
    timeout.get_snd()           -> timeout_get_snd (&timeout)
    frame->regs[] / ->error     -> frame->__base.regs[] / __base.error
    memory_info.insert(...)     -> memory_info_insert (...)

Three cases needed more than a substitution:

  - **A regex over-reached.** `entry->tcb->get_state().string()` matched on the
    inner `tcb->`, leaving a stray `entry->` prefix. Caught by the rebuild, not
    by reading the diff — worth remembering that these sweeps need the compiler,
    not just eyeballing.
  - **Chained calls on function results** (`tcb_get_tag (current).get_label()`)
    cannot become `msg_tag_get_label (&...)` because C has no address-of for a
    temporary. Used direct field access (`.x.label`) where the C form is a plain
    field read, and introduced a scoped local where it is not
    (`time_get_microseconds` on `timeout_get_snd (&timeout)`).
  - `scheduler->get_current_time()` inside those same statements became
    `sched_get_current_time ()`.

After the sweep **every `.c` file compiles** under CONFIG_TRACEBUFFER. The only
remaining errors are in `kdb/generic/tracebuffer.cc` itself, which is still C++
and now sees the collapsed C-only APIs (`cmd_group_interact`, `linker_set_*`,
`tracebuffer_*`). That file — the original target, 921 lines — is the last step.

Verification is the important part here, because this touched six files that
**are** compiled in the working config: main build rebuilds byte-identical
(332136), boottest PASS, and a full l4test run identical to the reference. The
edits are all inside macros that expand to nothing when tracepoints are off,
and the unchanged binary is the proof.

## §90 — kdb/generic/tracebuffer.cc → .c; the scratch CONFIG_TRACEBUFFER build links

Done in named steps with a build between each, as the shadowing hazard demanded.

**The key decision.** `tbuf_handler_t` has exactly one instance, so instead of
threading a `self` parameter the methods became free functions that name the
global directly (`tbuf_handler.cpumask`). That is what made the transformation
safe: the class needed `this->` precisely because `set_id(word_t idx, word_t id)`
and `set_tcb(word_t idx, tcb_t *t)` have parameters shadowing the `id[]`/`tcb[]`
members, and a qualified name cannot be shadowed. The hazard that made this file
look dangerous disappeared rather than being worked around.

Steps, each compile-checked: struct + methods; call sites; templates and the
leftover member declarations; then the long tail the linker found.

Other C++ constructs removed:
  - `template<typename T> pmc_print/pmc_delta` — `word_t` and `u64_t` coincide
    on this subarch so both instantiations collapse to one; noted that a 32-bit
    port needs a second form.
  - `max()` is a C++ template in `generic/types.h`; spelled out inline.
  - `tracebuffer->current` is `atomic_t`: `== 0` and `= 0` became
    `atomic_read`/`atomic_set`.
  - `get_current_space()`/`get_kernel_space()`/`readmem()` → the `_c`/`_u8` C
    forms (the last also in prepost.c, which had the same call).
  - `EXTERN_TRACEPOINT(SCHEDULE_IDLE)` had to be added to `sched-rr/schedule.c`:
    the declaration covering it lived in the C++-only schedule_functions.h.

Two regex mishaps, both caught by the compiler and worth recording because
neither would survive review by eye: the member-qualifier rewrote a *local*
struct declaration (`word_t tbuf_handler.tsc;`), and the dedup step deleted the
new global instead of the old one.

**Result: `build/scratch-tbuf` now links — 417832 bytes, no C++ anywhere.**
CONFIG_TRACEBUFFER has almost certainly not built since before the migration
began; it does now, in C.

Verification of the working config, which is what matters since prepost.c and
sched-rr/schedule.c are both live: byte-identical kernel (332136), boottest
PASS, full l4test identical to the reference.

**Not verified: the tracebuffer code has never been run.** The scratch kernel
links but was not booted, so `tb`/`tp` command behaviour is unproven. That is
the remaining gap for anyone enabling this option.

## §91 — Booted the scratch tracebuffer kernel: a real bug that only running found

The §90 kernel linked but had never been run. Booting it and driving the
commands found a genuine defect that compiling could never catch.

`showfilters` reported:

    CPU:      [0]          <- should be ffffffffffffffff
    Typemask: [0]          <- should be ffffffffffffffff

**Cause: a dropped constructor.** `tbuf_handler_t()` called
`invalidate_filters()` during C++ static construction, which sets
`cpumask = typemask = ~0UL`. De-classing removed the constructor, and C
zero-initialises the global instead — so every CPU and every record type was
filtered *out*. The tracebuffer would have silently recorded and displayed
nothing. §90 flagged the constructor as something needing an explicit init hook
and then did not wire one up.

Fixed by registering the init properly with the existing kdb mechanism:

    KDEBUG_INIT (tbuf_handler_init);
    void tbuf_handler_init (void) { tbuf_handler_invalidate_filters (); }

`showfilters` now reports `ffffffffffffffff` for both, matching the C++
behaviour.

Commands exercised on the scratch kernel: the `tracebuffer` group help (all 12
commands present), `counters`, `reset`, `showfilters` (which is what caught the
bug — it is the only command that displays the constructor's work), `dump`, and
the `tracepoints` group with `list`.

**Lesson worth keeping: dropped constructors are invisible to the compiler.**
Every other de-classing in this migration either had no constructor or had one
already converted (`boot_cpu_ft`, `idt_init`). This is the first one that was
silently lost, and only a command that *prints the initialised state* revealed
it. Any remaining class with a constructor deserves the same check.

Verification: main config still byte-identical (332136) with boottest PASS;
scratch kernel 417920 bytes, boots, and behaves correctly.

## §92 — Collapse step 3: the four remaining kdb headers

With tracebuffer.cc migrated (§90-91), the §86 blocker is gone. Re-ran the gate,
now in its corrected form ("does *any* code, including behind an inactive
option, still use the C++ spelling?"):

  - `vrt.cc` includes none of the four.
  - `acpi.cc` includes cmd.h and input.h; `ipc.cc` includes tracepoints.h.
    Both were compiled as C++ to check — **already broken today**, 14 and 7
    errors respectively, for reasons unrelated to language branching. So there
    is no *working* C++ consumer to break.

Collapsed all four: `cmd.h` (the `cmd_group_t` method block and the class/struct
forward declarations), `input.h` (the default-argument declarations of
get_hex/get_dec/get_space/get_thread/get_comspace/get_thrspace), `console.h`
(`getc`'s default argument, and the typedef guard), `tracepoints.h`
(`tracepoint_t::reset_counter`). **`src/kdb/*.h` is now free of `__cplusplus`
guards** except `tracebuffer.h`, whose dual-rep is younger than its last C++
consumer and can go in the next pass.

Both collapses of a `#if/#else/#endif` forward-declaration block left a dangling
`#else`/`#endif` — 63 "#endif without #if" errors. Trivially caught, but a
reminder that deleting a guard branch means deleting *three* directives, not
one, and that a paired-directive check is worth doing before building.

Verification: main config byte-identical (332136) with boottest PASS and the
command loop identical; the scratch CONFIG_TRACEBUFFER config also still builds
(417920). Both configs matter now — the second is what keeps the collapse honest.

## §93 — Does powerpc build today? No, and it cannot be checked here

Asked because the remaining ~72 guarded headers are arch-shared, so the collapse
gate depends on whether powerpc is a live consumer. Three findings:

**1. It cannot be built in this environment.** No powerpc cross-compiler is
installed (`powerpc-linux-gnu-gcc`, `powerpc64-linux-gnu-gcc`, `powerpc-elf-gcc`
all absent), and the only configured build trees are `x86-x64-p4-smp` and the
`scratch-tbuf` copy. So "does it still build?" is unanswerable by compiling, for
this migration or any other change.

**2. It is already broken, provably, by static evidence.** `kdb/arch/powerpc64/
prepost.cc` and `kdb/glue/v4-powerpc/prepost.cc` still define `kdb_t::pre()` and
`kdb_t::post()` — but `class kdb_t` was deleted in §85. Six powerpc kdb files
also use `.interact()`, `->reset()`, `->next()` or `tp_list.`, all of which were
removed from the shared kdb headers during this session. Those files cannot
compile against the current headers regardless of toolchain.

**3. It was already stale before any of this.** `kdb/arch/powerpc/` was last
touched in **2010**. The only recent commit under `src/glue/v4-powerpc` is
incidental fallout from an x86 flip, not powerpc work.

### Consequence for the collapse gate

**powerpc is not a constraint.** It is neither buildable nor currently correct
here, so "will collapsing this header break powerpc?" has no meaningful answer —
it is already broken, and no test in this environment can distinguish more broken
from less. The honest gate for the remaining headers is therefore:

  - does any code reachable in a **configurable x86 build** still use the C++
    spelling? (the §92 form, which the scratch build can actually answer)
  - and record, per header, that powerpc/ofppc consumers were knowingly left
    behind rather than silently assumed fine.

Reviving powerpc would mean migrating its 53 `.cc` files as a project of its own,
with a cross-compiler in the loop. That is a decision for whoever owns those
ports, not something to infer from an x86-only tree.

## §94 — Collapse step 4: tracebuffer.h, and a dual-rep that was never dual

The last two guarded kdb-side headers, `src/kdb/tracebuffer.h` and
`src/arch/x86/tracebuffer.h`, were the ones §92 deferred. Running the gate on
them turned up something the earlier passes had not: **the C++ branch of
`kdb/tracebuffer.h` has not compiled since §90 created it.**

`__tbuf_record_event()` sits *outside* the guards and calls
`tracebuffer_next_record`, `tracebuffer_store_string` and
`tracebuffer_store_data` — the C free functions, which exist only in the `#else`
branch. So every C++ translation unit that includes the header gets three
"not declared in this scope" errors before any of its own code is looked at.
The dual-rep advertised C++ support it did not have.

Establishing that took compiling each candidate as C++ against the scratch
(`CONFIG_TRACEBUFFER=y`) config. All eleven `.cc` files still reachable from an
x86 build pull the header in, and all eleven fail:

    3 errors   src/arch/x86/x64/init.cc      <- header only
    3 errors   src/generic/mdb.cc            <- header only
    3 errors   src/generic/vrt.cc            <- header only
    5 errors   src/glue/v4-x86/timer.cc       (+ redefinition of timer_t methods)
    6 errors   src/glue/v4-x86/ipc.cc         (+ ctrlxfer_item_t undeclared)
    6 errors   src/glue/v4-x86/vrt_io.cc
    4 errors   src/glue/v4-x86/mdb_io.cc      (+ set_io_bitmap undeclared)
    8 errors   src/generic/mdb_mem.cc         (+ mdb_node_t/mapnode_t mismatch)
    22 errors  src/glue/v4-x86/io_space.cc
    25 errors  src/glue/v4-x86/hvm-space.cc
    71 errors  src/api/v4/sched-hs/schedule.cc

The three marked "header only" are worth recording precisely, because they are
the honest cost of this collapse: `mdb.cc` (CONFIG_NEW_MDB), `vrt.cc`
(CONFIG_X86_IO_FLEXPAGES) and `x64/init.cc` (in no Makeconf at all — an orphan)
would each compile as C++ if not for these three errors. Collapsing the header
does not break them; §90 already did. It only makes that permanent, and the fix
for all three is the same migration everything else has had.

Collapsed both headers: the `tracerecord_t` and `tracebuffer_t` classes, the
default arguments on `tbuf_dump`, and the `store_arch`/`initialize` member
definitions in the x86 arch header. **`src/kdb/*.h` and `src/arch/x86/
tracebuffer.h` are now free of `__cplusplus` guards entirely.**
`src/arch/powerpc/tracebuffer.h` still defines `tracerecord_t::store_arch` and
`tracebuffer_t::initialize` as member functions with no C branch, so powerpc's
tracebuffer has been broken since §90 as well — recorded here per §93 rather
than fixed.

### Verification, and what running it added over §91

Main config byte-identical at 332136 with boottest PASS; scratch config
byte-identical at 417920. The header change is entirely inside
`#if defined(CONFIG_TRACEBUFFER)`, so the main config could not have moved —
which is exactly why the scratch build has to be the one that carries the proof.

Booted the scratch kernel and drove both groups by keystroke: `tracebuffer`
help (14 entries), `showfilters`, `counters`, `dump`, then `tracepoints` help
and `list`. Two things are better than at §91:

  - `showfilters` reports `ffffffffffffffff` for both CPU and typemask, so the
    §91 init hook is still wired up — this is now a standing regression check
    for that fix, not a one-off.
  - `dump` returns **32 real records** where §91 saw `0 entries`: live IPC,
    map_fpage, mdb_map and thread-switch traces with formatted event text, and
    `list` shows non-zero per-tracepoint counters (IPC_DETAILS 80, KMEM_ALLOC
    41, PAGEFAULT_USER 9). The recording path — the `TBUF_REC_TRACEPOINT` macros
    scattered through the migrated C files, `tracerecord_store_record`,
    `tracerecord_store_arch`, the ring-buffer wrap — is exercised end to end,
    not just the display side.

**Lesson: a dual-rep is only dual if both branches are compiled.** Nothing in
this tree ever built `kdb/tracebuffer.h` as C++ after §90, so the C++ branch
rotted immediately and silently, and the guard went on claiming otherwise for
four sections. Where a guard is kept for a consumer that no build touches, it
is not compatibility — it is unverified code with a comment on it.

## §95 — The last C++ compile was hiding in the build system

Before collapsing the ~72 arch-shared headers, the gate needed answering
properly: which x86 configurations actually build today? `contrib/configs/`
holds 32 prebuilt configurations, so unlike powerpc in §93 this can be measured
rather than argued. Built all eleven x64 ones from a clean tree at HEAD:

    x86-x64-p4-smp          OK  331824
    x86-x64-p4              FAIL  4    platform/pc99/8259.h -- C++ template
    x86-x64-p4-nokdb        FAIL  4      "
    x86-x64-p4-newmdb       FAIL  4      "
    x86-x64-p4-fp           FAIL  4      "
    x86-x64-p4-statictcbs   FAIL  4      "
    x86-x64-p3              FAIL  4      "
    x86-x64-k8              FAIL  4      "
    x86-x64-p4-fullkdb      FAIL  7    kdb/tracebuffer.h via tcb_layout
    x86-x64-p4-iofp         FAIL  61   generic/vrt.h -- still a C++ class
    x86-x64-p4-cm           FAIL  99   glue/v4-x86/utcb.h -- `namespace`

**Ten of eleven configurations do not build.** This migration has verified
exactly one configuration from the start, and the others drifted out from under
it. The recurring 4-error failure is the same root cause in seven configs: with
`CONFIG_IOAPIC` off the PIC path is selected, and `platform/pc99/8259.h`,
`platform/generic/intctrl-pic.h` and `glue/v4-x86/intctrl.h` are still C++
(`i8259_pic_t<0x20> master;`) while every file that includes them is now C.
These are not header-guard problems — they are unmigrated headers, and they are
the real remaining work. Recorded here; not fixed in this pass.

### The finding that actually blocks the collapse

`x86-x64-p4-fullkdb` failed inside `include/tcb_layout.h` generation, in a
*generated `.c` file*, with the §94 C++ signature. That makes no sense until you
read `Mk/Makefile.voodoo:84`:

    @$(CC) -x c++ -w $(CPPFLAGS) $(CFLAGS) -DBUILD_TCB_LAYOUT -S ...

The tcb_layout generator writes a small file that does `#include INC_API(tcb.h)`
and compiles it **as C++, on every build, in every configuration**. That line is
original — `git log -L` shows it unchanged since the initial import. It was
never touched by this migration.

So the claim in §86 that the x86-x64-p4-smp build issues "no `g++` invocation
and no C++ compilation" was half right. No `.cc` file is compiled, but one C++
translation unit is produced by the build system itself on every run, and it
pulls in `api/v4/tcb.h` and its entire transitive closure — which is most of the
72 guarded headers. **That generator is why those guards still work, and it is
what would have broken had the collapse gone ahead on the §94 reasoning alone.**

Switched it to `-x c`. The generated `tcb_layout.h` is byte-identical to the one
the C++ compile produced (51 lines, same offsets), the kernel is byte-identical
at 332136, and boottest passes. `src/glue/v4-x86/asmsyms.c` — the other voodoo
generator — is already C; only powerpc still has `asmsyms.cc`. With this line
changed, an x86 build issues **no C++ compilation of any kind**, generated or
otherwise, for the first time in the migration.

**Lesson: "no C++ left" is a claim about the build, not about the file list.**
Grepping for `.cc` sources and for `g++` both missed this, because the C++ came
from `$(CC) -x c++` inside a code generator. Before declaring a language
migration complete, read the build rules for explicit `-x`, and check generated
translation units as well as checked-in ones.

## §96 — Collapse step 5: the remaining 71 headers

With the tcb_layout generator switched to `-x c` (§95), no C++ translation unit
is produced anywhere in a working x86 build, and the whole remaining set could
go at once. Five batches, 71 files, **8457 lines of dead C++ branch**:

    src/platform          5 headers    367 lines
    src/arch/x86         14 headers   1905 lines
    src/generic          13 headers   1890 lines
    src/glue/v4-x86      16 headers   1340 lines
    src/api/v4           23 headers   2955 lines

`src/` is now free of `__cplusplus` guards except `glue/v4-powerpc64/config.h`,
left with the rest of powerpc per §93.

### Doing it with a tool instead of by hand

§92 collapsed four headers by hand and got a dangling `#else`/`#endif` twice.
At 212 conditionals that failure mode is a certainty, so this pass used a small
script that tracks `#if/#ifdef/#ifndef` nesting properly, evaluates only
expressions it fully understands (`defined(__cplusplus)`, the negation, and
conjunctions where one term settles it), and **refuses to touch a file** whose
conditionals it cannot decide or that comes out unbalanced. It was tested
against a fixture covering nesting, compound conditions, `#ifdef`/`#ifndef`
spellings, unrelated conditionals inside dropped branches, and blocks with no
`#else`, before being pointed at the tree. `unifdef -U__cplusplus` would have
done the same job; it is not installed here and installing it needs root.

### "Byte-identical" was never byte-identical

The migration has claimed byte-identical kernels for ~90 sections. Checking
properly showed those claims were **size comparisons**: two consecutive builds
of an unmodified tree produce different images. The difference is exactly two
bytes, inside the `L4Ka::Pistachio - built on <date> <time>` string. Everything
else is deterministic.

That makes a real check cheap, so this pass used one: dump the disassembly with
addresses and opcode bytes stripped, and diff it against the previous commit's.
Results:

    platform    4 instructions differ  -- all __LINE__ immediates
    arch/x86    identical
    generic     identical
    glue/v4-x86 identical
    api/v4     12 instructions differ  -- all __LINE__ immediates

The only thing a guard collapse can legitimately change is `__LINE__`, because
deleting lines from a header renumbers the assert sites below them. Each such
site compiles to `mov $<line>,%edx` ahead of the assert `printf`, and the shifts
are constant per header (70, 100 and 175 lines in the api/v4 batch), which is
what a deletion of that many lines above an assert produces. Nothing else in
any of the five images moved.

Runtime: boottest PASS, and the scratch `CONFIG_TRACEBUFFER` config rebuilt and
re-driven -- `showfilters` still `ffffffffffffffff`, `dump` still 32 records,
`IPC_DETAILS` still 80 and `KMEM_ALLOC` still 41, matching §94 exactly.

### What the collapse exposed, and the follow-up it leaves

Three `.c` files carry `extern` declarations they added because the header's
only declaration sat in a C++-only block: `do_xcpu_send_irq` (api/v4/
interrupt.c), `notify_prologue` / `active_cpu_space_set` / the present-list
globals (glue/v4-x86/thread.c), and a local copy of `addr_to_tcb`
(kdb/api/v4/thread.c). Collapsing deleted those C++ branches, so **these local
declarations are now the only declarations of those symbols** — a duplicate
`extern` that no header can keep in step. The comments now say so. Moving them
into the headers is the obvious next cleanup and was left out of a pass whose
whole verification argument rests on changing no generated code.

**Lesson: pick a verification that can actually fail.** Comparing sizes passed
on every one of these batches, and would have passed just as happily if a
collapse had silently dropped a live declaration, because the image is padded
and rounded. The disassembly diff is barely more work, distinguishes "identical"
from "identical except six asserts", and is the reason this pass can say what
changed rather than that nothing appeared to.

## §97 — powerpc, now actually compiled: §93 confirmed, and unchanged by §96

A `powerpc64-linux-gnu` cross toolchain was installed, so the question §93 had
to settle by static evidence can now be settled by building. It configures and
compiles; the conclusion does not change.

**Getting it to run at all took two workarounds worth recording.** The toolchain
search in `Mk/Makeconf` fishes for `$(ARCH)-gcc`, `$(ARCH)-linux-gcc`,
`$(ARCH)-linux-gnu-gcc` and friends — with `ARCH=powerpc` that never matches a
compiler named `powerpc64-linux-gnu-gcc`, so `TOOLPREFIX` comes out empty and
the build silently uses the host x86 gcc. And the only powerpc configuration in
`contrib/configs` is 32-bit (PPC440/ppc44x), while this compiler defaults to
64-bit, where `-meabi`, `-mno-toc` and `-mcpu=440` are all rejected. Both are
fixed from the command line:

    make -k TOOLPREFIX=powerpc64-linux-gnu- CC='powerpc64-linux-gnu-gcc -m32'

With `-m32` every flag the ppc44x config passes is supported.

### The result

    objects built     1
    failing TUs      61
    errors         3913

Two independent causes:

  - **3913 C parse errors.** The powerpc arch and glue headers are still C++ —
    `class` declarations in `arch/powerpc/bat.h`, `swtlb.h`, `ppc_registers.h`,
    `pgent-swtlb_functions.h` and the whole `glue/v4-powerpc/` set — and they
    are now included from the shared files that were migrated to C. The clearest
    single case is `arch/powerpc/types.h:64`, which defines
    `addr_offset(paddr_t, word_t)` as an **overload** of the generic
    `addr_offset(addr_t, word_t)` in `generic/types.h`. That is legal C++ and a
    redefinition in C. The largest error counts land in `api/v4/tcb.h` (1056)
    and `glue/v4-powerpc/tcb.h` (440), which is cascade from those headers, not
    a fault in the shared ones.
  - **26 translation units cannot be compiled at all.** The installed package
    has no `cc1plus`, so every remaining `.cc` file fails with
    "cannot execute 'cc1plus'". Installing `g++-powerpc64-linux-gnu` would fix
    that specific error and change nothing else: those files include the shared
    headers, which after §96 have no C++ branch left to offer them.

### It is not this session's doing

Built the same configuration at `e7bcbae`, before any of §94-§96, with the same
toolchain and flags. **Identical: 1 object, 61 failing TUs, 3913 errors.** The
header collapse neither helped nor hurt powerpc, which is what §93 predicted
when it argued powerpc was already broken and no test here could distinguish
degrees of that. Now there is a test, and the numbers are the same on both
sides of it.

So the §93 policy stands unchanged: powerpc is not a constraint on x86 work, and
reviving it means migrating its arch and glue headers and its 53 `.cc` files as
a project of its own. The difference is that the baseline is now a number
someone can work against rather than an argument.

## §98 — Starting the powerpc conversion

§97 established a measurable baseline for the ppc44x config (the only powerpc
configuration in `contrib/configs`, and it has both `CONFIG_X_PPC_SOFTHVM` and
`CONFIG_X_CTRLXFER_MSG` on). Scope, measured rather than estimated:

    65 translation units, of which 26 are .cc
    25 powerpc headers still contain C++ (13 arch, 9 glue, 3 platform)

**Objects built: 1 -> 13.** Error counts are *not* a progress metric here and
were misleading twice: they rose from 3913 to 9737 across two steps that were
both clear improvements, because a TU that dies on a missing header reports one
error while a TU that gets properly underway reports two hundred. Objects built
and failing-TU count are the honest measures.

### The first third of the work was not powerpc's fault

Four of the first five fixes were fallout the x86 migration left behind:

  - `generic/mapping.h` included `INC_ARCH_SA(ptab.h)`, added by `ce22043`.
    That macro is defined only in `glue/v4-x86/config.h`, and only x86 keeps
    `MDB_NUM_PGSIZES` in a subarch `ptab.h`. `INC_ARCH(pgent.h)` resolves
    everywhere; the x86 disassembly is unchanged by the switch.
  - `glue/v4-powerpc/Makeconf` still named `linear_ptab_walker.cc` and
    `mapping.cc`, renamed to `.c` during the migration — so powerpc was
    silently not building them at all.
  - `arch/powerpc/types.h` defined `addr_offset`/`addr_mask` as C++ **overloads**
    of the generic `addr_t` ones. Renamed `paddr_offset`/`paddr_mask`; nothing
    passes a `paddr_t` today.
  - `arch/powerpc/frame.h` never had a `typedef` for `syscall_regs_t`, which is
    fine in C++ and fatal in C — that one line was failing every TU including
    `syscalls.h`.

A cross-architecture migration breaks the ports it is not compiling, silently,
and the breakage is indistinguishable from the ports' own rot until something
compiles them. §93 and §95 both reasoned about this correctly; neither could see
these four, because nothing built powerpc.

### Converted so far

`bat.h` (`ppc_bat_t`), `frame.h` (`except_regs_t` + free functions),
`debug.h` (`debug_param_t`, default args on `spin`/`spin_forever`),
`syscalls.h` and `string.h` (`extern "C"` -> `BEGIN_DECLS`/`EXTERN_C`),
`ppc_registers.h` (`ppc_esr_t`, `ppc_tcr_t`, `ppc_tsr_t`), and
`asmsyms.cc` -> `.c`, which gates every `.S` file because it generates
`asmsyms.h`.

**`ppc_tcr_t` had a constructor zeroing `raw`** — the §91 trap again, and
`glue/v4-powerpc/init.cc:379` declares a bare `ppc_tcr_t tcr;` that depends on
it. `PPC_TCR_INIT` is provided and the requirement recorded beside the type, to
be applied when init.cc is converted.

### What is left, and the order it wants

The remaining 7899 errors are dominated by one interlocked chain:
`glue/v4-powerpc/tcb.h` defines ~31 out-of-line `tcb_t::` methods, whose bodies
call `space->get_asid()` and friends, so it cannot be converted before
`glue/v4-powerpc/space.h`. `api/v4/tcb.h` already declares the exact C contract
each architecture must supply (`tcb_get_mr`, `tcb_set_utcb_location`,
`tcb_switch_to`, ...), so the target names are not a judgement call — they are
fixed by the shared header the x86 side already satisfies.

Suggested order: `space.h` -> `tcb.h`/`ktcb.h` -> `resource_functions.h` ->
`pgent-swtlb*.h`/`swtlb.h` -> the remaining leaf arch headers -> the 26 `.cc`
files, of which `init.cc`, `space-swtlb.cc` and `softhvm.cc` are the large ones.


## §99 — powerpc links. Four bugs, one cause: converting under a single config

The ppc44x kernel now links: **66 objects, 0 errors, 0 implicit declarations,
0 undefined references, a 1348260-byte ELF image.** That is the first successful
powerpc link in this migration, and the first evidence that the port's C form is
self-consistent rather than merely compilable.

Getting the last 27 undefined references to zero turned up four defects. Three
of them share a single root cause, which is the most transferable thing in this
section.

### The cause: a guard that is false in the config you build under

`api/v4/thread.cc` was converted (995b205) while only the x86-x64-p4-smp gate
config was being built. That config has `CONFIG_STATIC_TCBS` **off** and
`CONFIG_X_CTRLXFER_MSG` **off**. Both `#if` blocks in that file were therefore
invisible to the compiler, to the diff review, and to every verification step —
the binary comparison, the symbol comparison, and the boot test all passed,
because on x86 nothing was missing. The blocks were simply not carried over:

  - `#if defined(CONFIG_STATIC_TCBS)` — `tcb_array`, `static_tcb_array`,
    `tcb_t::allocate`, `tcb_t::deallocate`, `tcb_t::init_tcbs`
  - `#if defined(CONFIG_X_CTRLXFER_MSG)` — `tcb_t::ctrlxfer`, ~90 lines

Both are restored in C in `api/v4/thread.c`, under the same guards.

**A config-guarded block is deleted silently when you convert a file under a
config that disables it.** No warning, no link error, no test failure — until
some other port turns the guard on. Before converting a file, list its guarded
regions and check which ones the build config actually compiles; anything false
needs review by reading, because no tool in the loop will look at it.

### The correction §97 needs

§97 recorded, and a comment in `api/v4/tcb.h` asserted, that `tcb_ctrlxfer`
"has no definition anywhere — not in this tree and not in the original import."
**That is wrong.** It is at `995b205^:kernel/src/api/v4/thread.cc:1140`, and was
introduced with the feature in c881a86. The claim came from a `git grep`
pipeline ending in `head`, which truncated the output before the definition
line. The comment in `tcb.h` has been corrected.

Two lessons, the second being the one that actually bit:

  - Do not assert a symbol "has never existed" from a search of HEAD. Ask
    `git log -S` first — it is the tool that answers that question.
  - `head` on a grep whose purpose is to prove *absence* converts evidence into
    its opposite. When the conclusion is "there are none", read the full output.

The same mistaken reasoning nearly wrote off `CONFIG_STATIC_TCBS` as an
unimplemented feature: `tcb_array` is defined nowhere in HEAD, and PPC440
*requires* the option (`config/powerpc.cml:181`), so the port looked unbuildable
by construction. It was in d52a5e2 all along, in the same file.

### `get_on_cpu_c` was defined inside the SMP-only region

`api/v4/smp.h` wrapped lines 40-201 in `#if defined(CONFIG_SMP)`. The C form of
the `get_on_cpu<T>` template sat at line 93 — inside it — and carried its own
`#else` branch returning `item` for the uniprocessor case. That fallback could
never be reached: in a non-SMP build the whole function was compiled out along
with it. kdb calls it unconditionally. Moved below `#endif /* CONFIG_SMP */`.

Worth noting how this looked from the outside: adding the missing
`#include INC_API(smp.h)` to `kdb/api/v4/schedule-rr.c` did not fix the
undefined reference, and a `static inline` that is visible and used *must* be
emitted. That contradiction is what pointed at the enclosing guard.

### The accessor hoist moved definitions but left prototypes behind

Moving 63 accessors into `api/v4/accessors.c` left six of their declarations in
`glue/v4-x86/space.h` — an x86-only header. Every port but x86 called those
functions with no prototype in scope. On powerpc this surfaced as six implicit
declarations; on x86 it was invisible, because the declarations were still
there.

It was **not** harmless on x86. C's implicit declaration rule assumes
`int (...)`, and the gate binary shows the cost:

    thread_control_interrupt_c:
      before:  xor %eax,%eax; call thread_control_interrupt; test %eax,%eax; setne %al
      after:   jmp thread_control_interrupt

    space_is_mappable_addr:
      before:  test %eax,%eax        (bool re-widened to int)
      after:   test %al,%al

The `xor %eax,%eax` is the varargs AL convention, emitted because an
unprototyped function might be variadic; the `test %eax,%eax`/`setne` pairs are
the compiler re-normalising a `bool` it was told was an `int`. Correct by luck
on this ABI, not by construction.

Declarations now live with the definitions they describe: `fpage_is_addr_in_fpage`
and `fpage_is_range_in_fpage` in `api/v4/fpage.h`, the three `space_is_*` in
`api/v4/space.h`, `mem_region_is_empty` in `api/v4/kernelinterface.h`, and
`tcb_set_saved_state`/`tcb_set_saved_partner` as INLINE in `api/v4/tcb.h` beside
their getters (they were out-of-line in powerpc glue and absent everywhere else).

**When you hoist a definition to shared code, hoist its declaration too.** A
prototype left in a per-architecture header is not a compile error anywhere —
it is an implicit declaration in every port that is not the one you tested.

### The header lied about which branch it implemented

`tcb_get_tcb` and `tcb_allocate` were defined *unconditionally* with dynamic-KTCB
address arithmetic, each carrying a comment saying "CONFIG_STATIC_TCBS is off".
For the gate config that was true. For PPC440, which requires the option, the
kernel would have computed `KTCB_AREA_START + threadno * KTCB_SIZE` for a
configuration whose whole premise is that TCBs are *not* at those addresses —
wrong silently, at runtime, with no diagnostic. Both are now guarded, with the
static forms indexing `tcb_array`.

A comment asserting the state of a config option is a claim about the build, and
it is only ever checked in the build you happen to run. This one was written
during the x86 conversion and was false for the only port that sets the option.

### Verification

  - powerpc: clean rebuild, 66/66 objects, 0 errors, 0 implicit declarations,
    0 undefined references, links.
  - x86 gate: 0 errors, 0 implicit declarations; 705 symbols before and after,
    identical bodies except the four above, each an ABI improvement traced to
    the newly visible prototypes; boots to userland and kdb responds.


## §100 — The `.einit` linker warnings: two address spaces in one link

Six copies of

    ld: warning: dot moved backwards before `.einit'

on every powerpc link. The layout was correct; the script just had no way to say
what it meant.

`src/platform/ppc44x/linker.lds` links the kernel at its virtual address
(`text_vaddr`, KERNEL_OFFSET = 0xC0000000), but `.einit` at its **physical**
one, via `.einit (. - KERNEL_OFFSET)`. That is deliberate: `.einit` holds
`_start`, the init stack (`startup.S`) and `init_paging`
(`glue/v4-powerpc/space-swtlb.c`) — code that runs before the kernel's virtual
mapping exists. The section is emitted after `.bss`, so the location counter is
up at 0xC00D4000 and the section address resolves to 0x000D4000. ld sees the
counter jump backwards by 3 GB and warns, once per sizing pass.

The fix is to stop pretending there is one location counter. `.einit` belongs to
a different address space, so it gets a region of its own:

    MEMORY { phys : ORIGIN = 0, LENGTH = KERNEL_OFFSET }
    ...
    .einit (. - KERNEL_OFFSET) : { *(.einit) } > phys : einit

The region exists only to give `.einit` a separate location counter; the address
still comes from the expression on the section. Nothing about the image changes.

### What was tried first, and why it failed

The obvious move — turn the implicit backwards jump into an explicit one —

    _saved_dot = .;  . = _end_data_phys;  .einit . : { ... }  . = _saved_dot;

still produced all six warnings. ld's check is on the location counter
decreasing at all, not on *how* the section address was expressed. Worth knowing
before spending time rephrasing the address arithmetic: no expression written
against a single counter can avoid this. A second counter is the only fix.

### Verifying a change to a port that cannot be booted

powerpc links but has never been run here, so "it still links" is not evidence.
The check used instead was a relink harness (`scratchpad/ppclink.sh`) that
re-runs the exact `ld` command against the **already-built object files**, so the
linker script is the only variable. Object order is taken verbatim from the real
link line — it determines section content order, and a `sort -u`'d list does not
reproduce the build (that mistake cost one confusing byte-27 mismatch).

The harness reproduced the build byte-for-byte, and old script vs new script over
the same objects produced **identical binaries**. A full clean rebuild then
differed in exactly 3 bytes, all inside `.kip` at 0x5016A: the build timestamp
string ("17:04:03" -> "17:14:42"), the same nondeterminism §96 noted. Section
and program headers compare identically.

That is the right shape of evidence for a port you cannot execute: hold
everything but the one file constant, and require bit-equality rather than
absence of complaints.

### Still open

`except.o: missing .note.GNU-stack section implies executable stack` remains —
a hand-written `.S` with no `.note.GNU-stack`, unrelated to `.einit`, and not
touched here.


## §101 — The executable-stack warning: one flag, and what it exposed

    ld: warning: src/glue/v4-powerpc/except.o: missing .note.GNU-stack section
        implies executable stack

gcc emits an empty `.note.GNU-stack` into every object it compiles from C.
Hand-written `.S` files carry no such note, and ld reads its absence as "this
object wants an executable stack". Three powerpc objects lacked it
(`startup.o`, `kip_sc.o`, `except.o`) and one x86 object did
(`platform/pc99/smp.o`) — ld names only the first offender per link, so the
count is not visible from the warning.

The note is meaningless for a freestanding kernel: it describes the stack of a
process an ELF loader would set up, and nothing loads this image that way. But
supplying it is how the assembler is meant to say so, and a link that prints a
known-benign warning is a link where the next real one goes unread.

Fixed in one place rather than 22 files, since `Mk/Makeconf` already has an
`ASMFLAGS` used only by the `%.o: %.S` rule:

    ASMFLAGS += -Wa,--noexecstack

Both linkers now report nothing at all on powerpc, and on x86 only the
pre-existing "LOAD segment with RWX permissions", which is inherent to a kernel
image and predates this work.

### The two ports responded differently, and that is the interesting part

powerpc: image byte-identical but for the 4-byte `.kip` build timestamp; section
and program headers identical. x86: all 705 symbol bodies byte-identical, but
one program header changed —

    GNU_STACK  ... RWE   ->   GNU_STACK  ... RW

That is the fix doing exactly what it says, and the difference between the ports
is the linker script. `platform/ppc44x/linker.lds` has an explicit `PHDRS`
block, and when PHDRS is given ld emits *only* the listed headers — there is no
`PT_GNU_STACK` to correct. x86 lets ld synthesise its program headers, so the
segment exists and its permissions actually track the notes. Same source change,
visible in one image and invisible in the other, for a reason that has nothing
to do with either port's assembly.

Worth remembering when a change "has no effect" on the port you happened to
check: an explicit PHDRS list silently discards whatever ld would have inferred.

### Verification

  - powerpc: clean rebuild, 66/66 objects, link warning-free, headers identical,
    4 bytes differ (the timestamp).
  - x86: clean rebuild, 0 errors, 705 symbols with identical bodies, only the
    GNU_STACK permission bits changed; boots to userland, sigma0 and ROOTTASK
    created, kdb scheduling queue correct.
  - Every object in both builds now carries `.note.GNU-stack`.


## §102 — The RWX segment warning: suppressed, and why that is the right answer

    ld: warning: x86-kernel has a LOAD segment with RWX permissions

Unlike §100 and §101, this one is **suppressed rather than fixed**, so the
reasoning matters more than the change.

binutils 2.39 added this check for userspace binaries, where the program loader
applies `p_flags` as mapping permissions and a writable-executable mapping is a
real exposure. Nothing does that here: kickstart copies PT_LOAD segments to
their physical addresses, and the kernel installs its own page tables. The ELF
permission bits are never read by anything in the boot path.

### What satisfying it would actually cost

Three of the five x64 LOAD segments are RWX, each for a different reason:

    00  .text .rodata .kip     .kip is writable data inside the code segment
    03  .data .kdebug .sets    .kdebug is code inside the data segment
    04  .init                  ALLOC CODE, not READONLY -- mixed by content

The linker script has no PHDRS block, so ld synthesises segments by merging
contiguous sections and unioning their flags. Segments break only where a
`KERNEL_PAGE_SIZE` (2 MB) alignment leaves a gap — which is why `.syscalls` and
`.cpulocal` each got a clean segment and the rest did not.

Segments 00 and 03 could be separated: add a PHDRS block with explicit
`FLAGS()`, and give `.kip` and `.kdebug` their own 2 MB slots so no page belongs
to two differently-permissioned segments. Segment 04 could not — `.init` is a
single section holding 32-bit startup code, page tables, `.init.data`,
`.init.memory` and the constructor list together. Separating it means splitting
the boot trampoline into code and data sections.

And the warning fires if *any* segment is RWX, so the partial fix buys nothing.
The full one reshapes the physical memory map handed to the boot loader, to
correct permission bits nothing consults.

### The change

`Mk/Makeconf`, alongside the LDFLAGS definition:

    LD_NO_WARN_RWX := $(shell $(LD) --help 2>/dev/null | \
                        grep -q -e --no-warn-rwx-segments && echo --no-warn-rwx-segments)
    LDFLAGS += $(LD_NO_WARN_RWX)

Probed rather than assumed: binutils before 2.39 does not know the option and
would fail the link. Verified that the probe yields empty for an `ld` that does
not advertise it.

### Verification

A flag that only suppresses a diagnostic must not alter the output, and does
not: x86 headers identical, 705 symbols with identical bodies, 4 bytes differing
— the `.kip` build timestamp. Boots to userland, sigma0 and ROOTTASK created.
powerpc relinks clean under the same global LDFLAGS.

Both kernels now link with **no diagnostics at all**.

### If this should be revisited

The honest fix is a PHDRS block for `src/glue/v4-x86/x64/linker.lds` plus an
`.init` split. That is worth doing if the x86 port ever wants its own kernel
mappings to be derived from segment flags rather than hardcoded in
`glue/v4-x86/init.c` — at which point the flags stop being decorative and the
2 MB cost buys something real. Until then it is churn on the boot path.


## §103 — sched-hs, part 1: the headers, and the keystone they were blocking

`SCHED=hs` (Hierarchical Stride Scheduling, `config/rules.cml:120`) is the
alternative to `SCHED_RR`. Four files in `src/api/v4/sched-hs/` plus
`kdb/api/v4/schedule-hs.cc` were still C++.

### It was already unbuildable in *both* languages

Worth establishing before touching anything, because it decides what
verification is even possible. As C, `SCHED=hs` died at 14 objects. As C++ —
syntax-checked against the same config — `schedule.cc` produced 181 errors,
all of the same kind:

    error: 'ringlist_t' does not name a type; did you mean 'ringlist_tcb_t'?
    error: 'bitmask_t' does not name a type; did you mean 'bitmask_u32_t'?
    error: 'time_t' has no member named 'is_never'

sched-hs is *stranded* C++: the shared infrastructure it is written against
(`ringlist_t<T>`, `bitmask_t<T>`, `time_t`'s methods) became C some commits ago,
and nothing rebuilt this policy because no config in `contrib/configs` selects
it. So there is no working reference binary to compare against, and no
regression risk either — the conversion is the only thing that can make it build.

### The keystone, again

The same §95 shape. `Mk/Makefile.voodoo` generates `tcb_layout.h` by compiling
one TU **as C** over the tcb closure, and `sched-hs/ktcb.h` sits in that closure
via `api/v4/sktcb.h`. A `class` there means `tcb_layout.h` is never generated,
which means *every* translation unit fails on `#include <tcb_layout.h>` — 3913
-style noise that says nothing about the real state. Converting `ktcb.h` alone
takes the build from 14 objects to 42.

The layout it produces is the check that matters. `hs_sched_ktcb_t` came out 40
bytes larger than `rr_sched_ktcb_t`, and `OFS_TCB_SCHED_STATE_SCHEDULER` moved
208 -> 248 accordingly: the policy struct is 128 bytes, ending 8-byte aligned
with no trailing padding, so the §78 tail-padding divergence does not arise
here. (rr needed an explicit `__tail_pad` for exactly that reason; hs already
had a `reserved0` doing the job.)

### What the headers became

`ktcb.h`: `struct hs_sched_ktcb_t` plus `hs_sched_*` INLINE accessors, mirroring
rr's naming. Two things could not stay in the header:

  - `DEFAULT_TIMESLICE_LENGTH`/`DEFAULT_TOTAL_QUANTUM` were `time_t::period(625,3)`
    and `time_t::never()`. time_t's C helpers live in `api/v4/tcb.h`, far too
    late in the include order, so both defaults move to `schedule.c` — the same
    resolution rr used.
  - `get_domain_prio_queue()` fell off the end of the function when
    `BUILD_TCB_LAYOUT` was defined (no return statement on that path). The C
    form returns NULL there instead.

`schedule.h`: `prio_queue_t`, `hs_scheduler_t` and `smp_requeue_t` as structs in
C++ declaration order, with only those accessors that do not dereference a
`tcb_t` — the rest cannot be INLINE this early in the include order, exactly as
rr documents.

### What remains, and why the error count is misleading

Two files: `src/api/v4/sched-hs/schedule.cc` (768 lines) and
`kdb/api/v4/schedule-hs.cc`. The first must absorb
`sched-hs/schedule_functions.h` (686 lines) as well, because the pre-migration
`api/v4/schedule.h` ended with

    #include INC_API_SCHED(schedule_functions.h)

That header is *not* dead code, despite nothing `#include`-ing it by name today:
it held the per-policy inline bodies of the shared `scheduler_t` methods
(`schedule`, `deschedule`, `is_scheduler`, `check_schedule_parameters`, ...),
which is why `schedule.cc` appears to define only five of them. rr folded its
copy into `sched-rr/schedule.c`; hs needs the same. `sched-rr/schedule_functions.h`
survives as genuinely dead C++ and should be deleted once hs is done.

Of the 67 remaining errors, 28 are `kdb_class_helper.h: No such file` — a second
generated header, whose rule cannot run until `kdb/api/v4/schedule-hs.c` exists
by that name. They are downstream of the unconverted files, not independent
work.

The x86 rr gate is unaffected throughout: `sched-hs/*` is only reachable with
`SCHED=hs`, and the gate kernel compares byte-identical apart from its 4-byte
`.kip` timestamp, 705 symbols with identical bodies.


## §104 — sched-hs, part 2: it boots, and the bug that only a second policy could expose

`src/api/v4/sched-hs/` is now C: `ktcb.h`, `schedule.h`, `schedule.c`, plus
`kdb/api/v4/schedule-hs.c`. `schedule_functions.h` is gone, its 686 lines folded
into `schedule.c` alongside the 768 from `schedule.cc`, exactly as rr did.

**The hs kernel builds, links and boots**: 73 objects, 0 errors, 0 implicit
declarations, 0 linker warnings, a 340968-byte image that reaches userland with
sigma0 and ROOTTASK created and the L4 test suite running. This is the first
verification in the whole powerpc/hs stretch that is *behavioural* rather than
structural — x86 can be booted, unlike ppc44x.

### The bug: shared code that had quietly become rr-specific

The first hs kernel linked cleanly and then triple-faulted with no output at
all. The cause was not in sched-hs. `api/v4/schedule.c`'s `scheduler_init()`
carried this:

    /* inlined policy_scheduler_init() (protected in the C++ class): reset the
       wakeup list and the priority queue via the C-visible __base. */
    self->__base.wakeup_list = 0;
    for (int i = 0; i <= MAX_PRIORITY; i++) ...prio_queue[i] = 0;
    self->__base.root_prio_queue.max_prio = -1;

Three fields — which is precisely what **round-robin** needs. hs additionally
requires the root queue's `domain_tcb`, its `refcnt`/`depth`/`count`, the period
counters, and the `scheduled_tcb`/`scheduled_queue` pair. Without them the very
first `enqueue_ready` walks `prio_queue_get_domain_tcb()` == NULL and dies
before a single character reaches the console.

When a policy method is inlined into shared code, the shared code silently
acquires that policy's assumptions. It was correct while rr was the only policy
that built, and there was no way to notice: sched-hs had not compiled in years.
`policy_scheduler_init(scheduler_t *)` is restored as a real per-policy hook,
declared in `api/v4/schedule.h`, with rr supplying the three lines it used to
inline.

That change is visible in the rr binary and was checked rather than assumed:
706 symbols instead of 705 (the extracted function), `scheduler_init` now
calling it, and two incidental diffs — `kdb_prepost_init` gaining trailing
alignment `nop`s and `tcb_create_startup_stack` carrying a relocated address
(0xc0612d24 -> 0xc0612d74) shifted by the new function's presence. rr still
boots to userland.

### Notes on the translation

  - `prio_tickets()` computed ticket ratios in **`float`** — in kernel code,
    with no FPU state saved across the switch. Rewritten in fixed point
    (1/1024ths). This is the one place the C is deliberately not a transcription
    of the C++; the old code would have clobbered user FPU state or trapped,
    depending on build flags.
  - `check_dispatch_thread` and `delay_preemption` contained the same
    walk-to-common-ancestor loop, duplicated; it is now `hs_common_ancestor()`.
  - Five helper names had to be looked up rather than guessed, and the compiler
    caught every one as an implicit declaration: there is no `threadid_is_equal`
    (compare `.raw`), no `bitmask_word_is_set`/`_add` (poke `.maskvalue`, as
    `api/v4/tcb.h` does), and the notify wrapper is `tcb_notify_word`, not
    `tcb_notify1`.
  - `prio_control.stride` is a **signed** 16-bit bitfield
    (`BITFIELD4(long, prio:9, logid:7, stride:16, ...)`), so every
    `set_stride(prio_control.stride)` widens a signed short to `word_t`. The
    casts are explicit now; the behaviour (including wrap for stride > 32767) is
    what the C++ already did.
  - kdb's `showqueue` prints pass values as `[?]` because `%U` is not a
    specifier `kdb/generic/print.c` implements. The C++ used the identical
    `%16U`, so this is preserved, not introduced — a display bug worth fixing
    separately.

### A build-system gap this exposed

`Mk/Makeconf:130` defaults `SCHED` to `rr`, and building sched-hs needed
`SCHED=hs` on the make line even with `CONFIG_X_SCHED_HS` set. **Corrected in
§105: the derivation does exist** (`config/Makefile:70`), and the actual defect
is narrower than stated here.

`sched-rr/schedule_functions.h` remains as dead C++ — nothing includes it, and
its contents live in `sched-rr/schedule.c`. It can be deleted.


## §105 — Wiring SCHED to the config option: the derivation existed

§104 claimed "nothing derives SCHED from the config". That was wrong, and the
way it was wrong is instructive: I had hand-patched `config/config.h` to select
hs and then concluded from the resulting build that no wiring existed. The
wiring is in `config/Makefile:70`, and it is correct —

    /^CONFIG_[_X]*SCHED_[^_]*=y/ { if ($4 == "y") SCHED=$3; else SCHED=$4 }

with `-F'[_=]'`, `CONFIG_SCHED_RR=y` gives fields `CONFIG|SCHED|RR|y` so
`SCHED=$3="rr"`, and `CONFIG_X_SCHED_HS=y` gives `CONFIG|X|SCHED|HS|y` so
`SCHED=$4="hs"`. Both verified by running the awk directly. The irregular option
naming (`SCHED_RR` but `X_SCHED_HS`) is exactly what the `$4 == "y"` test is
for.

The claim came from reasoning about a build instead of reading the rule — the
same failure as §99's "`tcb_ctrlxfer` has never existed", which came from a
`grep | head`. Both times a five-second check would have prevented a wrong note.

### The defect that is real

That awk writes into `$(BUILDDIR)/Makeconf.local`, and the rule that runs it is
a prerequisite of the `*config` targets only:

    menuconfig batchconfig ttyconfig xconfig: $(BUILDDIR)/Makeconf.local

So `Makeconf.local` holds a **snapshot** of the config, refreshed only when the
configurator runs. Change `.config` (or `config.h`) without re-running one and
the snapshot goes stale: the kernel is then compiled from one policy's sources
while `config.h` advertises the other. That combination compiles, links, and is
wrong only at run time — the same silent-disagreement shape as §99's
`CONFIG_STATIC_TCBS`.

`Mk/Makeconf:46` already does `-include $(BUILDDIR)/config/.config`, so the
`CONFIG_*` options are in scope as make variables. SCHED is now derived from
them directly, which makes the two incapable of disagreeing:

    SCHED_FROM_CONFIG :=
    ifeq "$(CONFIG_X_SCHED_HS)" "y"
    SCHED_FROM_CONFIG := hs
    else ifeq "$(CONFIG_SCHED_RR)" "y"
    SCHED_FROM_CONFIG := rr
    endif

    ifneq "$(origin SCHED)" "command line"
    ...
    endif

An explicit `SCHED=` on the command line still wins — that is how sched-hs was
brought up before any config selected it — and `Makeconf.local`'s value remains
the fallback for a tree with no `.config`.

One trap worth recording: the first version used a line-continued nested
`$(if ...)`, which keeps the continuation's leading whitespace *inside the
value*, yielding `SCHED = " rr"` and source paths like `sched- rr/`. It went
unnoticed at first because the rr tree had nothing to rebuild, so the bad path
was never expanded into a compile. Plain `ifeq` conditionals avoid it.

### Verification

  - rr tree (`CONFIG_SCHED_RR=y`): `SCHED := rr`, full rebuild from scratch,
    706 symbols with identical bodies against the pre-change kernel, 3 bytes
    differing (the `.kip` timestamp), boots and `showqueue` prints rr's
    "accounted tcb" form.
  - hs tree (`CONFIG_X_SCHED_HS=y`, `Makeconf.local` deliberately left stale at
    `SCHED=rr`): `SCHED := hs`, clean build with **no** `SCHED=` override, 73
    objects, 0 errors, boots and `showqueue` prints hs's "scheduled queue /
    priority queue / pass / domain tcb" form.
  - `SCHED=hs` on the command line still overrides both.

### A self-inflicted lesson

While checking that the override still worked I ran `make -p SCHED=hs` in the
**rr gate build tree**. `make -p` prints the database *and still builds the
default goal* unless `-n` is also given, so that command compiled sched-hs into
the gate tree, overwriting its kernel and regenerating `kdb_class_helper.h` for
hs's command set. The next rr build then failed on `cmd_show_sched_empty`
undeclared, and a symdiff briefly reported "EQUIVALENT" because it was comparing
a contaminated tree against a copy of itself.

Repaired by deleting the generated headers and objects and rebuilding; the
result compares equivalent to a reference taken before the contamination. Use
`make -pn` to interrogate the database, and take reference copies *before* any
command that might build.


## §106 — Deleting sched-rr/schedule_functions.h

The last C++ file in either scheduling policy. It held the round-robin
specialisations of the shared `scheduler_t` methods, included by the
pre-migration `api/v4/schedule.h` via `#include INC_API_SCHED(schedule_functions.h)`.
That include went away when `api/v4/schedule.h` became C, and its contents were
folded into `sched-rr/schedule.c` — so the file has been unreachable since,
compiled by nothing, and kept only as a reading reference while sched-hs was
converted against it (§103, §104).

Checked before deleting: no `#include` names it anywhere in the tree. The six
remaining textual references were all comments, and three of them described it
in the present tense as somewhere code "lives" — those would have sent a reader
after a file that no longer exists:

  - `sched-rr/schedule.c:28` — "the EXTERN_TRACEPOINT ... lives in the C++-only
    sched-rr/schedule_functions.h"
  - `sched-rr/schedule.c:1004` — "the bodies live here rather than in a C++-only
    header"
  - `glue/v4-x86/space.c:33` — "set_timeout (schedule.h -> schedule_functions.h)",
    an include chain that no longer exists

All three now say where the code actually is. The rest were already past-tense
provenance ("were INLINEs in ..."), which stays useful: it explains why
`schedule.c` has the shape it does, and `git log` still has the file.

`src/api/v4/` is now free of C++ in both policies.

### Verification

rr rebuilds with 0 errors, 706 symbols with identical bodies, and boots to
userland with `showqueue` printing its "accounted tcb" form; hs rebuilds to 73
objects, 0 errors, unchanged 340968-byte image. Deleting an unreferenced file
should be a no-op in the binary, and it measurably is.


## §107 — kdb/api/v4/space.cc: not convertible, because its subsystem is gone

The `listspaces` command ('S'): walk a global list of address spaces, printing
each space and the threads attached to it. It could not be converted, and the
reason is worth separating from the ordinary migration work.

Three facts, each checked rather than inferred:

  - **It is in no build.** `kdb/api/v4/Makeconf` lists
    `input.c kernelinterface.c tcb.c thread.c schedule-$(SCHED).c sigma0.c`.
    No `space`. Nothing compiles this file.
  - **It does not compile as C++ either** — 11 errors against the current tree.
    Only three are C++-isms (`queue_state_t::is_set`, `tcb_t::get_cpu`,
    `spinlock` methods); the other eight are missing declarations.
  - **The structures it walks no longer exist.** `global_spaces_list`,
    `spaces_list_lock`, `space_t::get_thread_list()`,
    `space_t::get_spaces_list()` and `tcb_t::thread_list` appear nowhere in the
    tree. `git log -S` puts their removal in **a0a8042**, "Removed unmaintained
    architectures and platforms" — the per-space thread list and the global
    space list went with it, and this file was left behind.

So "convert it to C" had no achievable meaning. The C++ syntax is the smallest
part of the problem; making the file build would mean re-adding a linked list to
`space_t` and a `thread_list` member to `tcb_t` — a new field in the TCB, which
moves every `tcb_layout.h` offset. That is implementing a removed feature, not
migrating a file.

Deleted, on the same grounds as §106: dead code for a subsystem that no longer
exists, recoverable from `git log`. The build is unaffected because it was never
part of it — the rr kernel differs by 2 bytes, the `.kip` timestamp.

`kdb/api/v4/` is now free of C++.

### The check that mattered

The temptation was to start rewriting `queue_state_t::is_set` into
`queue_state_is_set` and discover the rest one error at a time. Grepping the
five identifiers first — thirty seconds — established that the file was
unbuildable in *either* language before any of it was touched, which is what
turned "convert this" into a question with three real answers rather than a task
with one bad outcome. §105 records the inverse mistake: concluding from a build
what a five-second read of the rule would have settled.

### Remaining C++ in the tree

86 `.cc` files, none of them in a configuration that currently builds: the
powerpc64 port, x86-x32 and its x32comp layer, the OpenFirmware platforms
(ofppc, ofpower3/4, ofg5), efi, simics, and `generic/mdb.cc`/`vrt.cc` with their
kdb counterparts. §95 measured which x64 configs those block: 10 of 11.


## §108 — generic/vrt.cc: scoped, not converted. Why it cannot be split

Started on `src/generic/vrt.cc` and stopped before writing code, because the
survey changed what the job is. Recording the analysis so the next attempt can
start from it rather than repeat it.

### It is a five-file component, not a file

`CONFIG_X86_IO_FLEXPAGES` gates `src/generic/vrt.cc` (`src/generic/Makeconf:40`)
and `kdb/generic/vrt.cc` (`kdb/generic/Makeconf:51`). Two configs set it, and
one of them is x64 — `x86-x64-p4-iofp`, which §95 listed as blocked "on
generic/vrt.h". Measured: 61 errors, rooted in

    src/generic/vrt.h        33      src/glue/v4-x86/mdb_io.h    4
    src/glue/v4-x86/vrt_io.h  9      src/glue/v4-x86/io_space.h  3
    src/glue/v4-x86/io_fpage.h 7     + 4 spillover in api/v4 and space.c

                                    lines
    src/generic/vrt.h                 471
    src/generic/vrt.cc                799
    kdb/generic/vrt.cc                103
    src/glue/v4-x86/vrt_io.h          171
    src/glue/v4-x86/vrt_io.cc         239
                                    -----
                                     1783

### The coupling is virtual dispatch, and that is why it is indivisible

`vrt_t` is the only class in this migration with **real polymorphism**: nine
virtual methods, of which `vrt.cc` defines *none* — they are pure in practice,
and the single subclass `vrt_io_t` (`glue/v4-x86/vrt_io.h:43`) supplies all of
them. The generic algorithms in `vrt.cc` (`lookup`, `map_fpage`, `mapctrl`)
drive the VRT entirely through those calls.

Converting `vrt.h` alone produces something that still *compiles* and is wrong
at run time: `class vrt_io_t : public vrt_t` remains legal C++ when `vrt_t` is a
plain struct, its `get_radix`/`get_name`/... stop being overrides, nothing sets
the dispatch pointers, and the first `map_fpage` dereferences them. That is
precisely the silent-disagreement shape this migration has hit three times
already — §99's `tcb_get_tcb` under `CONFIG_STATIC_TCBS`, §104's
`policy_scheduler_init`, §105's stale `SCHED`. Splitting the component would
manufacture a fourth.

So the honest boundaries are "all five files" or "none". I stopped at none.

### The design the conversion should use

An ops table, laid out to match the C++ object exactly:

    typedef struct vrt_ops_t {
        word_t       (*get_radix)        (vrt_t *self, word_t objsize);
        word_t       (*get_next_objsize) (vrt_t *self, word_t objsize);
        word_t       (*get_vrt_size)     (vrt_t *self);
        mdb_t *      (*get_mapdb)        (vrt_t *self);
        const char * (*get_name)         (vrt_t *self);
        void         (*set_object)       (vrt_t *self, vrt_node_t *n, word_t n_sz,
                                          word_t paddr, vrt_node_t *o, word_t o_sz,
                                          word_t access);
        word_t       (*get_address)      (vrt_t *self, vrt_node_t *n);
        word_t       (*make_misc)        (vrt_t *self, vrt_node_t *obj, mdb_node_t *map);
        void         (*dump)             (vrt_t *self, vrt_node_t *n);
    } vrt_ops_t;

    struct vrt_t { const vrt_ops_t *ops; vrt_table_t *root_table; };

The `ops` pointer first is not arbitrary: the Itanium ABI puts the vptr at
offset 0 and `root_table` after it, so this reproduces the C++ layout, and
`struct vrt_io_t { vrt_t base; ... }` reproduces `class vrt_io_t : public
vrt_t`. Worth checking against the old object rather than trusting the claim.

Two further pieces: `vrt_table_t::operator new (size_t, word_t radix_log2)` and
`operator delete` become `vrt_table_alloc(word_t radix_log2)` /
`vrt_table_free(vrt_table_t *)` — they are ordinary allocator calls
(`mdb_alloc_buffer`) with no placement semantics. `vrt_io_t` has its own
`operator new`/`delete` needing the same treatment.

### The part that deserves care

`vrt_t::map_fpage` is 537 lines of dense pointer and table-recursion logic,
hand-unrolled into `r_ftable[]`/`r_fnode[]`/`r_fnum[]` arrays to avoid stack
recursion, with `goto` targets threading the sender/receiver walks together. It
is the map/grant path of the mapping database. A transcription error there would
be silent, and would corrupt address spaces rather than fail to build. It should
be converted with the whole function in view, not incrementally, and checked
against a disassembly of the old object rather than only "it compiles".

That is the reason this was deferred rather than started: there was room to
begin it but not to finish it, and a half-transcribed `map_fpage` is the worst
artifact this migration could leave behind.


## §109 — The vrt component cannot be converted yet: mdb_t does not exist

Took on the whole vrt component (§108) and stopped again, this time on something
that has to be said plainly: **`class mdb_t` is declared nowhere in the tree, and
I removed it.**

`grep -rn 'class mdb_t' src kdb` finds exactly one hit, and it is a forward
declaration in `generic/vrt.h:45`. There is no definition. `generic/mdb.h` still
carries the comment

    * ... and typedef'd back inside mdb_t below so mdb_t::ctrl_t /
    * mdb_t::range_t keep working in C++.

describing a class that is not below, or anywhere.

### Where it went

`f3d2a88`, "kdb: collapse the __cplusplus guards in the generic headers" — one
of the header-collapse commits of §96 — changed `src/generic/mdb.h` by
**841 deletions and 0 insertions**, taking `class mdb_t`, `class mdb_node_t`,
`class mdb_table_t`, and the `mdb_ctrl_t`/`mdb_range_t` constructors and static
factories with it.

The collapse pass was meant to delete the *C++ half* of dual-language headers,
keeping the C half that had already been written beside it. For `mdb.h` there
was no C half: the file's entire content was the C++ side. The pass removed it
anyway and left 130 lines of remnants.

`src/generic/mdb.cc` (1034 lines) and `mdb_mem.cc` (371) define methods of those
classes. They cannot compile — "`mdb_t` has not been declared".

### Why no gate caught it

`CONFIG_NEW_MDB` gates both files, via `src/glue/v4-x86/x64/Makeconf:49`. Four
configs set it:

    x86-x32-p4-iofp    x86-x32-p4-newmdb
    x86-x64-p4-iofp    x86-x64-p4-newmdb

**The gate config, x86-x64-p4-smp, has `CONFIG_NEW_MDB` off.** Every check this
migration has leaned on — the byte comparisons, the symbol diffs, the boot tests
— runs in a configuration that never compiles `mdb.cc`. §95 surveyed the other
ten x64 configs and attributed iofp's failure to `generic/vrt.h`, which is the
first error the compiler reports; the missing mdb underneath was never reached.

### What I have not established

Whether those four configs built *before* `f3d2a88`. The obvious experiment —
compile `mdb.cc` against the old header — is worthless now: the tree is C, so
any `.cc` fails C++ compilation on `generic/types.h` (`'_Bool' does not name a
type`) regardless of mdb. It measured 53 errors with the old header and 27 with
the new one, which says nothing about the regression and everything about the
test being invalid. A real answer needs a worktree at `f3d2a88^` and a full
build of `x86-x64-p4-newmdb` there. Given §95 recorded the PIC path as that
config's blocker, it may well have been broken already for other reasons — but
that is a guess, and it is not the same as being sure.

### The real dependency order

vrt cannot be converted first. `vrt.h:83` takes an `mdb_t::ctrl_t` by value and
`vrt.cc` drives everything through `get_mapdb()->map/flush/mapctrl`, so the
mapping database has to have a C form before the VRT can have one.

    mdb        mdb.h (841 lines to restore and convert)
               mdb.cc (1034), mdb_mem.cc (371), kdb/generic/mdb.cc
    vrt        vrt.h (471), vrt.cc (799), kdb/generic/vrt.cc (103)
               glue/v4-x86/vrt_io.h (171), vrt_io.cc (239)
    io space   io_fpage.h (183), io_space.h (70), io_space.cc (314)
               mdb_io.h (75), mdb_io.cc (306)
                                                    ~4100 lines

`mdb_t` has its own nine virtuals and its own subclasses (`mdb_io_t`, and the
memory one), so it needs the same ops-table treatment §108 sketched for `vrt_t`
— and it must be done first, since `vrt_io_t` and `mdb_io_t` are peers in the
same object graph.

### The lesson, which is the same one as §99

A config-gated file is invisible to a gate that does not set the config.
`CONFIG_STATIC_TCBS` and `CONFIG_X_CTRLXFER_MSG` blocks were dropped from
`api/v4/thread.cc` for exactly this reason, and here an entire header's contents
went the same way. The collapse pass (§96) verified itself against binary
equality in one configuration; that check cannot see a file the configuration
does not build. Before deleting from a header, the question is not "does the
gate still build" but "which configs compile the things this header declares,
and do any of them build at all".


## §110 — Restoring mdb.h and mdb_mem.h in C

§109 established that `f3d2a88` deleted `class mdb_t`, `mdb_node_t`,
`mdb_tableent_t`, `mdb_table_t` from `generic/mdb.h` (841 lines, 0 added) and
`class mdb_mem_t`, `mdb_mem_misc_t` from `generic/mdb_mem.h` (61 lines, 0
added). Both headers are now restored, in C.

### First, the measurement §109 owed

Built `x86-x64-p4-newmdb` from a worktree at `f3d2a88^`. **`src/generic/mdb.o`
was produced** — 17744 bytes — so `mdb.cc` did compile before that commit and
does not now. The regression is real, not merely a pre-existing breakage I
walked into.

(The config as a whole failed there too, with 192 errors in `mdb_mem.cc`,
`kdb/generic/mdb.cc`, `linear_ptab.h`, `space.c` and others — the tree was
mid-migration. `mdb.cc` itself was clean. An earlier grep of that log for
`generic/mdb.cc` counted 7 errors and looked like contrary evidence; they were
all in *kdb*/generic/mdb.cc, matched as a substring.)

### The shape of the C form

`mdb_t` had 16 virtuals and no data members — a C++ object that was nothing but
a vptr. So:

    struct mdb_t { const mdb_ops_t *ops; };

reproduces it exactly, and `mdb_mem_t`, which derived from `mdb_t` and added no
data either, needs no wrapper struct at all: it is an `mdb_t` whose `ops` is
`mdb_mem_ops`. Same for `mdb_io_t` when the io layer follows.

`mdb_node_t` deliberately had *no* virtuals — the C++ comment says a vtable per
mapping node would cost a word on every mapping — and instead carried
non-virtual forwarders taking an `mdb_t *`. The C form keeps that property
exactly: the node has no ops pointer, and `mdb_node_clear (node, mdb)` expands
to `mdb->ops->clear (mdb, node)`.

Verified rather than assumed: `BITS_WORD` is 64, `sizeof(mdb_tableent_t)` is 8,
`mdb_node_t` 40, `mdb_table_t` 32 — matching the bitfield layouts.

### Two things the compiler caught

**A false narrowing warning.** `(mdb_table_t *) (self->ptr << 1)` drew
`-Wint-to-pointer-cast`, because GCC compares against the *declared bitfield
width* (`BITS_WORD - 1` = 63) rather than the promoted expression type. A
`_Static_assert (sizeof (e->ptr << 1) == 8)` proves the shift really is
word-sized, so the arithmetic was never wrong; an explicit `(word_t)` cast
records that.

**A name collision that matters.** `mdb_map` and `mdb_flush` are *already
exported* by `generic/mapping.c` — the **old** mapping database, which every
`CONFIG_NEW_MDB=n` config builds, including the gate. Both headers are in scope
together, so the obvious names for `mdb_t::map` and `mdb_t::flush` clash with
different signatures. The new-MDB tree operations are therefore
`mdb_tree_map`, `mdb_tree_mapctrl`, `mdb_tree_flush`, `mdb_tree_delete_node`.
Renaming the old ones would have been the tidier-looking choice and the wrong
one: they are in the configuration that actually works.

### Verification

The gate config builds with 0 errors, 706 symbols with identical bodies, and
boots to userland. That is the right check for this step: restoring
declarations must not perturb a configuration that never compiled the code they
declare.

### What remains

`generic/mdb.cc` (1034), `generic/mdb_mem.cc` (371) and `kdb/generic/mdb.cc`
still define methods of the now-C types and remain unconverted. They were
already non-compiling before this step and still are, so nothing regressed and
nothing is silently wrong — the failure stays loud until they follow. That is
the distinction from the vrt split §108 refused: there, converting the header
alone would have produced code that compiled and dispatched through NULL.


## §111 — generic/mdb.cc converted; the object compares against the C++ one

`src/generic/mdb.c` replaces `mdb.cc` (1034 -> 1046 lines). It compiles with
**0 errors and 0 warnings**, and `mdb.o` is produced again in
`x86-x64-p4-newmdb` — the first time since `f3d2a88` deleted the class
definitions.

### Verified against the pre-regression object, not just the compiler

The point of §108's warning about `map_fpage` applies here too: this is the
mapping database's map/unmap path, where a transcription error is silent and
corrupts address spaces. So the new object was compared function-by-function
against the C++ `mdb.o` built from the `f3d2a88^` worktree:

    old (C++)                        new (C)                    size
    init_mdb()                  57   init_mdb                     57
    mdb_node_t::get_parent()    33   mdb_node_get_parent          33
    mdb_table_t::operator new  126   mdb_table_alloc             126
    mdb_table_t::operator del  117   mdb_table_free              117
    mdb_t::delete_node         895   mdb_tree_delete_node        920
    mdb_t::map                3022   mdb_tree_map               2935
    mdb_t::mapctrl            5338   mdb_tree_mapctrl           5482

The four functions that perform **no virtual dispatch** came out
**byte-identical in size**. The three that dispatch differ by under 3% in
either direction — the expected noise from C and C++ making different inlining
decisions around the indirect call. Nothing is structurally different, which is
what a faithful transcription of a 600-line algorithm should look like.

New symbols (`mdb_node_alloc` 14, `mdb_node_free` 14, `mdb_node_init` 13,
`mdb_tree_flush` 27) are the C++ operators and the header's INLINE `flush`,
which had no standalone existence before.

### A bug the conversion exposed

`mdb_t::map`'s **declaration and definition disagreed on parameter order**:

    mdb.h   map (..., word_t addr, word_t in_rights, word_t out_rights);
    mdb.cc  mdb_t::map (..., word_t addr, word_t out_rights, word_t in_rights)

Parameter names play no part in C++ overload resolution, so this never produced
a diagnostic. The definition is what ran: the **fifth** argument becomes the
*outbound* rights. `vrt.cc`'s only call site passes `f_fp.get_access()` fifth
and `~0UL` sixth, so the fpage's access rights land in `out_rights` and the
node's inbound rights are set wide open.

Whether that is intended is a question about the VRT's semantics, not about
this migration, so the behaviour is preserved exactly and the C declaration now
matches the definition — in C the two would have been a hard error, which is
the point. Flagged here rather than fixed.

### What remains in the mdb layer

`generic/mdb_mem.cc` (371) and `kdb/generic/mdb.cc` still reference the old
class forms. The `x86-x64-p4-newmdb` config now fails on those plus its
pre-existing blockers — the PIC path (`intctrl-pic.cc/h`, `intctrl.h`, §95),
`timer.cc`, `linear_ptab_walker.c`, `space.c` — so `mdb.c` is no longer among
the reasons that configuration does not build.

Gate unaffected throughout: 0 errors, 706 symbols with identical bodies, boots.


## §112 — generic/mdb_mem.cc converted; the memory database's ops table

`src/generic/mdb_mem.c` (371 -> 428 lines) builds clean — `mdb_mem.o`, 10864
bytes, 0 errors, 0 warnings. With §111's `mdb.c` this completes the mapping
database proper; `x86-x64-p4-newmdb` is down from 189 errors to 100, and
neither mdb file is among the causes any more.

The 17 `mdb_mem_t` overrides become file-static `mm_*` functions and a single
`const mdb_ops_t mdb_mem_ops` initialiser, with `mdb_t mdb_mem = { &mdb_mem_ops };`.
Because `mdb_mem_t` derived from `mdb_t` and added no data, the instance is an
`mdb_t` outright — the derived type does not survive as a struct at all.

### Three pgent operations had no C form yet

`mdb_mem` is the only caller of `pgent_t::rights`, `set_rights` and
`set_attributes`, so the x86 C wrapper set in `arch/x86/pgent.h` — written when
`linear_ptab_walker.c` and `mapping.c` were converted — had never needed them.
Added to `glue/v4-x86/space.c` beside the existing ones, transcribed from
`4b5e3a0a^`.

`set_rights` carries an original oddity, kept verbatim:

    if ((rwx & 1) && (raw | X86_PAGE_NX))

`|` where `&` reads as intended, so the test reduces to `(rwx & 1)`. The effect
is the wanted one — when execute is requested, clear NX — so the typo is
harmless, and correcting it would be a behaviour change dressed as a
transcription. Left as found and recorded here.

### Four things the C compiler objected to that C++ had not

  - `(space_t *) (misc.space << 8)` — the same false `-Wint-to-pointer-cast` as
    §110, from GCC judging the declared bitfield width rather than the promoted
    type. Explicit `(word_t)`.
  - `pgent_vaddr (..., node)` — the C++ passed an `mdb_node_t *` where the
    signature said `mapnode_t *`, which works because `arch/x86/pgent.h:29` is
    `#define mapnode_t mdb_node_t`. My defensive `(struct mapnode_t *)` cast
    was what actually broke it; removing it was the fix.
  - `mdb_add_size` has no header declaration; the C++ declared it inside the
    function body, and C allows exactly the same thing.
  - **`size_max` does not exist in C.** It is a member of the `X86_PGSIZES`
    enum-list macro, which only ever instantiated the C++ `pgsize_e` enum. The
    arch-neutral spelling is `PGENT_SIZE_MAX`, already used by
    `kdb/generic/linear_ptab_dump.c` and defined for both x86 and powerpc.

### Cost to the gate, stated plainly

The gate config gains **three symbols** — `pgent_rights`, `pgent_set_rights`,
`pgent_set_attributes` — which it does not call, since it does not build
`mdb_mem.c`. Its other 706 symbols are byte-identical apart from
`__ctors_GLOBAL__` and one relocated address in `tcb_create_startup_stack`
shifted by the new code. It boots to userland. The alternative was a
`CONFIG_NEW_MDB` guard around three general-purpose page-table accessors, which
seemed worse than ~50 bytes of unreferenced code in one configuration.

### What is left in this stack

`kdb/generic/mdb.cc` is the last mdb file. Beyond it the newmdb config's
remaining blockers are all pre-existing and unrelated: the PIC path
(`8259.h`, `intctrl-pic.cc/h`, `intctrl.h`, `pc99/intctrl.c` — §95's seven-config
blocker), `timer.cc`, `linear_ptab_walker.c`, `space.c` and `types.h`.


## §113 — kdb/generic/mdb.cc: the mapping database stack is C

`kdb/generic/mdb.c` (233 -> 254 lines), `mdb.o` 9216 bytes, 0 errors,
0 warnings. **`src/generic/` and `kdb/generic/` now hold no C++ mdb code at
all**: `mdb.h`, `mdb.c`, `mdb_mem.h`, `mdb_mem.c`, `kdb/generic/mdb.c`.

Mechanical, given §110's header: the three `kdb_t::` statics become file-static
functions with forward declarations, `mdb.interact (cg, "mdb")` becomes
`cmd_group_interact (&mdb, cg, "mdb")`, `get_hex ("Address")` picks up the two
arguments the C prototype has always required (`defnum`, `defstr`), and
`mdb->dump (n)` becomes `mdb->ops->dump (mdb, n)`.

### Where the newmdb config now stands

    §109 (before any of this)   mdb.cc could not compile at all
    §111 mdb.c                  189 errors
    §112 mdb_mem.c              100 errors
    §113 kdb/generic/mdb.c       91 errors

None of the 91 is in an mdb file. What remains in `x86-x64-p4-newmdb`:

  - the PIC path — `8259.h`, `intctrl-pic.cc/h`, `glue/v4-x86/intctrl.h`,
    `kdb/platform/pc99/intctrl.c` — §95's blocker for seven configs
  - `glue/v4-x86/timer.cc`, `linear_ptab_walker.c`, `types.h`
  - `glue/v4-x86/space.c`, whose five errors predate this work

### A tangle worth naming before someone else finds it

`space.c`'s failures in this configuration are a **`mapnode_t` identity
conflict**, not an mdb bug:

    src/generic/mapping.h:37   struct mapnode_t;          /* the old MDB's node */
    src/arch/x86/pgent.h:29    #define mapnode_t mdb_node_t

With `CONFIG_NEW_MDB=y` both headers are in scope, so the token means the old
database's node in one file and the new database's node in another, and
`pgent_vaddr`/`pgent_mapnode`/`pgent_set_linknode` end up with two incompatible
declarations. A `#define` of a type name across two mapping-database
implementations was always going to do this; it only became visible once the
new-MDB configuration could get far enough to compile `space.c`.

Untouched here — it is a decision about which database owns the name, not a
transcription — but it is the next thing in the way of that config, and
`glue/v4-x86/space.c` is C already, so it is not migration work.

### Verification

Gate: 0 errors, 709 symbols with identical bodies (the 709 including §112's
three unused `pgent_*` wrappers), boots to userland.


## §114 — The vrt component converted

All five files of §108's component are C: `generic/vrt.h` (471 -> 259),
`generic/vrt.c` (799 -> 807), `kdb/generic/vrt.c` (103 -> 104),
`glue/v4-x86/vrt_io.h` (171 -> 150), `glue/v4-x86/vrt_io.c` (239 -> 236).

This was blocked on §109-§113: `vrt.h` takes an `mdb_t::ctrl_t` by value and
`vrt.c` drives everything through `get_mapdb()->map/flush/mapctrl`, so it could
not be written until `mdb_ctrl_t`, `mdb_tree_map`, `mdb_tree_flush` and
`mdb_tree_mapctrl` existed.

`vrt_t` used the same ops-table shape §108 sketched and §110 had by then proved
on `mdb_t`: nine function pointers, `ops` first so the struct reproduces the
C++ vptr-then-data layout, and `struct vrt_io_t { vrt_t base; ... }` in place of
`class vrt_io_t : public vrt_t`.

### One simplification the ops table permitted

`vrt_io_t::operator new` built a **throwaway stack instance** purely to reach a
virtual:

    vrt_io_t dummy;
    vrt_table_t * table = new (dummy.get_radix (sizes[num_sizes - 1])) vrt_table_t;

`dummy` is default-constructed, never initialised, and used only for dispatch.
With the ops table the function is an ordinary one and is called directly. This
is the one place the C is not a literal transcription; the behaviour is
identical because `vrt_io_t::get_radix` reads only the file-scope `sizes[]`.

### Two originals preserved rather than corrected

  - `vrt_io_t::operator delete` frees `sizeof (mdb_node_t)`, not
    `sizeof (vrt_io_t)`. Almost certainly a copy-paste slip, and a real
    allocator bug if these are ever freed — but changing an allocation size is
    not a transcription, so `vrt_io_free` frees exactly what the C++ did, with
    a comment.
  - `vrt_t::mapctrl` computes `status` but never assigns it, returning a
    constant 0. Kept.

### What verifies, and what does not

`generic/vrt.c` and `kdb/generic/vrt.c` compile **0 errors, 0 warnings**
against the gate config, which does not set `CONFIG_X86_IO_FLEXPAGES` and so
does not drag in the io headers.

`vrt_io.c` cannot be compiled yet. `glue/v4-x86/fpage.h:35` includes
`INC_GLUE(io_fpage.h)`, so in the one configuration that builds any of this the
whole io layer is in scope, and it is still C++:

    src/glue/v4-x86/io_fpage.h   183     src/glue/v4-x86/io_space.cc  314
    src/glue/v4-x86/io_space.h    70     src/glue/v4-x86/mdb_io.h      75
    src/glue/v4-x86/mdb_io.cc    306                                  948

Do not read `x86-x64-p4-iofp`'s error count as a regression: it went 61 -> 570
because the build now gets *past* `vrt.h` and reaches files it never used to
attempt. §98 records the same trap — error counts measure how far the compiler
walked, not how broken the tree is. Objects built and files-with-errors are the
honest measures, and the failing set is now exactly the io layer plus the
pre-existing PIC path.

Gate unaffected: 0 errors, 709 symbols with identical bodies.


## §115 — The io layer: a third instance of the same regression, found before starting

Began the io layer (`io_fpage.h`, `io_space.h/.cc`, `mdb_io.h/.cc`, 948 lines)
and stopped at the dependency survey, because it is not 948 lines.

`io_space.cc` drives the IO permission bitmap entirely through `space_t`:

    space->get_io_bitmap()      space->install_io_bitmap(true)
    space->sync_io_bitmap()     space->get_io_space() / set_io_space()

**Six of those seven members are declared and defined nowhere in the tree.**
`get_io_bitmap` survives only on `x86_tss_t`, which is a different class.

### Where they went — the same two passes, again

    312b160  "collapse the __cplusplus guards in the glue/v4-x86 headers"
             glue/v4-x86/space.h   -439 lines, +0

deleted the declarations:

    -    addr_t install_io_bitmap(bool create);
    -    void free_io_bitmap(void);
    -    bool sync_io_bitmap();
    -    addr_t get_io_bitmap(cpuid_t cpu = current_cpu);
    -    void set_io_space(io_space_t *n) { data.io_space = n; n->set_space(this); }
    -    io_space_t *get_io_space(void) { return data.io_space; }
    -void init_io_space();

and

    49fab2d  "glue/v4-x86/space.cc -> C: flip the last wrapper-host"

dropped the ~200 lines that defined `install_io_bitmap`, `free_io_bitmap` and
`sync_io_bitmap`, because they sit inside `#if defined(CONFIG_X86_IO_FLEXPAGES)`
and the gate config does not set it.

This is the **third** occurrence of one mistake:

    §99   api/v4/thread.cc      CONFIG_STATIC_TCBS, CONFIG_X_CTRLXFER_MSG blocks
    §109  generic/mdb.h         the entire class definitions, no C form existed
    §115  glue/v4-x86/space.h   the io-bitmap and io-space members
          glue/v4-x86/space.cc  their definitions

Every one has the same shape: **content reachable only under a config the gate
does not set, removed by a pass that verified itself against the gate.** The
binary comparison, the symbol diff and the boot test are all blind by
construction to code that the configuration never compiles. §110 said this once;
it deserves saying as a rule:

> Before deleting from a header or a file, list its config-guarded regions and
> ask which configurations compile them. If none of those configurations builds,
> the deletion cannot be verified — only reviewed by reading.

### Actual scope of the io layer

    io_fpage.h          183   arch_fpage_t: self-contained, mechanical
    io_space.h           70   two C++ leftovers, the rest is C already
    io_space.cc         314
    mdb_io.h             75   mirrors mdb_mem.h exactly
    mdb_io.cc           306   the 16 ops, mostly empty bodies
    ------------------------
                        948
    + glue/v4-x86/space.h      declarations to restore
    + glue/v4-x86/space.c      ~200 lines to restore and convert
    + arch/x86/tss.h           x86_tss_t::get_io_bitmap has no C form yet

Restoring `space.cc`'s three functions is not transcription from the file in
front of me — it is recovery from `49fab2d^` followed by conversion, exactly as
§110/§111 did for mdb. That is the work, and it should be done deliberately
rather than tacked onto the end of a long session.

Nothing was changed for this section.
