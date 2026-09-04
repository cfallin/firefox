// |jit-test| skip-if: typeof Intl === 'undefined'
function a(b, c) {
  b.formatToParts(c)
}
d = ["", "B"];
b = new Intl.ListFormat;
a(b, d);
