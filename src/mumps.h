/*
 * Package:    NodeM
 * File:       mumps.h
 * Summary:    A YottaDB/GT.M database driver and binding for Node.js
 * Maintainer: David Wicksell <dlw@linux.com>
 *
 * Written by David Wicksell <dlw@linux.com>
 * Copyright © 2015-2020 Fourth Watch Software LC
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

#include "utility.h"

#include <node.h>
#include <node_object_wrap.h>
#include <uv.h>

extern "C" {
#    include <gtmxc_types.h>
}

#if NODEM_SIMPLE_API == 1
extern "C" {
#    include <libydberrors.h>
}

// SimpleAPI && Node.js <= 5 don't work quite right with large amounts of data [zwrite.js] (this is a hack)
#    if NODE_MAJOR_VERSION <= 5
#        undef NODEM_SIMPLE_API
#        define NODEM_SIMPLE_API 0
#    endif
#endif

#include <cstring>

#include <string>
#include <vector>

#if YDB_RELEASE >= 124
#    define YDB_NODE_END YDB_ERR_NODEEND
#endif

#if NODEM_YDB == 1
#    define NODEM_DB "YottaDB"
#else
#    define NODEM_DB "GT.M"
#endif

#define NODEM_MAJOR_VERSION 0
#define NODEM_MINOR_VERSION 18
#define NODEM_PATCH_VERSION 0

#define NODEM_STRING(number) NODEM_STRINGIFY(number)
#define NODEM_STRINGIFY(number) #number

#define NODEM_VERSION NODEM_STRING(NODEM_MAJOR_VERSION) "." NODEM_STRING(NODEM_MINOR_VERSION) "." NODEM_STRING(NODEM_PATCH_VERSION)

#define ERR_LEN 2048
#define RES_LEN 1048576

namespace nodem {

enum mode_t {
    STRICT,
    STRING,
    CANONICAL
};

enum debug_t {
    OFF,
    LOW,
    MEDIUM,
    HIGH
};

extern uv_mutex_t mutex_g;
extern int save_stdout_g;
extern bool utf8_g;
extern bool auto_relink_g;
extern enum mode_t mode_g;
extern enum debug_t debug_g;

/*
 * @class nodem::Gtm
 * @summary Wrap the Nodem API in a C++ class
 * @constructor Gtm
 * @destructor ~Gtm
 * @method {class} Init
 * @method {class} {private} open
 * @method {class} {private} configure
 * @method {class} {private} close
 * @method {class} {private} help
 * @method {class} {private} version
 * @method {class} {private} data
 * @method {class} {private} get
 * @method {class} {private} set
 * @method {class} {private} kill
 * @method {class} {private} merge
 * @method {class} {private} order
 * @method {class} {private} previous
 * @method {class} {private} next_node
 * @method {class} {private} previous_node
 * @method {class} {private} increment
 * @method {class} {private} lock
 * @method {class} {private} unlock
 * @method {class} {private} function
 * @method {class} {private} procedure
 * @method {class} {private} global_directory
 * @method {class} {private} local_directory
 * @method {class} {private} retrieve
 * @method {class} {private} update
 * @method {class} {private} New
 */
class Gtm : public node::ObjectWrap {
public:
    Gtm()
    {
        if (getpid() == gettid())
            uv_mutex_init(&mutex_g);

        return;
    }

    ~Gtm()
    {
        if (getpid() == gettid())
            uv_mutex_destroy(&mutex_g);

        return;
    }

    static void Init(v8::Local<v8::Object>);

private:
    static void open(const v8::FunctionCallbackInfo<v8::Value>&);
    static void configure(const v8::FunctionCallbackInfo<v8::Value>&);
    static void close(const v8::FunctionCallbackInfo<v8::Value>&);
    static void help(const v8::FunctionCallbackInfo<v8::Value>&);
    static void version(const v8::FunctionCallbackInfo<v8::Value>&);
    static void data(const v8::FunctionCallbackInfo<v8::Value>&);
    static void get(const v8::FunctionCallbackInfo<v8::Value>&);
    static void set(const v8::FunctionCallbackInfo<v8::Value>&);
    static void kill(const v8::FunctionCallbackInfo<v8::Value>&);
    static void merge(const v8::FunctionCallbackInfo<v8::Value>&);
    static void order(const v8::FunctionCallbackInfo<v8::Value>&);
    static void previous(const v8::FunctionCallbackInfo<v8::Value>&);
    static void next_node(const v8::FunctionCallbackInfo<v8::Value>&);
    static void previous_node(const v8::FunctionCallbackInfo<v8::Value>&);
    static void increment(const v8::FunctionCallbackInfo<v8::Value>&);
    static void lock(const v8::FunctionCallbackInfo<v8::Value>&);
    static void unlock(const v8::FunctionCallbackInfo<v8::Value>&);
    static void function(const v8::FunctionCallbackInfo<v8::Value>&);
    static void procedure(const v8::FunctionCallbackInfo<v8::Value>&);
    static void global_directory(const v8::FunctionCallbackInfo<v8::Value>&);
    static void local_directory(const v8::FunctionCallbackInfo<v8::Value>&);
    static void retrieve(const v8::FunctionCallbackInfo<v8::Value>&);
    static void update(const v8::FunctionCallbackInfo<v8::Value>&);

    static void New(const v8::FunctionCallbackInfo<v8::Value>&);
}; // @end nodem::Gtm class

/*
 * @class nodem::GtmValue
 * @summary Convert UTF-8 encoded buffer to/from a byte encoded buffer
 * @constructor GtmValue
 * @destructor ~GtmValue
 * @method {instance} to_byte
 * @method {class} from_byte
 * @method {Local<String>} value
 * @method {int} size
 * @method {uint8_t*} buffer
 */
class GtmValue {
public:
#if NODE_MAJOR_VERSION >= 8
    explicit GtmValue(v8::Local<v8::Value>& val)
    {
        v8::Isolate* isolate = v8::Isolate::GetCurrent();

        v8::TryCatch try_catch(isolate);
        try_catch.SetVerbose(true);

        v8::MaybeLocal<v8::String> maybe_string = val->ToString(isolate->GetCurrentContext());

        if (maybe_string.IsEmpty() || try_catch.HasCaught()) {
            value = v8::String::Empty(isolate);
        } else {
            value = maybe_string.ToLocalChecked();
        }

        size = value->Length() + 1;
        buffer = new uint8_t[size];

        return;
    }
#else
    explicit GtmValue(v8::Local<v8::Value>& val) :
        value {val->ToString()},
        size {value->Length() + 1},
        buffer {new uint8_t[size]}
    {
        return;
    }
#endif

    ~GtmValue()
    {
        delete[] buffer;
        return;
    }

    gtm_char_t* to_byte(void);
    static v8::Local<v8::String> from_byte(gtm_char_t buffer[]);

private:
    v8::Local<v8::String> value;
    int size;
    uint8_t* buffer;
}; // @end nodem::GtmValue class

/*
 * @class nodem::GtmState
 * @summary Holds global state data in a form that can be accessed by multiple threads safely
 * @constructor GtmState
 * @destructor ~GtmState
 * @member {bool} utf8
 * @member {bool} auto_relink
 * @member {pid_t} pid
 * @member {pid_t} tid
 * @member {gtm_char_t[]} error
 * @member {gtm_char_t[]} result
 * @member {enum mode_t} mode
 * @member {enum debug_t} debug
 * @member {struct sigaction} signal_attr
 * @member {Persistent<Function>} constructor_p
 * @method {class} {private} DeleteState
 * @member {Persistent<Object>} {private} exports_p
 */
class GtmState {
public:
    GtmState(v8::Isolate* isolate, v8::Local<v8::Object> exports) :
#if YDB_RELEASE >= 126
        reset_handler {false},
#endif
        utf8 {utf8_g},
        auto_relink {auto_relink_g},
        mode {mode_g},
        debug {debug_g}
    {
        exports_p.Reset(isolate, exports);
#if NODE_MAJOR_VERSION >= 3
        exports_p.SetWeak(this, DeleteState, v8::WeakCallbackType::kParameter);
#else
        exports_p.SetWeak<GtmState>(this, DeleteState);
#endif
        pid = getpid();
        tid = gettid();

        return;
    }

    ~GtmState()
    {
        if (!exports_p.IsEmpty()) {
            exports_p.ClearWeak();
            exports_p.Reset();
        }

        return;
    }

#if YDB_RELEASE >= 126
    bool                         reset_handler;
#endif
    bool                         utf8;
    bool                         auto_relink;
    pid_t                        pid;
    pid_t                        tid;
    gtm_char_t                   error[ERR_LEN];
    gtm_char_t                   result[RES_LEN];
    enum mode_t                  mode;
    enum debug_t                 debug;
    struct sigaction             signal_attr;
    v8::Persistent<v8::Function> constructor_p;

private:
#if NODE_MAJOR_VERSION >= 3
    static void DeleteState(const v8::WeakCallbackInfo<GtmState>& info)
    {
        delete info.GetParameter();
#else
    static void DeleteState(const v8::WeakCallbackData<v8::Object, GtmState>& data)
    {
        delete data.GetParameter();
#endif
        return;
    }

    v8::Persistent<v8::Object> exports_p;
}; // @end nodem::GtmState class

/*
 * @struct nodem::GtmBaton
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
 * @member {int32_t} node_only
 * @member {uint32_t} relink
 * @member {gtm_double_t} option
 * @member {gtm_status_t} status
 * @member {gtm_char_t*} error
 * @member {gtm_char_t*} result
 * @member {gtm_status_t *(GtmBaton*)} gtm_function
 * @member {Local<Value> *(GtmBaton*)} ret_function
 * @member {GtmState*} gtm_state
 */
struct GtmBaton {
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
    int32_t                      node_only;
    uint32_t                     relink;
    gtm_double_t                 option;
    gtm_status_t                 status;
    gtm_char_t*                  error;
    gtm_char_t*                  result;
    gtm_status_t                 (*gtm_function)(GtmBaton*);
    v8::Local<v8::Value>         (*ret_function)(GtmBaton*);
    GtmState*                    gtm_state;
}; // @end nodem::GtmBaton struct

} // @end namespace nodem

#endif // @end MUMPS_H
