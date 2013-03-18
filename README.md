# NodeM #

## A Node.js binding to the GT.M language and database ##

Version 0.2.0 - 2013 Mar 18

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

## Summary and Info ##

Nodem is an Open Source Addon module for Node.js that integrates Node.js
with the [GT.M][] database, providing access to GT.M's database from
Javascript, via GT.M's C Call-in Interface. From Node.js you can perform
the basic primitive Global Database handling operations and also invoke
GT.M/Mumps functions. Although designed exclusively for use with GT.M,
Nodem aims to be API-compatible with the in-process Node.js interface for
[Globals][] and [Cache][], at least for the APIs that have been
implemented in Nodem. As such, please refer to the Globals Node.js [API
documentation][Documentation] for further details on how to use those
APIs.

## Installation ##

There are a few things to be aware of in order to use the Nodem library.
In order to use Nodem you will need to have GT.M installed and configured
correctly. Including setting up your environment with the required GT.M
environment variables. You will also need to have Node.js installed.

These instructions will assume that the nodem repository has been
installed in your home directory. The paths will likely be different if
you have installed this with npm. You will need to change directories to
~/nodem/lib/ and copy the correct version of mumps.node for your system
architecture (mumps.node_i686 for 32-bit or mumps.node_x8664 for 64-bit)
to mumps.node in the ~/nodem/lib/ directory. By default there is a
mumps.node already there, which is a copy of the 64-bit version. E.g.

    $ cd ~/nodem/lib
    $ cp mumps.node_x8664 mumps.node
    $ cd -

You will also have to move a copy of libgtmshr.so (GT.M shared runtime
library) into a directory that will be searched by the linker/loader when
mumps.node links against it, if it isn't already located in one. Then you
will have to run ldconfig as root to rebuild the linker's cache. You will
find libgtmshr.so bundled with GT.M where ever you installed it. You will
probably want to use /usr/local/lib/ and you could create a symbolic link
to it if you choose. E.g.

    $ sudo -i
    # cd /usr/local/lib
    # ln -s /opt/lsb-gtm/6.0-001_x8664/libgtmshr.so
    # ldconfig
    # exit

You may have to add the /usr/local/lib/ directory to /etc/ld.so.conf or
create an /etc/ld.so.conf.d/libc.conf file and add it there, and then run
ldconfig as root. Instead, you can avoid having to copy or link to the
library in a directory searched by the linker, by setting the environment
variable, LD_LIBRARY_PATH, to point to it, as the linker will search there
first.

In addition you will need to set a few environment variables in order for
GT.M to find the call-in table and the MUMPS routine that it maps to. The
Nodem package supplies a sample environment file, called environ. It sets
LD_LIBRARY_PATH to $gtm_dist by default, which may not be wanted. It is
located in ~/nodem/resources/ and can simply be sourced into your working
environment, either directly, or from your own environment scripts or
profile script. E.g.

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

It is usually advisable not to export LD_LIBRARY_PATH into your
environment, so you might want to define it right before calling node.
E.g.

    $ LD_LIBRARY_PATH=${gtm_dist} node 
    or
    $ LD_LIBRARY_PATH=${gtm_dist} node test

As you can see though, that is more of a pain.

You can clone the repository with this command..

    $ git clone git://github.com/dlwicksell/nodem.git

You can also install it via npm with this command..

    $ npm install nodem

I hope you enjoy the Nodem package. If you have any questions, feature
requests, or bugs to report, please contact David Wicksell <dlw@linux.com>

[GT.M]: http://sourceforge.net/projects/fis-gtm/
[Globals]: http://globalsdb.org/
[Cache]: http://www.intersystems.com/cache/
[Documentation]: http://globalsdb.org/api-nodejs/Node.js%20Interface%20-%20User%20Guide%20-%20e1.5%20-%20v2012.2.0.580.x.pdf

[![githalytics.com alpha](https://cruel-carlota.pagodabox.com/a637d9ddd6ebc0e7f45f49ca0c2ea701 "githalytics.com")](http://githalytics.com/dlwicksell/nodem)
