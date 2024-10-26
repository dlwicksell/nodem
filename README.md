[![Node Version][version-image]][npm-url]
[![NPM License][license-image]][npm-url]
[![NPM Downloads][downloads-image]][npm-url]

# NodeM #

## A YottaDB and GT.M database driver and language binding for Node.js ##

Version 0.20.9 - 2024 Oct 26

## Copyright and License ##

Addon module written and maintained by David Wicksell <dlw@linux.com>  
Copyright © 2012-2024 Fourth Watch Software LC

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU Affero General Public License (AGPL) as published by the
Free Software Foundation, either version 3 of the License, or (at your option)
any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>.

Full license text: [AGPL-3.0][license-text]

Contact me if you are interested in using Nodem with a different license.

## Summary and Info ##

Nodem is an open source addon module that integrates [Node.js][] with the
[YottaDB][] and [GT.M][] implementations of M, providing in-process access to
their database systems and some language features, as well as networked access
to their core database functionality. Nodem provides access to the basic global
database handling operations, as well as providing the ability to invoke M
language functions and procedures. It also supports full local symbol table
management and manipulation of the M environment.

All of Nodem's APIs support synchronous operation and accept arguments passed
via a single JavaScript object (except for help, which takes no arguments or an
argument string, and version, which takes no arguments), containing specific
per-API properties, usually `global` or `local`, and `subscripts` or
`arguments`. The APIs that currently support both synchronous and asynchronous
operation, as well as accepting arguments passed by-position (except `version`,
which takes no arguments, and `merge`, which requires passing arguments via a
JavaScript object), are: `version`, `data`, `get`, `set`, `kill`, `merge`,
`order`, `previous`, `nextNode`, `previousNode`, `increment`, `lock`, `unlock`,
`function`, and `procedure`. In order to use the asynchronous versions of those
APIs, you must pass a JavaScript function, taking two arguments - conventionally
`error` and `result` - as the last argument to the API. When passing arguments
to those APIs by-position, the first argument would be the global (prefaced by
`^`), local, or intrinsic special variable string (prefaced by `$` and only
supported in the `get` and `set` APIs), and the next set of arguments would be
each subscript (or function/procedure argument), separated as a different
argument. In order to specify an intrinsic special variable (in the `get` or
`set` API) when passing arguments inside of a JavaScript object, use the `local`
property, and preface the name with a `$`. For the `set` API, the last
non-function argument would be treated as the data to set into the node.
Asynchronous support for the rest of the API (`open`, `close`, `configure`,
`globalDirectory`, and `localDirectory`) is coming soon.

Nodem uses the YottaDB and GT.M C Call-in interface. YottaDB has released a
faster, low-level database access API, with version r1.20, called the SimpleAPI.
Nodem uses YottaDB's SimpleAPI for the `data`, `get`, `set`, `kill`, `order`,
`previous`, `nextNode`, `previousNode`, `increment`, `lock`, and `unlock` APIs,
when it is available, and falls back to the Call-in interface when it is not.

YottaDB, LLC. has created extensive documentation for [Nodem][].

**NOTE:** The Nodem Developer API and User Guide [Wiki][] is in development.

## Example Usage ##

Using Nodem with YottaDB, in the Node.js REPL:

```javascript
> const ydb = require('nodem').Ydb(); // Create YottaDB connection instance - use Gtm() when connecting to GT.M
undefined
> ydb.open(); // Open connection to YottaDB
{ ok: true, pid: 12345, tid: 12345 }
> ydb.version();
'Node.js Adaptor for YottaDB: Version: 0.20.9 (ABI=131) [FWS]; YottaDB Version: 2.00'
> ydb.get({global: 'v4wTest', subscripts: [0, 2, 0]}); // write ^v4wTest(0,2,0)
{
  ok: true,
  global: 'v4wTest',
  subscripts: [ 0, 2, 0 ],
  data: '2 bags of wheat',
  defined: true
}
> ydb.get('^v4wTest', 0, 2, 0); // write ^v4wTest(0,2,0)
'2 bags of wheat'
> ydb.get({global: 'v4wTest', subscripts: [0, 2, 0]}, (error, result) => {if (!error) {console.log('result:', result);}});
undefined
> result: {
  ok: true,
  global: 'v4wTest',
  subscripts: [ 0, 2, 0 ],
  data: '2 bags of wheat',
  defined: true
}

> ydb.get('^v4wTest', 0, 2, 0, (error, result) => {if (!error) {console.log('result:', result);}});
undefined
> result: 2 bags of wheat

> ydb.set('^v4wTest', 0, 2, 0, '3 bags of wheat'); // set ^v4wTest(0,2,0)="3 bags of wheat"
true
> ydb.get({global: 'v4wTest', subscripts: [0, 2, 0]});
{
  ok: true,
  global: 'v4wTest',
  subscripts: [ 0, 2, 0 ],
  data: '3 bags of wheat',
  defined: true
}
> ydb.get({global: 'v4wTest', subscripts: ['']});
{
  ok: false,
  errorCode: 150373498,
  errorMessage:
   '(SimpleAPI),%YDB-E-NULSUBSC, DB access failed because Null subscripts are not allowed for current region,%YDB-I-GVIS, Global variable: ^v4wTest("")'
}
> ydb.close(); // Close connection to YottaDB, releasing resources and restoring terminal settings
undefined
```

## Installation ##

Nodem should run on every version of Node.js starting with version 0.12.0,
through the current release (v23.1.0 at this time), as well as every version of
IO.js. However, in the future, both Node.js and the V8 JavaScript engine at its
core, could change their APIs in a non-backwards compatible way, which might
break Nodem for that version.

In order to use the Nodem addon, you will need to have YottaDB (or GT.M)
installed and [configured][get-started] correctly, including setting up your
environment with the required YottaDB (or GT.M) environment variables, or
setting the appropriate options in the `open` API. Make sure you have either
`$ydb_dist` (only applicable for YottaDB) or `$gtm_dist` set to the root of the
YottaDB (or GT.M) instance before you compile Nodem, whether manually, or via
`npm`. You will also need to have Node.js installed and working.

**ATTENTION:** These instructions assume that the `nodem` repository has been
installed in your home directory. The paths will likely be different if you have
installed this with `npm`.

**NOTE:** If you have installed Nodem using `npm`, it will attempt to build
`nodem.node` during installation. If there is a file in the `nodem` directory
called `builderror.log`, and if that file contains no build errors for
`nodem.node`, it built without issue. It also attempts to pre-compile the
`v4wNode.m` integration routine, and there might be warnings from that, which
won't affect the building of `nodem.node` itself. If you downloaded Nodem any
other way, including cloning it from its github repository, then you'll have to
build it from source. Remember to make sure that either `$ydb_dist` or
`$gtm_dist` is set to the root of the YottaDB (or GT.M) instance before building
Nodem. In order to build it, while in the root of the Nodem repository, run the
`npm run install` command, e.g.

```bash
$ cd ~/nodem
$ npm run install
```
or
```bash
$ node-gyp rebuild 2> builderror.log
```

In addition you will need to set a few environment variables, or set the
appropriate configuration options in the call to the `open` API, in order for
YottaDB (or GT.M) to find the Call-in table and the `v4wNode.m` routine that it
maps to. The Nodem package supplies a sample environment file, called `environ`.
It has a commented out command to set `$LD_LIBRARY_PATH` to `$ydb_dist` or
`$gtm_dist`, which you will need to uncomment if you need it. It is located in
`~/nodem/resources` and can be sourced into your working environment, either
directly, or from your own environment scripts or profile/login script, e.g.

```bash
$ cd ~/nodem/resources
$ source environ
```
or
```bash
$ echo "source ~/nodem/resources/environ" >> ~/.profile
```

If you don't source the `environ` file, then you will need to put a copy of
`v4wNode.m` into a directory that is specified in your `$ydb_routines` (only
applicable for YottaDB) or `$gtmroutines` routines path, or in the
`routinesPath` property in your call to the `open` API, so that YottaDB (or
GT.M) can find it. It is located in the `~/nodem/src` directory. Again, if you
don't source the `environ` file, then you will also need to define the `$ydb_ci`
(only applicable for YottaDB) or `$GTMCI` environment variable, or set the
`callinTable` property in your call to the `open` API, and point it at the file
`nodem.ci`, located in the `~/nodem/resources` directory, e.g.

```bash
$ export ydb_ci=~/nodem/resources/nodem.ci
$ cp ~/nodem/src/v4wNode.m ~/p
```
or
```javascript
> const callinTable = process.env.HOME + '/nodem/resources/nodem.ci';
> const routinesPath = process.env.HOME + '/nodem/src .'; // Make sure to include your routine directories
> ydb.open({callinTable: callinTable, routinesPath: routinesPath});
```

**NOTE:** As of Nodem version 0.20.7, if `$ydb_ci` and `$GTMCI` are undefined,
Nodem will use the path to the `nodem.ci` file within the repository, without
having to set the `callinTable` property, by default, simplifying configuration.

**NOTE:** As of Nodem version 0.20.7, if `$ydb_routines` and `$gtmroutines` are
undefined, Nodem will use the path to the `v4wNode.m` file within the
repository, without having to set the `routinesPath` property, by default,
simplifying configuration when you don't need to call other M code with the
`function` or `procedure` APIs.

You can clone the repository with this command..

```bash
$ git clone https://github.com/dlwicksell/nodem.git
```

You can also install it via `npm` with this command..

```bash
$ npm install nodem
```

If you are having an issue installing `nodem`, you can try this..

```bash
$ npm install nodem --ignore-scripts
```

You can update to the latest version with this command..

```bash
$ npm update nodem
```
or
```bash
$ npm install nodem@latest
```

## Important Notes ##

The `open` call does not require any arguments, and will connect with the
database specified in the environment variable `$ydb_gbldir` (only applicable
for YottaDB) or `$gtmgbldir`. If you have more than one database and would like
to connect to a different one than what is defined in your environment, you can
pass an object, with a property called either `globalDirectory` or `namespace`,
defined as the path to your global directory file for that database, e.g.

```javascript
> ydb.open({globalDirectory: process.env.HOME + '/data/globals/db_utf8.gld'});
```

Nodem supports setting up a custom routines path, for resolving calls to other M
functions and procedures, via the `routinesPath` property. Make sure that one of
the directories in the `routinesPath` contains the `v4wNode.m` routine, located
in the Nodem src directory, or its compiled object, `v4wNode.o`, otherwise Nodem
will not be fully functional. This could be used to provide some security, by
giving access only to certain routines, within a Nodem process, within an
environment that contains routines with unfettered access to the system in its
default environment configuration, e.g.

```javascript
> const HOME = process.env.HOME;
> ydb.open({routinesPath: `${HOME}/code/local/r138(${HOME}/code/local)`});
```

Nodem supports setting the Call-in path directly in the `open` call, via the
`callinTable` property. This can be handy if you are running Nodem in an
environment that has other software that uses the YottaDB (or GT.M) Call-in
interface, and you don't want to worry about namespace issues. Nor would you
need to set the `$ydb_ci`/`$GTMCI` environment variable, in order for Nodem to
be fully functional, e.g.

```javascript
> ydb.open({callinTable: process.env.HOME + '/nodem/resources/nodem.ci'});
```

### GT.CM Networking Support ###

You can configure Nodem to function as a [GT.CM][] client, allowing Nodem to
connect with a remote database. In the `open` method, you can set an
`ipAddress`, and/or a `tcpPort` property, and Nodem will set up the environment
to connect with a YottaDB (or GT.M) database on a remote server that already has
a GT.CM server running on that address and port. If only `ipAddress` or
`tcpPort` is defined, the other one will be set with a default value; 127.0.0.1
for `ipAddress`, or 6789 for `tcpPort`. Nodem will then set the `$ydb_cm_NODEM`
(if you are using YottaDB) or `$GTCM_NODEM` (if you are using GT.M) environment
variable, for that Nodem process only, with the address and port you set in the
`open` call, e.g.

```javascript
> ydb.open({ipAddress: '127.0.0.1', tcpPort: 6789});
```

If you are using IPv6, you need to surround your IP address with square
brackets, e.g.

```javascript
> ydb.open({ipAddress: '[::1]', tcpPort: 6789});
```

You will also need to create, or modify, a global directory file that maps one
or more database segments to a data file on the remote server you want to
connect with, noting that the prefix to the `-file=` argument in the example
below must be NODEM, in order to match the `$ydb_cm_NODEM`/`$GTCM_NODEM`
environment variable name that Nodem sets up for you, e.g.

```bash
$ $ydb_dist/mumps -run GDE
GDE> change -segment DEFAULT -file=NODEM:/home/user/data/globals/gtcm-server.dat
```

Then on the server you are connecting to, make sure you have the data file set
up at the same path that you set the `-file=` option to in the global directory
of your GT.CM client configuration, and have started the GT.CM server on the
same IP address and port that you configured in the `open` call in Nodem, e.g.

```bash
$ $ydb_dist/gtcm_gnp_server -log=gtcm.log -service=6789
```

**NOTE:** GT.CM only allows remote connections for the database access APIs, not
the `function` nor `procedure` APIs. So while using Nodem in a remote GT.CM
configuration, any calls to the `function` or `procedure` APIs will result in
local calls, not remote [RPC] calls. Data nodes accessed by GT.CM cannot
participate in transactions.

### Character Encodings ###

Nodem supports two different character encodings, UTF-8 and M. It defaults to
UTF-8 mode. M mode is similar to ASCII, except that it utilizes all 8 bits in a
byte, and it collates slightly differently. Instead of collation based only on
the character codes themselves, it sorts numbers before everything else (except
for the empty string). The character encoding you set in Nodem is decoupled from
the underlying character encoding you have set up for the YottaDB (or GT.M)
environment it is running in. So it is possible to work with UTF-8 encoded data
in the database, while in Nodem, even if you haven't set up YottaDB (or GT.M) to
work with UTF-8 directly. You can set it to UTF-8 mode directly by passing
`utf-8` or `utf8`, case insensitively, to the `charset` property. If you'd
rather work with an older byte-encoding scheme, that stores all characters in a
single byte, you can set charset to either `m`, `ascii`, or `binary`, case
insensitively. One thing to keep in mind when you do so, is that Node.js
internally stores data in UTF-16, but interprets data in UTF-8 in most cases.
You can control this through the process stream encoding methods inside of your
Node.js code. Call those methods to change the encoding to `binary` or `ascii`,
and it will interpret your data as a byte encoding, using the character glyphs
in your current locale, e.g.

```javascript
> process.stdin.setEncoding('binary');
> process.stdout.setDefaultEncoding('binary');
> ydb.open({charset: 'm'}); // For all threads
```
or
```javascript
> process.stdin.setEncoding('binary');
> process.stdout.setDefaultEncoding('binary');
> ydb.configure({charset: 'm'}); // For the current thread
```

### Data Modes ###

There are currently two different data modes that Nodem supports. The mode can
be set to `canonical` or `string`. The default is `canonical`, and interprets
data using the M canonical representation. I.e. Numbers will be represented
numerically, rather than as strings, and numbers collate before strings (except
for the empty string). The other mode, `string`, interprets all data as strings,
though they still collate the same as canonical mode, e.g.

```javascript
> ydb.open({mode: 'canonical'}); // For all threads
> ydb.get('v4wTest', 'numOfBeds');
25
```
or
```javascript
> ydb.configure({mode: 'string'}); // For the current thread
> ydb.get('v4wTest', 'numOfBeds');
'25'
```

### Debug Tracing Mode ###

Nodem also has a debug tracing mode, in case something doesn't seem to be
working right, or you want to see what happens to data as it moves through the
Nodem APIs. It has four levels of debugging, defaulting to `off`. The other
debug levels are `low`, `medium`, and `high`. You can also use the numbers 0-3.
The higher the debug level, the more verbose the debug output will be, e.g.

```javascript
> ydb.open({debug: 'low'}); // For all threads
```
or
```javascript
> ydb.open({debug: 2}); // For all threads
```
or
```javascript
> ydb.configure({debug: 'high'}); // For the current thread
```

### Signal Handling ###

Nodem handles several common signals that are typically used to stop processes,
by closing the database connection, resetting the controlling terminal
configuration, and stopping the Node.js process. These signals include `SIGINT`,
`SIGTERM`, and `SIGQUIT`. The handling of the `SIGQUIT` signal will also
generate a core dump of the process. All three signal handlers are on by
default. However, you can turn the signal handling on or off directly, via
passing true or false to a `signalHandler` object (with properties for each of
the signals) for each individual signal, or all of them at once, e.g.

```javascript
> ydb.open({signalHandler: {sigint: true, sigterm: false, sigquit: false}});
```
or
```javascript
> ydb.open({signalHandler: false});
```

### Function and Procedure Auto-relink ###

Nodem supports a feature called auto-relink, which will automatically relink a
routine object containing any function or procedure called by the `function` or
`procedure` API. By default auto-relink is off. You can enable it in one of four
ways. First, you can pass it as a property of the JavaScript object argument
which is passed to the `function` or `procedure` API directly, with a value of
true. This will turn on auto-relink just for that call. You can also disable it,
by setting `autoRelink` to false if it was already enabled by one of the global
settings, e.g.

```javascript
> ydb.function({function: 'version^v4wTest', autoRelink: true});
```

Second, you can enable it globally, for every thread, and for every call to the
`function` (or `procedure`) API, by setting the same property in a JavaScript
object passed to the `open` API, e.g.

```javascript
> ydb.open({autoRelink: true});
```

Third, you can enable it globally, per-thread, for every call to the `function`
(or `procedure`) API, by setting the same property in a JavaScript object passed
to the `configure` API, e.g.

```javascript
> ydb.configure({autoRelink: true});
```

Fourth, you can also enable it globally, for every thread, by setting the
environment variable NODEM_AUTO_RELINK to 1, or any other non-zero number, e.g.

```bash
$ export NODEM_AUTO_RELINK=1
$ node function.js
```
or
```bash
$ NODEM_AUTO_RELINK=1 node function.js
```

### Asynchronous APIs ###

Nodem's asynchronous APIs, do their work in a separate thread pool,
pre-allocated by Node.js via libuv. By default, four threads are created, and
will take turns executing each asynchronous call, including asynchronous calls
from other APIs. Nodem supports setting a different value for the pre-allocated
thread pool for asynchronous calls, in its `open` API, up to a max of 1024, in
the latest versions of Node.js, e.g.

```javascript
> ydb.open({threadpoolSize: 1024});
```

However, if your Node.js process executes any call asynchronously, from any API
or module, before you open the database connection with Nodem, then the
threadpoolSize property is ignored. So make sure you open the database
connection first in any process, if you want to control how large the
pre-allocated thread pool is.

**NOTE:** The Node.js core worker_thread API, which also allocates threads from
the same worker thread pool in libuv, allows complete control of creating and
destroying threads, and does not utilize the threadpoolSize (which just sets the
libuv environment variable `UV_THREADPOOL_SIZE`) set in the Nodem `open` API.

### Terminal Handling ###

YottaDB (and GT.M) changes some settings of its controlling terminal device, and
Nodem resets them when it closes the database connection. By default, Nodem will
restore the terminal device to the state it was in when the `open` call was
invoked. Normally this is the desired option, however, if you wish to reset the
terminal to typically sane settings, the `close` call allows this by setting the
`resetTerminal` property to true, e.g.

```javascript
> ydb.close({resetTerminal: true});
```

### Worker Threads ###

Nodem supports the Worker Threads [API][worker-threads], for both synchronous
and asynchronous calls. Since YottaDB and GT.M are single-threaded, opening and
closing a connection to their database and runtime, should only be done once per
process lifetime. Nodem's `open` and `close` APIs will only work when called
from the main thread of the process. In order to work with the worker threads
API, you should call the Nodem `open` API in the main thread before creating any
worker threads, and you should call the Nodem `close` API in the main thread,
after all the worker threads have exited. You will still need to make sure that
you require Nodem in each worker thread, as well as the main thread, in order to
have access to the Nodem API in each thread.

### Configure API ###

Nodem has a `configure` API, which will allow worker threads to change some
per-thread database configuration options. It can be called from the worker
threads, or the main thread, and will allow you to change per-thread
configuration options as often as you like. There are four configuration options
that are set per-thread. They can be set in the `open` API, by the main thread,
before any other Nodem calls are made, or they can be set in the `configure`
API, anytime you like, in the main thread, or in the worker threads. Those
configuration options are: `charset`, `mode`, `autoRelink`, and `debug`.

### Transaction API ###

Nodem has a `transaction` API, which provides support for full ACID
transactions. It is only supported when running Nodem with YottaDB at this time.
It requires, as its first argument, a JavaScript function, which takes no
arguments, and which can contain other Nodem calls, nested `transaction` calls,
or any JavaScript code. The JavaScript function will be run within a transaction
by YottaDB. It will also be run synchronously, and every Nodem API that is
called within the transaction must also be run synchronously. By default,
transactions are run in serial mode (providing full ACID semantics), and no
local variables are reset during transaction restarts. You can pass an optional
second argument; a JavaScript object, with one or two properties. The properties
are `variables`, an array of local variables that are reset to their values
before the transaction started, whenever a transaction is restarted, and `type`,
which if set to `Batch` (`batch` or `BATCH` will also work) will run the
transaction in batch mode, (which does not provide durability, but does provide
the rest of the ACID semantics). If `variables` has `'*'` as its only array
item, then every local variable in the symbol table will be reset during a
transaction restart.

In order to restart a transaction, pass the string 'Restart' ('restart',
'RESTART', or the `tpRestart` property will also work), as the argument to the
return statement. In order to rollback a transaction, pass the string 'Rollback'
('rollback', 'ROLLBACK', or the `tpRollback` property will also work), as the
argument to the return statement. Any other argument to the return statement
will commit the transaction, including functions without a return statement.
When you call a Nodem API within a transaction, make sure to check for returned
errors, and return with 'Rollback' in that case. If any Nodem API within a
transaction returns with an error code of `YDB_TP_RESTART` (a YottaDB restart
code), or with an error code of `YDB_TP_ROLLBACK` (a YottaDB rollback code) make
sure to return with the appropriate transaction message, 'Restart' or 'Rollback'
respectively, or simply return with the `YDB_TP_*` error code directly. In order
to make it simpler to test for restart and rollback error codes from the YottaDB
transaction engine, when more sophisticated logic is desired, Nodem stores the
restart code in the `tpRestart` property, and the rollback code in the
`tpRollback` property, for convenience.

If you throw a JavaScript error inside of the transaction function, it will be
written to standard error, and will cause a rollback operation to occur. If you
handle it with a try-catch block, then what happens will depend upon how you
handle it, and whether you throw another error, or return with a specific
transaction processing message or not. Transaction code should be as short and
simple as possbile, and should avoid calling any code with side effects, e.g.

```javascript
> ydb.transaction(() => {
    let flag = ydb.get({global: 'v4wTest', subscripts: ['flag']});

    if (flag.errorCode === ydb.tpRestart) return 'Restart';
    if (!flag.ok) return 'Rollback';

    let data = ydb.get({global: 'v4wTest', subscripts: ['data']});

    if (data.errorCode === ydb.tpRestart) return 'Restart';
    if (!data.ok) return 'Rollback';

    if (data.data < 0) return 'Rollback';

    if (data.data < 10) {
        let increment = ydb.increment({global: 'v4wTest'});

        if (increment.errorCode === ydb.tpRestart) return 'Restart';
        if (!increment.ok) return 'Rollback';
    }

    let type = ydb.get({global: 'v4wTest', subscripts: ['type']});

    if (type.errorCode === ydb.tpRestart) return 'Restart';
    if (!type.ok) return 'Rollback';

    let test = ydb.set({global: 'v4wTest', data: type + ':' + data});

    if (test.errorCode === ydb.tpRestart) return 'Restart';
    if (!test.ok) return 'Rollback';

    return 'Commit';
});
```

Even though the `transaction` API runs synchronously, it is fully compatible
with the Worker Threads API. By creating a new worker thread and running the
`transaction` API, and any other APIs it calls in it, you can emulate an
asynchronous pattern, as the running transaction will not block the main thread,
or any of the other worker threads. For an example of this pattern, see the
supplied `transaction.js` program in the `examples` directory.

### Procedure API ###

Nodem has a `procedure` or `routine` API, which is similar to the `function`
API, except that it is used to call M procedures or routines, which do not
return any values. If the `procedure` API is called via a JavaScript object,
then the object must contain the required `procedure`/`routine` property, set to
the name of the procedure/routine. It may also contain an optional property,
called `arguments`, which is an array of arguments to pass to the
procedure/routine. It can also be called by-position, just like the `function`
API. It also supports the `autoRelink` option, just as described in the
`function` API, e.g.

```javascript
> ydb.procedure({procedure: 'set^v4wTest', arguments: ['test', 5]});
```
or
```javascript
> ydb.procedure('set^v4wTest', 'test', 5);
```

### Lock API ###

The `lock` API takes an optional `timeout` argument. If you do not set a
timeout, it will wait to acquire the lock indefinitely. If you wish to come back
from the call right away, if the lock is not available, simply pass a timeout
argument of 0, e.g.

```javascript
> ydb.lock({global: 'v4wTest', timeout: 5});
```
or
```javascript
> ydb.lock({global: 'v4wTest', timeout: 0});
```

### Kill API ###

The `kill` API takes an optional `nodeOnly` argument. It can be set to true or
false, defaulting to false. If set to true, then it will only remove the node
that is passed to it; if set to false, then it will remove the node passed to
it, and all of its children, or the full sub-tree, e.g.

```javascript
> ydb.kill({global: 'v4wTest', nodeOnly: true});
```
or
```javascript
> ydb.kill({local: 'v4wTest', nodeOnly: true});
```

The `nodeOnly` option is available when calling the `kill` API by passing
arguments in a single JavaScript object, like above, but not when passing
arguments by-position.

### Additional Features ###

Nodem provides a built-in API usage help menu. By calling the `help` method
without an argument, Nodem will display a list of APIs and a short description
of what they do. Calling the help method with an argument string of one of those
APIs will display more in-depth usage information for that method.

Nodem supports full M local symbol table manipulation with the current APIs. In
order to use it, instead of defining a `global` property in your argument
object, you define a `local` property. For APIs that support passing arguments
by-position, you signify that you want them to work on a global, by using a `^`
as the first character of the first argument, otherwise they are working on a
local variable. For the get and set APIs, if the first character of the first
argument is a `$`, then you are working with an intrinsic special variable
(ISV). There is also a `localDirectory` API, that works the same way as the
`globalDirectory` API, except that it lists the local symbols in the symbol
table, rather than the globals in the database. One caveat is that you cannot
manipulate any local variable that begins with `v4w`, as Nodem internally uses
that namespace to implement the `v4wNode.m` integration routine. You can also
call the `kill` API with no arguments, and it will clear the local symbol table.
This functionality will allow you to call legacy M functions and procedures,
without having to write M routine wrappers. Here is an example of using the
local symbol table functionality to call a legacy API directly from Nodem. In
this example, the local variable, 'U', needs to be set before this API is
called, as it expects it to be defined already. You can also see how the local
symbol table changes, after setting the required local variable, making the
call, and then clearing the symbol table, e.g.

```javascript
> ydb.localDirectory();
[]
> ydb.set({local: 'U', data: '^'});
{ ok: true, local: 'U', data: '^' }
> ydb.localDirectory();
[ 'U' ]
> ydb.procedure({procedure: 'AGET^ORWORR', arguments: [, 9, '2^0', 13, 0]});
{
  ok: true,
  procedure: 'AGET^ORWORR',
  arguments: [ <1 empty item>, 9, '2^0', 13, 0 ]
}
> ydb.localDirectory();
[ 'DILOCKTM', 'DISYS', 'DT', 'DTIME', 'DUZ', 'IO', 'U', 'XPARSYS' ]
> ydb.kill();
true
> ydb.localDirectory();
[]
```

Nodem supports calling functions and procedures with arguments passed
by-reference, or by-variable, in addition to the standard passing by-value.
This will allow someone who needs to interface Nodem with legacy M APIs that
require using local variables in this manner, the ability to do so directly in
Nodem, rather than having to write an M wrapper around the API, and calling that
from Nodem. In order to use this functionality, you need to pass your arguments
via a specially formatted object, in order to instruct Nodem that you wish to
pass arguments differently than normal. This is necessary because if you tried
to pass an argument by-reference or by-variable directly, Node.js will try to
dereference it as a local JavaScript variable, and you would never be able to
refer to the right symbol in the back-end M environment. The structure of the
specially formatted object is simple. It contains a `type` property, which can
be one of three values: `reference`, `variable`, or `value`; and it also
contains a `value` property which contains the name you want to use when the
type is `reference` or `variable`, and the actual data you want to pass if type
is `value`. The `value` type is there for consistency, but you would normally
just pass arguments by value directly, without resorting to this specially
formatted argument object. Here is an example of how you could use this
functionality, while calling a legacy M API, many of which require passing
arguments in this fashion, e.g.

```javascript
> ydb.set({local: 'U', data: '^'});
{ ok: true, local: 'U', data: '^' }
> const arg = {type: 'reference', value: 'LIST'};
undefined
> ydb.procedure({procedure: 'LISTALL^ORWPT', arguments: [arg, 'A', 1]});
{
  ok: true,
  procedure: 'LISTALL^ORWPT',
  arguments: [ { type: 'reference', value: 'LIST' }, 'A', 1 ]
}
> ydb.localDirectory();
[ 'LIST', 'U' ]
> ydb.data({local: 'LIST'});
{ ok: true, local: 'LIST', defined: 10 }
> ydb.nextNode({local: 'LIST'});
{
  ok: true,
  local: 'LIST',
  subscripts: [ 1 ],
  data: '1^ZZ PATIENT,TEST ONE^^^^ZZ PATIENT,TEST ONE',
  defined: true
}
> ydb.nextNode({local: 'LIST', subscripts: [1]});
{
  ok: true,
  local: 'LIST',
  subscripts: [ 2 ],
  data: '3^ZZ PATIENT,TEST THREE^^^^ZZ PATIENT,TEST THREE',
  defined: true
}
> ydb.nextNode({local: 'LIST', subscripts: [2]});
{
  ok: true,
  local: 'LIST',
  subscripts: [ 3 ],
  data: '2^ZZ PATIENT,TEST TWO^^^^ZZ PATIENT,TEST TWO',
  defined: true
}
> ydb.nextNode({local: 'LIST', subscripts: [3]});
{ ok: true, local: 'LIST', defined: false }
```

## Interface ##

API                      | Description
-------------------------|------------------------------------------------------------------------------------------------------
*open*                   | Open the database connection
*configure*              | Configure per-thread parameters of the database connection
*close*                  | Close the database connection
*help*                   | Display a help menu of method usage
*version* or *about*     | Display version information; if database connection open, display its version
*data*                   | Determine whether a global or local node has data and/or children
*get*                    | Retrieve the value of a global, local, or intrinsic special variable node
*set*                    | Set a global, local, or intrinsic special variable node, to a new value
*kill*                   | Delete a global or local node, and optionally, all of its children; or delete all local variables
*merge*                  | Merge a global or local tree/sub-tree, or data node, to a global or local tree/sub-tree, or data node
*order* or *next*        | Retrieve the next global or local node, at the current subscript level
*previous*               | Same as order, only in reverse
*nextNode*               | Retrieve the next global or local node, regardless of subscript level
*previousNode*           | Same as nextNode, only in reverse
*increment*              | Atomically increment the value stored in a global or local node
*lock*                   | Lock a global or global node, or local or local node, incrementally
*unlock*                 | Unlock a global or global node, or local or local node, incrementally; or release all locks
*transaction*            | Call a JavaScript function within a YottaDB transaction - synchronous only
*function*               | Call an extrinsic function
*procedure* or *routine* | Call a procedure/routine
*globalDirectory*        | List the names of the globals in the database
*localDirectory*         | List the names of the variables in the local symbol table
*retrieve*               | Not yet implemented
*update*                 | Not yet implemented

## Disclaimer ##

Fourth Watch Software endeavors not to make any breaking changes to APIs, but as
Nodem is still in development, its interface may change in future versions.

## Contact Info ##

If you have any questions or feature requests, email me at <dlw@linux.com>  
To report any issues, visit <https://github.com/dlwicksell/nodem/issues>

## See Also ##

* The [Node.js][] server-side JavaScript runtime.
* The [YottaDB][] implementation of M.
* The [GT.M][] implementation of M.

[version-image]: https://img.shields.io/node/v/nodem.svg
[license-image]: https://img.shields.io/npm/l/nodem.svg?colorB=blue
[downloads-image]: https://img.shields.io/npm/dm/nodem.svg?colorB=orange
[npm-url]: https://npmjs.org/package/nodem
[license-text]: https://github.com/dlwicksell/nodem/blob/HEAD/COPYING
[get-started]: https://yottadb.com/product/get-started
[worker-threads]: https://nodejs.org/api/worker_threads.html

[Node.js]: https://nodejs.org
[YottaDB]: https://yottadb.com
[GT.M]: https://sourceforge.net/projects/fis-gtm
[Nodem]: https://docs.yottadb.com/MultiLangProgGuide/jsprogram.html
[Wiki]: https://github.com/dlwicksell/nodem/wiki
[GT.CM]: https://docs.yottadb.com/AdminOpsGuide/gtcm.html
