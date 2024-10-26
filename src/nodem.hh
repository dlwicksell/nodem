/*
 * Package:    NodeM
 * File:       nodem.hh
 * Summary:    A YottaDB/GT.M database driver and binding for Node.js
 * Maintainer: David Wicksell <dlw@linux.com>
 *
 * Written by David Wicksell <dlw@linux.com>
 * Copyright © 2015-2024 Fourth Watch Software LC
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License (AGPL) as published
 * by the Free Software Foundation, either version 3 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see http://www.gnu.org/licenses/.
 */

#ifndef NODEM_HH
#   define NODEM_HH

#include "utility.hh"
#include <node.h>
#include <node_object_wrap.h>
#include <node_buffer.h>
#include <uv.h>

extern "C" {
#include <gtmxc_types.h>
}

#if NODEM_SIMPLE_API == 1
extern "C" {
#   include <libydberrors.h>
}

//  SimpleAPI && Node.js <= 5 don't work quite right with large amounts of data [zwrite.js] (this is a hack)
#   if NODE_MAJOR_VERSION <= 5
#       undef NODEM_SIMPLE_API
#       define NODEM_SIMPLE_API 0
#   endif
#endif

#include <cstring>
#include <string>
#include <vector>

#if NODEM_YDB == 1
#   define NODEM_DB "YottaDB"
#else
#   define NODEM_DB "GT.M"
#endif

#if YDB_RELEASE >= 124
#   define YDB_NODE_END YDB_ERR_NODEEND
#endif

#define NODEM_MAJOR_VERSION 0
#define NODEM_MINOR_VERSION 20
#define NODEM_PATCH_VERSION 9

#define NODEM_STRINGIFY(number) #number
#define NODEM_STRING(number)    NODEM_STRINGIFY(number)
#define NODEM_VERSION NODEM_STRING(NODEM_MAJOR_VERSION) "." NODEM_STRING(NODEM_MINOR_VERSION) "." NODEM_STRING(NODEM_PATCH_VERSION)

#define ERR_LEN 2048
#define RES_LEN 1048576

namespace nodem {

typedef enum {
    STRING,
    CANONICAL
} mode_t;

typedef enum {
    OFF,
    LOW,
    MEDIUM,
    HIGH
} debug_t;

typedef enum {
    CLOSED,
    NOT_OPEN,
    OPEN
} nodem_state_t;

extern uv_mutex_t    mutex_g;
extern mode_t        mode_g;
extern debug_t       debug_g;
extern nodem_state_t nodem_state_g;
extern bool          utf8_g;
extern bool          auto_relink_g;
extern int           save_stdout_g;

/*
 * @class nodem::Nodem
 * @summary Wrap the Nodem API in a C++ class
 * @constructor Nodem
 * @destructor ~Nodem
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
 * @method {class} {private} transaction
 * @method {class} {private} function
 * @method {class} {private} procedure
 * @method {class} {private} global_directory
 * @method {class} {private} local_directory
 * @method {class} {private} retrieve
 * @method {class} {private} update
 * @method {class} {private} New
 * @method {class} {private} restart
 * @method {class} {private} rollback
 * @member {int} {private} tp_restart
 * @member {int} {private} tp_rollback
 */
class Nodem : public node::ObjectWrap {
public:
    Nodem()
    {
        if (getpid() == gettid()) uv_mutex_init(&mutex_g);
        return;
    }

    ~Nodem()
    {
        if (getpid() == gettid()) uv_mutex_destroy(&mutex_g);
        return;
    }

    static void Init(v8::Local<v8::Object>);

private:
    static void New(const v8::FunctionCallbackInfo<v8::Value>&);
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
    static void next_node_deprecated(const v8::FunctionCallbackInfo<v8::Value>&);
    static void previous_node(const v8::FunctionCallbackInfo<v8::Value>&);
    static void previous_node_deprecated(const v8::FunctionCallbackInfo<v8::Value>&);
    static void increment(const v8::FunctionCallbackInfo<v8::Value>&);
    static void lock(const v8::FunctionCallbackInfo<v8::Value>&);
    static void unlock(const v8::FunctionCallbackInfo<v8::Value>&);
#if NODEM_SIMPLE_API == 1
    static void transaction(const v8::FunctionCallbackInfo<v8::Value>&);
#endif
    static void function(const v8::FunctionCallbackInfo<v8::Value>&);
    static void procedure(const v8::FunctionCallbackInfo<v8::Value>&);
    static void global_directory(const v8::FunctionCallbackInfo<v8::Value>&);
    static void global_directory_deprecated(const v8::FunctionCallbackInfo<v8::Value>&);
    static void local_directory(const v8::FunctionCallbackInfo<v8::Value>&);
    static void local_directory_deprecated(const v8::FunctionCallbackInfo<v8::Value>&);
    static void retrieve(const v8::FunctionCallbackInfo<v8::Value>&);
    static void update(const v8::FunctionCallbackInfo<v8::Value>&);
#if NODEM_SIMPLE_API == 1
#   if NODE_MAJOR_VERSION >= 23
    static void restart(const v8::FunctionCallbackInfo<v8::Value>&);
    static void rollback(const v8::FunctionCallbackInfo<v8::Value>&);
#   else
    static void restart(v8::Local<v8::String>, const v8::PropertyCallbackInfo<v8::Value>&);
    static void rollback(v8::Local<v8::String>, const v8::PropertyCallbackInfo<v8::Value>&);
#   endif

    int tp_restart = YDB_TP_RESTART;
    int tp_rollback = YDB_TP_ROLLBACK;
#endif
}; // @end nodem::Nodem class

/*
 * @class nodem::NodemValue
 * @summary Convert UTF-8 encoded buffer to/from a byte encoded buffer
 * @constructor NodemValue
 * @destructor ~NodemValue
 * @method {instance} to_byte
 * @method {class} from_byte
 * @member {Local<String>} value
 * @member {int} size
 * @member {uint8_t*} buffer
 */
class NodemValue {
public:
#if NODE_MAJOR_VERSION >= 8
    explicit NodemValue(v8::Local<v8::Value>& val)
    {
        v8::Isolate* isolate = v8::Isolate::GetCurrent();
        v8::TryCatch try_catch(isolate);
        try_catch.SetVerbose(true);

        v8::MaybeLocal<v8::String> maybe_string = val->ToString(isolate->GetCurrentContext());

        if (maybe_string.IsEmpty() || try_catch.HasCaught()) {
            isolate->ThrowException(try_catch.Exception());
            try_catch.Reset();

            value = v8::String::Empty(isolate);
        } else {
            value = maybe_string.ToLocalChecked();
        }

        size = value->Length() + 1;
        buffer = new uint8_t[size];
        return;
    }
#else
    explicit NodemValue(v8::Local<v8::Value>& val) :
        value {val->ToString()},
        size {value->Length() + 1},
        buffer {new uint8_t[size]}
    {
        return;
    }
#endif

    ~NodemValue()
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
}; // @end nodem::NodemValue class

/*
 * @class nodem::NodemState
 * @summary Holds global state data in a form that can be accessed by multiple threads safely
 * @constructor NodemState
 * @destructor ~NodemState
 * @member {bool} utf8
 * @member {bool} auto_relink
 * @member {pid_t} pid
 * @member {pid_t} tid
 * @member {gtm_char_t[]} error
 * @member {gtm_char_t[]} result
 * @member {mode_t} mode
 * @member {debug_t} debug
 * @member {struct sigaction} signal_attr
 * @member {Persistent/Global<Function>} constructor_p
 * @method {class} {private} DeleteState
 * @member {Persistent/Global<Object>} {private} exports_p
 */
class NodemState {
public:
    NodemState(v8::Isolate* isolate, v8::Local<v8::Object> exports) :
#if YDB_RELEASE >= 126
        reset_handler {false},
#endif
        utf8 {utf8_g},
        auto_relink {auto_relink_g},
        tp_level {0},
        tp_restart {0},
        mode {mode_g},
        debug {debug_g}
    {
        exports_p.Reset(isolate, exports);
#if NODE_MAJOR_VERSION >= 3
        exports_p.SetWeak(this, DeleteState, v8::WeakCallbackType::kParameter);
#else
        exports_p.SetWeak<NodemState>(this, DeleteState);
#endif
        pid = getpid();
        tid = gettid();

        return;
    }

    ~NodemState()
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
    short                        tp_level;
    short                        tp_restart;
    gtm_char_t                   error[ERR_LEN];
    gtm_char_t                   result[RES_LEN];
    mode_t                       mode;
    debug_t                      debug;
    struct sigaction             signal_attr;
#if NODE_MAJOR_VERSION >= 3
    v8::Global<v8::Function>     constructor_p;
#else
    v8::Persistent<v8::Function> constructor_p;
#endif

private:
#if NODE_MAJOR_VERSION >= 3
    static void DeleteState(const v8::WeakCallbackInfo<NodemState>& info)
    {
        delete info.GetParameter();
        return;
    }
#else
    static void DeleteState(const v8::WeakCallbackData<v8::Object, NodemState>& data)
    {
        delete data.GetParameter();
        return;
    }
#endif

#if NODE_MAJOR_VERSION >= 3
    v8::Global<v8::Object> exports_p;
#else
    v8::Persistent<v8::Object> exports_p;
#endif
}; // @end nodem::NodemState class

/*
 * @struct nodem::NodemBaton
 * @summary Common structure to transfer data between main thread and worker threads when Nodem APIs are called asynchronously
 * @member {uv_work_t} request
 * @member {Persistent/Global<Function>} callback_p
 * @member {Persistent/Global<Function>} object_p
 * @member {Persistent/Global<Function>} arguments_p
 * @member {Persistent/Global<Function>} data_p
 * @member {string} name
 * @member {string} to_name
 * @member {string} args
 * @member {string} to_args
 * @member {string} value
 * @member {vector<string>} subs_array
 * @member {mode_t} mode
 * @member {bool} async
 * @member {bool} local
 * @member {bool} position
 * @member {bool} routine
 * @member {bool} node_only
 * @member {uint32_t} relink
 * @member {gtm_double_t} option
 * @member {gtm_status_t} status
 * @member {gtm_uint_t} info
 * @member {gtm_char_t*} error
 * @member {gtm_char_t*} result
 * @member {gtm_status_t *(NodemBaton*)} nodem_function
 * @member {Local<Value> *(NodemBaton*)} ret_function
 * @member {NodemState*} nodem_state
 */
struct NodemBaton {
    uv_work_t                    request;
#if NODE_MAJOR_VERSION >= 3
    v8::Global<v8::Function>     callback_p;
    v8::Global<v8::Object>       object_p;
    v8::Global<v8::Value>        arguments_p;
    v8::Global<v8::Value>        data_p;
#else
    v8::Persistent<v8::Function> callback_p;
    v8::Persistent<v8::Object>   object_p;
    v8::Persistent<v8::Value>    arguments_p;
    v8::Persistent<v8::Value>    data_p;
#endif
    std::string                  name;
    std::string                  to_name;
    std::string                  args;
    std::string                  to_args;
    std::string                  value;
    std::vector<std::string>     subs_array;
    mode_t                       mode;
    bool                         async;
    bool                         local;
    bool                         position;
    bool                         routine;
    bool                         node_only;
    uint32_t                     relink;
    gtm_double_t                 option;
    gtm_status_t                 status;
    gtm_uint_t                   info;
    gtm_char_t*                  error;
    gtm_char_t*                  result;
    gtm_status_t                 (*nodem_function)(NodemBaton*);
    v8::Local<v8::Value>         (*ret_function)(NodemBaton*);
    NodemState*                  nodem_state;
}; // @end nodem::NodemBaton struct

} // @end namespace nodem

#endif // @end NODEM_HH
