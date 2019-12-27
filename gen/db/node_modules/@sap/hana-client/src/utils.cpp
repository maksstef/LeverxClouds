// ***************************************************************************
// Copyright (c) 2016 SAP SE or an SAP affiliate company. All rights reserved.
// ***************************************************************************
#include "nodever_cover.h"
#include "hana_utils.h"

using namespace v8;
using namespace node;

void getErrorMsg( int           code,
                  int&          errCode,
                  std::string&  errText,
                  std::string&  sqlState )
/********************************************/
{
    errCode = code;
    sqlState = std::string( "HY000" );

    switch( code ) {
	case JS_ERR_INVALID_OBJECT:
	    errText = std::string( "Invalid Object" );
	    break;
	case JS_ERR_ALLOCATION_FAILED:
	    errText = std::string( "Allocation Failed" );
	    break;
	case JS_ERR_INVALID_ARGUMENTS:
            errText = std::string( "Invalid Arguments" );
	    break;
	case JS_ERR_CONNECTION_ALREADY_EXISTS:
            errText = std::string( "Already Connected" );
	    break;
	case JS_ERR_INITIALIZING_DBCAPI:
            errText = std::string( "Can't initialize DBCAPI" );
	    break;
	case JS_ERR_NOT_CONNECTED:
            errText = std::string( "No Connection Available" );
	    break;
	case JS_ERR_BINDING_PARAMETERS:
            errText = std::string( "Can not bind parameter(s)" );
	    break;
	case JS_ERR_GENERAL_ERROR:
            errText = std::string( "An error occurred" );
	    break;
	case JS_ERR_RESULTSET:
            errText = std::string( "Error making result set Object" );
	    break;
	case JS_ERR_RESULTSET_CLOSED:
            errText = std::string( "ResultSet is closed" );
	    break;
	case JS_ERR_STATEMENT_DROPPED:
            errText = std::string( "Statement is dropped" );
	    break;
        case JS_ERR_TOO_MANY_PARAMETERS:
            errText = std::string("Too many parameters for the SQL statement");
            break;
        case JS_ERR_NOT_ENOUGH_PARAMETERS:
            errText = std::string("Not enough parameters for the SQL statement");
            break;
        case JS_ERR_NO_FETCH_FIRST:
            errText = std::string("ResetSet not fetched");
            break;
        case JS_ERR_STRING_TOO_LONG:
            errText = std::string("String is too long");
            break;
        case JS_ERR_INVALID_DATA_TYPE:
            errText = std::string("Invalid data type");
            break;
        default:
            errText = std::string( "Unknown Error" );
    }
}

void getErrorMsgBindingParam( int&          errCode,
                              std::string&  errText,
                              std::string&  sqlState,
                              int           invalidParam )
/********************************************/
{
    std::ostringstream msg;
    msg << "Can not bind parameter(" << invalidParam << ").";

    errCode = JS_ERR_BINDING_PARAMETERS;
    sqlState = std::string("HY000");
    errText = msg.str();
}

void getErrorMsgInvalidParam( int&          errCode,
                              std::string&  errText,
                              std::string&  sqlState,
                              int           invalidParam )
/********************************************/
{
    std::ostringstream msg;
    msg << "Invalid argument(" << invalidParam << ").";

    errCode = JS_ERR_INVALID_ARGUMENTS;
    sqlState = std::string("HY000");
    errText = msg.str();
}

void getErrorMsg( dbcapi_connection*    conn,
                  int&                  errCode,
                  std::string&          errText,
                  std::string&          sqlState )
/*************************************************************/
{
    char bufferSqlState[6];
    size_t errLen = api.dbcapi_error_length( conn );
    errLen = (errLen > 0) ? errLen : 1;
    char *bufferErrText = new char[errLen];
    api.dbcapi_sqlstate( conn, bufferSqlState, sizeof( bufferSqlState ) );
    errCode = api.dbcapi_error( conn, bufferErrText, errLen );
    errText = std::string( bufferErrText );
    sqlState = std::string( bufferSqlState );
    delete[] bufferErrText;
}

void setErrorMsg( Local<Object>&   error,
                  int              errCode,
                  std::string&     errText,
                  std::string&     sqlState )
/******************************************/
{
    Isolate *isolate = Isolate::GetCurrent();
    error->Set( String::NewFromUtf8( isolate, "code" ), Integer::New( isolate, errCode ) );
    error->Set( String::NewFromUtf8( isolate, "message" ), String::NewFromUtf8( isolate, errText.c_str() ) );
    error->Set( String::NewFromUtf8( isolate, "sqlState" ), String::NewFromUtf8( isolate, sqlState.c_str() ) );
}

void throwError( int              errCode,
                 std::string&     errText,
                 std::string&     sqlState )
/******************************************/
{
    Isolate *isolate = Isolate::GetCurrent();
    Local<Object> error = Object::New( isolate );
    setErrorMsg( error, errCode, errText, sqlState );

    // Add stack trace
    Local<StackTrace> stackTrace = StackTrace::CurrentStackTrace( isolate, 10, StackTrace::StackTraceOptions::kDetailed );

    std::ostringstream lines;
    lines << "Error: " << errText << std::endl;

    for ( int i = 0; i < stackTrace->GetFrameCount(); i++ ) {
#if NODE_MAJOR_VERSION >= 12
        Local<StackFrame> stackFrame = stackTrace->GetFrame( isolate, i );
#else
        Local<StackFrame> stackFrame = stackTrace->GetFrame( i );
#endif
        int line = stackFrame->GetLineNumber();
        int column = stackFrame->GetColumn();
        //int scriptId = stackFrame->GetScriptId();

        Local<String> src = stackFrame->GetScriptNameOrSourceURL();
        std::string strSrc = convertToString(isolate, src);

        Local<String> script = stackFrame->GetScriptName();
        std::string strScript = convertToString(isolate, script);

        Local<String> fun = stackFrame->GetFunctionName();
        std::string strFun = convertToString(isolate, fun);

        lines << "    at " << strScript;
        if (strFun.size() > 0) {
            lines << "." << strFun;
        }
        lines << " (" << strSrc << ":" << line << ":" << column << ")" << std::endl;
    }

    error->Set( String::NewFromUtf8(isolate, "stack" ),
               String::NewFromUtf8( isolate, lines.str().c_str() ) );

    isolate->ThrowException( error );
}

void throwError( dbcapi_connection *conn )
/******************************************/
{
    int errCode;
    std::string errText;
    std::string sqlState;

    getErrorMsg( conn, errCode, errText, sqlState );
    throwError( errCode, errText, sqlState );
}

void throwError( int code )
/*************************/
{
    int errCode;
    std::string errText;
    std::string sqlState;

    getErrorMsg( code, errCode, errText, sqlState );
    throwError( errCode, errText, sqlState );
}

void throwErrorIP( int paramIndex,
                   const char *function,
                   const char *expectedType,
                   const char *receivedType )
/*************************/
{
    int errCode = JS_ERR_INVALID_ARGUMENTS;
    std::string sqlState = "HY000";
    char buffer[256];

    memset(buffer, 0, sizeof(buffer));
    sprintf(buffer, "Invalid parameter %d for function '%s': expected %s, received %s.",
            paramIndex + 1, function, expectedType, receivedType);

    std::string errText = buffer;
    throwError(errCode, errText, sqlState);
}

unsigned int getJSType( Local<Value> value )
{
    if (value->IsUndefined()) {
        return JS_UNDEFINED;
    } else if (value->IsNull()) {
        return JS_NULL;
    } else if (value->IsString()) {
        return JS_STRING;
    } else if (value->IsBoolean()) {
        return JS_BOOLEAN;
    } else if (value->IsInt32()) {
        return JS_INTEGER;
    } else if (value->IsNumber()) {
        return JS_NUMBER;
    } else if (value->IsFunction()) {
        return JS_FUNCTION;
    } else if (value->IsArray()) {
        return JS_ARRAY;
    } else if (Buffer::HasInstance(value)) {
        return JS_BUFFER;
    } else if (value->IsObject()) {
        return JS_OBJECT;
    } else if (value->IsSymbol()) {
        return JS_SYMBOL;
    } else {
        return JS_UNKNOWN_TYPE;
    }
}

std::string getJSTypeName( unsigned int type )
{
    std::string typeName;

    if ((type & JS_UNDEFINED) == JS_UNDEFINED) {
        typeName += (typeName.length() > 0) ? "|" : "";
        typeName += "undefined";
    }
    if ((type & JS_NULL) == JS_NULL) {
        typeName += (typeName.length() > 0) ? "|" : "";
        typeName += "null";
    }
    if ((type & JS_STRING) == JS_STRING) {
        typeName += (typeName.length() > 0) ? "|" : "";
        typeName += "string";
    }
    if ((type & JS_BOOLEAN) == JS_BOOLEAN) {
        typeName += (typeName.length() > 0) ? "|" : "";
        typeName += "boolean";
    }
    if ((type & JS_INTEGER) == JS_INTEGER) {
        typeName += (typeName.length() > 0) ? "|" : "";
        typeName += "integer";
    }
    if ((type & JS_NUMBER) == JS_NUMBER ) {
        typeName += (typeName.length() > 0) ? "|" : "";
        typeName += "number";
    }
    if ((type & JS_FUNCTION) == JS_FUNCTION) {
        typeName += (typeName.length() > 0) ? "|" : "";
        typeName += "function";
    }
    if ((type & JS_ARRAY) == JS_ARRAY) {
        typeName += (typeName.length() > 0) ? "|" : "";
        typeName += "array";
    }
    if ((type & JS_OBJECT) == JS_OBJECT) {
        typeName += (typeName.length() > 0) ? "|" : "";
        typeName += "object";
    }
    if ((type & JS_BUFFER) == JS_BUFFER) {
        typeName += (typeName.length() > 0) ? "|" : "";
        typeName += "buffer";
    }
    if ((type & JS_SYMBOL) == JS_SYMBOL) {
        typeName += (typeName.length() > 0) ? "|" : "";
        typeName += "symbol";
    }
    if ((type & JS_UNKNOWN_TYPE) == JS_UNKNOWN_TYPE) {
        typeName += (typeName.length() > 0) ? "|" : "";
        typeName += "unknown type";
    }

    return typeName;
}

bool checkParameters( Isolate* isolate,
                      const FunctionCallbackInfo<Value> &args,
                      const char *function,
                      int argCount,
                      unsigned int *expectedType,
                      int *cbFunArg,
                      bool *isOptional,
                      bool *foundOptionalArg )

{
    int argIndex = -1;
    bool boolVal;
    std::string receivedTypeName;

    if (cbFunArg != NULL) {
        *cbFunArg = -1;
    }

    for (int i = 0; i < argCount; i++) {
        argIndex = i;

        if (foundOptionalArg != NULL) {
            foundOptionalArg[i] = false;
        }

        if (i == args.Length()) {
            if (isOptional == NULL || !isOptional[i]) {
                receivedTypeName = getJSTypeName(JS_UNDEFINED);
                break;
            }
        } else {
            unsigned int receivedType = getJSType(args[i]);

            if ((expectedType[i] & receivedType) == receivedType) {
                if (foundOptionalArg != NULL) {
                    foundOptionalArg[i] = true;
                }
            }  else {
                if ((expectedType[i] == JS_BOOLEAN) && convertToBool(isolate, args[i], boolVal)) {
                    if (foundOptionalArg != NULL) {
                        foundOptionalArg[i] = true;
                    }
                } else if (!((receivedType == JS_UNDEFINED || receivedType == JS_NULL) &&
                            (isOptional != NULL && isOptional[i]))) {
                    receivedTypeName = getJSTypeName(receivedType);
                    break;
                }
            }

            if ((cbFunArg != NULL) && (receivedType == JS_FUNCTION)) {
                *cbFunArg = i;
            }
        }
    }

    if (receivedTypeName.length() > 0) {
        throwErrorIP(argIndex, function,
                     getJSTypeName(expectedType[argIndex]).c_str(),
                     receivedTypeName.c_str());
        return false;
    }

    return true;
}

void callBack( int                      errCode,
               std::string *		errText,
               std::string *            sqlState,
	       Persistent<Function> &	callback,
	       Local<Value> &		Result,
	       bool			callback_required,
               bool			has_result )
/*********************************************************/
{
/*    Isolate *isolate = Isolate::GetCurrent();
    HandleScope scope( isolate );
    Local<Function> local_callback = Local<Function>::New( isolate, callback );

    // If string is NULL, then there is no error
    if( callback_required ) {
	if( !local_callback->IsFunction() ) {
	    throwError( JS_ERR_INVALID_ARGUMENTS );
	    return;
	}

        Local<Value> Err;
        if (errText == NULL) {
            Err = Local<Value>::New(isolate, Undefined(isolate));
        }
        else {
            Err = Exception::Error(String::NewFromUtf8(isolate, errText->c_str()));
        }

	int argc = 2;
	Local<Value> argv[2] = { Err, Result };

	TryCatch try_catch( isolate );
	local_callback->Call( isolate->GetCurrentContext()->Global(), argc, argv );
	if( try_catch.HasCaught()) {
	    node::FatalException( isolate, try_catch );
	}
    } else {
	if( errText != NULL ) {
            throwError( errCode, *errText, *sqlState );
	}
    }
*/

    Isolate *isolate = Isolate::GetCurrent();
    HandleScope scope( isolate );

    // If string is NULL, then there is no error
    if( callback_required ) {
        Local<Function> local_callback = Local<Function>::New(isolate, callback);

        if( !local_callback->IsFunction() ) {
	    throwError( JS_ERR_INVALID_ARGUMENTS );
	    return;
	}

        if (errText == NULL) {
            int argc = (has_result) ? 2 : 1;
            Local<Value> Err = Local<Value>::New(isolate, Undefined(isolate));
	    Local<Value> argv[2] = { Err, Result };

	    TryCatch try_catch( isolate );
	    //local_callback->Call( isolate->GetCurrentContext()->Global(), argc, argv );
            MakeCallback(isolate, isolate->GetCurrentContext()->Global(), local_callback, argc, argv);
            if( try_catch.HasCaught()) {
	        node::FatalException( isolate, try_catch );
	    }
        } else {
            int argc = (has_result) ? 2 : 1;
            Local<Object> Err = Object::New( isolate );
            setErrorMsg( Err, errCode, *errText, *sqlState );
            Local<Value> argv[2] = {Err, Object::New(isolate)};

	    TryCatch try_catch( isolate );
	    //local_callback->Call( isolate->GetCurrentContext()->Global(), argc, argv );
            MakeCallback(isolate, isolate->GetCurrentContext()->Global(), local_callback, argc, argv);
            if( try_catch.HasCaught()) {
	        node::FatalException( isolate, try_catch );
	    }
        }
    } else {
	if( errText != NULL ) {
            throwError( errCode, *errText, *sqlState );
	}
    }
}

void callBack( int                      errCode,
               std::string *		errText,
               std::string *            sqlState,
               Persistent<Function> &	callback,
	       Persistent<Value> &	Result,
	       bool			callback_required,
               bool			has_result )
/*********************************************************/
{
    Isolate *isolate = Isolate::GetCurrent();
    HandleScope scope( isolate );
    Local<Value> local_result = Local<Value>::New( isolate, Result );

    callBack( errCode, errText, sqlState, callback, local_result, callback_required, has_result );
}

void callBack( int                      errCode,
               std::string *		errText,
               std::string *            sqlState,
               const Local<Value> &	arg,
	       Local<Value> &		Result,
	       bool			callback_required,
               bool			has_result )
/*********************************************************/
{
/*    Isolate *isolate = Isolate::GetCurrent();
    HandleScope scope( isolate );

    // If string is NULL, then there is no error
    if( callback_required ) {
	if( !arg->IsFunction() ) {
	    throwError( JS_ERR_INVALID_ARGUMENTS );
	    return;
	}
	Local<Function> callback = Local<Function>::Cast(arg);

        Local<Value> Err;
        if (errText == NULL) {
            Err = Local<Value>::New(isolate, Undefined(isolate));
        }
        else {
            Err = Exception::Error(String::NewFromUtf8(isolate, errText->c_str()));
        }

	int argc = 2;
	Local<Value> argv[2] = { Err,  Result };

	TryCatch try_catch;
	callback->Call( isolate->GetCurrentContext()->Global(), argc, argv );
	if( try_catch.HasCaught()) {
	    node::FatalException( isolate, try_catch );
	}
    } else {
	if( errText != NULL ) {
            throwError( errCode, *errText, *sqlState );
	}
    }
*/

    Isolate *isolate = Isolate::GetCurrent();
    HandleScope scope( isolate );

    // If string is NULL, then there is no error
    if( callback_required ) {
	if( !arg->IsFunction() ) {
	    throwError( JS_ERR_INVALID_ARGUMENTS );
	    return;
	}

	Local<Function> callback = Local<Function>::Cast(arg);

        if (errText == NULL) {
            int argc = (has_result) ? 2 : 1;
            Local<Value> Err = Local<Value>::New(isolate, Undefined(isolate));
	    Local<Value> argv[2] = { Err,  Result };

	    TryCatch try_catch( isolate );
	    //callback->Call(isolate->GetCurrentContext()->Global(), argc, argv);
            MakeCallback(isolate, isolate->GetCurrentContext()->Global(), callback, argc, argv);
	    if( try_catch.HasCaught()) {
	        node::FatalException( isolate, try_catch );
	    }
        } else {
            int argc = (has_result) ? 2 : 1;
            Local<Object> Err = Object::New(isolate);
            setErrorMsg(Err, errCode, *errText, *sqlState);
            Local<Value> argv[2] = {Err, Object::New(isolate)};

            TryCatch try_catch( isolate );
            //callback->Call(isolate->GetCurrentContext()->Global(), argc, argv);
            MakeCallback(isolate, isolate->GetCurrentContext()->Global(), callback, argc, argv);
            if (try_catch.HasCaught()) {
                node::FatalException(isolate, try_catch);
            }
        }
    } else {
	if( errText != NULL ) {
            throwError( errCode, *errText, *sqlState );
	}
    }
}

int getReturnValue(Isolate *isolate,
                    dbcapi_data_value & value,
                    dbcapi_native_type nativeType,
                    Local<Value> & localValue)
    /**********************************************************************/
{
    if (value.is_null != NULL && *(value.is_null)) {
        localValue = (Null(isolate));
        return 0;
    }

    int int_number;

    switch (value.type) {
        case A_INVALID_TYPE:
            localValue = (Null(isolate));
            return 0;
        case A_VAL32:
        case A_VAL16:
        case A_UVAL16:
        case A_VAL8:
        case A_UVAL8:
            convertToInt(value, int_number, true);
            if (nativeType == DT_BOOLEAN) {
                localValue = (Boolean::New(isolate, int_number > 0 ? true : false));
            } else {
                localValue = (Integer::New(isolate, int_number));
            }
            return 0;
        case A_UVAL32:
        case A_UVAL64:
        {
            unsigned long long int64_number = *(unsigned long long*)(value.buffer);
            if (int64_number > kMaxSafeInteger ) {
                std::ostringstream strstrm;
                strstrm << int64_number;
                std::string str = strstrm.str();
                localValue = String::NewFromUtf8(isolate, str.c_str(),
                         NewStringType::kNormal, (int)str.length()).ToLocalChecked();
            } else {
                localValue = Number::New(isolate, (double)int64_number);
            }
            return 0;
        }
        case A_VAL64:
        {
            long long int64_number = *(long long*)(value.buffer);
            if (int64_number > kMaxSafeInteger || int64_number < kMinSafeInteger) {
                std::ostringstream strstrm;
                strstrm << int64_number;
                std::string str = strstrm.str();
                localValue = String::NewFromUtf8(isolate, str.c_str(),
                         NewStringType::kNormal, (int)str.length()).ToLocalChecked();
            } else {
                localValue = Number::New(isolate, (double)int64_number);
            }
            return 0;
        }
        case A_FLOAT:
        case A_DOUBLE:
            localValue = (Number::New(isolate, *(double*)(value.buffer)));
            return 0;
        case A_BINARY:
        {
            MaybeLocal<Object> mbuf = node::Buffer::Copy(
                isolate, (char *)value.buffer,
                (int)*(value.length));
            localValue = (mbuf.ToLocalChecked());
            return 0;
        }
        case A_STRING:
        {
            int len = 0;
            if (value.buffer == NULL) {
                len = 0;
            } else {
                if (value.length != NULL && *value.length == 0) {
                    len = 0;
                } else if (value.length != NULL) {
                    len = (int)*value.length;
                } else {
                    len = (int)strlen((char*)value.buffer);
                }
            }
            MaybeLocal<String> mlValue = String::NewFromUtf8(isolate,
                (char *)value.buffer, NewStringType::kNormal, len);
            if (mlValue.IsEmpty()) {
                return JS_ERR_STRING_TOO_LONG;  // nullptr, string is too long
            } else {
                localValue = mlValue.ToLocalChecked();
                return 0;
            }
        }
        default: return JS_ERR_INVALID_DATA_TYPE;
    }
}

bool checkParameterCount( int &                             errCode,
                          std::string &                     errText,
                          std::string &                     sqlState,
                          std::vector<dbcapi_bind_data*> &  providedParams,
                          dbcapi_stmt *                     dbcapi_stmt_ptr )
/**********************************************************************/
{
    int inputParamCount = 0;
    int numParams = api.dbcapi_num_params(dbcapi_stmt_ptr);

    for (int i = 0; i < numParams; i++) {
        dbcapi_bind_data param;
        api.dbcapi_describe_bind_param(dbcapi_stmt_ptr, i, &param);
        if (param.direction == DD_INPUT || param.direction == DD_INPUT_OUTPUT) {
            inputParamCount++;
        }
    }

    if (inputParamCount < (int) providedParams.size()) {
        getErrorMsg(JS_ERR_TOO_MANY_PARAMETERS, errCode, errText, sqlState);
        return false;
    } else if (inputParamCount > (int) providedParams.size()) {
        getErrorMsg(JS_ERR_NOT_ENOUGH_PARAMETERS, errCode, errText, sqlState);
        return false;
    }

    return true;
}

bool getInputParameters( Isolate*                          isolate,
                         Local<Value>                      arg,
                         std::vector<dbcapi_bind_data*> &  params,
                         dbcapi_stmt *                     stmt,
                         int &                             errCode,
                         std::string &                     errText,
                         std::string &                     sqlState )
/**********************************************************************/
{
    Local<Context> context = isolate->GetCurrentContext();

    params.clear();

    if (arg->IsArray()) {
        Local<Array> bind_params = Local<Array>::Cast(arg);

        for (int i = 0; i < (int)bind_params->Length(); i++) {
            dbcapi_bind_data* data = getBindParameter(isolate, bind_params->Get(i));
            if (data == NULL) {
                getErrorMsgBindingParam(errCode, errText, sqlState, i);
                return false;
            } else {
                params.push_back(data);
            }
        }
    } else if (arg->IsObject()) {
        Local<Object> obj = arg->ToObject(isolate);
        Local<Array> props = (obj->GetOwnPropertyNames(context)).ToLocalChecked();
        int numParams = api.dbcapi_num_params(stmt);

        for (int i = 0; i < numParams; i++) {
            dbcapi_bind_data param;
            api.dbcapi_describe_bind_param(stmt, i, &param);
            if (param.direction == DD_INPUT || param.direction == DD_INPUT_OUTPUT) {
                bool found = false;
                if (param.name != NULL) {
                    for (unsigned int j = 0; j < props->Length(); j++) {
                        Local<String> key = props->Get(j).As<String>();
#if NODE_MAJOR_VERSION >= 12
                        String::Utf8Value key_utf8(isolate, key);
#else
                        String::Utf8Value key_utf8(key);
#endif
                        std::string strKey(*key_utf8);
                        std::string strName = param.name;
                        if (compareString(strKey, strName, true)) {
                            Local<Value> val = obj->Get(key);
                            dbcapi_bind_data* data = getBindParameter(isolate, val);
                            found = (data != NULL);
                            if (found) {
                                params.push_back(data);
                            }
                            break;
                        }
                    }
                }
                if (!found) {
                    getErrorMsgBindingParam(errCode, errText, sqlState, i);
                    return false;
                }
            }
        }
    }

    return true;
}

int getBindParameters(std::vector<dbcapi_bind_data*> &    inputParams,
                      std::vector<dbcapi_bind_data*> &    params,
                      dbcapi_stmt *                       stmt)
 /**********************************************************************/
{
    int numParams = api.dbcapi_num_params(stmt);

    params.clear();

    for (int i = 0; i < numParams; i++) {
        dbcapi_bind_data* param = new dbcapi_bind_data();
        api.dbcapi_describe_bind_param(stmt, i, param);
        param->value.length = new size_t;
        *param->value.length = 0;
        param->value.is_null = new dbcapi_bool;
        *param->value.is_null = false;

        if ( ( param->direction == DD_INPUT || param->direction == DD_INPUT_OUTPUT ) &&
             ( inputParams.size() > 0 ) ) {
            if ( inputParams[0]->value.type == A_STRING && param->value.type == A_VAL32 ) {
                int invalidParam = -1;
                int* pInt = new int;
                char* pEnd = NULL;
                if (inputParams[0]->value.buffer == NULL || (inputParams[0]->value.buffer)[0] == 0) {
                    invalidParam = i;
                } else {
                    *pInt = (int)strtol(inputParams[0]->value.buffer, &pEnd, 10);
                    if (*pEnd) { // parameter not a number
                        invalidParam = i;
                    }
                }
                param->value.buffer = (char *)(pInt);
                param->value.buffer_size = sizeof(int);
                *param->value.length = sizeof(int);
                params.push_back(param);
                clearParameter(inputParams[0], true);
                inputParams.erase(inputParams.begin());
                if (invalidParam >= 0) {
                    return invalidParam;
                }
            } else {
                if (param->value.type == A_STRING && inputParams[0]->value.type == A_BINARY) {
                    inputParams[0]->value.type = A_STRING;
                }
                params.push_back(inputParams[0]);
                inputParams.erase(inputParams.begin());
                clearParameter(param, true);
            }
        } else {
            params.push_back(param);
        }
    }

    return -1;
}

// Used for execBatch
bool getBindParameters(Isolate *                       isolate,
                       Local<Value>                    arg,
                       int                              row_param_count,
                       std::vector<dbcapi_bind_data*> &	params,
                       std::vector<size_t> &	        buffer_size,
                       std::vector<unsigned int> &      param_types)
/**********************************************************************/
{
    Local<Context> context = isolate->GetCurrentContext();
    Local<Array> bind_params = Local<Array>::Cast(arg);

    params.clear();
    buffer_size.clear();

    for (int k = 0; k < row_param_count; k++) {
        buffer_size.push_back(0);
    }

    for (unsigned int i = 0; i < bind_params->Length(); i++) {
        Local<Array> row = Local<Array>::Cast(bind_params->Get(i));
        for (int j = 0; j < row_param_count; j++) {
            dbcapi_bind_data* param = NULL;

            if (param_types[j] == JS_NUMBER && row->Get(j)->IsInt32()) {
                param = new dbcapi_bind_data();
                memset(param, 0, sizeof(dbcapi_bind_data));
                param->value.is_null = new dbcapi_bool;
                *param->value.is_null = false;
                double *param_double = new double;
                *param_double = (double) (row->Get(j)->Int32Value(context)).FromJust();
                param->value.buffer = (char *)(param_double);
                param->value.type = A_DOUBLE;
                param->value.buffer_size = sizeof(double);
                param->value.length = new size_t;
                *param->value.length = sizeof(double);
            } else {
                param = getBindParameter(isolate, row->Get(j));
            }

            if (param == NULL) {
                return false;
            }

            if (row->Get(j)->IsNull() || row->Get(j)->IsUndefined()) {
                if (param_types[j] == JS_BOOLEAN) {
                    param->value.type = A_VAL32;
                } else if (param_types[j] == JS_INTEGER) {
                    param->value.type = A_VAL32;
                } else if (param_types[j] == JS_NUMBER) {
                    param->value.type = A_DOUBLE;
                } else if (param_types[j] == JS_STRING) {
                    param->value.type = A_STRING;
                } else if (param_types[j] == JS_BUFFER) {
                    param->value.type = A_BINARY;
                }
            }

            if (buffer_size[j] < param->value.buffer_size) {
                buffer_size[j] = param->value.buffer_size;
            }

            params.push_back(param);
        }
    }

    return true;
}

dbcapi_bind_data* getBindParameter( Isolate *isolate, Local<Value> element )
/**********************************************************************/
{
    if (!(element->IsNull() || element->IsUndefined() || element->IsInt32() || element->IsNumber() ||
          element->IsBoolean() || element->IsString() || element->IsObject() || Buffer::HasInstance(element))) {
        return NULL;
    }

    Local<Context> context = isolate->GetCurrentContext();
    dbcapi_bind_data* param = new dbcapi_bind_data();

    memset(param, 0, sizeof(dbcapi_bind_data));
    param->value.length = new size_t;
    *param->value.length = 0;
    param->value.is_null = new dbcapi_bool;
    *param->value.is_null = false;

    if (element->IsNull() || element->IsUndefined()) {
        param->value.type = A_VAL32;
        *param->value.is_null = true;
    }
    else if (element->IsBoolean()) {
        int *param_int = new int;
        *param_int = (element->BooleanValue(context)).FromJust() ? 1 : 0;
        param->value.buffer = (char *)(param_int);
        param->value.type = A_VAL32;
        param->value.buffer_size = sizeof(int);
        *param->value.length = sizeof(int);
    }
    else if (element->IsInt32()) {
        int *param_int = new int;
        *param_int = (element->Int32Value(context)).FromJust();
        param->value.buffer = (char *)(param_int);
        param->value.type = A_VAL32;
        param->value.buffer_size = sizeof(int);
        *param->value.length = sizeof(int);
    }
    else if (element->IsNumber()) {
        double *param_double = new double;
        *param_double = (element->NumberValue(context)).FromJust(); // Remove Round off Error
        param->value.buffer = (char *)(param_double);
        param->value.type = A_DOUBLE;
        param->value.buffer_size = sizeof(double);
        *param->value.length = sizeof(double);
    }
    else if (element->IsString()) {
#if NODE_MAJOR_VERSION >= 12
        String::Utf8Value paramValue(isolate, (element->ToString(context)).ToLocalChecked());
#else
        String::Utf8Value paramValue((element->ToString(context)).ToLocalChecked());
#endif
        size_t len = (size_t)paramValue.length();
        char *param_char = new char[len + 1];
        memcpy(param_char, *paramValue, len + 1);
        param->value.type = A_STRING;
        param->value.buffer = param_char;
        *param->value.length = len;
        param->value.buffer_size = len + 1;
    }
    else if (Buffer::HasInstance(element)) {
        size_t len = Buffer::Length(element);
        char *param_char = new char[len];
        memcpy(param_char, Buffer::Data(element), len);
        param->value.type = A_BINARY;
        param->value.buffer = param_char;
        *param->value.length = len;
        param->value.buffer_size = len;
    }
    else if (element->IsObject()) { // Length for LOB types
        Local<Object> obj = (element->ToObject(context)).ToLocalChecked();
        Local<Array> props = (obj->GetOwnPropertyNames(context)).ToLocalChecked();
        std::string propKey = "sendParameterData";
        std::string propVal = "true";
        bool hasProp = false;

        for (unsigned int j = 0; j < props->Length(); j++) {
            Local<String> key = props->Get(j).As<String>();
            Local<String> val = obj->Get(key).As<String>();
#if NODE_MAJOR_VERSION >= 12
            String::Utf8Value key_utf8(isolate, key);
            String::Utf8Value val_utf8(isolate, val);
#else
            String::Utf8Value key_utf8(key);
            String::Utf8Value val_utf8(val);
#endif
            if (*val_utf8 == NULL) {
                continue;
            }
            std::string strKey(*key_utf8);
            std::string strVal(*val_utf8);
            if (compareString(strKey, propKey, false) && compareString(strVal, propVal, false)) {
                size_t *len = new size_t;
                *len = 2147483647;
                param->value.length = len;
                param->value.buffer = NULL;
                param->value.type = A_INVALID_TYPE; // LOB types
                param->value.buffer_size = 0;
                hasProp = true;
                break;
            }
        }
        if (!hasProp) {
            clearParameter(param, true);
            return NULL;
        }
    }

    return param;
}

bool bindParameters(dbcapi_connection *                 conn,
                    dbcapi_stmt *                       stmt,
                    std::vector<dbcapi_bind_data*> &    params,
                    int &                               errCode,
                    std::string &                       errText,
                    std::string &                       sqlState,
                    bool &                              sendParamData)
/*************************************************************************/
{
    sendParamData = false;

    for (int i = 0; i < (int)(params.size()); i++) {
        dbcapi_bind_data param;
        dbcapi_bind_param_info info;

        if (!api.dbcapi_get_bind_param_info(stmt, i, &info) ||
            !api.dbcapi_describe_bind_param(stmt, i, &param)) {
            getErrorMsg(conn, errCode, errText, sqlState);
            return false;
        }

        if (param.direction == DD_OUTPUT || param.direction == DD_INPUT_OUTPUT) {
            if ((info.native_type != DT_BLOB && info.native_type != DT_CLOB && info.native_type != DT_NCLOB) &&
                (params[i]->value.buffer == NULL || info.max_size >= params[i]->value.buffer_size ||
                 (params[i]->value.type == A_STRING && info.max_size * 4 >= params[i]->value.buffer_size))) {
                size_t size = info.max_size + 1;
                if (params[i]->value.type == A_STRING && info.max_size * 4 >= params[i]->value.buffer_size) {
                    size = info.max_size * 4 + 1; // Max UTF-8 Encoding size is 4 bytes.
                }
                char *bufferNew = new char[size];
                if (params[i]->value.buffer != NULL) {
                    memcpy(bufferNew, params[i]->value.buffer, params[i]->value.buffer_size);
                    delete params[i]->value.buffer;
                }
                params[i]->value.buffer = bufferNew;
                params[i]->value.buffer_size = size;
            }
        }

        param.value.buffer = params[i]->value.buffer;
        param.value.length = params[i]->value.length;
        param.value.buffer_size = params[i]->value.buffer_size;
        param.value.is_null = params[i]->value.is_null;

        if (params[i]->value.type == A_INVALID_TYPE) { // LOB
            sendParamData = true;
        } else {
            param.value.type = params[i]->value.type;
        }

        if (!api.dbcapi_bind_param(stmt, i, &param)) {
            getErrorMsg(conn, errCode, errText, sqlState);
            return false;
        }
    }

    return true;
}

void clearParameter(dbcapi_bind_data* param, bool free)
/*************************************************************************/
{
    if (param != NULL) {
        if (param->value.buffer != NULL) {
            delete param->value.buffer;
            param->value.buffer = NULL;
        }
        if (param->value.is_null != NULL) {
            delete param->value.is_null;
            param->value.is_null = NULL;
        }
        if (param->value.length != NULL) {
            delete param->value.length;
            param->value.length = NULL;
        }
        if (free) {
            delete param;
        }
    }
}

void clearParameters(std::vector<dbcapi_bind_data*> & params)
/*************************************************************************/
{
    while (params.size() > 0) {
        clearParameter(params[params.size() -1], true);
        params.pop_back();
    }
}

void clearParameters(dbcapi_bind_data* params, int count)
/*************************************************************************/
{
    if (params != NULL) {
        dbcapi_bind_data *p = params;
        for (int i = 0; i < count; i++) {
            clearParameter(p, false);
            p++;
        }
        delete params;
    }
}

void copyParameters(std::vector<dbcapi_bind_data*> & paramsDest,
                    std::vector<dbcapi_bind_data*> & paramsSrc)
/*************************************************************************/
{
    clearParameters(paramsDest);

    for (size_t i = 0; i < paramsSrc.size(); i++) {
        dbcapi_bind_data *data = new dbcapi_bind_data();
        memcpy(data, paramsSrc[i], sizeof(dbcapi_bind_data));
        paramsDest.push_back(data);
        if (paramsSrc[i]->value.is_null != NULL) {
            paramsDest[i]->value.is_null = new dbcapi_bool;
            *paramsDest[i]->value.is_null = *paramsSrc[i]->value.is_null;
        }
        if (paramsSrc[i]->value.length != NULL) {
            paramsDest[i]->value.length = new size_t;
            *paramsDest[i]->value.length = *paramsSrc[i]->value.length;
        }
        if (paramsSrc[i]->value.buffer != NULL) {
            size_t size = paramsSrc[i]->value.buffer_size;
            if (size > 0) {
                paramsDest[i]->value.buffer = new char[size];
                memcpy(paramsDest[i]->value.buffer, paramsSrc[i]->value.buffer, size);
            } else {
                paramsDest[i]->value.buffer = NULL;
            }
        }
    }
}

bool fillResult(executeBaton *baton, Persistent<Value> &ResultSet)
/*************************************************************************/
{
    Isolate *isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);
    Local<Value> undef = Local<Value>::New(isolate, Undefined(isolate));

    if (baton->err) {
        callBack(baton->error_code, &(baton->error_msg), &(baton->sql_state),
                 baton->callback, undef, baton->callback_required);
        return false;
    }

    if (!getResultSet(ResultSet, baton->rows_affected, baton->col_names,
        baton->string_vals, baton->int_vals, baton->long_vals, baton->ulong_vals, baton->double_vals,
        baton->date_vals, baton->time_vals, baton->timestamp_vals,
        baton->col_types, baton->col_native_types, baton)) {
        getErrorMsg(JS_ERR_RESULTSET, baton->error_code, baton->error_msg, baton->sql_state);
        callBack(baton->error_code, &(baton->error_msg), &(baton->sql_state),
                 baton->callback, undef, baton->callback_required);
        return false;
    }
    if (baton->callback_required) {
        // No result for DDL statements
        bool hasResult = baton->function_code != 1;
        callBack(baton->error_code, NULL, &(baton->sql_state),
            baton->callback, ResultSet, baton->callback_required, hasResult);
    }
    return true;
}

bool getResultSet(Persistent<Value> &			Result,
                  int &				        rows_affected,
                  std::vector<char *> &		        colNames,
                  std::vector<char*> &			string_vals,
                  std::vector<int> &			int_vals,
                  std::vector<long long> &	        long_vals,
                  std::vector<unsigned long long> &	ulong_vals,
                  std::vector<double> &                 double_vals,
                  std::vector<a_date> &                 date_vals,
                  std::vector<a_time> &                 time_vals,
                  std::vector<a_timestamp> &            timestamp_vals,
                  std::vector<dbcapi_data_type> &	col_types,
                  std::vector<dbcapi_native_type> &     col_native_types,
                  executeBaton *                        baton)
/*****************************************************************/
{
    Isolate *isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);
    bool	reset_strings;
    int 	num_rows = 0;
    size_t	num_cols = colNames.size();

    if (rows_affected >= 0) {
        Result.Reset(isolate, Integer::New(isolate, rows_affected));
        return true;
    }

    if (num_cols > 0) {
        size_t count = 0;
        size_t count_int = 0, count_long = 0, count_ulong = 0, count_double = 0, count_string = 0;
        size_t count_date = 0, count_time = 0, count_timestamp = 0;
        Local<Array> ResultSet = Array::New(isolate);

        Local<Object> curr_row;
        Local<Array> curr_row_ar;
        Local<Object> row;
        std::vector<Local<Object>> objects;

        std::vector<Local<String>> colNamesLocal;
        for (size_t i = 0; i < num_cols; i++) {
            colNamesLocal.push_back(String::NewFromUtf8(isolate, colNames[i]));
        }

        while (count < col_types.size() ||
               count_int < int_vals.size() ||
               count_long < long_vals.size() ||
               count_ulong < ulong_vals.size() ||
               count_double < double_vals.size() ||
               count_date < date_vals.size() ||
               count_time < time_vals.size() ||
               count_timestamp < timestamp_vals.size() ||
               count_string < string_vals.size()) {

            if (!baton->exec_options.rowsAsArray) {
                curr_row = Object::New(isolate);
                if (baton->exec_options.nestTables) {
                    objects.clear();
                    for (uint32_t i = 0; i < num_cols; i++) {
                        char* table_name = baton->col_infos[i]->table_name;
                        if (table_name == NULL || strlen(table_name) == 0) {
                            objects.push_back(curr_row);
                        } else {
                            for (uint32_t j = 0; j < i; j++) {
                                if (strcmp(table_name, baton->col_infos[j]->table_name) == 0) {
                                    objects.push_back(objects[j]);
                                    break;
                                }
                            }
                            if (objects.size() < i + 1) {
                                Local<Object> object = Object::New(isolate);
                                curr_row->Set(String::NewFromUtf8(isolate, table_name), object);
                                objects.push_back(object);
                            }
                        }
                    }
                }
            } else {
                curr_row_ar = Array::New(isolate, (int)num_cols);
            }

            num_rows++;

            for (uint32_t i = 0; i < num_cols; i++) {
                if (!baton->exec_options.rowsAsArray) {
                    row = (baton->exec_options.nestTables) ? objects[i] : curr_row;
                }
                switch (col_types[count]) {
                    case A_INVALID_TYPE:
                        if (baton->exec_options.rowsAsArray) {
                            curr_row_ar->Set(i, Null(isolate));
                        } else {
                            row->Set(colNamesLocal[i], Null(isolate));
                        }
                        break;

                    case A_VAL32:
                    case A_VAL16:
                    case A_UVAL16:
                    case A_VAL8:
                    case A_UVAL8:
                        if (baton->exec_options.rowsAsArray) {
                            if (col_native_types[i] == DT_BOOLEAN) {
                                curr_row_ar->Set(i, Boolean::New(isolate, int_vals[count_int] > 0 ? true : false));
                            } else {
                                curr_row_ar->Set(i, Integer::New(isolate, int_vals[count_int]));
                            }
                        } else {
                            if (col_native_types[i] == DT_BOOLEAN) {
                                row->Set(colNamesLocal[i], Boolean::New(isolate, int_vals[count_int] > 0 ? true : false));
                            } else {
                                row->Set(colNamesLocal[i], Integer::New(isolate, int_vals[count_int]));
                            }
                        }
                        count_int++;
                        break;

                    case A_UVAL32:
                    case A_UVAL64:
                    {
                        unsigned long long int64_number = ulong_vals[count_ulong];
                        if (baton->exec_options.rowsAsArray) {
                            if (int64_number > kMaxSafeInteger) {
                                std::ostringstream strstrm;
                                strstrm << int64_number;
                                std::string str = strstrm.str();
                                curr_row_ar->Set(i, String::NewFromUtf8(
                                                    isolate, str.c_str(),
                                                    NewStringType::kNormal, (int)str.length()).ToLocalChecked());
                            } else {
                                curr_row_ar->Set(i, Number::New(isolate, (double)int64_number));
                            }
                        } else {
                            if (int64_number > kMaxSafeInteger) {
                                std::ostringstream strstrm;
                                strstrm << int64_number;
                                std::string str = strstrm.str();
                                row->Set(colNamesLocal[i], String::NewFromUtf8(
                                                    isolate, str.c_str(),
                                                    NewStringType::kNormal, (int)str.length()).ToLocalChecked());
                            } else {
                                row->Set(colNamesLocal[i], Number::New(isolate, (double)int64_number));
                            }
                        }
                        count_ulong++;
                        break;
                    }
                    case A_VAL64:
                    {
                        long long int64_number = long_vals[count_long];
                        if (baton->exec_options.rowsAsArray) {
                            if (int64_number > kMaxSafeInteger || int64_number < kMinSafeInteger) {
                                std::ostringstream strstrm;
                                strstrm << int64_number;
                                std::string str = strstrm.str();
                                curr_row_ar->Set(i, String::NewFromUtf8(
                                                    isolate, str.c_str(),
                                                    NewStringType::kNormal, (int)str.length()).ToLocalChecked());
                            } else {
                                curr_row_ar->Set(i, Number::New(isolate, (double)int64_number));
                            }
                        } else {
                            if (int64_number > kMaxSafeInteger || int64_number < kMinSafeInteger) {
                                std::ostringstream strstrm;
                                strstrm << int64_number;
                                std::string str = strstrm.str();
                                row->Set(colNamesLocal[i], String::NewFromUtf8(
                                                    isolate, str.c_str(),
                                                    NewStringType::kNormal, (int)str.length()).ToLocalChecked());
                            } else {
                                row->Set(colNamesLocal[i], Number::New(isolate, (double)int64_number));
                            }
                        }
                        count_long++;
                        break;
                    }
                    case A_FLOAT:
                    case A_DOUBLE:
                        if (baton->exec_options.rowsAsArray) {
                            curr_row_ar->Set(i, Number::New(isolate, double_vals[count_double]));
                        } else {
                            row->Set(colNamesLocal[i], Number::New(isolate, double_vals[count_double]));
                        }
                        count_double++;
                        break;
                    case A_BINARY:
                        if (baton->exec_options.rowsAsArray) {
                            if (string_vals[count_string] == NULL) {
                                curr_row_ar->Set(i, Null(isolate));
                            } else {
                                size_t len;
                                char *data;
                                memcpy(&len, string_vals[count_string], sizeof(len));
                                data = string_vals[count_string] + sizeof(size_t);
                                MaybeLocal<Object> mbuf = node::Buffer::Copy(
                                    isolate, data,
                                    static_cast<int>(len));
                                Local<Object> buf = mbuf.ToLocalChecked();
                                curr_row_ar->Set(i, buf);
                            }
                        } else {
                            if (string_vals[count_string] == NULL) {
                                row->Set(colNamesLocal[i], Null(isolate));
                            } else {
                                size_t len;
                                char *data;
                                memcpy(&len, string_vals[count_string], sizeof(len));
                                data = string_vals[count_string] + sizeof(size_t);
                                MaybeLocal<Object> mbuf = node::Buffer::Copy(
                                    isolate, data,
                                    static_cast<int>(len));
                                Local<Object> buf = mbuf.ToLocalChecked();
                                row->Set(colNamesLocal[i], buf);
                            }
                        }
                        delete[] string_vals[count_string];
                        string_vals[count_string] = NULL;
                        count_string++;
                        break;

                    case A_STRING:
                        reset_strings = false;
                        if (baton->exec_options.rowsAsArray) {
                            int dbcapi_len = 0;
                            if (col_native_types[i] == DT_DATE || col_native_types[i] == DT_DAYDATE) {
                                dbcapi_len = date_vals[count_date].len;
                                if (dbcapi_len != -1) {
                                    curr_row_ar->Set(i,
                                                  String::NewFromUtf8(isolate,
                                                  date_vals[count_date].val,
                                                  NewStringType::kNormal,
                                                  dbcapi_len).ToLocalChecked());
                                }
                                count_date++;
                            } else if (col_native_types[i] == DT_TIME || col_native_types[i] == DT_SECONDTIME) {
                                dbcapi_len = time_vals[count_time].len;
                                if (dbcapi_len != -1) {
                                    curr_row_ar->Set(i,
                                                  String::NewFromUtf8(isolate,
                                                  time_vals[count_time].val,
                                                  NewStringType::kNormal,
                                                  dbcapi_len).ToLocalChecked());
                                }
                                count_time++;
                            } else if (col_native_types[i] == DT_TIMESTAMP || col_native_types[i] == DT_LONGDATE ||
                                       col_native_types[i] == DT_SECONDDATE) {
                                dbcapi_len = timestamp_vals[count_timestamp].len;
                                if (dbcapi_len != -1) {
                                    curr_row_ar->Set(i,
                                                  String::NewFromUtf8(isolate,
                                                  timestamp_vals[count_timestamp].val,
                                                  NewStringType::kNormal,
                                                  dbcapi_len).ToLocalChecked());
                                }
                                count_timestamp++;
                            } else {
                                if (string_vals[count_string] == NULL) {
                                    dbcapi_len = -1;
                                } else {
                                    size_t len;
                                    char *data;
                                    memcpy(&len, string_vals[count_string], sizeof(len));
                                    data = string_vals[count_string] + sizeof(size_t);
                                    curr_row_ar->Set(i,
                                                  String::NewFromUtf8(isolate,
                                                  data,
                                                  NewStringType::kNormal,
                                                  static_cast<int>(len)).ToLocalChecked());
                                }
                                reset_strings = true;
                            }
                            if (dbcapi_len == -1) {
                                curr_row_ar->Set(i, Null(isolate));
                            }
                        } else {
                            int dbcapi_len = 0;
                            if (col_native_types[i] == DT_DATE || col_native_types[i] == DT_DAYDATE) {
                                dbcapi_len = date_vals[count_date].len;
                                if (dbcapi_len != -1) {
                                    row->Set(colNamesLocal[i],
                                                  String::NewFromUtf8(isolate,
                                                  date_vals[count_date].val,
                                                  NewStringType::kNormal,
                                                  dbcapi_len).ToLocalChecked());
                                }
                                count_date++;
                            } else if (col_native_types[i] == DT_TIME || col_native_types[i] == DT_SECONDTIME) {
                                dbcapi_len = time_vals[count_time].len;
                                if (dbcapi_len != -1) {
                                    row->Set(colNamesLocal[i],
                                                  String::NewFromUtf8(isolate,
                                                  time_vals[count_time].val,
                                                  NewStringType::kNormal,
                                                  dbcapi_len).ToLocalChecked());
                                }
                                count_time++;
                            } else if (col_native_types[i] == DT_TIMESTAMP || col_native_types[i] == DT_LONGDATE ||
                                       col_native_types[i] == DT_SECONDDATE) {
                                dbcapi_len = timestamp_vals[count_timestamp].len;
                                if (dbcapi_len != -1) {
                                    row->Set(colNamesLocal[i],
                                                  String::NewFromUtf8(isolate,
                                                  timestamp_vals[count_timestamp].val,
                                                  NewStringType::kNormal,
                                                  dbcapi_len).ToLocalChecked());
                                }
                                count_timestamp++;
                            } else {
                                if (string_vals[count_string] == NULL) {
                                    dbcapi_len = -1;
                                } else {
                                    size_t len;
                                    char *data;
                                    memcpy(&len, string_vals[count_string], sizeof(len));
                                    data = string_vals[count_string] + sizeof(size_t);
                                    row->Set(colNamesLocal[i],
                                                  String::NewFromUtf8(isolate,
                                                  data,
                                                  NewStringType::kNormal,
                                                  static_cast<int>(len)).ToLocalChecked());
                                }
                                reset_strings = true;
                            }
                            if (dbcapi_len == -1) {
                                row->Set(colNamesLocal[i], Null(isolate));
                            }
                        }
                        if (reset_strings) {
                            delete[] string_vals[count_string];
                            string_vals[count_string] = NULL;
                            count_string++;
                        }
                        break;

                    default:
                        return false;
                }
                count++;
            }
            if (baton->exec_options.rowsAsArray) {
                ResultSet->Set(num_rows - 1, curr_row_ar);
            } else {
                ResultSet->Set(num_rows - 1, curr_row);
            }
        }
        Result.Reset(isolate, ResultSet);
    } else {
        Result.Reset(isolate, Local<Value>::New(isolate,
                     Undefined(isolate)));
    }

    return true;
}

void clearValues(std::vector<dbcapi_data_value*> & values)
/*****************************************************************/
{
}

bool fetchResultSet( dbcapi_stmt *			dbcapi_stmt_ptr,
		     int 				rs_size,
		     int &				rows_affected,
		     std::vector<char *> &		colNames,
		     std::vector<char*> &		string_vals,
		     std::vector<int> &		        int_vals,
		     std::vector<long long> &		long_vals,
		     std::vector<unsigned long long> &	ulong_vals,
		     std::vector<double> &		double_vals,
		     std::vector<a_date> &		date_vals,
		     std::vector<a_time> &		time_vals,
		     std::vector<a_timestamp> &		timestamp_vals,
		     std::vector<dbcapi_data_type> &	col_types,
                     std::vector<dbcapi_native_type> &  col_native_types )
/*****************************************************************/
{
    dbcapi_data_value		value;
    int				num_cols = 0;

    rows_affected = api.dbcapi_affected_rows( dbcapi_stmt_ptr );
    num_cols = api.dbcapi_num_cols( dbcapi_stmt_ptr );

    if( num_cols < 1 ) {
        return true;
    }

    rows_affected = -1;
    if (num_cols > 0) {
        bool has_lob = false;
        int rowset_size = rs_size;
        dataValueCollection bind_cols;

        bool has_string = false;
        bool has_int = false;
        bool has_long = false;
        bool has_ulong = false;
        bool has_double = false;
        bool has_date = false;
        bool has_time = false;
        bool has_timestamp = false;

        for (int i = 0; i < num_cols; i++) {
            dbcapi_column_info info;
            api.dbcapi_get_column_info(dbcapi_stmt_ptr, i, &info);
            size_t size = strlen(info.name) + 1;
            char *name = new char[size];
            memcpy(name, info.name, size);
            colNames.push_back(name);
            col_native_types.push_back(info.native_type);

            // The native type of ARRAY/ST_POINT/ST_GEOMETRY is DT_VARBINARY.
            if (info.native_type == DT_BLOB || info.native_type == DT_CLOB || info.native_type == DT_NCLOB || info.native_type == DT_VARBINARY) {
                has_lob = true;
            }

            if (!has_lob) {
                dbcapi_data_value* bind_col = new dbcapi_data_value();
                size_t size = info.max_size;
                if (info.native_type == DT_DECIMAL) {
                    size = 128;
                } else if (info.native_type == DT_NVARCHAR || info.native_type == DT_NCHAR ||
                           info.native_type == DT_VARCHAR1 || info.native_type == DT_VARCHAR2 ||
                           info.native_type == DT_CHAR) {
                    size = (info.max_size > 0) ? info.max_size * 4 + 1 : 5000 * 4 + 1; // Max UTF-8 Encoding size is 4 bytes.
                }
                bind_col->buffer_size = size;
                bind_col->buffer = new char[size * rowset_size];
                bind_col->length = new size_t[rowset_size];
                bind_col->is_null = new dbcapi_bool[rowset_size];
                bind_col->type = info.type;
                bind_cols.push_back(bind_col);
            }
            if (info.type == A_STRING) {
                if (info.native_type == DT_DATE || info.native_type == DT_DAYDATE) {
                    has_date = true;
                } else if (info.native_type == DT_TIME || info.native_type == DT_SECONDTIME) {
                    has_time = true;
                } else if (info.native_type == DT_TIMESTAMP || info.native_type == DT_LONGDATE ||
                           info.native_type == DT_SECONDDATE) {
                    has_timestamp = true;
                }
            }
            if (info.type == A_STRING || info.type == A_BINARY) {
                has_string = true;
            } else if (info.type == A_VAL64) {
                has_long = true;
            } else if (info.type == A_UVAL64 || info.type == A_UVAL32) {
                has_ulong = true;
            } else if (info.type == A_VAL32 || info.type == A_VAL16 || info.type == A_UVAL16 ||
                       info.type == A_VAL8 || info.type == A_UVAL8) {
                has_int = true;
            } else if (info.type == A_DOUBLE || info.type == A_FLOAT) {
                has_double = true;
            }
        }

        // Increase the capacity to improve performance.
        int reserve_size = 16 * 1024;
        col_types.reserve(reserve_size);
        if (has_string) {
            string_vals.reserve(reserve_size);
        }
        if (has_int) {
            int_vals.reserve(reserve_size);
        }
        if (has_long) {
            long_vals.reserve(reserve_size);
        }
        if (has_ulong) {
            ulong_vals.reserve(reserve_size);
        }
        if (has_double) {
            double_vals.reserve(reserve_size);
        }
        // Reduce the capacity to save memory.
        reserve_size = 4 * 1024;
        if (has_date) {
            date_vals.reserve(reserve_size);
        }
        if (has_time) {
            time_vals.reserve(reserve_size);
        }
        if (has_timestamp) {
            timestamp_vals.reserve(reserve_size);
        }

        if (!has_lob) {

            if (!api.dbcapi_set_rowset_size(dbcapi_stmt_ptr, rowset_size)) {
                return false;
            }

            for (int i = 0; i < num_cols; i++) {
                if (!api.dbcapi_bind_column(dbcapi_stmt_ptr, i, bind_cols[i])) {
                    return false;
                }
            }
        }

        int count_string = 0, count_int = 0, count_long = 0, count_ulong = 0, count_double = 0;
        int count_date = 0, count_time = 0, count_timestamp = 0;
        while (api.dbcapi_fetch_next(dbcapi_stmt_ptr)) {

            int fetched_rows = api.dbcapi_fetched_rows(dbcapi_stmt_ptr);

            for (int row = 0; row < fetched_rows; row++) {

                for (int i = 0; i < num_cols; i++) {
                    if (has_lob) {
                        if (!api.dbcapi_get_column(dbcapi_stmt_ptr, i, &value)) {
                            return false;
                        }
                    }
                    else {
                        value.buffer = bind_cols[i]->buffer + row * bind_cols[i]->buffer_size;
                        value.buffer_size = bind_cols[i]->buffer_size;
                        value.type = bind_cols[i]->type;
                        value.length = bind_cols[i]->length + row;
                        value.is_null = bind_cols[i]->is_null + row;
                    }

                    if (*(value.is_null)) {
                        col_types.push_back(A_INVALID_TYPE);
                        continue;
                    }

                    switch (value.type) {
                        case A_BINARY:
                        {
                            char *mem = new char[*(value.length) + 1 + sizeof(size_t)];
                            size_t *size = (size_t*)mem;
                            char *val = mem + sizeof(size_t);
                            *size = *(value.length);
                            memcpy(val, (char *)value.buffer, *size);
                            string_vals.push_back(mem);
                            count_string++;
                            break;
                        }

                        case A_STRING:
                        {
                            size_t length = *value.length;
                            if (col_native_types[i] == DT_DATE || col_native_types[i] == DT_DAYDATE) {
                                a_date date_val;
                                date_val.len = static_cast<int>(length);
                                memcpy(date_val.val, value.buffer, date_val.len);
                                date_vals.push_back(date_val);
                                count_date++;
                            } else if (col_native_types[i] == DT_TIME || col_native_types[i] == DT_SECONDTIME) {
                                a_time time_val;
                                time_val.len = static_cast<int>(length);
                                memcpy(time_val.val, value.buffer, time_val.len);
                                time_vals.push_back(time_val);
                                count_time++;
                            } else if (col_native_types[i] == DT_TIMESTAMP || col_native_types[i] == DT_LONGDATE ||
                                       col_native_types[i] == DT_SECONDDATE) {
                                a_timestamp ts_val;
                                ts_val.len = static_cast<int>(length);
                                memcpy(ts_val.val, value.buffer, ts_val.len);
                                timestamp_vals.push_back(ts_val);
                                count_timestamp++;
                            } else {
                                char *mem = new char[length + 1 + sizeof(size_t)];
                                size_t *size = (size_t*)mem;
                                char *val = mem + sizeof(size_t);
                                *size = length;
                                memcpy(val, (char *)value.buffer, *size);
                                string_vals.push_back(mem);
                                count_string++;
                            }
                            break;
                        }

                        case A_VAL64:
                        {
                            long_vals.push_back(*(long long *)value.buffer);
                            count_long++;
                            break;
                        }

                        case A_UVAL64:
                        {
                            ulong_vals.push_back(*(unsigned long long *)value.buffer);
                            count_ulong++;
                            break;
                        }

                        case A_UVAL32:
                        {
                            unsigned int val = *(unsigned int*)value.buffer;
                            ulong_vals.push_back((unsigned long long)val);
                            count_ulong++;
                            break;
                        }

                        case A_VAL32:
                        {
                            int_vals.push_back(*(int*)value.buffer);
                            count_int++;
                            break;
                        }

                        case A_VAL16:
                        {
                            int_vals.push_back((int)*(short*)value.buffer);
                            count_int++;
                            break;
                        }

                        case A_UVAL16:
                        {
                            int_vals.push_back((int)*(unsigned short*)value.buffer);
                            count_int++;
                            break;
                        }

                        case A_VAL8:
                        {
                            int_vals.push_back((int)*(char *)value.buffer);
                            count_int++;
                            break;
                        }

                        case A_UVAL8:
                        {
                            int_vals.push_back((int)*(unsigned char *)value.buffer);
                            count_int++;
                            break;
                        }

                        case A_DOUBLE:
                        {
                            // FLOAT/REAL/DOUBLE are all bound as double
                            double_vals.push_back(*(double *)value.buffer);
                            count_double++;
                            break;
                        }
                        case A_FLOAT:
                        {
                            float val = *(float *)value.buffer;
                            double_vals.push_back((double)val);
                            count_double++;
                            break;
                        }

                        default:
                            return false;
                    }
                    col_types.push_back(value.type);
                }
            }
        }
    }

    return true;
}

bool compareString( const std::string &str1, const std::string &str2, bool caseSensitive )
/*************************************************************/
{
    std::locale loc;

    if( str1.length() == str2.length() ) {
        for( size_t i = 0; i < str1.length(); i++ ) {
            if( caseSensitive && ( str1[i] != str2[i] ) ) {
                return false;
            } else if( !caseSensitive && ( std::tolower( str1[i], loc ) != std::tolower( str2[i], loc) ) ) {
                return false;
            }
        }
        return true;
    }

    return false;
}

bool compareString(const std::string &str1, const char* str2, bool caseSensitive)
/*************************************************************/
{
    return compareString( str1, std::string( str2 ), caseSensitive );
}

// Connection Functions

int getHostsString(Isolate* isolate, Local<Array> arHosts, std::string& strHosts)
/*************************************************************/
{
    Local<Context> context = isolate->GetCurrentContext();
    int hostCount = 0;

    if (arHosts->Length() > 0) {
        std::string hostKey = "host";
        std::string portKey = "port";
        std::string dbKey = "database";
        std::string instNumKey = "instanceNumber";

        for (int i = 0; i < (int) arHosts->Length(); i++) {
            if (!arHosts->Get(i)->IsObject()) {
                continue;
            }

            std::string hostVal = "";
            std::string portVal = "";
            std::string dbVal = "";
            std::string instNumVal = "";

            Local<Object> hostObj = arHosts->Get(i).As<Object>();
            Local<Array> hostProps = (hostObj->GetOwnPropertyNames(context)).ToLocalChecked();

            for (int j = 0; j < (int) hostProps->Length(); j++) {
                Local<String> key = hostProps->Get(j).As<String>();
                Local<String> val = hostObj->Get(key).As<String>();
#if NODE_MAJOR_VERSION >= 12
                String::Utf8Value key_utf8(isolate, key);
                String::Utf8Value val_utf8(isolate, val);
#else
                String::Utf8Value key_utf8(key);
                String::Utf8Value val_utf8(val);
#endif
                std::string strKey(*key_utf8);
                std::string strVal(*val_utf8);

                if( compareString( strKey, hostKey, false )) {
                    hostVal = strVal;
                    continue;
                } else if( compareString( strKey, portKey, false )) {
                    portVal = strVal;
                    continue;
                } else if( compareString( strKey, dbKey, false )) {
                    dbVal = strVal;
                    continue;
                } else if( compareString( strKey, instNumKey, false )) {
                    instNumVal = strVal;
                    continue;
                }
            }

            if (hostVal.length() > 0) {
                hostCount++;
                if (hostCount > 1) {
                    strHosts += ",";
                }
                if ((hostVal.find(':') != std::string::npos) && (hostVal.find('[') == std::string::npos)) {
                    strHosts += '[' + hostVal + ']';
                } else {
                    strHosts += hostVal;
                }
                if (portVal.length() > 0) {
                    strHosts += ":";
                    strHosts += portVal;
                } else if (instNumVal.length() > 0) {
                    strHosts += ":3";
                    strHosts += instNumVal;
                    if ( dbVal.length() > 0 ) {
                        strHosts += "13";
                    } else {
                        strHosts += "15";
                    }
                }
            }
        }
    }

    return hostCount;
}

int getString(Isolate* isolate, Local<Value> value, std::string& str)
/*************************************************************/
{
    int count = 0;

    if (value->IsString()) {
        Local<String> str_local = value.As<String>();
#if NODE_MAJOR_VERSION >= 12
        String::Utf8Value str_utf8(isolate, str_local);
#else
        String::Utf8Value str_utf8(str_local);
#endif
        str = *str_utf8;
        count++;
    } else if (value->IsArray()) {
        Local<Array> ar = value.As<Array>();
        for (int i = 0; i < (int)ar->Length(); i++) {
            Local<Value> item = ar->Get(i);
            if (item->IsString()) {
                Local<String> str_local = item.As<String>();
#if NODE_MAJOR_VERSION >= 12
                String::Utf8Value str_utf8(isolate, str_local);
#else
                String::Utf8Value str_utf8(str_local);
#endif
                if (count > 0) {
                    str += "\n";
                    str += *str_utf8;
                } else {
                    str = *str_utf8;
                }
                count++;
            }
        }
    }

    return count;
}

void getConnectionString( Isolate * isolate,
                          Local<Object> obj,
                          Persistent<String> & ret,
                          std::vector<std::string*> & conn_prop_keys,
                          std::vector<std::string*> & conn_prop_values )
/*************************************************************/
{
    Local<Context> context = isolate->GetCurrentContext();
    HandleScope	scope(isolate);
    Local<Array> props = (obj->GetOwnPropertyNames(context)).ToLocalChecked();
    int length = props->Length();
    std::string params = "";
    std::string hostsKey = "hosts";
    std::string hostsVal = "";
    std::string hostKey = "host";
    std::string hostVal = "";
    std::string portKey = "port";
    std::string portVal = "";
    std::string dbKey = "database";
    std::string dbKey2 = "databasename";
    std::string dbVal = "";
    std::string instNumKey = "instanceNumber";
    std::string instNumVal = "";
    std::string sslKeyKey = "key";
    std::string sslKeyVal = "";
    std::string sslCertKey = "cert";
    std::string sslCertVal = "";
    std::string sslCaKey = "ca";
    std::string sslCaVal = "";
    bool	first = true;

    for (size_t i = 0; i < conn_prop_keys.size(); i++) {
        delete conn_prop_keys[i];
    }
    conn_prop_keys.clear();
    for (size_t i = 0; i < conn_prop_values.size(); i++) {
        delete conn_prop_values[i];
    }
    conn_prop_values.clear();

    for( int i = 0; i < length; i++ ) {
	Local<String> key = props->Get(i).As<String>();
#if NODE_MAJOR_VERSION >= 12
        String::Utf8Value key_utf8(isolate, key);
#else
        String::Utf8Value key_utf8(key);
#endif
        std::string strKey(*key_utf8);

        if (compareString(strKey, hostsKey, false)) {
            getHostsString(isolate, obj->Get(key).As<Array>(), hostsVal);
            continue;
        } else if (compareString(strKey, sslKeyKey, false)) {
            getString(isolate, obj->Get(key), sslKeyVal);
            continue;
        } else if (compareString(strKey, sslCertKey, false)) {
            getString(isolate, obj->Get(key), sslCertVal);
            continue;
        } else if (compareString(strKey, sslCaKey, false)) {
            getString(isolate, obj->Get(key), sslCaVal);
            continue;
        }

	Local<String> val = obj->Get(key).As<String>();
#if NODE_MAJOR_VERSION >= 12
        String::Utf8Value val_utf8( isolate, val );
#else
        String::Utf8Value val_utf8( val );
#endif
        std::string strVal( *val_utf8 );

        // Check host and port
        if( compareString( strKey, hostKey, false )) {
            hostVal = strVal;
            continue;
        } else if( compareString( strKey, portKey, false )) {
            portVal = strVal;
            continue;
        } else if( compareString( strKey, dbKey, false ) || compareString( strKey, dbKey2, false) ) {
            dbVal = strVal;
            continue;
        } else if ( compareString( strKey, instNumKey, false )) {
            instNumVal = strVal;
            continue;
        }

        // Check other keys
        if( compareString( strKey, "user", false ) || compareString( strKey, "username", false ) ) {
            strKey = std::string( "uid" );
        } else if ( compareString( strKey, "password", false ) || compareString( strKey, "pwd", false ) ) {
            strKey = std::string( "pwd" );
        }

        if( !first ) {
	    params += ";";
	}
	first = false;
	params += strKey;
	params += "=";
	params += strVal;

        conn_prop_keys.push_back(new std::string(strKey));
        conn_prop_values.push_back(new std::string(strVal));
    }

    if( hostsVal.length() > 0 ) {
        if( params.length() > 0 ) {
            params += ";";
        }
        params += "ServerNode=";
        params += hostsVal;
        conn_prop_keys.push_back(new std::string("ServerNode"));
        conn_prop_values.push_back(new std::string(hostsVal));
    }

    if( hostVal.length() > 0 ) {
        std::string hostStr;
        if ((hostVal.find(':') != std::string::npos) && (hostVal.find('[') == std::string::npos)) {
            hostStr += '[' + hostVal + ']';
        } else {
            hostStr += hostVal;
        }
        if( portVal.length() > 0) {
            hostStr += ":";
            hostStr += portVal;
        } else if( instNumVal.length() > 0) {
            hostStr += ":3";
            hostStr += instNumVal;
            if ( dbVal.length() > 0 ) {
                hostStr += "13";
            } else {
                hostStr += "15";
            }
        }
        if (params.length() > 0) {
            params += ";";
        }
        params += "ServerNode=";
        params += hostStr;
        conn_prop_keys.push_back(new std::string("ServerNode"));
        conn_prop_values.push_back(new std::string(hostStr));
    }

    if (dbVal.length() > 0) {
        if (params.length() > 0) {
            params += ";";
        }
        params += "databaseName=";
        params += dbVal;
        conn_prop_keys.push_back(new std::string("databaseName"));
        conn_prop_values.push_back(new std::string(dbVal));
    }

    if (sslCaVal.length() > 0) {
        if (params.length() > 0) {
            params += ";";
        }
        params += "sslTrustStore=";
        params += sslCaVal;
        conn_prop_keys.push_back(new std::string("sslTrustStore"));
        conn_prop_values.push_back(new std::string(sslCaVal));
    }

    if (sslKeyVal.length() > 0 || sslCertVal.length() > 0) {
        std::string sslKeyStoreStr("");
        if (sslKeyVal.length() > 0) {
            sslKeyStoreStr += sslKeyVal;
        }
        if (sslCertVal.length() > 0) {
            if (sslKeyVal.length() > 0) {
                sslKeyStoreStr += "\n";
                sslKeyStoreStr += sslCertVal;
            } else {
                sslKeyStoreStr += sslCertVal;
            }
        }
        if (params.length() > 0) {
            params += ";";
        }
        params += "sslKeyStore=";
        params += sslKeyStoreStr;
        conn_prop_keys.push_back(new std::string("sslKeyStore"));
        conn_prop_values.push_back(new std::string(sslKeyStoreStr));
    }

    if (sslKeyVal.length() > 0 || sslCertVal.length() > 0 || sslCaVal.length() > 0) {
        params += ";ENCRYPT=true";
        conn_prop_keys.push_back(new std::string("ENCRYPT"));
        conn_prop_values.push_back(new std::string("true"));
        if (!(sslKeyVal.length() > 0 || sslCertVal.length() > 0)) {
            params += ";sslKeyStore=";
            conn_prop_keys.push_back(new std::string("sslKeyStore"));
            conn_prop_values.push_back(new std::string(""));
        }
    }

    ret.Reset( isolate, String::NewFromUtf8( isolate, params.c_str() ) );
}

bool getExecuteOptions(Isolate* isolate, MaybeLocal<Object> mbObj, executeOptions* options)
/*************************************************************/
{
    Local<Context> context = isolate->GetCurrentContext();
    Local<Object> obj = mbObj.ToLocalChecked();
    Local<Array> props = (obj->GetOwnPropertyNames(context)).ToLocalChecked();
    std::string nestTablesKey = "nestTables";
    std::string rowsAsArrayKey = "rowsAsArray";
    std::string propVal = "true";
    bool hasProp = false;

    options->init();

    for (unsigned int i = 0; i < props->Length(); i++) {
        Local<String> key = props->Get(i).As<String>();
        Local<String> val = obj->Get(key).As<String>();
#if NODE_MAJOR_VERSION >= 12
        String::Utf8Value key_utf8(isolate, key);
        String::Utf8Value val_utf8(isolate, val);
#else
        String::Utf8Value key_utf8(key);
        String::Utf8Value val_utf8(val);
#endif
        std::string strKey(*key_utf8);
        std::string strVal(*val_utf8);
        if (compareString(strKey, nestTablesKey, false) && compareString(strVal, propVal, false)) {
            options->nestTables = true;
            hasProp = true;
        } else if (compareString(strKey, rowsAsArrayKey, false) && compareString(strVal, propVal, false)) {
            options->rowsAsArray = true;
            hasProp = true;
        }
    }

    return hasProp;
}

#if 0
// Handy function for determining what type an object is.
static void CheckArgType( Local<Value> &obj )
/*******************************************/
{
    static const char *type = NULL;
    if( obj->IsArray() ) {
	type = "Array";
    } else if( obj->IsBoolean() ) {
	type = "Boolean";
    } else if( obj->IsBooleanObject() ) {
	type = "BooleanObject";
    } else if( obj->IsDate() ) {
	type = "Date";
    } else if( obj->IsExternal() ) {
	type = "External";
    } else if( obj->IsFunction() ) {
	type = "Function";
    } else if( obj->IsInt32() ) {
	type = "Int32";
    } else if( obj->IsNativeError() ) {
	type = "NativeError";
    } else if( obj->IsNull() ) {
	type = "Null";
    } else if( obj->IsNumber() ) {
	type = "Number";
    } else if( obj->IsNumberObject() ) {
	type = "NumberObject";
    } else if( obj->IsObject() ) {
	type = "Object";
    } else if( obj->IsRegExp() ) {
	type = "RegExp";
    } else if( obj->IsString() ) {
	type = "String";
    } else if( obj->IsStringObject() ) {
	type = "StringObject";
    } else if( obj->IsUint32() ) {
	type = "Uint32";
    } else if( obj->IsUndefined() ) {
	type = "Undefined";
    } else {
	type = "Unknown";
    }
}

bool StringtoDate(const char *str, Local<Value> &val, dbcapi_connection *conn)
/********************************************************************************/
{
    Isolate *isolate = Isolate::GetCurrent();

    dbcapi_data_value value;
    std::string reformat_stmt = "SELECT DATEFORMAT( '";
    reformat_stmt.append(str);
    reformat_stmt.append("', 'YYYY-MM-DD HH:NN:SS.SSS' ) ");

    dbcapi_stmt *stmt = api.dbcapi_execute_direct(conn, reformat_stmt.c_str());
    if (stmt == NULL) {
        throwError(conn);
        return false;
    }
    if (!api.dbcapi_fetch_next(stmt)) {
        api.dbcapi_free_stmt(stmt);
        throwError(conn);
        return false;
    }
    if (!api.dbcapi_get_column(stmt, 0, &value)) {
        api.dbcapi_free_stmt(stmt);
        throwError(conn);
        return false;
    }

    std::ostringstream out;
    if (!convertToString(value, out)) {
        return false;
    }

    api.dbcapi_free_stmt(stmt);
    std::string tempstr;

    std::stringstream ss;
    ss << out.str().substr(0, 4) << ' '
        << out.str().substr(5, 2) << ' '
        << out.str().substr(8, 2) << ' '
        << out.str().substr(11, 2) << ' '
        << out.str().substr(14, 2) << ' '
        << out.str().substr(17, 2) << ' '
        << out.str().substr(20, 2);

    unsigned int year, month, day, hour, min, sec, millisec;

    ss >> year >> month >> day >> hour >> min >> sec >> millisec;

    Local<Object> obj = Local<Object>::Cast(val);
    {
        Local<Value> set_prop = Local<Object>::Cast(val)->Get(
            String::NewFromUtf8(isolate, "setDate"));
        Local<Value> argv[1] = { Local<Value>::New(isolate,
                                                   Number::New(isolate, day)) };
        Local<Function> set_func = Local<Function>::Cast(set_prop);
        set_func->Call(obj, 1, argv);
    }

    {
        Local<Value> set_prop = Local<Object>::Cast(val)->Get(
            String::NewFromUtf8(isolate, "setMonth"));
        Local<Value> argv[1] = { Local<Value>::New(isolate,
                                                   Number::New(isolate, month - 1)) };
        Local<Function> set_func = Local<Function>::Cast(set_prop);
        set_func->Call(obj, 1, argv);
    }

    {
        Local<Value> set_prop = Local<Object>::Cast(val)->Get(
            String::NewFromUtf8(isolate, "setFullYear"));
        Local<Value> argv[1] = { Local<Value>::New(isolate,
                                                   Number::New(isolate, year)) };
        Local<Function> set_func = Local<Function>::Cast(set_prop);
        set_func->Call(obj, 1, argv);
    }

    {
        Local<Value> set_prop = Local<Object>::Cast(val)->Get(
            String::NewFromUtf8(isolate, "setMilliseconds"));
        Local<Value> argv[1] = { Local<Value>::New(isolate,
                                                   Number::New(isolate, millisec)) };
        Local<Function> set_func = Local<Function>::Cast(set_prop);
        set_func->Call(obj, 1, argv);
    }

    {
        Local<Value> set_prop = Local<Object>::Cast(val)->Get(
            String::NewFromUtf8(isolate, "setSeconds"));
        Local<Value> argv[1] = { Local<Value>::New(isolate,
                                                   Number::New(isolate, sec)) };
        Local<Function> set_func = Local<Function>::Cast(set_prop);
        set_func->Call(obj, 1, argv);
    }

    {
        Local<Value> set_prop = Local<Object>::Cast(val)->Get(
            String::NewFromUtf8(isolate, "setMinutes"));
        Local<Value> argv[1] = { Local<Value>::New(isolate,
                                                   Number::New(isolate, min)) };
        Local<Function> set_func = Local<Function>::Cast(set_prop);
        set_func->Call(obj, 1, argv);
    }

    {
        Local<Value> set_prop = Local<Object>::Cast(val)->Get(
            String::NewFromUtf8(isolate, "setHours"));
        Local<Value> argv[1] = { Local<Value>::New(isolate,
                                                   Number::New(isolate, hour)) };
        Local<Function> set_func = Local<Function>::Cast(set_prop);
        set_func->Call(obj, 1, argv);
    }
    return true;
}
#endif

bool convertToBool(Isolate* isolate, Local<Value> val, bool &out)
{
    Local<Context> context = isolate->GetCurrentContext();

    if (val->IsBoolean()) {
        out = (val->BooleanValue(context)).FromJust();
        return true;
    } else if (val->IsString()) {
        Local<String> localStr = val->ToString(isolate);
        std::string str = convertToString(isolate, localStr);
        if (compareString(str, "true", false) ||
            compareString(str, "on", false) ||
            compareString(str, "1", false)) {
            out = true;
            return true;
        } else if (compareString(str, "false", false) ||
                   compareString(str, "off", false) ||
                   compareString(str, "0", false)) {
            out = false;
            return true;
        }
    }
    return false;
}

bool convertToString(dbcapi_data_value	        value,
                     std::ostringstream &	out,
                     bool			do_throw_error)
/**************************************************************/
{
    switch (value.type) {
        case A_BINARY:
            out.write((char *)value.buffer, (int)*(value.length));
            break;
        case A_STRING:
            out.write((char *)value.buffer, (int)*(value.length));
            break;
        case A_VAL64:
            out << (double)*(long long *)value.buffer;
            break;
        case A_UVAL64:
            out << (double)*(unsigned long long *)value.buffer;
            break;
        case A_VAL32:
            out << *(int*)value.buffer;
            break;
        case A_UVAL32:
            out << *(unsigned int*)value.buffer;
            break;
        case A_VAL16:
            out << *(short*)value.buffer;
            break;
        case A_UVAL16:
            out << *(unsigned short*)value.buffer;
            break;
        case A_VAL8:
            out << *(char *)value.buffer;
            break;
        case A_UVAL8:
            out << *(unsigned char *)value.buffer;
            break;
        case A_DOUBLE:
            out << *(double *)value.buffer;
            break;
        default:
            if (do_throw_error) {
                throwError(JS_ERR_RETRIEVING_DATA);  // Can't retrieve data
            }
            return false;
            break;
    }
    return true;
}

bool convertToDouble(dbcapi_data_value	        value,
                     double &			number,
                     bool			do_throw_error)
/**************************************************************/
{
    switch (value.type) {
        case A_VAL64:
            number = (double)*(long long *)value.buffer;
            break;
        case A_UVAL64:
            number = (double)*(unsigned long long *)value.buffer;
            break;
        case A_VAL32:
            number = *(int*)value.buffer;
            break;
        case A_UVAL32:
            number = *(unsigned int*)value.buffer;
            break;
        case A_VAL16:
            number = *(short*)value.buffer;
            break;
        case A_UVAL16:
            number = *(unsigned short*)value.buffer;
            break;
        case A_VAL8:
            number = *(char *)value.buffer;
            break;
        case A_UVAL8:
            number = *(unsigned char *)value.buffer;
            break;
        case A_DOUBLE:
            number = *(double *)value.buffer;
            break;
        case A_STRING:
        {
            size_t	len = *value.length;
            char *strval = (char *)malloc(len + 1);
            if (strval != NULL) {
                memcpy(strval, value.buffer, len);
                strval[len] = '\0';
                number = atof(strval);
                free(strval);
                break;
            } // else fall through
        }
        default:
            if (do_throw_error) {
                throwError(JS_ERR_RETRIEVING_DATA);  // Can't retrieve data
            }
            return false;
            break;
    }

    return true;
}

bool convertToInt(dbcapi_data_value value, int &number, bool do_throw_error)
/******************************************************************************/
{
    switch (value.type) {
        case A_VAL32:
            number = *(int*)value.buffer;
            break;
        case A_VAL16:
            number = *(short*)value.buffer;
            break;
        case A_UVAL16:
            number = *(unsigned short*)value.buffer;
            break;
        case A_VAL8:
            number = *(char *)value.buffer;
            break;
        case A_UVAL8:
            number = *(unsigned char *)value.buffer;
            break;
        case A_STRING:
        {
            size_t	len = *value.length;
            char *strval = (char *)malloc(len + 1);
            if (strval != NULL) {
                memcpy(strval, value.buffer, len);
                strval[len] = '\0';
                number = atoi(strval);
                free(strval);
                break;
            } // else fall through
        }
        default:
            if (do_throw_error) {
                throwError(JS_ERR_RETRIEVING_DATA);  // Can't retrieve data
            }
            return false;
    }

    return true;
}

const char* getTypeName(dbcapi_data_type type)
/******************************************************************************/
{
    switch (type) {
        case A_INVALID_TYPE: return "INVALID_TYPE";
        case A_BINARY:       return "BINARY";
        case A_STRING:       return "STRING";
        case A_DOUBLE:       return "DOUBLE";
        case A_VAL64:        return "VAL64";
        case A_UVAL64:       return "UVAL64";
        case A_VAL32:        return "VAL32";
        case A_UVAL32:       return "UVAL32";
        case A_VAL16:        return "VAL16";
        case A_UVAL16:       return "UVAL16";
        case A_VAL8:         return "VAL8";
        case A_UVAL8:        return "UVAL8";
        case A_FLOAT:        return "FLOAT";
    }
    return "UNKNOWN";
}

const char* getNativeTypeName(dbcapi_native_type nativeType)
/******************************************************************************/
{
    switch (nativeType) {
        case DT_NULL:                return "NULL";
        case DT_TINYINT:             return "TINYINT";
        case DT_SMALLINT:            return "SMALLINT";
        case DT_INT:                 return "INT";
        case DT_BIGINT:              return "BIGINT";
        case DT_DECIMAL:             return "DECIMAL";
        case DT_REAL:                return "REAL";
        case DT_DOUBLE:              return "DOUBLE";
        case DT_CHAR:                return "CHAR";
        case DT_VARCHAR1:            return "VARCHAR1";
        case DT_NCHAR:               return "NCHAR";
        case DT_NVARCHAR:            return "NVARCHAR";
        case DT_BINARY:              return "BINARY";
        case DT_VARBINARY:           return "VARBINARY";
        case DT_DATE:                return "DATE";
        case DT_TIME:                return "TIME";
        case DT_TIMESTAMP:           return "TIMESTAMP";
        case DT_TIME_TZ:             return "TIME_TZ";
        case DT_TIME_LTZ:            return "TIME_LTZ";
        case DT_TIMESTAMP_TZ:        return "TIMESTAMP_TZ";
        case DT_TIMESTAMP_LTZ:       return "TIMESTAMP_LTZ";
        case DT_INTERVAL_YM:         return "INTERVAL_YM";
        case DT_INTERVAL_DS:         return "INTERVAL_DS";
        case DT_ROWID:               return "ROWID";
        case DT_UROWID:              return "UROWID";
        case DT_CLOB:                return "CLOB";
        case DT_NCLOB:               return "NCLOB";
        case DT_BLOB:                return "BLOB";
        case DT_BOOLEAN:             return "BOOLEAN";
        case DT_STRING:              return "STRING";
        case DT_NSTRING:             return "NSTRING";
        case DT_LOCATOR:             return "LOCATOR";
        case DT_NLOCATOR:            return "NLOCATOR";
        case DT_BSTRING:             return "BSTRING";
        case DT_DECIMAL_DIGIT_ARRAY: return "DECIMAL_DIGIT_ARRAY";
        case DT_VARCHAR2:            return "VARCHAR2";
        case DT_TABLE:               return "TABLE";
        case DT_ABAPSTREAM:          return "ABAPSTREAM";
        case DT_ABAPSTRUCT:          return "ABAPSTRUCT";
        case DT_ARRAY:               return "ARRAY";
        case DT_TEXT:                return "TEXT";
        case DT_SHORTTEXT:           return "SHORTTEXT";
        case DT_BINTEXT:             return "BINTEXT";
        case DT_ALPHANUM:            return "ALPHANUM";
        case DT_LONGDATE:            return "LONGDATE";
        case DT_SECONDDATE:          return "SECONDDATE";
        case DT_DAYDATE:             return "DAYDATE";
        case DT_SECONDTIME:          return "SECONDTIME";
        case DT_CLOCATOR:            return "CLOCATOR";
        case DT_BLOB_DISK_RESERVED:  return "BLOB_DISK_RESERVED";
        case DT_CLOB_DISK_RESERVED:  return "CLOB_DISK_RESERVED";
        case DT_NCLOB_DISK_RESERVE:  return "NCLOB_DISK_RESERVE";
        case DT_ST_GEOMETRY:         return "ST_GEOMETRY";
        case DT_ST_POINT:            return "ST_POINT";
        case DT_FIXED16:             return "FIXED16";
        case DT_ABAP_ITAB:           return "ABAP_ITAB";
        case DT_RECORD_ROW_STORE:    return "RECORD_ROW_STORE";
        case DT_RECORD_COLUMN_STORE: return "RECORD_COLUMN_STORE";
        case DT_NOTYPE:              return "NOTYPE";
    }
    return "UNKNOWN";
}

void setColumnInfo(Isolate*             isolate,
                   Local<Object>&       object,
                   dbcapi_column_info*  columnInfo)
/******************************************************************************/
{
    object->Set(String::NewFromUtf8(isolate, "columnName"),
                String::NewFromUtf8(isolate, columnInfo->name));
    object->Set(String::NewFromUtf8(isolate, "originalColumnName"),
                String::NewFromUtf8(isolate, columnInfo->column_name));
    object->Set(String::NewFromUtf8(isolate, "tableName"),
                String::NewFromUtf8(isolate, columnInfo->table_name));
    object->Set(String::NewFromUtf8(isolate, "ownerName"),
                String::NewFromUtf8(isolate, columnInfo->owner_name));
    object->Set(String::NewFromUtf8(isolate, "type"),
                Integer::New(isolate, columnInfo->type));
    object->Set(String::NewFromUtf8(isolate, "typeName"),
                String::NewFromUtf8(isolate, getTypeName(columnInfo->type)));
    object->Set(String::NewFromUtf8(isolate, "nativeType"),
                Integer::New(isolate, columnInfo->native_type));
    object->Set(String::NewFromUtf8(isolate, "nativeTypeName"),
                String::NewFromUtf8(isolate, getNativeTypeName(columnInfo->native_type)));
    object->Set(String::NewFromUtf8(isolate, "precision"),
                Integer::NewFromUnsigned(isolate, columnInfo->precision));
    object->Set(String::NewFromUtf8(isolate, "scale"),
                Integer::NewFromUnsigned(isolate, columnInfo->scale));
    object->Set(String::NewFromUtf8(isolate, "nullable"),
                Integer::NewFromUnsigned(isolate, columnInfo->nullable ? 1 : 0));
}

int fetchColumnInfos(dbcapi_stmt* dbcapi_stmt_ptr,
                     std::vector<dbcapi_column_info*>& column_infos)
/*****************************************/
{
    freeColumnInfos(column_infos);

    int num_cols = api.dbcapi_num_cols(dbcapi_stmt_ptr);
    if (num_cols > 0) {
        for (int i = 0; i < num_cols; i++) {
            dbcapi_column_info* info = new dbcapi_column_info();
            memset(info, 0, sizeof(dbcapi_column_info));
            api.dbcapi_get_column_info(dbcapi_stmt_ptr, i, info);
            info->name = copyString(info->name);
            info->column_name = copyString(info->column_name);
            info->table_name = copyString(info->table_name);
            info->owner_name = copyString(info->owner_name);
            column_infos.push_back(info);
        }
    }

    return num_cols;
}

void freeColumnInfos(std::vector<dbcapi_column_info*>& column_infos)
/*****************************************/
{
    for (size_t i = 0; i < column_infos.size(); i++) {
        dbcapi_column_info* info = column_infos[i];
        if (info != NULL) {
            if (info->name != NULL) {
                delete info->name;
            }
            if (info->column_name != NULL) {
                delete info->column_name;
            }
            if (info->table_name != NULL) {
                delete info->table_name;
            }
            if (info->owner_name != NULL) {
                delete info->owner_name;
            }
            delete info;
        }
    }
    column_infos.clear();
}

char* copyString(const char* src)
/*****************************************/
{
    char* buffer = NULL;
    if (src != NULL) {
        size_t len = strlen(src);
        buffer = new char[len + 1];
        memcpy(buffer, src, len);
        buffer[len] = 0;
    }
    return buffer;
}
