var gtm = require("../lib/mumps");

var db = new gtm.Gtm();

db.open();

var node;
var ret;

console.log('Testing the set command, starting at: ' + Date());

for (var i = 0; i < 1000000; i++) {

  node = {global: 'dlw', subscripts: ["testing", i], data: 'record ' + i};

  ret = db.set(node);

  if (ret.ok == 0) {
    break;
  }
}

db.close()

if (ret.ok == 1) {
  console.log('Set a million nodes in ^dlw("testing"), ending at: ' + Date());
} else {
  console.log('There was an error: ' + ret.errorCode + ' ' + ret.errorMessage);
}
