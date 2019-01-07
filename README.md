[![NPM Info][info-image]][npm-url]

[![Node Version][version-image]][npm-url]
[![NPM License][license-image]][npm-url]
[![NPM Downloads][downloads-image]][npm-url]

# NodeM #

## A YottaDB and GT.M database driver and language binding for Node.js ##

Version 0.13.4 - 2019 Jan 6

## Copyright and License ##

Addon Module written and maintained by David Wicksell <dlw@linux.com>  
Copyright © 2012-2019 Fourth Watch Software LC

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU Affero General Public License (AGPL) as published by the
Free Software Foundation, either version 3 of the License, or (at your option)
any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>.

Full license text: [AGPL-3.0][license]

Contact me if you are interested in using Nodem with a different license.

## Summary and Info ##

Nodem is an open source addon module that integrates [Node.js][] with the
[YottaDB][] and [GT.M][] implementations of M[UMPS], providing in-process access
to their database systems and some language features. Nodem provides access to
the basic global database handling operations, as well as providing the ability
to invoke M language functions and procedures. It also supports full local
symbol table management. Although designed for use with YottaDB and GT.M, Nodem
aims to be API-compatible (while in 'strict' mode) with the in-process Node.js
interface for [Caché][].

All of Nodem's APIs support synchronous operation and accept arguments passed
via a single JavaScript object, containing a few specified properties. The APIs
that currently support both synchronous and asynchronous operation, as well as
accepting arguments passed by-position, are: data, get, kill, nextNode, order,
previous, previousNode, and set. In order to use the asynchronous versions of
those APIs, you must pass a JavaScript function, taking two arguments - error
and result, as the last argument to the normal synchronous APIs. When passing
arguments to those APIs by-position, the first argument would be the global,
local, or intrinsic special variable (only supported in the get and set APIs)
string, and the next set of arguments would be each subscript, separated as a
different argument. In order to specify an intrinsic special variable when
passing arguments inside of a JavaScript object, use the local property, and
preface the name with a $. For the set API, the last non-function argument would
be treated as the data to set in to the node. Asynchronous, and call
by-position, support for the rest of the API is coming soon.

Nodem uses the YottaDB and GT.M C call-in interface. YottaDB has released a new,
faster, low-level database access API, with version r1.20, called the SimpleAPI.
Nodem uses YottaDB's SimpleAPI for the data, get, kill, nextNode, order,
previous, previousNode, and set APIs, when it is available, and falls back to
the call-in interface when it is not. SimpleAPI support for the rest of the API
is coming soon.

**NOTE:** Nodem Developer API and User Guide coming soon. In the meantime,
please refer to the Caché Node.js [API][BXJS] documentation for further details.

## Example Usage ##

Using Nodem with YottaDB, in the Node.js REPL:

```javascript
> const ydb = require('nodem').Ydb(); // Create YottaDB connection instance - use Gtm() when connecting to GT.M
undefined
> ydb.open(); // Open connection to YottaDB
{ ok: true, pid: 15975 }
> ydb.version();
'Node.js Adaptor for YottaDB: Version: 0.13.4 (FWS); YottaDB version: 1.22'
> ydb.get({global: 'test', subscripts: [0, 2, 0]}); // write ^test(0,2,0)
{ ok: true,
  global: 'test',
  subscripts: [ 0, 2, 0 ],
  data: '2 bags of wheat',
  defined: 1 }
> ydb.get('^test', 0, 2, 0); // write ^test(0,2,0)
'2 bags of wheat'
> ydb.get({global: 'test', subscripts: [0, 2, 0]}, (error, result) => {if (!error) {console.log('result:', result);}});
undefined
> result: { ok: true,
  global: 'test',
  subscripts: [ 0, 2, 0 ],
  data: '2 bags of wheat',
  defined: 1 }

> ydb.get('^test', 0, 2, 0, (error, result) => {if (!error) {console.log('result:', result);}});
undefined
> result: 2 bags of wheat

> ydb.set('^test', 0, 2, 0, '3 bags of wheat'); // set ^test(0,2,0)="3 bags of wheat"
undefined
> ydb.get({global: 'test', subscripts: [0, 2, 0]});
{ ok: true,
  global: 'test',
  subscripts: [ 0, 2, 0 ],
  data: '3 bags of wheat',
  defined: 1 }
> ydb.get({global: 'test', subscripts: ['']});
{ ok: false,
  errorCode: 150373498,
  errorMessage:
   '(SimpleAPI),%YDB-E-NULSUBSC, DB access failed because Null subscripts are not allowed for current region,%YDB-I-GVIS, \t\tGlobal variable: ^test("")' }
> ydb.close(); // Close connection to YottaDB, releasing resources and restoring terminal settings
true
```

## Installation ##

Nodem should run on every version of Node.js starting with version 0.12.0,
through the current release (v11.6.0 at this time), as well as every version of
IO.js. However, in the future, both Node.js and the V8 JavaScript engine at its
core, could change their APIs in a non-backwards-compatible way, which might
break Nodem for that version.

In order to use the Nodem addon, you will need to have YottaDB or GT.M installed
and configured correctly, including setting up your environment with the
required YottaDB/GT.M environment variables. Make sure you have either $ydb_dist
(only applicable for YottaDB) or $gtm_dist set to the root of the YottaDB/GT.M
instance before you compile Nodem. You will also need to have Node.js installed
and working.

**ATTENTION:** These instructions assume that the nodem repository has been
installed in your home directory. The paths will likely be different if you have
installed this with npm.

**NOTE:** If you have installed Nodem using npm, it will attempt to build
mumps.node during installation. If there is a file in the nodem/ directory
called builderror.log, and if that file contains no build errors for mumps.node,
it built without issue. It also attempts to pre-compile the v4wNode.m
integration routine, and there might be warnings from that, which won't affect
the building of mumps.node itself. If you downloaded Nodem any other way,
including cloning it from its github repository, then you'll have to build it
from source. Remember to make sure that either $ydb_dist or $gtm_dist is set to
the root of the YottaDB/GT.M instance before building Nodem. In order to build
it, while in the root of the Nodem repository, run the 'npm install' command,
e.g.

```bash
$ cd ~/nodem
$ npm install
```
or
```bash
$ node-gyp rebuild 2> builderror.log
```

In addition you will need to set a few environment variables in order for
YottaDB/GT.M to find the call-in table and the M routine that it maps to. The
Nodem package supplies a sample environment file, called environ. It has a
commented out command to set $LD_LIBRARY_PATH to $gtm_dist, which you will need
to uncomment if you need it. It is located in ~/nodem/resources/ and can be
sourced into your working environment, either directly, or from your own
environment scripts or profile/login script, e.g.

```bash
$ cd ~/nodem/resources/
$ source environ
```
or
```bash
$ echo "source ~/nodem/resources/environ" >> ~/.profile
```

If you don't source the environ file, then you will need to put a copy of
v4wNode.m into a directory that is specified in your $gtmroutines routines path,
or in the routinesPath property in your call to the open API, so that
YottaDB/GT.M can find it. It is located in the ~/nodem/src/ directory. Again, if
you don't source the environ file, then you will also need to define the $GTMCI
environment variable, or set the callinTable property in your call to the open
API, and point it at the file nodem.ci, located in the ~/nodem/resources/
directory, e.g.

```bash
$ export GTMCI=~/nodem/resources/nodem.ci
$ cp ~/nodem/src/v4wNode.m ~/p/
```
or
```javascript
> const callinTable = '/home/dlw/nodem/resources/nodem.ci';
> const routinesPath = 'home/dlw/nodem/src'; // Make sure to include your routine directories
> ydb.open({callinTable: callinTable, routinesPath: routinesPath});
```

You can clone the repository with this command..

```bash
$ git clone https://github.com/dlwicksell/nodem.git
```

You can also install it via npm with this command..

```bash
$ npm install nodem
```

You can update to the latest version with this command..

```bash
$ npm update nodem
```

## Important Notes ##

The open() call works a bit differently than the Caché version; it does not
require any arguments, and will connect with the database specified in the
environment variable $ydb_gbldir (or $gtmgbldir). If you have more than one
database and would like to connect to a different one than what is defined in
your environment, you can pass an object, with a property called either
globalDirectory or namespace, defined as the path to your global directory file
for that database, e.g.

```javascript
> ydb.open({globalDirectory: '/home/dlw/g/db_utf.gld'});
```

Nodem supports setting up a custom routines path, for resolving calls to other M
functions and procedures, via the routinesPath property. Make sure that one of
the directories in the routinesPath contains the v4wNode.m routine, located in
the Nodem src/ directory, or its compiled object, v4wNode.o, otherwise Nodem
will not be fully functional. This could be used to provide a certain level of
security, by giving access only to certain routines, within a Nodem process,
within an environment that contains routines with unfettered access to the
system in its default environment configuration, e.g.

```javascript
> ydb.open({routinesPath: '/home/dlw/p/r120(/home/dlw/p)'});
```

Nodem supports setting the call-in path directly in the open call, via the
callinTable property. This can be handy if you are running Nodem in an
environment that has other software that uses the YottaDB/GT.M call-in
interface, and you don't want to worry about namespace issues. Nor would you
need to set the $GTMCI environment variable, in order for NodeM to be fully
functional, e.g.

```javascript
> ydb.open({callinTable: '/home/dlw/nodem/resources/nodem.ci'});
```

You can configure Nodem to function as a [GT.CM][] client, allowing Nodem to
connect with a remote database. In the open() method, you can set an ipAddress,
and/or a tcpPort property, and Nodem will set up the environment to connect with
a YottaDB/GT.M database on a remote server that already has a GT.CM server
running on that address and port. If only ipAddress or tcpPort is defined, the
other one will be set with a default value; 127.0.0.1 for ipAddress, or 6879 for
tcpPort. Nodem will then set the $GTCM_NODEM environment variable, for that
Nodem process only, with the address and port you set in the open() call, e.g.

```javascript
> ydb.open({ipAddress: '127.0.0.1', tcpPort: 6879});
```

You will also need to create a global directory file that maps one or more
database segments to a data file on the remote server you want to connect with,
noting that the prefix to the -file argument in the example below must be NODEM,
in order to match the $GTCM_NODEM environment variable name that Nodem sets up
for you, e.g.

```bash
$ mumps -run GDE
GDE> change -segment DEFAULT -file=NODEM:/home/dlw/g/gtcm-server.dat
```

Then on the server you are connecting to, make sure you have the data file set
up at the same path that you set the '-file=' option to in the global directory
of your GT.CM client configuration, and have started the GT.CM server on the
same ipAddress and tcpPort that you configured in the open() call in Nodem, e.g.

```bash
$ $ydb_dist/gtcm_gnp_server -log=gtcm.log -service=127.0.0.1:6879
```

**NOTE:** GT.CM only allows remote connections for the database access APIs, not
the function or procedure APIs. So while using Nodem in a remote GT.CM
configuration, any calls to the function or procedure APIs will result in local
calls, not remote calls.

Nodem supports two different character encodings, UTF-8 and M (or binary). It
defaults to UTF-8 mode. The character encoding you set in Nodem is decoupled
from the underlying character encoding you have set up for the YottaDB/GT.M
environment it is running in. So it is possible to work with UTF-8 encoded data
in the database, while in Nodem, even if you haven't set up YottaDB/GT.M to work
with UTF-8 directly. You can set it to UTF-8 mode directly by passing utf-8 or
utf8, case insensitively, to the charset property. If you'd rather work with an
older byte-encoding scheme, that represents all characters in a single byte, you
can set charset to either m, ascii, or binary, case insensitively. One thing to
keep in mind when you do so, is that Node.js internally represents data in
UTF-16, but interprets data in UTF-8 in most cases. You can control this through
the process stream encoding methods inside of your Node.js code. Call those
methods to change the encoding to 'binary' or 'ascii', and it will interpret
your data as a byte encoding, using the character glyphs in your current locale,
e.g.

```javascript
> process.stdin.setEncoding('binary');
> process.stdout.setDefaultEncoding('binary');
> ydb.open({charset: 'm'});
```

There are currently two different data modes that Nodem supports. The mode can
be set to either 'canonical' or 'strict'. The default is canonical, and
interprets data using the M canonical representation. I.e. Numbers will be
represented numerically, etc. Strict mode interprets all data as strings,
strictly following the convention set with InterSystems' Node.js driver, e.g.

```javascript
> ydb.open({mode: 'strict'});
```

Nodem also has a debug tracing mode, in case something doesn't seem to be
working right. It has four levels of debugging, defaulting to 'off'. The other
debug levels are 'low', 'medium', and 'high'. The higher the debug level, the
more verbose the debug output will be, e.g.

```javascript
> ydb.open({debug: 'low'});
```

Nodem handles several common signals by default, closing the database connection
and stopping the Node.js process. These signals include SIGINT, SIGTERM, and
SIGQUIT. The handling of the SIGQUIT signal will also generate a core dump of
the process. All three signal handlers are on by default. However, you can turn
the signal handling on or off directly, via passing true or false to a
signalHandler object (with properties for each of the signals) for each
individual signal, or all of them at once, e.g.

```javascript
> ydb.open({signalHandler: {sigint: true, sigterm: false, sigquit: false}});
```
or
```javascript
> ydb.open({signalHandler: false});
```

Nodem supports a feature called auto-relink, which will automatically relink a
routine object containing any function or procedure called by the function or
procedure API. By default auto-relink is off. You can enable it in one of three
ways. First, you can pass it as a property of the JavaScript object argument
which is passed to the function (or procedure) API directly, with a value of
true, or any non-zero number. This will turn on auto-relink just for that call.
You can also disable it, by setting autoRelink to false, or 0, if it was already
enabled by one of the global settings, e.g.

```javascript
> ydb.function({function: 'version^v4wTest', autoRelink: true});
```

Second, you can enable it globally, for every call to the function (or
procedure) API, by setting the same property in a JavaScript object passed to
the open API, e.g.

```javascript
> ydb.open({autoRelink: true});
```

Third, you can also enable it globally, by setting the environment variable
NODEM_AUTO_RELINK to 1, or any other non-zero number, e.g.

```bash
$ export NODEM_AUTO_RELINK=1
$ node function.js
```
or
```bash
$ NODEM_AUTO_RELINK=1 node function.js
```

YottaDB/GT.M changes some settings of its controlling terminal device, and Nodem
resets them when it closes the database connection. By default, Nodem will
restore the terminal device to the state it was in when the open() call was
invoked. Normally this is the desired option, however, if you wish to reset the
terminal to typically sane settings, the close() call allows this by setting the
resetTerminal property to true, or any non-zero number, e.g.

```javascript
> ydb.close({resetTerminal: true});
```

Nodem has a procedure or routine API, which is similar to the function API,
except that it is used to call M procedures or subroutines, which do not return
any values. The procedure API accepts one argument, a JavaScript object. The
object must contain the required procedure/routine property, set to the name of
the procedure/routine. It may also contain an optional property, called
arguments, which is an array of arguments to pass to the procedure/routine. It
also supports the autoRelink option, just as described in the function API, e.g.

```javascript
> ydb.procedure({procedure: 'set^v4wTest', arguments: ['dlw', 5]});
```

The lock API takes an optional timeout argument. If you do not set a timeout, it
will wait to acquire the lock indefinitely. If you wish to come back from the
call right away, if the lock is not available, simply pass a timeout argument of
0, e.g.

```javascript
> ydb.lock({global: 'dlw', timeout: 5});
```
or
```javascript
> ydb.lock({global: 'dlw', timeout: 0});
```

## Additional Features ##

Nodem provides a built-in API usage help menu. By calling the help method
without an argument, Nodem will display a list of APIs and a short description
of what they do. Calling the help method with an argument string of one of those
APIs will display more in-depth usage information for that method.

Nodem supports full local symbol table manipulation with the current database
APIs. In order to use it, instead of defining a 'global' property in your
argument object, you define a 'local' property. For APIs that support passing
arguments by-position, you signify that you want them to work on a global, by
using a '^' as the first character of the first argument. For the get and set
APIs, if the first character of the first argument is a '$', then you are
signifying that you are working with a intrinsic special variable (ISV). There
is also a localDirectory API, that works the same way as the globalDirectory
API, except that it lists the local symbols in the symbol table, rather than the
globals in the database. One caveat is that you cannot manipulate any local
variable that begins with 'v4w', as Nodem internally uses that namespace to
implement the v4wNode.m integration routine. You can also call the kill API with
no arguments, and it will clear the local symbol table. This functionality will
allow you to call legacy M functions and procedures, without having to write
wrappers in M anymore, in most cases. Here is an example of using the local
symbol table functionality to call a legacy API directly from Nodem. In this
example, the local variable, U, needs to be set before this API is called, as it
expects it to be defined already. You can also see how the local symbol table
changes, after setting the required local variable, making the call, and then
clearing the symbol table, e.g.

```javascript
> ydb.localDirectory();
[]
> ydb.set({local: 'U', data: '^'});
{ ok: true, local: 'U', data: '^' }
> ydb.localDirectory();
[ 'U' ]
> ydb.procedure({procedure: 'AGET^ORWORR', arguments: [, 9, '2^0', 13, 0]});
{ ok: true,
  procedure: 'AGET^ORWORR',
  arguments: [ <1 empty item>, 9, '2^0', 13, 0 ] }
> ydb.localDirectory();
[ 'DILOCKTM', 'DISYS', 'DT', 'DTIME', 'DUZ', 'IO', 'U', 'XPARSYS' ]
> ydb.kill();
undefined
> ydb.localDirectory();
[]
```

Nodem supports calling functions and procedures with arguments passed by
reference, or by variable, in addition to the standard passing by value. This
will allow someone who needs to interface Nodem with legacy M APIs that require
using local variables in this manner, the ability to do so directly in Nodem,
rather than having to write an M wrapper around the API, and calling that from
Nodem. In order to use this functionality, you need to pass your arguments via
a specially formatted object, in order to instruct Nodem that you wish to pass
arguments differently than normal. This is necessary because if you tried to
pass an argument by reference or by variable directly, Node.js will try to
dereference it as a local JavaScript variable, and you would never be able to
refer to the right symbol in the back-end database environment. The structure
of the specially formatted object is simple. It contains a 'type' property,
which can be one of three values: 'reference', 'variable', or 'value'; and it
also contains a 'value' property which contains the name you want to use when
the type is 'reference' or 'variable', and the actual data you want to pass if
type is 'value'. The 'value' type is there for consistency, but you would
normally just pass arguments by value directly, without resorting to this
specially formatted argument object. Here is an example of how you could use
this functionality, while calling a legacy M API, many of which require passing
arguments in this fashion, e.g.

```javascript
> ydb.set({local: 'U', data: '^'});
{ ok: true, local: 'U', data: '^' }
> const arg = {type: 'reference', value: 'LIST'};
undefined
> ydb.procedure({procedure: 'LISTALL^ORWPT', arguments: [arg, 'A', 1]});
{ ok: true,
  procedure: 'LISTALL^ORWPT',
  arguments: [ { type: 'reference', value: 'LIST' }, 'A', 1 ] }
> ydb.localDirectory();
[ 'LIST', 'U' ]
> ydb.data({local: 'LIST'});
{ ok: true, local: 'LIST', defined: 10 }
> ydb.nextNode({local: 'LIST'});
{ ok: true,
  local: 'LIST',
  subscripts: [ 1 ],
  data: '1^ZZ PATIENT,TEST ONE^^^^ZZ PATIENT,TEST ONE',
  defined: 1 }
> ydb.nextNode({local: 'LIST', subscripts: [1]});
{ ok: true,
  local: 'LIST',
  subscripts: [ 2 ],
  data: '3^ZZ PATIENT,TEST THREE^^^^ZZ PATIENT,TEST THREE',
  defined: 1 }
> ydb.nextNode({local: 'LIST', subscripts: [2]});
{ ok: true,
  local: 'LIST',
  subscripts: [ 3 ],
  data: '2^ZZ PATIENT,TEST TWO^^^^ZZ PATIENT,TEST TWO',
  defined: 1 }
> ydb.nextNode({local: 'LIST', subscripts: [3]});
{ ok: true, local: 'LIST', defined: 0 }
```

## Interface ##

API                      | Description
-------------------------|--------------------------------------------------------------------------------------------------------
*close*                  | Close the database connection
*data*                   | Determine whether a global or local node has data and/or children
*function*               | Call an extrinsic function
*get*                    | Retrieve the value of a global, local, or intrinsic special variable node
*globalDirectory*        | List the names of the globals in the database
*help*                   | Display a help menu of method usage
*increment*              | Atomically increment the value stored in a global or local node
*kill*                   | Delete a global or local node, and all of its children; or kill all variables in the local symbol table
*localDirectory*         | List the names of the variables in the local symbol table
*lock*                   | Lock a global or global node, or local or local node, incrementally
*merge*                  | Merge a global or local tree/sub-tree, or data node, to a global or local tree/sub-tree, or data node
*nextNode*               | Retrieve the next global or local node, regardless of subscript level
*open*                   | Open the database connection
*order* or *next*        | Retrieve the next global or local node, at the current subscript level
*previous*               | Same as order, only in reverse
*previousNode*           | Same as nextNode, only in reverse in YottaDB r1.10 or newer, otherwise not yet implemented
*procedure* or *routine* | Call a procedure/routine/subroutine
*retrieve*               | Not yet implemented
*set*                    | Set a global, local, or intrinsic special variable node, to a new value
*unlock*                 | Unlock a global or global node, or local or local node, incrementally; or release all locks
*update*                 | Not yet implemented
*version* or *about*     | Display version information; if database connection open, display its version

## Disclaimer ##

Fourth Watch Software endeavors not to make any breaking changes to APIs, but as
Nodem is still in development, its interface may change in future versions.

## Contact Info ##

If you have any questions or feature requests, email me at <dlw@linux.com>.  
If you want to report any issues, visit <https://github.com/dlwicksell/nodem/issues>.

## See Also ##

* The [Node.js][] server-side JavaScript runtime.
* The [YottaDB][] implementation of M[UMPS].
* The [GT.M][] implementation of M[UMPS].

[info-image]: https://nodei.co/npm/nodem.png?downloads=true&downloadRank=true&stars=true
[version-image]: https://img.shields.io/node/v/nodem.svg
[license-image]: https://img.shields.io/npm/l/nodem.svg?colorB=blue
[downloads-image]: https://img.shields.io/npm/dm/nodem.svg?colorB=orange
[npm-url]: https://npmjs.org/package/nodem
[license]: https://github.com/dlwicksell/nodem/blob/HEAD/COPYING

[Node.js]: https://nodejs.org/
[YottaDB]: https://yottadb.com/
[GT.M]: https://www.fisglobal.com/solutions/banking-and-wealth/services/database-engine
[Caché]: https://www.intersystems.com/products/cache/
[BXJS]: https://docs.intersystems.com/documentation/cache/20181/pdfs/BXJS.pdf
[GT.CM]: https://docs.yottadb.com/AdminOpsGuide/gtcm.html
