
function do_concat(val) {
    for (let i = 0; i < 100; i++) {
        val += "X";
    }
    
    return val;
}

function test() {
    for (let i = 0; i < 100; i++) {
      let result = do_concat("X");
    }
}
test();
