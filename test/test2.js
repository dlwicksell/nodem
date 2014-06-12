var gtm = require('../lib/nodem');
var db = new gtm.Gtm();
db.open();

var get = db.get({global: 'dlw', subscripts: ['testing', 1]});
console.log(JSON.stringify(get));

var func1 = db.function({function: 'version^%zewdAPI'});
console.log(JSON.stringify(func1));

var func2 = db.function({function: 'version^v4wNode'});
console.log(JSON.stringify(func2));

db.close();
