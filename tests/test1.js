var gtm = require("../lib/mumps");

var db = new gtm.Gtm();

db.open();

var node;

console.log('Testing the set command, starting at: ' + Date());

for (var i=0;i<1000000;i++) {

  node = {global: 'dlw', subscripts: ["testing", i], data: 'record ' + i};

  db.set(node);
}

db.close()

console.log('Set a million nodes at the ^dlw("testing") global node, ending at: ' + Date());
