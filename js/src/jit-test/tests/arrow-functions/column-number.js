// |jit-test| skip-if: nightTierEnabled()
function f() { return g(abcd => Error()); }
function g(x) { return x(); }
var err = f(1, 2);
var lines = err.stack.split("\n");
assertEq(lines[0].endsWith(":2:33"), true);
assertEq(lines[1].endsWith(":3:24"), true);