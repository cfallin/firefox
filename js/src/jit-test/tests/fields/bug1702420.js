// |jit-test| --more-compartments; skip-if: nightTierEnabled()

a = newGlobal()
b = a.Debugger(this)
function c() {
    b.getNewestFrame().eval("")
}
c()
d = class {
    #e
}