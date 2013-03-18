var gtm = require('../lib/mumps');

var db = new gtm.Gtm();

db.open();

var get = db.get({global: 'dlw', subscripts: ['testing', 1]});
console.log(get);
console.log();

var func1 = db.function({function: 'version^%zewdAPI'});
console.log(func1);
console.log();

var func2 = db.function({function: 'version^node'});
console.log(func2);

db.close();
