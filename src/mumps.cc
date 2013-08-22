/*
 * mumps.cc - A set of APIs to work on a GT.M database from Node.js
 *
 * Written by David Wicksell <dlw@linux.com>
 *
 * Copyright © 2012,2013 Fourth Watch Software, LC
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


#include <nodejs/node.h>

using namespace v8;
using namespace node;

extern "C" {
    #include <errno.h>
    #include <signal.h>
    #include <stdlib.h>
    #include <string.h>
    #include <termios.h>
    #include <unistd.h>
    #include <sys/types.h>
    #include <gtmxc_types.h>
}

#define LENGTH 9999
#define BUF_LEN (2048 + 1)
#define RET_LEN ((1024 * 1024) + 1)
#define WIDTH 6

gtm_char_t msgbuf[BUF_LEN];
gtm_char_t ret[RET_LEN];
gtm_status_t status;

bool is_utf = false;

struct termios tp;
struct sigaction n_signal;

static int gtm_is_open = 0;

Handle<String> db_error;


class Gtm: public ObjectWrap
{
    public:
        static void Init(Handle<Object> target);

    private:
        Gtm();
        ~Gtm();

        static Handle<Value> New(const Arguments&);
        static Handle<Value> close(const Arguments&);
        static Handle<Value> data(const Arguments&);
        static Handle<Value> function(const Arguments&);
        static Handle<Value> get(const Arguments&);
        static Handle<Value> global_directory(const Arguments&);
        static Handle<Value> increment(const Arguments&);
        static Handle<Value> kill(const Arguments&);
        static Handle<Value> lock(const Arguments&);
        static Handle<Value> merge(const Arguments&);
        static Handle<Value> next_node(const Arguments&);
        static Handle<Value> open(const Arguments&);
        static Handle<Value> order(const Arguments&);
        static Handle<Value> previous(const Arguments&);
        static Handle<Value> previous_node(const Arguments&);
        static Handle<Value> retrieve(const Arguments&);
        static Handle<Value> set(const Arguments&);
        static Handle<Value> unlock(const Arguments&);
        static Handle<Value> update(const Arguments&);
        static Handle<Value> version(const Arguments&);
}; //End of class Gtm


static void catch_intr(int signum)
{
    gtm_exit();

    if (tcsetattr(STDIN_FILENO, TCSANOW, &tp) == -1)
        strerror(errno);

    exit(EXIT_FAILURE);
}


int db_is_open(void)
{
    int err = 0;

    if (gtm_is_open > 0) {
        db_error = String::New("GT.M is already open");
        err = 0;
    } else if (gtm_is_open == 0) {
        db_error = String::New("GT.M is not open");
        err = -1;
    } else if (gtm_is_open < 0) {
        db_error = String::New("GT.M cannot be re-opened");
        err = -2;
    }

    return err;
} //End of db_is_open


Handle<Object> gtm_status(gtm_char_t *msg_buf)
{
    char *error_msg;
    char *err_code = strtok_r(msg_buf, ",", &error_msg);

    uint32_t error_code = atoi(err_code);

    Local<Object> result = Object::New();

    result->Set(String::New("ok"), Number::New(0));
    result->Set(String::New("errorCode"), Number::New(error_code));
    result->Set(String::New("errorMessage"), String::New(error_msg));

    return result;
}


Handle<Value> Gtm::open(const Arguments &args)
{
    HandleScope scope;

    int db_status = db_is_open();

    if (db_status != -1)
        return scope.Close(db_error);

    if (tcgetattr(STDIN_FILENO, &tp) == -1)
        strerror(errno);

    Local<Object> object = Local<Object>::Cast(args[0]);

    char *chset = getenv("gtm_chset");

    if (chset == NULL) {
        is_utf = false;
    } else if (strcmp(chset, "utf-8") == 0) {
        is_utf = true;
    } else if (strcmp(chset, "Utf-8") == 0) {
        is_utf = true;
    } else if (strcmp(chset, "UTF-8") == 0) {
        is_utf = true;
    }

    if (! object->IsUndefined()) {
        Local<Value> nspace = object->Get(String::New("namespace"));

        if (! nspace->IsUndefined()) {
            if (setenv("gtmgbldir", *String::Utf8Value(nspace), 1) == -1)
                strerror(errno);
        }
    }

    status = gtm_init();

    if (status) {
        gtm_zstatus(msgbuf, BUF_LEN); 

        Handle<Object> result = gtm_status(msgbuf);

        return scope.Close(result);
    }

    gtm_is_open = 1;

    n_signal.sa_handler = catch_intr;
    n_signal.sa_flags = 0;

    if (sigemptyset(&n_signal.sa_mask) == -1) {
        ThrowException(Exception::TypeError(String::
                       New("Cannot empty signal handler")));

        return scope.Close(Undefined());
    }

    if (sigaction(SIGINT, &n_signal, NULL) == -1) {
        ThrowException(Exception::TypeError(String::
                       New("Cannot initialize signal handler")));

        return scope.Close(Undefined());
    }

    Local<Object> result = Object::New();

    result->Set(String::New("ok"), Number::New(1));
    result->Set(String::New("result"), String::New("1"));

    return scope.Close(result);
} //End of Gtm::open


Handle<Value> Gtm::close(const Arguments &args)
{
    HandleScope scope;

    int db_status = db_is_open();

    if (db_status)
        return scope.Close(db_error);

    status = gtm_exit();

    if (status) { 
        gtm_zstatus(msgbuf, BUF_LEN); 

        Handle<Object> result = gtm_status(msgbuf);

        return scope.Close(result);
    }

    gtm_is_open = -1;

    if (tcsetattr(STDIN_FILENO, TCSANOW, &tp) == -1)
        strerror(errno);

    return scope.Close(String::New("1"));
} //End of Gtm::close


Handle<Value> Gtm::version(const Arguments &args)
{
    HandleScope scope;

    if (gtm_is_open < 1) {
        return scope.Close(
            String::New("Node.js Adaptor for GT.M: Version: 0.3.0 (FWSLC)"));
    }

    ci_name_descriptor version;

    char str[] = "version";

    version.rtn_name.address = str;
    version.rtn_name.length = strlen(version.rtn_name.address);
    version.handle = NULL;

    status = gtm_cip(&version, ret, NULL);

    if (status) { 
        gtm_zstatus(msgbuf, BUF_LEN); 

        Handle<Object> result = gtm_status(msgbuf);

        return scope.Close(result);
    }

    return scope.Close(String::New(ret));
} //End of Gtm::version & about


Handle<Value> parse_json(Handle<Value> json_string)
{
    HandleScope scope;

    Handle<Context> context = Context::GetCurrent();
    Handle<Object> global = context->Global();

    Handle<Object> JSON = global->Get(String::New("JSON"))->ToObject();
    Handle<Function> JSON_parse = Handle<Function>::
                                  Cast(JSON->Get(String::New("parse")));

    return scope.Close(JSON_parse->Call(JSON, 1, &json_string));
} //End of parse_json


Handle<Value> call_gtm(Local<Value> cmd, const Arguments &args)
{
    HandleScope scope;

    Local<Value> name;
    Local<Value> arrays;
    Local<Value> max;
    Local<Value> lo;
    Local<Value> hi;
    Local<Value> data;
    Local<Value> number;

    Local<Object> to_object;
    Local<Object> from_object;

    Local<Value> to_name;
    Local<Value> to_arrays;
    Local<Value> from_name;
    Local<Value> from_arrays;

    int db_status = db_is_open();

    if (db_status)
        return scope.Close(db_error);

    Local<String> test = String::Cast(*cmd)->ToString();

    Local<String> function = String::New("function");
    Local<String> global_directory = String::New("global_directory");
    Local<String> increment = String::New("increment");
    Local<String> order = String::New("order");
    Local<String> previous = String::New("previous");
    Local<String> set = String::New("set");
    Local<String> unlock = String::New("unlock");
    Local<String> merge = String::New("merge");
       
    if (! test->Equals(global_directory) && ! test->Equals(unlock)
                                             && args.Length() < 1) {
        ThrowException(Exception::TypeError(String::
                       New("Argument must be specified")));

        return scope.Close(Undefined());
    }

    Local<Object> object = Local<Object>::Cast(args[0]);

    if (object->IsUndefined())
        object = object->New();

    if (test->Equals(function)) {
        name = object->Get(String::New("function"));
        arrays = object->Get(String::New("arguments"));

        to_arrays = object->Get(Undefined());
        from_arrays = object->Get(Undefined());

        if (name->IsUndefined()) {
            ThrowException(Exception::TypeError(String::New
                                 ("Need to supply a function property")));

            return scope.Close(Undefined());
        }
    } else if (test->Equals(global_directory)) {
        max = object->Get(String::New("max"));
        lo = object->Get(String::New("lo"));
        hi = object->Get(String::New("hi"));

        arrays = object->Get(Undefined());

        to_arrays = object->Get(Undefined());
        from_arrays = object->Get(Undefined());
    } else if (test->Equals(unlock)) {
        name = object->Get(String::New("global"));
        arrays = object->Get(String::New("subscripts"));

        to_arrays = object->Get(Undefined());
        from_arrays = object->Get(Undefined());
    } else if (test->Equals(merge)) {
        to_object = Local<Object>::Cast(object->Get(String::New("to")));
        from_object = Local<Object>::Cast(object->Get(String::New("from")));

        to_name = to_object->Get(String::New("global"));
        to_arrays = to_object->Get(String::New("subscripts"));

        from_name = from_object->Get(String::New("global"));
        from_arrays = from_object->Get(String::New("subscripts"));

        arrays = object->Get(Undefined());
    } else {
        name = object->Get(String::New("global"));
        arrays = object->Get(String::New("subscripts"));

        to_arrays = object->Get(Undefined());
        from_arrays = object->Get(Undefined());

        if (name->IsUndefined()) {
            ThrowException(Exception::TypeError(String::New
                                 ("Need to supply a global property")));

            return scope.Close(Undefined());
        }
    }

    if (test->Equals(set)) {
        data = object->Get(String::New("data"));

        if (data->IsUndefined()) {
            ThrowException(Exception::TypeError(String::New
                                 ("Need to supply a data property")));

            return scope.Close(Undefined());
        }
    } else if (test->Equals(increment)) {
        number = Local<Number>::Cast(args[1]);
    }

    Local<Value> arg = String::Empty();
    Local<Value> to_arg = String::Empty();
    Local<Value> from_arg = String::Empty();

    if (! arrays->IsUndefined()) {
        Local<Array> array = Local<Array>::Cast(arrays);
        Local<Array> temparg = Array::New();

        char buf[WIDTH];

        for (uint32_t i = 0; i < array->Length(); i++) {
            Local<String> str = Local<String>::Cast(array->Get(i)->ToString());

            int length = str->Length();
            if (length > LENGTH)
                return scope.Close(String::New("Subscript too big!"));

            sprintf(buf, "%d:", length);

            Local<Value> out = String::Concat(String::New(buf), str);
            temparg->Set(i, out);
        }

        arg = Local<Array>::Cast(temparg);
    } else  if (test->Equals(merge)) {
        if (! to_arrays->IsUndefined()) {
            Local<Array> to_array = Local<Array>::Cast(to_arrays);
            Local<Array> to_temparg = Array::New();

            char to_buf[WIDTH];

            for (uint32_t i = 0; i < to_array->Length(); i++) {
                Local<String> to_str = Local<String>::Cast
                                    (to_array->Get(i)->ToString());

                int to_length = to_str->Length();
                if (to_length > LENGTH)
                    return scope.Close(String::New("Subscript too big!"));

                sprintf(to_buf, "%d:", to_length);

                Local<Value> to_out = String::Concat
                                      (String::New(to_buf), to_str);
                to_temparg->Set(i, to_out);
            }

            to_arg = Local<Array>::Cast(to_temparg);
        } else {
            from_arg = String::Empty();
        }

        if (! from_arrays->IsUndefined()) {
            Local<Array> from_array = Local<Array>::Cast(from_arrays);
            Local<Array> from_temparg = Array::New();

            char from_buf[WIDTH];

            for (uint32_t j = 0; j < from_array->Length(); j++) {
                Local<String> from_str = Local<String>::Cast
                                    (from_array->Get(j)->ToString());

                int from_length = from_str->Length();
                if (from_length > LENGTH)
                    return scope.Close(String::New("Subscript too big!"));

                sprintf(from_buf, "%d:", from_length);

                Local<Value> from_out = String::Concat
                                        (String::New(from_buf), from_str);
                from_temparg->Set(j, from_out);
            }

            from_arg = Local<Array>::Cast(from_temparg);
        } else {
            from_arg = String::Empty();
        }
    } else {
        arg = String::Empty();
    }

    ret[0] = '\0';

    ci_name_descriptor access;

    Local<String> string = cmd->ToString();

    String::AsciiValue str(string);

    if (is_utf)
        String::Utf8Value str(string);

    access.rtn_name.address = *str;
    access.rtn_name.length = strlen(access.rtn_name.address);
    access.handle = NULL;

    if (test->Equals(set)) {
        if (is_utf) {
            status = gtm_cip(&access, ret,
                            *String::Utf8Value(name),
                            *String::Utf8Value(arg),
                            *String::Utf8Value(data)); 
        } else {
            status = gtm_cip(&access, ret,
                            *String::AsciiValue(name),
                            *String::AsciiValue(arg),
                            *String::AsciiValue(data)); 
        }
    } else if (test->Equals(increment)) {
        if (number->IsUndefined())
            number = Number::New(1);

        status = gtm_cip(&access, ret,
                        *String::AsciiValue(name),
                        *String::AsciiValue(arg),
                        number->NumberValue());
    } else if (test->Equals(unlock)) {
        if (name->IsUndefined())
            name = String::Empty();

        if (arg->IsUndefined())
            arg = String::Empty();

        status = gtm_cip(&access, ret,
                        *String::AsciiValue(name),
                        *String::AsciiValue(arg));
    } else if (test->Equals(global_directory)) {
        if (max->IsUndefined())
            max = Number::New(0);

        if (lo->IsUndefined())
            lo = String::Empty();

        if (hi->IsUndefined())
            hi = String::Empty();

        status = gtm_cip(&access, ret,
                        max->Uint32Value(),
                        *String::AsciiValue(lo),
                        *String::AsciiValue(hi));
    } else if (test->Equals(merge)) {
        status = gtm_cip(&access, ret,
                        *String::AsciiValue(to_name),
                        *String::AsciiValue(to_arg),
                        *String::AsciiValue(from_name),
                        *String::AsciiValue(from_arg));
    } else {
        if (is_utf) {
            status = gtm_cip(&access, ret,
                            *String::Utf8Value(name),
                            *String::Utf8Value(arg));
        } else {
            status = gtm_cip(&access, ret,
                            *String::AsciiValue(name),
                            *String::AsciiValue(arg));
        }
    }

    if (status) { 
        gtm_zstatus(msgbuf, BUF_LEN); 

        Handle<Object> result = gtm_status(msgbuf);

        return scope.Close(result);
    } 

    Local<String> nstring = String::New(ret);

    if (nstring->Length() < 1) {
        ThrowException(Exception::TypeError(String::New
                             ("No JSON string present")));

        return scope.Close(Undefined());
    }

    Handle<Value> retvalue = parse_json(nstring);    

    if (retvalue.IsEmpty()) {
        return scope.Close(Undefined());
    }

    Handle<Object> retobject = Handle<Object>::Cast(retvalue);

    if ((arrays->IsUndefined()) || (test->Equals(function))) {

        return scope.Close(retobject);
    } else if ((test->Equals(order)) || (test->Equals(previous))){
        Local<Array> array = Local<Array>::Cast(arrays);

        if (retobject->Get(String::New("errorCode"))->IsUndefined()) {
            array->Set(Number::New(array->Length() - 1),
                       retobject->Get(String::New("result")));

            retobject->Set(String::New("subscripts"), array);
        }

        return scope.Close(retobject);
    } else {
        Local<Array> array = Local<Array>::Cast(arrays);

        if (retobject->Get(String::New("errorCode"))->IsUndefined())
            retobject->Set(String::New("subscripts"), array);

        return scope.Close(retobject);
    }
} //End of call_gtm


Handle<Value> Gtm::set(const Arguments &args)
{
    Local<Value> cmd = String::New("set");

    return call_gtm(cmd, args);
} //End of Gtm::set


Handle<Value> Gtm::get(const Arguments &args)
{
    Local<Value> cmd = String::New("get");

    return call_gtm(cmd, args);
} //End of Gtm::get


Handle<Value> Gtm::kill(const Arguments &args)
{
    Local<Value> cmd = String::New("kill");

    return call_gtm(cmd, args);
} //End of Gtm::kill


Handle<Value> Gtm::data(const Arguments &args)
{
    Local<Value> cmd = String::New("data");

    return call_gtm(cmd, args);
} //End of Gtm::data


Handle<Value> Gtm::order(const Arguments &args)
{
    Local<Value> cmd = String::New("order");

    return call_gtm(cmd, args);
} //End of Gtm::order


Handle<Value> Gtm::previous(const Arguments &args)
{
    Local<Value> cmd = String::New("previous");

    return call_gtm(cmd, args);
} //End of Gtm::previous


Handle<Value> Gtm::next_node(const Arguments &args)
{
    Local<Value> cmd = String::New("next_node");

    return call_gtm(cmd, args);
} //End of Gtm::next_node


Handle<Value> Gtm::previous_node(const Arguments &args)
{
    Local<Value> cmd = String::New("previous_node");

    return call_gtm(cmd, args);
} //End of Gtm::previous_node


Handle<Value> Gtm::increment(const Arguments &args)
{
    Local<Value> cmd = String::New("increment");

    return call_gtm(cmd, args);
} //End of Gtm::increment


Handle<Value> Gtm::merge(const Arguments &args)
{
    Local<Value> cmd = String::New("merge");

    return call_gtm(cmd, args);
} //End of Gtm::merge


Handle<Value> Gtm::global_directory(const Arguments &args)
{
    Local<Value> cmd = String::New("global_directory");

    return call_gtm(cmd, args);
} //End of Gtm::global_directory


Handle<Value> Gtm::lock(const Arguments &args)
{
    Local<Value> cmd = String::New("lock");

    return call_gtm(cmd, args);
} //End of Gtm::lock


Handle<Value> Gtm::unlock(const Arguments &args)
{
    Local<Value> cmd = String::New("unlock");

    return call_gtm(cmd, args);
} //End of Gtm::unlock


Handle<Value> Gtm::function(const Arguments &args)
{
    Local<Value> cmd = String::New("function");

    return call_gtm(cmd, args);
} //End of Gtm::function


Handle<Value> Gtm::retrieve(const Arguments &args)
{
    Local<Value> cmd = String::New("retrieve");

    return call_gtm(cmd, args);
} //End of Gtm::retrieve


Handle<Value> Gtm::update(const Arguments &args)
{
    Local<Value> cmd = String::New("update");

    return call_gtm(cmd, args);
} //End of Gtm::update


Gtm::Gtm() {}
Gtm::~Gtm() {}


void Gtm::Init(Handle<Object> target)
{
    //Prepare constructor template
    Local<FunctionTemplate> tpl = FunctionTemplate::New(New);

    tpl->SetClassName(String::NewSymbol("Gtm"));
    tpl->InstanceTemplate()->SetInternalFieldCount(1);

    //Define prototype macro
    #define NODE_GTM_SET_METHOD(tpl, name, func) \
        tpl->PrototypeTemplate()->Set(String::NewSymbol(name), \
            FunctionTemplate::New(func)->GetFunction());

    NODE_GTM_SET_METHOD(tpl, "about", version);
    NODE_GTM_SET_METHOD(tpl, "close", close);
    NODE_GTM_SET_METHOD(tpl, "data", data);
    NODE_GTM_SET_METHOD(tpl, "function", function);
    NODE_GTM_SET_METHOD(tpl, "get", get);
    NODE_GTM_SET_METHOD(tpl, "global_directory", global_directory);
    NODE_GTM_SET_METHOD(tpl, "increment", increment);
    NODE_GTM_SET_METHOD(tpl, "kill", kill);
    NODE_GTM_SET_METHOD(tpl, "lock", lock);
    NODE_GTM_SET_METHOD(tpl, "merge", merge);
    NODE_GTM_SET_METHOD(tpl, "next", order);
    NODE_GTM_SET_METHOD(tpl, "next_node", next_node);
    NODE_GTM_SET_METHOD(tpl, "open", open);
    NODE_GTM_SET_METHOD(tpl, "order", order);
    NODE_GTM_SET_METHOD(tpl, "previous", previous);
    NODE_GTM_SET_METHOD(tpl, "previous_node", previous_node);
    NODE_GTM_SET_METHOD(tpl, "retrieve", retrieve);
    NODE_GTM_SET_METHOD(tpl, "set", set);
    NODE_GTM_SET_METHOD(tpl, "unlock", unlock);
    NODE_GTM_SET_METHOD(tpl, "update", update);
    NODE_GTM_SET_METHOD(tpl, "version", version);

    #undef NODE_GTM_SET_METHOD

    Persistent<Function> constructor =
        Persistent<Function>::New(tpl->GetFunction());

    target->Set(String::NewSymbol("Gtm"), constructor);
} //End of Gtm::Init


Handle<Value> Gtm::New(const Arguments& args)
{
    HandleScope scope;

    Gtm* obj = new Gtm();
    obj->Wrap(args.This());

    return args.This();
} //End of Gtm::New


NODE_MODULE(mumps, Gtm::Init)
