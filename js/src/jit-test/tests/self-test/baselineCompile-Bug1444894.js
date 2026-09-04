// |jit-test| skip-if: nightTierEnabled()

if (typeof baselineCompile == "function") {
    gc();
    newGlobal().baselineCompile();
}
