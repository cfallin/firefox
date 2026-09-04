// |jit-test| skip-if: nightTierEnabled()
// (AOT allocation profile shifts a nursery-GC boundary this test counts.)
gczeal(0);
gcparam("minNurseryBytes", 256 * 1024);
gcparam("maxNurseryBytes", 256 * 1024);
gc();

let initialCount = gcparam("minorGCNumber");

let array = new Array(500000);
array.fill(0);
let keys = Object.keys(array);

let count = gcparam("minorGCNumber") - initialCount;
assertEq(count <= 3, true);
