// ***************************************************************************
// Copyright (c) 2016 SAP SE or an SAP affiliate company. All rights reserved.
// ***************************************************************************

#ifndef __HANA_NODE_H__
#define __HANA_NODE_H__

#include <list>

const double kMaxSafeInteger = 9007199254740991;
const double kMinSafeInteger = -9007199254740991;

template<class T>
class RefPointer {
public:
    RefPointer( T* o = NULL )
        : obj( o ) {
        if( obj ) {
            obj->Ref();
        }
    }
    RefPointer( const RefPointer<T> &o )
        : obj( o.obj ) {
        if( obj ) {
            obj->Ref();
        }
    }
    ~RefPointer() {
        if( obj ) {
            obj->Unref();
        }
    }

    void reset() {
        if( obj ) {
            obj->Unref();
        }
        obj = NULL;
    }

    RefPointer& operator =( const RefPointer<T> &o ) {
        if( obj ) {
            obj->Unref();
        }
        obj = o.obj;
        if( obj ) {
            obj->Ref();
        }
        return *this;
    }

    RefPointer& operator =( T *o ) {
        if( obj ) {
            obj->Unref();
        }
        obj = o;
        if( obj ) {
            obj->Ref();
        }
        return *this;
    }

    const T& operator *() const {
        return *obj;
    }

    T& operator *() {
        return *obj;
    }

    const T* operator ->() const {
        return obj;
    }

    T* operator ->() {
        return obj;
    }

    operator bool() const {
        return !!obj;
    }

    operator T*() {
        return obj;
    }
    operator const T*() const {
        return obj;
    }
private:
    T *obj;
};

class Connection;
typedef RefPointer<Connection> ConnectionPointer;
class Statement;
typedef RefPointer<Statement> StatementPointer;
class ResultSet;
typedef RefPointer<ResultSet> ResultSetPointer;

#endif
