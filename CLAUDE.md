# CLAUDE.md — fbpanel project instructions

## Tool permissions

You are pre-authorized to run any of the following standard Linux development
tools via the Bash tool **without prompting for confirmation**:

### Version control
- `git` (all subcommands)
- `gh` (GitHub CLI — all subcommands: pr, release, run, issue, api, etc.)

### Build tools
- `cmake`, `ccmake`, `ctest`, `cpack`
- `make`, `ninja`, `bear`
- `gcc`, `g++`, `cc`, `c++`, `clang`, `clang++`
- `ar`, `ranlib`, `ld`, `nm`, `objdump`, `readelf`, `objcopy`, `strip`
- `pkg-config`

### Shell / file utilities
- `ls`, `ll`, `la`, `dir`
- `cat`, `head`, `tail`, `less`, `more`
- `grep`, `rg` (ripgrep), `awk`, `sed`, `cut`, `tr`, `sort`, `uniq`, `wc`
- `find`, `locate`, `which`, `type`, `whereis`
- `cp`, `mv`, `mkdir`, `rmdir`, `rm`, `ln`, `touch`, `install`
- `chmod`, `chown`, `chgrp`
- `diff`, `patch`, `cmp`
- `tar`, `zip`, `unzip`, `gzip`, `gunzip`, `bzip2`, `xz`, `zstd`
- `file`, `stat`, `du`, `df`, `lsof`
- `env`, `export`, `printenv`, `echo`, `printf`

### Process / system
- `ps`, `pgrep`, `pkill`, `kill`
- `top`, `htop`, `free`, `uptime`, `uname`
- `ldd`, `ltrace`, `strace`
- `dpkg`, `dpkg-deb`, `apt-cache`, `apt-get` (read-only: show, search, list)

### Scripting / text
- `python3`, `python`, `perl`, `bash`, `sh`, `awk`
- `jq`, `xmllint`, `base64`

### Network (read-only)
- `curl`, `wget` (fetching docs, checking URLs)
- `ping`

## Destructive / irreversible actions — still require confirmation

Even with the above permissions, **always ask before**:
- `git push --force` or `git reset --hard`
- `git branch -D` (deleting branches)
- `rm -rf` on directories outside the build tree
- `sudo` / `su` commands
- Any command that modifies shared infrastructure (CI secrets, org settings)
- Dropping database tables or deleting persistent data

### Analysis / testing utilities
- `valgrind`, `callgrind`, `massif`, `cachegrind`
- `scan-build`, `clang-tidy`, `clang-format`, `cppcheck`
- `gdb`, `cgdb`, `lldb`
- `addr2line`, `gprof`, `perf`
- `splint`, `flawfinder`, `rats`

## Build and compile policy

**Compile locally at will — no confirmation needed.**
Whenever making code changes, after fixing bugs, or to verify correctness,
run a local build immediately without asking.  Use the full cmake reconfigure
+ make pipeline as needed.  Use any of the analysis/testing utilities above
to validate results.  Proactively rebuild after every non-trivial edit.

## Project conventions

- CMake build directory: `build/`
- Build command: `cmake -B build -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib && make -C build -j$(nproc)`
- Packaging: `cd build && cpack -G DEB`
- Main branch: `master`
- Release tags: `vMAJOR.MINOR.PATCH` format
