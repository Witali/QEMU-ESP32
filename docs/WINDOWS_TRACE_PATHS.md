# Native Windows trace generator paths

An isolated fresh QEMU build exposed two path bugs with native Windows Python
and Meson. QAPI trace group names used `full_path().split('/')`, which retained
the entire `C:\...` path when Meson returned backslashes. Generated C expected
the basename and could not include its trace header. `fs.name(item)` handles the
native path before converting it to the identifier.

Once that was corrected, the log backend emitted unescaped backslashes in C
`#line` strings. GCC rejected `\q`; valid escapes such as `\t` can also silently
change `__FILE__`. A shared `c_filename()` encoder now escapes input/output
filenames. The log, syslog and ftrace backends all use it. Simulation behavior
and the optional runtime debug logging setting are unchanged by these fixes.

`tests/test_tracetool_paths.py` exercises actual generated directives with
backslashes and quotes. It compiles C under `-Werror`, executes the programs,
and checks every `__FILE__` result. Six normal cases pass; six variants with
escaping disabled are rejected. This checks C preprocessing/runtime behavior,
not only a Python reimplementation of string escaping.

```text
python -B tests/test_tracetool_paths.py --cc gcc --output build/trace-path-check
python -B tools/verify_trace_path_evidence.py --proof docs/validation/windows-trace-paths-20261024.json
```

The preserved run used MSYS2 GCC16.1.0, Meson1.5.0/native Python3.14.6 for the
fresh build, and Python3.12 for the standalone C regression. Both RISC-V and
Xtensa executables built successfully and ran `--version`/`-machine help`.
SDL, SLIRP, pixman and gcrypt are enabled; the ESP debug flag is off. The
offline configuration uses `--enable-fdt=internal --disable-download`; dtc and
keycodemapdb were copied from the already populated local source cache. Initial
missing-fdt configuration failures are archived and are not successful builds.

Evidence: [windows-trace-paths-20261024.json](validation/windows-trace-paths-20261024.json).
It retains the exact source, compiler reports, generated C/headers, before/after
failures and final build log in a lossless compressed payload. Baseline binaries
are retained under the task worktree's ignored `build/baseline-*.exe` for the
subsequent radio comparison; preserve them before removing this worktree.
The verifier checks their exact hashes as well. Later source changes can be
checked with `--source-revision PRODUCER_COMMIT` without a checkout/reset.

This checkpoint proves the build/path fix. No firmware guest or physical board
was run here. CYD/C3/C2 boot, network and multi-client acceptance remain separate
checks. Radio/NOR/cache model sources match parent0860e568d4 at this checkpoint.
