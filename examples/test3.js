var gtm = require("../lib/mumps");

var db = new gtm.Gtm();

db.open();

var node = {global: 'zewd', subscripts: ['loader']};

var retrieve = function(node) {
  
  var global = node.global;
  var subscripts = JSON.stringify(node.subscripts);
  
  var retrieveData = function(node) {
    console.log("in retrieveData: node = " + JSON.stringify(node));

    var subs = '';
    var data;
    var value;
    var obj = {};

    node.subscripts.push(subs);

    do {
      console.log("calling order: node = " + JSON.stringify(node));

      subs = db.order(node).result;

      console.log("subs = " + subs);

      if (subs !== '') {
        node.subscripts.pop();
        node.subscripts.push(subs);

        data = db.data(node).defined;

        if (data === 10) {
          var newNode = {global: node.global, subscripts: node.subscripts};
          var subObj = retrieveData(newNode);

          obj[subs] = subObj;

          node.subscripts.pop();
        }

        if (data === 1) {
          value = db.get(node).data;

          obj[subs] = value;
        }
      }
    } while (subs !== '')

    return obj;  
  };

    var obj = retrieveData(node);

    return {node: {global: global, subscripts: JSON.parse(subscripts)}, object: obj};
};

var obj = retrieve(node);

console.log("results = " + JSON.stringify(obj));

db.close();
