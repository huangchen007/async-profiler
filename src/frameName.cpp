/*
 * Copyright 2017 Andrei Pangin
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cxxabi.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "frameName.h"
#include "vmStructs.h"


FrameName::FrameName(bool simple, bool annotate, bool dotted, Mutex& thread_names_lock, ThreadMap& thread_names) :
    _cache(),
    _simple(simple),
    _annotate(annotate),
    _dotted(dotted),
    _thread_names_lock(thread_names_lock),
    _thread_names(thread_names)
{
    // Require printf to use standard C format regardless of system locale
    _saved_locale = uselocale(newlocale(LC_NUMERIC_MASK, "C", (locale_t)0));
    memset(_buf, 0, sizeof(_buf));
}

FrameName::~FrameName() {
    freelocale(uselocale(_saved_locale));
}

const char* FrameName::cppDemangle(const char* name) {
    if (name != NULL && name[0] == '_' && name[1] == 'Z') {
        int status;
        char* demangled = abi::__cxa_demangle(name, NULL, NULL, &status);
        if (demangled != NULL) {
            strncpy(_buf, demangled, sizeof(_buf) - 1);
            free(demangled);
            return _buf;
        }
    }
    return name;
}

char* FrameName::javaMethodName(jmethodID method) {
    jclass method_class;
    char* class_name = NULL;
    char* method_name = NULL;
    char* result;

    jvmtiEnv* jvmti = VM::jvmti();
    jvmtiError err;

    if ((err = jvmti->GetMethodName(method, &method_name, NULL, NULL)) == 0 &&
        (err = jvmti->GetMethodDeclaringClass(method, &method_class)) == 0 &&
        (err = jvmti->GetClassSignature(method_class, &class_name, NULL)) == 0) {
        // Trim 'L' and ';' off the class descriptor like 'Ljava/lang/Object;'
        result = javaClassName(class_name + 1, strlen(class_name) - 2, _simple, _dotted);
        strcat(result, ".");
        strcat(result, method_name);
        if (_annotate) strcat(result, "_[j]");
    } else {
        snprintf(_buf, sizeof(_buf), "[jvmtiError %d]", err);
        result = _buf;
    }

    jvmti->Deallocate((unsigned char*)class_name);
    jvmti->Deallocate((unsigned char*)method_name);

    return result;
}

char* FrameName::javaClassName(const char* symbol, int length, bool simple, bool dotted) {
    char* result = _buf;

    int array_dimension = 0;
    while (*symbol == '[') {
        array_dimension++;
        symbol++;
    }

    if (array_dimension == 0) {
        strncpy(result, symbol, length);
        result[length] = 0;
    } else {
        switch (*symbol) {
            case 'B': strcpy(result, "byte");    break;
            case 'C': strcpy(result, "char");    break;
            case 'I': strcpy(result, "int");     break;
            case 'J': strcpy(result, "long");    break;
            case 'S': strcpy(result, "short");   break;
            case 'Z': strcpy(result, "boolean"); break;
            case 'F': strcpy(result, "float");   break;
            case 'D': strcpy(result, "double");  break;
            default:
                length -= array_dimension + 2;
                strncpy(result, symbol + 1, length);
                result[length] = 0;
        }

        do {
            strcat(result, "[]");
        } while (--array_dimension > 0);
    }

    if (simple) {
        for (char* s = result; *s; s++) {
            if (*s == '/') result = s + 1;
        }
    }

    if (dotted) {
        for (char* s = result; *s; s++) {
            if (*s == '/') *s = '.';
        }
    }

    return result;
}

const char* FrameName::name(ASGCT_CallFrame& frame) {
    if (frame.method_id == NULL) {
        return "[unknown]";
    }

    switch (frame.bci) {
        case BCI_NATIVE_FRAME:
            return cppDemangle((const char*)frame.method_id);

        case BCI_SYMBOL: {
            VMSymbol* symbol = (VMSymbol*)frame.method_id;
            char* class_name = javaClassName(symbol->body(), symbol->length(), _simple, _dotted);
            return strcat(class_name, _dotted ? "" : "_[i]");
        }

        case BCI_SYMBOL_OUTSIDE_TLAB: {
            VMSymbol* symbol = (VMSymbol*)((uintptr_t)frame.method_id ^ 1);
            char* class_name = javaClassName(symbol->body(), symbol->length(), _simple, _dotted);
            return strcat(class_name, _dotted ? " (out)" : "_[k]");
        }

        case BCI_THREAD_ID: {
            int tid = (int)(uintptr_t)frame.method_id;
            MutexLocker ml(_thread_names_lock);
            ThreadMap::iterator it = _thread_names.find(tid);
            if (it != _thread_names.end()) {
                snprintf(_buf, sizeof(_buf), "[%s tid=%d]", it->second.c_str(), tid);
            } else {
                snprintf(_buf, sizeof(_buf), "[tid=%d]", tid);
            }
            return _buf;
        }

        case BCI_ERROR: {
            snprintf(_buf, sizeof(_buf), "[%s]", (const char*)frame.method_id);
            return _buf;
        }

        default: {
            JMethodCache::iterator it = _cache.lower_bound(frame.method_id);
            if (it != _cache.end() && it->first == frame.method_id) {
                return it->second.c_str();
            }

            const char* newName = javaMethodName(frame.method_id);
            _cache.insert(it, JMethodCache::value_type(frame.method_id, newName));
            return newName;
        }
    }
}
