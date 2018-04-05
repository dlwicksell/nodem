# NodeM #

## A Node.js binding and driver for the YottaDB/GT.M language and database ##

Version 0.10.0 - 2018 Apr 5

## Copyright and License ##

Addon Module written and maintained by David Wicksell <dlw@linux.com>  
Copyright © 2012-2018 Fourth Watch Software LC

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU Affero General Public License (AGPL) as published by the
Free Software Foundation, either version 3 of the License, or (at your option)
any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License along
with this program. If not, see <http://www.gnu.org/licenses/>.

Contact me if you are interested in using Nodem with a different license.
***

## Disclaimer ##

Fourth Watch Software endeavors not to make any breaking changes to APIs, but as
Nodem is still in development, its interface may change in future versions, at
least until version 1.0.0 is released.

## Summary and Info ##

Nodem is an open source addon module for Node.js that integrates Node.js with
[YottaDB][] or [GT.M][], providing in-process access to their database and some
language features from within JavaScript, via their C call-in interface. From
Node.js you can perform the basic primitive global database handling operations
and invoke MUMPS functions and procedures, as well as having access to full
local symbol table management. Although designed exclusively for use with
YottaDB and GT.M, Nodem aims to be API-compatible (while in 'strict' mode) with
the in-process Node.js interface for [Caché][], at least for the APIs that have
been implemented in Nodem. As such, please refer to the Caché Node.js [API
documentation][BXJS] for further details on how to use those APIs.

Currently only the synchronous versions of the Nodem APIs are fully implemented,
and their arguments have to be passed in JavaScript objects. However, the
asynchronous versions of the APIs, and the ability to also pass arguments by
position, is coming soon.

The current implementation relies upon the YottaDB and GT.M C call-in interface.
Recently, YottaDB has released a new, lower-level database access API in C,
known as the Simple API, which provides faster access to the underlying database
operations. Support for YottaDB's new Simple API will be coming soon.

Nodem now supports full local symbol table manipulation with the current
database APIs. In order to use it, insteading of defining a 'global' property in
your argument object, you define a 'local' property. For most APIs, that is all
you have to know. There is also now a localDirectory API, that works the same
way as the globalDirectory API, except that it lists the local symbols in the
symbol table, rather than the globals in the database. One caveat is that you
cannot manipulate any local variable that begins with 'v4w', as Nodem internally
uses that namespace to implement the v4wNode.m integration routine. You can also
now call the kill API with no arguments, and it will clear the local symbol
table. This functionality will allow you to call legacy MUMPS functions and
procedures, without having to write wrappers in M anymore, in most cases. It
currently doesn't support call by-reference, or call by variable indirection,
but that support is coming soon. Here is an example of using the local symbol
table functionality to call a legacy API directly from Nodem. In this example,
the local variable, U, needs to be set before this API is called, as it expects
it to be defined already. You can also see how the local symbol table changes,
after setting the required local variable, making the call, and then clearing
the symbol table, e.g.

    > gtm.localDirectory();
      []
    > gtm.set({local: 'U', data: '^'});
    > gtm.localDirectory();
      [ 'U' ]
    > gtm.procedure({procedure: 'AGET^ORWORR', arguments: [, 9, '2^0', 13, 0]});
    > gtm.localDirectory();
      [ 'DILOCKTM', 'DISYS', 'DT', 'DTIME', 'DUZ', 'IO', 'U', 'XPARSYS' ]
    > gtm.kill();
    > gtm.localDirectory();
      []

The open() call works a bit differently than the Caché version; it does not
require any arguments, and it will default to using the database specified in
the environment variable, $gtmgbldir. If you have more than one database and
would like to connect to a different one than $gtmgbldir points to, you can
define an object, with a property called either globalDirectory or namespace,
defined as the path to your global directory file for that database, e.g.

    > gtm.open({globalDirectory: '/home/dlw/g/db_utf.gld'});

Nodem supports setting up a custom routine path, for resolving calls to other M
functions and procedures, via the routinePath property. Make sure that one of
the directories in the routinePath contains the v4wNode.m routine, located in
the Nodem src/ directory, or its compiled object, v4wNode.o, otherwise Nodem
will not be able to access the database. This could be used to provide a certain
level of security, by giving access only to certain routines, within a Nodem
process, within an environment that contains routines with unfettered access to
the system in its default environment configuration, e.g.

    > gtm.open({routinePath: '/home/dlw/p/V6.3-000_x86_64(/home/dlw/p)'});

Nodem supports setting the call-in path directly in the open call. This can be
handy if you are running Nodem in an environment that has other software that
uses the YottaDB/GT.M call-in interface, and you don't want to worry about
namespace issues. As you won't need to set the $GTMCI environment variable, and
can instead set callinPath in the open call, e.g.

    > gtm.open({callinPath: '/home/dlw/nodem/resources/nodem.ci'});

Nodem supports setting a [GT.CM][] client to allow for remote connections across
a network interface, using YottaDB/GT.M's built-in GT.CM functionality. In the
open() method, you can set an ipAddress, or ip_address, and/or a tcpPort, or
tcp_port, property, and Nodem will set up the environment to connect with a
YottaDB or GT.M database on a remote server that already has a GT.CM server
running on that address and port. If only ipAddress or tcpPort is defined, the
other one will be set with a default value; 127.0.0.1 for ipAddress, and 6879
for tcpPort. Nodem will then set the $GTCM_NODEM environment variable, for that
Nodem process only, with the address and port you set in the open() call, e.g.

    > gtm.open({ipAddress: '127.0.0.1', tcpPort: 6879});

You will also need to create a global directory file that maps one or more
database segments to a data file on the remote server you want to connect with,
noting that the prefix to the -file argument in the example below must be NODEM,
in order to match the $GTCM_NODEM environment variable name that Nodem sets up
for you, e.g.

    $ mumps -run GDE
    GDE> change -segment DEFAULT -file=NODEM:/home/dlw/g/gtcm-server.dat

Then on the server you are connecting to, make sure you have the data file set
up at the same path that you set the '-file=' option to in the global directory
of your GT.CM client configuration, and have started the GT.CM server on the
same ipAddress and tcpPort that you configured in the open() call in Nodem, e.g.

    $ $gtm_dist/gtcm_gnp_server -log=gtcm.log -service=127.0.0.1:6879

Nodem supports two different character encodings, UTF-8 and M or binary. In
previous releases, it would use whichever encoding you had set up in your
environment for YottaDB/GT.M. This was mostly because the v4wNode.m integration
routine would do data transformations in the encoding that was set up in that
environment for YottaDB/GT.M. Now Nodem allows you to set the character encoding
directly, and it is decoupled from the encoding you have set up for the YottaDB
or GT.M environment it is running in. So it is now possible to work with UTF-8
encoded data in the database, while in Nodem, even if you haven't set up YottaDB
or GT.M to work with UTF-8 directly. It also now defaults to running in UTF-8
mode. You can set it to UTF-8 mode directly by passing utf-8 or utf8, case
insensitively, to the charset property in the open method. If you'd rather work
with an older byte-encoding scheme, that does not represent any characters in a
multiple bytes, you can set charset to either m, ascii, or binary, case
insensitively. One thing to keep in mind when you do so, is that Node.js
internally represents data in UTF-16, but interprets data in UTF-8 in most
cases. You can control this through process.stdout.setDefaultEncoding inside of
your Node.js code. Set that property to 'binary' or 'ascii', and it will
interpret your data as a byte encoding, using the character glyphs in your
current locale, e.g.

    > gtm.open({charset: 'm'});

Nodem allows you to set the data mode you want to use. Currently mode can be set
to either 'canonical' or 'strict'. The default is canonical, and interprets data
using MUMPS' canonical representation. I.e. Numbers will be represented
numerically, etc. Strict mode interprets all data as strings, strictly following
the convention set with InterSystems' Node.js driver, e.g.

    > gtm.open({mode: 'strict'});

Nodem also has a debug tracing mode, in case something doesn't seem to be
working right. It has four levels of debugging, defaulting to 'off', 0, or
false. The other debug levels are 'low', 1, or true; 'medium' or 2, and 'high'
or 3. Obviously the higher the debug level, the more verbose the debug output
will be, e.g.

    > gtm.open({debug: true});

Nodem supports a feature called auto-relink, which will automatically relink a
routine object containing any function (or procedure) called by the function (or
procedure) API. By default auto-relink is off. You can enable it in one of three
ways. First, you can pass it as a property of the JavaScript object argument
which is passed to the function (or procedure) API directly, with a value of
true, or any non-zero number. This will turn on auto-relink just for that call.
You can also disable it, by setting autoRelink to false, or 0, if it was already
enabled by one of the global settings, e.g.

    > gtm.function({function: 'version^v4wTest', autoRelink: true});

Second, you can enable it globally, for every call to the function (or
procedure) API, by setting the same property in a JavaScript object passed to
the open API, e.g.

    > gtm.open({autoRelink: true});

Third, you can also enable it globally, by setting the environment variable
NODEM_AUTO_RELINK to 1, or any other non-zero number, e.g.

    $ export NODEM_AUTO_RELINK=1
    $ node examples/set.js
    or
    $ NODEM_AUTO_RELINK=1 node examples/set.js

GT.M changes some settings of its controlling terminal device, and Nodem has to
reset them when it closes the database connection. By default, Nodem will
restore the terminal device to the state it was in when the open() call was
invoked. Normally this is the desired option, however, if you wish to reset the
terminal to typically sane settings, the close() call allows this by setting the
resetTerminal property to true, or any non-zero number, e.g.

    > gtm.close({resetTerminal: true});

Nodem has a new procedure or routine API, which is similar to the function API,
except that it is used to call M procedures or subroutines, which do not return
any values. The procedure API accepts one argument, a JavaScript object. The
object must contain the required procedure or routine property, set to the name
of the procedure or routine. It may also contain an optional property, called
arguments, which is an array of arguments to pass to the procedure. It also
supports the autoRelink option, just as described in the function API, e.g.

    > gtm.procedure({procedure: 'set^v4wTest', arguments: ['dlw', 5]});

Nodem has added support for timeouts in the lock API. You can pass a timeout, in
seconds, as the second argument, after the JavaScript object. You can also pass
the timeout argument in a property, called timeout, in the first object
argument. If you do not pass a timeout argument, the lock API will wait to
acquire a lock indefinitely. If you wish to come back from the call right away,
if the lock is not available, simply pass a timeout argument of 0, e.g.

    > gtm.lock({global: 'dlw'}, 0);
    or
    > gtm.lock({global: 'dlw', timeout: 5});

## Installation ##

There are a few things to be aware of in order to use the Nodem addon. You will
need to have YottaDB or GT.M installed and configured correctly, including
setting up your environment with the required YottaDB/GT.M environment
variables. You will also need to have Node.js installed and working.

**ATTENTION:** These instructions assume that the nodem repository has been
installed in your home directory. The paths will likely be different if you have
installed this with npm.

**NOTE:** If you have installed Nodem using npm, it will attempt to build
mumps.node during installation. If there is a file in the nodem/ directory
called builderror.log, and if that file contains no build errors for mumps.node,
it built without issue. It also attempts to pre-compile the v4wNode.m
integration routine, and there might be warnings from that, which don't affect
the building of mumps.node itself. If you downloaded Nodem any other way,
including cloning it from its github repository, then you'll have to build it
from source. While in the root of the Nodem repository, you simply run the 'npm
install' command, e.g.

    $ cd ~/nodem
    $ npm install

Nodem should run on every version of Node.js starting with version 0.12.2,
through the current release (v9.11.0 at this time), as well as every version of
IO.js. However, in the future, both Node.js and the V8 JavaScript engine at its
core, could change their APIs in a non-backwards-compatible way, which might
break Nodem for that version.

**NOTE:** The build file specifies a runtime linker path, which is the path that
is defined in the $gtm_dist environment variable, at the time mumps.node was
compiled. In order to successfully compile mumps.node, you should have the
$gtm_dist environment variable set correctly when you install Nodem.

**NOTE** None of the following steps, dealing with shared libraries and other
linking issues, should be necessary if you set $gtm_dist before installing Nodem
and are running Nodem in the same environment you compile it in. Some of these
steps may be necessary if you compile Nodem in one environment, but run it in a
different environment, with a different path to the YottaDB or GT.M
distribution.

You might have to move a copy of libgtmshr.so (YottaDB/GT.M shared runtime
library) into a directory that will be searched by the dynamic linker/loader
when mumps.node is loaded at runtime, if it isn't already located in one. You
will find libgtmshr.so bundled with YottaDB or GT.M wherever you have installed
it. There are a couple of things you can do at this point. You can move
libgtmshr.so to a standard directory that is searched by the loader, such as
lib/, or /usr/lib/, or on some systems, /usr/local/lib/. Then you will have to
run ldconfig as root to rebuild the linker's cache. You could also create a
symbolic link to it if you choose, e.g.

    $ sudo -i
    # cd /usr/local/lib/
    # ln -s /opt/lsb-fis/gtm/V6.3-000_x86_64/libgtmshr.so
    # ldconfig
    # exit

You may have to add the /usr/local/lib/ directory to /etc/ld.so.conf or create
an /etc/ld.so.conf.d/libc.conf file and add it there, and then run ldconfig as
root. If you go this route, you should consider giving the library a real linker
name and soname link, based on its version. Instead, you can avoid having to
copy or link to the library by setting the environment variable,
$LD_LIBRARY_PATH, to point to it, as the loader will search there first. It is
usually advisable not to export $LD_LIBRARY_PATH into your environment, so you
might want to define it right before calling node, e.g.

    $ LD_LIBRARY_PATH=${gtm_dist} node
    or
    $ LD_LIBRARY_PATH=${gtm_dist} node examples/set.js

In addition you will need to set a few environment variables in order for
YottaDB or GT.M to find the call-in table and the MUMPS routine that it maps to.
The Nodem package supplies a sample environment file, called environ. It has a
commented out command to set $LD_LIBRARY_PATH to $gtm_dist, which you will need
to uncomment if you need it. It is located in ~/nodem/resources/ and can simply
be sourced into your working environment, either directly, or from your own
environment scripts or profile/login script, e.g.

    $ cd ~/nodem/resources/
    $ source environ
    or
    $ echo "source ~/nodem/resources/environ" >> ~/.profile

If you don't source the environ file, then you will need to put a copy of
v4wNode.m into a directory that is specified in your $gtmroutines routine path,
or in the routinePath property in your call to the open API, so that YottaDB or
GT.M can find it. It is located in the ~/nodem/src/ directory. Again, if you
don't source the environ file, then you will also need to define the $GTMCI
environment variable, or set the callinPath property in your call to the open
API, and point it at the file nodem.ci, located in the ~/nodem/resources/
directory, e.g.

    $ cp ~/nodem/src/v4wNode.m ~/p/
    $ export GTMCI=~/nodem/resources/nodem.ci

You can clone the repository with this command..

    $ git clone https://github.com/dlwicksell/nodem.git

You can also install it via npm with this command..

    $ npm install nodem

You can update to the latest version with this command..

    $ npm update nodem

## Building from source ##

If you have installed Nodem via npm, it will usually build the shared library
for you automatically, and you can skip the step of building it manually from
source.

If you happen to be running a version of GT.M that is older than V5.5-000, you
might have to change a configuration option in the build file, before you build
from source. This is because Nodem is using a GT.M API that didn't exist in
older versions. This shouldn't be necessary, however, as the preinstall script
should take care of this automatically for you. If it is necessary, open up the
file binding.gyp (the build specification file), and edit the line that contains
the string 'GTM_VERSION', to the version of GT.M that you are running. You can
find out what version of GT.M you are running by invoking GT.M in direct mode,
e.g.

    $ mumps -direct
    GTM>write $zversion
    GT.M V6.3-000A Linux x86_64

Then take the first two numbers in the version string, in this example, 6.3, and
drop the decimal point and that is what you would set GTM_VERSION to in
binding.gyp. That would be 'GTM_VERSION=63' in this case. Next, if you have
installed Nodem via npm, while in the root of the repository, run this command:

    $ npm install

Or, if you have not installed Nodem via npm, while in the root of the
repository, run this command instead:

    $ node-gyp rebuild 2> builderror.log

If there were any errors, they will show up in builderror.log. That's it, now
Nodem should work with your older version of GT.M.

If you have any questions, feature requests, or bugs to report, please contact
David Wicksell <dlw@linux.com>

* The [YottaDB][] implementation of MUMPS.
* The [GT.M][] implementation of MUMPS.

[YottaDB]: https://yottadb.com/
[GT.M]: https://www.fisglobal.com/solutions/banking-and-wealth/services/database-engine
[Caché]: https://www.intersystems.com/products/cache/
[BXJS]: https://docs.intersystems.com/documentation/cache/20172/pdfs/BXJS.pdf
[GT.CM]: http://tinco.pair.com/bhaskar/gtm/doc/books/ao/UNIX_manual/webhelp/content/ch13.html

### APIs ###

* *close*                  - Close the database connection
* *data*                   - Determine whether a global or local node has data and/or children
* *function*               - Call an extrinsic function
* *get*                    - Retrieve a global or local node value
* *globalDirectory*        - List the names of the globals in the database
* *help*                   - Display a help menu of method usage
* *increment*              - Atomitcally increment the value stored in a global or local node
* *kill*                   - Delete a global or local node, and all of its children; or kill all variables in the local symbol table
* *localDirectory*         - List the names of the variables in the local symbol table
* *lock*                   - Lock a global or global node, or local or local node, incrementally
* *merge*                  - Merge a global or local tree/sub-tree, or data node, to a global or local tree/sub-tree, or data node
* *nextNode*               - Retrieve the next global or local node, regardless of subscript level
* *open*                   - Open the database connection
* *order* or *next*        - Retrieve the next global or local node, at the current subscript level
* *previous*               - Same as order, only in reverse
* *previousNode*           - Same as nextNode, only in reverse in YottaDB r1.10 or newer, otherwise not yet implemented
* *procedure* or *routine* - Call a procedure/routine/subroutine
* *retrieve*               - Not yet implemented
* *set*                    - Set a global or local node to a new value
* *unlock*                 - Unlock a global or global node, or local or local node, incrementally; or release all locks
* *update*                 - Not yet implemented
* *version* or *about*     - Display version information; if database connection open, display its version
