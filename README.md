# NodeM #

## A Node.js binding to the GT.M language and database ##

Version 0.1.0 - 2013 Jan 23

## Copyright and License ##

Addon Module written and maintained by David Wicksell <dlw@linux.com>  
Copyright © 2012,2013 Fourth Watch Software, LC

Licensing information forthcoming.

***

## Summary and Info ##

NodeM is an Open Source Addon module for Node.js that integrates Node.js with
the GT.M database, providing access to GT.M's database from Javascript, via
GT.M's C Call-in Interface. From Node.js you can perform the basic primitive
Global Database handling operations and also invoke GT.M/Mumps functions.
Although designed exclusively for use with GT.M, NodeM aims to be API-compatible
with the in-process Node.js interface for [Globals][] and Cache, at least for
the APIs that have been implemented in NodeM. As such, please refer to the
Globals Node.js [API documentation][Documentation] for further details on how to
use those APIs.

[Globals]: http://globalsdb.org/
[Documentation]: http://globalsdb.org/api-nodejs/Node.js%20Interface%20-%20User%20Guide%20-%20e1.5%20-%20v2012.2.0.580.x.pdf
