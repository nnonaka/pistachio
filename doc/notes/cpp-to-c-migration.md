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

**Correction (§116):** that second half was *not* silent. `49fab2d` left an
explicit `#error "CONFIG_X86_IO_FLEXPAGES: space.c io_bitmap methods need
translation"` in place of the bodies, so any IO-flexpage build stopped there
loudly. The deletion in `space.h` (312b160) was silent; the one in `space.cc`
was a marked deferral. The rule below still holds, but this instance was half
as bad as stated.

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


## §116 — The io layer converted; the vrt/mdb stack is C

    glue/v4-x86/io_fpage.h    183 -> 140    arch_fpage_t
    glue/v4-x86/io_space.h     70 ->  80
    glue/v4-x86/io_space.c    314 -> 254
    glue/v4-x86/mdb_io.h       75 ->  54
    glue/v4-x86/mdb_io.c      306 -> 282
    glue/v4-x86/space.c       +212            recovered from 49fab2d^
    glue/v4-x86/space.h        +9             recovered from 312b160^
    arch/x86/x64/tss.h         +5             x86_tss_get_io_bitmap

`x86-x64-p4-iofp` goes from **15 objects to 62**, and every file of the mdb,
vrt and io stack now compiles: `mdb.o`, `mdb_mem.o`, `kdb/generic/mdb.o`,
`vrt.o`, `vrt_io.o`, `kdb/generic/vrt.o`, `mdb_io.o`, `io_space.o` — with **no
implicit declarations**.

### The recovery half

`space_install_io_bitmap`, `space_free_io_bitmap` and `space_sync_io_bitmap`
came back from `49fab2d^` and were converted; `space_get_io_bitmap`,
`space_get_io_space`, `space_set_io_space` were inline members recovered from
`312b160^`. `space_arch_free`'s IO block — unmap the io space, free the bitmap
— was restored too; the C stub had `(void) self;` and a comment saying there was
nothing to do.

`space_get_io_bitmap`'s `cpuid_t cpu = current_cpu` default is dropped and
callers pass `current_cpu`, matching what `space_add_tcb`/`space_remove_tcb`
already do in this header.

### Two traps in the conversion half

**`min` is not the C++ `min`.** `generic/lib.h` defines `INLINE int min (int, int)`.
The C++ used a template, and `zero_io_bitmap`/`set_io_bitmap` call it on
`word_t` operands — so the obvious transcription silently narrows 64-bit values
to `int`. A file-local `min_word` keeps the original width. Worth checking
wherever else the C `min` was substituted for the template.

**Two definitions of one function under opposite guards.**
`acceptor_t::get_arch_specific_rcvwindow` was an INLINE specialisation in
`io_space.h` returning the complete IO window, while `api/v4/accessors.c`
defines the generic nil-window version unconditionally. In C++ the header
inline shadowed nothing — the two never met, because `accessors.c` did not exist
yet when that pattern was written. In C they collide at link time, so
`accessors.c`'s copy is now under `#if !defined(CONFIG_X86_IO_FLEXPAGES)` and
`io_space.c` supplies the other.

### Preserved as found

`arch_fpage_t::get_rwx()` returned `true` — i.e. 1, not an rwx mask — while its
siblings `is_read`/`is_write`/`is_execute` all return true unconditionally. An
IO fpage has no permission bits, so this is consistent with the type's intent
even though the value looks wrong; transcribed verbatim.

### What is left in this configuration

    intctrl-pic.cc/h, 8259.h, glue/v4-x86/intctrl.h, kdb/platform/pc99/intctrl.c
                                       the PIC path -- §95's blocker for 7 configs
    glue/v4-x86/timer.cc
    glue/v4-x86/space.c                active_cpu_space, and the mapnode_t
                                       identity conflict of §113
    generic/linear_ptab_walker.c, api/v4/generic-archmap.h, generic/types.h

None of it is mdb, vrt or io. Gate unaffected: 0 errors, 709 symbols with
identical bodies, boots to userland.


## §117 — The PIC path converted; the seven configs now reach the linker

    platform/pc99/8259.h              176 -> 180
    platform/generic/intctrl-pic.h     97 -> 115
    platform/generic/intctrl-pic.c    116 -> 144
    glue/v4-x86/intctrl.h              -7   (the stray inline method)
    glue/v4-x86/timer.c               178 -> 174
    platform/pc99/rtc.h                +18  (rtc_read/rtc_write)

`x86-x64-p4`, `-p3` and `-p4-nokdb` now **compile with zero errors** and fail
only at the link. Before this they died in `8259.h`.

### A template with two instantiations

`i8259_pic_t` was `template<u16_t base> class`, and the header says why: "The
template parameter BASE enables compile-time resolution of the PIC's control
register addresses." In C the base is a field, so `out_u8` takes it in `%dx`
instead of as an immediate. There are exactly two instances — master 0x20,
slave 0xa0 — touched only on init/mask/ack, so the lost immediate is not worth
the alternative (macro-generated duplicate function sets).

`I8259_CACHE_PICSTATE` made this awkward: with caching the mask lives in a
field, without it in a local read back from the port, and every method had an
`#if` picking one. Two macros — `I8259_LOAD_MASK` and `I8259_MASK` — let the
bodies stay identical, which is what the C++ was doing with its per-method
`#if`.

`rtc_t<0x70>` had already lost its template in an earlier pass, leaving
`timer.cc` referring to a type that no longer existed. `rtc_read`/`rtc_write`
now sit beside `wait_for_second_tick`, which had already been rewritten in
direct port I/O for exactly this reason.

### Three faults the conversion surfaced, none of them in the PIC

**`active_cpu_space` in a uniprocessor build.** `312b160` deleted
`class active_cpu_space_t` and its `extern` from `glue/v4-x86/space.h`; the C
flip re-created the type and the global inside `space.c`'s `#if defined(CONFIG_SMP)`
block, but left `active_cpu_space_set`/`_get` *outside* it. The gate is SMP, so
it never noticed. In C++ the accessors were members of a class that only existed
in the SMP branch, so the guard came for free. Both are now guarded, matching
their only caller in `thread.c`.

**`arch_map_fpage` defined twice.** `api/v4/generic-archmap.h` defines no-op
`INLINE` versions and is included unconditionally by `api/v4/thread.c` and
`space.c`; `glue/v4-x86/io_space.h` declares the real ones `extern` under
`CONFIG_X86_IO_FLEXPAGES`. As C++ inlines the two coexisted; as C statics they
collide. The no-ops are now under the opposite guard.

**`DEBUG_SCREEN` without `CONFIG_DEBUG`.** `spin_forever_c` used it under
`CONFIG_SPIN_WHEELS` alone, but `glue/v4-x86/debug.h` defines it only under
`CONFIG_DEBUG`. `x86-x64-p4-nokdb` sets the first and not the second. Now
requires both, falling back to the plain busy loop — which is all a kernel
without a debug screen can do.

### What the three configs fail on now

    undefined: migrate_interrupt_start  pgent_smp_sync  space_end_update
               space_flush_tlb  space_flush_tlbent  tcb_migrate_to_processor

Every one is a **uniprocessor stub**. The SMP variants exist; the non-SMP forms
were inline members of the classes `312b160` deleted from `glue/v4-x86/space.h`,
and nothing has replaced them, because the gate is SMP. This is the same
gate-blindness as §115 and §117's `active_cpu_space`, now in its fourth
appearance — and it is the last thing between these configs and a link.

`x86-x64-k8` additionally needs `x86_amdhwcr_t` in `glue/v4-x86/init.c`.

Gate: 0 errors, 709 symbols with identical bodies, boots to userland.


## §118 — The uniprocessor stubs: six x64 configs now build

**1 of 11 x64 configs built at §95. Six do now.**

    x86-x64-p4-smp   332328   (the gate)
    x86-x64-p4       247936   uniprocessor, PIC
    x86-x64-p3       149088
    x86-x64-p4-nokdb 149472
    x86-x64-p4-fp    248312
    x86-x64-p4-statictcbs  2349704

`x86-x64-p4` boots to userland — a uniprocessor PIC kernel reaching the test
suite, which is what makes §117's PIC conversion and these stubs real rather
than merely link-clean.

### What was missing, and which half was a regression

    space_flush_tlb / space_flush_tlbent / space_end_update   restored
    pgent_sync                                                restored
    tcb_migrate_to_processor                                  never existed
    migrate_interrupt_start_c                                 guard added

The first four were **inline members in the `#ifndef CONFIG_SMP` branch** of
`glue/v4-x86/space.h` and the non-SMP branch of `arch/x86/pgent.h`, deleted by
`312b160`. The C flip reproduced only the SMP branch of each, because the gate
config is SMP. Recovered from `312b160^`.

`tcb_migrate_to_processor` is different and worth separating: in `thread.cc` it
sat inside `#if defined(CONFIG_SMP)` while `commit_schedule_parameters` called
it unconditionally, so **a uniprocessor build never linked in C++ either**. That
is an upstream gap, not something this migration broke. The stub returns false —
a `processor_control` request cannot be honoured without SMP — matching what the
powerpc port already needed; powerpc's local copy is removed in favour of the
shared one.

`migrate_interrupt_start_c` in `glue/v4-x86/thread.c` was an unconditional
wrapper around an SMP-only function whose only real caller, `xcpu_release_thread`,
was properly guarded. Only the wrapper needed the guard.

### A mistake made and caught here

The first attempt spliced the non-SMP block in at "the first `#endif` after
`space_end_update`". That `#endif` closes an inner conditional 55 lines before
the `CONFIG_SMP` block actually ends, so the edit **silently deleted
`space_move_tcb`, `space_alloc_cpu_top_pdir` and `space_free_cpu_top_pdir`**.
The gate build caught it immediately — three undefined references — and
`git checkout` plus a splice anchored on `#endif /* defined(CONFIG_SMP) */`
fixed it.

Worth recording because it is the same class of error as the bugs being fixed:
a structural edit aimed at a pattern rather than at the construct it belongs to.
The lesson is the cheap one — when splicing into `#if` blocks, anchor on the
*commented* closing marker, and re-run the gate before believing the result.

### Verification

Gate: 0 errors, 709 symbols with identical bodies, boots. powerpc: 66 objects,
0 errors, links (its duplicate `tcb_migrate_to_processor` removed). The five
newly-building configs produce images; `x86-x64-p4` boots to the test suite.

### Still failing

    x86-x64-p4-fullkdb   7 errors
    x86-x64-k8           2  -- x86_amdhwcr_t in glue/v4-x86/init.c
    x86-x64-p4-cm       99  -- compatibility mode, glue/v4-x86/utcb.h (§95)
    x86-x64-p4-iofp          the mapnode_t identity conflict (§113)
    x86-x64-p4-newmdb        same


## §119 — k8: two flush-filter setters, and the pattern's fifth appearance

`x86-x64-k8` failed on one line:

    glue/v4-x86/init.c:359: x86_amdhwcr_t::disable_flushfilter();

`arch/x86/amdhwcr.h` had been converted to C — fourteen `amdhwcr_is_*`
predicates and a `dump_hwcr` — but **not** `enable_flushfilter` /
`disable_flushfilter`. They were static members of `class x86_amdhwcr_t`,
removed by `4b5e3a0` with the rest of the C++ half, and the C half never had
them because their only caller sits under `CONFIG_CPU_X86_K8`, which the gate
does not set.

That is the same shape as §99, §109, §115, §117 and §118 — the fifth
appearance, and by now the diagnosis is mechanical: a header converted while
building a config that compiles only part of it, verified by a check that
cannot see the rest.

Both are one-liners over `x86_rdmsr`/`x86_wrmsr` on `X86_AMDHWCR_FFDIS`, and
the naming is worth care in passing: `enable_flushfilter` *clears* FFDIS and
`disable_flushfilter` *sets* it, matching the fourteen predicates already in the
file, seven of which negate a `*DIS*` bit.

**`x86-x64-k8` builds (243448 bytes) and boots to userland.** Seven of eleven
x64 configs now build:

    x86-x64-p4-smp  x86-x64-p4  x86-x64-p3  x86-x64-p4-nokdb
    x86-x64-p4-fp   x86-x64-p4-statictcbs   x86-x64-k8

Gate: 0 errors, 709 symbols with identical bodies.

### The four that remain

    x86-x64-p4-fullkdb   7 errors
    x86-x64-p4-cm       99  -- compatibility mode, glue/v4-x86/utcb.h (§95)
    x86-x64-p4-iofp          mapnode_t identity conflict (§113)
    x86-x64-p4-newmdb        same


## §120 — mapnode_t, and the NEW_MDB configs: nine build, seven boot

`x86-x64-p4-newmdb` (254176) and `x86-x64-p4-iofp` (311368) now **compile and
link**. Nine of eleven x64 configs build. **Seven of them boot** — these two do
not, and that is stated plainly below rather than counted as success.

### What "the mapnode_t conflict" actually was

Three separate things, only the last of which was a name conflict.

**1. `linear_ptab_walker.c` had lost its new-MDB path.** The C++ file carried
**15** `CONFIG_NEW_MDB` conditionals; the C had **2**. Seven
`#if defined(CONFIG_NEW_MDB)` blocks — 28 lines — were dropped in conversion,
including the sigma0 map, the ordinary map, the overmap parent test, the
grant-time flush and the whole mapctrl call. Recovered from the C++ and
converted (`mdb_mem.map` -> `mdb_tree_map (&mdb_mem, ...)`, `->set_misc` ->
`mdb_node_set_misc`, `mdb_t::range_t (...)` -> `mdb_range_make (...)`).

**2. The alias was scoped so that no consumer could use it.**
`arch/x86/pgent.h` does `#define mapnode_t mdb_node_t` for its own
declarations and `#undef`s it at the end of the header. So `pgent_mapnode` was
*declared* returning `struct mdb_node_t *` and *defined* in
`glue/v4-x86/space.c` returning `struct mapnode_t *` — the same source spelling
resolving two ways.

Removing the `#undef` is the obvious fix and is wrong: `generic/mapping.h`
defines a real `struct mapnode_t` for the **old** database, and the leaked
alias turns that into a second definition of `mdb_node_t`. The header is right
to scope it. What was missing is that the two files which *define* those pgent
functions, or hold the type in locals, must re-establish the alias for
themselves — which is exactly what the C++ `linear_ptab_walker.cc` did with its
own file-local `#define`. Both now do.

**3. Two kdb dump functions lost external linkage.** `kdb_t::dump_table` and
`kdb_t::dump_resource_map` were *class* statics, which have external linkage;
converting them to file-`static` broke `kdb/platform/pc99/io.c`, which calls
both for the IO space. Only `dump_resource_table` is genuinely file-local.

Plus a sixth instance of the config-gated pattern: nine `readmem (space, ...)`
calls in `glue/v4-x86/exception.c`, left from the `readmem<T>` template, inside
`#if defined(CONFIG_X86_IO_FLEXPAGES)`. They are `readmem_u8` — the buffer is
`u8_t i[4]`.

### The two that build but do not boot

Both hang immediately after `Launching kernel ...` with no output. The
experiment is clean: `x86-x64-p4-newmdb`'s config differs from the **booting**
`x86-x64-p4` in `CONFIG_NEW_MDB` **and nothing else**. So the fault is in the
new mapping database path, not in the PIC, timer or uniprocessor work.

Checked and eliminated: `init_mdb()` is called from `glue/v4-x86/init.c:493`,
and the linker set is populated — `_start_mdb_funcs`..`_end_mdb_funcs` spans
four entries, with `mdb_buflist_init`, `init_mdb_mem_sizes` and `init_mdb_mem`
all present in the image. The MDB init functions do run.

Whether the hang is something in the §111/§112/§120 conversions or a
pre-existing fault in a feature that has not been runnable since `f3d2a88` (and
is `default NEW_MDB from n`, marked experimental) is **not established**. There
is no C++ baseline to compare against: the configuration did not build then
either. It needs a debugger or a bisect against a much older tree, and it should
not be counted as working until then.

### Where the x64 configs stand

    build and boot   x86-x64-p4-smp  x86-x64-p4  x86-x64-p3  x86-x64-k8
                     x86-x64-p4-nokdb  x86-x64-p4-fp  x86-x64-p4-statictcbs
    build, hang      x86-x64-p4-newmdb  x86-x64-p4-iofp
    do not build     x86-x64-p4-fullkdb (7 errors)
                     x86-x64-p4-cm (compatibility mode, glue/v4-x86/utcb.h)

Gate: 0 errors, 709 symbols with identical bodies. powerpc: 66 objects, links.


## §121 — Bisecting the NEW_MDB hang: it maps twice, then stops

Not a fix — a localisation, recorded so the next attempt starts from the
answer rather than from "it hangs".

### What it is not

  - **Not an infinite loop.** The monitor shows `RIP=ffffffffc060b07d`,
    `HLT=1`, inside `sched_idle`. The CPU is halted waiting for an interrupt.
  - **Not a failure to initialise the database.** All four `MDB_INIT_FUNCTION`
    entries are linked into the set (`_start_mdb_funcs`.. `_end_mdb_funcs`
    spans four 16-byte entries: `mdb_buflist_init` at priorities 0 and 2,
    `init_mdb_mem_sizes` at 1, `init_mdb_mem` at 3), and `init_mdb()` is called
    from `glue/v4-x86/init.c:493`.
  - **Not an early crash.** With `CONFIG_VERBOSE_INIT` switched on, the kernel
    prints its whole init sequence through "Creating sigma0", "Creating root
    server" and "Idle thread started on CPU 0".

### What it is

The scheduler's ready queue is **empty**. Read straight out of memory at
`scheduler` (0xffffffffc0a00000):

    wakeup_list    = 0
    max_prio       = 0xffff        /* s16_t -1: nothing runnable */
    timeslice_tcb  = 0xffffffffc0a01000   /* the idle TCB */
    prio_queue[0..2] = 0

So both root servers exist and neither is runnable — they are blocked, and the
kernel correctly idles.

Instrumenting `mdb_tree_map` shows why that is interesting:

    XX mdb_tree_map objsize=12 addr=0000000001000000
    XX mdb_tree_map objsize=12 addr=0000000001012000

**The new mapping database works — twice.** Two 4 KB mappings are created for
roottask (which loads at 0x1000000), and then nothing further. Roottask needs
far more pages than two, so the third fault never resolves: roottask stays
blocked on its pager, sigma0 stays blocked, and the system idles.

### Where that points

Two successful maps followed by silence is the signature of the **mapping-tree
growth path**, not of init and not of the `linear_ptab_walker.c` blocks
recovered in §120 — those are demonstrably exercised, since the two maps happen
through them.

In `mdb_tree_map`, the first mapping under sigma0's node inserts directly; a
second, smaller object forces the creation of a sub-table with path
compression; a third has to find and reuse that table. That third step —
`mdb_table_alloc` / `mdb_table_set_table` / the `match_prefix` walk — is the
first code that has never executed, and it is the 400-line block §111 flagged
as the part where a transcription error would be silent.

### For whoever picks this up

The cheap next probe is a `printf` in each arm of `mdb_tree_map`'s `for (;;)`
— the `objsize == table objsize` insert, the recurse-into-subtable branch, the
new-table branch and the intermediate-table branch — booted against
`x86-x64-p4-newmdb` with `CONFIG_VERBOSE_INIT` on. That says in one run which
arm is taken on the third fault and whether it returns.

Worth keeping in mind: **there is still no evidence this ever worked.** The
configuration has not built since `f3d2a88`, the option is `default NEW_MDB
from n` and marked experimental, and §111's parameter-order finding
(`mdb_t::map`'s declaration and definition disagreed, silently, in C++) is the
kind of thing that suggests the feature was never finished rather than that the
migration broke it.

Instrumentation reverted; tree clean.


## §122 — NEW_MDB: abandoned

Work on the `CONFIG_NEW_MDB` hang stops here. This section is the record so a
later sweep does not mistake the state for a regression, or re-derive §121.

### Status of the two configs

`x86-x64-p4-newmdb` and `x86-x64-p4-iofp` **build and link** and are **known not
to boot**. They are not regressions: neither had built since `f3d2a88`, and
§121 found no evidence the new mapping database ever ran in this tree. Treat
them as *expected-fail* in any build sweep, distinct from `x86-x64-p4-fullkdb`
and `x86-x64-p4-cm`, which still fail to compile.

Final tally for the x64 configs:

    build and boot (7)   p4-smp  p4  p3  k8  p4-nokdb  p4-fp  p4-statictcbs
    build, known hang (2) p4-newmdb  p4-iofp
    do not build (2)     p4-fullkdb  p4-cm

### What is kept, and why

The code stays. `generic/mdb.h`, `mdb.c`, `mdb_mem.h`, `mdb_mem.c`,
`kdb/generic/mdb.c`, the vrt component and the io layer are all C now, compile
clean, and cost the seven working configurations nothing — `CONFIG_NEW_MDB` is
off in every one of them, so not a byte of it is linked. Deleting it would
throw away a working conversion of ~4000 lines to remove a feature that is
merely unfinished, and would also take the vrt and io-space layers with it,
since they exist only under these options.

It also, incidentally, closed a real regression: `f3d2a88` had deleted
`generic/mdb.h`'s entire contents, which left `mdb.cc` unable to compile at all.
That is repaired regardless of whether the runtime path is ever fixed.

### If it is picked up again

§121 has the localisation: init completes, the ready queue is empty,
`mdb_tree_map` succeeds exactly twice (two 4 KB mappings for roottask at
0x1000000 and 0x1012000) and then the third fault never resolves. The suspect
is the mapping-tree growth path — sub-table creation and reuse — inside
`mdb_tree_map`'s `for (;;)`. The next probe is one `printf` per arm of that
loop.

And the standing caution from §111: `mdb_t::map`'s C++ declaration and
definition disagreed on parameter order, silently, so the fifth argument is the
*outbound* rights and `vrt.c`'s only call site leaves inbound rights wide open.
Whether that is intended is a question about the feature's design, and it is
unanswered.


## §123 — fullkdb: builds. It does not boot, and that is not the conversion.

`x86-x64-p4-fullkdb` **compiles and links** (1460200 bytes). Ten of eleven x64
configs now build. It **triple-faults at boot**, before a single character of
output, and the investigation below did not find the cause.

### The four C++ leftovers

All four sat behind `CONFIG_KMEM_TRACE` or the full-kdb options, which the gate
does not set — the seventh appearance of the pattern, and the first one to
block `tcb_layout.h` generation since §103, so it took the whole build down.

  - `generic/kmemory.h` — `class kmem_group_t` with both members public, under
    `#if defined(CONFIG_KMEM_TRACE)`. A plain struct.
  - `kdb/arch/x86/x64/disas.cc` (65 lines) — `extern "C"` on `disas`, and
    `f->rip`, which is `f->__base.regs[X86_EXC_IPREG]` in the C frame. Also
    picked up the two arguments `get_hex` has always required.
  - `kdb/generic/sprintf.cc` (272 lines) — two `extern "C"` markers and nothing
    else; the body was already C.
  - `kdb/generic/kmemory.c` — `__kmem_groups.reset()` / `.next()`, C++ methods
    on the linker set. `linker_set_reset` / `linker_set_next` already existed.

### The boot failure

qemu **exits** rather than hangs, which with `-no-reboot` means a triple fault,
not the idle-and-wait of §121. It happens before the `CONFIG_VERBOSE_INIT`
banner, so within the first moments of kernel entry.

Ruled out by bisecting the config:

  - **`CONFIG_KMEM_TRACE` off — still faults.** So the `kmem_group_t`
    conversion above is not the cause.
  - **`CONFIG_IPC_FASTPATH` off — still faults.**

Not ruled out: `CONFIG_TRACEBUFFER`, `CONFIG_TRACEPOINTS`, `CONFIG_KDB_BREAKIN`,
`CONFIG_TBUF_PERFMON`, `CONFIG_DEBUG_SYMBOLS` — the remaining options that
distinguish this config from the booting `x86-x64-p4`.

Kickstart's segment report is healthy and matches `x86-x64-p4`'s shape (a larger
`.text`, 0x600000-0x636500 against 0x600000-0x617500, everything else at the
same addresses), so this is not a load or layout failure.

As with §122, there is no evidence this configuration ever ran: it has not
compiled in this tree, so the fault may be as old as the options themselves.
The next probe is to keep bisecting the five remaining options, cheapest first
(`DEBUG_SYMBOLS`, then `TBUF_PERFMON`, `KDB_BREAKIN`, `TRACEPOINTS`,
`TRACEBUFFER`).

### x64 status

    build and boot (7)    p4-smp  p4  p3  k8  p4-nokdb  p4-fp  p4-statictcbs
    build, do not boot (3) p4-fullkdb (triple fault)
                           p4-newmdb  p4-iofp   (idle, §121-§122)
    do not build (1)      p4-cm  -- compatibility mode, glue/v4-x86/utcb.h (§95)

Gate: 0 errors, 709 symbols with identical bodies. The other six building
configs rebuild unchanged.


## §124 — fullkdb's triple fault: `rdpmc`, and QEMU does not implement it

Bisected to a single option and then to a single instruction. **It is not a
migration fault.**

### The bisect

`x86-x64-p4-fullkdb` differs from the booting `x86-x64-p4` in nine options.
Disabling all nine boots; disabling either half boots; disabling
`CONFIG_TBUF_PERFMON` alone boots. `CONFIG_TRACEBUFFER` alone also boots, but
only because `TBUF_PERFMON` depends on it — `TBUF_PERFMON` is the minimal
culprit.

An earlier round of this bisect gave contradictory answers (single options
faulting, pairs booting, which reads like a size threshold). That was a broken
harness, not a real signal: it detected "booted" by grepping for the
`CONFIG_VERBOSE_INIT` banner, in runs whose build directory still carried
hand-edits from previous experiments. Re-running with the tar re-extracted each
time and the roottask banner as the marker gave a clean single-option answer.
Worth remembering that a bisect over configurations needs the build directory
reset every iteration, exactly like a `git bisect` needs a clean tree.

### The instruction

`qemu -d int` shows **18 × `v=06`** — #UD, invalid opcode — followed by `v=08`,
a double fault. Resolving the first faulting IP:

    ffffffffc0612ef5:  0f 33    rdpmc
    ...inside __tbuf_record_event

With `CONFIG_TBUF_PERFMON`, `arch/x86/tracebuffer.h` reads
`x86_rdpmc(0)`/`x86_rdpmc(1)` on **every trace record**. QEMU's TCG does not
implement the performance counters and raises #UD. Tracepoints fire during
early init, the fault handler traces as well, so the #UD recurses eighteen
times into a double fault and then a triple fault — which is why QEMU exits
before a single character of output.

On a real Pentium 4 with the counters configured this is a normal instruction.
The configuration is not broken; it is asking for hardware the emulator does
not provide.

### Consequence for the tally

`x86-x64-p4-fullkdb` should be counted as **building and correct**, with the
caveat that it cannot be boot-tested under QEMU. Booting it here requires
`CONFIG_TBUF_PERFMON=n`, which was verified to reach userland.

    build, boot verified (7)   p4-smp  p4  p3  k8  p4-nokdb  p4-fp  p4-statictcbs
    build, boots without
      TBUF_PERFMON (1)         p4-fullkdb   -- rdpmc unimplemented in QEMU
    build, do not boot (2)     p4-newmdb  p4-iofp   (§121-§122)
    do not build (1)           p4-cm

Eight of eleven x64 configurations are now known-good, against one at §95.


## §125 — `namespace`, in C: compatibility mode, and all eleven x64 configs build

`x86-x64-p4-cm` was the last configuration that did not compile. It does now,
and it boots to userland. **Eleven of eleven x64 configurations build**, which
is the first time in this migration that the configuration set has been whole.

### What it was actually blocked on

Compatibility mode runs 32-bit tasks on the 64-bit kernel, so it needs a second
copy of the V4 API types built with a 32-bit word: a 32-bit `threadid_t`, a
32-bit KIP, a 32-bit UTCB, alongside the 64-bit ones, in the same translation
unit. The original got that by re-including `api/v4/{types,thread,
kernelinterface}.h` — include guards undefined, `word_t` typedef'd to `u32_t` —
inside `namespace x32 { }`.

That is the one C++ feature this tree used that has no C spelling. Everything
else the migration met was a class, a method, a template or an `extern "C"`;
this is a name-isolation mechanism, and C has exactly one: the preprocessor.

### The stand-in

`glue/v4-x86/x64/x32comp/x32-names.h` renames every name those headers declare
to an `x32_`-prefixed one. It is used in pairs around each re-include:

    #include INC_GLUE_SA(x32comp/x32-names.h)
    #undef __API__V4__THREAD_H__
    #include INC_API(thread.h)
    #define X32_UNRENAME
    #include INC_GLUE_SA(x32comp/x32-names.h)
    #undef X32_UNRENAME

108 names, in two lists that have to stay in step — and they stay in step under
pressure, which is what makes this tolerable rather than merely ugly. A name
missing from the rename list collides on the second include; a name missing
from the unrename list leaves the 64-bit code compiling against an `x32_` name.
Both are compile errors, immediately, with no silent-wrong state in between.

The nested includes cost nothing: their guards are already set from the first
pass, so the rename only ever reaches the one file's own text. That was already
true of the `namespace` version — it had to be, or the nested content would
have landed inside the namespace.

### The part that came out for free

`BITS_WORD` is `(sizeof(word_t)*8)` (`generic/types.h`). It is a macro, so it
is expanded at each *use*, with whatever `word_t` means there — and inside the
rename it means `x32_word_t`. So `memory_info_t`'s `n : BITS_WORD/2` becomes a
16-bit field, and the whole KIP lays itself out in 32-bit words without a
single width being written down twice. Confirmed in the image: `.kip_32` has
the magic at 0 and `api_version` at 4, against 0 and 8 in `.kip`.

Which incidentally answers what `KIP_BITS_WORD` was for. It is set by
`x32comp/kernelinterface.h` and read *nowhere* — not in this tree, not in the
2007 upstream import either. Someone saw that the KIP's field widths did not
follow the KIP's word size and started to parameterise it; the C
`sizeof`-derived form solves it by construction.

### The UTCB had to become a function call again

Compatibility mode's `utcb_t` is a union of the two layouts, and every accessor
picks between them on a flag. That only works if the accessors are functions —
and this migration had emptied `api/v4/generic-utcb.h` and inlined the field
access at each call site (`self->utcb->pager`), which reads better everywhere
except the one configuration that cannot do it.

So `generic-utcb.h` is a set of C free functions again, `api/v4/accessors.c`,
`api/v4/tcb.h`, both `sched-*/schedule.c` and `glue/v4-x86/thread.c` go through
them, and `x32comp/utcb.h` supplies the dispatching set under the same names.
On everything but compatibility mode they are the field access spelled out:
**the reference config rebuilds byte-identical**, disassembly included.

The layout itself moved to `glue/v4-x86/utcb-body.h`, which takes its struct
name from `UTCB_NAME` so it can be emitted twice. `Mk/Makefile.voodoo` scans it
for the `TCB_START_MARKER` field list along with `utcb.h`, and gets the plain
64-bit layout because `utcb.h` keys the union off `!defined(BUILD_TCB_LAYOUT)`
— which is right, since the generator's consumer is the 64-bit syscall stub.

### One thing that genuinely could not be a static initializer

`KIP_MEMORY_INFO` is `{{raw: (addr_word_t) &KIP_MEMDESCS_RAW}}`, where the
"address" is a linker-computed `(offset << 16) + size`. For the 32-bit KIP that
field is 32 bits wide, the value fits, and the linker could emit it — but to
the compiler it is a truncating cast of a 64-bit symbol address, which is not a
load-time constant. `kernel_interface_page_init` assigns it instead, under
`KIP_MEMDESCS_RAW_AT_RUNTIME`.

### What the sweep found once it was run properly

§124's lesson was that a sweep over configurations needs the build directory
reset every iteration. Acting on it turned up two failures that had been hidden
behind a build directory whose config had drifted from every shipped one:

  - `glue/v4-x86/exception.c` calls `frame->dump()` under `CONFIG_KDB` — the
    eighth appearance of the gate-blind pattern, and it broke **seven** of the
    eleven configurations. The main build directory's config has `CONFIG_KDB`
    off, so nothing had compiled that line since it was converted.
  - `CONFIG_KDB_BOOT_CONS` is read unguarded by `kdb/generic/console.c`, and
    the pre-2010 configs in `contrib/configs` predate the option. Defaulted to
    `rules.cml`'s 0.

Plus one defective artifact: `x86-x64-p4-statictcbs.kernel.tar` ships
`config/config.h` but no `config/.config`, and the *make* variables that decide
which subsystems reach `SOURCES` come from `.config` — so it configured a kdb
kernel and linked one without kdb. Derived from `config.h` when absent.

All of this is now `tools/configsweep`, so the clean-tree sweep is a command
rather than a discipline.

### Result

    build (11)          p4-smp  p4  p3  k8  p4-nokdb  p4-fp  p4-statictcbs
                        p4-fullkdb  p4-newmdb  p4-iofp  p4-cm
    boot verified (8)   the seven of §124, plus p4-cm
    boot, with caveats  p4-fullkdb   -- needs TBUF_PERFMON=n under QEMU (§124)
    do not boot (2)     p4-newmdb  p4-iofp   (§121-§122)

`p4-cm` boots to the l4test menu, with sigma0 and the root task relinked: the
extra `.kip_32` section pushes `.init` to 0x00f0f000, past sigma0's default
0x00f00000 link base, and kickstart rejects the overlap. That is a link-base
choice, not a kernel fault.

Honest limit on what that proves: there is no 32-bit userland in this tree, so
the compatibility path is exercised as far as `init_kip_32` — which does build
the 32-bit KIP and run `memory_info_insert` on the 32-bit twin — and no
further. The syscall dispatch, the UTCB dispatch and the 32-bit thread ids
compile and are reachable, but nothing has called them.

Gate: reference config byte-identical (332328, disassembly identical),
`tools/boottest` PASS, `tools/configsweep` 11/11.


## §126 — Boot-testing all eleven: nine reach userland, and two never could

With §125's sweep building every x64 configuration, the obvious next question
is which of them run. Booted all eleven from clean-tree builds
(`tools/configsweep`, then `tools/boottest` on each). Nine reach userland.

    boot to the l4test menu (9)   p4-smp  p4  p3  k8  p4-nokdb  p4-fp
                                  p4-statictcbs  p4-cm  p4-fullkdb*
    do not boot (2)               p4-newmdb  p4-iofp   (§121-§122)

    * p4-fullkdb needs CONFIG_TBUF_PERFMON=n under QEMU (§124's rdpmc) and a
      'g' at the KDB break-in prompt.  It was never failing -- it was waiting.

All eleven were booted against one userland with sigma0 at 0x01000000 and the
root task at 0x01400000, rather than the configured 0x00f00000/0x01000000:
`p4-cm`'s extra `.kip_32` section pushes `.init` past sigma0's link base and
kickstart refuses the overlap. Using the relinked pair for every configuration
keeps the eleven runs comparable.

### Two configurations were failing before the kernel ran

`p3` and `p4-nokdb` linked `.init` at **0x40101000** — a gigabyte up, past the
256 MB QEMU gives them — so kickstart saw the kernel's span swallow sigma0 and
refused to launch.

The cause is four characters in the linker script. `.kdebug` collected
`*(.comment)`, and with `CONFIG_KDB` off there is no `.kdebug` input at all, so
`.comment` was the output section's only content. `.comment` is not
`SHF_ALLOC`; a section whose entire content is non-alloc is itself non-alloc;
the location counter does not run through a non-alloc section; `.` wrapped past
2^64, and `. - KERNEL_OFFSET` came out a gigabyte high.

`.comment` was *already* listed in the script's `/DISCARD/` — the earlier match
simply won. Deleting it from `.kdebug` fixes the link and, incidentally, stops
a 50-byte `GCC: (Ubuntu 13.3.0-...)` identification string being loaded into an
allocated, executable section of every kernel this tree has built.

### And one was triple-faulting on its first `printf`

`k8` launched and then exited QEMU within five seconds with no output at all.
`-d int` showed a single `v=0e` straight into `v=08`; `-d in_asm` showed the
last block executed was

    mov  -0x1b4b(%rip),%rax      # kdb_current_console
    shl  $0x5,%rax               # sizeof(kdb_console_t)
    jmp  *-0x3f3f92b0(%rax)      # kdb_consoles[cur].putc

jumping to 0, running off through the BIOS area and into `.rodata`. That is
`putc`, on the very first `printf` — the `CONFIG_VERBOSE_INIT` banner.

`k8`'s configuration has `CONFIG_DEBUG` set and `CONFIG_KDB` **unset**. That
combination compiles the debug output but no console driver: every driver is
gated on a `CONFIG_KDB_CONS_*`, and those need `CONFIG_KDB`. The linker set is
empty, `kdb_consoles[0].putc` is null, and the first character printed is a
triple fault — nothing to see, and no way to see it.

`init_console` and the console-switch command have always tested that pointer
before calling it. `putc` and `getc` never did, in this tree or in the upstream
import. They do now: two instructions in the debug path, and `k8` boots.

Note what the l4test output on `k8` is and is not. The kernel prints nothing —
it has no console. The banner and menu on the serial line come from the
*userland's* own COM driver (`user/lib/io`), so reaching the menu still proves
kernel, sigma0 and root task all ran. What it does not prove is that anything
in the kernel printed. The menu then polls a console that is not there, so
`k8` spins: 174167 `int3` in sixty seconds, against thirteen interrupts total
for `p4-smp` sitting idle at the same prompt. That is the userland busy-waiting
on an empty console, not a kernel fault.

### The gate, and a lesson about the gate

    709 symbols in common, 706 with identical bodies

The three that differ are `putc` and `getc` — the null tests, deliberate — and
`cmd_virt_to_phys`, which is not a code change at all: the `.comment` bytes sat
directly after it, so objdump had been disassembling them as part of that
symbol. Its own instructions are untouched.

Getting that number required admitting the comparison had been wrong. The
ad-hoc script used up to here diffed two whole disassemblies and piped the
result through `head -60`. That is fine while changes are tiny — and every
earlier check in this migration produced two or twenty-two differing lines, well
inside the window, so those conclusions stand. But `putc` growing by sixteen
bytes shifts every symbol after it in `.kdebug`, and the honest diff was 10635
lines: `head` reported fifteen hunks of "only a call target moved" and hid the
rest. A truncated verification does not fail loudly; it agrees with you.

`tools/cmpsyms` replaces it: split at symbol boundaries, drop each
instruction's own address, replace hex literals and branch targets with a
placeholder, compare bodies. Whole-section shifts stop mattering and the number
means what it says.


## §127 — x32: the other half of the architecture, and why none of it built

Boot-testing the x32 configurations turned out to be a question about building
them. **None of the twenty had ever built in this tree.** They do compile now —
`x86-x32-p4` reaches the link with zero errors — and stop one symbol short of a
kernel for a reason that is not in the source.

### Three failures deep, and the cause was a filename

The first sweep gave every x32 configuration the same three errors:
`tcb_layout.h: No such file or directory`. But the generator had not failed —
it had never run, because `make` had no dependency information at all.

`Mk/Makeconf`'s `.depend` rule ends:

    done 2>&1 | $(GREP) . && $(RM_F) $@ && exit -1 || exit 0

Any output at all from the preprocessing loop deletes `.depend` — and then
`|| exit 0` reports success. `make` proceeds with no dependencies, nothing
generates `include/tcb_layout.h`, and every object fails on the missing header.
The output in question was `No rule to make target
'src/generic/linear_ptab_walker.cc'`: the x32 `Makeconf` still named three
files this migration had renamed to `.c` long ago. Fixed, along with the same
staleness in the powerpc64 kdb `Makeconf`.

### What was actually left of x32

Thirteen headers still C++, and fifteen `.cc` files. Converted this pass, in
the order the compiler asked for them:

    arch/x86/x32/     trapgate.h  ptab.h  tss.h  segdesc.h
    glue/v4-x86/x32/  ktcb.h  tcb.h  space.h  syscalls.h  config.h  hwirq.h
                      init.c  exception.c  space.c  user.c  memcontrol.c
                      thread.c
    kdb/              arch/x86/x32/x86.c  glue/v4-x86/x32/space.c

`x86-x32-p4`'s error count went 100 → 96 → 74 → 69 → 4 → 0.

### The x64 migration had folded x64 into the shared files

Three shared files turned out to be x64-only in their bodies, because there was
no second subarchitecture to keep them honest:

  - `glue/v4-x86/thread.c` — `switch_to`, `do_ipc`, `return_from_ipc`,
    `return_from_user_interruption`, `copy_mrs`, the `notify` trio and
    `initial_switch_to` are all register-level asm. Split on `CONFIG_IS_64BIT`,
    which is the idiom that file already used for `return_to_user` and
    `EXC_FRAME_SIZE`.
  - `glue/v4-x86/space.c` — six x64-isms: the canonical-address sign extension
    (x32 has no non-canonical hole, so it is the identity), the kernel-PDP
    accessors (a level of the four-level page table x32 does not have), and the
    copy-area shift.
  - `glue/v4-x86/exception.c` and `init.c` — `frame->ds`, `mem_region_t::set`
    and friends, inside `#if defined(CONFIG_SUBARCH_X32)` blocks that no build
    had ever compiled. The ninth and tenth appearances of the gate-blind
    pattern; they cost nothing to fix once something finally compiled them.

Two spellings had to converge. `X86_EXC_RAXREG` and friends named x64
registers in code shared with x32, so both subarchitectures now define a
role-based set — `X86_EXC_AREG`, `X86_EXC_DREG`, `X86_EXC_IPREG` — and the
shared glue indexes by role. Likewise `x86_idtdesc_set` takes x64's `ist`
argument on both, ignored on x32, so `glue/v4-x86/idt.c` has one call to make.

`X86_EXC_NUM_DBGREGS` was hardcoded to 18 in the shared `trapgate.h`; x32 dumps
twelve registers. It comes from the subarch header now.

### Where it stops

    ld: cannot find -lgcc: file in wrong format

The one unresolved symbol is `__udivdi3` — 64-bit division on a 32-bit target,
from `libgcc`. This machine has no 32-bit `libgcc.a`; `-m32` compiles, but
`gcc-multilib` is not installed. That is the whole remaining distance between
here and an `x86-x32-p4` kernel image, and it is not in the tree.

### Not converted, and deliberately

`CONFIG_X_CTRLXFER_MSG` — `arch_ktcb_t`'s static tables and their definitions in
`x32/thread.c`. No configuration in `contrib/configs` sets the option, so it has
not been compiled at any point in this migration, and §95, §116 and §123 are all
records of what converting under an uncompiled gate produces. `x32/ktcb.h`
`#error`s if the option is turned on, which is a worse state than before only
for someone who was already going to have to do this work.

The five configurations outside the core set — HVM (`vmx.cc`, `hvm-vmx.cc`,
`hvm-vtlb.cc`), logging, small spaces — are untouched: 4229 lines, and their
configurations were never in this pass's scope.

Gate: x64 unaffected, 709 symbols with 709 identical bodies.


## §128 — x32 boots, and the alignment attribute the conversion dropped

`gcc-multilib` is installed on this machine now, so `__udivdi3` resolves and
`x86-x32-p4` links. It is the first x32 kernel image this migration has
produced. It also asserts on the first kernel-memory allocation of the boot:

    Assertion (size % KMEM_CHUNKSIZE) == 0 failed in generic/kmemory.c:170

The caller is `space_init_kernel_space`, and the size is `sizeof (space_t)`:
3096 on x32, not a multiple of 1024.

### One attribute, and what it was holding up

The C++ `x86_space_t` ended

    } __attribute__((aligned(X86_PAGE_SIZE)));

and §127's conversion of `x32/space.h` did not carry it over. x64's equivalent
`aligned(X86_PTAB_BYTES)` survived its own conversion, so the x64 sweep had no
way to notice. Nothing else in the tree mentions the alignment, and nothing
had ever compiled the x32 header, so between the two passes the requirement
existed only in a line that was gone.

It is holding up `space_allocate_space`, which carves a space and its top page
directory out of a single block:

    kmem_alloc (sizeof (space_t) + sizeof (x86_top_pdir_t))
    top_pdir = (addr_t) space + sizeof (space_t)

Two properties are required and neither is stated anywhere. `sizeof (space_t)`
must be a whole number of pages, because the top pdir is walked by hardware.
And the sum must be a power of two, because `kmem_do_alloc`'s alignment test is
`!(addr & (size - 1))` — a mask, not a modulus — and its `ASSERT` wants a
`KMEM_CHUNKSIZE` multiple. Padded to 4096 both hold, and 4096 + 4096 is what
x64 has always passed.

With assertions compiled out this would not have stopped: `kmem_do_alloc` would
have walked three 1024-byte chunks for a 3096-byte request and returned a page
directory 3096 bytes into the block, unaligned. The assertion is the only thing
between the dropped attribute and a kernel that corrupts its own heap.

`glue/v4-x86/space.c` now states both properties as `_Static_assert`s next to
the allocation. They are cheap and they are checked on every subarchitecture.

### Three more files, and the five configurations outside the core set

`x86-x32-p4` reaches the l4test menu once the space is page aligned. The sweep
then put eight of nineteen configurations at three distinct compile failures,
all in the sources §127 left alone:

  - `kdb/arch/x86/x32/disas.c` — the x32 disassembler wrapper, three
    configurations. x64's equivalent was converted in the x64 pass; this is the
    same work. `f->eip` becomes `f->__base.regs[X86_EXC_IPREG]`,
    `get_kernel_space` becomes `get_kernel_space_c`, and `get_hex` takes the
    third argument the C form has.
  - `glue/v4-x86/x32/logging.c` and its header, two configurations. A default
    argument, two `extern "C"` in macros, `x86_mmu_t::flush_tlb`, the memdesc
    and pgent method calls, and `sched_state.get_logid()` — which is a plain
    `word_t logid` member in C, as `sched-hs/schedule.c` already assumed.
    `memdesc_t::set` had no C form yet; it does now.
  - `glue/v4-x86/x32/smallspaces.c`, its kdb command file and the
    `smallspace_id_t` class, one configuration. The class becomes a struct with
    `smallspace_id_*` functions, and the `x86_space_t` methods §127 dropped
    from the header come back as `x86_space_*` functions with `space_*`
    wrappers in the shared header, the way x64 does compatibility mode.

Two more gate-blind leftovers surfaced while compiling those — the eleventh and
twelfth. `api/v4/schedule.c` had `sched_state.set_logid()` and `get_idle_tcb()`
inside `#if defined(CONFIG_X_EVT_LOGGING)`; `glue/v4-x86/exception.c` had
`frame->reason`, `frame->eflags`, `frame->ecx`, `frame->eip` and three tcb
methods inside `CONFIG_X86_SMALL_SPACES`. Neither gate had ever been on.

`memdesc_set` needed adding to the x32comp prefix list, or compatibility mode
sees two conflicting declarations of it.

### Where it stands

Seventeen of nineteen configurations compile; the two that do not are HVM,
whose `vmx.cc`, `hvm-vmx.cc` and `hvm-vtlb.cc` are still C++ (285 errors, and
`x32/ktcb.h` and `kdb/arch/x86/x32/disas.c` both `#error` under the option
rather than converting blind). Ten reach the l4test menu.

Of the seven that build and do not boot, `p4-fullkdb` is §124's `rdpmc` again —
`CONFIG_TBUF_PERFMON`, QEMU, not this tree. The other six are three separate
faults, none of them diagnosed yet: the four `CONFIG_TRACEBUFFER` builds fail
an SMP-only-looking `tcb_get_cpu (self) == tcb_get_cpu (dest)` in `switch_to`;
`p4-iofp` stops at `map_fpage(): invalid fpage size`; `p4-smallspaces` has
sigma0 touching `df001000`, inside the kernel area, before it starts.

`contrib/configs/x86-x32-cxfer-kernel.tar` is named with a dash where every
other config tar has a dot, so `tools/configsweep`'s glob has never matched it.
Nineteen of twenty, and the twentieth is the `CONFIG_X_CTRLXFER_MSG`
configuration §127 declined to convert, so nothing is lost — but the sweep was
reporting a full pass over a set it had silently narrowed.

Gate: x64 unaffected. `x86-x64-p4-smp`, 706 symbols with 706 identical bodies;
`x86-x64-p4-cm`, 553 with 552, the odd one being `kernel_version_string`, which
is the build date disassembled as instructions.


## §129 — Four of the seven were the harness, or QEMU

§128 left seven x32 configurations that build and do not boot. Four of them
are not faults.

`hsched-pic`, `hsched-smp`, `logging` and `logging-smp` set
`CONFIG_KDB_ON_STARTUP`: the kernel stops in the debugger before the root task
runs and waits for a key. `tools/boottest` gave QEMU `-serial file:`, which is
write only, so the run sat at

    --- "KD# System started (press 'g' to continue)" ---

until the timeout and was reported as a kernel that produced no output. The
harness drives the line now — `-serial stdio` with a `printf 'g'` every second
— when the build's `config.h` has the option. `hsched-pic` and `hsched-smp`
reach the l4test menu with no other change.

`p4-fullkdb` is §124 again: turning `CONFIG_TBUF_PERFMON` off boots it, exactly
as on x64. QEMU's TCG does not implement `rdpmc`.

That leaves three real faults, all in features whose configurations have never
been built in this tree at all:

  - `CONFIG_X_EVT_LOGGING` — `switch_to` fails
    `tcb_get_cpu (self) == tcb_get_cpu (dest)` on the first thread switch after
    the root servers are created, and then repeats it forever. Turning only
    that option off in the same build boots to userland, so it is the logging
    code and not the rest of the configuration. `LOG_PMC` is never invoked
    anywhere in the tree, so the injected trace points are not it; what the
    option does change is `sched_ktcb_t` (it gains a `logid`, moving every
    `tcb_t` field after `sched_state`), the KIP, and `init_logging_cpu`'s
    remapping of a megabyte of kernel log area.
  - `CONFIG_X86_IO_FLEXPAGES` — `map_fpage(): invalid fpage size` during init.
  - `CONFIG_X86_SMALL_SPACES` — sigma0 touches `df001000`, inside the kernel
    area, before it starts. The small space area is carved out of the user
    area, so a boundary is the obvious suspect.

Ten of nineteen configurations reached userland when §128 was written; twelve
do now, and thirteen with `TBUF_PERFMON` off.


## §130 — Small spaces: the thirteenth gate-blind block

    sigma0 accessed kernel space @ df001000, ip=df001000 - deny

`df001000` is `UTRAMP_MAPPING`, and the instruction pointer is in it, so sigma0
was executing there. With `CONFIG_X86_SMALL_SPACES` and `CONFIG_X86_SYSENTER`
together, `sysexit` cannot return straight to the user IP — the user code
segment is limited to the small space — so `x32/trap.S` sends it to a
four-instruction trampoline that reloads `%ds` and `%ss` and does an `lret`.
The trampoline is its own linker section, placed at `UTRAMP_MAPPING`, which is
inside the kernel area, and it therefore has to be mapped back into every
address space with user rights:

    #if defined(CONFIG_X86_SMALL_SPACES) && defined(CONFIG_X86_SYSENTER)
        /* User-level trampoline for ipc_sysexit, readonly but global. */
        extern word_t _start_utramp_p[];
        add_mapping ((addr_t) UTRAMP_MAPPING, (addr_t) &_start_utramp_p,
                     pgent_t::size_4k, false, false, true);
    #endif

That block was in `space.cc`'s `init_kernel_mappings` and did not survive
§119's conversion of the file. It is the thirteenth of these — a conditional
compiled by no configuration in the tree at the time it was rewritten, so
nothing could report it missing. The first user-level instruction sigma0
executes after its first IPC is in that page, so the fault is immediate and
total: the config had never got a single instruction into the root task.

Restored verbatim, with `cacheable` spelled out (it was a C++ default
argument). `x86-x32-p4-smallspaces` reaches the l4test menu, and driving the
suite through it gets the KIP tests, IA-32 exception IPC and the start of
memtest with no assertion and no kdb entry.

Thirteen of nineteen x32 configurations reach userland now, fourteen with
`TBUF_PERFMON` off. Gate: `x86-x64-p4-smp`, 706 symbols with 706 identical
bodies — the restored block is inside a gate x64 does not set.


## §131 — IO flexpages: three stubs that were only right with the option off

    KD# map_fpage(): invalid fpage size

The kdb entry is in `linear_ptab_walker.c`'s `space_map_fpage`, which is the
memory mapping path. An IO flexpage had reached it. `api/v4/ipcx.c` decides:

    if (fpage_is_mempage (&snd_fpage))       space_map_fpage (...)
    else if (fpage_is_archpage (&snd_fpage)) arch_map_fpage_c (...)

and `api/v4/accessors.c` had

    bool fpage_is_archpage (fpage_t *self)  { return false; }
    bool fpage_is_mempage (fpage_t *self)   { return true; }

with a comment saying CONFIG_X86_IO_FLEXPAGES is off. It is off in ten of the
eleven x64 configurations and in eighteen of the nineteen x32 ones, and
`accessors.c` is compiled in all of them.

In C++ these were `arch.is_valid_page() == false` and `== true`, and
`arch_fpage_t::is_valid_page()` is a constant `false` on an architecture with
no architecture-specific flexpages and a real test of the two-bit tag under
`CONFIG_X86_IO_FLEXPAGES`. The same flattening had happened to `get_base`,
`get_address`, `get_size`, `get_size_log2`, `set` and `is_complete_fpage`,
each of which the C++ wrote as `is_mempage() ? mem... : arch...`, and to
`is_overlapping`, `is_range_in_fpage` and `is_range_overlapping`, which called
`is_complete_fpage()` and had been rewritten to test `mem.x` directly.
`generic-archfpage.h` now carries the C forms of the arch_fpage_t methods --
all of them the constant that makes the arch half fold away -- and the
accessors have both branches back.

That got the classification right and the fpage still did not map, because

    void arch_map_fpage_c (...) { }
    void arch_unmap_fpage_c (...) { }

in the same file are also unconditional. They are the C wrappers `ipcx.c`
needs because it cannot see `INC_GLUE(map.h)`, and the no-op body is the
generic `arch_map_fpage` inline, not the one in `glue/v4-x86/io_space.c`. They
forward now.

And with the call arriving, `arch_map_fpage` still did nothing, because it
opens `if (space_get_io_space (sspace))` and sigma0's space had none.
`space_t::init` had

    #if defined(CONFIG_X86_IO_FLEXPAGES)
        if (!sigma0_space)
        {
            set_io_space(new vrt_io_t);
            get_io_space()->populate_sigma0();
        }
    #endif

-- the first space initialised is sigma0's, and it starts out owning every IO
port. §119 flattened that function too, and with it the
`CONFIG_X86_COMPATIBILITY_MODE` arm above it, which maps the 32-bit KIP into a
compatibility-mode space instead of the 64-bit one. Both are back; the C++
`else` is a `return`.

Three separate stubs, each individually correct for the configuration the
author was compiling and wrong for the one that was not. `x86-x32-p4-iofp`
reaches the l4test menu: five IO pagefaults, five mappings, done.

`x86-x64-p4-iofp` still does not boot. It takes a kernel-mode #GP in early
init, before any of this code runs, and it did so before this change as well.
Not the same fault, and not yet looked at.

### The gate was comparing the tree with itself

§128's and §130's "706 symbols with 706 identical bodies" were not
measurements. The reference build was a `git worktree` at the previous commit,
built with a `Makeconf.local` copied from `build/x86-x64-p4-smp/kernel` -- and
`SRCDIR` in that file is absolute, pointing at the main tree. Both sides of
every comparison compiled the same, current, sources. `tools/configsweep`
copies the same donor, so any reference build made this way is worthless
unless `SRCDIR` is rewritten to the worktree.

Rerun properly, against `48c8cda` (this session's starting point, so it covers
all three commits): `x86-x64-p4-smp`, 706 symbols and 705 identical bodies,
the one being `fpage_is_range_in_fpage`, which now routes through
`fpage_is_complete_fpage` and `fpage_get_address` instead of testing `mem.x`
inline -- same instructions, different registers. `x86-x64-p4-cm`, 553 and 549:
that one, `space_init` with the compatibility-mode arm restored, the ctor table
that shifted because `space_init` moved, and `kernel_version_string`, which is
the build date.

The x64 boot tally is unchanged by all three commits -- the same six of eleven
pass at `48c8cda` as pass now. Which also means §126's "nine of eleven" does
not reproduce: `p4-cm` and `p4-statictcbs` do not get past kickstart with the
current userland, and `p4-newmdb` and `p4-iofp` reach the kernel and stop. All
four fail identically at the older commit, so this is a difference in how they
were measured then, not a regression.

Fourteen of nineteen x32 configurations reach userland. `p4-fullkdb` is §124's
`rdpmc`; the two `CONFIG_X_EVT_LOGGING` builds are §129's remaining fault; the
two HVM configurations do not compile.


## §132 — Logging: `--gc-sections`, and four linker scripts with no `KEEP`

The `CONFIG_X_EVT_LOGGING` builds failed an assertion in `switch_to`:

    tcb_get_cpu (self) == tcb_get_cpu (dest)

`self` was the idle TCB and `dest` was `NULL`; the return address was in
`cpu_kdb_do_enter_kdebug`, which ends

    tcb_switch_to (get_current_tcb(), cpu_kdb.kdb_tcb);

and `cpu_kdb.kdb_tcb` was null because `cpu_kdb_ctor` had never run. Nothing
was wrong with the constructor. `__ctors_GLOBAL__` in that kernel contained
its terminating `QUAD(0)` and nothing else, where the same build without the
option has an entry.

`Mk/Makeconf.x86`:

    ifeq "$(CONFIG_X_EVT_LOGGING)" "y"
    KLDFLAGS_x86_x32 += --gc-sections
    endif

That is the only configuration in the tree that links `--gc-sections`, and
`generic/ctors.ldi` has no `KEEP`. Constructor sections are never referenced by
symbol -- they are walked at run time between the `__ctors_CPU__`,
`__ctors_NODE__` and `__ctors_GLOBAL__` markers -- so from the collector's
point of view they are unreachable, and every static initializer in the kernel
was discarded. ld's own default linker script wraps `.ctors`, `.init_array` and
friends in `KEEP` for exactly this reason; this script does not use the
default.

With the constructors back the kernel reached `System started (press 'g' to
continue)` and then ignored the key, because the same thing had happened to the
kdb command table: `_start_sets == _end_sets`. Linker sets are walked between
markers too. `generic/linkersets.ldi`, `generic/mdb.ldi` and the logging
feature's own `x32/logging.ldi` -- whose `.log.evtenable.*` and
`.log.evtlist.*` sections hold the patch points `toggle_events` walks, and
whose `0xDEADBEEF` terminators had gone the same way -- all now `KEEP`.

`KEEP` is a no-op without `--gc-sections`, so nothing else in the tree moves:
`x86-x64-p4-smp` is 706 symbols with 706 identical bodies against the previous
commit, and the x64 boot tally is unchanged.

This is not a migration fault. The flag, the linker scripts and the marker-walk
idiom are all upstream, and no x32 configuration had ever been linked. It is
the first fault in this whole pass that the C rewrite could not have caused --
and the reason it took three probes to find is that a collected section leaves
nothing behind to read: the constructor is simply not there, and the code that
walks the list finds an empty list and carries on.

Sixteen of nineteen x32 configurations reach userland. The remaining three are
`p4-fullkdb`, which is §124's `rdpmc` under QEMU and not a kernel fault, and
the two HVM configurations, which do not compile.


## §133 — x32 ctrlxfer: the twentieth configuration, and `get_user_frame`

`tools/configsweep` globs `contrib/configs/$PATTERN.kernel.tar`.
`x86-x32-cxfer-kernel.tar` spells the suffix with a dash, so nineteen of the
twenty shipped x32 configurations were being swept and the twentieth -- the
only one that sets `CONFIG_X_CTRLXFER_MSG` -- was invisible. That is what
§128's comment in `x32/ktcb.h` was wrong about: it said no configuration in
`contrib/configs` turns the option on, and refused to convert the control-transfer
path for want of something to compile it against. There was something.

The gate now covers both spellings, and the sweep reports twenty.

The conversion itself is the powerpc shape, which was already done:
`arch_ctrlxfer_item_t` in `glue/v4-x86/ipc.h` was a class wrapping nothing but
enums, so the wrapper goes and `id_gpregs`, `gpreg_eflags` and the rest keep
their names at file scope; `arch_ktcb_t::get_x86_gpregs` and its three siblings
take the receiver first and turn their `word_t&` out-parameters into pointers;
the two static member tables become the file-scope `get_ctrlxfer_regs[id_max]` /
`set_ctrlxfer_regs[id_max]` that `api/v4/thread.c` already indexed;
`ctrlxfer_item_t::num_hwregs` and `::hwregs` become `ctrlxfer_num_hwregs` and
`ctrlxfer_hwregs`, which `api/v4/ipc.h` had already been declaring.
`bitmask_t<u32_t>` is `bitmask_u32_t`, and its `string()` -- the bracketed
picture kdb prints for a fault mask -- is a `bitmask_string (maskvalue, width)`
in `generic/bitmask.h`.

`glue/v4-x86/ipc.cc` and `kdb/glue/v4-x86/ipc.cc` are the last two x86 `.cc`
files outside HVM, and both are now `.c`.

Two things were not mechanical.

**Four dropped `#if` blocks.** `api/v4/thread.cc` had seven
`CONFIG_X_CTRLXFER_MSG` sites; `api/v4/thread.c` had two. The five missing ones
are `fake_wait_for_startup` (set the ctrlxfer acceptor bit and a
complete-memory receive window), `tcb_activate` (zero `fault_ctrlxfer[]`),
`send_pagefault_ipc` and `send_preemption_ipc` (both set the acceptor bit and
append the kernel fault item). `x32/tcb.h` lost two more: the three
`tcb_t` ctrlxfer methods. Same class of fault as §95, §116 and §123 -- a
conditional block deleted because nothing compiled it -- and the same reason it
went unnoticed: with the option unreachable, nothing linked against the result.
The three `tcb_t` methods are out-of-line in `glue/v4-x86/thread.c` rather than
`INLINE` in a header, because `api/v4/tcb.h` declares two of them `extern` and
C rejects a `static inline` definition of a name already declared without it.
`tcb_append_ctrlxfer_item` is declared in `x32/ktcb.h` instead, because powerpc
keeps its own copy `INLINE` and a declaration in `api/v4/tcb.h` would clash
with it.

**`get_user_frame` has no definition anywhere upstream.** `glue/v4-x86/ipc.h`
declared `x86_exceptionframe_t *get_user_frame(tcb_t *)` and no translation
unit in the tree defines it -- `git grep` over `c881a86`, the import commit,
finds the declaration and eight calls and nothing else. So the x86
control-transfer path has never linked, in this tree or the one it came from,
and neither has HVM, which calls it four times. What upstream's own `x32/tcb.h`
had, before §116 collapsed the `__cplusplus` guards over it, was

    INLINE x86_exceptionframe_t *get_user_frame(tcb_t *tcb)
    { return ((x86_exceptionframe_t*) (tcb->get_stack_top()) - 1); }

inside the `CONFIG_X_CTRLXFER_MSG` block -- an `INLINE`, so no symbol, which is
why nothing ever complained. It is restored as an out-of-line function in
`x32/thread.c` (it cannot be `INLINE` in `ipc.h`: that header is reached from
`api/v4/tcb.h` well before `tcb_get_stack_top` is declared), and the arithmetic
checks out against `x32/config.h`: the frame is 17 words, the trapgate wrapper
pushes it at the top of the kernel stack, and `KSTACK_UIP`, `KSTACK_UFLAGS` and
`KSTACK_USP` are -5, -3 and -2 against `regs[]` indices 12, 14 and 15 of 17.

`x86-x32-cxfer` compiles, links and boots to the `l4test` menu. Eighteen of
twenty x32 configurations compile; the two that do not are the HVM pair, whose
error count went from 285 to 347 because the `ktcb.h` `#error` is no longer
short-circuiting the rest of the HVM headers.

Nothing else moves: `x86-x32-p4-smp` and `x86-x64-p4-smp` are 635 and 706
symbols against `0e13138` with 634 and 706 identical bodies, the one being
`kernel_version_string`, which is the build date. All eleven x64
configurations still compile.


## §134 — HVM: the VMCS field template, and five more dropped `#if` blocks

The two HVM configurations are the last x86 ones that did not compile. Five
thousand lines across ten files: `arch/x86/vmx.h`, `arch/x86/x32/vmx.h` and
`vmx.cc`, `glue/v4-x86/hvm.h`, `hvm-vtlb.h`, `hvm-space.h` and `hvm-space.cc`,
and `glue/v4-x86/x32/hvm-vmx.h`, `hvm-vmx.cc` and `hvm-vtlb.cc`.

The interesting one is `arch/x86/vmx.h`. A VMCS is a hardware-managed page
whose fields are reachable only through VMREAD and VMWRITE, indexed by an
encoding; upstream expressed that as

    template<word_t index, typename T> class vmcs_field
    {
        T operator= (T val)
            { ASSERT (x86_vmptrtest ((u64_t) (word_t) this));
              x86_vmwrite (index, val.raw); return val; }
        operator T ()
            { ASSERT (x86_vmptrtest ((u64_t) (word_t) this));
              T val; val.raw = x86_vmread (index); return val; }
    };

with specialisations for `word_t`, `u16_t` and `u64_t`, and a hundred and
eleven typedefs binding an index to a type. A VMCS field then looked like a
struct member: `vmcs->gs.cr0 = x` issued the VMWRITE. The empty field classes
sat in unions, and the areas -- guest state, host state, the three control
areas, exit information -- were themselves a union in `vmcs_t`, so every member
address equalled the object's own address, which was the VMCS physical address;
that is what the `x86_vmptrtest (this)` assertion checks.

In C the field name becomes a pair of accessors over an explicit `vmcs_t *`,
generated one line per field by three macros, and named
`vmcs_<area>_get_<field>` / `vmcs_<area>_set_<field>`. The area no longer
nests -- it was a union at offset zero, so the name only ever documented which
part of the VMCS a field belongs to, and it is kept in the accessor name for
exactly that. `vmcs->gs.cr0 = x` is `vmcs_gs_set_cr0 (vmcs, x)`. The macros are
worth reading once: the struct-typed form uses `__typeof__ (val.raw)` so that
the narrowing stays where upstream had it, rather than needing a cast per raw
type. All 120 `VMCS_IDX_*` encodings and all 133 bitfield declarations are
byte-for-byte the ones that were there; that was checked by extraction and diff
rather than by eye.

The nested enums flatten with prefixes -- `vmcs_ei_reason_t::be_cr` is
`VMCS_BE_CR` -- and the enum-typed bitfields become `u32_t`, because C enum
bitfields have implementation-defined signedness and every use site compares
against the constants anyway. `vmcs_ei_vm_instr_t` and `vmcs_ei_qual_t` declared
`gpr_e` and `mem_reg_e` identically, so one copy of each serves both.

The inheritance chain `arch_ktcb_t : arch_hvm_ktcb_t : x86_svmx_hvm_t` is two
levels of pure state, so the two bases merge into one `arch_hvm_ktcb_t` --
base members first, in order, so the layout is what the chain produced -- and
`arch_ktcb_t` holds it by value as `hvm`. `addr_to_tcb()` still recovers the TCB
from a pointer to it, which is how the HVM code finds its own TCB. The one
subtlety is the ctrlxfer tables: they hold pointers to member *of
`arch_ktcb_t`*, which is what `get_ctrlxfer_regs_t` describes, so the twelve
HVM get/set entries keep that signature and open with
`arch_hvm_ktcb_t *self = &ktcb->hvm;`.

**Five more dropped `#if` blocks**, all `CONFIG_X_X86_HVM`, all the §95/§116/§123
pattern:

  - `glue/v4-x86/space.c`: `space_add_tcb` and `space_remove_tcb` lost the
    VCPU enqueue/dequeue, and both the SMP and non-SMP `space_flush_tlb` /
    `space_flush_tlbent` lost the `handle_gphys_unmap` that invalidates the
    VTLBs. Four hooks; without them a VTLB would keep stale entries across
    every TLB flush.
  - `glue/v4-x86/x32/space.c`: `space_control` lost the `v` bit that activates
    virtualization for a space -- so `SpaceControl` could never turn HVM on.
  - `glue/v4-x86/thread.c`: `tcb_return_from_ipc` lost the early return for
    threads holding the HVM resource.
  - `glue/v4-x86/space.h`: `get_hvm_space` and `is_hvm_space` went with the
    `__cplusplus` collapse and are back as `space_get_hvm_space` /
    `space_is_hvm_space`.
  - and one that is not HVM at all: `tcb_create_startup_stack` in
    `glue/v4-x86/thread.c` had both its `#if` blocks replaced by the comment
    "CONFIG_X_X86_HVM / CONFIG_X86_COMPATIBILITY_MODE are off in this config".
    The second is not off -- `x86-x64-p4-cm` sets it, and the block is what
    gives a 32-bit thread `X86_UCS32` instead of `X86_UCS` for its startup
    frame. That configuration compiled and was reported as "does not get past
    kickstart" in §131, so nothing ever executed the wrong selector; the reason
    it does not get that far is a kickstart link-base conflict between the
    compatibility-mode kernel image and the x64 sigma0, which is a build-layout
    problem and still open. Restoring the block is the only change in this
    commit that moves code in a non-HVM configuration: `x86-x64-p4-cm` is 553
    symbols with 550 identical bodies, the three being
    `tcb_create_startup_stack` itself, the constructor table that shifted
    because it grew, and the build date.

Three smaller gaps, of the `get_user_frame` kind from §133 -- declared or
called and defined nowhere:

  - `min()` was already noted in `generic/lib.h` as defined nowhere upstream;
    `hvm-vtlb.c` is the first x86 caller and simply needed the include.
  - `readmem<u64_t>` had no C form; the HVM GDT/IDT dump is its only user. Note
    that the template's `case 8` assigned the `word_t` it read straight
    through, so from user memory only the low half of a descriptor ever came
    back. Kept as it was.
  - `pgent_t::is_global (space, pgsize)` had no C form either; `hvm-vtlb.c` is
    its only caller.

`CONFIG_IO_FLEXPAGES`, which gates four blocks in `hvm-vmx.cc` and one in
`hvm-space.cc`, is spelled `CONFIG_X86_IO_FLEXPAGES` everywhere else in the
tree. Those blocks are unreachable, and one of them calls a
`create_io_bitmap` that exists nowhere; they are translated by inspection and
marked, not fixed.

Both HVM configurations compile, link and boot to the `l4test` menu, and so
does the pair with `CONFIG_KDB_DISAS` forced on, which is the only way to
compile the HVM arm of `kdb/arch/x86/x32/disas.c` -- no shipped configuration
sets both. All twenty x32 and all eleven x64 configurations compile. Nothing
moves outside HVM except the compatibility-mode block above:
`x86-x32-p4-smp` is 635 symbols with 634 identical bodies and `x86-x64-p4-smp`
706 with 706, against `0e13138`, the one difference being the build date.

`kernel/src/arch/x86/x64/init.cc` is in no `SOURCES`; the remaining `.cc` files
that are, `platform/efi/acpi.cc` and `kdb/generic/acpi.cc`, are gated on
`CONFIG_ACPI`, which no shipped x86 configuration sets. So no x86
configuration in `contrib/configs` compiles a C++ translation unit any more.

The new code carries nineteen `-Wconversion` / `-Wsign-conversion` warnings,
all narrowings that were implicit in the C++ (MSR reads into `word_t`, the
`s32_t` one-bit flags in `vmcs_exectr_pinbased_t`, the 4-bit `cr_num` field).
They are faithful and left alone.


## §135 — `x86-x64-p4-cm`: the kernel had grown past sigma0

§131 recorded `x86-x64-p4-cm` as not getting past kickstart, and §134 as a
link-base conflict. It is, and it is a real one, not a false positive:

    kernel    (0x0010a000-0x00149ff0)   => 0x00f0f000
      (0x0010a1e0-0x00122700) -> 0x00600000-0x00618520
      (0x00122700-0x00123007) -> 0x00800000-0x00800907
      (0x00123020-0x00123c2a) -> 0x00a00000-0x00a00c0a
      (0x00124000-0x00129194) -> 0x00c00000-0x00c05194
      (0x0012a000-0x00138430) -> 0x00e00000-0x00e0e430
      (0x00139000-0x001406a8) -> 0x00f0f000-0x00f166a8
     sigma0    (0x0014a000-0x00166aa8)   => 0x00f00000
      (0x0014a0c0-0x001502e0) -> 0x00f00000-0x00f06220
         Conflict with module 0 (0x00600000-0x00f166a8)

The tempting reading is that kickstart is too coarse. `elf_load` reports the
*enclosing* range of a module's segments and `check_memory` tests against that,
so the kernel occupies `0x600000-0xf166a8` as far as the check is concerned;
sigma0 at `0xf00000-0xf06220` sits in the gap between the fifth segment
(ends `0xe0e430`) and the sixth (starts `0xf0f000`) and overlaps neither.

The gap is not free space. `x64/linker.lds` reads

    _start_bootmem = .;
    . = . + BOOTMEM_SIZE;
    _end_bootmem = .;
    _start_init = . - KERNEL_OFFSET;

so between the last loaded section and `.init` there is a megabyte of boot
memory that has no ELF section and therefore no `PT_LOAD` segment. For this
kernel that is `0xe0f000-0xf0f000`, and sigma0 is inside it. A per-segment
conflict check would have let the load through and the kernel would then have
allocated boot memory over sigma0's image. The coarse check is right here, for
a reason it does not state.

Why this configuration and no other: the amd64 kernel links its physical image
at `0x600000` and gives each differently-mapped region a 2M superpage of its
own -- `.text`, `.syscalls`, `.cpulocal`, `.data` -- so the footprint is
`0x600000` plus 2M per region plus BOOTMEM_SIZE plus `.init`, and it grows by a
whole superpage whenever a configuration adds a region. `CONFIG_X86_COMPATIBILITY_MODE`
adds `.kip_32`, a fifth: `x86-x64-p4-smp` ends at `0xd20df8`, comfortably below
sigma0, and `x86-x64-p4-cm` at `0xf166a8`, above it. `x64/linker.lds` already
carries a comment about this failure mode, from the `.comment`/`/DISCARD/`
interaction in §120: "the kernel loads over sigma0 and kickstart refuses to
boot it."

So the fix is to give the kernel room, which is what kickstart's message asks
for. `user/configure.in`'s amd64 defaults move from

    default_sigma0_linkbase=00f00000        ->  01800000
    default_roottask_linkbase=01000000      ->  01900000

24M and 25M, leaving space for four more superpage regions before this can
recur. `configure` is generated, not tracked, and `autoconf` reproduces the
patched script exactly. Nothing in the tree hard-codes either address; the ia32
defaults (sigma0 at `0x20000`, below the kernel) are untouched, so x32 is
unaffected.

`x86-x64-p4-cm` now boots to the `l4test` menu. Seven of eleven x64
configurations boot, where six did before, and the four that do not are
unchanged by this:

  - `p4-iofp` and `p4-newmdb` take the same early kernel fault at
    `ffffffffc0602a68`, the one §131 left undiagnosed. (§136 diagnoses it.)
  - `p4-fullkdb` now prints the virtual-memory layout and stops there.
    (§137 diagnoses it.)
  - `p4-statictcbs` cannot be boot-tested at all. Its shipped tar has a
    `config.h` and no `.config`, and that `config.h` enables
    `CONFIG_KDB_CONS_OF1275`, `_PSIM_COM` and `_KBD` -- PowerPC consoles -- with
    `CONFIG_KDB_CONS_COM` off. `tools/boottest` turns `CONFIG_KDB_CONS_COM` on
    in `config.h`, but the console sources are selected from `.config`, so the
    link fails on `printf` and `init_console`. The configuration compiles under
    `tools/configsweep`, which derives its own `.config`; it just has no console
    a serial harness can read.

**WRONG -- see §138.** That last bullet is not a property of the configuration.
The link failure came from the scratch helper this section was built with, which
did not derive a `.config`; no `Makeconf` selects console sources from
`CONFIG_KDB_CONS_*` at all, and `boottest`'s edit to `config.h` is sufficient
for this configuration as for every other. `p4-statictcbs` boots, and it booted
at this commit too -- it sets neither `CONFIG_NEW_MDB` nor `CONFIG_TBUF_PERFMON`,
so nothing in §136 or §137 changed its behaviour, while the link-base move in
this section did. The count here should read **eight** of eleven, not seven.


## §136 — The fault at `ffffffffc0602a68`: a 63-bit bit-field shifted left

`x86-x64-p4-iofp` and `x86-x64-p4-newmdb` both took a #GP at
`ffffffffc0602a68`, which §131 left undiagnosed. That address is inside
`mdb_tree_map`, and the faulting instruction is a load through `rbx`, which the
register dump gives as `7fffffffc0672040` -- a kernel pointer with bit 63
cleared, hence non-canonical, hence #GP with error code 0. `rdi` in the same
dump is `7fffffffffffffff`, and the code that computed `rbx` is

    mov    0x10(%r13),%rbx
    movabs $0x7fffffffffffffff,%rdi
    and    $0xfffffffffffffffe,%rbx
    and    %rdi,%rbx                    <- clears bit 63

The source is `mdb_node_get_table` in `generic/mdb.h`:

    return (mdb_table_t *) (word_t) (self->next << 1);

`next` is a `word_t next : BITS_WORD - 1` bit-field holding a pointer shifted
right by one, the spare bit going to `next_is_table`. The C++ original was the
same expression, `(mdb_table_t *) (next << 1)`.

GCC's C and C++ front ends do not agree on the type of that shift. Compiled as
C++, `(next << 1)` is a 64-bit shift and the reconstruction is
`and $0xfffffffffffffffe`. Compiled as C, GCC gives a bit-field wider than
`int` the bit-field's own precision, so the shift is evaluated modulo 2^63 and
the result is masked to 63 bits: `and $0x7ffffffffffffffe`. Two lines of
throwaway C and C++ over the same struct reproduce it exactly. The `(word_t)`
cast outside the parentheses does not help -- by then the value has already
been truncated. Moving it inside does:

    return (mdb_table_t *) (((word_t) self->next) << 1);

which compiles to the instruction the C++ produced.

Why only these two configurations. On x32 the field is 31 bits wide, `int` can
represent every value of a 31-bit unsigned, so C's integer promotion converts it
to `int` and the shift is a full-width 32-bit one -- correct by accident.
63 bits do not fit in `int`, so only 64-bit builds are affected, and only where
the truncated value is a pointer with bit 63 set, which on x86-64 is every
kernel pointer. Four shipped configurations set `CONFIG_NEW_MDB`
(`x86-x32-p4-iofp`, `x86-x32-p4-newmdb` and the two x64 ones); the x32 pair
boots, the x64 pair faults on its first sub-table lookup.

This hazard was found once before in this migration and fixed in one place.
`api/v4/memdesc.h` already carries the cast and a comment saying why:

    NOTE the explicit (word_t) casts before the shifts: _low/_high are 54-bit
    bitfields, and C gives such an expression a 54-bit type (so << 10 discards
    the top bits) where C++ uses the declared word_t.

`generic/mapping.h` -- the old mapping database, which every other
configuration uses -- has it too, on all twelve of its
`((word_t) self->x.next_ptr << 2)` reconstructions. That is why the old MDB
works and the new one does not. The remaining three places had not been done:

  - `generic/mdb.h`, four sites: `mdb_tableent_get_table`,
    `mdb_tableent_get_node`, `mdb_node_get_table`, `mdb_node_get_next`. Fatal
    on x64.
  - `generic/mdb_mem.c`, `mm_space`: `misc.space` is 56 bits holding a
    `space_t *` shifted right by eight, so the read back lost the whole kernel
    half. Fatal on x64 as soon as anything asks a node which space it belongs
    to.
  - `generic/vrt.h`, `vrt_node_get_table_ptr`: 63 bits, same shape as
    `mdb.h`. `x86-x64-p4-iofp` is the only configuration that builds it, and it
    was already failing in `mdb_tree_map` before reaching this.
  - `api/v4/ipc.h`, `acceptor_get_rcv_window`: 60 bits shifted left by four.
    Latent rather than live -- the top four bits of an fpage a user can name are
    zero -- but the same defect, and fixing it *removes* an instruction:
    `extended_transfer` loses the `movabs $0x0fffffffffffffff; and` pair that
    the truncation had been generating.

A grep for the shape -- a cast outside a shift of a struct member -- now finds
nothing but the comment quoted above.

`x86-x64-p4-iofp` and `x86-x64-p4-newmdb` boot to the `l4test` menu. Nine of
eleven x64 configurations boot, where seven did after §135 and six before it.
The two that do not are `p4-fullkdb`, which prints the virtual-memory layout and
stops, and `p4-statictcbs`, which §135 explains cannot be given a serial
console at all. All twenty x32 configurations still compile and the four that
were spot-checked still boot.

**Corrected by §138.** `p4-statictcbs` was already booting; the counts here are
one low. Ten of eleven boot at this commit, not nine.

Against `f201f44`: `x86-x64-p4-smp`, which has neither `CONFIG_NEW_MDB` nor IO
flexpages, is 706 symbols with 704 identical bodies -- `extended_transfer` and
the constructor table that shifted when it shrank. `x86-x64-p4-newmdb` is 559
with 538, and every one of the twenty-one that moved is MDB code.


## §137 — `p4-fullkdb`: §124's `rdpmc`, and why it looked like a hang

Both `fullkdb` configurations stop after the last line of verbose init and
produce nothing further. §124 had already named the cause on x32 -- QEMU has no
`rdpmc` -- but not the mechanism, and the x64 one had not been connected to it.
It is the same fault, and the mechanism is worth writing down because it is not
a hang.

QEMU exits. `-no-reboot` turns a triple fault into a clean exit, so the harness
sees a truncated serial log and no process. `-d int` shows the whole chain:

     0: v=0e  IP=ffffffffc0610ad0  CR2=fffffffe80010018
     1: v=06  IP=ffffffffc0612ef5  SP=ffffffffc0a01d40
     2: v=06  IP=ffffffffc0617515  SP=ffffffffc0a01b90
     3: v=06  IP=ffffffffc0617515  SP=ffffffffc0a019e0
     ...
    check_exception old: 0xe new 0xe
    check_exception old: 0x8 new 0xe

One legitimate page fault in the KTCB area, then an invalid opcode, then the
same invalid opcode over and over with the stack pointer dropping 0x1b0 a time
until it reaches `0xffffffffc0a00000` -- `_start_cpu_local`, the bottom of the
CPU-local area -- where the push faults, the fault becomes a double fault and
then a triple. Both `v=06` addresses are inside `__tbuf_record_event`, at

    mov    $0xc,%ecx
    rdpmc

`CONFIG_TBUF_PERFMON` sets `tracebuffer->config.pmon` in
`tracebuffer_initialize`, and `tracerecord_store_arch` in
`arch/x86/tracebuffer.h` then reads counters 12 and 14 with `rdpmc` on every
record. The `rdpmc` is compiled in unconditionally and gated at run time on
that bit, which is why turning the option off does not remove the 150 `rdpmc`
sites from the image -- it stops them executing. QEMU's TCG raises #UD.

The recursion is the part that turns a missing instruction into a triple fault:
`exc_invalid_opcode` is itself a traced path, so the #UD handler records a
tracepoint on the way in, which executes `rdpmc`, which raises #UD. Nothing in
that loop makes progress and nothing bounds the depth.

Nothing here is a migration fault -- `tracebuffer_initialize` matches the C++
`initialize()` line for line, including all four option tests -- and on a P4
with the counters it works. So the fix is in the harness, which already edits
`config.h` to give itself a serial console: `tools/boottest` now also turns
`CONFIG_TBUF_PERFMON` off, with a notice, for the same reason and in the same
place. Only the two `fullkdb` configurations set it.

Both now boot to the `l4test` menu, which closes §124 as well. Ten of eleven x64
configurations boot; the one that does not is `p4-statictcbs`, which §135
explains cannot be given a serial console at all. On x32, nineteen of twenty
boot -- `p4-statictcbs` there is the same story.

**Corrected by §138.** `p4-statictcbs` boots on both subarchitectures and always
did; §135's account of it was wrong and this paragraph inherited it. The tally
after this section is eleven of eleven on x64 and twenty of twenty on x32 --
every shipped x86 configuration -- which §138 measured rather than inferred.

One aside worth recording, since it cost a wrong answer first time round:
`cp -r` of a configured build directory does not give you a forkable copy. The
`.depend` it copies names the *original* directory's `config.h` by absolute
path, so editing the copy's `config.h` rebuilds nothing and the stale objects
link into a kernel that appears to contradict the diagnosis. Extract the config
tar afresh instead.


## §138 — `p4-statictcbs` boots; §135 and §137 were wrong about it

`x86-x64-p4-statictcbs` boots to the `l4test` menu, and so does the x32 one.
§135 concluded it "cannot be boot-tested at all", §137 repeated it, and both
were wrong. The link failure they rested on

    ld: exception.c:(.text+0x3ea): undefined reference to `printf'
    ld: init.c:(.init.init64+0x54): undefined reference to `init_console'

was not a property of the configuration or of `tools/boottest`. It was the
scratch helper being used to build one configuration at a time. That helper
extracted the config tar and wrote a `Makeconf.local`, but did not derive a
`.config` -- and `x86-x64-p4-statictcbs.kernel.tar` is the one shipped config
that has `config.h` and no `.config`.

The two files are not interchangeable. `config.h` reaches the compiler through
`-imacros`, so the kernel's `printf()` and `init_console()` calls compile
whenever `CONFIG_KDB` is defined there. `.config` supplies the make-level
`CONFIG_*` variables, and `Mk/Makeconf` gates the entire kdb subtree on one of
them:

    ifeq "$(CONFIG_DEBUG)" "y"
    SRCSRC+= kdb/generic kdb/platform/$(PLATFORM) ...
    endif

With `.config` absent, `CONFIG_DEBUG` is empty, `kdb/generic` never contributes
`print.c` or `console.c`, and the link fails on the calls `config.h` had just
enabled. `tools/configsweep` derives a `.config` from `config.h` for exactly
this case, which is why the configuration always compiled under the sweep;
nothing else in the tree did, and the error it produces names neither cause.

`tools/boottest` now refuses to run without `config/.config` and says what to
do about it, so the next occurrence reads as a diagnosis rather than as a
missing `printf`.

Two things about that configuration are genuinely odd but harmless. Its
`config.out` sets `CONFIG_KDB_CONS_OF1275` and `CONFIG_KDB_CONS_PSIM_COM` --
PowerPC consoles -- on an `ARCH_X86` config, evidently values the configurator
carried over when the file was saved in 2010. No `Makeconf` gates on any
`CONFIG_KDB_CONS_*`, so they select nothing; the x86 console driver in
`kdb/platform/pc99/io.c` is unconditional, and `CONFIG_KDB_CONS_COM` in
`config.h` alone decides whether the serial line is used. Which is why
`boottest`'s existing edit is sufficient here, as it is everywhere else.

**The tally, measured rather than inferred: every shipped x86 configuration
compiles, links and boots to the `l4test` menu -- twenty on x32 and eleven on
x64, thirty-one of thirty-one.** §131 recorded six of eleven on x64 and §133
sixteen of nineteen on x32.

§135, §136 and §137 all counted one x64 configuration low as a result, and each
now carries a pointer here. Restated, the x64 progression is: six of eleven at
§131, eight after §135's link-base move, ten after §136's bit-field fix, eleven
after §137's `rdpmc`.

The wrong conclusion survived two commit messages and two sections of these
notes, and was only caught because someone asked for a diagnosis of the thing it
had declared undiagnosable. The shape to distrust: a property of the *tree*
inferred from a failure produced by a helper that is not part of it. Both
tools in the tree were right the whole time -- `configsweep` derived the
`.config`, `boottest` would have accepted one -- and the scratch script between
them was not.


## §139 — The powerpc build: a config broken since 2010, and clean C behind it

There is one shipped PowerPC configuration, `powerpc-bg-config.kernel.tar`
(BlueGene/P, PPC440, `CONFIG_X_CTRLXFER_MSG` and `CONFIG_X_PPC_SOFTHVM` both
on). It does not configure the tree. The build stops before compiling anything:

    src/platform/ppc44x/intctrl.h:41:3: error: #error undefined interrupt controller

`intctrl.h` chooses between `bic.h` and `uic.h` on `CONFIG_SUBPLAT_440_BGP` /
`CONFIG_SUBPLAT_440_EBONY`. The config sets `CONFIG_PLAT_440_BGP`, which no
`.cml` in the tree defines and nothing reads.

The history is exact. `af23a20`, 13 September 2010, "PPC: use SUBPLATFORM
instead of PLATFORM to distinguish between bg and ebony", renamed the symbol
across `powerpc.cml` and seven sources -- and, being a textual substitution over
the whole tree, across three binary config tarballs as well. The PowerPC tar
grew by 18 bytes, which is six occurrences of `PLAT_440` gaining `SUB`, and
`tar` cannot read past its second header afterwards. Three hours later
`c38ee12`, "Fix config tarballs (broke during string replacement over the
tree)", restored all three to their byte counts of the previous day. That
undid the corruption and the intended update together, and also fixed the
filename, which `af23a20` had left as `powercp-bg-config.kernel.tar`.

So the configuration has named a symbol nothing reads for fifteen years, a
decade before this migration started. The two x86 `statictcbs` tars were caught
by the same substitution and the same repair; theirs contained no `PLAT_440`,
so nothing was lost.

The tar is repacked here with `SUBPLAT_440_BGP` in `config.h`, `config.out`,
`.config` and the two `.old` backups -- by extracting, editing and re-creating
it, not by substituting over the archive.

Behind that, the C is in good shape. There is no PowerPC cross toolchain on
this machine, so nothing here assembles or links; what can be checked is every
C translation unit the configuration builds, with the host compiler in `-m32`
(the config is `CONFIG_IS_32BIT`, so `word_t` is the right width), the real
include and `-imacros` flags, and `-Wall -Wconversion`. **All sixty-two compile
clean.** That covers `api/v4`, `generic`, `kdb`, `arch/powerpc`,
`glue/v4-powerpc` and `platform/ppc44x`, including the SOFTHVM and ctrlxfer
paths that no x86 configuration reaches by the same route.

What that does not cover, and should not be read as covering: the inline
assembly (its register constraints are parsed but never assembled), the four
`.S` files, `tcb_layout.h` and `asmsyms.h` -- which on PowerPC only the assembly
includes, which is why the C checks without them -- and the link.

**§140 supersedes the toolchain claim above.** A `powerpc-linux-gnu` cross
compiler is installed now and the configuration links. The list in the previous
paragraph turned out to be the right list: the asm register constraints were
exactly where two out-parameters had lost their indirection, and neither the
host check nor any x86 configuration could have found them.

Two things fall out of it.

`api/v4/tcb.h` carried a comment, added in `c50dc09`, saying that declaring
`tcb_append_ctrlxfer_item` there "would clash" with the `INLINE` powerpc keeps
in `glue/v4-powerpc/tcb.h`. It would not. The glue header arrives at the
`INC_GLUE(tcb.h)` earlier in the same file, so the `static inline` definition
*precedes* the declaration, and GCC accepts that order; it rejects only
declaration-then-`static`-definition. Corrected in place. The per-architecture
split is still reasonable, but it is a choice and the comment now says so.

Forty-four `.cc` files remain under the PowerPC trees, and this configuration
builds none of them: they belong to `powerpc64`, to the Open Firmware platforms
(`ofppc`, `ofpower3`, `ofpower4`, `ofg5`), and to `platform/ppc44x/uic.cc` --
which is the Ebony arm of the very `intctrl.h` above. So of the two
subplatforms this configuration chooses between, BlueGene/P is C and Ebony is
not.


## §140 — The powerpc kernel links, and what `-Wconversion` found on the way

§139 checked every PowerPC translation unit with the host compiler in `-m32`
and reported all sixty-two clean. That was true and it was not enough. A real
`powerpc-linux-gnu` cross toolchain is installed now, and with it the
configuration compiles, assembles and links: `powerpc-kernel`, 330,172 bytes of
text and 9,768 of data, zero errors.

Getting there took nine files. Reading the warnings it then emitted took several
more, and they divide cleanly: four defects the conversion introduced beyond the
ones that had blocked the build, four that upstream has carried for a decade or
more, and three classes that look alarming and are not.

### What the host check could not see

The host check compiled C, so it caught C errors. It did not link, did not
assemble, and -- this is the part worth naming -- it did not instantiate the
PowerPC `-imacros` and `INC_ARCH`/`INC_GLUE` chain the way the real build does.
Four classes of fallout only appear under the cross build.

**Derived-to-base on a pointer.** `fdt_header_t` and `fdt_property_t` both begin
with an `fdt_node_t base`, and in C++ they derived from it, so a
`fdt_header_t *` converted to `fdt_node_t *` implicitly -- null pointer
included, which the standard requires to convert to null. In C that conversion
has to be written. `fdt_header_node()` in `platform/ppc44x/fdt.h` is it, null
check and all; it folds away, because the base is at offset zero. Nine call
sites across `fdt.c`, `bic.c`, `bluegene.h` and `kdb/platform/ppc44x/io.c`.

**`inline` that is not `INLINE`.** `ppc_get_pid` and `ppc_set_pid` in
`arch/powerpc/swtlb.h` were bare `inline` and call `ppc_get_spr`/`ppc_set_spr`,
which are `static`. C forbids a non-`static` `inline` function from referring to
an identifier with internal linkage; C++ does not. `INLINE` (which is
`static inline`) is the fix, and it is what every neighbour in the file already
used.

**`static_cast` that was never parsed.** `glue/v4-powerpc/space-swtlb.c` and
`softhvm.c` still contained `static_cast<word_t>` and `reinterpret_cast<paddr_t>`
after conversion. They compiled because their only appearances are inside
`TRACE_TLB` and `TRACE_EMUL`, which expand to nothing when tracing is off, so
the arguments are never parsed as expressions. Turning tracing on would not have
built. The same blindness hid a `RELOC` macro that had gone unused when
`space_sigma0_translate` became a loop over `transtable[]`.

**An out-parameter that lost its indirection.** `space_handle_hvm_tlb_miss` took
`paddr_t &gpaddr`; the conversion changed the parameter to `paddr_t *gpaddr` and
left all three uses spelled as the reference had them.

`generic/asid.h` also cleared one element past the end of `asid_user[]` -- a
`<=` where the bound is the array size. Upstream had it; GCC's
`-Waggressive-loop-optimizations` flags the last iteration as undefined.

### Two C++ overload sets that C dropped in silence

These are the findings that matter, and neither is visible on x86.

**`addr_offset` / `addr_mask` on a `paddr_t`.** PowerPC's `arch/powerpc/types.h`
carried, since `919fd02`, a second pair:

    INLINE paddr_t addr_offset (paddr_t addr, word_t off);
    INLINE paddr_t addr_mask   (paddr_t addr, word_t mask);

overloading the `addr_t` pair in `generic/types.h`. On `ppc44x`, `paddr_t` is
`u64_t` -- the 440 addresses 36 bits of physical memory -- while `addr_t` is a
32-bit `void *`. The conversion renamed them `paddr_offset`/`paddr_mask`,
correctly, because C has no overloading; and then recorded, in a comment, that
"no caller passes a `paddr_t` today; every site in the powerpc tree uses the
`addr_t` forms."

The second half is true and the first is false, and the gap between them is the
bug: the callers are not in the powerpc tree. `generic/linear_ptab_walker.c`
passes a `paddr_t` at eighteen sites, and with the overload gone each one
round-trips a 64-bit physical address through `void *`. **The top four bits of
every physical address are discarded** -- in `map_fpage`, where
`pgent_set_entry` receives the address a mapping is established at, and in
`space_readmem`, which the debugger reads memory through. On x86 `paddr_t` *is*
`void *`, so the same source line resolved to the same function before and
after, and nothing showed.

The fix restores the overload as an explicit choice the caller makes.
`generic/types.h` now defines `paddr_offset`/`paddr_mask` forwarding to the
`addr_t` pair, for the architectures where `paddr_t` is `addr_t`; PowerPC
defines `HAVE_ARCH_PADDR_OPS` and keeps its own, because forwarding would
truncate. The eighteen call sites name the `paddr` form.

**`virt_to_phys` / `phys_to_virt`.** `glue/v4-powerpc/hwspace.h` had these as
`template<typename T> INLINE T f(T x)` -- the return type is `T`, not `void *`.
The conversion fixed the parameter at `void *`, and the callers do not all pass
pointers: `arch/powerpc/pghash.c` and `glue/v4-powerpc/init.c` pass a `word_t`
and expect one back, and `glue/v4-powerpc/space.c` passes a `paddr_t`. The first
two became implicit integer/pointer conversions; the third truncated.
`glue/v4-x86/hwspace.h` had already solved exactly this with `__typeof__`, so
PowerPC now does the same -- plus a `+ 0`, because two callers pass arrays and
`__typeof__` of an array is an array type, which is not something you can cast
to. Binding an array to the template's by-value `T x` used to perform that decay.

Together these were the twenty-four `-Wint-conversion` warnings. There are none
now. In C++ every one of them was a hard error; in C they are, by default, a
warning that the migration had never yet been in a position to see.

### Three precedence bugs, all older than the migration

`-Wparentheses` was on the whole time and had nothing to say about x86.
PowerPC produced sixty-four instances, at six sites. Two of the six are real.

`platform/ppc44x/bic.h`:

    return val & (0xf << offset) == 0;

`==` binds tighter than `&`, so this is `val & ((0xf << offset) == 0)`. `offset`
is `(7 - (hwirq & 7)) * 4`, so zero to twenty-eight, so the shift is never zero,
so the comparison is always false, so the expression is always zero.
**`bgic_is_masked` has never once reported an interrupt as masked.** The
intended reading is the parenthesised one: masked means the four-bit target
field is zero, which is exactly what `bgic_mask_irq` writes.

`glue/v4-powerpc/space-swtlb.c`, twice -- in `space_map_device_pinned` and in
`setup_console_mapping`:

    if (vaddr & (size - 1) != 0)
        vaddr = (vaddr + size) & ~(size - 1);

Same precedence, so the test is `vaddr & 1`. The mapping is aligned up only when
`vaddr` is odd, never when it is merely misaligned for its page size, and
`ppc_tlb0_init_vaddr_size` wants it aligned.

C++ parses `&` and `==` exactly as C does, so all three are upstream, and all
three would have been found the day anyone compiled this tree with
`-Wparentheses`. The remaining three sites -- `arch/powerpc/softhvm.h`,
`arch/powerpc/softhvm.c`, and the two `fdt.h` size helpers -- parse the way
their authors meant; they have had the parentheses written out and nothing else.

Note what the shape of the `bic.h` one was before this pass. It carried a
comment, added during the conversion, reading "this reads as
`(val & (0xf << offset)) == 0` only because `==` binds tighter than `&`". That
is the precedence stated correctly and the conclusion drawn backwards: `==`
binding tighter is precisely why it does *not* read that way. A note that
records the right fact and the wrong inference is worse than no note, because it
answers the question that would otherwise get asked.

### Two asm operands that lost an indirection, and one that never had a constraint

A reference parameter that becomes a pointer has to grow a `*` at every use, and
the compiler enforces that everywhere except one place: inside an `asm` operand
list, where `"=r" (p)` and `"=r" (*p)` are both perfectly well-formed and mean
entirely different things. Two functions were converted with the operand left
alone, and in both the result is written to the local pointer and discarded, so
the caller's variable is never written at all.

`arch/powerpc/swtlb.h`'s `ppc_tlbsx` took `word_t &index`:

    : [index] "=b"(index), [found] "=&b"(found)

`space_flush_tlbent` then invalidates whatever its uninitialised `idx` held, and
`init_paging` preserves the wrong TLB entry while clearing all the others.
`-Wuninitialized` reported it at all three call sites.

`glue/v4-powerpc/softhvm.c`'s `read_hvm_instruction` took `word_t &instr`, and
its operand is the destination of the `lwz` that fetches the faulting guest
instruction:

    : [instr] "=r" (instr), [origmsr] "=&r" (origmsr), ...

so **every HVM instruction emulation and every HVM pagefault message carried a
garbage instruction word.** `-Wmaybe-uninitialized` reported this one, at the
two call sites in `handle_hvm_tlb_miss` and `handle_hvm_program`, and it is the
more consequential of the two: `ppc_softhvm_emulate_instruction` decodes that
word, and `arch_ktcb_send_hvm_pagefault` sends it to the guest monitor.

Both operands now name `*index` and `*instr`. The rest of the PowerPC tree was
swept for the same shape -- every other `asm` output in `arch/powerpc`,
`glue/v4-powerpc`, `platform/ppc44x` and the three `kdb` trees binds a local,
not a parameter.

The third is a different animal. `arch/powerpc/ppc_registers.h`'s
`ppc_get_fpscr` is upstream, unchanged by the
migration, and wrong in the same family of way: the asm reaches `value` only
through the address in a `"b"` input, declares no output and no memory clobber,
and so GCC is never told the memory is written. It reported `value` as used
uninitialised on the way out, in the FPU context-save path. `ppc_set_fpscr` has
the mirror-image omission on the read side. Both now declare the access with an
`"=m"`/`"m"` operand alongside the address.

### `-Waddress-of-packed-member`: left alone, deliberately

Two hundred and twenty-five instances, from two `__attribute__((packed))`
structs, both upstream. C is what made them visible: the C++ called member
functions on `tlb0`, and C has to write `&self->tlb0`.

Eleven of them, in `platform/ppc44x/bic.h`, are spurious. `bgic_group_t` is
packed because its layout is hardware-defined, but every member is a `word_t` at
a four-aligned offset and the trailing `__align` pads it to exactly 0x80 bytes.
Nothing in it is ever misaligned; `packed` merely drops the *declared* alignment
to one, and GCC warns on the declaration rather than on the arithmetic.

The other two hundred and fourteen are not spurious.
`sizeof(ppc_hvm_tlb_t)` is 21, measured, and it is
used as `ppc_hvm_tlb_t tlb[PPC_MAX_TLB_ENTRIES]`, so entries sit on 21-byte
strides and three out of every four `&tlb[i].tlb0` genuinely are misaligned.
The 440 core resolves unaligned integer loads and stores in hardware, so this
costs cycles rather than correctness, and the layout is load-bearing:
`ppc_hvm_tlb_ctrlxfer_get`/`_set` index the entry as a flat word array, and the
struct's own comment says so.

Dropping `packed` would make `sizeof` 24, leave the first five words where they
are, and fix the alignment -- but it would also change the size of every
`arch_ktcb_t` on the platform, and there is no way to boot a PowerPC kernel on
this machine. That is not a change to make against a build that cannot be run.
Recorded here instead.

### What is left

    1046  -Wsign-conversion
     952  -Wconversion
     225  -Waddress-of-packed-member   (above; not to be fixed)
      55  -Wint-to-pointer-cast
      37  -Wframe-address
      28  -Wunused-but-set-variable
      11  -Wpointer-to-int-cast
       9  -Wmaybe-uninitialized       (SRA temporaries; conservative)
       6  -Wcpp                        (upstream #warning directives)
       3  -Wunused-{variable,function}

Three of these were checked and are benign, which is worth recording so the
check is not repeated. The twenty-eight `-Wunused-but-set-variable` are dead
code in the C++ too, not statements dropped in conversion -- `pgent_clear`'s
`tmp`, both variants of `init_bootmem`'s `tot`, and the rest were verified
against their pre-conversion sources one by one. The nine remaining
`-Wmaybe-uninitialized` are GCC's SRA temporaries (`r_num$`, `r_pg$`, `r_fnum$`)
plus two conservative reports in the page-table walker. The six `-Wcpp` are
upstream `#warning` directives left by the original authors as to-do markers.

The two conversion classes are the bulk and are untouched: the PowerPC tree has
never been compiled with `-Wconversion`, where x86 was driven from 190 to 14
over earlier sections. Two of the fifty-five `-Wint-to-pointer-cast` are new and
are the honest form of what used to be hidden -- `space.c`'s two
`(addr_t) phys_to_virt (paddr)`, where the macro now correctly yields `paddr_t`
and the source's own cast narrows it. A 36-bit physical address does not fit a
32-bit kernel virtual address, and it is better for that to be legible.

**§141 takes up the two conversion classes.** They are triaged rather than
driven down: 1998 warnings are 243 distinct sites, and the reconstruction
against the C++ baseline says they are upstream. The exception is a guest
register taken into a signed `int` and bounds-checked on one side only.

### Verification

`powerpc-kernel` links, from a clean tree, with zero errors.

Nothing in the PowerPC tree can be run here. Three of the changed files are
shared -- `generic/types.h`, `generic/asid.h` and `generic/linear_ptab_walker.c`
-- and those are covered the usual way: `tools/configsweep 'x86-*'` builds all
thirty-one shipped x86 configurations, and `tools/boottest` boots each of them
to the `l4test` menu. **Thirty-one of thirty-one, unchanged from §138.**


## §141 — The two conversion warning classes, and why almost none of them is ours

§140 left `-Wsign-conversion` (1046) and `-Wconversion` (952) untouched and
called them "the bulk". They are, and the question this section answers is not
how to silence them but whether any of them is conversion fallout. On x86 the
same two classes went from 190 to 14 over earlier sections, and it was tempting
to read that as a target. It is not the same situation.

### Why the x86 method does not transfer

On x86 the count came down because each warning could be judged against a C++
build of the same file: `g++` and `gcc` diagnose the same implicit conversions,
so a warning that appeared only after conversion was, by construction, ours.
There is no `powerpc-linux-gnu-g++` on this machine, so that differential does
not exist here. Compiling the PowerPC tree with `-Wconversion` for the first
time produces two thousand warnings with no baseline to subtract.

So the baseline had to be reconstructed from the source. `master` at `8be66aa`
is the pre-migration C++ tree, and every converted file has a counterpart there.

### What the reconstruction says

The 1998 warnings are 243 distinct source locations -- headers are counted once
per including translation unit, and `swtlb.h` alone accounts for 602 of them
from fourteen lines. Of the 243, one is the toolchain's own `stdarg.h`. Of the
remaining 242, **117 are textually identical to the C++**, once `self->` and
`->`/`.` are normalised: the same expression, in the same file, warning for the
same reason it would have warned in 2010 had anyone turned the flag on.

The other 125 differ textually, and reading them, the difference is in every
case the mechanical shape of the conversion rather than the arithmetic:
`regs->get_register(instr.ra())` became
`except_regs_get_register (regs, ppc_instr_ra (instr))`. The operands, and
therefore the conversions, are the same.

Two audits were run over the whole PowerPC tree rather than over the warning
sites, because a warning only fires where a narrowing is *reachable*, and the
defect being looked for is a type that got narrower:

  - **Return types.** 515 C++ methods indexed from `master` and paired against
    the C functions that replaced them. Three mismatches, all artefacts of
    pairing by name across files (`read` exists in four unrelated classes).
    No drift.
  - **Parameter types.** Same pairing, comparing argument lists. Three
    mismatches: two are the `tlb_t` -> `ppc_hvm_tlb_t` nested-type rename, one
    is an unnamed parameter in a declaration. No drift.

That leaves the case §140 was actually about -- an overload set collapsing to
its narrower member, which a signature comparison passes because the surviving
signature does match one of the originals. There is exactly one such set in
play, `addr_offset(addr_t, addr_t)` beside `addr_offset(addr_t, word_t)` in
`generic/types.h`, and the wide one was already inside `#if defined(__cplusplus)`
upstream. It never existed for C. Dropping it was right, and any caller that
had depended on it would have surfaced as `-Wint-conversion`, which §140 drove
to zero.

**The conclusion is that these two classes are upstream.** They are worth
leaving alone, and worth not re-opening: narrowing a `word_t` into a four-bit
`erpn` field is what the hardware layout requires, `s16_t d()` returning an
unsigned sixteen-bit bitfield is how a D-form displacement is sign-recovered,
and `int index` on `ppc_tlb0_write` beside `word_t index` on `ppc_tlb1_write` is
an inconsistency the original authors left and the conversion faithfully kept.
Changing any of them is a behaviour change to a tree that cannot be run here.

### The one that was not benign

`ppc_softhvm_tlbre` and `ppc_softhvm_tlbwe` emulate the guest's TLB access
instructions. Both took the entry index from a guest register into an `int`:

    int idx = except_regs_get_register (regs, ppc_instr_ra (instr));
    if (idx < PPC_MAX_TLB_ENTRIES)
        ... self->tlb[idx] ...

`except_regs_get_register` returns `word_t`. As an `int`, every guest value with
bit 31 set is negative, passes `idx < 64`, and indexes `tlb[]` from before its
start. In `tlbre` that is an out-of-bounds read returned to the guest in a
register; in `tlbwe` it is an out-of-bounds *write*, at a guest-chosen negative
offset, with a guest-supplied value. `tlb[]` is a member of `ppc_softhvm_t`, so
what precedes it is other kernel state. The `is_user` check above it only
establishes that the guest is in its own supervisor mode, which under a
soft-hypervisor is exactly the caller you are defending against.

Both are `word_t` now. This is upstream -- `master`'s `softhvm.cc` has the same
`int` and the same one-sided test -- and it is the fourth of the same kind as
§140's `bgic_is_masked` and the two alignment tests: a bug that C++ compiled as
silently as C, surfaced only because `-Wsign-conversion` was finally turned on.

The fix is one instruction wide and was confirmed in the disassembly rather than
assumed, since a signed and an unsigned compare are the same size and the linked
image is byte-for-byte identical either way:

    before:  c003a508:  2c 03 00 3f    cmpwi   r3,63
    after:   c003a508:  28 03 00 3f    cmplwi  r3,63

`-Wsign-conversion` is 1046 -> 1044.

### C++ still inside the .c files

Two of the three `#if 0` blocks in the converted swtlb path had been left in
C++: `check_tlb`'s `vm->tlb[entry].vaddr_in_entry(...)` in
`glue/v4-powerpc/softhvm.c` and `dump_tlb`'s
`ppc_mmucr_read (&mmucr).get_search_id()` in `space-swtlb.c` -- the latter also
needing a statement split, because `ppc_mmucr_read` returns `void` in C and
fills its argument. Both are converted and were verified by compiling them with
the guard flipped to `#if 1`.

Looking for the rest of that pattern found that it is not confined to `#if 0`.
The PowerPC port builds exactly one configuration, and the conversion is
complete for exactly that configuration. C++ syntax survives in live blocks
that this configuration does not select:

    CONFIG_SMP                  space-swtlb.c:190-192, 489-491
                                init.c:446, 522, 528
                                tcb.h:186
    CONFIG_TRACEBUFFER          space-swtlb.c:508, 510, 513
    CONFIG_KDB_CONS_BGP_JTAG    kdb/platform/ppc44x/io.c:295, 318

None of these is a warning; each is a compile error waiting for whoever first
enables the feature. They are listed rather than fixed because none can be
compiled here to check the fix, and guessing at a rewrite that cannot be
verified is how the two asm out-parameters in §140 got their indirection lost in
the first place. The segment-MMU path (`pgent-pghash_functions.h`, `pgtab.h`,
the `CONFIG_PPC_MMU_SEGMENT` arms of `space.c` and `thread.c`) and the Ebony
board header are unconverted wholesale, which §139 already records.

### An upstream `#ifdef` that never fires

While inventorying those guards: `powerpc.cml` derives **`PPC_MMU_SEGMENTS`**,
plural, and seventeen sites test `CONFIG_PPC_MMU_SEGMENTS` correctly. Eight
sites test `CONFIG_PPC_MMU_SEGMENT`, singular, which no configuration defines:

    arch/powerpc/pgent-pghash_functions.h:47, 144, 170
    glue/v4-powerpc/config.h:160
    glue/v4-powerpc/space.c:118, 137, 145
    glue/v4-powerpc/thread.c:681

On a segment-MMU configuration -- IBM 750 or PPC604 -- those eight blocks are
silently omitted, including `space_add_mapping`'s call to `insert_4k_mapping`
and `thread.c`'s `pdir_cache` assignment. All eight are in `master` verbatim, so
this is upstream and predates the conversion by fifteen years. It is recorded
and not corrected: correcting the spelling would
enable eight blocks of never-compiled C++ in a path that has not been converted,
on hardware that cannot be built for here. It belongs to whoever revives the
segment MMU.

### Verification

Clean rebuild, zero errors, `powerpc-kernel` at 330,172 text / 9,768 data --
unchanged, as a signed-to-unsigned compare must be.

`arch/powerpc/softhvm.c`, `glue/v4-powerpc/softhvm.c` and
`glue/v4-powerpc/space-swtlb.c` are PowerPC-only; nothing shared was touched, so
the x86 gate is not implicated and stands where §140 left it at thirty-one of
thirty-one.

## §142 — The dead branches compile: the three inactive configs §141 listed

§141 inventoried C++ syntax surviving in already-converted `.c` files behind
options the one buildable PowerPC configuration does not select, and declined to
fix it: *"none can be compiled here to check the fix, and guessing at a rewrite
that cannot be verified is how the two asm out-parameters in §140 got their
indirection lost in the first place."*

That premise was wrong. `rules.cml:270` reads `unless ARCH_X86 or ARCH_POWERPC
suppress dependent SMP`, and `TRACEBUFFER` is generic — both options are
selectable on this port, and the console options are a plain either/or in
`powerpc.cml:143`. All three branches can be compiled here, by the same scratch
tree recipe §84 used for x86. What follows is that build.

### CONFIG_SMP

89 errors, in exactly the files §141 named plus one it missed
(`kdb/glue/v4-powerpc/thread.c`). Four are conversion leftovers and are fixed:

  - `space-swtlb.c` `init_swtlb[idx].tlb0.read(idx)` and `.write(idx)`, six
    calls, to `ppc_tlb0_read (&init_swtlb[idx].tlb0, idx)` and so on. Note
    `ppc_tlb1_*` takes `word_t index` where `ppc_tlb0_*` and `ppc_tlb2_*` take
    `int` -- the upstream inconsistency §141 recorded, left as it is.
  - `init.c` `cpu_start_lock.lock()` / `.unlock()`, three calls, to
    `spinlock_lock (&cpu_start_lock)` / `spinlock_unlock`.
  - `tcb.h` `get_idle_tcb()->get_cpu()`. Neither spelling survives: `get_cpu`
    is `tcb_get_cpu` and `get_idle_tcb` is `get_idle_tcb_c`, but this header is
    included from `api/v4/tcb.h:209`, ahead of both declarations (`:232` and
    `:324`), so the body reaches `__idle_tcb->cpu` directly, exactly as
    `accessors.c:244` defines the accessor.
  - `kdb/glue/v4-powerpc/thread.c`'s `INLINE u16_t dbg_get_current_cpu()`,
    deleted. In C `INLINE` is `static inline`, which collides with the `extern`
    declaration in `src/kdb/tracepoints.h`; in C++ it did not. It had no caller
    in its own translation unit, so `TP_CPU` has always bound to the shared
    definition in `kdb/api/v4/tcb.c` and removing it changes nothing. The
    sibling `dbg_get_current_tcb` had already gone the same way earlier.

**The rest is not ours, and is worse than §141 assumed.** Two errors are
structural, and `master` compiled with `powerpc-linux-gnu-g++` and
`CONFIG_SMP=y` reproduces both verbatim:

    src/glue/v4-powerpc/tcb.h: error: redefinition of 'cpuid_t get_current_cpu()'
    src/glue/v4-powerpc/space-swtlb.cc: error: 'current_cpu' was not declared in this scope

`api/v4/cpu.h:69` defines `get_current_cpu()` unguarded, `glue/v4-powerpc/tcb.h`
defines it again under `CONFIG_SMP`, and both land in one translation unit.
Which was meant is not recoverable from the source: x86 has no override at all
and relies on the `cpu.h` one, which is fed by `current_cpu = cpu` in `space.c`
precisely as `space-swtlb.c:198` does here -- so the PowerPC copy looks
vestigial. That is inference about code that has never run, so it is translated
and left in place with the reasoning written beside it, not deleted.

`current_cpu` is the one upstream defect fixed here, because it admits no
behavioural choice: it is defined in `api/v4/smp.c:39` and declared nowhere at
file scope on this port (`cpu.h` has the `extern` *inside* the function body).
`glue/v4-x86/space.h:21` already carries the identical declaration; ppc's
`space.h` now does too.

With the collision renamed away to isolate the question, **all 65 objects
compile.** What remains is the link, and it settles the matter:

    undefined reference to `tcb_lock_state_init'
    undefined reference to `migrate_interrupt_start_c'
    undefined reference to `space_switch_to_kernel_space'

None of the three is implemented for PowerPC. `space_switch_to_kernel_space` is
*declared* in `master`'s ppc `space.h` and defined nowhere; the other two do not
appear in the PowerPC tree at all, in `master` or here. The linker also reports
`.cpu` overlapping `.eh_frame`. So PowerPC SMP is not a configuration someone
switched off -- it was never finished. §141's framing of these as "a compile
error waiting for whoever first enables the feature" understated it: enabling it
is a porting job, not a build fix.

### CONFIG_TRACEBUFFER

Tested without SMP, which is the combination that can actually link. Five
sites, all conversion leftovers, all fixed:

  - `arch/powerpc/tracebuffer.h`, still holding `tracerecord_t::store_arch` and
    `tracebuffer_t::initialize` as out-of-line member definitions. Now
    `tracerecord_store_arch (self, config)` and `tracebuffer_initialize (self)`,
    matching the x86 header, which had already been collapsed. `current = 0`
    becomes `atomic_set (&self->current, 0)` -- `current` is an `atomic_t`, so
    the assignment went through `operator=`.
  - `space-swtlb.c`'s `setup_tracebuffer`: `get_kernel_space()->map_device_pinned(...)`,
    `get_kip()->memory_info.insert(memdesc_t::reserved, ...)` and
    `tracebuffer->initialize()`. The four-argument `insert` was a forwarder to
    the five-argument one with `subtype = 0` (`master`'s `kernelinterface.h:99`);
    only the latter has a C form, so the `0` is now written out, as x86's
    `init.c:291` already does.

**`powerpc-kernel` builds and links, 1,459,004 bytes.** A configuration that
did not previously compile.

### CONFIG_KDB_CONS_BGP_JTAG

`kdb/platform/ppc44x/io.c` still had `jtag_console_t` as a half-converted
`typedef struct` with three member functions inside it. De-classed the same way
`bgp_mailbox_t` above it already was, into `jtag_console_send_command`,
`jtag_console_putc` and `jtag_console_init`, with the two `cons.` call sites
updated.

Selecting JTAG *alone* then fails to link: `init_jtag()` calls `init_bgtree()`,
whose definition sits inside the `CONFIG_KDB_CONS_BGP_TREE` guard, while
`powerpc.cml:143` offers the two consoles independently. `master` has the same
structure, so this is upstream and is left alone. With both consoles selected
the kernel builds and links, 1,461,884 bytes.

### CONFIG_DYNAMIC_TCBS, and the one defect this migration created

The first three branches were the ones §141 had listed. Enumerating every
`CONFIG_*` guard appearing in a PowerPC `.c` file and testing it against the
config turned up more that are off, so the remaining selectable ones --
`DYNAMIC_TCBS` (in place of `STATIC_TCBS`), `KDB_BREAKIN` with
`KDB_BREAKIN_ESCAPE`, and `KDB_CONS_COM` -- were built together. The break-in
and serial-console branches were clean. Dynamic TCBs were not, and the first
error is **the only defect in this whole section that the conversion
introduced**:

    src/api/v4/tcb.h: error: conflicting types for 'tcb_allocate';
                             have 'tcb_t *(threadid_t)'

`master`'s `tcb_t` declares `static tcb_t *allocate(threadid_t)` (`tcb.h:238`)
and PowerPC adds a *non-static* `void allocate()` (`glue/v4-powerpc/tcb.h:189`).
Different signatures, so C++ overloads them happily. De-classing flattened both
to `tcb_allocate` and C has no overloading. x86 never defined the second one,
which is why this went unseen until a PowerPC build selected dynamic TCBs.

The PowerPC one is renamed `tcb_allocate_arch` rather than deleted, though it is
demonstrably dead: nothing in either tree calls it, and the factory does the
allocating touch itself via `kernel_stack[0]`.

The rest of that branch is un-migrated C++ in `glue/v4-powerpc/space.c`'s
`!CONFIG_STATIC_TCBS` block, and it is worth recording *how* it presented,
because only one of the four was an error:

  - `space_add_mapping` called with six arguments where it takes seven. The
    seventh is `attrib`, which `master`'s `space.h:196` gave the default
    `pgent_t::cache_standard`; C has no default arguments, so it is written out.
  - `kmem_tcb` undeclared -- the file declares three other kmem groups but not
    this one, since nothing else in it allocates TCBs.
  - `sync_kernel_space(addr)`, `add_mapping(...)` and `get_dummy_tcb()`: two
    implicit-`this` member calls and one renamed accessor, all three of which C
    accepts as **warnings** (`-Wimplicit-function-declaration`), not errors.
    They would have linked into calls to nonexistent symbols had the two real
    errors above not stopped the build first.

That last point is the standing rule restated: a sweep that greps only for
`error:` reports a clean file that is not clean. Both `error:` and `implicit
declaration` are now zero for this configuration.

With all of `DYNAMIC_TCBS`, `KDB_BREAKIN`, `KDB_BREAKIN_ESCAPE`, `KDB_CONS_COM`,
both BGP consoles and `TRACEBUFFER` selected at once, the kernel builds and
links: 1,466,952 bytes.

### Verification

The base configuration was rebuilt from scratch and compared against the §141
binary. `.data` and `.rodata` are identical; `.text` differs in **fifteen
instructions, every one of them an `li` immediate**:

    243 -> 249  322 -> 328                    (+6, glue/v4-powerpc/space.h)
     98 -> 103  107 -> 112  171 -> 176
    271 -> 276  285 -> 290  296 -> 301
    318 -> 323                                (+5, glue/v4-powerpc/space.c)
    581 -> 584  601 -> 604  626 -> 629
    641 -> 644  690 -> 693  691 -> 694        (+3, glue/v4-powerpc/space-swtlb.c)

Each is a `__LINE__` constant in an `ASSERT`, `WARNING`, `TRACEF` or `panic`,
shifted by exactly the number of lines inserted above it in its own file. The
mapping is one-to-one in all fifteen cases, and a filter for differences that
are *not* `li` immediates returns nothing. Section sizes are unchanged at
330,172 text and 9,768 data.

Every file touched is PowerPC-only, so the x86 gate is not implicated and stands
where §140 left it at thirty-one of thirty-one.

### What is left

Four options remain untested, and none is a build fix:

  - `CONFIG_PPC_MMU_SEGMENTS` and `CONFIG_PPC_SEGMENT_LOOP` reach the segment
    MMU path, which §139 records as unconverted wholesale --
    `pgent-pghash_functions.h` alone still holds some twenty `pgent_t::` member
    definitions. `CONFIG_PPC_MMU_SEGMENT`, singular, is the misspelling §141
    found that no configuration defines.
  - `CONFIG_PLAT_OFPPC` is a different platform entirely.
  - `CONFIG_COMPORT`, tested as `#if CONFIG_COMPORT == 0` in
    `kdb/platform/ppc44x/io.c`, is defined nowhere -- the file defines
    `CONFIG_KDB_COMPORT`. The undefined identifier evaluates to `0`, so the
    comparison is true and the FDT branch is taken; the `== 1` arm holds only an
    `UNIMPLEMENTED()`. It works by accident, and is a second instance of the
    same misspelling class as `PPC_MMU_SEGMENT`.

So the honest statement is narrower than "no C++ remains": **every PowerPC
configuration that can be selected on this platform now compiles and links,
warning-clean and with no implicit declarations.** What is still C++ sits behind
the segment MMU and the OFPPC platform, both of which are unconverted by
design and recorded as such.

The two structural findings, neither of them the migration's: PowerPC SMP is
unimplemented upstream -- three entry points have no definition anywhere -- and
the JTAG console cannot be selected without the tree console. Both belong to
whoever revives the port.


## §143 — Ebony: the subplatform §142 did not look at

§142 closed with *"every PowerPC configuration that can be selected on this
platform now compiles and links"*, and listed four options it had not tested,
none of which it thought was a build fix. The list was drawn from the option
symbols under the one subplatform it had been building. It did not look one
level up, at the subplatform choice itself.

`powerpc.cml:163` is a two-way `choices` block:

    choices powerpc_subplatform
	    SUBPLAT_440_BGP
	    SUBPLAT_440_EBONY
	    default SUBPLAT_440_BGP

`SUBPLAT_440_EBONY` is the AMCC Ebony evaluation board, and it is selectable
here -- same architecture, same CPU, same platform, same toolchain. Configured
and built, it produces **45 errors**, every one of them from compiling
`platform/ppc44x/uic.cc` as C++ against a tree that is no longer C++:

    src/generic/types.h:47:9: error: '_Bool' does not name a type
    src/platform/ppc44x/uic.h:142:1: error: expected class-name before '{' token
    uic.cc:51: 'fdt_t' has no member named 'find_subtree'
    uic.cc:249: 'spinlock_t' has no member named 'lock'

So the closing claim was one subplatform too broad. `Makeconf` selects `bic.c`
under BGP and `uic.cc` under Ebony; the conversion swept everything the BGP
branch reaches and stopped at the `ifeq` it did not take.

### What was converted

Two files, plus the `Makeconf` line naming the first.

**`uic.h`.** `class intctrl_t : public generic_intctrl_t` becomes a plain
`struct`, and the members become the same free `intctrl_*` entry points `bic.h`
already exposes -- deliberately name-for-name, since `platform/ppc44x/intctrl.h`
picks between the two headers by subplatform and everything upstream of that
choice calls one contract. The two upstream oddities are preserved rather than
tidied: `is_enabled` returns `is_masked` and not its negation (`bic.h` has the
same inversion, and nothing in the tree calls either), and `get_number_irqs`
returns the `INT_LEVEL_MAX + 1` constant rather than the `num_irqs` member,
which on this controller is never written.

**`ebony.h`**, which the build reached second, from `glue/v4-powerpc/init.c` by
way of `platform.h`. It needed no thought: its body is byte-identical to
`bluegene.h`'s before conversion -- same three functions, differing only in
copyright block and include guard -- so the conversion `bluegene.h` already
received was extracted and applied verbatim.

The result is 0 errors and 0 implicit declarations from a scratch build, and it
links: 132,704 text, 7,936 data, 892,456 bytes of kernel. The 38 warnings from
the two files are all `-Wconversion` and `-Wsign-conversion` on `1 << (31 - irq)`
feeding a `word_t` -- the §141 class, upstream, left alone.

### The C++ baseline §141 said did not exist

§141 reconstructed its baseline by reading source, because *"there is no
`powerpc-linux-gnu-g++` on this machine, so that differential does not exist
here."* That is wrong -- it is at `/usr/bin/powerpc-linux-gnu-g++`, and §142
itself used it two sections later to reproduce the SMP collision on `master`
without noting the contradiction. §141's conclusions do not depend on it, but
the differential is available, and this section is the first to use it on a
whole object.

`master`'s C++ `uic.o` builds. Against the converted one, per function:

    init_cpu  map  start_new_cpu  is_masked  is_pending  dump
	instruction-for-instruction identical

    handle_irq  mask  unmask
	differ only in how the receiver arrives -- C++ takes `intctrl' in r3
	(one `mr'), C loads its address (`lis'/`addi')

    send_ipi        C++ 41 insns (send_ipi + raise_irq), C 39: C++ less the
		    tail-call `b' and the receiver `mr'.  `raise_irq' is
		    `static' here and inlines into its only caller.
    init_arch       C++ 365 (init_arch + init_controllers), C 358, same reason

The hardware contract is the DCR traffic, and it is exact: **62 accesses in
both, identical opcodes, identical DCR numbers, identical order.** Three
instructions differ in register allocation and nothing else.

Two differences are real and worth recording:

  - Fourteen DCR writes move from `.text` to `.init`. Upstream marks
    `init_arch` and `init_cpu` `SECTION(".init")` but not `init_controllers`,
    so the whole controller-reset sequence sat in resident text; making it
    `static` inlines it into its only caller, which is in `.init`.
  - `.bss` shrinks 136 -> 132. `spinlock_t` is an empty struct without
    `CONFIG_SMP`; C sizes it 0 where C++ sized it 1, padded to 4. This is
    tree-wide, not local to `uic` -- every struct holding a lock is four bytes
    smaller on a uniprocessor build.

The second of those turned up an incorrect comment, added by e365a84, claiming
`generic/sync.h`'s uniprocessor `spinlock_t` *"declared none at all"* of the
lock operations upstream. `master` declares all four (`lock`, `unlock`, `init`,
`is_locked`) as empty class members; only `init` gained an argument in
conversion. The comment is corrected in place.

Rebuilding BGP from scratch confirms it is untouched: `.text`, `.data`,
`.rodata` and every other section byte-identical to the §142 binary, with four
bytes differing in `.kip` -- the build timestamp string.

### The arm that cannot be built at all

`uic.c` carries `#if defined(PPC440EPx)` blocks for a third interrupt
controller. Nothing in the tree defines `PPC440EPx`, and defining it does not
help: `UIC2_DCR_BASE`, from which `uic.h` derives all nine UIC2 registers, is
defined nowhere -- `uic.h:53` carries a `FIXME` saying exactly that. The arm is
unbuildable by construction, in either language.

It is converted anyway, for consistency, and it carries four upstream defects
that had never been diagnosed because nothing ever parsed them:

    uic.cc:139  printf(...)  -- no semicolon
    uic.cc:148  panic(...)   -- no semicolon
    uic.cc:301  uic2_dchain_Mask -- no such member; the field is _mask
    uic.cc:399, 604, 632  `} else if (...)' with no opening brace, so the
			  closing brace three lines down closes nothing

All four are corrected, marked where they occur, and both halves of the claim
are checked by supplying a stand-in base address on the command line:
`master`'s C++ file produces 11 errors at exactly those four sites; the
converted C file compiles clean. Without the stand-in, the only error from
either is `UIC2_DCR_BASE undeclared`.

### What is left, stated more carefully than §142 stated it

Both ppc44x subplatforms now build. What remains unconverted is one
configuration, not four options: **`CONFIG_PLAT_OFPPC` with the segment MMU.**
§142 listed `PPC_MMU_SEGMENTS` and `PLAT_OFPPC` as separate items and described
the latter as "a different platform entirely", which reads as though neither
were reachable. They are the same item and it configures:

    make batchconfig CMLBATCH_PARAMS="ARCH_POWERPC=y CPU_POWERPC_IBM750=y PLAT_OFPPC=y"
	CONFIG_PLAT_OFPPC=y
	CONFIG_PPC_MMU_SEGMENTS=y

(That line is the minimum that builds.  To *run l4test* add `X_PAGER_EXREGS=y`
-- see §159; without it the inter-space abort tests hang.)

`powerpc.cml:179` derives `PPC_MMU_SEGMENTS` from the 750 or the 604, and
`:169` suppresses `PLAT_OFPPC` unless it is set -- so choosing the CPU is what
opens the platform, and the segment MMU comes with it. Behind it sit
`glue/v4-powerpc/pghash.cc` and `space-pghash.cc`, the twenty-odd `pgent_t::`
member definitions in `pgent-pghash_functions.h`, and eight more `.cc` files
under `platform/ofppc` and `kdb/platform/ofppc`. That is a section's worth of
work, not a loose end, and it is the last of it on this architecture.


## §144 — OFPPC: one commit in 2010, and the eleven ways it broke a platform

§143 closed by naming `CONFIG_PLAT_OFPPC` with the segment MMU as the last
unconverted configuration on this architecture, and estimated it at a section's
worth of work. That was right about the size and wrong about the shape. The
C++ was the smaller half.

`powerpc.cml:155` offers two platforms and OFPPC is the **default**:

    choices powerpc_platform
	    PLAT_OFPPC
	    PLAT_PPC44X
	    default PLAT_OFPPC

So the platform a bare `ARCH_POWERPC=y` selects is the one that has not built
since 2010.

### The blocker, and what it points at

Configured for OFPPC the tree stops before compiling anything, on an assertion
in its own headers:

    src/glue/v4-powerpc/config.h:98: error: "The page hash area overlaps the cpu data area."

It is not a stale check. The arithmetic:

    DEVICE   0xD0000000 - 0xD2000000
    PINNED   0xD2000000 - 0xD3000000
    CONSOLE  0xD3000000 - 0xD4000000
    PGHASH   0xD4000000 - 0xD6000000	(32M, 32M-aligned)
    CPU_AREA 0xD4000000 + 128K		(KERNEL_CPU_OFFSET)

`PGHASH_AREA_START` is `CONSOLE_AREA_END`, and `CONSOLE_AREA_END` is where the
cpu area begins. Before **d52a5e2**, "Added support for PPC440 processors", the
page hash sat at `DEVICE_AREA_END` and its 32MB ended exactly at
`KERNEL_CPU_OFFSET` -- the check passed with nothing to spare. That commit
inserted the 16MB pinned and 16MB console areas into the span the hash occupied
and repointed `PGHASH_AREA_START` at `CONSOLE_AREA_END`. PPC440 selects
`CONFIG_PPC_MMU_TLB`, so the whole `#ifdef CONFIG_PPC_MMU_SEGMENTS` block the
assertion lives in was never compiled by its author.

Every one of the ten further defects below is from the same commit or the same
neglect, and `master` reproduces all of them.

### Eleven things wrong, none of them the migration's

  1. **The layout.** Above. Nothing can go back where it was -- there is no
     32MB-aligned 32MB slot left below `0xD4000000` -- and shrinking
     `PGHASH_AREA_SIZE` would cap the largest hash `pghash_init` may pick at
     run time. `KERNEL_CPU_OFFSET` moves to `0xD6000000` on segment-MMU builds
     only, which is forced: 128KB-aligned for its BAT, clear of the hash below
     and of `KTCB_AREA_START` above, in the 160MB that was already empty.
     ppc44x keeps `0xD4000000`.
  2. **Eight `#ifdef CONFIG_PPC_MMU_SEGMENT`** -- singular, defined by no
     `.cml`, read by nothing. §141 found this misspelling class in one place
     and §142 in a second; this is the third and by far the worst. It switches
     off `insert_4k_mapping`, `flush_4k_mapping`, the reference-bit sync, and
     the whole `pdir_cache`/`sync_kernel_space`/`handle_hash_miss` sequence in
     thread switch. With it misspelt **nothing is ever put into the hardware
     page hash and nothing is ever taken out**. The port could not have worked
     even if it had compiled.
  3. `current->pdir_cache` in `switch_to`, where no `current` is in scope.
  4. `ppc_esr_read`, `ppc_tcr_read`/`write`, `ppc_tsr_write` outside the
     `CONFIG_PPC_BOOKE` block defining the SPRs they name; `space.c` and
     `init.c` include `swtlb.h`, which reaches the same SPRs, unconditionally.
  5. `install_exception_handlers` called ~200 lines before its `static`
     definition with no declaration on this branch.
  6. `_except_extern_int` names both the vector slot and the `.init` template
     copied into it -- `except.S` does not assemble on any non-BookE build.
  7. `get_of1275_tree` called from ten 32-bit files, declared only in
     `arch/powerpc64/1275tree.h`.
  8. No `platform/ofppc/platform.h`, though `init.c` includes
     `INC_PLAT(platform.h)` unconditionally -- and the same commit replaced
     init's `ofppc_get_cpu_speed`/`ofppc_get_cpu_count` calls with the neutral
     pair, orphaning both.
  9. `startup.S` branches to `l4_powerpc_init`, defined nowhere; the entry
     point is `startup_system`.
 10. `linker.lds` includes neither `ctors.ldi` nor `mdb.ldi` -- the §132 class.
 11. `space-pghash.cc` is in `SOURCES` with **no includes at all**;
     `tcb_resources_enable_copy_area` calls a method on a type that does not
     exist; `opic_in32be` assembles a form `lwz` does not have;
     `TRACEPOINT(hash_miss_cnt)` passes one argument to a macro taking two.

One is ours and worth recording as such: **`asid.h`**. The C++ was
`template <class T, int SIZE> class asid_manager_t`, so nothing existed until
something named the instantiation. Spelling the single instantiation out made
an array unconditionally sized by `CONFIG_MAX_NUM_ASIDS`, which
`glue/v4-powerpc/config.h` defines only on the `CONFIG_PPC_MMU_TLB` branch. A
template's laziness was doing work the conversion did not notice it was
relying on. §141's audits compared return types and parameter lists and would
not have caught it; the defect is at the instantiation point, not the
signature.

### The conversion

Eleven files, ~2,550 lines. `pgent-pghash.h` and its functions header;
`pghash.cc` and `space-pghash.cc`; `1275tree`, `intctrl`, `opic` and `ofppc` on
the platform side; `io`, `of1275`, `ofppc`, `opic` and `reset` in the kdb.

Two things are worth naming. `of1275_device_t::get_prop` was **three
overloads** -- by name, by index, and a word-sized wrapper -- and all three keep
distinct C names; collapsing an overload set to its narrowest member is exactly
what §140 spent a section chasing. And `opic.h` turned out to be a byte-for-byte
duplicate of `intctrl.h` **sharing its include guard**, so whichever a
translation unit reached first expanded and the other became a no-op; keeping
the two in step was never checked by anything, and `opic.h` now names the other.

Where upstream's intent could not be recovered it is not guessed:
`tcb_resources_enable_copy_area` casts to a `ppc_resource_bits_t` that exists
nowhere, and is left explicitly unimplemented with the original body kept in a
comment.

### Verification

    ofppc     0 errors, 0 implicit declarations, links, 231,060 bytes
    ebony     0 errors, 0 implicit declarations, links, 892,504 bytes
    bgp       0 errors, 0 implicit declarations, links, 1,467,032 bytes

The blocks that no configuration selects are compiled anyway, the way §142
compiled its dead branches: `CONFIG_PPC_MMU_SEGMENT` and
`CONFIG_KDB_CONS_OF1275` defined on the command line, zero errors in each of
`space.c`, `thread.c`, `pghash.c`, `of1275.c`, `ofppc.c` and `reset.c`.

**ppc44x is unchanged.** Rebuilt from scratch against the §142 binary the
instruction stream is identical -- same count, same mnemonics, same order, no
function differing in length -- and fifteen bytes differ, all of them `li`
immediates carrying `__LINE__`. That is why `dtree_remap` is split per platform
with upstream's ppc44x text left alone rather than merged behind a shared
accessor: the merged version was correct and cost four instructions in the one
PowerPC kernel that runs.

`x86-x64-p4` was rebuilt because two *generic* headers were touched
(`asid.h`, `memregion.h`): `.text`, `.data` and `.rodata` byte-identical, 28,457
instructions unchanged.

### What this is not

It builds and links. **It has not been booted** -- there is no ofppc hardware
and no emulator here, and the layout change in item 1 is therefore verified
only by the two assertions it was made to satisfy. Item 2 is the more serious
caveat: with the guard left misspelt, as it is, a segment-MMU kernel still
never touches its page hash. Correcting that spelling is a behavioural change
to code that has never executed, and belongs to whoever brings the port up on
real hardware -- together with the note that the corrected code now compiles,
which is the part that could be settled here.

With this, **every configuration selectable on the 32-bit PowerPC architecture
compiles and links**: both platforms, both subplatforms. What remains C++ in
the tree is powerpc64 (21 files, no toolchain here), the three OF platforms
that belong to it, and the five dead files §143 listed.


## §145 — The guards, corrected: what a typo had switched off

§144 found eight `#ifdef CONFIG_PPC_MMU_SEGMENT`, singular, defined by no
`.cml`, and left the spelling alone: correcting it changes behaviour on a port
that cannot be run here, so it belonged to whoever brings the port up. That was
the right default and it has now been overridden deliberately. This is the
change and what it measures.

### What the misspelling was worth

The seven `#ifdef`s (the eighth site was the comment describing them) are now
`CONFIG_PPC_MMU_SEGMENTS`. Building the same configuration immediately before
and after:

    pghash_flush_4k_mapping     0 -> 5 references
    pghash_insert_4k_mapping    1 -> 2
    space_handle_hash_miss      3 -> 5
    space_sync_kernel_space     3 -> 4
    .text                  0xe0b1 -> 0xe3bd	(+780 bytes, +196 in .kdb)

**Zero to five on the flush is the line that matters.** A page-hash MMU whose
page hash is never flushed does not fail visibly at boot; it fails the first
time a mapping is revoked and the stale translation is still live in the hash.
Nothing in the port removed a translation, and nothing put one in from
`space_add_mapping` either -- the single reference before the change was
`space_handle_hash_miss`, filling the hash on a miss, which is the one path
that never went through a guard.

`pghash_insert_4k_mapping` reads 1 -> 2 rather than 1 -> 2 `bl` sites because
`space_add_mapping` ends in the call and GCC tail-calls it: the new reference
is a `b`, not a `bl`. Counting only `bl` said 1 -> 1 and looked like the guard
had not taken effect. It had.

### Two of the seven change nothing, and it is worth knowing which

`config.h`'s guard selects `KIP_ARCH_PAGEINFO`, and correcting it leaves
`kernelinterface.o` **byte-identical**. Compiling it both ways and diffing
`.data` shows no difference at all. The reason is the redefinition §144 noted
in passing: the `#else` arm defines `HW_VALID_PGSIZES` as
`((1 << 12) | (1 << 22))`, and `pgent-pghash.h` redefines it to `(1 << 12)`
before `KIP_ARCH_PAGEINFO` is expanded at `kernelinterface.c:134`, so
`size_mask` was already 4. One arm was quietly overwriting the other.

What correcting it does fix is the noise that overwriting produced: **41
"HW_VALID_PGSIZES redefined" warnings, now zero.** A macro redefined between
its definition and its use is exactly the shape that hides a wrong value, and
here it hid the fact that the wrong arm was selected at all.

The reference-bit sync in `pgent-pghash_functions.h` is the other one: it is
now compiled, but `pgent_reference_bits` has no caller in this configuration,
so nothing reaches it. `ppc_htab_locate_pte` stays at two references for that
reason.

### Regressions

None. Neither spelling was ever defined on a `CONFIG_PPC_MMU_TLB` build, so
both arms already took the `#else` there. Rebuilt from scratch, ppc44x still
differs from the §142 binary in fifteen `li` immediates carrying `__LINE__` and
nothing else, and ebony is unchanged.

    ofppc   0 errors, 0 implicit, 0 redefinitions, 231,104 bytes
    ebony   0 errors, 0 implicit, 0 redefinitions, 892,504 bytes
    bgp     0 errors, 0 implicit, 0 redefinitions, 1,467,032 bytes

### What this does and does not settle

It is still not booted -- there is no ofppc hardware and no emulator here. But
the caveat §144 closed on has changed shape. It used to be "the port compiles
and its MMU is switched off"; it is now "the port compiles and its MMU is
wired up, unverified." The second is a much better position to hand over, and
the difference between them was seven characters.

The layout change of §144 item 1 remains verified only by the two assertions it
was made to satisfy, and that is now the last untested thing on this
architecture.


## §146 — OFPPC boots: what running it found that reading it had not

§144 converted the platform and closed on "verified to build and link, not to
boot: there is no ofppc hardware or emulator here." That was wrong, and cheaply
so. `qemu-system-ppc -M mac99 -cpu g3` is a Uninorth/KeyLargo PowerMac with an
OpenBIOS Open Firmware and a `PowerPC,750` — an IEEE 1275 machine with an MPIC,
which is the machine this platform is written for. The OPIC driver's own
comment names a KeyLargo MPIC2 as its test bed.

It boots.

    [==== Pistachio PowerPC Open Firmware Boot Loader ====]
    [ L4 PowerPC ]
    Activated the Open Firmware console.
    L4Ka::Pistachio - built on Jul 31 2026 ...
    virtual memory layout:
        user area            0 - c0000000
        copy area     f0000000 - ffffffff
        kernel area   c0000000 - d0000000
    Initializing kernel memory (c0042000-c00c2000) [512K]
    Initializing kernel space
    Initializing TCBs
    Initializing boot CPU
    Initializing kernel debugger
    Activated page hash at virtual address 0xd4000000,
        physical address 0x100000, size 0x80000.
    Initializing mapping database
        Initializing threading (CPU 0)
        Switching to idle thread (CPU 0)
    Assertion tcb == get_sprg_tcb() failed  (thread.c:49)

**The page hash line is the one worth reading twice.** It activates at
`0xd4000000`, which is where §144 left it after moving `KERNEL_CPU_OFFSET` up to
`0xd6000000` to clear the overlap that had blocked the build since 2010 — a
change §144 could only justify by the two assertions it was made to satisfy.
And it activates at all only because §145 corrected the misspelt guards. Two
changes argued from static evidence, both doing what they were argued to do.

### The verification method that was not one

§144 said `CONFIG_KDB_CONS_OF1275`'s blocks were checked "by compiling each file
with the option defined on the command line". They were not, and could not have
been. `config.h` is force-included with `-imacros` and contains

    #undef  CONFIG_KDB_CONS_OF1275

which overrides a `-D` on the command line. Every such check compiled the
blocks *disabled* and reported zero errors for it. Two files — `io.c` and
`of1275.c` — still held C++ inside them, and a configuration that actually
selects the option found it immediately.

This does not touch §145's guard checks: `CONFIG_PPC_MMU_SEGMENT` is a
misspelling no `.cml` knows, so `config.h` never mentions it and `-D` was real
there. The rule to carry forward is narrower than "compile with `-D`": that is
only a valid check for identifiers the configuration system has never heard of.
For anything else, configure it.

### Two bugs that cost the first boot

**`.lcomm` does not advance the location counter.** `startup.S` wrote

    _init_stack_bottom:
    .lcomm  init_stack, INIT_STACK_SIZE, 16
    _init_stack_top:

`.lcomm` declares a local common symbol; it emits nothing where it appears. So
both labels landed on the same address and the init stack had zero size.
`_start` never noticed, because it computes its stack pointer from `init_stack`
plus the size directly. `ofppc_stack_top()` returns the collapsed symbol, and
the Open Firmware console runs OF on a stack derived from it —
`execute_of1275` takes `stack_top - 16` and zeroes four words there before
every call. With top == bottom == `_start_bss + 16`, that address is `kmem`.

So **every `printf` through the OF console zeroed the kernel's memory
allocator.** The symptom was that the free list read back empty at the first
`kmem_alloc`, in `space_init_kernel_space`, having been written correctly three
times. Raising the bootmem reservation from 512K to the 3584K the ppc44x
variant uses changed nothing, which is what ruled under-provisioning out; a
probe that wrote `0xdeadbeef` to the address, read it back intact, printed one
line and read it back zero is what ruled it in.

**A computed value thrown away.** Fixing the labels made things worse: OF's
stack moved from `kmem` onto the kernel's own. `execute_of1275` computes `sp`,
null when already running on the init stack — `kdb_switch_space` documents null
as "reuse the current stack" — and then passes `of1275_stack_top-16` anyway,
ignoring what it computed. It now passes `sp`.

### Two on the user side

`include/l4/powerpc/arch.h` declares sixteen flexible array members inside
unions, which `g++` rejects, so no C++ user program has ever compiled for this
architecture — `l4test` and `pingpong` both fail on it. The file already spells
one of them `raw[0]` at line 383; the other sixteen now match.

`piggybacker/ofppc/main.cc` called `update_kip(of1275_entry)` against a nullary
`update_kip`. Dropping the argument is not the fix: `io.c` reads
`get_kip()->boot_info` as the OF client-interface entry, and `update_kip`
copies a `boot_info` field initialised to zero whose setter nothing calls. The
argument belonged one line earlier, on `set_boot_info`.

### One thing that was not the problem

Open Firmware's `/memory` `available` property lists `0x4000-0x4000000` as
free, and the loader never claims any of it — `prom_claim`'s only caller is
`prom_map`, for devices. That looked like the answer, since "available" means
free *to claim*, not that OF will leave it alone. Claiming the three module
ranges succeeds, and changes nothing: the clobber was ours. The experiment is
recorded and not kept, because adding unverified behaviour to fix a problem
that turned out to be elsewhere is how the next section gets written.

### Where it stops

`notify_trampoline`, on `ASSERT(tcb == get_sprg_tcb())` — the TCB derived from
the stack pointer does not match the one in SPRG at the first thread switch.
That is a separate never-run defect and is not fixed here.

The honest position is now three steps better than §144's. It was "compiles and
links". It became "compiles, links, and its MMU is wired up". It is now
**boots, brings up its address space, activates its page hash, builds its
mapping database, and reaches the idle thread.** What remains is one assertion
in the thread switch, and a machine to reproduce it on.


## §147 — The assertion was right: a line the conversion did not carry over

§146 left ofppc stopping on

    Assertion tcb == get_sprg_tcb() failed  (thread.c:49)

at the first instruction the idle thread executes. Printing both sides settles
it in one run:

    NT: frame-derived tcb d6000800, sprg tcb ffffff70, stack d6000f68

The frame-derived side is right — `d6000800` is `__whole_idle_tcb`, 2048-aligned
as `KTCB_BITS` requires, and the stack is inside it. `SPRG_CURRENT_TCB` holds
`0xffffff70`, which is what SPRG1 contained at reset. Nothing had written it.

`mtsprg 1` appears **once** in the whole kernel, inside `tcb_switch_to`. The
initial switch never sets it.

### Not a conversion defect — a rewrite

`glue/v4-powerpc/tcb.h` has always carried `initial_switch_to`, and its first
line is the one that matters:

    INLINE void NORETURN initial_switch_to( tcb_t *tcb )
    {
	// Store the target thread's tcb in the appropriate sprg.
	set_sprg_tcb( tcb );
	asm volatile ( "mtctr %0 ; mr %%r1, %1 ; bctr ;" : :
		       "r" (get_kthread_ip(tcb)), "b" (tcb->stack) );

The migration did not convert this. It wrote a **new** `initial_switch_to_c` in
`thread.c`, from scratch, and that function got the difficult half right and
dropped the easy one: reading the resume address from `0(%r1)` is precisely
`get_kthread_ip(tcb)`, because `tswitch_frame_t` puts `ip` first. The
`set_sprg_tcb` line simply is not there.

This is worth separating from every other defect in §144 to §146. Those were
upstream's, and the audits in §141 were built to catch the conversion's own:
return types, parameter lists, collapsed overloads. **None of them can see
this one**, because there is no C++ counterpart to compare `initial_switch_to_c`
against. It is a hand-written function whose only obligation was to match a
sibling nobody diffed it against. The fix is to delete it: the wrapper now
calls the arch inline, so the two cannot drift again.

`asid.h` in §144 was the same shape of mistake -- a construct rewritten rather
than converted, losing a property the original had for free. Two in the whole
migration, both found only by running the result.

### It is not an ofppc bug

`thread.c` is shared. **Every PowerPC configuration has been making its first
thread switch with a stale SPRG**, including the shipped ppc44x kernel, which
now goes from one `mtsprg 1` to two. It survived because ppc44x has only ever
been verified to build and link -- §140 through §142 never ran it -- and because
the assertion that catches it is compiled out of a non-debug build, so the
damage would have been a wrong `get_sprg_tcb()` somewhere later rather than a
clean stop.

That is the argument for booting things, made better than any of the previous
six sections made it: three sections of static reasoning about ofppc found
eleven upstream defects and missed a live bug in the port that was supposed to
be working.

### Where it reaches now

    Switching to idle thread (CPU 0)
    Remapping device tree from 0061e000 to d0040000 (sz=4000)
	Registering processor 0 in KIP (1MHz, 1MHz)
    The open-pic device: /pci@f2000000/mac-io@c/interrupt-controller@40000
    Found an open-pic at 0x80040000, size 0x40000.
    Open-Pic version 2, supports 1 cpu's and 64 interrupt sources
    Open-Pic timer freq 4.160000 MHz
    Found cpu: /cpus/PowerPC,750@0
    Detected 1 processors
    System has 68 hardware interrupts

The interrupt controller is the OPIC driver §144 converted, talking to the
MPIC in QEMU's KeyLargo, reporting a version, a source count and a timer
frequency it read out of the hardware. Kernel initialisation is complete.

It stops inside `init_all_threads`, after `init_interrupt_threads` and before
or during `init_root_servers`, with no diagnostic -- so sigma0 and the root
task do not start. Two smaller faults are visible above and untouched: the cpu
and bus speeds are not found in the device tree, so the decrementer runs off a
1MHz fallback.

The position, once more: **boots, initialises its address space, activates its
page hash, builds its mapping database, reaches and runs the idle thread, and
brings up its interrupt controller.** What is left is starting the root
servers.


## §148 — init_root_servers was never reached: vectors overwritten after the source was freed

§147 left the boot stopping inside `init_all_threads`, with no diagnostic,
between `init_interrupt_threads` and `init_root_servers`. Bisecting it with
prints puts it in `init_kernel_threads`, on the first touch of the TCB area:

    IKT: tcb addr e0022000
    IKT: about to read e0022000
    <nothing>

`0xe0022000` is in the KTCB area, which is mapped on demand: the read must
fault, and `space_handle_pagefault` routes a TCB-area fault to
`space_allocate_tcb`. A counter in the DSI handler never fired -- **no
exception was taken at all**. Reading the vector directly explains why:

    VEC@0x300: 0 0 0 0

### Two meanings for one function name

`install_exception_handlers` is called from two places, neither guarded, and it
does something different in each MMU variant:

  - **BookE** (`except_handlers.c`): writes `SPR_IVOR(0..15)` and `SPR_IVPR`.
    These are *per-processor* registers, so the call from `cpu_init` is exactly
    right and every processor must make it.
  - **Segment MMU** (`init.c`): a one-time `memcpy_cache_flush` of the vector
    code from `_start_except` down to `PHYS_EXCEPT_START`. It returns
    immediately unless `cpu == 0`.

`startup_system` calls it once, before `init_bootmem`. `init_bootmem` then does
this:

    // Claim the memory used by the exception vector code.
    size = (word_t)memcfg_start_kernel() - phys_to_virt(PHYS_START_AVAIL);
    if( size ) kmem_add(&kmem, (addr_t)phys_to_virt(PHYS_START_AVAIL), size);

-- handing the `.except` source region to the kernel allocator, which is only
sound *because the copy has already happened*. The comment says so.

Then `cpu_init(0)` runs, and calls it again. By then the source has been handed
out and written over. Instrumenting both runs shows it exactly:

    IEH: right after copy: +0=7d9343a6 +0x200=7d9343a6
    IEH: right after copy: +0=0        +0x200=0

`7d9343a6` is `mtsprg 3,r12`, the first instruction of `EXCEPT_STACK`. The
second call copied zeroes over it. From that point the machine had no exception
vectors, and the first page fault branched into an empty page — which is why
there was no diagnostic to see. `master` has the same unguarded pair of calls.

Guarding the `cpu_init` call to `CONFIG_PPC_MMU_TLB` fixes it and leaves BookE
alone: ppc44x keeps both call sites, and its instruction stream is unchanged
byte for byte against the same tree without the commit.

### It boots

    System has 68 hardware interrupts
    Initializing root servers
    root-servers: utcb_area: bf000110 (128KB), kip_area: bff000c0 (4KB)
    Creating sigma0 (SIGMA0)
    Creating root server (ROOTTASK)
	Idle thread started on CPU 0

`init_root_servers` completes. sigma0 and the root task are created, and the
idle thread runs. **`Idle thread started` is the string `tools/boottest`
already greps for as a pass on x86**, so by the harness's own criterion this
platform now boots.

### What the last five sections cost, and what they were worth

§143 through §148 began from "the segment MMU and OFPPC are unconverted by
design" and end with the platform booting. Counting the defects: **fourteen
upstream**, of which one commit (`d52a5e2`, 2010) accounts for eleven, and
**three the migration's own** -- `asid.h`'s template instantiation (§144),
`initial_switch_to_c`'s dropped `set_sprg_tcb` (§147), and the retracted
verification method in §146. Two of those three were invisible to every audit
§141 designed, because both were *rewrites* rather than conversions, with no
C++ counterpart to diff against.

Of the eleven found by reading, none was wrong. Of the four found by running,
none could have been found by reading: a `.lcomm` that does not advance the
location counter, a computed stack pointer thrown away, a missing SPRG store,
and a second call to an idempotent-looking function that is not idempotent.
That is the case for booting things, and it is the last thing §142 said it
could not do.

What is left: the cpu and bus speeds are not found in the device tree, so the
decrementer runs off a 1MHz fallback; and nothing yet confirms sigma0 and the
root task run past creation, because the userland has no console on this
platform.


## §149 — The cpu node is not called what the code thinks it is

§148 left the decrementer running off a 1MHz fallback:

    Error: unable to obtain the cpu and bus speeds.
	PowerPC CPU speed: 1000 KHz (CPU 0)
	Bus speed: 1000 KHz (CPU 0)

`ofppc_get_cpu_speed` tries two lookups. The first reads a `cpu` property from
`/chosen`; OpenBIOS's `/chosen` has `stdin`, `stdout`, `nvram`, `mmu`, `rtc`
and `memory`, and no `cpu`. The second is the literal path `/cpus/cpu@0`, and
Open Firmware does not promise that name -- a processor node is named for the
part it describes. Here:

    0 > dev /cpus ls
    fff6692c PowerPC,750@0

The properties are all present on that node, including both the function wants:

    device_type               "cpu"
    timebase-frequency        5f5e100         (100 MHz)
    clock-frequency           35a4e900        (900 MHz)
    bus-frequency             5f5e100         (100 MHz)

The file already knows the name cannot be trusted -- `ofppc_get_cpu_count`,
thirty lines below, finds processors by walking `/cpus/` and checking depth
rather than by matching a name. `ofppc_get_cpu_speed` was written the other way
and contradicts it.

`device_type` is what identifies the node, and
`of1275_tree_find_device_type` already exists: it is how `opic.c` locates the
`open-pic`. It now goes in ahead of the literal path, which stays for firmware
that has the node but no `device_type`.

    PowerPC CPU speed: 900000 KHz (CPU 0)
    Bus speed: 100000 KHz (CPU 0)
    Decrementer 100000 KHz, timer tick 1953 us
    Decrementer ticks 195300 (CPU 0)
    Registering processor 0 in KIP (100MHz, 900MHz)

Both figures now match the device tree, and the decrementer is programmed from
the real timebase rather than a guess -- which matters for anything that
measures time, and would have been an invisible wrongness rather than a
failure had the port ever run without it being noticed.

This is upstream's, converted faithfully in §144; `master` has the same two
lookups. It is the fifteenth upstream defect in this platform and the first
that a working system would have tolerated rather than died of.

What remains: nothing confirms sigma0 and the root task run past creation,
because the userland has no console on this platform.


## §150 — l4test runs: the console was a configuration, not a defect

§149 closed on "nothing confirms sigma0 and the root task run past creation,
because the userland has no console on this platform." That turned out to be
the easiest thing in the last eight sections, and the only one that needed no
source change at all.

`user/lib/io/powerpc.cc` has two console backends. Under `CONFIG_COMPORT` it
drives a UART, either at a fixed address or one it locates through a flattened
device tree. Without it:

    extern "C" void __l4_putc( int c )
    {
	L4_KDB_PrintChar( c );
	if( c == '\n' ) L4_KDB_PrintChar( '\r' );
    }

-- the kernel debugger syscall, which the kernel already answers
(`kdb/glue/v4-powerpc/prepost.c` handles `L4_TRAP_KPUTC` by calling `putc`).
The kernel's console works, so user output only has to be routed through it.

`configure` defaults `CONFIG_COMPORT` to 0, and `#if defined(CONFIG_COMPORT)`
is true for 0, so the default build takes the UART path and looks for a device
tree that a PowerMac does not have. `--with-comport=no` leaves it undefined --
`configure.in` guards the `AC_DEFINE` with `!= xno` precisely so it can be --
and the KDB path is selected. That is the whole fix.

    Creating sigma0 (SIGMA0)
    Creating root server (ROOTTASK)
	Idle thread started on CPU 0
    L4/Pistachio test suite ready to go.

    Main menu
    =========
    0) Test KIP
    1) PowerPC Tests
    ...

### The menu cannot be driven, and that is the firmware

Nothing typed at it arrives. Instrumenting the read shows why:

    GETC: stdin ihandle 7c5ab88
    GETC: read -> 0
    GETC: read -> 0

The ihandle is right -- it matches `/chosen`'s `stdin`.

**Corrected by §152.** The conclusion drawn from this -- that OpenBIOS's
client-interface `read` returns no data -- is wrong, and the measurement did
not support it. The trace prints only the first five reads, and all five happen
at the first `getc` call, before anything has been typed. Reads return 0
because there is nothing to read yet, which is what a poll is supposed to do.
Instrumented to print reads that return *non-zero*, the same console delivers
keystrokes: `[GETC ret=1 c=67]`. Input works. What §152 does with it is drive
the kernel debugger.

`l4test` already has the answer: `main.cc` runs `all_tests()` without the menu
when built `-DL4TEST_AUTORUN`. The x86 harness cannot type either.

### What the tests say

    Kernel Interface Page
    =====================
    KernelID reports 0x4.0x2: L4Ka/Pistachio from UKa
    Address of KIP is 0xbff00000
    KIP alignment is OK
    Threads: IRQs=68, sys=32, valid TID bits=17
    Kernel Version: 0.4.0
    Processors: 1
      CPU0: int freq=900000kHz, ext freq=100000kHz
    Checking KIP, depth 0 .. depth 9
      Generic exception test:                             FAILED
      Legacy system call exception test:                  FAILED
      Generic exception unwind:                           OK
      Legacy system call unwind:                          OK
    Unable to deliver user exception: no exception handler.
    >> KD# unhandled user exception, halting thread

The KIP test passes throughout, and `CPU0: int freq=900000kHz, ext
freq=100000kHz` is §149's fix arriving where it was always meant to go: user
space, out of the KIP, matching the device tree.

Then two of the four PowerPC exception tests fail and the run stops on an
undelivered user exception. Those are real failures on a port that has never
executed a user thread before today, and they are the next thing.

`tools/boottest-ofppc` makes all of this reproducible. It cannot share
`tools/boottest`: there is no multiboot here, and QEMU's `-kernel` relocates
the image past a loader that is not position-independent, so Open Firmware must
do the loading from a filesystem -- ext2, because OpenBIOS reads ISO9660 names
mangled. One detail that cost an hour and belongs in writing: **Open Firmware
separates device from path with a backslash.** `hd:\ofppc` opens; `hd:,/ofppc`,
`hd:/ofppc` and `hd:,ofppc` all return zero, and `boot` reports only "No valid
state has been set by load or init-program", which says nothing about why.

### Where the platform stands

    kernel      boots, page hash active, MPIC up, idle thread running
    sigma0      created and running
    root task   created, running, printing, executing tests
    l4test      KIP suite passes; 2 of 4 exception tests fail

Eight sections ago this configuration did not compile, and §142 recorded the
segment MMU and OFPPC as "unconverted by design". Fifteen upstream defects and
three of the migration's own separate that from a system running its own test
suite.


## §151 — Two exception tests, and a page size that changes what a test may assume

§150 left `l4test` reporting

      Generic exception test:                             FAILED
      Legacy system call exception test:                  FAILED
      Generic exception unwind:                           OK
      Legacy system call unwind:                          OK

The asymmetry is the clue, and it is not about exceptions at all. The unwind
variants run *second*.

`exception_tests` creates a handler thread and a subject thread; the unwind
variants create only the subject and make the controller its own handler. In
the failing runs the subject's first `dprintf` never appeared -- so it never
executed a line -- and yet the controller received a message *from* it. Asking
what that message was settles it:

    W1: from 1a0001 label ffe1 u=2 mr1=610a50 mr2=610a50

Label `0xffe1` is `0xffe0 | x`, the pagefault protocol, with two untyped words
-- not the six-word exception message the test waits for. Both words are
`0x610a50`: fault address equals faulting IP, an **execute fault on the
subject's own entry point**, in the root task's text.

`create_thread` makes the creating thread the new thread's pager:

    L4_ThreadControl (tid, me, me, me, (void *) utcb_location);

and the controller does not serve faults -- it goes on to `L4_Wait` for
results. So a created thread that faults delivers into a wait expecting
something else, and the test reports failure. The unwind variants pass because
the two failures ahead of them have already faulted that page in.

### Why it passes elsewhere

The test assumes a created thread never faults. That holds where sigma0 can
hand out large pages: one fault brings in a superpage and the image is
resident. **The PowerPC page hash maps 4K and nothing else** --
`pgent-pghash.h` sets `HW_VALID_PGSIZES` to `(1 << 12)`, and §145 showed the
KIP advertising the same `size_mask` of 4 -- so every new code page faults on
first execution, and the assumption fails.

This is worth separating from the other forty-odd defects in these sections. It
is not upstream's mistake and not the conversion's: it is a test written
against one page-size regime meeting another. The hardware is behaving
correctly, the kernel is behaving correctly, and the test is wrong only on a
machine nobody had run it on.

### The fix

`start_thread` already covers the stack -- `get_startup_values` asks
`get_pages` to touch it. The entry point gets the same treatment: one word read
through the creating thread's own pager, before the new thread runs. Reading
suffices, since sigma0 maps rwx.

      Generic exception test:                             OK
      Legacy system call exception test:                  OK
      Generic exception unwind:                           OK
      Legacy system call unwind:                          OK

`threads.cc` is shared with x86, where the read is a no-op against an
already-mapped page; both x86 userlands still build it.

### Where the platform stands

    kernel      boots; page hash active; MPIC up; idle thread running
    sigma0      created and running
    root task   running its own test suite
    l4test      KIP suite passes; all four PowerPC exception tests pass

What is next: after the exception tests the suite reports "Kernel doesn't
support hypervisor feature" and then halts a thread on an undelivered user
exception at user IP 0x61009c -- a later test that installs no handler. That is
one more never-run path, and it is where §152 would start.


## §152 — The undelivered exception was the test working, and a claim of mine that was not

§151 ended on the suite halting at

    Unable to deliver user exception: no exception handler.
    >> KD# unhandled user exception, halting thread

That is not a fault. `l4test`'s next test is called **`unhandled_exception_test`**,
and it exists to provoke exactly this: it creates a thread with no exception
handler, lets it trap, and then checks -- through `L4_ExchangeRegisters` -- that
the kernel halted the thread at the faulting instruction, restarts it past that
instruction, and waits for it to finish. The kernel printing that line and
halting the thread *is the behaviour under test*.

What stopped the run was the line above it. `except_handlers.c` calls

    enter_kdebug( "unhandled user exception, halting thread" );
    halt_user_thread();

and `enter_kdebug` blocks reading a command. `master` has the identical pair, so
this is not the conversion's; and `tools/boottest` has dealt with it on x86
since it was written, by feeding `g` at the console and saying so in its own
comments. `boottest-ofppc` now does the same.

    Unhandled exception test:                             OK
    Unhandled exception resume:                           OK

### A correction

Doing that required retracting something §150 asserted: that OpenBIOS's
client-interface `read` returns no data on this console, and therefore that the
menu could not be driven at all.

The measurement did not support the claim. The instrumentation printed the
first five reads, and all five happen at the first `getc` call -- before
anything has been typed. They return 0 because there is nothing to read yet,
which is what a poll does. Printing reads that return *non-zero* instead shows
keystrokes arriving intact:

    [GETC ret=1 c=67]

`0x67` is `g`, and kdb echoes `go` and continues. **Input works.** §150 is
corrected in place.

The mistake is worth naming precisely, because it is the same shape as the one
§146 retracted about `-D` and `-imacros`: a check that could only have produced
the answer it produced, read as though it had tested something. Five samples
taken before the stimulus cannot say anything about the response.

### Where the platform stands

Seventeen named tests pass and none fails:

    Generic exception test / unwind                       OK
    Legacy system call exception test / unwind            OK
    Unhandled exception test / resume                     OK
    Page touch                                            OK
    From parameter (global) / (local)                     OK
    Send / ReplyWait Message transfer                     OK
    Send / Receive timeout                                OK
    Local destination Id                                  OK
    Send / Receive / Pagefault cancelled                  OK

-- the whole PowerPC exception suite, the memory touch test, and ten IPC tests
including timeouts and cancellation. The kernel is delivering exception IPC,
paging, switching address spaces, and running IPC with timeouts on hardware it
had never executed on nine sections ago.

It stops in the string-copy IPC test, where a kernel path answers
`unimplemented`, at user IP `0x604788`. That is the next thing.


## §153 — The copy area: a number the tree already had, kept in two places

§152 left the suite stopping in the string-copy IPC test, on a kernel path
answering `unimplemented`. That path is one this migration wrote. §144 met

    ppc_resource_bits_t *bits = (ppc_resource_bits_t *)&src->resource_bits;
    ...
    ppc_set_sr( COPY_AREA_SEGMENT,
		partner_seg.raw | bits->get_copy_area_dst_seg() );

in `tcb_resources_enable_copy_area`, found that neither `ppc_resource_bits_t`
nor `get_copy_area_dst_seg` exists anywhere -- `master` included -- and left
the function `UNIMPLEMENTED()` on the grounds that recovering the intent would
be invention. That was the right call with the information available then, and
wrong on the facts: **the value is determined by the two functions on either
side of it.**

`tcb_resources_setup_copy_area`, ten lines above, stores

    self->copy_area_offset = (word_t)*daddr & ~(COPY_AREA_SIZE - 1);

and `COPY_AREA_SIZE` is `0x10000000` on the segment MMU -- one PowerPC segment
exactly. So `copy_area_offset` is the base of the segment the destination lives
in, in the partner's space. `space_get_vsid` builds a VSID as

    return seg.raw | ((word_t)addr >> 28);

So the segment register wants the partner's segment id or'd with that segment's
index, which is `copy_area_offset >> 28`. That is what the missing accessor
returned. `ppc_resource_bits_t` was a second place to keep a number
`copy_area_offset` already held, and losing it lost nothing.

`tcb_resources_copy_area_real_address` -- the other side of the pair, which
maps a copy-area address back to the partner's -- confirms the reading:

    return addr_offset(addr, self->copy_area_offset - COPY_AREA_START);

The function moves out of line into `resources.c`. It needs `tcb_get_partner`
and `tcb_get_tcb`, and the header is reached from `api/v4/tcb.h` before either
is declared -- which is the mechanical reason §144 found `UNIMPLEMENTED()`
easier than a translation.

### What it buys

Cross-address-space string copy, pagefaults and all:

    Intra address space string copy IPC test (no pagefaults)
      Simple / substring / compound / multiple complex transfers   OK
      Scatter / Gather / Complex Scatter-Gather                    OK
      Too long, no, missing, too short receive buffer              OK
      Complex cut message                                          OK

    Inter addres space string copy IPC test (with pagefaults)
      Simple string transfer (no page faults)                      OK
      Single sender pagefault                                      OK
      Single receiver pagefault                                    OK
      Multiple sender and receiver pagefaults                      OK

The pagefault lines are the ones worth reading: a fault taken *inside the copy
area* is resolved through the partner's address space, which is the whole point
of the segment register this section restored.

The suite now reaches **fifty passes** and seven failures, all seven in
territory it could not previously get to: transfer timeouts, transfer aborts,
`ThreadControl+ExReg`, and priority change. The author's own warning about
tunnelled pagefaults, in the same function, still stands and is untouched.

### On the shape of this one

This is the third defect of the migration's own making, after `asid.h` (§144)
and `initial_switch_to_c` (§147) -- and like both, it is not a mistranslation.
It is a place where the conversion, meeting something it could not compile,
chose to stop rather than to look one function further. The three of them
together say that the risk in a mechanical migration is not the mechanical
part; it is the handful of places where the machinery stops and a judgement is
made with less context than the codebase actually offers.


## §154 — Unmap never unmapped: two fields lost when a generic inline was split

The five transfer-timeout failures §153 left were not about timeouts. Every
measurement taken to find them said the same thing in a different way, and it
took four of them to hear it:

  - no copy-area DSI is ever taken;
  - `handle_xfer_timeouts` is never called;
  - `pghash_flush_4k_mapping` is never called;
  - `pgent_clear` fires six times in a run, all of them UTCB and KIP teardown.

Nothing was faulting because **nothing was being unmapped**. `l4test` revokes a
page with `L4_Flush` and expects the string transfer to stop on it; the page
stayed mapped, the transfer ran to completion, and the test reported the only
thing it could -- no error, no cut.

`master` has one generic `space_t::unmap_fpage`, an inline in
`api/v4/space.h`:

    ctrl.mapctrl_self	= flush;
    ctrl.unmap		= fpage.is_rwx ();
    ctrl.set_rights	= ! fpage.is_rwx ();
    ctrl.reset_status	= 1;
    ctrl.deliver_status	= 1;

The conversion turned it into one copy per architecture. `glue/v4-x86/space.c`
has all five fields. `glue/v4-powerpc/space.c` had three: **`mapctrl_self` and
`unmap` are missing.** The mapping database was told neither to unmap nor to
act on the caller's own mappings, so `L4_Flush` did nothing whatever -- on both
PowerPC platforms, not just the one being brought up.

    Zero xfer timeouts:                                   OK
    Sender xfer timeout:                                  OK
    Receiver xfer timeout:                                OK

### Two things not done

A `tlbie` was *not* added to `pghash_flush_4k_mapping`. Clearing a page hash
entry leaves the translation the CPU has already cached, and the eviction path
in `pghash_insert_4k_mapping` brackets exactly that store with `sync()` and
`ppc_invalidate_tlbe()` -- so the omission looks wrong. It was added while the
real cause was still unknown, and then tested: the tests pass identically
without it. An unverifiable fix is not worth carrying, and this is the same
judgement §146 made about claiming memory from Open Firmware.

The **transfer-abort tests are not fixed**, and now hang rather than fail. They
used to complete instantly, because no IPC ever blocked long enough to abort;
now one does, and aborting it through `L4_ExchangeRegisters` does not return.
The suite therefore stops before `ThreadControl+ExReg` and priority change,
which it previously reached and failed. That bug was always there; the fix
above is what made it reachable.

### The fourth of the migration's own

This is the fourth defect that belongs to the conversion rather than to
upstream, after `asid.h` (§144), `initial_switch_to_c` (§147) and the copy area
(§153). Three of the four share a shape worth stating plainly: **a construct
that existed once became several copies, and one copy lost something.** A
template collapsed to its single instantiation; a generic inline split per
architecture, twice. None is a mistranslation of an expression. Each is a place
where structure that used to guarantee agreement was replaced by duplication
that merely permits it -- and where nothing then checked that the copies still
said the same thing.

`glue/v4-x86/space.c` and `glue/v4-powerpc/space.c` still hold two copies of a
function that has no architecture-specific content at all. They agree today
because this section made them agree.


## §155 — The abort hang: two hypotheses eliminated, not fixed

§154's unmap fix made `Sender abort` and `Receiver abort` reachable, and they
hang. This section did not fix them. It records what they are *not*, because
both wrong answers were plausible enough to cost a day twice.

**Not a thread-state classification problem.** The tunnelled-pagefault path
leaves the copier in `THREAD_STATE_WAITING_TUNNELED_PF` and the partner in
`LOCKED_RUNNING_NESTED`. `exregs.c` acts on an abort only for

    is_sending()	polling, locked_running
    is_receiving()	waiting_forever, waiting_timeout, locked_waiting

and neither tunnelled state appears in either list -- while `tcb_unwind` has an
explicit `WAITING_TUNNELED_PF` branch, so the machinery plainly expects to be
called for it. That asymmetry looks exactly like the bug and is not: printing
the state at every abort shows the four that occur are `polling`,
`waiting_forever` and `halted`, all handled correctly. `master`'s predicates are
identical, so it would have been upstream's had it been anything.

**Not a missing `tlbie`.** Recorded in §154 and repeated here because it is the
same trap: `pghash_flush_4k_mapping` clears a hash entry without invalidating
the cached translation, where the eviction path a few lines up does. Adding it
changes no observable behaviour.

What is established: the run stops *before* any `ExchangeRegisters` for this
test -- the pager never issues the abort. The sender unmaps the second page of
its own send buffer, sets `ipc_pf_abort_address` to it, and sends; the pager is
supposed to receive the fault, match the address, and abort. Either the fault
does not arrive or it does not match. Note that the sender runs in a *separate
address space* (`setup_ipc_threads (..., rcv_same_space = true,
snd_same_space = false)`), so `ipc_pf_abort_address` reaches the pager only
because sigma0 hands both spaces the same physical page -- a sharing assumption
worth checking before anything subtler.

The tests that this unblocked stand: `Zero xfer timeouts`, `Sender xfer
timeout` and `Receiver xfer timeout` pass, and the suite has no failures where
it reaches. It reaches less far in wall-clock terms than before §154, because
transfers now genuinely block for the thirty-second periods the tests ask for.


## §156 — The abort hang: the pager's flags are not shared with the sender

§155 asked whether `ipc_pf_abort_address` is really visible to the pager, given
that the sender runs in its own address space. Measured, the answer is no --
and it is the other flag of the pair that wedges the run.

`l4test`'s pager loop and the threads it pages share two globals,
`ipc_pf_block_address` and `ipc_pf_abort_address`, and the sender writes them
from a space created by `setup_ipc_threads (..., snd_same_space = false)`.
Printing both sides of one write:

    [PGR blocking 32d5000 (blk=32d5000)]     pager withholds the fault
    [T2 cleared blk, readback=0]             sender writes 0, reads back 0
    [PGR blocking 32d5000 (blk=32d5000)]     pager still sees the old value

The sender's store lands somewhere the pager does not read. **They are not the
same page.**

**Wrong; corrected by §157.** The two `blocking` lines are not one value read
twice. The sender sets the flag once per test and clears it once per test, and
the instrumented clear was the first of two such pairs -- so the second
`blocking` line is a legitimate block from the *following* test, which had set
the flag again. The pages are shared, and §157 shows the kernel sharing them.

That is the hang. After `Sender xfer timeout`, the sender clears
`ipc_pf_block_address` so the pager will serve faults again; the pager never
sees the clear, so it keeps withholding faults at that address. The next test
begins with `setup_t2_mappings`, whose first act is to write over both pages of
the send buffer -- the second of which is exactly the address the pager is
still refusing. The sender faults, nobody serves it, and the run stops there,
before it reaches the abort at all.

Two corrections to §155, both mine. It said the pager never issues the abort --
true, but not because the sender fails to reach that code: the sender does
reach it, and stops a few statements earlier, inside `setup_t2_mappings`.
§155's evidence for the stronger claim was a run whose instrumentation was
printing forty lines through the Open Firmware console, which is slow enough
that the run never got that far. Instrumentation that changes what it measures
is worth remembering here: the same console made the timing of §150's input
test misleading too.

What is *not* yet established is why the pages differ. The pager serves a fault
by touching the address in its own space and replying with
`L4_MapItem (fp, faddr)`, which should hand the child the same physical page;
`snd_same_space = false` is exactly the case where that map item is appended.
Either the map item is not delivered, or it is delivered and does not share.
That is the question to answer next, and it is a kernel question rather than a
test one -- `L4_Flush` not working (§154) hid it, because before that fix the
sender never faulted here at all.


## §157 — Retracting §156: the pages are shared, and three localisations were wrong

§156 concluded that `l4test`'s pager and a sender in its own address space do
not share the globals they coordinate through. That is wrong, and the kernel
says so directly. Tracing every cross-space `space_map_fpage`:

    [MAP f_spc=c0032000 t_spc=c003a000 snd=6190c7 base=619000 rcv=10]

`c0032000` is the root task's space and `c003a000` the sender's.
`snd=0x6190c7` is page `0x619000`, size_log2 12, **rwx 7**; `rcv=0x10` is the
complete address space. And `ipc_pf_block_address` lives at `0x619528`:

    00619524 B ipc_pf_abort_address
    00619528 B ipc_pf_block_address

The page holding both flags is mapped from the pager's space into the sender's,
fully accessible. They are shared, and the map items work -- which they must,
since the sender executes at all only because its text arrives the same way.

### What the evidence actually said

    164:   Zero xfer timeouts:                          OK
    165: [PGR blocking 32d5000 (blk=32d5000)]
    166: [T2 cleared blk, readback=0]
    167:   Sender xfer timeout:                         OK
    168: [PGR blocking 32d5000 (blk=32d5000)]

Line 168 does follow line 166, which is what the claim rested on. But the
sender sets the flag once per test and clears it once per test, and there are
two such pairs; the instrumented clear was the first. Line 168 is the block
belonging to the *next* test, which had set the flag again a few statements
earlier. Nothing was stale.

### Three wrong localisations, one cause

This investigation produced three confident and wrong answers before this one:

  - §155: the sender never reaches the `Sender abort` section. It does.
  - §156: the coordinating flags are not shared. They are.
  - and, within §156's own follow-up, that the hang sits in
    `setup_t2_mappings`. Tracing it shows the function completing, including
    with the `no_access` argument.

Two of the three came from the same mechanism, and it is worth stating because
it will recur: **printing through the Open Firmware console costs enough time
to change what the run reaches.** Forty lines of pager tracing kept the run
from ever arriving at the section under investigation, which then looked like
evidence that the section was unreachable. §150's input measurement failed the
same way from the other side -- sampling before the stimulus.

On a console where each character is a client-interface call, instrumentation
is not free and is not neutral. Anything measured this way needs a control run,
and a claim of the form "X never happens" needs to be distinguished from "the
run never got to X".

### What is known

`L4_Flush` works (§154), the transfer timeouts pass, the map items share, and
the sender reaches `Sender abort` and stops somewhere after
`setup_t2_mappings` returns and before the send completes. Where, exactly, is
not established, and the remaining candidates are the `L4_Load`/`L4_Send` pair
and the fault path underneath them.


## §158 — `L4_Send` blocks; the fault path is quiescent

§157 left two candidates for where the sender stops: the `L4_Load`/`L4_Send`
pair, or the fault path underneath. Measuring it needed a method that does not
perturb what it measures, which rules out the console: §157's own retraction was
caused by printing.

The root task is loaded at physical `0x600000` and runs identity-mapped there,
so its globals can be read from **QEMU's monitor** while the guest is wedged --
`xp/1xw`, no guest cost at all. Four `volatile` globals were added: a progress
marker the sender bumps through the `Sender abort` section, a count of faults
the pager sees from the sender, and the last such fault address.

Sampled twice, thirty seconds apart, identical both times:

    0x619534  dbg_mark       = 4           entered L4_Send, never returned
    0x61952c  dbg_t2_faults  = 0x18 (24)   not increasing
    0x619530  dbg_last_fault = 0x032d5000  the abort address
    0x619524  ipc_pf_abort_address = 0     cleared

Reading them in order gives the whole sequence:

  - `mark = 4` is set immediately before `L4_Send` and `5` immediately after.
    It is 4, so **the send is what blocks.**
  - The fault count is static across thirty seconds. **There is no fault loop**;
    the fault path is not where it is stuck.
  - The last fault the pager saw is `0x32d5000`, which is
    `&t2_buf[PAGE_SIZE]` -- the page the test revoked, and the address it had
    just installed as `ipc_pf_abort_address`.
  - That variable now reads 0, and the *only* code that clears it is the
    pager's abort branch. So the pager matched the fault, called
    `L4_ExchangeRegisters (tid, 3 << 1, ...)` to abort the sender's IPC, and
    deliberately did not reply to the fault.

So the fault arrives exactly once, the pager aborts, and the sender never
returns. **The hang is in the abort, not underneath it.**

That restores §155's original hypothesis, which this investigation discarded on
bad grounds. §155 checked the thread state at four `ExchangeRegisters` aborts,
found them all in states the code handles, and concluded the abort was
innocent. Those four were aborts from *earlier* tests: the check never
established that any of them was this one, nor -- more importantly -- that an
abort which reports success actually unblocks the thread. "The mechanism works
somewhere" is not evidence that it worked here.

### A fragility worth recording

Twice now, an unrelated change to `l4test`'s layout has left the run hanging in
the KIP test at `APIVersion reports 132.5` -- once when a `L4_Msg_t` was added
to a stack frame, once when two initialised globals were added. Both times the
addresses of everything else were unchanged. Something in the KIP test or its
surroundings is sensitive to layout in a way that has nothing to do with what
was being measured, and it will waste time again if it is not expected.

### Next

The sender is in `L4_Send`, inside a string copy, faulting on its own send
buffer, with a pagefault IPC outstanding to its pager, when the pager aborts it.
What remains is to determine the sender's thread state at that moment and follow
`tcb_unwind` from it -- the interesting case being whether an abort delivered
while a *nested* pagefault IPC is outstanding unwinds the outer string-copy IPC
as well, or leaves it half-unwound with nothing to resume it.


## §159 — the abort hang was a missing config option, not a defect

§158 concluded the sender blocks in `L4_Send`, the fault path is quiescent, and
the pager had taken its abort branch -- so the abort was issued and failed to
unblock anything. The first half was right. The last clause was not, and it was
inferred rather than observed: `ipc_pf_abort_address` reading 0 was taken as
proof the pager cleared it, but the test clears that variable itself on the line
after the send. Zero was consistent with the abort never happening.

Settling it needed the *order* of events, not their final values, so the probes
became a 256-entry ring in the kernel -- `dbg_ev (id, a, b, c)` writing into a
`.bss` array -- dumped through the QEMU monitor after the hang. Events recorded:
ExchangeRegisters aborts, every `tcb_unwind` pass with its state and reason,
the active-sender branch's partner check, `handle_ipc_error`, and every
pagefault IPC sent and returned.

The tail of the ring ends:

    2415 pf-SEND     t117 addr=0x032d5000 ip=0x0060943c
    2416 pf-ret-ok   t117 tag=0x80 err=0x1000c
    2417 pf-SEND     t117 addr=0x032d5000 ip=0xc0015b24     <- never returns

`ip=0xc0015b24` is a kernel address: this is the tunnelled pagefault raised from
inside the string-copy path, at the address the test revoked. It is the last
event in the ring. **No `exregs-abort` event follows it** -- and the ring shows
those events firing happily for the intra-space abort tests earlier
(`exregs-abort tid=t112.1 state=POLLING ctrl=0x6`). So the pager's
`L4_ExchangeRegisters` was never reaching the abort code at all.

It was being rejected at the door. `has_exregs_perms` allows ExchangeRegisters
only within one address space, with one exception:

    #if defined(CONFIG_X_PAGER_EXREGS)
        // all threads in pager address space can ex-regs
        ...
    #endif

l4test's pager lives in the root task's space; `t117` is in a separate space for
the inter-space string-copy tests. Without that option the call returns
`EINVALID_THREAD`, the pager silently gets a nilthread back, nobody replies to
the fault, and the sender waits forever. The code is byte-for-byte master's --
the function, the caller, and the check all match.

`CONFIG_X_PAGER_EXREGS` defaults to `n`. Every build directory that runs l4test
has it on, **including `scratch-ppc` (ppc44x)**; the only two with it off are
`scratch-ofppc` and `scratch-ebony`, the two configs created during this work
with a minimal `batchconfig` line listing just the architecture, CPU and
platform. The option was never turned off -- it was never turned on.

    make batchconfig CMLBATCH_PARAMS="ARCH_POWERPC=y CPU_POWERPC_IBM750=y \
                                      PLAT_OFPPC=y X_PAGER_EXREGS=y"

With it enabled, `Sender abort` and `Receiver abort` both pass, and the run
continues into fourteen tests it had never reached:

    before:  OK=38  FAILED=0   (suite hangs at "Sender abort")
    after:   OK=54  FAILED=2   (suite runs to the end menu)

The two failures -- `ThreadControl+ExReg` and `Change priority associated` --
are in test groups the hang had been hiding. They are new ground, not
regressions.

Because `build/` is not tracked, the config itself cannot be committed; the
recipe above and `tools/boottest-ofppc`'s header are the durable record, and
both now say so.

### What went wrong in §155 and §158

Three sections chased this bug and the first two localised it wrongly, in the
same way each time: a fact was *inferred* from a value that had another equally
good explanation, and the inference was then treated as established.

  - §155: four aborts were in states the code handles, therefore the abort
    mechanism is innocent. Those four were from other tests.
  - §158: `ipc_pf_abort_address` is 0, therefore the pager cleared it. The test
    clears it too.

Both would have been caught by asking "what else makes this value what it is?"
Neither was caught by more measurement of the same kind -- final values cannot
distinguish "happened and did nothing" from "never happened". The ring buffer
settled it in one run because it records order and identity, and a *missing*
event in a sequence is evidence in a way that a zero in a variable is not.


## §160 — the two tests §159 uncovered: one upstream, one live on x86

Enabling `X_PAGER_EXREGS` let the suite run fourteen tests further and exposed
two failures. They turned out to have nothing to do with each other.

### `ThreadControl+ExReg` — powerpc's `setup_exreg` never set the entry point

`setup_exreg (&ip, &sp, func)` allocates a stack and reports where to start the
thread. Every port's copy ends with

    *ip = (L4_Word_t) func;

except powerpc's, which allocates the stack and returns. So `ip` stays whatever
was on the caller's stack.

Only one caller notices. `exreg.cc` and `tcontrol.cc`'s `run_a_thread` pass the
value to `start_thread` and then check what `ExchangeRegisters` *returned*, so a
junk entry point never shows up -- and `run_a_thread` prints
`print_result ("Run a thread", true)`, a literal. `tc_then_exreg` is the one
test that starts a thread purely by ExchangeRegisters and then checks whether it
ran; it is the one that failed.

`master`'s powerpc/help.cc has the same omission, so this is upstream, not the
conversion. powerpc64's file has neither function.

### `Change priority associated` — `nilctrl` became zero

`L4_Set_Priority` passes `~0UL` for the three control words it does not want to
change. Master tests for exactly that:

    if (req.time_control != schedule_ctrl_t::nilctrl() && ...)   // nilctrl() == ~0UL

The conversion rendered every one of those as `.raw != 0` -- the opposite
sentinel -- and dropped `nilctrl` entirely. Both directions are wrong: a caller
saying "leave this alone" (`~0UL`) now takes the change path, and a caller
asking for zero (priority 0) is now skipped.

The visible symptom was the `time_control` guard. With `~0UL` no longer
recognised as "unchanged", `check_schedule_parameters` went on to test whether
the quantum and timeslice were periods, which `~0UL` is not, and returned
`EINVALID_THREAD`. Every `L4_Set_Priority` failed. Its companion test
"Change priority unassociated (ok=change failed)" passed throughout -- it
expects failure, and got it for the wrong reason.

Seventeen sites, matching master's seventeen `nilctrl` comparisons exactly:
seven in `sched-rr/schedule.c`, ten in `sched-hs/schedule.c`. `nilctrl` is
restored as `schedule_ctrl_is_nil` in `api/v4/syscalls.h`.

**This one was live on x86**, in generic code, on both schedulers -- found only
because a platform nobody had booted since 2010 ran a test the x86 harness never
reaches (x86's l4test is not built `-DL4TEST_AUTORUN`, so it sits at a menu):

    x86-x64-p4-smp     (sched-rr)  before: FAILED   after: OK
    x86-x32-p4-hsched  (sched-hs)  before: FAILED   after: OK
    ofppc              (sched-rr)  before: FAILED   after: OK

The rest of the x86 run is unchanged line for line, including its one
pre-existing failure (`Local destination Id`).

    ofppc:  OK=56  FAILED=0

### Unrelated, found while sweeping

`tools/configsweep 'x86-x*'` builds 30 of 31 configurations. `x86-x64-p4-cm`
fails with four `conflicting types for 'mem_region_is_intersection'` errors in
`generic/memregion.h` -- the function §144 restored, against an
`x32_mem_region_t`. It fails identically with these changes stashed, so it
predates them and is not touched here.


## §161 — `x86-x64-p4-cm`: a name missing from the x32 rename list

The sweep failure §160 noted was one line of omission.

Compatibility mode needs a second copy of the V4 API types built with a 32-bit
word. In C++ that was a re-include inside `namespace x32`; C has no namespaces,
so `glue/v4-x86/x64/x32comp/x32-names.h` does it with the preprocessor -- a list
renaming every name the re-included headers declare to an `x32_`-prefixed one,
and a matching list undoing it afterwards. Its own header comment states the
invariant:

    The two lists below must stay in step.  They do so under pressure: a name
    missing from the rename list collides on the second include, and one missing
    from the unrename list leaves the 64-bit code compiling against x32_ names.
    Both are compile errors, immediately.

Which is precisely what happened. The list covers `mem_region_t`,
`mem_region_get_size`, `mem_region_set` and `mem_region_is_empty` -- everything
`generic/memregion.h` declared *when the list was written*. §144 then restored
`mem_region_is_intersection` to that header, having found it dropped because its
only caller was in a configuration that did not build. Nothing connected the two:
the restored function was not renamed, so on the second include it was redeclared
with `x32_mem_region_t` parameters against the first copy's `mem_region_t`, four
times over.

Only `x86-x64-p4-cm` sets `CONFIG_X86_COMPATIBILITY_MODE`, so it was the only
configuration that could see it.

Adding the name to both lists is the whole fix. The block is realigned because
the new name is longer than the column the other four shared.

    tools/configsweep 'x86-x*'    before: 30 OK, 1 FAIL
                                  after:  31 OK

The cm kernel had never been run in this migration, only built, so it was worth
booting rather than trusting the compile: it reaches the l4test menu, passes both
schedule tests, and its full run ends at `Local destination Id` -- the same
pre-existing failure `x86-x64-p4-smp` stops at, so nothing here is specific to
compatibility mode.

Worth noting what made this cheap. The failure is loud, immediate, and names the
exact symbol; §144's dropped function was silent and took a boot-time
investigation to find. The difference is that the rename list is a place where
the invariant is written down and checked by the compiler. There is no such
place for "this header declares a function nobody currently calls".


## §162 — `kdb/generic/acpi.cc`, and the config nobody could select

Converting this file was straightforward; what it uncovered was not.

### The conversion

`generic/acpi.h` was converted on 2026-07-28 (`f3d2a88`) and its one C++ consumer
was not, so `CONFIG_ACPI` had not compiled since. The mapping is mechanical:

    acpi_rsdp_t::locate()          -> acpi_rsdp_locate()      (platform/pc99/acpi.h)
    rsdp->rsdt() / ->xsdt()        -> acpi_rsdp_rsdt/xsdt()
    acpi.interact (cg, "acpi")     -> cmd_group_interact (&acpi, cg, "acpi")
    acpi_madt_irq_t::polarity_t    -> ACPI_MADT_* defines, via the get_* helpers
    dump_acpi_header (h, prefix="") -> pointer parameter, prefix at every call

The RSDT and XSDT dumps were two near-identical loops differing only in pointer
width; they are one macro now, which is also how `generic/acpi.h` renders the
`find`/`list` template pair.

### Three defects that only appear once it runs

**The RSDT loop walked the XSDT.** Inside `if (rsdt)` every reference after the
header was to `xsdt` -- its length, its pointer array. On ACPI 1.0
`acpi_rsdp_xsdt` returns NULL, so the branch guarded by a non-null `rsdt`
dereferenced a null `xsdt`. Collapsing both loops into one macro removes the
possibility rather than patching the instance.

**`locate()` was declared `locate(addr_t addr = NULL)`** and kdb called it with
no argument -- a 128K scan from virtual zero. It never found anything and on x64
it faults. The scan now starts at `ACPI20_PC99_RSDP_START` through
`acpi_remap`, matching `platform/generic/intctrl-apic.c`, the one caller in the
tree that locates the RSDP successfully.

**`phys_to_virt` on the header strings.** Once the pointers come through the
remap window, translating again lands on mapped-but-unrelated memory. The tell
was precise: both `%.6s` strings printed empty while the integer fields beside
them, read off the same header, came out right.

Driven under QEMU it now produces the machine's real tables:

    /arch/acpi> dump
    ACPI rev: 0, OEM: "BOCHS "
    RSDT @ fffffffecffe1c52
      OEM:    ["BOCHS ", tid: "BXPC    ", rev: 1]
      APIC @ fffffffecffe1b7a
        Local APIC @ 00000000fee00000
          Local APIC  [Id: 0x0, CPU Id: 0x0, enabled]
          I/O APIC  [Id: 0x0, IRQ base: 0, Addr: 00000000fec00000]
          Interrupt Override  [ISA, Bus IRQ: 0, Glob IRQ: 2, ...]

### The object was on the link line twice

With the compile fixed, the link failed: every non-static function in
`src/generic/acpi.o` collided with itself. `CONFIG_ACPI` adds that source in
`src/generic/Makeconf` and `CONFIG_IOAPIC` adds it again in the x64 and x32 glue
Makeconfs. Master has the identical pair, so this is upstream -- latent, because
nothing had ever linked a configuration with both set. The IOAPIC adds are now
skipped when ACPI already contributed it, which keeps the link order the `sort`
that would also dedupe it would disturb.

### And the thing worth knowing

**`CONFIG_ACPI` is not a configuration symbol.** It appears nowhere in
`kernel/config/`, no shipped config tar sets it, and only three Makeconf guards
mention it. The `build/scratch-acpi` directory has it because it was written into
`.config` by hand during this work.

So §160's description of it as "a real, selectable option" was wrong. Nothing
distinguishes it from the EFI and simics files except that a hand-edited
`.config` reaches it, which is exactly how it was reached here. What that means
in practice: this file could rot for years without any sweep, boot test or
shipped configuration noticing -- and it did.

    tools/configsweep 'x86-x*'   31 OK, 0 FAIL
    kernel .cc remaining          40 -> 39


## §163 — powerpc64: a configuration, and the first six headers

powerpc64 is the last unconverted architecture: 32 `.cc` files (~4,400 lines)
across `arch/powerpc64`, `glue/v4-powerpc64`, three platforms and the kdb tree,
plus 25 headers (~3,600 lines) still holding classes. It has never been built in
this migration.

### It is not blocked on a toolchain

§162's listing said powerpc64 was untestable for want of a cross compiler. That
was wrong -- `powerpc64-linux-gnu-gcc 13.3.0` is installed. What is genuinely
missing is a shipped config tar, so a build directory has to be made by hand.
The recipe, which is not obvious:

    mkdir -p build/scratch-ofg5/kernel/config
    cp build/scratch-ofppc/kernel/{Makefile,Makeconf.local} build/scratch-ofg5/kernel/
    # fix SRCDIR; Makeconf.local must already carry ARCH=/SUBARCH=/CPU=/
    # PLATFORM=/SCHED= lines, because config/Makefile rewrites them in place
    # rather than generating the file
    cd build/scratch-ofg5/kernel
    make batchconfig CMLBATCH_PARAMS="ARCH_POWERPC64=y PLAT_OFG5=y \
                                      CPU_POWERPC64_PPC970=y SCHED_RR=y"

`SCHED_RR=y` has to be spelled out. The awk that derives `SCHED` matches
`^CONFIG_[_X]*SCHED_[^_]*=y`, and nothing in `powerpc64.cml` defaults the
scheduler, so without it `SCHED=` comes out empty and the build looks for
`src/api/v4/sched-/`.

Of the three platforms, **OFG5** is the one to start from: it is the smallest
(its own `intr.cc` plus `ofpower4/prom.cc` and the two Open Firmware files),
and `qemu-system-ppc64 -M mac99 -cpu 970fx` is the same Uninorth/OPIC machine
the ofppc harness already drives, so §150's boot method should carry over.

### Six headers converted

    arch/powerpc64/frame.h        two plain data classes -> structs
    arch/powerpc64/vsid_asid.h    vce_t / vsid_asid_cache_t / vsid_asid_t
    glue/v4-powerpc64/debug.h     spin/spin_forever default arguments
    glue/v4-powerpc64/syscalls.h  extern "C" on the RTAS syscall declaration
    glue/v4-powerpc64/pgent.h     pgent_t -> struct; pgsize_e, permission_e
                                  and wimg_e out of class scope
    glue/v4-powerpc64/space.h     the 373-line space_t

`space.h` follows the 32-bit port's naming exactly -- `space_init`,
`space_is_user_area`, `space_get_copy_limit` -- so the two ports read the same
way. `pgent.h` follows `arch/powerpc/pgent-pghash.h`, including leaving the
operations unprototyped: a non-static declaration ahead of a static-inline
definition is a conflict in C, and every consumer reaches both headers through
`pgent.h`.

`pgent_inline.h`'s thirty methods converted cleanly because upstream wrote every
member access as `this->`, so the rewrite was `this->` to `self->` plus the
signature -- and the calls a method made on itself showed up as `self->name (`,
a shape that is easy to find exhaustively rather than by eye. Nothing was left
behind: no `pgent_t::`, no `inline`, no `->method(` anywhere in the file.

### Where it stands

    first build:   the cascade -- headers unparseable, nothing reached
    now:           29 errors, in five named places

    8  platform/ofpower4/prom.cc    .cc against converted generic headers
    8  arch/powerpc64/pghash.h      ppc64_sdr1_t / ppc64_pte_t / ppc64_htab_t
    4  glue/v4-powerpc64/space.h    residue from pgent/pghash not yet done
    3  glue/v4-powerpc64/pghash.h
    3  generic/types.h              _Bool seen by a .cc -- every remaining
                                    .cc fails here, which is the whole point:
                                    they cannot stay C++
    2  glue/v4-powerpc64/pgent_inline.h
    1  platform/ofg5/intctrl.h      inherits from a base that is now a struct

The next header is `arch/powerpc64/pghash.h`, and it holds the one construct
that needs a decision rather than a transcription: `ppc64_pte_t::create` is
**overloaded three ways** and C cannot carry that. The signatures differ in
arity and meaning, so the names should say which is which rather than being
numbered.

ofppc, ppc44x and x86-x64 all still build; these headers are reachable from
powerpc64 configurations only.


## §164 — powerpc64: the page hash, the device tree, and the first two sources

Four more headers and the first two `.cc` files. The kernel `.cc` count is
39 -> 37.

### The three-way overload

`arch/powerpc64/pghash.h` held the construct §163 flagged: `ppc64_pte_t::create`
declared three ways -- four arguments, five, and six. They are not three
functions. Each begins by zeroing `word0`, so the fields the shorter forms omit
(`l` and `bolted`) are *already* zero, and what the longest form does with them
is assign zero. The four-argument form is the six-argument one with
`large = 0, bolted = 0`; the five-argument form is it with `bolted = 0`.

That is checkable rather than assumed, so the conversion keeps all three entry
points -- `ppc64_pte_create_4k`, `ppc64_pte_create`, `ppc64_pte_create_bolted`
-- with the first two delegating. The names say which field each admits instead
of numbering them. Only the six-argument form has callers
(`glue/v4-powerpc64/pghash.c`, twice); the other two were already dead.

`ppc64_htab_t`'s `optimal_size` and `min_size` were instance methods touching no
member, so they become free functions; `get_pteg` takes the htab. The
`pghash_t` glue class had a `bolted` argument defaulting to `false`, so its two
spellings become two entry points, `pghash_insert_mapping` and
`pghash_insert_mapping_bolted`.

### The device tree, against a converted twin

`arch/powerpc64/1275tree.{h,cc}` and `platform/ofppc/1275tree.{h,c}` are the
same code -- the 64-bit copy adds `find_device_type`, `next_by_type`,
`get_parent` and four PCI address structures. The ofppc pair was converted
earlier in this migration, so the 64-bit one had a finished model to match
name for name rather than a style to invent.

Its `get_prop` was overloaded two ways, by name and by index. As with `create`,
the names now say what they look up: `of1275_device_get_prop` and
`of1275_device_get_prop_index`, plus the existing word-sized wrapper as
`of1275_device_get_prop_word`.

The one thing the model did not cover is that C wants loop variables declared
before the loop; four `for( word_t i = ...)` and `for( char *c = ...)` headers
needed a surrounding block. That is mechanical but it is where a careless
conversion silently changes a scope, so each one was bracketed explicitly.

### The stub interrupt controller

`platform/ofg5/intctrl.h` inherited from `generic_intctrl_t`, a base with no
members that describes an interface. As in ofppc and ppc44x the struct stands
alone and the members become free `intctrl_*` entry points. Every one of them
is `UNIMPLEMENTED()` upstream and the controller reports a single interrupt --
this platform's interrupt support does not exist yet, which is worth knowing
before anything tries to boot it.

### Where it stands

    §163:  29 errors
    now:   35, and they have moved

The number went up because it is measuring something different. Every C file
that now compiles reaches further and finds the next unconverted header:
`hwspace.h`, `string.h`, `glue/v4-powerpc64/intctrl.h`, `space.h`'s residue.
That is the shape of this job -- each converted source exposes the next layer,
and the count will keep rising before it falls.

Remaining: 15 headers and 13 `.cc` files for OFG5; the other two platforms and
the eleven kdb files after that. ofppc, ppc44x and x86-x64 all still build.

## §165 -- powerpc64: the TCB, and the last of the C side

With `hwspace.h`, `string.h` and `intctrl.h` out of the way the build reached
the core: `tcb.h`, `utcb.h`, `ktcb.h`, `resources.h`. Those converted, *every
`.c` file in the OFG5 configuration now compiles*. What is left is the thirteen
remaining `.cc` files, and the errors they produce are all of the same kind:
C++ sources calling members of structs that are no longer classes.

### The TCB split

Upstream defined nearly all of powerpc64's `tcb_t` inline in
`glue/v4-powerpc64/tcb.h` -- 672 lines of it, including both thread-switch
assembly blocks. `api/v4/tcb.h` declares those operations as out-of-line
`tcb_*` prototypes *after* it includes the glue header, so the definitions
cannot stay inline: they moved to `glue/v4-powerpc64/thread.c` (was
`thread.cc`), which is exactly the arrangement the 32-bit port already uses.
tcb.h keeps 155 lines: `tcb_stack_top`, the new `tcb_irq_context` helper that
every user-context accessor had spelled out longhand, `initial_switch_to`,
`get_current_tcb`, and the SMP `get_current_cpu`.

`tcb_t::notify` had three overloads, one per argument count. They cannot be
collapsed the way `ppc64_pte_t::create` could (§164): the frame is *not*
cleared first, so the fields a shorter form omits keep whatever was on the
stack. `api/v4/tcb.h` already names all three -- `tcb_notify`,
`tcb_notify_word`, `tcb_notify_word2`.

### Macro hygiene, and a real capture

`return_exchange_registers` in `glue/v4-powerpc64/syscalls.h` declares locals
named `tid`, `ctrl`, `sp_r` ... and the converted `api/v4/exregs.c` now passes
`ctrl.raw` for the control word. The macro's own `register word_t ctrl` was
declared before that argument expanded, so `= ctrl.raw` read the macro's local
and failed. The locals are underscore-prefixed now. In C++ this never bit
because master's exregs.cc passed a `schedule_ctrl_t` object, not `.raw`.

Same file: `register threadid_t tid asm("r3") = result` could not be
initialised from `threadid_get_raw()`'s word. `tid` is a `word_t` now; `pgr`
stays a `threadid_t` because that is still what the caller hands over.

### Three gaps upstream never had to face

None of these are conversion damage; they are places where powerpc64 has been
carried along without being compiled.

`glue/v4-powerpc64/ipc.h` **does not exist** upstream, but both
`api/v4/sched-rr/ktcb.h` and `api/v4/sched-hs/ktcb.h` include it
unconditionally. Those schedulers and the ctrlxfer protocol that gave the
header its contents both postdate the powerpc64 port. Nothing in it would be
powerpc64-specific -- `CONFIG_X_CTRLXFER_MSG` is x86-only -- so the file is
new and deliberately empty.

`CACHE_LINE_SIZE` is undefined for powerpc64, and `api/v4/schedule.h` pads
`schedule_request_queue_t` out to one. `POWERPC64_CACHE_LINE_SIZE` (128) has
been in `arch/powerpc64/cache.h` all along; the glue config.h just never
forwarded it, because CONFIG_SMP was never built here.

`debug_param_t` is likewise undefined for powerpc64 -- the port puts a
`powerpc64_irq_context_t` in `kdb.kdb_param`, as its own frame.cc and disas.cc
show -- yet generic `kdb/api/v4/tcb.c` casts `kdb_param` to `debug_param_t *`.
So master does not compile here either, and had it compiled it would have
misread the pointer. The generic file is guarded on a new
`HAVE_DEBUG_PARAM_T`, which the two ports that define the type now announce.
The space it recovers is only the default value in a `get_hex` prompt.

### resource_bits_t's other half

`api/v4/resources.h` has always had two forms: a bitmask class when the glue
header defines `resource_type_e`, and a bare `word_t` when it does not.
powerpc64 is the only port taking the second branch, and the conversion had
given the `resource_bits_*` accessors only to the first -- so `api/v4/thread.c`
reached `->resource_bits.maskvalue` through a `word_t`. Both branches now
carry the same five entry points plus `resource_bits_raw` for kdb's dump, and
the callers no longer know which branch they are on.

### Where it stands

    §164:  35 errors
    now:   every `.c` compiles; the remainder is 13 `.cc` files

ofppc, ppc44x and x86-x64-p4 all still build. Kernel `.cc` count 37 -> 36.

## §166 -- powerpc64: Open Firmware, the segment abstraction, the timer

Four more headers and four sources. Nothing structurally new after §165; two
things are worth recording.

`arch/powerpc64/segment.h` declared `generic_segment_t`, a base with no members
and four `static inline` declarations that neither it nor anyone else defines
-- an interface description, exactly like `generic_intctrl_t` in §164. The two
implementations (`slb.h` for the SLB machines, `seghash.h` for the segment-table
ones) each define all four, and every caller went through `segment_t::`, never
through a base pointer. So the base is gone and the implementations define
`segment_init_cpu`, `segment_flush_segments`, `segment_flush_segment_entry`
and `segment_insert_entry` directly. Only one of the two headers is ever
included; the config picks it.

`arch/powerpc64/of1275.cc` ends with

    s32_t SECTION (".init")
    interpret( const char *forth )

-- no class qualifier. So upstream defines a *free* function named `interpret`
and leaves `of1275_client_interface_t::interpret` declared-but-undefined.
Nothing calls either one, which is why it never came up. The header names it as
a member, so the C version is `of1275_interpret` taking the interface pointer;
the alternative reading (a genuinely free helper that happens to be dead) is
not supported by anything.

`spinlock_t` in `arch/powerpc64/sync.h` follows the 32-bit port's naming
(`spinlock_init`/`_lock`/`_unlock`); `init`'s defaulted `val=0` is spelled
out at the one call site.

    §165:  every `.c` compiles
    now:   17 `.cc` files left, all of them powerpc64's own

## §167 -- powerpc64: the page hash and the address space

`memcontrol.cc`, `resources.cc`, both `pghash.cc`s and `space.cc` are now C.
Mechanical, except for two things that were never going to compile.

### TRACEPOINT, frozen at an older signature

`glue/v4-powerpc64/space.cc` contains

    TRACEPOINT( hash_miss_cnt,
        printf ( "hash miss @ %p (current=%p, space=%p)\n", ... ));
    ...
    TRACEPOINT( hash_insert_cnt );

`kdb/tracepoints.h` has taken `(tp, str, args...)` for a long time -- master's
copy is identical to ours here. So the first call passes a whole `printf()`
expression where the format string goes (harmless only because
`TBUF_REC_TRACEPOINT` compiles away when the tracebuffer is off, and the
`printf(str, ##args)` branch needs CONFIG_DEBUG), and the second passes no
format string at all, which is a hard arity error in any configuration. This
is the shape TRACEPOINT had before it grew a format string. Both are rewritten
to the current contract; `resources.cc`'s `DISABLED_FPU` had the same first
problem.

### A typo in a branch that is never taken

`early_kernel_map()`, CONFIG_POWERPC64_LARGE_PAGES branch:

    pg.set_entry( kernel_space, pgent_t::size_16m, 0,
                  7, pgent_t;:l4default, true );

`pgent_t;:l4default` -- a semicolon for the first colon. The 4K branch below it
is what every shipped configuration takes, so this has sat there unnoticed.
Converted as `l4default`, which is unambiguous.

### Defaulted arguments, again

`space_t::lookup_mapping`'s `cpuid_t cpu = 0` gets the `_c` suffix the 32-bit
port already uses; `pghash_t::insert_mapping`'s `bool bolted = false` splits
into `pghash_insert_mapping` and `pghash_insert_mapping_bolted` (§164 did the
same for the header).

    §166:  17 `.cc` files
    now:   11
