/*
 * mumps.cc - A GT.M database driver for Node.js
 *
 * Written by David Wicksell <dlw@linux.com>
 * Copyright © 2012-2017 Fourth Watch Software LC
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


#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/types.h>

extern "C" {
    #include <gtmxc_types.h>
}

#include <string>
#include <iostream>

#include "mumps.hh"

using namespace std;
using namespace v8;
using namespace node;

#define BUF_LEN (2048 + 1)
#define RET_LEN ((1024 * 1024) + 1)
#define WIDTH 9

#define CANONICAL 0
#define STRICT 1

gtm_char_t msg_buf[BUF_LEN];
gtm_char_t ret_buf[RET_LEN];
gtm_status_t stat_buf;

bool is_utf = false;

struct termios term_attr;
struct sigaction signal_attr;

static int gtm_is_open = 0;

static gtm_uint_t mode = CANONICAL;

static uint32_t auto_relink = 0;
static uint32_t reset_term = 0;

Persistent<Function> constructor;


static void catch_intr(int signum)
{
    gtm_exit();

    if (reset_term) {
        term_attr.c_iflag |= ICRNL;
        term_attr.c_lflag |= (ICANON | ECHO);
    }

    if (tcsetattr(STDIN_FILENO, TCSANOW, &term_attr) == -1)
        std::cerr << strerror(errno) << std::endl;

    exit(EXIT_FAILURE);
} //End of catch_intr


Handle<Object> gtm_status(gtm_char_t *msg_buf)
{
    ISOLATE_CURRENT;

    char *error_msg;
    char *e_code = strtok_r(msg_buf, ",", &error_msg);

    uint32_t error_code = atoi(e_code);

    Local<Object> result = Object::New(ISOLATE);

    result->Set(STRING("ok"), NUMBER(0));
    result->Set(STRING("errorCode"), NUMBER(error_code));
    result->Set(STRING("errorMessage"), STRING(error_msg));

    return result;
} //End of gtm_status


RETURN_DECL Gtm::open(ARGUMENTS args)
{
    ISOLATE_CURRENT;
    SCOPE_HANDLE;

    char *char_set;
    char *a_relink;
    Local<Object> object;

    if (gtm_is_open == -1) {
        EXCEPTION(Exception::Error(STRING("GT.M database cannot be reopened")));

        SCOPE_SET(args, Undefined(ISOLATE));
        SCOPE_RETURN(Undefined(ISOLATE));
    } else if (gtm_is_open == 1) {
        EXCEPTION(Exception::Error(STRING("GT.M database already open")));

        SCOPE_SET(args, Undefined(ISOLATE));
        SCOPE_RETURN(Undefined(ISOLATE));
    }

    char_set = getenv("gtm_chset");

    if (char_set == NULL) {
        is_utf = false;
    } else if (strcmp(char_set, "utf-8") == 0) {
        is_utf = true;
    } else if (strcmp(char_set, "Utf-8") == 0) {
        is_utf = true;
    } else if (strcmp(char_set, "UTF-8") == 0) {
        is_utf = true;
    }

    a_relink = getenv("NODEM_AUTO_RELINK");

    if (a_relink != NULL)
        auto_relink = atoi(a_relink);

    object = Local<Object>::Cast(args[0]);

    if (! object->IsUndefined()) {
        Local<Value> path = object->Get(STRING("path"));

        if (! path->IsUndefined()) {
            if (setenv("gtm_dist", *String::Utf8Value(path), 1) == -1)
                std::cerr << strerror(errno) << std::endl;
        }

        Local<Value> name_space = object->Get(STRING("namespace"));

        if (! name_space->IsUndefined()) {
            if (setenv("gtmgbldir", *String::Utf8Value(name_space), 1) == -1)
                std::cerr << strerror(errno) << std::endl;
        }

        Local<Value> addr = object->Get(STRING("ip_address"));
        Local<Value> port = object->Get(STRING("tcp_port"));

        if (! addr->IsUndefined() || ! port->IsUndefined()) {
            Local<Value> gtcm_nodem;

            if (addr->IsUndefined())
                addr = Local<Value>::New(ISOLATE_COMMA STRING("127.0.0.1"));

            if (port->IsUndefined())
                port = Local<Value>::New(ISOLATE_COMMA STRING("6789"));

            if (port->IsString()) {
                gtcm_nodem = String::Concat(Local<String>::Cast(addr), String::Concat(STRING(":"), Local<String>::Cast(port)));
            } else if (port->IsNumber()) {
                gtcm_nodem = String::Concat(Local<String>::Cast(addr), String::Concat(STRING(":"), port->ToString()));
            } else {
                EXCEPTION(Exception::Error(STRING("tcp_port must be a number or a string")));

                SCOPE_SET(args, Undefined(ISOLATE));
                SCOPE_RETURN(Undefined(ISOLATE));
            }

            if (setenv("GTCM_NODEM", *String::Utf8Value(gtcm_nodem), 1) == -1)
                std::cerr << strerror(errno) << std::endl;
        }

        auto_relink = object->Get(STRING("autoRelink"))->Uint32Value();

        String::Utf8Value data_mode(object->Get(STRING("mode")));

        if (strcmp(*data_mode, "strict") == 0)
            mode = STRICT;
    }

    if (tcgetattr(STDIN_FILENO, &term_attr) == -1)
        std::cerr << strerror(errno) << std::endl;

    stat_buf = gtm_init();

    if (stat_buf) {
        gtm_zstatus(msg_buf, BUF_LEN);

        Handle<Object> result = gtm_status(msg_buf);

        SCOPE_SET(args, result);
        SCOPE_RETURN(result);
    }

    gtm_is_open = 1;

    signal_attr.sa_handler = catch_intr;
    signal_attr.sa_flags = 0;

    if (sigemptyset(&signal_attr.sa_mask) == -1) {
        EXCEPTION(Exception::Error(STRING("Cannot empty signal handler")));

        SCOPE_SET(args, Undefined(ISOLATE));
        SCOPE_RETURN(Undefined(ISOLATE));
    }

    if (sigaction(SIGINT, &signal_attr, NULL) == -1) {
        EXCEPTION(Exception::Error(STRING("Cannot initialize signal handler")));

        SCOPE_SET(args, Undefined(ISOLATE));
        SCOPE_RETURN(Undefined(ISOLATE));
    }

    Local<Object> result = Object::New(ISOLATE);

    result->Set(STRING("ok"), NUMBER(1));
    result->Set(STRING("result"), NUMBER(1));
    result->Set(STRING("gtm_pid"), NUMBER(getpid()));

    SCOPE_SET(args, result);
    SCOPE_RETURN(result);
} //End of Gtm::open


RETURN_DECL Gtm::close(ARGUMENTS args)
{
    ISOLATE_CURRENT;
    SCOPE_HANDLE;

    if (gtm_is_open < 1) {
        EXCEPTION(Exception::Error(STRING("GT.M database not open")));

        SCOPE_SET(args, Undefined(ISOLATE));
        SCOPE_RETURN(Undefined(ISOLATE));
    }

    Local<Object> object = Local<Object>::Cast(args[0]);

    if (! object->IsUndefined())
        reset_term = object->Get(STRING("resetTerminal"))->Uint32Value();

    stat_buf = gtm_exit();

    if (stat_buf) {
        gtm_zstatus(msg_buf, BUF_LEN);

        Handle<Object> result = gtm_status(msg_buf);

        SCOPE_SET(args, result);
        SCOPE_RETURN(result);
    }

    gtm_is_open = -1;

    if (reset_term) {
        term_attr.c_iflag |= ICRNL;
        term_attr.c_lflag |= (ISIG | ECHO);
    }

    if (tcsetattr(STDIN_FILENO, TCSANOW, &term_attr) == -1)
        std::cerr << strerror(errno) << std::endl;

    SCOPE_SET(args, STRING("1"));
    SCOPE_RETURN(STRING("1"));
} //End of Gtm::close


RETURN_DECL Gtm::version(ARGUMENTS args)
{
    ISOLATE_CURRENT;
    SCOPE_HANDLE;

    if (gtm_is_open < 1) {
        SCOPE_SET(args, STRING("Node.js Adaptor for GT.M: Version: 0.9.0 (FWSLC)"));
        SCOPE_RETURN(STRING("Node.js Adaptor for GT.M: Version: 0.9.0 (FWSLC)"));
    }

    char str[] = "version";

#if (GTM_VERSION > 54)
    ci_name_descriptor version;

    version.rtn_name.address = str;
    version.rtn_name.length = strlen(version.rtn_name.address);
    version.handle = NULL;

    stat_buf = gtm_cip(&version, ret_buf, NULL);
#else
    stat_buf = gtm_ci(str, ret_buf, NULL);
#endif

    if (stat_buf) {
        gtm_zstatus(msg_buf, BUF_LEN);

        Handle<Object> result = gtm_status(msg_buf);

        SCOPE_SET(args, result);
        SCOPE_RETURN(result);
    }

    SCOPE_SET(args, STRING(ret_buf));
    SCOPE_RETURN(STRING(ret_buf));
} //End of Gtm::version & about


Handle<Value> parse_json(Handle<Value> json_string)
{
    ISOLATE_CURRENT;
    SCOPE_HANDLE;

    Handle<Context> context = CONTEXT_CURRENT;
    Handle<Object> global = context->Global();

    Handle<Object> JSON = global->Get(STRING("JSON"))->ToObject();
    Handle<Function> JSON_parse = Handle<Function>::Cast(JSON->Get(STRING("parse")));

    return SCOPE_ESCAPE(JSON_parse->Call(JSON, 1, &json_string));
} //End of parse_json


RETURN_DECL call_gtm(Local<Value> cmd, ARGUMENTS args)
{
    ISOLATE_CURRENT;
    SCOPE_HANDLE;

    Local<Value> a_relink;
    Local<Value> arrays, data, incr, name, number;
    Local<Value> hi, lo, max;

    Local<Object> from_object, to_object;

    Local<Value> from_arrays, from_name;
    Local<Value> to_arrays, to_name;

    if (gtm_is_open < 1) {
        EXCEPTION(Exception::Error(STRING("GT.M database not open")));

        SCOPE_SET(args, Undefined(ISOLATE));
        SCOPE_RETURN(Undefined(ISOLATE));
    }

    Local<String> test = String::Cast(*cmd)->ToString();

    Local<String> function = STRING("function");
    Local<String> global_directory = STRING("global_directory");
    Local<String> increment = STRING("increment");
    Local<String> lock = STRING("lock");
    Local<String> merge = STRING("merge");
    Local<String> next_node = STRING("next_node");
    Local<String> order = STRING("order");
    Local<String> previous = STRING("previous");
    Local<String> previous_node = STRING("previous_node");
    Local<String> procedure = STRING("procedure");
    Local<String> retrieve = STRING("retrieve");
    Local<String> set = STRING("set");
    Local<String> unlock = STRING("unlock");
    Local<String> update = STRING("update");

    if (! test->Equals(global_directory) && ! test->Equals(unlock) && ! test->Equals(previous_node) &&
        ! test->Equals(retrieve) && ! test->Equals(update) && args.Length() < 1) {

        EXCEPTION(Exception::SyntaxError(STRING("Argument must be specified")));

        SCOPE_SET(args, Undefined(ISOLATE));
        SCOPE_RETURN(Undefined(ISOLATE));
    }

    Local<Object> object = Local<Object>::Cast(args[0]);

    if (object->IsUndefined()) {
        object = object->New(ISOLATE);
    } else if (! object->IsObject()) {
        EXCEPTION(Exception::SyntaxError(STRING("Argument must be an object")));

        SCOPE_SET(args, Undefined(ISOLATE));
        SCOPE_RETURN(Undefined(ISOLATE));
    }

    if (test->Equals(function)) {
        name = object->Get(STRING("function"));
        arrays = object->Get(STRING("arguments"));

        if (object->Has(STRING("autoRelink"))) {
            a_relink = Local<Number>::Cast(object->Get(STRING("autoRelink")));
        } else {
            a_relink = Local<Number>::New(ISOLATE_COMMA NUMBER(auto_relink));
        }

        to_arrays = object->Get(Undefined(ISOLATE));
        from_arrays = object->Get(Undefined(ISOLATE));

        if (name->IsUndefined()) {
            EXCEPTION(Exception::SyntaxError(STRING("Need to supply a 'function' property")));

            SCOPE_SET(args, Undefined(ISOLATE));
            SCOPE_RETURN(Undefined(ISOLATE));
        }
    } else if (test->Equals(global_directory)) {
        max = object->Get(STRING("max"));
        lo = object->Get(STRING("lo"));
        hi = object->Get(STRING("hi"));

        arrays = object->Get(Undefined(ISOLATE));

        to_arrays = object->Get(Undefined(ISOLATE));
        from_arrays = object->Get(Undefined(ISOLATE));
    } else if (test->Equals(merge)) {
        to_object = Local<Object>::Cast(object->Get(STRING("to")));

        if (to_object->IsUndefined()) {
            EXCEPTION(Exception::SyntaxError(STRING("Need to supply a 'to' property")));

            SCOPE_SET(args, Undefined(ISOLATE));
            SCOPE_RETURN(Undefined(ISOLATE));
        }

        if (! to_object->IsObject()) {
            EXCEPTION(Exception::SyntaxError(STRING("'to' property must be an object")));

            SCOPE_SET(args, Undefined(ISOLATE));
            SCOPE_RETURN(Undefined(ISOLATE));
        }

        from_object = Local<Object>::Cast(object->Get(STRING("from")));

        if (from_object->IsUndefined()) {
            EXCEPTION(Exception::SyntaxError(STRING("Need to supply a 'from' property")));

            SCOPE_SET(args, Undefined(ISOLATE));
            SCOPE_RETURN(Undefined(ISOLATE));
        }

        if (! from_object->IsObject()) {
            EXCEPTION(Exception::SyntaxError(STRING("'from' property must be an object")));

            SCOPE_SET(args, Undefined(ISOLATE));
            SCOPE_RETURN(Undefined(ISOLATE));
        }

        to_name = to_object->Get(STRING("global"));

        if (to_name->IsUndefined()) {
            EXCEPTION(Exception::SyntaxError(STRING("Need to supply a 'global' property in your 'to' object")));

            SCOPE_SET(args, Undefined(ISOLATE));
            SCOPE_RETURN(Undefined(ISOLATE));
        }

        to_arrays = to_object->Get(STRING("subscripts"));

        from_name = from_object->Get(STRING("global"));

        if (from_name->IsUndefined()) {
            EXCEPTION(Exception::SyntaxError(STRING("Need to supply a 'global' property in your 'from' object")));

            SCOPE_SET(args, Undefined(ISOLATE));
            SCOPE_RETURN(Undefined(ISOLATE));
        }

        from_arrays = from_object->Get(STRING("subscripts"));

        arrays = object->Get(Undefined(ISOLATE));
    } else if (test->Equals(procedure)) {
        name = object->Get(STRING("procedure"));
        arrays = object->Get(STRING("arguments"));

        if (object->Has(STRING("autoRelink"))) {
            a_relink = Local<Number>::Cast(object->Get(STRING("autoRelink")));
        } else {
            a_relink = Local<Number>::New(ISOLATE_COMMA NUMBER(auto_relink));
        }

        to_arrays = object->Get(Undefined(ISOLATE));
        from_arrays = object->Get(Undefined(ISOLATE));

        if (name->IsUndefined()) {
            EXCEPTION(Exception::SyntaxError(STRING("Need to supply a 'procedure' property")));

            SCOPE_SET(args, Undefined(ISOLATE));
            SCOPE_RETURN(Undefined(ISOLATE));
        }
    } else if (test->Equals(unlock)) {
        name = object->Get(STRING("global"));
        arrays = object->Get(STRING("subscripts"));

        to_arrays = object->Get(Undefined(ISOLATE));
        from_arrays = object->Get(Undefined(ISOLATE));

        if (name->IsUndefined())
            name = String::Empty(ISOLATE);

    } else if (test->Equals(previous_node) || test->Equals(retrieve) || test->Equals(update)) {
        name = object->Get(Undefined(ISOLATE));
        arrays = object->Get(Undefined(ISOLATE));

        to_arrays = object->Get(Undefined(ISOLATE));
        from_arrays = object->Get(Undefined(ISOLATE));
    } else {
        name = object->Get(STRING("global"));
        arrays = object->Get(STRING("subscripts"));

        to_arrays = object->Get(Undefined(ISOLATE));
        from_arrays = object->Get(Undefined(ISOLATE));

        if (name->IsUndefined()) {
            EXCEPTION(Exception::SyntaxError(STRING("Need to supply a 'global' property")));

            SCOPE_SET(args, Undefined(ISOLATE));
            SCOPE_RETURN(Undefined(ISOLATE));
        }
    }

    if (test->Equals(increment)) {
        incr = Local<Number>::Cast(object->Get(STRING("increment")));
    } else if (test->Equals(lock)) {
        number = Local<Number>::Cast(args[1]);
    } else if (test->Equals(set)) {
        data = object->Get(STRING("data"));

        if (data->IsUndefined()) {
            EXCEPTION(Exception::SyntaxError(STRING("Need to supply a 'data' property")));

            SCOPE_SET(args, Undefined(ISOLATE));
            SCOPE_RETURN(Undefined(ISOLATE));
        }

        if (object->Get(STRING("data"))->IsString())
            data = String::Concat(STRING("\""), String::Concat(Local<String>::Cast(data), STRING("\"")));
    }

    Local<Value> arg = String::Empty(ISOLATE);
    Local<Value> from_arg = String::Empty(ISOLATE);
    Local<Value> to_arg = String::Empty(ISOLATE);

    if (! arrays->IsUndefined()) {
        Local<Array> array = Local<Array>::Cast(arrays);
        Local<Array> temparg = Array::New(ISOLATE);

        char buf[WIDTH];

        for (uint32_t i = 0; i < array->Length(); i++) {
            Local<String> str = Local<String>::Cast(array->Get(i)->ToString());

            int length = str->Length();

            if (array->Get(i)->IsString()) {
                sprintf(buf, "%d:", length + 2);

                Local<String> out_string = String::Concat(STRING("\""), String::Concat(str, STRING("\"")));

                Local<Value> out = String::Concat(STRING(buf), out_string);
                temparg->Set(i, out);
            } else {
                sprintf(buf, "%d:", length);

                Local<Value> out = String::Concat(STRING(buf), str);
                temparg->Set(i, out);
            }
        }

        arg = Local<Array>::Cast(temparg);
    } else  if (test->Equals(merge)) {
        if (! from_arrays->IsUndefined()) {
            Local<Array> from_array = Local<Array>::Cast(from_arrays);
            Local<Array> from_temparg = Array::New(ISOLATE);

            char from_buf[WIDTH];

            for (uint32_t j = 0; j < from_array->Length(); j++) {
                Local<String> from_str = Local<String>::Cast(from_array->Get(j)->ToString());

                int from_length = from_str->Length();

                if (from_array->Get(j)->IsString()) {
                    sprintf(from_buf, "%d:", from_length + 2);

                    Local<String> from_string = String::Concat(STRING("\""), String::Concat(from_str, STRING("\"")));
                    Local<Value> from_out = String::Concat(STRING(from_buf), from_string);
                    from_temparg->Set(j, from_out);
                } else {
                    sprintf(from_buf, "%d:", from_length);

                    Local<Value> from_out = String::Concat(STRING(from_buf), from_str);
                    from_temparg->Set(j, from_out);
                }
            }

            from_arg = Local<Array>::Cast(from_temparg);
        } else {
            from_arg = String::Empty(ISOLATE);
        }

        if (! to_arrays->IsUndefined()) {
            Local<Array> to_array = Local<Array>::Cast(to_arrays);
            Local<Array> to_temparg = Array::New(ISOLATE);

            char to_buf[WIDTH];

            for (uint32_t i = 0; i < to_array->Length(); i++) {
                Local<String> to_str = Local<String>::Cast(to_array->Get(i)->ToString());

                int to_length = to_str->Length();

                if (to_array->Get(i)->IsString()) {
                    sprintf(to_buf, "%d:", to_length + 2);

                    Local<String> to_string = String::Concat(STRING("\""), String::Concat(to_str, STRING("\"")));
                    Local<Value> to_out = String::Concat(STRING(to_buf), to_string);
                    to_temparg->Set(i, to_out);
                } else {
                    sprintf(to_buf, "%d:", to_length);

                    Local<Value> to_out = String::Concat(STRING(to_buf), to_str);
                    to_temparg->Set(i, to_out);
                }
            }

            to_arg = Local<Array>::Cast(to_temparg);
        } else {
            to_arg = String::Empty(ISOLATE);
        }
    } else {
        arg = String::Empty(ISOLATE);
    }

    ret_buf[0] = '\0';

    Local<String> string = cmd->ToString();

    ASCII_STRING(str, string);

#if (GTM_VERSION > 54)
    ci_name_descriptor access;

    access.rtn_name.address = CAST(str, gtm_char_t*);
    access.rtn_name.length = strlen(access.rtn_name.address);
    access.handle = NULL;
#endif

    if (test->Equals(function) || test->Equals(procedure)) {
        if (is_utf) {
#if (GTM_VERSION > 54)
            stat_buf = gtm_cip(&access, ret_buf, *String::Utf8Value(name), *String::Utf8Value(arg), a_relink->Uint32Value(), mode);
#else
            stat_buf = gtm_ci(*str, ret_buf, *String::Utf8Value(name), *String::Utf8Value(arg), a_relink->Uint32Value(), mode);
#endif
        } else {
            ASCII_PROTO(name);
            ASCII_PROTO(arg);

#if (GTM_VERSION > 54)
            stat_buf = gtm_cip(&access, ret_buf, ASCII_VALUE(name), ASCII_VALUE(arg), a_relink->Uint32Value(), mode);
#else
            stat_buf = gtm_ci(*str, ret_buf, ASCII_VALUE(name), ASCII_VALUE(arg), a_relink->Uint32Value(), mode);
#endif
        }
    } else if (test->Equals(global_directory)) {
        if (max->IsUndefined())
            max = NUMBER(0);

        if (lo->IsUndefined())
            lo = String::Empty(ISOLATE);

        if (hi->IsUndefined())
            hi = String::Empty(ISOLATE);

        if (is_utf) {
#if (GTM_VERSION > 54)
            stat_buf = gtm_cip(&access, ret_buf, max->Uint32Value(), *String::Utf8Value(lo), *String::Utf8Value(hi));
#else
            stat_buf = gtm_ci(*str, ret_buf, max->Uint32Value(), *String::Utf8Value(lo), *String::Utf8Value(hi));
#endif
        } else {
            ASCII_PROTO(lo);
            ASCII_PROTO(hi);

#if (GTM_VERSION > 54)
            stat_buf = gtm_cip(&access, ret_buf, max->Uint32Value(), ASCII_VALUE(lo), ASCII_VALUE(hi));
#else
            stat_buf = gtm_ci(*str, ret_buf, max->Uint32Value(), ASCII_VALUE(lo), ASCII_VALUE(hi));
#endif
        }
    } else if (test->Equals(increment)) {
        if (incr->IsUndefined())
            incr = NUMBER(1);

        if (is_utf) {
#if (GTM_VERSION > 54)
            stat_buf = gtm_cip(&access, ret_buf, *String::Utf8Value(name), *String::Utf8Value(arg), incr->NumberValue(), mode);
#else
            stat_buf = gtm_ci(*str, ret_buf, *String::Utf8Value(name), *String::Utf8Value(arg), incr->NumberValue(), mode);
#endif
        } else {
            ASCII_PROTO(name);
            ASCII_PROTO(arg);

#if (GTM_VERSION > 54)
            stat_buf = gtm_cip(&access, ret_buf, ASCII_VALUE(name), ASCII_VALUE(arg), incr->NumberValue(), mode);
#else
            stat_buf = gtm_ci(*str, ret_buf, ASCII_VALUE(name), ASCII_VALUE(arg), incr->NumberValue(), mode);
#endif
        }
    } else if (test->Equals(lock)) {
        if (! number->IsNumber()) {
            number = NUMBER(-1);
        } else if (number->NumberValue() < 0) {
            number = NUMBER(0);
        }

        if (is_utf) {
#if (GTM_VERSION > 54)
            stat_buf = gtm_cip(&access, ret_buf, *String::Utf8Value(name), *String::Utf8Value(arg), number->NumberValue(), mode);
#else
            stat_buf = gtm_ci(*str, ret_buf, *String::Utf8Value(name), *String::Utf8Value(arg), number->NumberValue(), mode);
#endif
        } else {
            ASCII_PROTO(name);
            ASCII_PROTO(arg);

#if (GTM_VERSION > 54)
            stat_buf = gtm_cip(&access, ret_buf, ASCII_VALUE(name), ASCII_VALUE(arg), number->NumberValue(), mode);
#else
            stat_buf = gtm_ci(*str, ret_buf, ASCII_VALUE(name), ASCII_VALUE(arg), number->NumberValue(), mode);
#endif
        }
    } else if (test->Equals(merge)) {
        if (is_utf) {
#if (GTM_VERSION > 54)
            stat_buf = gtm_cip(&access, ret_buf, *String::Utf8Value(from_name), *String::Utf8Value(from_arg),
                               *String::Utf8Value(to_name), *String::Utf8Value(to_arg), mode);
#else
            stat_buf = gtm_ci(*str, ret_buf, *String::Utf8Value(from_name), *String::Utf8Value(from_arg),
                              *String::Utf8Value(to_name), *String::Utf8Value(to_arg), mode);
#endif
        } else {
            ASCII_PROTO(from_name);
            ASCII_PROTO(from_arg);
            ASCII_PROTO(to_name);
            ASCII_PROTO(to_arg);

#if (GTM_VERSION > 54)
            stat_buf = gtm_cip(&access, ret_buf, ASCII_VALUE(from_name), ASCII_VALUE(from_arg),
                               ASCII_VALUE(to_name), ASCII_VALUE(to_arg), mode);
#else
            stat_buf = gtm_ci(*str, ret_buf, ASCII_VALUE(from_name), ASCII_VALUE(from_arg),
                              ASCII_VALUE(to_name), ASCII_VALUE(to_arg), mode);
#endif
        }
    } else if (test->Equals(set)) {
        if (is_utf) {
#if (GTM_VERSION > 54)
            stat_buf = gtm_cip(&access, ret_buf, *String::Utf8Value(name), *String::Utf8Value(arg), *String::Utf8Value(data), mode);
#else
            stat_buf = gtm_ci(*str, ret_buf, *String::Utf8Value(name), *String::Utf8Value(arg), *String::Utf8Value(data), mode);
#endif
        } else {
            ASCII_PROTO(name);
            ASCII_PROTO(arg);
            ASCII_PROTO(data);

#if (GTM_VERSION > 54)
            stat_buf = gtm_cip(&access, ret_buf, ASCII_VALUE(name), ASCII_VALUE(arg), ASCII_VALUE(data), mode);
#else
            stat_buf = gtm_ci(*str, ret_buf, ASCII_VALUE(name), ASCII_VALUE(arg), ASCII_VALUE(data), mode);
#endif
        }
    } else if (test->Equals(previous_node) || test->Equals(retrieve) || test->Equals(update)) {
#if (GTM_VERSION > 54)
            stat_buf = gtm_cip(&access, ret_buf);
#else
            stat_buf = gtm_ci(*str, ret_buf);
#endif
    } else {
        if (is_utf) {
#if (GTM_VERSION > 54)
            stat_buf = gtm_cip(&access, ret_buf, *String::Utf8Value(name), *String::Utf8Value(arg), mode);
#else
            stat_buf = gtm_ci(*str, ret_buf, *String::Utf8Value(name), *String::Utf8Value(arg), mode);
#endif
        } else {
            ASCII_PROTO(name);
            ASCII_PROTO(arg);

#if (GTM_VERSION > 54)
            stat_buf = gtm_cip(&access, ret_buf, ASCII_VALUE(name), ASCII_VALUE(arg), mode);
#else
            stat_buf = gtm_ci(*str, ret_buf, ASCII_VALUE(name), ASCII_VALUE(arg), mode);
#endif
        }
    }

    if (stat_buf) {
        gtm_zstatus(msg_buf, BUF_LEN);

        Handle<Object> result = gtm_status(msg_buf);

        SCOPE_SET(args, result);
        SCOPE_RETURN(result);
    }

    Local<String> n_string;

    if (is_utf) {
        n_string = STRING(ret_buf);
    } else {
        ASCII_NAME(n_string, ret_buf);
    }

    if (n_string->Length() < 1) {
        EXCEPTION(Exception::RangeError(STRING("No JSON string present")));

        SCOPE_SET(args, Undefined(ISOLATE));
        SCOPE_RETURN(Undefined(ISOLATE));
    }

    Handle<Value> retvalue = parse_json(n_string);

    if (retvalue.IsEmpty()) {
        SCOPE_SET(args, Undefined(ISOLATE));
        SCOPE_RETURN(Undefined(ISOLATE));
    }

    Handle<Object> retobject = Handle<Object>::Cast(retvalue);

    if (arrays->IsUndefined()) {
        SCOPE_SET(args, retobject);
        SCOPE_RETURN(retobject);
    } else if (test->Equals(function) || test->Equals(procedure)) {
        Local<Array> array = Local<Array>::Cast(arrays);

        if (retobject->Get(STRING("errorCode"))->IsUndefined())
            retobject->Set(STRING("arguments"), array);

        SCOPE_SET(args, retobject);
        SCOPE_RETURN(retobject);
    } else if (test->Equals(order) || test->Equals(previous)) {
        Local<Array> array = Local<Array>::Cast(arrays);

        if (retobject->Get(STRING("errorCode"))->IsUndefined()) {
            array->Set(NUMBER(array->Length() - 1), retobject->Get(STRING("result")));

            retobject->Set(STRING("subscripts"), array);
        }

        SCOPE_SET(args, retobject);
        SCOPE_RETURN(retobject);
    } else if (test->Equals(next_node)) {
        SCOPE_SET(args, retobject);
        SCOPE_RETURN(retobject);
    } else {
        Local<Array> array = Local<Array>::Cast(arrays);

        if (retobject->Get(STRING("errorCode"))->IsUndefined())
            retobject->Set(STRING("subscripts"), array);

        SCOPE_SET(args, retobject);
        SCOPE_RETURN(retobject);
    }
} //End of call_gtm


RETURN_DECL Gtm::data(ARGUMENTS args)
{
    ISOLATE_CURRENT;

    Local<Value> cmd = STRING("data");

    RETURN call_gtm(cmd, args);
} //End of Gtm::data


RETURN_DECL Gtm::function(ARGUMENTS args)
{
    ISOLATE_CURRENT;

    Local<Value> cmd = STRING("function");

    RETURN call_gtm(cmd, args);
} //End of Gtm::function


RETURN_DECL Gtm::get(ARGUMENTS args)
{
    ISOLATE_CURRENT;

    Local<Value> cmd = STRING("get");

    RETURN call_gtm(cmd, args);
} //End of Gtm::get


RETURN_DECL Gtm::global_directory(ARGUMENTS args)
{
    ISOLATE_CURRENT;

    Local<Value> cmd = STRING("global_directory");

    RETURN call_gtm(cmd, args);
} //End of Gtm::global_directory


RETURN_DECL Gtm::increment(ARGUMENTS args)
{
    ISOLATE_CURRENT;

    Local<Value> cmd = STRING("increment");

    RETURN call_gtm(cmd, args);
} //End of Gtm::increment


RETURN_DECL Gtm::kill(ARGUMENTS args)
{
    ISOLATE_CURRENT;

    Local<Value> cmd = STRING("kill");

    RETURN call_gtm(cmd, args);
} //End of Gtm::kill


RETURN_DECL Gtm::lock(ARGUMENTS args)
{
    ISOLATE_CURRENT;

    Local<Value> cmd = STRING("lock");

    RETURN call_gtm(cmd, args);
} //End of Gtm::lock


RETURN_DECL Gtm::merge(ARGUMENTS args)
{
    ISOLATE_CURRENT;

    Local<Value> cmd = STRING("merge");

    RETURN call_gtm(cmd, args);
} //End of Gtm::merge


RETURN_DECL Gtm::next_node(ARGUMENTS args)
{
    ISOLATE_CURRENT;

    Local<Value> cmd = STRING("next_node");

    RETURN call_gtm(cmd, args);
} //End of Gtm::next_node


RETURN_DECL Gtm::order(ARGUMENTS args)
{
    ISOLATE_CURRENT;

    Local<Value> cmd = STRING("order");

    RETURN call_gtm(cmd, args);
} //End of Gtm::order


RETURN_DECL Gtm::previous(ARGUMENTS args)
{
    ISOLATE_CURRENT;

    Local<Value> cmd = STRING("previous");

    RETURN call_gtm(cmd, args);
} //End of Gtm::previous


RETURN_DECL Gtm::previous_node(ARGUMENTS args)
{
    ISOLATE_CURRENT;

    Local<Value> cmd = STRING("previous_node");

    RETURN call_gtm(cmd, args);
} //End of Gtm::previous_node


RETURN_DECL Gtm::procedure(ARGUMENTS args)
{
    ISOLATE_CURRENT;

    Local<Value> cmd = STRING("procedure");

    RETURN call_gtm(cmd, args);
} //End of Gtm::procedure


RETURN_DECL Gtm::retrieve(ARGUMENTS args)
{
    ISOLATE_CURRENT;

    Local<Value> cmd = STRING("retrieve");

    RETURN call_gtm(cmd, args);
} //End of Gtm::retrieve


RETURN_DECL Gtm::set(ARGUMENTS args)
{
    ISOLATE_CURRENT;

    Local<Value> cmd = STRING("set");

    RETURN call_gtm(cmd, args);
} //End of Gtm::set


RETURN_DECL Gtm::unlock(ARGUMENTS args)
{
    ISOLATE_CURRENT;

    Local<Value> cmd = STRING("unlock");

    RETURN call_gtm(cmd, args);
} //End of Gtm::unlock


RETURN_DECL Gtm::update(ARGUMENTS args)
{
    ISOLATE_CURRENT;

    Local<Value> cmd = STRING("update");

    RETURN call_gtm(cmd, args);
} //End of Gtm::update


Gtm::Gtm() {}
Gtm::~Gtm() {}


void Gtm::Init(Handle<Object> exports)
{
    ISOLATE_CURRENT;

    //Prepare constructor template
    Local<FunctionTemplate> tpl = FunctionTemplate::New(ISOLATE_COMMA New);

    tpl->SetClassName(SYMBOL("Gtm"));
    tpl->InstanceTemplate()->SetInternalFieldCount(1);

    NODE_SET_PROTOTYPE_METHOD(tpl, "about", version);
    NODE_SET_PROTOTYPE_METHOD(tpl, "close", close);
    NODE_SET_PROTOTYPE_METHOD(tpl, "data", data);
    NODE_SET_PROTOTYPE_METHOD(tpl, "function", function);
    NODE_SET_PROTOTYPE_METHOD(tpl, "procedure", procedure);
    NODE_SET_PROTOTYPE_METHOD(tpl, "get", get);
    NODE_SET_PROTOTYPE_METHOD(tpl, "global_directory", global_directory);
    NODE_SET_PROTOTYPE_METHOD(tpl, "increment", increment);
    NODE_SET_PROTOTYPE_METHOD(tpl, "kill", kill);
    NODE_SET_PROTOTYPE_METHOD(tpl, "lock", lock);
    NODE_SET_PROTOTYPE_METHOD(tpl, "merge", merge);
    NODE_SET_PROTOTYPE_METHOD(tpl, "next", order);
    NODE_SET_PROTOTYPE_METHOD(tpl, "next_node", next_node);
    NODE_SET_PROTOTYPE_METHOD(tpl, "open", open);
    NODE_SET_PROTOTYPE_METHOD(tpl, "order", order);
    NODE_SET_PROTOTYPE_METHOD(tpl, "previous", previous);
    NODE_SET_PROTOTYPE_METHOD(tpl, "previous_node", previous_node);
    NODE_SET_PROTOTYPE_METHOD(tpl, "retrieve", retrieve);
    NODE_SET_PROTOTYPE_METHOD(tpl, "set", set);
    NODE_SET_PROTOTYPE_METHOD(tpl, "unlock", unlock);
    NODE_SET_PROTOTYPE_METHOD(tpl, "update", update);
    NODE_SET_PROTOTYPE_METHOD(tpl, "version", version);

    PERSISTENT_FUNCTION(constructor, tpl->GetFunction());

    exports->Set(SYMBOL("Gtm"), CONSTRUCTOR(constructor, tpl));
} //End of Gtm::Init


RETURN_DECL Gtm::New(ARGUMENTS args)
{
    ISOLATE_CURRENT;
    SCOPE_HANDLE;

    if (args.IsConstructCall()) {
        Gtm* obj = new Gtm();
        obj->Wrap(args.This());

        SCOPE_SET(args, args.This());
        SCOPE_RETURN(args.This());
    } else {
        Local<Function> cons = Local<Function>::New(ISOLATE_COMMA constructor);

        SCOPE_SET(args, cons->NewInstance());
        SCOPE_RETURN(cons->NewInstance());
    }
} //End of Gtm::New


NODE_MODULE(mumps, Gtm::Init)
