// A ZWRITE clone - Pass the name of your global as the first argument

var mumps = require('../lib/nodem');
var db = new mumps.Gtm();

db.open();

var global = process.argv[2] || 'dlw';
var node = {};
var subscripts = [];
var data, i;

while (true) {
  node = db.next_node({global: global, subscripts: node.subscripts});

  if (node.subscripts === undefined) { break; }

  subscripts = node.subscripts;
  data = node.data;

  console.log('^' + global + '(' +
              JSON.stringify(subscripts)
              .replace('[', '')
              .replace(']', '') +
              ')=' + JSON.stringify(data));
}

db.close();
