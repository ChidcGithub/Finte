# Linux x86_64 persona

Finte's compatibility strategy is a **personality**, not a second kernel.

A flow marked `FINTE_PERSONA_LINUX` receives Linux syscall numbers and Linux-shaped results. The persona translates each call into Finte verbs, then the core validates capabilities as usual. The core itself has no `open`, no `read`, no paths and no errno.

```
Linux app / guest
      |  syscall number + args
finte_linux_call
      |  fd -> cap, path -> dir entry, mmap -> arena
finte_syscall
      |  rights check
channel / arena / file / console
```

This is why the core can stay small: compatibility cost is only paid by persona flows.

## Enabling the persona

```c
fhandle_t fh = finte_flow_create("guest", &guest_program, 8192,
                                 FINTE_PERSONA_LINUX);
fflow_t *f = finte_flow_lookup(fh);

int console = finte_cap_install(f, console_handle, FINTE_R_WRITE | FINTE_R_READ);
int dir     = finte_cap_install(f, dir_handle, FINTE_R_READ);
int arena   = finte_cap_install(f, arena_handle, FINTE_R_WRITE);

finte_linux_bootstrap(f, console, dir, arena);
finte_flow_start(fh);
```

`finte_linux_bootstrap` validates the three capabilities, initializes fds 0/1/2 to the console cap, and stores the hidden dir/arena cap indices. `openat` later installs file caps and returns fd numbers >= 3.

The same setup is shared by the host simulator (`finte compat`) and the kernel (`finte_compat_bootstrap`).

## Supported syscalls

| number | name | status | notes |
| ---: | --- | --- | --- |
| 0 | `read` | yes | console returns 0; files read at the fd position |
| 1 | `write` | yes | console only; file fds return `EROFS` |
| 2 | `open` | subset | read-only, flat RAMFS |
| 3 | `close` | yes | drops file caps; stdio fds are marked closed |
| 4 | `stat` | subset | fills `flinux_stat_t` |
| 5 | `fstat` | subset | fills `flinux_stat_t` |
| 6 | `lstat` | subset | same as `stat`; no symlinks |
| 8 | `lseek` | yes | `SET`/`CUR`/`END` |
| 9 | `mmap` | subset | anonymous from arena; file mapping returns a read pointer |
| 10 | `mprotect` | no-op | protection is not enforced yet |
| 11 | `munmap` | no-op | arena memory is not individually unmapped |
| 12 | `brk` | subset | within the current arena chunk |
| 13 | `rt_sigaction` | no-op | no signal delivery |
| 16 | `ioctl` | `ENOSYS` | no device multiplexer |
| 17 | `pread64` | yes | does not move the fd position |
| 18 | `pwrite64` | `EROFS` | read-only RAMFS |
| 19 | `readv` | yes | iterates iovecs |
| 20 | `writev` | yes | iterates iovecs |
| 21 | `access` | subset | read/execute checks; `W_OK` returns `EROFS` |
| 24 | `sched_yield` | yes | cooperative yield when called by a running flow |
| 32 | `dup` | yes | duplicates the fd's capability |
| 33 | `dup2` | yes | replaces a target fd capability |
| 35 | `nanosleep` | yes | cooperative sleep |
| 39 | `getpid` | yes | flow slot + 1 |
| 60 | `exit` | yes | terminates the flow |
| 63 | `uname` | yes | reports `Linux` / `finte` / `x86_64` for compatibility |
| 72 | `fcntl` | no-op | returns 0 |
| 79 | `getcwd` | yes | always `/` |
| 89 | `readlink` | `EINVAL` | no symlinks |
| 96 | `gettimeofday` | yes | nanoseconds since HAL boot |
| 102/104 | `getuid`/`getgid` | yes | always 0; no uid model |
| 110 | `getppid` | yes | always 0; no process tree |
| 158 | `arch_prctl` | no-op | no FS/GS TLS setup |
| 186 | `gettid` | yes | flow slot + 1 |
| 204 | `sched_getaffinity` | subset | one CPU, mask bit 0 |
| 218 | `set_tid_address` | yes | returns the flow id |
| 228 | `clock_gettime` | yes | `REALTIME` and `MONOTONIC` both use boot time |
| 229 | `clock_getres` | yes | reports 1 ns |
| 231 | `exit_group` | yes | same as `exit` |
| 257 | `openat` | subset | read-only, flat RAMFS |
| 262 | `newfstatat` | subset | resolves within the root dir |
| 302 | `prlimit64` | no-op | returns 0; no resource limits |
| 318 | `getrandom` | yes | xorshift from the core RNG |

Unsupported numbers return `-ENOSYS`.

The authoritative constants are in `src/include/finte/compat_linux.h`; the implementation is `src/core/compat_linux.c`.

## Return convention

`finte_linux_call` returns:

- `>= 0` on success (bytes, fd, pid, pointer as `int64_t`)
- `-errno` on failure

Status-to-errno mapping includes `EINVAL`, `ENOENT`, `ENOMEM`, `EPERM`, `EAGAIN`, `ENOSYS`, `EFAULT`, `ERANGE`, `EEXIST`, `EBADF`, `EROFS`, `ENOTDIR`, `EISDIR`.

The core's `fret_t` status/value split is preserved underneath; the persona flattens it into Linux convention at the boundary.

## RAMFS and paths

- A directory node holds a flat list of up to 24 entries.
- File names are at most 23 characters (`FINTE_NAME_MAX - 1`).
- Up to 64 files.
- `openat` strips leading `/`, rejects nested paths, and looks up the name in the flow's root directory cap.
- Files are read-only; `O_WRONLY`, `O_RDWR`, `O_CREAT`, `O_TRUNC` return `EROFS` (or are ignored where harmless).
- fd 0/1/2 all map to the console capability, so stdin reads return 0 and stdout/stderr write.
- `fstat`/`newfstatat` fill Finte's `flinux_stat_t` (size, mode, nlink, block counts), not the exact glibc `struct stat` layout. These structures are a compatibility shape, not a promise of binary compatibility.

## Memory calls

`mmap` has two paths:

1. **Anonymous** (`MAP_PRIVATE | MAP_ANONYMOUS`): allocate a page-aligned chunk from the flow's arena cap, zero it, bump `brk`, return the pointer. The host simulator returns a real host pointer; the kernel returns a kernel-heap pointer (identity mapped). A ring-3 port would return a user virtual address backed by a mapped arena window.
2. **File-backed**: return a pointer into the RAMFS bytes. The mapping is read-only in practice because the file is immutable.

`brk(0)` returns 0 before the first heap allocation. `brk(addr)` only accepts addresses inside the current chunk. A new chunk is allocated when `mmap` needs more room; the break then jumps to the new chunk. This is intentionally simple and not a full demand-paging implementation.

`mprotect` and `munmap` are accepted as no-ops. There is no page table manipulation in the persona yet.

## What is deliberately missing

- **No general ELF loader for the persona.** The guest is linked into the image
  and started as a flow. The separate `user` command loads a minimal static
  ELF64 into ring 3, but that is a demo ABI, not a Linux userspace. There is no
  `execve`, interpreter or dynamic linker.
- **The persona runs in the kernel address space.** Native flows and the Linux
  guest share the kernel mapping. The ring-3 demo has its own single user
  address space, but the Linux persona does not use it yet. Syscalls cannot
  bypass capability checks, but a malicious in-kernel guest could write
  arbitrary memory directly.
- **No fork/clone/threads/futex.** Flow creation is explicit at the native API; the persona does not implement process duplication.
- **No signals.** `rt_sigaction` is accepted but no signal is ever delivered.
- **Read-only persona RAMFS.** The Linux-facing RAMFS has no write path. Disk
  lfc1 files are writable through the native file verbs and shell, but the
  persona does not expose them. There are no device nodes, sockets or ioctl.
- **No TLS.** `arch_prctl` is a no-op; a libc that requires FS-relative TLS will not start.
- **No `struct stat` binary compatibility.** The demo and tests use Finte's structures.

## Security properties

The persona is not a sandbox by itself, but it does not weaken the capability model:

- `openat` needs a `READ` cap on the directory; the resulting file cap has `READ`.
- `write` needs a `WRITE` cap on a console; file fds are read-only.
- `mmap` needs a `WRITE` cap on an arena.
- A guest cannot turn a syscall number into ambient authority; every path ends in `finte_cap_check`.

The missing piece is memory isolation, not syscall mediation. Ring 3 plus per-flow page tables is the next step.

## Adding a syscall

1. Add the number to `src/include/finte/compat_linux.h`.
2. Add a `case` in `src/core/compat_linux.c`.
3. Translate arguments into a native verb or a persona helper.
4. Validate the relevant capability through the core.
5. Return a Linux-shaped result (`>= 0` or `-errno`).
6. Add a test in `src/tests/test_main.c` using a non-running persona flow.

Do not add a syscall-shaped path to the core. If the operation is useful natively, add a verb; if it is naming/ABI policy, keep it in the persona.

## Example guest

The demo guest (`src/core/guest_linux.c`) performs:

```c
int64_t fd = lx(f, FINTE_NR_openat, AT_FDCWD, "/hello.txt", O_RDONLY, ...);
int64_t n  = lx(f, FINTE_NR_read, fd, buf, sizeof(buf), ...);
lx(f, FINTE_NR_write, 1, text, len, ...);
void *p    = (void *)lx(f, FINTE_NR_mmap, 0, 4096, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
lx(f, FINTE_NR_exit, 0, ...);
```

It runs unchanged in the host simulator and in QEMU.
