# IC corpus

This directory is available to include a pre-collected IC corpus which will be
used if `--enable-aot-ics` is configured.

## (Re-)collecting a corpus

The corpus is not checked into upstream because it would change too frequently.
To build a corpus, perform the following steps:

1. Build a PBL-only native SpiderMonkey shell with the following configuration:

```plain
# file mozconfig.pbl.native.release.ics
ac_add_options --enable-project=js
ac_add_options --enable-application=js
ac_add_options --enable-optimize=-O3
ac_add_options --enable-portable-baseline-interp
ac_add_options --enable-portable-baseline-interp-force
ac_add_options --enable-aot-ics
ac_add_options --enable-aot-ics-force
ac_add_options --enable-aot-ics-enforce
ac_add_options --prefix=obj-release/dist
mk_add_options MOZ_OBJDIR=obj-release-ics
```

```shell
MOZCONFIG=mozconfig.pbl.native.release.ics ./mach build
```

2. Run the jit-tests with this shell, collecting IC files:

```shell
AOT_ICS_KEEP_GOING=1 js/src/jit-test/jit_test.py -f obj-release-ics/dist/bin/js
```

3. Remove the existing corpus and move these files into the directory (use
   `find` to avoid "command line too long" errors):

```shell
find js/src/ics/ -name 'IC-*' -exec rm {} \;
find . -maxdepth 1 -name 'IC-*' -exec mv {} js/src/ics/ \;
```

4. Remove duplicates and remove ICs that depend on platform-dependent CacheIR
   opcodes (e.g. those that are disabled on 32-bit targets):

```shell
cd js/src/ics/
./remove-duplicates.py
./remove-platform-dependent-ics.py
```

5. Build a new PBL-based SpiderMonkey with AOT ICs:

```plain
# file mozconfig.pbl.native.release
ac_add_options --enable-project=js
ac_add_options --enable-application=js
ac_add_options --enable-optimize=-O3
ac_add_options --enable-portable-baseline-interp
ac_add_options --enable-portable-baseline-interp-force
ac_add_options --enable-aot-ics
ac_add_options --prefix=obj-release/dist
mk_add_options MOZ_OBJDIR=obj-release
```
