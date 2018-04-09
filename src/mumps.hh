/*
 * Package:    NodeM
 * File:       mumps.hh
 * Summary:    A YottaDB/GT.M database driver and binding for Node.js
 * Maintainer: David Wicksell <dlw@linux.com>
 *
 * Written by David Wicksell <dlw@linux.com>
 * Copyright © 2015-2018 Fourth Watch Software LC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License (AGPL)
 * as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see http://www.gnu.org/licenses/.
 */

#ifndef MUMPS_HH
#define MUMPS_HH

#include <node.h>
#include <node_object_wrap.h>

namespace nodem {

#define NODEM_MAJOR_VERSION  0
#define NODEM_MINOR_VERSION  10
#define NODEM_PATCH_VERSION  1

#ifdef LIBYOTTADB_TYPES_H
    #define NODEM_DB "YottaDB"
#else
    #define NODEM_DB "GT.M"
#endif

#define NODEM_STRING(number)    NODEM_STRINGIFY(number)
#define NODEM_STRINGIFY(number) #number

#define NODEM_VERSION NODEM_STRING(NODEM_MAJOR_VERSION) "." NODEM_STRING(NODEM_MINOR_VERSION) "." NODEM_STRING(NODEM_PATCH_VERSION)

class Gtm :public node::ObjectWrap
{
    public:
        static void Init(v8::Local<v8::Object>);

    private:
        static void New(const v8::FunctionCallbackInfo<v8::Value>&);

        static void close(const v8::FunctionCallbackInfo<v8::Value>&);
        static void data(const v8::FunctionCallbackInfo<v8::Value>&);
        static void function(const v8::FunctionCallbackInfo<v8::Value>&);
        static void get(const v8::FunctionCallbackInfo<v8::Value>&);
        static void global_directory(const v8::FunctionCallbackInfo<v8::Value>&);
        static void help(const v8::FunctionCallbackInfo<v8::Value>&);
        static void increment(const v8::FunctionCallbackInfo<v8::Value>&);
        static void kill(const v8::FunctionCallbackInfo<v8::Value>&);
        static void local_directory(const v8::FunctionCallbackInfo<v8::Value>&);
        static void lock(const v8::FunctionCallbackInfo<v8::Value>&);
        static void merge(const v8::FunctionCallbackInfo<v8::Value>&);
        static void next_node(const v8::FunctionCallbackInfo<v8::Value>&);
        static void open(const v8::FunctionCallbackInfo<v8::Value>&);
        static void order(const v8::FunctionCallbackInfo<v8::Value>&);
        static void previous(const v8::FunctionCallbackInfo<v8::Value>&);
        static void previous_node(const v8::FunctionCallbackInfo<v8::Value>&);
        static void procedure(const v8::FunctionCallbackInfo<v8::Value>&);
        static void retrieve(const v8::FunctionCallbackInfo<v8::Value>&);
        static void set(const v8::FunctionCallbackInfo<v8::Value>&);
        static void unlock(const v8::FunctionCallbackInfo<v8::Value>&);
        static void update(const v8::FunctionCallbackInfo<v8::Value>&);
        static void version(const v8::FunctionCallbackInfo<v8::Value>&);
}; // @end classs Gtm

} // @end namespace nodem

#endif // @end MUMPS_HH
