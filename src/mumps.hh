/*
 * mumps.hh - A MUMPS driver for Node.js
 *
 * Written by David Wicksell <dlw@linux.com>
 * Copyright © 2015-2017 Fourth Watch Software LC
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

#define NODEM_STRING(num)    NODEM_STRINGIFY(num)
#define NODEM_STRINGIFY(num) #num

#define NODEM_MAJOR_VERSION  0
#define NODEM_MINOR_VERSION  9
#define NODEM_PATCH_VERSION  0

#define NODEM_VERSION_STRING NODEM_STRING(NODEM_MAJOR_VERSION) "." \
                             NODEM_STRING(NODEM_MINOR_VERSION) "." \
                             NODEM_STRING(NODEM_PATCH_VERSION)

#if (NODE_MAJOR_VERSION == 0 && NODE_MINOR_VERSION >= 12) || (NODE_MAJOR_VERSION >= 1)
    #define ISOLATE                         isolate
    #define ISOLATE_COMMA                   isolate,
    #define ISOLATE_CURRENT                 v8::Isolate* isolate = v8::Isolate::GetCurrent();
    #define CONTEXT_CURRENT                 isolate->GetCurrentContext();
    #define EXCEPTION(arg)                  isolate->ThrowException(arg)

    #define CAST(name, type)                (type) name

    #define RETURN_DECL                     void
    #define ARGUMENTS                       const v8::FunctionCallbackInfo<v8::Value>&
    #define RETURN

    #define SYMBOL(str)                     v8::String::NewFromUtf8(isolate, str)
    #define STRING(str)                     v8::String::NewFromUtf8(isolate, str)
    #define NUMBER(num)                     v8::Number::New(isolate, num)

    #define ASCII_STRING(name, str)         uint8_t* name = new uint8_t[str->Length() + 1]; str->WriteOneByte(name)
    #define ASCII_PROTO(name)               v8::Local<v8::String> name##_str = name->ToString(); \
                                            uint8_t* name##_buf = new uint8_t[name##_str->Length() + 1]; \
                                            name##_str->WriteOneByte(name##_buf, 0, \
                                            name##_str->Length() + 1)
    #define ASCII_VALUE(name)               (gtm_char_t*) name##_buf

    #define ASCII_NAME(name, str)           const uint8_t* bytes = reinterpret_cast<const uint8_t*>(str); \
                                            name = v8::String::NewFromOneByte(isolate, bytes)

    #define PERSISTENT_FUNCTION(name, func) name.Reset(isolate, func)
    #define CONSTRUCTOR(cons, tpl)          tpl->GetFunction()

    #define SCOPE_HANDLE                    v8::EscapableHandleScope scope(isolate)
    #define SCOPE_ESCAPE(args)              scope.Escape(args)
    #define SCOPE_RESET(name)               name.Reset()
    #define SCOPE_SET(name, args)           name.GetReturnValue().Set(args)
    #define SCOPE_RETURN(args)              return
#else
    #define ISOLATE
    #define ISOLATE_COMMA
    #define ISOLATE_CURRENT
    #define CONTEXT_CURRENT                 v8::Context::GetCurrent()
    #define EXCEPTION(arg)                  v8::ThrowException(arg)

    #define CAST(name, type)                *name

    #define RETURN_DECL                     v8::Handle<v8::Value>
    #define ARGUMENTS                       const v8::Arguments&
    #define RETURN                          return

    #define SYMBOL(str)                     v8::String::NewSymbol(str)
    #define STRING(str)                     v8::String::New(str)
    #define NUMBER(num)                     v8::Number::New(num)

    #define ASCII_STRING(name, str)         v8::String::AsciiValue name(str)
    #define ASCII_PROTO(name)
    #define ASCII_VALUE(name)               *v8::String::AsciiValue(name)

    #define ASCII_NAME(name, str)           name = v8::String::New(str)

    #define PERSISTENT_FUNCTION(name, func) name = v8::Persistent<v8::Function>::New(func)
    #define CONSTRUCTOR(cons, tpl)          cons

    #define SCOPE_HANDLE                    v8::HandleScope scope
    #define SCOPE_ESCAPE(args)              scope.Close(args)
    #define SCOPE_RESET(name)               name.Dispose(); name.Clear()
    #define SCOPE_SET(name, args) 
    #define SCOPE_RETURN(args)              return scope.Close(args)
#endif


class Gtm: public node::ObjectWrap
{
    public:
        Gtm();
        ~Gtm();

        static void Init(v8::Handle<v8::Object>);

    private:
        static RETURN_DECL close(ARGUMENTS);
        static RETURN_DECL data(ARGUMENTS);
        static RETURN_DECL function(ARGUMENTS);
        static RETURN_DECL get(ARGUMENTS);
        static RETURN_DECL global_directory(ARGUMENTS);
        static RETURN_DECL increment(ARGUMENTS);
        static RETURN_DECL kill(ARGUMENTS);
        static RETURN_DECL lock(ARGUMENTS);
        static RETURN_DECL merge(ARGUMENTS);
        static RETURN_DECL next_node(ARGUMENTS);
        static RETURN_DECL open(ARGUMENTS);
        static RETURN_DECL order(ARGUMENTS);
        static RETURN_DECL previous(ARGUMENTS);
        static RETURN_DECL previous_node(ARGUMENTS);
        static RETURN_DECL procedure(ARGUMENTS);
        static RETURN_DECL retrieve(ARGUMENTS);
        static RETURN_DECL set(ARGUMENTS);
        static RETURN_DECL unlock(ARGUMENTS);
        static RETURN_DECL update(ARGUMENTS);
        static RETURN_DECL version(ARGUMENTS);

        static RETURN_DECL New(ARGUMENTS);
}; //End of class Gtm


#endif //End of MUMPS_HH
