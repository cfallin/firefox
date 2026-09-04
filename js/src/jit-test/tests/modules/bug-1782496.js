// |jit-test| exitstatus: 6; allow-overrecursed; skip-if: nightTierEnabled()

setInterruptCallback(function() {
  import("javascript:null");
  interruptIf(true);
});

interruptIf(true);
for (;;) {}  // Wait for interrupt.
