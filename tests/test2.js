var gtm = require('../lib/mumps');

var db = new gtm.Gtm();

db.open();

var get = db.get({global: 'dlw', subscripts: ['testing', 1]});
console.log(get);

var func1 = db.function({function: 'version^%zewdAPI'});
console.log(func1);

db.close();
