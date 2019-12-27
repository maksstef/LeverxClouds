// ***************************************************************************
// Copyright (c) 2016 SAP SE or an SAP affiliate company. All rights reserved.
// ***************************************************************************
#include "nodever_cover.h"
#include "hana_utils.h"

#if !defined(__APPLE__)
#include <thread>
#endif

#ifndef MIN
#define MIN(x, y)             ((x) <= (y) ? (x) : (y))
#endif

using namespace v8;
using namespace node;

ConnectionLock::ConnectionLock( Statement *stmt ) :
    conn( stmt->connection ),
    lock( conn->conn_mutex )
{}

// Stmt Object Functions
Statement::Statement( Connection *conn )
/**************************************/
    : connection( conn )
{
    dbcapi_stmt_ptr = NULL;
    execBaton = NULL;
    is_dropped = false;
    is_dropping = false;
    batch_size = -1;
    uv_mutex_init(&stmt_mutex);
}

struct freeBaton
{
    dbcapi_stmt     *dbcapi_stmt_ptr;
    ConnectionPointer      conn;
};

void Statement::freeWork(uv_work_t *req)
/******************************************/
{
    freeBaton *baton = static_cast<freeBaton*>(req->data);
    if (baton->conn && baton->dbcapi_stmt_ptr != NULL) {
        ConnectionLock lock(baton->conn);
        if( lock.isValid() ) {
            api.dbcapi_free_stmt(baton->dbcapi_stmt_ptr);
        }
    }
}

void Statement::freeAfter(uv_work_t *req)
/*******************************************/
{
    Isolate *isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);
    freeBaton *baton = static_cast<freeBaton*>(req->data);
    delete baton;
    delete req;
}

#if defined(_WIN32) || defined(_WIN64)
unsigned __stdcall freeStmt(void* args)
#else // defined(__linux__) || defined(__APPLE__)
void* freeStmt(void* args)
#endif
{
    api.dbcapi_free_stmt(reinterpret_cast<dbcapi_stmt*>(args));
    return NULL;
}

Statement::~Statement()
/***********************/
{
    // Destructor should only be run by garbage collection
    // If Statement is being garbage collection, there
    // must be no resultsets, as all resultsets hold a Ref()
    // to the Statement
    assert( resultsets.empty() );

    if (execBaton != NULL) {
        delete execBaton;
        execBaton = NULL;
    }

    clearParameters(params);
    param_infos.clear();

    if( !is_dropped && !is_dropping ) {
        is_dropped = true;
        if( connection ) {
            connection->removeStatement( this );
        }

        if (dbcapi_stmt_ptr != NULL) {
            /*freeBaton *baton = new freeBaton();
            baton->conn = connection;
            baton->dbcapi_stmt_ptr = dbcapi_stmt_ptr;
            dbcapi_stmt_ptr = NULL;
            uv_work_t *req = new uv_work_t();
            req->data = baton;
            int status = uv_queue_work(uv_default_loop(), req, freeWork,
                                       (uv_after_work_cb)freeAfter);
            assert(status == 0);*/
#if defined(_WIN32) || defined(_WIN64)
            _beginthreadex(NULL,
                           0,
                           freeStmt,
                           reinterpret_cast<void *>(dbcapi_stmt_ptr),
                           0, 0);
#else // defined(__linux__) || defined(__APPLE__)
            pthread_t freeStmtThread;
            pthread_create(&freeStmtThread, NULL, freeStmt, reinterpret_cast<void*>(dbcapi_stmt_ptr));
#endif
        }
    }
}

Persistent<Function> Statement::constructor;

void Statement::Init(Isolate *isolate)
/***************************************/
{
    HandleScope	scope(isolate);
    // Prepare constructor template
    Local<FunctionTemplate> tpl = FunctionTemplate::New(isolate, New);
    tpl->SetClassName(String::NewFromUtf8(isolate, "Statement"));
    tpl->InstanceTemplate()->SetInternalFieldCount(1);

    // Prototype
    NODE_SET_PROTOTYPE_METHOD(tpl, "isValid", isValid);
    NODE_SET_PROTOTYPE_METHOD(tpl, "exec", exec);
    NODE_SET_PROTOTYPE_METHOD(tpl, "execute", exec);
    NODE_SET_PROTOTYPE_METHOD(tpl, "execQuery", execQuery);
    NODE_SET_PROTOTYPE_METHOD(tpl, "executeQuery", execQuery);
    NODE_SET_PROTOTYPE_METHOD(tpl, "execBatch", execBatch);
    NODE_SET_PROTOTYPE_METHOD(tpl, "executeBatch", execBatch);
    NODE_SET_PROTOTYPE_METHOD(tpl, "drop", drop);
    NODE_SET_PROTOTYPE_METHOD(tpl, "getParameterInfo", getParameterInfo);
    NODE_SET_PROTOTYPE_METHOD(tpl, "getParameterLength", getParameterLength);
    NODE_SET_PROTOTYPE_METHOD(tpl, "getParameterValue", getParameterValue);
    NODE_SET_PROTOTYPE_METHOD(tpl, "sendParameterData", sendParameterData);
    NODE_SET_PROTOTYPE_METHOD(tpl, "isParameterNull", isParameterNull);
    NODE_SET_PROTOTYPE_METHOD(tpl, "getData", getData);
    NODE_SET_PROTOTYPE_METHOD(tpl, "functionCode", functionCode);
    NODE_SET_PROTOTYPE_METHOD(tpl, "getColumnInfo", getColumnInfo);
    NODE_SET_PROTOTYPE_METHOD(tpl, "getPrintLines", getPrintLines);
    NODE_SET_PROTOTYPE_METHOD(tpl, "getRowStatus", getRowStatus);
    NODE_SET_PROTOTYPE_METHOD(tpl, "getServerCPUTime", getServerCPUTime);
    NODE_SET_PROTOTYPE_METHOD(tpl, "getServerMemoryUsage", getServerMemoryUsage);
    NODE_SET_PROTOTYPE_METHOD(tpl, "getServerProcessingTime", getServerProcessingTime);
    NODE_SET_PROTOTYPE_METHOD(tpl, "setTimeout", setTimeout);

    Local<Context> context = isolate->GetCurrentContext();
    constructor.Reset(isolate, tpl->GetFunction(context).ToLocalChecked());
}

void Statement::New(const FunctionCallbackInfo<Value> &args)
/*************************************************************/
{
    Statement* obj = new Statement();

    obj->Wrap(args.This());
    args.GetReturnValue().Set(args.This());
}

void Statement::NewInstance(const FunctionCallbackInfo<Value> &args)
/*********************************************************************/
{
    Persistent<Object> obj;
    CreateNewInstance(args, obj);
    args.GetReturnValue().Set(obj);
}

void Statement::CreateNewInstance(const FunctionCallbackInfo<Value> &	args,
                                  Persistent<Object> &		obj )
/***************************************************************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope	scope(isolate);
    const unsigned argc = 1;
    Local<Value> argv[argc] = { args[0] };
    Local<Context> context = isolate->GetCurrentContext();
    Local<Function> cons = Local<Function>::New(isolate, constructor);
    Local<Object> instance = cons->NewInstance(context, argc, argv).ToLocalChecked();
    obj.Reset(isolate, instance);
}

bool Statement::checkExecParameters(const FunctionCallbackInfo<Value> &args,
                                    const char *function,
                                    bool &bind_required,
                                    int  &cbfunc_arg)
/*******************************/
{
    int  num_args = args.Length();
    int  invalidArg = -1;
    unsigned int expectedTypes[] = { JS_ARRAY | JS_OBJECT | JS_FUNCTION, JS_FUNCTION };

    cbfunc_arg = -1;
    bind_required = false;

    if (num_args == 1) {
        if (args[0]->IsFunction()) {
            cbfunc_arg = 0;
        } else if (args[0]->IsArray() || args[0]->IsObject()) {
            bind_required = true;
        } else if (!args[0]->IsUndefined() && !args[0]->IsNull()) {
            invalidArg = 0;
        }
    } else if (num_args >= 2) {
        if (args[0]->IsArray() || args[0]->IsObject() || args[0]->IsNull() || args[0]->IsUndefined()) {
            bind_required = args[0]->IsArray() || args[0]->IsObject();
            if (args[1]->IsFunction() || args[1]->IsUndefined() || args[1]->IsNull()) {
                cbfunc_arg = (args[1]->IsFunction()) ? 1 : -1;
            } else {
                invalidArg = 1;
            }
        } else {
            invalidArg = 0;
        }
    }

    if (invalidArg >= 0) {
        throwErrorIP(invalidArg, function,
                     getJSTypeName(expectedTypes[invalidArg]).c_str(),
                     getJSTypeName(getJSType(args[invalidArg])).c_str());
        return false;
    }

    return true;
}

bool Statement::checkExecParameters(bool bind_required,
                                    std::vector<dbcapi_bind_param_info> &param_infos)
/*******************************/
{
    int inputParamCount = 0;
    for (size_t i = 0; i < param_infos.size(); i++) {
        if (param_infos[i].direction == DD_INPUT || param_infos[i].direction == DD_INPUT_OUTPUT) {
            inputParamCount++;
        }
    }
    if (inputParamCount > 0 && !bind_required) {
        std::string sqlState = "HY000";
        std::string errText = "No binding parameter(s)";
        throwError(JS_ERR_INVALID_ARGUMENTS, errText, sqlState);
        return false;
    }

    return true;
}

NODE_API_FUNC(Statement::isValid)
/*****************************************************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope(isolate);

    Statement *stmt = ObjectWrap::Unwrap<Statement>(args.This());
    args.GetReturnValue().Set(Boolean::New(isolate, !stmt->is_dropped && !stmt->is_dropping));
}

NODE_API_FUNC(Statement::exec)
/*******************************/
{
    Isolate *isolate = args.GetIsolate();
    Local<Context> context = isolate->GetCurrentContext();
    HandleScope scope(isolate);

    int  num_args = args.Length();
    int  cbfunc_arg = -1;
    int  options_arg = -1;
    bool invalidArg = false;
    bool bind_required = false;
    executeOptions execOptions;
    Statement *stmt = ObjectWrap::Unwrap<Statement>(args.This());

    args.GetReturnValue().SetUndefined();

    if (num_args == 1) {
        if (args[0]->IsArray()) {
            bind_required = true;
        } else if (args[0]->IsFunction()) {
            cbfunc_arg = 0;
        } else if (args[0]->IsObject()) {
            if (getExecuteOptions(isolate, args[0]->ToObject(context), &execOptions)) {
                options_arg = 0;
            } else {
                bind_required = true;
            }
        } else if (!args[0]->IsNull() && !args[0]->IsUndefined()) {
            invalidArg = true;
        }
    } else if (num_args == 2) {
        if (args[0]->IsArray()) {
            bind_required = true;
            if (args[1]->IsFunction()) {
                cbfunc_arg = 1;
            } else if (args[1]->IsObject()) {
                if (getExecuteOptions(isolate, args[1]->ToObject(context), &execOptions)) {
                    options_arg = 1;
                }  else {
                    invalidArg = true;
                }
            } else if (!args[1]->IsNull() && !args[1]->IsUndefined()) {
                invalidArg = true;
            }
        } else if (args[0]->IsObject()) {
            if (getExecuteOptions(isolate, args[0]->ToObject(context), &execOptions)) {
                options_arg = 0;
            } else {
                bind_required = true;
            }
            if (args[1]->IsFunction()) {
                cbfunc_arg = 1;
            } else if (args[1]->IsObject()) {
                bind_required = true;
                options_arg = 1;
            } else if (!args[1]->IsNull() && !args[1]->IsUndefined()) {
                invalidArg = true;
            }
        } else if (args[0]->IsFunction()) {
            cbfunc_arg = 0;
            if (!args[1]->IsNull() && !args[1]->IsUndefined()) {
                invalidArg = true;
            }
        } else if (args[0]->IsNull() || args[0]->IsUndefined()) {
            if (args[1]->IsFunction()) {
                cbfunc_arg = 1;
            } else if (args[1]->IsObject()) {
                if (getExecuteOptions(isolate, args[1]->ToObject(context), &execOptions)) {
                    options_arg = 0;
                } else {
                    invalidArg = true;
                }
            } else if (!args[1]->IsNull() && !args[1]->IsUndefined()) {
                invalidArg = true;
            }
        }
    } else if (num_args >= 3) {
        if (args[0]->IsArray() || args[0]->IsObject() || args[0]->IsNull() || args[0]->IsUndefined()) {
            bind_required = args[0]->IsArray() || args[0]->IsObject();
            if (args[1]->IsObject() || args[1]->IsNull() || args[1]->IsUndefined()) {
                options_arg = args[1]->IsObject() ? 1 : -1;
                if (args[2]->IsFunction() || args[2]->IsNull() || args[2]->IsUndefined()) {
                    cbfunc_arg = args[2]->IsFunction() ? 2 : -1;
                }
            }
        } else {
            invalidArg = true;
        }
    }

    if ( invalidArg ) {
        std::string sqlState = "HY000";
        std::string errText = "Invalid parameter for function 'exec[ute]([params][, options][, callback])'";
        throwError(JS_ERR_INVALID_ARGUMENTS, errText, sqlState);
        return;
    }

    if (!checkExecParameters(bind_required, stmt->param_infos)) {
        return;
    }

    bool callback_required = (cbfunc_arg >= 0);

    if (args[0]->IsArray()) {
        Local<Array> bind_params = Local<Array>::Cast(args[0]);
        int array_len = bind_params->Length();
        int count = 0;
        for (int i = 0; i < array_len; i++) {
            if (bind_params->Get(i)->IsArray()) {
                count++;
            }
        }
        if (array_len > 0 && count == array_len) {
            stmt->execBatch(args);
            return;
        }
    }

    if (!Statement::checkStatement(stmt, args, cbfunc_arg, callback_required)) {
        return;
    }

    stmt->batch_size = -1;

    executeBaton *baton = new executeBaton();

    baton->conn = stmt->connection;
    baton->stmt = stmt;
    baton->dbcapi_stmt_ptr = stmt->dbcapi_stmt_ptr;
    baton->callback_required = callback_required;

    if (options_arg >= 0) {
        getExecuteOptions(isolate, args[options_arg]->ToObject(context), &baton->exec_options);
    }

    if (bind_required) {
        if (!getInputParameters(isolate, args[0], baton->provided_params, baton->dbcapi_stmt_ptr,
            baton->error_code, baton->error_msg, baton->sql_state)) {
            Local<Value> undef = Local<Value>::New(isolate, Undefined(isolate));
            if (callback_required) {
                callBack(baton->error_code, &baton->error_msg, &baton->sql_state, args[cbfunc_arg], undef, callback_required);
            } else {
                throwError(baton->error_code, baton->error_msg, baton->sql_state);
            }
            return;
        }
    }

    uv_work_t *req = new uv_work_t();
    req->data = baton;

    if (callback_required) {
        Local<Function> callback = Local<Function>::Cast(args[cbfunc_arg]);
        baton->callback.Reset(isolate, callback);

        int status;
        status = uv_queue_work(uv_default_loop(), req, executeWork,
                               (uv_after_work_cb)executeAfter);
        assert(status == 0);

        return;
    }

    Persistent<Value> ResultSet;

    executeWork(req);
    bool success = fillResult(baton, ResultSet);
    delete req;
    // delete baton; deleted in the destructor

    if (!success) {
        return;
    }
    args.GetReturnValue().Set(ResultSet);
    ResultSet.Reset();
}

struct executeBatchBaton
{
    Persistent<Function>		callback;
    bool 				err;
    int                                 error_code;
    std::string 			error_msg;
    std::string                         sql_state;
    bool 				callback_required;

    StatementPointer                    stmt;
    dbcapi_stmt 			*dbcapi_stmt_ptr;

    std::vector<dbcapi_bind_data*> 	params;
    std::vector<size_t> 	        buffer_size;

    int 				rows_affected;
    int                                 batch_size;
    int                                 row_param_count;

    executeBatchBaton()
    {
        err = false;
        callback_required = false;
        dbcapi_stmt_ptr = NULL;
        batch_size = -1;
        rows_affected = -1;
        row_param_count = -1;
    }

    ~executeBatchBaton()
    {
        // the Statement will free dbcapi_stmt_ptr
        dbcapi_stmt_ptr = NULL;
        callback.Reset();
        clearParameters(params);
    }
};

NODE_API_FUNC(Statement::execBatch)
/*******************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope(isolate);
    int cbfunc_arg = -1;
    bool bind_required = false;
    const char *fun = "exec[ute]Batch([params][, callback])";
    Statement *stmt = ObjectWrap::Unwrap<Statement>(args.This());

    args.GetReturnValue().SetUndefined();

    if (!checkExecParameters(args, fun, bind_required, cbfunc_arg) ||
        !checkExecParameters(bind_required, stmt->param_infos)) {
        return;
    }

    int row_param_count = -1;
    bool invalid_arguments = false;
    Local<Array> bind_params = Local<Array>::Cast(args[0]);
    int batch_size = bind_params->Length();

    if (batch_size < 1) {
        invalid_arguments = true;
    } else {
        for (int i = 0; i < batch_size; i++) {
            if (!bind_params->Get(i)->IsArray()) {
                invalid_arguments = true;
                break;
            } else {
                Local<Array> values = Local<Array>::Cast(bind_params->Get(i));
                int length = values->Length();
                if (length < 1 || (row_param_count != -1 && length != row_param_count)) {
                    invalid_arguments = true;
                    break;
                }  else {
                    row_param_count = length;
                }
            }
        }
    }

    if (invalid_arguments) {
        char buffer[256];
        memset(buffer, 0, sizeof(buffer));
        sprintf(buffer, "Invalid parameter 1 for function '%s': expected an array of arrays with same length.", fun);
        std::string errText = buffer;
        std::string sqlState = "HY000";
        throwError(JS_ERR_INVALID_ARGUMENTS, errText, sqlState);
        return;
    }

    // For batch execution, the values of every parameter have to be the same type.
    int param_with_mixed_type = -1;
    std::vector<unsigned int> param_types;
    for (int i = 0; i < batch_size; i++) {
        Local<Array> values = Local<Array>::Cast(bind_params->Get(i));
        int length = values->Length();
        if (i == 0) {
            for (int j = 0; j < length; j++) {
                param_types.push_back(getJSType(values->Get(j)));
            }
        } else {
            for (int j = 0; j < length; j++) {
                unsigned int param_type = getJSType(values->Get(j));
                if (param_type != JS_UNDEFINED && param_type != JS_NULL) {
                    if (param_types[j] == JS_UNDEFINED || param_types[j] == JS_NULL) {
                        param_types[j] = param_type;
                    } else if ((param_types[j] == JS_NUMBER && param_type == JS_INTEGER) ||
                               (param_types[j] == JS_INTEGER && param_type == JS_NUMBER)) {
                        param_types[j] = JS_NUMBER; // integers converted to numbers
                    } else if (param_types[j] != param_type) {
                        param_with_mixed_type = j;
                        break;
                    }
                }
            }
        }
    }
    if (param_with_mixed_type >= 0) {
        std::ostringstream str_strm;
        str_strm << "Invalid parameter: 'parameter (";
        str_strm << param_with_mixed_type;
        str_strm << ")' contains different types.";
        std::string errText = str_strm.str();
        std::string sqlState = "HY000";
        throwError(JS_ERR_INVALID_ARGUMENTS, errText, sqlState);
        return;
    }

    bool callback_required = (cbfunc_arg >= 0);
    if (!Statement::checkStatement(stmt, args, cbfunc_arg, callback_required)) {
        return;
    }

    executeBatchBaton *baton = new executeBatchBaton();

    baton->stmt = stmt;
    baton->dbcapi_stmt_ptr = stmt->dbcapi_stmt_ptr;
    baton->callback_required = callback_required;
    baton->stmt->batch_size = batch_size;
    baton->batch_size = batch_size;
    baton->row_param_count = row_param_count;

    if (!getBindParameters(isolate, args[0], row_param_count, baton->params, baton->buffer_size, param_types)) {
        int error_code;
        std::string error_msg;
        std::string sql_state;
        Local<Value> undef = Local<Value>::New(isolate, Undefined(isolate));
        getErrorMsg(JS_ERR_BINDING_PARAMETERS, error_code, error_msg, sql_state);
        if (callback_required) {
            callBack(error_code, &error_msg, &sql_state, args[cbfunc_arg], undef, callback_required);
        } else {
            throwError(baton->error_code, baton->error_msg, baton->sql_state);
        }
        delete baton;
        return;
    }

    uv_work_t *req = new uv_work_t();
    req->data = baton;

    if (callback_required) {
        Local<Function> callback = Local<Function>::Cast(args[cbfunc_arg]);
        baton->callback.Reset(isolate, callback);

        int status;
        status = uv_queue_work(uv_default_loop(), req, executeBatchWork,
                               (uv_after_work_cb)executeBatchAfter);
        assert(status == 0);
        return;
    }

    executeBatchWork(req);

    int rows_affected = baton->rows_affected;
    bool err = baton->err;

    executeBatchAfter(req);

    if (err) {
        return;
    }

    args.GetReturnValue().Set(Integer::New(isolate, rows_affected));
}

void Statement::executeBatchAfter(uv_work_t *req)
/*********************************/
{
    Isolate *isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);
    executeBatchBaton *baton = static_cast<executeBatchBaton*>(req->data);
    Local<Value> undef = Local<Value>::New(isolate, Undefined(isolate));

    if (baton->err) {
        callBack(baton->error_code, &(baton->error_msg), &(baton->sql_state),
                 baton->callback, undef, baton->callback_required);
        delete baton;
        delete req;
        return;
    }

    if (baton->callback_required) {
        Persistent<Value> rows_affected;
        rows_affected.Reset(isolate, Integer::New(isolate, baton->rows_affected));
        callBack(0, NULL, NULL, baton->callback, rows_affected, baton->callback_required);
        rows_affected.Reset();
    }

    delete baton;
    delete req;
}

void Statement::executeBatchWork(uv_work_t *req)
/********************************/
{
    executeBatchBaton *baton = static_cast<executeBatchBaton*>(req->data);
    ConnectionLock lock(baton->stmt);

    if( !lock.isValid() ) {
        baton->err = true;
        getErrorMsg(JS_ERR_NOT_CONNECTED, baton->error_code, baton->error_msg, baton->sql_state);
        return;
    }
    
    if (baton->dbcapi_stmt_ptr == NULL && baton->stmt->sql.length() > 0) {
        baton->dbcapi_stmt_ptr = api.dbcapi_prepare(baton->stmt->connection->dbcapi_conn_ptr,
                                                    baton->stmt->sql.c_str());
        if (baton->dbcapi_stmt_ptr == NULL) {
            baton->err = true;
            getErrorMsg(baton->stmt->connection->dbcapi_conn_ptr, baton->error_code, baton->error_msg, baton->sql_state);
            return;
        }
    }
    else if (baton->dbcapi_stmt_ptr == NULL) {
        baton->err = true;
        getErrorMsg(JS_ERR_INVALID_OBJECT, baton->error_code, baton->error_msg, baton->sql_state);
        return;
    }

    if (!api.dbcapi_reset(baton->dbcapi_stmt_ptr)) {
        baton->err = true;
        getErrorMsg(baton->stmt->connection->dbcapi_conn_ptr, baton->error_code, baton->error_msg, baton->sql_state);
        return;
    }

    dbcapi_bind_data param;
    std::vector<dbcapi_bind_data*> params;
    void* bufferSrc;

    for (int i = 0; i < baton->row_param_count; i++) {
        memset(&param, 0, sizeof(dbcapi_bind_data));

        if (!api.dbcapi_describe_bind_param(baton->dbcapi_stmt_ptr, i, &param)) {
            baton->err = true;
            getErrorMsg(baton->stmt->connection->dbcapi_conn_ptr, baton->error_code, baton->error_msg, baton->sql_state);
            return;
        }

        dbcapi_bind_data* paramNew = new dbcapi_bind_data();
        memset(paramNew, 0, sizeof(dbcapi_bind_data));
        params.push_back(paramNew);

        params[i]->value.buffer_size = baton->buffer_size[i];
        params[i]->value.type = baton->params[i]->value.type;
        params[i]->direction = param.direction;

        params[i]->value.buffer = new char[baton->batch_size * baton->buffer_size[i]];
        for (int j = 0; j < baton->batch_size; j++) {
            bufferSrc = baton->params[baton->row_param_count * j + i]->value.buffer;
            if (bufferSrc != NULL) {
                size_t copy_len = MIN(*baton->params[baton->row_param_count * j + i]->value.length, baton->buffer_size[i]);
                memcpy(params[i]->value.buffer + baton->buffer_size[i] * j, bufferSrc, copy_len);
            }
        }

        params[i]->value.length = new size_t[baton->batch_size * sizeof(size_t)];
        for (int j = 0; j < baton->batch_size; j++) {
            params[i]->value.length[j] = *baton->params[baton->row_param_count * j + i]->value.length;
        }

        params[i]->value.is_null = new dbcapi_bool[baton->batch_size * sizeof(dbcapi_bool)];
        for (int j = 0; j < baton->batch_size; j++) {
            params[i]->value.is_null[j] = *baton->params[baton->row_param_count * j + i]->value.is_null;
        }

        if (!api.dbcapi_bind_param(baton->dbcapi_stmt_ptr, i, params[i])) {
            baton->err = true;
            getErrorMsg(baton->stmt->connection->dbcapi_conn_ptr, baton->error_code, baton->error_msg, baton->sql_state);
            clearParameters(params);
            return;
        }
    }

    dbcapi_bool success_execute = api.dbcapi_set_batch_size(baton->dbcapi_stmt_ptr, baton->batch_size);
    if (!success_execute) {
        baton->err = true;
        getErrorMsg(baton->stmt->connection->dbcapi_conn_ptr, baton->error_code, baton->error_msg, baton->sql_state);
        clearParameters(params);
        return;
    }

    success_execute = api.dbcapi_execute(baton->dbcapi_stmt_ptr);
    if (!success_execute) {
        baton->err = true;
        getErrorMsg(baton->stmt->connection->dbcapi_conn_ptr, baton->error_code, baton->error_msg, baton->sql_state);
        clearParameters(params);
        return;
    }

    baton->rows_affected = api.dbcapi_affected_rows(baton->dbcapi_stmt_ptr);
    clearParameters(params);
}

bool Statement::checkStatement(Statement *stmt,
                               const FunctionCallbackInfo<Value> &args,
                               int cbfunc_arg,
                               bool callback_required)
/*******************************/
{
    Isolate *isolate = args.GetIsolate();
    Local<Value> undef = Local<Value>::New(isolate, Undefined(isolate));

    if (stmt == NULL || stmt->dbcapi_stmt_ptr == NULL || stmt->is_dropped || stmt->is_dropping) {
        int error_code;
        std::string error_msg;
        std::string sql_state;
        getErrorMsg(JS_ERR_STATEMENT_DROPPED, error_code, error_msg, sql_state);
        if (callback_required) {
            callBack(error_code, &error_msg, &sql_state, args[cbfunc_arg], undef, callback_required);
        } else {
            throwError(error_code, error_msg, sql_state);
        }
        args.GetReturnValue().SetUndefined();
        return false;
    }
    return true;
}

struct executeQueryBaton {
    Persistent<Function>		callback;
    bool 				err;
    int                                 error_code;
    std::string 			error_msg;
    std::string                         sql_state;
    bool 				callback_required;

    StatementPointer                    stmt;
    dbcapi_stmt                         *dbcapi_stmt_ptr_rs;

    std::vector<dbcapi_bind_data*> 	params;
    std::vector<dbcapi_bind_data*> 	provided_params;

    Persistent<Value> 		        resultSetObj;

    executeQueryBaton()
    {
        err = false;
        callback_required = false;
    }

    ~executeQueryBaton()
    {
        //dbcapi_stmt_ptr will be freed by ResultSet
        callback.Reset();
        resultSetObj.Reset();
        clearParameters(params);
        clearParameters(provided_params);
    }
};

// NOT THREAD-SAFE; DO NOT CALL FROM WORKER THREAD OR WHILE DROPPING
void Statement::addResultSet( ResultSet *rs )
/*******************************************/
{
    assert(!is_dropping);

    rs->stmt = this;
    resultsets.push_back( rs );
}

// NOT THREAD-SAFE; DO NOT CALL FROM WORKER THREAD
void Statement::removeResultSet( ResultSet *rs )
/**********************************************/
{
    // if dropping, don't modify the resultsets list; dropWork will take care of clearing the list
    if( !is_dropping ) {
        resultsets.remove( rs );
        rs->stmt = NULL;
    }
}

void Statement::executeQueryAfter(uv_work_t *req)
/******************************************************/
{
    Isolate *isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);
    executeQueryBaton *baton = static_cast<executeQueryBaton*>(req->data);
    Local<Value> undef = Local<Value>::New(isolate, Undefined(isolate));

    if (!baton->err && baton->stmt->is_dropping ) {
        // if the statement was dropped after it finished executing
        // we should not continue to create the ResultSet; throw an error instead

        /* ResultSet uses Statement's dbcapi_stmt_ptr which is freed by the Statement.
        // we allocated a new dbcapi_stmt_ptr that we need to destroy as well
        // do that on a worker thread so that we don't run into issues with the mutex
        if (baton->dbcapi_stmt_ptr_rs != NULL) {
            freeBaton *freebaton = new freeBaton();
            freebaton->conn = baton->stmt->connection;
            freebaton->dbcapi_stmt_ptr = baton->dbcapi_stmt_ptr_rs;
            baton->dbcapi_stmt_ptr_rs = NULL;
            uv_work_t *freereq = new uv_work_t();
            freereq->data = freebaton;
            int status = uv_queue_work(uv_default_loop(), freereq, freeWork,
                                       (uv_after_work_cb)freeAfter);
            assert(status == 0);
        }*/

        baton->err = true;
        getErrorMsg(JS_ERR_INVALID_OBJECT, baton->error_code, baton->error_msg, baton->sql_state);
    }

    if (baton->err) {
        // Error Message is already set in the executeQueryWork() function
        callBack(baton->error_code, &(baton->error_msg), &(baton->sql_state),
                 baton->callback, undef, baton->callback_required);
        delete baton;
        delete req;
        return;
    }

    if (baton->callback_required) {
        Local<Value> resultSetObj = Local<Value>::New(isolate,
                                                      baton->resultSetObj);
        baton->resultSetObj.Reset();
        ResultSet *resultset = node::ObjectWrap::Unwrap<ResultSet>(resultSetObj->ToObject(isolate));
        resultset->dbcapi_stmt_ptr = baton->dbcapi_stmt_ptr_rs;
        baton->stmt->addResultSet( resultset );

        resultset->getColumnInfos();

        callBack(0, NULL, NULL, baton->callback, resultSetObj, baton->callback_required);
    }

    delete baton;
    delete req;
}

void Statement::executeQueryWork(uv_work_t *req)
/*****************************************************/
{
    executeQueryBaton *baton = static_cast<executeQueryBaton*>(req->data);
    ConnectionLock lock(baton->stmt);

    if( !lock.isValid() ) {
        baton->err = true;
        getErrorMsg(JS_ERR_NOT_CONNECTED, baton->error_code, baton->error_msg, baton->sql_state);
        return;
    }

    bool sendParamData = false;
    if (!bindParameters(baton->stmt->connection->dbcapi_conn_ptr, baton->dbcapi_stmt_ptr_rs, baton->params,
        baton->error_code, baton->error_msg, baton->sql_state, sendParamData)) {
        baton->err = true;
        return;
    }

    dbcapi_bool success_execute = api.dbcapi_execute(baton->dbcapi_stmt_ptr_rs);

    if (success_execute) {
        copyParameters(baton->stmt->params, baton->params);
    } else {
        baton->err = true;
        getErrorMsg(baton->stmt->connection->dbcapi_conn_ptr, baton->error_code, baton->error_msg, baton->sql_state);
    }
}

NODE_API_FUNC(Statement::execQuery)
/*******************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope(isolate);
    int  cbfunc_arg = -1;
    bool bind_required = false;
    Statement *stmt = ObjectWrap::Unwrap<Statement>(args.This());

    args.GetReturnValue().SetUndefined();

    if (!checkExecParameters(args, "exec[ute]Query([params][, callback])", bind_required, cbfunc_arg) ||
        !checkExecParameters(bind_required, stmt->param_infos)) {
        return;
    }

    bool callback_required = (cbfunc_arg >= 0);

    if (!Statement::checkStatement(stmt, args, cbfunc_arg, callback_required)) {
        return;
    }

    if (stmt->resultsets.size() > 0) {
        std::string sqlState = "HY000";
        std::string errText = "There is already an open ResultSet associated with this Statement which must be closed first.";
        throwError(JS_ERR_RS_MUST_BE_CLOSED, errText, sqlState);
        return;
    }

    stmt->batch_size = -1;

    executeQueryBaton *baton = new executeQueryBaton();

    baton->stmt = stmt;
    baton->dbcapi_stmt_ptr_rs = stmt->dbcapi_stmt_ptr;
    baton->callback_required = callback_required;

    Local<Value> undef = Local<Value>::New(isolate, Undefined(isolate));

    if (bind_required) {
        if (!getInputParameters(isolate, args[0], baton->provided_params, baton->stmt->dbcapi_stmt_ptr, baton->error_code, baton->error_msg, baton->sql_state) ||
            !checkParameterCount(baton->error_code, baton->error_msg, baton->sql_state, baton->provided_params, baton->stmt->dbcapi_stmt_ptr)) {
            if (callback_required) {
                callBack(baton->error_code, &baton->error_msg, &baton->sql_state, args[cbfunc_arg], undef, callback_required);
            } else {
                throwError(baton->error_code, baton->error_msg, baton->sql_state);
            }
            delete baton;
            return;
        }
    }

    int invalidParam = getBindParameters(baton->provided_params, baton->params, baton->stmt->dbcapi_stmt_ptr);
    if (invalidParam >= 0) {
        getErrorMsgInvalidParam(baton->error_code, baton->error_msg, baton->sql_state, invalidParam);
        if (callback_required) {
            callBack(baton->error_code, &baton->error_msg, &baton->sql_state, args[cbfunc_arg], undef, callback_required);
        } else {
            throwError(baton->error_code, baton->error_msg, baton->sql_state);
        }
        delete baton;
        return;
    }

    Persistent<Object> resultSetObj;
    ResultSet::CreateNewInstance(args, resultSetObj);

    uv_work_t *req = new uv_work_t();
    req->data = baton;

    if (callback_required) {
        baton->resultSetObj.Reset(isolate, resultSetObj);
        Local<Function> callback = Local<Function>::Cast(args[cbfunc_arg]);
        baton->callback.Reset(isolate, callback);
        int status = uv_queue_work(uv_default_loop(), req, executeQueryWork,
                                   (uv_after_work_cb)executeQueryAfter);
        assert(status == 0);
        _unused(status);
        resultSetObj.Reset();
        return;
    }

    Local<Object>lres = Local<Object>::New(isolate, resultSetObj);
    ResultSet *resultset = node::ObjectWrap::Unwrap<ResultSet>(lres);
    stmt->addResultSet( resultset );

    executeQueryWork(req);
    resultset->dbcapi_stmt_ptr = baton->dbcapi_stmt_ptr_rs;

    bool err = baton->err;
    executeQueryAfter(req);

    if (err) {
        return;
    }

    resultset->getColumnInfos();
    args.GetReturnValue().Set(resultSetObj);
    resultSetObj.Reset();  
}

struct dropBaton
{
    Persistent<Function> 	callback;
    bool 			err;
    int                         error_code;
    std::string                 error_msg;
    std::string                 sql_state;
    bool 			callback_required;

    StatementPointer 		stmt;

    dropBaton()
    {
        err = false;
        callback_required = false;
    }

    ~dropBaton()
    {
        callback.Reset();
    }
};

void Statement::dropAfter(uv_work_t *req)
/*******************************************/
{
    Isolate *isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);
    dropBaton *baton = static_cast<dropBaton*>(req->data);
    Local<Value> undef = Local<Value>::New(isolate, Undefined(isolate));

    if( baton->stmt->connection ) {
        baton->stmt->connection->removeStatement( baton->stmt );
    }

    if (baton->err) {
        callBack(baton->error_code, &(baton->error_msg), &(baton->sql_state),
                 baton->callback, undef, baton->callback_required);
        delete baton;
        delete req;
        return;
    }

    callBack(0, NULL, NULL, baton->callback, undef, baton->callback_required);

    delete baton;
    delete req;
}

void Statement::dropWork(uv_work_t *req)
/******************************************/
{
    dropBaton *baton = static_cast<dropBaton*>(req->data);
    ConnectionLock lock(baton->stmt);

    if( !lock.isValid() ) {
        baton->err = true;
        getErrorMsg(JS_ERR_NOT_CONNECTED, baton->error_code, baton->error_msg, baton->sql_state);
        return;
    }

    if (!baton->stmt->is_dropping) {
        baton->err = true;
        getErrorMsg(JS_ERR_INVALID_OBJECT, baton->error_code, baton->error_msg, baton->sql_state);
        return;
    }

    baton->stmt->_drop();
}

void Statement::_setDropping()
/****************************/
{
    if( is_dropping ) {
        return;
    }

    is_dropping = true;

    // to ensure garbage collection doesn't clean this up, add a ref to this
    Ref();

    for( std::list<ResultSet*>::iterator it = resultsets.begin(); it != resultsets.end(); ++it ) {
        (*it)->_setClosing();
    }
}

void Statement::_drop()
/*********************/
{
    assert(is_dropping);
    if (is_dropped) {
        return;
    }

    // we are now dropping the statement
    // it is safe to traverse resultsets list
    for( std::list<ResultSet*>::iterator it = resultsets.begin(); it != resultsets.end(); ++it ) {
        (*it)->_close();
    }
    resultsets.clear();
    
    is_dropped = true;

    if (dbcapi_stmt_ptr) {
        api.dbcapi_free_stmt(dbcapi_stmt_ptr);
        dbcapi_stmt_ptr = NULL;
    }

    Unref();
}

NODE_API_FUNC(Statement::drop)
/*******************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope(isolate);
    int cbfunc_arg = -1;

    args.GetReturnValue().SetUndefined();

    // check parameters
    unsigned int expectedTypes[] = { JS_FUNCTION };
    bool isOptional[] = { true };
    if (!checkParameters(isolate, args, "drop([callback])", 1, expectedTypes, &cbfunc_arg, isOptional)) {
        return;
    }
    bool callback_required = (cbfunc_arg >= 0);

    Statement *stmt = ObjectWrap::Unwrap<Statement>(args.This());

    if (stmt->is_dropped) {
        if (callback_required) {
            Local<Value> undef = Local<Value>::New(isolate, Undefined(isolate));
            Local<Function> callback = Local<Function>::Cast(args[cbfunc_arg]);
            callBack(0, NULL, NULL, callback, undef, true);
        }
        return;
    }

    stmt->_setDropping();

    dropBaton *baton = new dropBaton();
    baton->stmt = stmt;
    baton->callback_required = callback_required;

    uv_work_t *req = new uv_work_t();
    req->data = baton;

    if (callback_required) {
        Local<Function> callback = Local<Function>::Cast(args[cbfunc_arg]);
        baton->callback.Reset(isolate, callback);

        int status;
        status = uv_queue_work(uv_default_loop(), req, dropWork,
                               (uv_after_work_cb)dropAfter);
        assert(status == 0);
        return;
    }

    dropWork(req);
    dropAfter(req);

    return;
}

NODE_API_FUNC(Statement::getParameterInfo)
/*******************************************************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope(isolate);
    Statement *stmt = ObjectWrap::Unwrap<Statement>(args.This());
    ConnectionLock lock(stmt);

    if( !lock.isValid() ) {
        throwError(JS_ERR_NOT_CONNECTED);
        return;
    }

    int num_params = api.dbcapi_num_params(stmt->dbcapi_stmt_ptr);
    Local<Array> paramInfos = Array::New(isolate);

    dbcapi_bind_data data;
    dbcapi_bind_param_info info;

    args.GetReturnValue().SetUndefined();

    for (int i = 0; i < num_params; i++) {
        if (api.dbcapi_get_bind_param_info(stmt->dbcapi_stmt_ptr, i, &info) &&
            api.dbcapi_describe_bind_param(stmt->dbcapi_stmt_ptr, i, &data)) {
            Local<Object> paramInfo = Object::New(isolate);
            paramInfo->Set(String::NewFromUtf8(isolate, "name"),
                           String::NewFromUtf8(isolate, info.name));
            paramInfo->Set(String::NewFromUtf8(isolate, "direction"),
                           Integer::New(isolate, info.direction));
            paramInfo->Set(String::NewFromUtf8(isolate, "nativeType"),
                           Integer::New(isolate, info.native_type));
            paramInfo->Set(String::NewFromUtf8(isolate, "nativeTypeName"),
                           String::NewFromUtf8(isolate, getNativeTypeName(info.native_type)));
            paramInfo->Set(String::NewFromUtf8(isolate, "precision"),
                           Integer::NewFromUnsigned(isolate, info.precision));
            paramInfo->Set(String::NewFromUtf8(isolate, "scale"),
                           Integer::NewFromUnsigned(isolate, info.scale));
            paramInfo->Set(String::NewFromUtf8(isolate, "maxSize"),
                           Integer::New(isolate, (int)info.max_size));
            paramInfo->Set(String::NewFromUtf8(isolate, "type"),
                           Integer::New(isolate, data.value.type));
            paramInfo->Set(String::NewFromUtf8(isolate, "typeName"),
                           String::NewFromUtf8(isolate, getTypeName(data.value.type)));
            paramInfos->Set(i, paramInfo);
        } else {
            throwError(stmt->connection->dbcapi_conn_ptr);
        }
    }

    args.GetReturnValue().Set(paramInfos);
}

bool Statement::checkParameterIndex(Isolate *isolate,
                                    Statement *stmt,
                                    const FunctionCallbackInfo<Value> &args,
                                    int &paramIndex)
/*******************************/
{
    Local<Context> context = isolate->GetCurrentContext();
    std::string errText;
    bool	invalid_index = true;

    if (args[0]->IsInt32()) {
        paramIndex = (args[0]->Int32Value(context)).FromMaybe(-1);
        if (paramIndex >= 0 && (size_t)paramIndex < stmt->params.size()) {
            // parameter index is out of range
            return true;
        } else if (paramIndex >= 0 && (size_t)paramIndex < stmt->param_infos.size() && stmt->params.size() == 0) {
            // the statement has been prepared, but not executed yet
            invalid_index = false;
        }
    }
    std::string sqlState = "HY000";
    if (invalid_index) {
        errText.assign("Invalid parameter index.");
    } else {
        errText.assign("The statement has not been executed yet.");
    }
    throwError(JS_ERR_INVALID_INDEX, errText, sqlState);

    return false;
}

struct getParameterValueBaton
{
    Persistent<Function>		callback;
    bool 				callback_required;

    bool 				err;
    int                                 error_code;
    std::string 			error_msg;
    std::string                         sql_state;

    StatementPointer                    stmt;
    ResultSetPointer                    rs;

    int                                 param_index;

    dbcapi_bool                         succeeded;

    getParameterValueBaton()
    {
        err = false;
        callback_required = false;
        succeeded = true;
    }

    ~getParameterValueBaton()
    {
        callback.Reset();
    }
};

void Statement::getParameterValueAfter(uv_work_t *req)
/*******************************************/
{
    Isolate *isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);
    getParameterValueBaton *baton = static_cast<getParameterValueBaton*>(req->data);
    Local<Value> undef = Local<Value>::New(isolate, Undefined(isolate));

    if (baton->err) {
        callBack(baton->error_code, &(baton->error_msg), &(baton->sql_state),
                 baton->callback, undef, baton->callback_required);
    } else {
        dbcapi_data_value & value = baton->stmt->params[baton->param_index]->value;
        dbcapi_native_type nativeType = baton->stmt->param_infos[baton->param_index].native_type;

        Local<Value> paramVal;
        int retCode = getReturnValue(isolate, value, nativeType, paramVal);

        if (retCode == 0) {
            callBack(0, NULL, NULL, baton->callback, paramVal, baton->callback_required);
        } else {
            getErrorMsg(retCode, baton->error_code, baton->error_msg, baton->sql_state);
            callBack(baton->error_code, &(baton->error_msg), &(baton->sql_state),
                     baton->callback, undef, baton->callback_required);
        }
    }

    delete baton;
    delete req;
}

void Statement::getParameterValueWork(uv_work_t *req)
/******************************************/
{
    getParameterValueBaton *baton = static_cast<getParameterValueBaton*>(req->data);
    ConnectionLock lock(baton->stmt);

    if( !lock.isValid() ) {
        baton->err = true;
        getErrorMsg(JS_ERR_NOT_CONNECTED, baton->error_code, baton->error_msg, baton->sql_state);
        return;
    }

    dbcapi_data_value & value = baton->stmt->params[baton->param_index]->value;
    dbcapi_native_type nativeType = baton->stmt->param_infos[baton->param_index].native_type;

    if ((nativeType == DT_BLOB || nativeType == DT_CLOB || nativeType == DT_NCLOB) &&
        (value.buffer == NULL) && (*value.length > 0)) { // get LOB value
        size_t bufLen = *value.length;
        // The length returned from DBCAPI/SQLDBC is the number of characters.
        if (nativeType == DT_CLOB) {
            bufLen++;
        } else if (nativeType == DT_NCLOB) {
            // UTF8 code points use one to four bytes.
            bufLen = bufLen * 4 + 1;
        }
        value.buffer_size = bufLen;
        value.buffer = new char[bufLen];
        dbcapi_stmt *dbcapi_stmt_ptr = baton->rs ? baton->rs->dbcapi_stmt_ptr : baton->stmt->dbcapi_stmt_ptr;
        dbcapi_i32 data_length = api.dbcapi_get_param_data(dbcapi_stmt_ptr, baton->param_index, 0, value.buffer, bufLen);
        if(data_length == -1) {
            baton->succeeded = false;
            *value.length = 0;
        } else {
            baton->succeeded = true;
            *value.length = (size_t)data_length;
        }
    }

    if (!baton->succeeded) {
        baton->err = true;
        getErrorMsg(baton->stmt->connection->dbcapi_conn_ptr, baton->error_code, baton->error_msg, baton->sql_state);
    }
}

NODE_API_FUNC(Statement::getParameterValue)
/*******************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope(isolate);
    int cbfunc_arg = -1;

    args.GetReturnValue().SetUndefined();

    // check parameters
    unsigned int expectedTypes[] = { JS_INTEGER, JS_FUNCTION };
    bool isOptional[] = { false, true };
    if (!checkParameters(isolate, args, "getParameterValue(paramIndex[, callback])",
        2, expectedTypes, &cbfunc_arg, isOptional)) {
        return;
    }
    bool callback_required = (cbfunc_arg >= 0);

    Statement *stmt = ObjectWrap::Unwrap<Statement>(args.This());

    int index;
    if (!checkParameterIndex(isolate, stmt, args, index)) {
        return;
    }

    if (!Statement::checkStatement(stmt, args, cbfunc_arg, callback_required)) {
        return;
    }

    getParameterValueBaton *baton = new getParameterValueBaton();

    baton->stmt = stmt;

    if( !stmt->resultsets.empty() ) {
        // use the last-executed resultset if there is one
        baton->rs = stmt->resultsets.back();
    }
    
    baton->callback_required = callback_required;
    baton->param_index = index;

    uv_work_t *req = new uv_work_t();
    req->data = baton;

    if (callback_required) {
        Local<Function> callback = Local<Function>::Cast(args[cbfunc_arg]);
        baton->callback.Reset(isolate, callback);

        int status;
        status = uv_queue_work(uv_default_loop(), req, getParameterValueWork,
            (uv_after_work_cb)getParameterValueAfter);
        assert(status == 0);

        return;
    }

    getParameterValueWork(req);

    bool err = baton->err;

    delete baton;
    delete req;

    if (!err) {
        dbcapi_data_value & value = stmt->params[index]->value;
        dbcapi_native_type nativeType = stmt->param_infos[index].native_type;
        Local<Value> paramVal;
        int retCode = getReturnValue(isolate, value, nativeType, paramVal);
        if (retCode == 0) {
            args.GetReturnValue().Set(paramVal);
        } else {
            int errorCode;
            std::string errorMsg;
            std::string sqlState;
            getErrorMsg(retCode, errorCode, errorMsg, sqlState);
            throwError(errorCode, errorMsg, sqlState);
        }
    }
}

struct sendParameterDataBaton
{
    Persistent<Function>		callback;
    bool 				callback_required;

    bool 				err;
    int                                 error_code;
    std::string 			error_msg;
    std::string                         sql_state;

    StatementPointer                    stmt;
    dbcapi_stmt 			*dbcapi_stmt_ptr;

    int                                 param_index;
    size_t                              data_length;
    char*                               data;
    int                                 send_data_chunk;

    dbcapi_bool                         succeeded;

    sendParameterDataBaton()
    {
        err = false;
        callback_required = false;
        dbcapi_stmt_ptr = NULL;
        succeeded = false;
    }

    ~sendParameterDataBaton()
    {
        dbcapi_stmt_ptr = NULL;
        callback.Reset();
    }
};

void Statement::sendParameterDataAfter(uv_work_t *req)
/*******************************************/
{
    Isolate *isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);
    sendParameterDataBaton *baton = static_cast<sendParameterDataBaton*>(req->data);
    Local<Value> undef = Local<Value>::New(isolate, Undefined(isolate));

    if (baton->err) {
        callBack(baton->error_code, &(baton->error_msg), &(baton->sql_state),
                 baton->callback, undef, baton->callback_required);
        delete baton;
        delete req;
        return;
    }

    callBack(0, NULL, NULL, baton->callback, undef, baton->callback_required);

    delete baton;
    delete req;
}

void Statement::sendParameterDataWork(uv_work_t *req)
/******************************************/
{
    bool done = false;
    sendParameterDataBaton* baton = static_cast<sendParameterDataBaton*>(req->data);

    do {
        uv_mutex_lock(&(baton->stmt->stmt_mutex));

        if (baton->stmt->send_data_current_chunk == baton->send_data_chunk) {
            ConnectionLock lock(baton->stmt);
            if (lock.isValid()) {
                if (baton->data == NULL) {
                    baton->succeeded = true;
                    if (!baton->stmt->send_param_data_started[baton->param_index]) {
                        baton->succeeded = api.dbcapi_send_param_data(baton->dbcapi_stmt_ptr, baton->param_index, NULL, 0);
                    }
                    if (baton->succeeded) {
                        baton->succeeded = api.dbcapi_finish_param_data(baton->dbcapi_stmt_ptr, baton->param_index);
                        baton->stmt->send_param_data_cols_finished++;
                        if (baton->stmt->send_param_data_cols_finished == baton->stmt->send_param_data_cols) {
                            // Finished sending data.
                        }
                    }
                } else {
                    baton->stmt->send_param_data_started[baton->param_index] = true;
                    baton->succeeded = api.dbcapi_send_param_data(baton->dbcapi_stmt_ptr, baton->param_index, baton->data, baton->data_length);
                }

                baton->stmt->send_data_current_chunk++;

                if (baton->succeeded) {
                    done = true;
                } else {
                    baton->err = true;
                    getErrorMsg(baton->stmt->connection->dbcapi_conn_ptr, baton->error_code, baton->error_msg, baton->sql_state);
                }
            } else {
                baton->err = true;
                getErrorMsg(JS_ERR_NOT_CONNECTED, baton->error_code, baton->error_msg, baton->sql_state);
            }
        }

        uv_mutex_unlock(&(baton->stmt->stmt_mutex));

        if (baton->err) {
            return;
        } else if (!done) {
#if !defined(__APPLE__)
            std::this_thread::yield();
#endif
        }
    } while (!done);
}

NODE_API_FUNC(Statement::sendParameterData)
/*******************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope(isolate);
    int cbfunc_arg = -1;

    args.GetReturnValue().SetUndefined();

    // check parameters
    unsigned int expectedTypes[] = { JS_INTEGER, JS_BUFFER | JS_NULL | JS_UNDEFINED, JS_FUNCTION };
    bool isOptional[] = { false, false, true };
    if (!checkParameters(isolate, args, "sendParameterData(columnIndex, buffer[, callback])",
        3, expectedTypes, &cbfunc_arg, isOptional)) {
        return;
    }
    bool callback_required = (cbfunc_arg >= 0);

    Statement *stmt = ObjectWrap::Unwrap<Statement>(args.This());

    int index;
    if (!checkParameterIndex(isolate, stmt, args, index)) {
        return;
    }

    if (!Statement::checkStatement(stmt, args, cbfunc_arg, callback_required)) {
        return;
    }

    sendParameterDataBaton *baton = new sendParameterDataBaton();

    baton->stmt = stmt;
    baton->dbcapi_stmt_ptr = stmt->dbcapi_stmt_ptr;
    baton->callback_required = callback_required;
    baton->param_index = index;
    if (args[1]->IsNull() || args[1]->IsUndefined() || Buffer::Length(args[1]) <= 0) {
        baton->data_length = 0;
        baton->data = NULL;
    } else {
        baton->data_length = Buffer::Length(args[1]);
        baton->data = Buffer::Data(args[1]);
    }

    baton->send_data_chunk = stmt->send_data_total_chunk;
    stmt->send_data_total_chunk++;

    uv_work_t *req = new uv_work_t();
    req->data = baton;

    if (callback_required) {
        Local<Function> callback = Local<Function>::Cast(args[cbfunc_arg]);
        baton->callback.Reset(isolate, callback);

        int status;
        status = uv_queue_work(uv_default_loop(), req, sendParameterDataWork,
            (uv_after_work_cb)sendParameterDataAfter);
        assert(status == 0);

        return;
    }

    sendParameterDataWork(req);

    bool succeeded = (baton->succeeded == 1) ? true : false;
    bool err = baton->err;

    delete baton;
    delete req;

    if (err) {
        return;
    }

    args.GetReturnValue().Set(Boolean::New(isolate, succeeded));
}

NODE_API_FUNC(Statement::functionCode)
/************************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope(isolate);
    Statement *stmt = ObjectWrap::Unwrap<Statement>(args.This());
    ConnectionLock lock(stmt);
    if( !lock.isValid() ) {
        throwError(JS_ERR_NOT_CONNECTED);
        return;
    }
    args.GetReturnValue().Set(Integer::New(isolate, api.dbcapi_get_function_code(stmt->dbcapi_stmt_ptr)));
}

NODE_API_FUNC(Statement::getColumnInfo)
/*************************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope(isolate);
    Statement *stmt = ObjectWrap::Unwrap<Statement>(args.This());

    std::vector<dbcapi_column_info*> column_infos;
    int num_cols = fetchColumnInfos(stmt->dbcapi_stmt_ptr, column_infos);
    Local<Array> columnInfos = Array::New(isolate);

    for (int i = 0; i < num_cols; i++) {
        Local<Object> columnInfo = Object::New(isolate);
        setColumnInfo(isolate, columnInfo, column_infos[i]);
        columnInfos->Set(i, columnInfo);
    }

    args.GetReturnValue().Set(columnInfos);
    freeColumnInfos(column_infos);
}

NODE_API_FUNC(Statement::getPrintLines)
/*************************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope(isolate);
    Statement *stmt = ObjectWrap::Unwrap<Statement>(args.This());

    int count = 0;
    size_t buffer_size = 1024;
    size_t length_indicator = 0;
    char* buffer = new char[buffer_size];
    int host_type = 4; // SQLDBC_HOSTTYPE_UTF8
    Local<Array> printLines = Array::New(isolate);

    do {
        dbcapi_retcode ret = api.dbcapi_get_print_line(stmt->dbcapi_stmt_ptr, host_type, (void*) buffer,
                                                       &length_indicator, buffer_size, true);
        if (ret == DBCAPI_OK) {
            printLines->Set(count, String::NewFromUtf8(isolate, buffer, NewStringType::kNormal,
                                                       static_cast<int>(length_indicator)).ToLocalChecked());
            count++;
        } else if (ret == DBCAPI_DATA_TRUNC) {
            delete[] buffer;
            buffer_size = length_indicator + 4;
            buffer = new char[buffer_size];
        } else {
            break;
        }
    } while (true);

    args.GetReturnValue().Set(printLines);
    delete[] buffer;
}

NODE_API_FUNC(Statement::getRowStatus)
/*************************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope(isolate);
    Statement *stmt = ObjectWrap::Unwrap<Statement>(args.This());

    args.GetReturnValue().SetUndefined();

    if (stmt->batch_size >= 1) {
        dbcapi_i32* rowStatus = api.dbcapi_get_row_status(stmt->dbcapi_stmt_ptr);
        if (rowStatus != NULL) {
            Local<Array> ret = Array::New(isolate);
            for (int i = 0; i < stmt->batch_size; i++) {
                ret->Set(i, Integer::New(isolate, rowStatus[i]));
            }
            args.GetReturnValue().Set(ret);
        }
    }
}

NODE_API_FUNC(Statement::getServerCPUTime)
/*************************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope(isolate);
    Statement *stmt = ObjectWrap::Unwrap<Statement>(args.This());
    dbcapi_i64 cpu_time = api.dbcapi_get_stmt_server_cpu_time(stmt->dbcapi_stmt_ptr);
    args.GetReturnValue().Set( Number::New( isolate, (double)cpu_time) );
}

NODE_API_FUNC(Statement::getServerMemoryUsage)
/*************************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope(isolate);
    Statement *stmt = ObjectWrap::Unwrap<Statement>(args.This());
    dbcapi_i64 memory_usage = api.dbcapi_get_stmt_server_memory_usage(stmt->dbcapi_stmt_ptr);
    args.GetReturnValue().Set( Number::New( isolate, (double)memory_usage) );
}

NODE_API_FUNC(Statement::getServerProcessingTime)
/*************************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope(isolate);
    Statement *stmt = ObjectWrap::Unwrap<Statement>(args.This());
    dbcapi_i64 processing_time = api.dbcapi_get_stmt_server_processing_time(stmt->dbcapi_stmt_ptr);
    args.GetReturnValue().Set( Number::New( isolate, (double)processing_time) );
}

struct getParamDataBaton {
    Persistent<Function> 	callback;
    bool 			err;
    int                         error_code;
    std::string 		error_msg;
    std::string                 sql_state;
    bool 			callback_required;

    StatementPointer 		stmt;
    ResultSetPointer            rs;
    int 			retVal;

    int                         param_index;
    int                         data_offset;
    int                         length;
    void                        *buffer;

    getParamDataBaton() {
        err = false;
        callback_required = false;
    }

    ~getParamDataBaton() {
        callback.Reset();
    }
};

void Statement::getDataAfter(uv_work_t *req)
/****************************************************/
{
    Isolate *isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);
    getParamDataBaton *baton = static_cast<getParamDataBaton*>(req->data);
    Local<Value> undef = Local<Value>::New(isolate, Undefined(isolate));

    if (baton->err) {
        callBack(baton->error_code, &(baton->error_msg), &(baton->sql_state),
                 baton->callback, undef, baton->callback_required);
    } else {
        Local<Value> retObj = Local<Value>::New(isolate, Integer::New(isolate, baton->retVal));
        callBack(0, NULL, NULL, baton->callback, retObj, baton->callback_required);
    }

    delete baton;
    delete req;
}

void Statement::getDataWork(uv_work_t *req)
/***************************************************/
{
    getParamDataBaton *baton = static_cast<getParamDataBaton*>(req->data);
    ConnectionLock lock(baton->stmt);

    if( !lock.isValid() ) {
        baton->err = true;
        getErrorMsg(JS_ERR_NOT_CONNECTED, baton->error_code, baton->error_msg, baton->sql_state);
        return;
    }

    dbcapi_stmt *dbcapi_stmt_ptr = baton->rs ? baton->rs->dbcapi_stmt_ptr : baton->stmt->dbcapi_stmt_ptr;
    baton->retVal = api.dbcapi_get_param_data(dbcapi_stmt_ptr, baton->param_index, baton->data_offset, baton->buffer, baton->length);

    if (baton->retVal == -1) {
        baton->err = true;
        getErrorMsg(baton->stmt->connection->dbcapi_conn_ptr, baton->error_code, baton->error_msg, baton->sql_state);
    }
}

NODE_API_FUNC(Statement::getData)
/*******************************/
{
    Isolate *isolate = args.GetIsolate();
    Local<Context> context = isolate->GetCurrentContext();
    HandleScope scope(isolate);
    int cbfunc_arg = -1;

    args.GetReturnValue().SetUndefined();

    // JavaScript Parameters
    // int paramIndex   - zero-based parameter ordinal.
    // int dataOffset   - index within the LOB parameter from which to begin the read operation.
    // byte[] buffer    - buffer into which to copy the data.
    // int bufferOffset - index with the buffer to which the data will be copied.
    // int length       - maximum number of bytes/characters to read.
    // function cb      - callback function

    unsigned int expectedTypes[] = { JS_INTEGER, JS_INTEGER, JS_BUFFER, JS_INTEGER, JS_INTEGER, JS_FUNCTION };
    bool isOptional[] = { false, false, false, false, false, true };
    if (!checkParameters(isolate, args, "getData(paramIndex, dataOffset, buffer, bufferOffset, length[, callback])",
        6, expectedTypes, &cbfunc_arg, isOptional)) {
        return;
    }
    bool callback_required = (cbfunc_arg == 5);

    Statement *stmt = ObjectWrap::Unwrap<Statement>(args.This());

    int index;
    if (!checkParameterIndex(isolate, stmt, args, index)) {
        return;
    }

    if (!Statement::checkStatement(stmt, args, cbfunc_arg, callback_required)) {
        return;
    }

    int data_offset = (args[1]->Int32Value(context)).FromJust();
    int buffer_offset = (args[3]->Int32Value(context)).FromJust();
    int length = (args[4]->Int32Value(context)).FromJust();

    // Check data_offset, buffer_offset, length
    std::string errText;
    if (data_offset < 0) {
        errText = "Invalid dataOffset.";
    }
    else if (buffer_offset < 0) {
        errText = "Invalid bufferOffset.";
    }
    else if (length < 0) {
        errText = "Invalid length.";
    }
    if (errText.length() > 0) {
        std::string sqlState = "HY000";
        throwError(JS_ERR_INVALID_ARGUMENTS, errText, sqlState);
        return;
    }

    getParamDataBaton *baton = new getParamDataBaton();
    baton->stmt = stmt;
    if( !stmt->resultsets.empty() ) {
        // use the last-executed resultset if there is one
        baton->rs = stmt->resultsets.back();
    }
    baton->callback_required = callback_required;
    baton->param_index = index;
    baton->data_offset = data_offset;
    baton->length = length;
    baton->buffer = Buffer::Data(args[2]) + buffer_offset;

    uv_work_t *req = new uv_work_t();
    req->data = baton;

    if (callback_required) {
        Local<Function> callback = Local<Function>::Cast(args[cbfunc_arg]);
        baton->callback.Reset(isolate, callback);

        int status = uv_queue_work(uv_default_loop(), req, getDataWork,
            (uv_after_work_cb)getDataAfter);
        assert(status == 0);
        _unused(status);

        return;
    }

    getDataWork(req);
    int retVal = baton->retVal;
    getDataAfter(req);

    args.GetReturnValue().Set(Integer::New(isolate, retVal));
}

NODE_API_FUNC(Statement::setTimeout)
/***********************************************************************/
{
    Isolate *isolate = args.GetIsolate();
    Local<Context> context = isolate->GetCurrentContext();
    HandleScope scope(isolate);

    args.GetReturnValue().SetUndefined();

    // check parameters
    unsigned int expectedTypes[] = { JS_INTEGER };
    if (!checkParameters(isolate, args, "setTimeout(timeout)", 1, expectedTypes)) {
        return;
    }

    int timeout = (args[0]->Int32Value(context)).FromMaybe(-1);
    if (timeout < 0) {
        std::string sqlState = "HY000";
        std::string errText = "Invalid timeout.";
        throwError(JS_ERR_INVALID_ARGUMENTS, errText, sqlState);
        return;
    }

    Statement *stmt = ObjectWrap::Unwrap<Statement>(args.This());
    if (!Statement::checkStatement(stmt, args, -1, false)) {
        return;
    }

    ConnectionLock lock(stmt);
    if (!lock.isValid()) {
        throwError(JS_ERR_NOT_CONNECTED);
        return;
    }

    if (!api.dbcapi_set_query_timeout(stmt->dbcapi_stmt_ptr, timeout)) {
        int errorCode;
        std::string errorMsg;
        std::string sqlState;
        getErrorMsg(stmt->connection->dbcapi_conn_ptr, errorCode, errorMsg, sqlState);
        throwError(errorCode, errorMsg, sqlState);
    }
}

void Statement::isParameterNull(const FunctionCallbackInfo<Value> &args)
/**********************************************************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope(isolate);

    args.GetReturnValue().SetUndefined();

    // check parameter
    unsigned int expectedTypes[] = { JS_INTEGER };
    if (!checkParameters(isolate, args, "isParameterNull(paramIndex)", 1, expectedTypes)) {
        return;
    }

    Statement *stmt = ObjectWrap::Unwrap<Statement>(args.This());
    if (!Statement::checkStatement(stmt, args, -1, false)) {
        return;
    }

    ConnectionLock lock(stmt);
    if( !lock.isValid() ) {
        throwError(JS_ERR_NOT_CONNECTED);
        return;
    }

    int index;
    if (!checkParameterIndex(isolate, stmt, args, index)) {
        return;
    }

    dbcapi_data_value & value = stmt->params[index]->value;
    args.GetReturnValue().Set(Boolean::New(isolate, (*value.is_null)));
}

void Statement::getParameterLength(const FunctionCallbackInfo<Value> &args)
/*************************************************************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope(isolate);

    args.GetReturnValue().SetUndefined();

    // check parameter
    unsigned int expectedTypes[] = { JS_INTEGER };
    if (!checkParameters(isolate, args, "getParameterLength(paramIndex)", 1, expectedTypes)) {
        return;
    }

    Statement *stmt = ObjectWrap::Unwrap<Statement>(args.This());
    if (!Statement::checkStatement(stmt, args, -1, false)) {
        return;
    }

    ConnectionLock lock(stmt);
    if( !lock.isValid() ) {
        throwError(JS_ERR_NOT_CONNECTED);
        return;
    }

    int index;
    if (!checkParameterIndex(isolate, stmt, args, index)) {
        return;
    }

    dbcapi_data_value & value = stmt->params[index]->value;
    args.GetReturnValue().Set(Integer::New(isolate, (int)(*value.length)));
}
