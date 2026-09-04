// |jit-test| skip-if: nightTierEnabled()
const arr = [];
arr[Symbol.toPrimitive] = quit;
const stack = {stack: saveStack(), cause: arr};
const bound = bindToAsyncStack(function() {}, stack);
bound();
