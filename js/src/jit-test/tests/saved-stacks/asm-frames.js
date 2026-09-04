// |jit-test| skip-if: nightTierEnabled()
function AsmModule(stdlib, foreign, heap) {
  "use asm";
  var ffi = foreign.t;

  function doTest() {
    ffi();
  }

  function test() {
    doTest();
  }

  return { test: test };
}

let stack;

function tester() {
  stack = saveStack();
}

const buf = new ArrayBuffer(1024*8);
const module = AsmModule(this, { t: tester }, buf);
module.test();

print(stack);
assertEq(stack.functionDisplayName, "tester");

assertEq(stack.parent.functionDisplayName, "doTest");
assertEq(stack.parent.line, 7);

assertEq(stack.parent.parent.functionDisplayName, "test");
assertEq(stack.parent.parent.line, 11);

assertEq(stack.parent.parent.parent.line, 25);

assertEq(stack.parent.parent.parent.parent, null);
