/*
 * Package:    NodeM
 * File:       mumps.h
 * Summary:    A YottaDB/GT.M database driver and binding for Node.js
 * Maintainer: David Wicksell <dlw@linux.com>
 *
 * Written by David Wicksell <dlw@linux.com>
 * Copyright © 2015-2019 Fourth Watch Software LC
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

#ifndef MUMPS_H
#define MUMPS_H

extern "C" {
#include <gtmxc_types.h>

#if YDB_SIMPLE_API == 1
#include <libydberrors.h>
#endif
}

#include <node.h>
#include <node_object_wrap.h>
#include <uv.h>
#include <string>
#include <vector>

namespace nodem {

#if YDB_IMPLEMENTATION == 1
#define NODEM_DB "YottaDB"
#else
#define NODEM_DB "GT.M"
#endif

#define NODEM_MAJOR_VERSION 0
#define NODEM_MINOR_VERSION 14
#define NODEM_PATCH_VERSION 0

#define NODEM_STRING(number)    NODEM_STRINGIFY(number)
#define NODEM_STRINGIFY(number) #number

#define NODEM_VERSION NODEM_STRING(NODEM_MAJOR_VERSION) "." NODEM_STRING(NODEM_MINOR_VERSION) "." NODEM_STRING(NODEM_PATCH_VERSION)

#define MSG_LEN 2048
#define RET_LEN 1048576

extern enum debug_t {OFF, LOW, MEDIUM, HIGH} debug_g;
extern uv_mutex_t mutex_g;

/*
 * @struct Baton
 * @summary Common structure to transfer data between main thread and worker threads when Nodem APIs are called asynchronously
 * @member {uv_work_t} request
 * @member {Persistent<Function>} callback_p
 * @member {Persistent<Function>} arguments_p
 * @member {Persistent<Function>} data_p
 * @member {string} name
 * @member {string} args
 * @member {string} value
 * @member {vector<string>} subs_array
 * @member {mode_t} mode
 * @member {bool} async
 * @member {bool} local
 * @member {bool} position
 * @member {bool} routine
 * @member {uint32_t} node_only
 * @member {uint32_t} relink
 * @member {gtm_status_t} status
 * @member {gtm_char_t} msg_bug
 * @member {gtm_char_t} ret_buf
 * @member {gtm_status_t} function
 * @member {Local<Value>} function_return
 */
struct Baton {
    uv_work_t                    request;
    v8::Persistent<v8::Function> callback_p;
    v8::Persistent<v8::Value>    arguments_p;
    v8::Persistent<v8::Value>    data_p;
    std::string                  name;
    std::string                  args;
    std::string                  value;
    std::vector<std::string>     subs_array;
    mode_t                       mode;
    bool                         async;
    bool                         local;
    bool                         position;
    bool                         routine;
    uint32_t                     node_only;
    uint32_t                     relink;
    gtm_status_t                 status;
    gtm_char_t                   msg_buf[MSG_LEN];
    gtm_char_t                   ret_buf[RET_LEN];
    gtm_status_t                 (*function)(Baton*);
    v8::Local<v8::Value>         (*function_return)(Baton*);
}; // @end Baton struct

/*
 * @class GtmValue
 * @summary Convert UTF-8 encoded buffer to/from a byte encoded buffer
 * @constructor GtmValue
 * @destructor ~GtmValue
 * @method {instance} to_byte
 * @method {class} from_byte
 */
class GtmValue {
public:
    explicit GtmValue(v8::Local<v8::Value>& val) : value{val->ToString()}, size{value->Length() + 1}, buffer{new uint8_t[size]} {}
    ~GtmValue() {delete buffer;}

    gtm_char_t* to_byte(void);
    static v8::Local<v8::String> from_byte(gtm_char_t buffer[]);

private:
    v8::Local<v8::String> value;
    int size;
    uint8_t* buffer;
}; // @end GtmValue class

/*
 * @class Gtm
 * @summary Wrap the Nodem API in a C++ class
 * @constructor {private} Gtm
 * @destructor {private} ~Gtm
 * @method {class} Init
 * @method {class} {private} New
 * @method {class} {private} close
 * @method {class} {private} data
 * @method {class} {private} function
 * @method {class} {private} get
 * @method {class} {private} global_directory
 * @method {class} {private} help
 * @method {class} {private} increment
 * @method {class} {private} kill
 * @method {class} {private} local_directory
 * @method {class} {private} lock
 * @method {class} {private} merge
 * @method {class} {private} next_node
 * @method {class} {private} open
 * @method {class} {private} order
 * @method {class} {private} previous
 * @method {class} {private} previous_node
 * @method {class} {private} procedure
 * @method {class} {private} retrieve
 * @method {class} {private} set
 * @method {class} {private} unlock
 * @method {class} {private} update
 * @method {class} {private} version
 * @method {class} {private} constructor_p
 */
class Gtm : public node::ObjectWrap {
public:
    static void Init(v8::Local<v8::Object>);

private:
    Gtm() {uv_mutex_init(&mutex_g);}
    ~Gtm() {uv_mutex_destroy(&mutex_g);}

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

    static v8::Persistent<v8::Function> constructor_p;
}; // @end Gtm class

} // @end namespace nodem

#endif // @end MUMPS_H
