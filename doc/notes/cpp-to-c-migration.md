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

**Kernel fault #2 — CR3 points at an empty top-level page table (ROOT-CAUSED, NOT fixed).**
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
