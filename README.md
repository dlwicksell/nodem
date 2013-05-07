# NodeM #

## A Node.js binding to the GT.M language and database ##

Version 0.2.1 - 2013 May 7

## Copyright and License ##

Addon Module written and maintained by David Wicksell <dlw@linux.com>  
Copyright © 2012,2013 Fourth Watch Software, LC

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License (AGPL)
as published by the Free Software Foundation, either version 3 of
the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program. If not, see <http://www.gnu.org/licenses/>.

***

## Disclaimer ##

Nodem is experimental, and not yet ready for production. It is a work in
progress, and as such, its implementation is likely to change a lot.

## Summary and Info ##

Nodem is an Open Source Addon module for Node.js that integrates Node.js
with the [GT.M][] database, providing access to GT.M's database from
Javascript, via GT.M's C Call-in Interface. From Node.js you can perform
the basic primitive Global Database handling operations and also invoke
GT.M/Mumps functions. Although designed exclusively for use with GT.M,
Nodem aims to be API-compatible with the in-process Node.js interface for
[Globals][] and [Caché][], at least for the APIs that have been
implemented in Nodem. As such, please refer to the Caché Node.js [API
documentation][Docs] for further details on how to use those
APIs.

## Installation ##

There are a few things to be aware of in order to use the Nodem addon.
You will need to have GT.M installed and configured correctly, including
setting up your environment with the required GT.M environment variables.
You will also need to have Node.js installed and working.

These instructions assume that the nodem repository has been installed in
your home directory. The paths will likely be different if you have
installed this with npm. You will need to copy the correct version of
mumps.node for your system architecture and version of Node.js, to
~/nodem/lib/mumps.node. The mumps.node pre-built modules, which you will
find in ~/nodem/lib/, are named for the version of Node.js that they
support, mumps10 for Node.js version 0.10.x, and mumps8 for Node.js
version 0.8.x (and 0.6.x). Each module will also end in _x8664 for 64-bit,
or _i686 for 32-bit systems. By default there is a mumps.node already
there, which is a copy of the 64-bit version for Node.js version 0.8.x
(and 0.6.x). It is important to realize that the addon will not function
unless it is called mumps.node, and a symbolic link won't work. E.g.

    $ cd ~/nodem/lib
    $ cp mumps10.node_x8664 mumps.node
    $ cd -

You will also have to move a copy of libgtmshr.so (GT.M shared runtime
library) into a directory that will be searched by the dynamic
linker/loader when mumps.node is loaded at runtime, if it isn't already
located in one. You will find libgtmshr.so bundled with GT.M wherever you
have installed it. There are a couple of things you can do at this point.
You can move libgtmshr.so to a standard directory that is searched by the
loader, such as /usr/lib/, or on some systems, /usr/local/lib/. Then you
will have to run ldconfig as root to rebuild the linker's cache. You could
also create a symbolic link to it if you choose. E.g.

    $ sudo -i
    # cd /usr/local/lib
    # ln -s /opt/lsb-gtm/6.0-001_x8664/libgtmshr.so
    # ldconfig
    # exit

You may have to add the /usr/local/lib/ directory to /etc/ld.so.conf or
create an /etc/ld.so.conf.d/libc.conf file and add it there, and then run
ldconfig as root. If you go this route, you should consider giving the
library a real linker name and soname link, based on its version. Instead,
you can avoid having to copy or link to the library by setting the
environment variable, LD_LIBRARY_PATH, to point to it, as the loader will
search there first. It is usually advisable not to export LD_LIBRARY_PATH
into your environment, so you might want to define it right before calling
node. E.g.

    $ LD_LIBRARY_PATH=${gtm_dist} node
    or
    $ LD_LIBRARY_PATH=${gtm_dist} node test

As you can see though, that is more of a pain. If you happen to have
installed GT.M where I have, in /opt/lsb-gtm/6.0-001_x8664/, then you
don't have to do anything. There is an embedded rpath in the pre-built
modules, so the loader will also check in that directory.

In addition you will need to set a few environment variables in order for
GT.M to find the call-in table and the MUMPS routine that it maps to. The
Nodem package supplies a sample environment file, called environ. It has a
commented out command to set LD_LIBRARY_PATH to $gtm_dist, which you will
need to uncomment if you need it. It is located in ~/nodem/resources/ and
can simply be sourced into your working environment, either directly, or
from your own environment scripts or profile script. E.g.

    $ cd ~/nodem/resources
    $ source environ
    or
    $ echo "source ~/nodem/resources/environ" >> ~/.profile

If you did not install Nodem in your home directory, you will need to fix
the paths in the environ file. If you don't source the environ file, than
you will need to put a copy of node.m into a directory that is specified
in your $gtmroutines routine path, so that the GT.M shared library can
find it. It is located in the ~/nodem/resources/ directory. Again, if you
don't source the environ file, than you will need to define the GTMCI
environment variable, and point it at the file calltab.ci, located in the
~/nodem/resources/ directory. E.g.

    $ export GTMCI=~/nodem/resources/calltab.ci

You can clone the repository with this command..

    $ git clone git://github.com/dlwicksell/nodem.git

You can also install it via npm with this command..

    $ npm install nodem

I hope you enjoy the Nodem package. If you have any questions, feature
requests, or bugs to report, please contact David Wicksell <dlw@linux.com>

### See Also ###

* The [GT.M][] implementation of MUMPS.

[GT.M]: http://sourceforge.net/projects/fis-gtm/
[Globals]: http://globalsdb.org/
[Caché]: http://www.intersystems.com/cache/
[Docs]: http://docs.intersystems.com/documentation/cache/20122/pdfs/BXJS.pdf

## APIs ##

* *about* *version* - Display version information
* *close* - Close the database
* *data* - Call the $DATA intrinsic function
* *function* - Call an extrinisic function
* *get* - Call the $GET intrinsic function
* *global_directory* - List the names of the globals in the database
* *increment* - Call the $INCREMENT intrinsic function
* *kill* - Delete a global node, and all of its children
* *lock* - Not yet implemented
* *merge* - Not yet implemented
* *next* *order* - Call the $ORDER intrinsic function
* *next_node* - Not yet implemented
* *open* - Open the database
* *previous* - Call the $ORDER intrinsic function in reverse
* *previous_node* - Not yet implemented
* *retrieve* - Not yet implemented
* *set* - Set a global node to a new value
* *unlock* - Not yet implemented
* *update* - Not yet implemented

[![githalytics.com alpha](https://cruel-carlota.pagodabox.com/a637d9ddd6ebc0e7f45f49ca0c2ea701 "githalytics.com")](http://githalytics.com/dlwicksell/nodem)
