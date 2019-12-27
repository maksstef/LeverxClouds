// ***************************************************************************
// Copyright (c) 2016 SAP SE or an SAP affiliate company. All rights reserved.
// ***************************************************************************
#include "nodever_cover.h"
#include "hana_utils.h"

using namespace v8;

DBCAPIInterface api;
unsigned openConnections = 0;
uv_mutex_t api_mutex;
ConnectionPoolManager connPoolManager;

ConnectionLock::ConnectionLock( Connection *conn ) :
    conn( conn ),
    lock( conn->conn_mutex )
{}

bool ConnectionLock::isValid() const
/**********************************/
{
    return conn && conn->dbcapi_conn_ptr && conn->is_connected && !conn->is_disconnecting && !conn->is_disconnected;
}

void DBCAPI_CALLBACK warningCallback(dbcapi_stmt    *stmt,
                                     const char     *warning,
                                     dbcapi_i32     error_code,
                                     const char     *sql_state,
                                     void           *user_data)
/*********************************************/
{
    Connection *conn = (Connection*)user_data;

    { // Add the warning to conn->warnings
        scoped_lock lock( conn->warning_mutex );
        warningMessage* warningMsg = new warningMessage( error_code, warning, sql_state );
        conn->warnings.push_back( warningMsg );
    }

    if( conn->warningCallback.IsEmpty() ) {
        return;
    }

    warningCallbackBaton *baton = new warningCallbackBaton();

    if (baton == NULL) {
        return;
    }

    baton->conn = conn;
    baton->err = true;
    baton->error_code = error_code;
    baton->error_msg = warning;
    baton->sql_state = sql_state;
    baton->callback_required = true;

    uv_work_t *req = new uv_work_t();
    req->data = baton;

    int status;
    status = uv_queue_work(uv_default_loop(), req, Connection::warningCallbackWork,
        (uv_after_work_cb)Connection::warningCallbackAfter);
    assert(status == 0);
}

void Connection::warningCallbackWork(uv_work_t *req)
/**************************************************/
{
}

void Connection::warningCallbackAfter(uv_work_t *req)
/***************************************************/
{
    warningCallbackBaton *baton = static_cast<warningCallbackBaton*>(req->data);
    Isolate *isolate = Isolate::GetCurrent();
    if (isolate == NULL) {
        isolate = baton->conn->isolate;
    }

    HandleScope	scope(isolate);
    Local<Value> undef = Local<Value>::New(isolate, Undefined(isolate));

    baton->callback.Reset( isolate, baton->conn->warningCallback );
    if (!baton->conn->is_disconnected && !baton->conn->is_disconnecting && !baton->callback.IsEmpty()) {
        callBack(baton->error_code, &(baton->error_msg), &(baton->sql_state),
                 baton->callback, undef, baton->callback_required);
    }

    delete baton;
    delete req;
}

Connection::Connection(const FunctionCallbackInfo<Value> &args)
/*************************************************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope(isolate);
    uv_mutex_init(&conn_mutex);
    uv_mutex_init(&warning_mutex);
    dbcapi_conn_ptr = NULL;
    autoCommit = true;
    is_connected = false;
    is_disconnected = false;
    is_disconnecting = false;

    is_pooled = false;
    isolation_level = 1; // ISOLATIONLEVEL_READ_COMMITTED
    locale = "";
    current_schema = "";
    use_props_to_connect = false;
    row_set_size = DEFAULT_ROW_SET_SIZE;

    if (args.Length() >= 1) {
        if (args[0]->IsString()) {
            Local<String> str = args[0]->ToString(isolate);
#if NODE_MAJOR_VERSION >= 12
            int string_len = str->Utf8Length(isolate);
            char *buf = new char[string_len + 1];
            str->WriteUtf8(isolate, buf);
#else
            int string_len = str->Utf8Length();
            char *buf = new char[string_len + 1];
            str->WriteUtf8(buf);
#endif
            _arg.Reset(isolate, String::NewFromUtf8(isolate, buf));
            delete[] buf;
        } else if (args[0]->IsObject()) {
            getConnectionString(isolate, args[0]->ToObject(isolate), _arg, conn_prop_keys, conn_prop_values);
            use_props_to_connect = true;
        }  else if (!args[0]->IsUndefined() && !args[0]->IsNull()) {
            throwErrorIP(0, "createConnection[createClient]([conn_params])",
                         "string|object", getJSTypeName(getJSType(args[0])).c_str());
            return;
        } else {
            _arg.Reset(isolate, String::NewFromUtf8(isolate, ""));
        }
    } else {
        _arg.Reset(isolate, String::NewFromUtf8(isolate, ""));
    }
}

struct freeBaton
{
    dbcapi_connection     *dbcapi_conn_ptr;
};

void Connection::freeWork(uv_work_t *req)
/******************************************/
{
    // we do not need to acquire conn_mutex here because
    // there are no references to Connection that could
    // possibly be doing anything with dbcapi_conn_ptr

    freeBaton *baton = static_cast<freeBaton*>(req->data);
    if (baton->dbcapi_conn_ptr != NULL) {
        api.dbcapi_disconnect(baton->dbcapi_conn_ptr);
        api.dbcapi_free_connection(baton->dbcapi_conn_ptr);
        openConnections--;
    }
}

void Connection::freeAfter(uv_work_t *req)
/*******************************************/
{
    Isolate *isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);
    freeBaton *baton = static_cast<freeBaton*>(req->data);
    delete baton;
    delete req;
}

#if defined(_WIN32) || defined(_WIN64)
unsigned __stdcall freeConnection(void* args)
#else // defined(__linux__) || defined(__APPLE__)
void* freeConnection(void* args)
#endif
{
    dbcapi_connection* dbcapi_conn_ptr = reinterpret_cast<dbcapi_connection*>(args);
    api.dbcapi_disconnect(dbcapi_conn_ptr);
    api.dbcapi_free_connection(dbcapi_conn_ptr);
    return NULL;
}

Connection::~Connection()
/***********************/
{
    // Destructor should only be run by garbage collection
    // If Connection is being garbage collection, there
    // must be no statements, as all statements hold a Ref()
    // to the Connection
    assert( statements.empty() );

    _arg.Reset();

    for (size_t i = 0; i < client_infos.size(); i++) {
        delete client_infos[i];
    }

    for (size_t i = 0; i < conn_prop_keys.size(); i++) {
        delete conn_prop_keys[i];
    }

    for (size_t i = 0; i < conn_prop_values.size(); i++) {
        delete conn_prop_values[i];
    }

    warningCallback.Reset();

    if (this->dbcapi_conn_ptr != NULL) {
        /*freeBaton *baton = new freeBaton();
        baton->dbcapi_conn_ptr = this->dbcapi_conn_ptr;
        this->dbcapi_conn_ptr = NULL;
        uv_work_t *req = new uv_work_t();
        req->data = baton;
        int status = uv_queue_work(uv_default_loop(), req, freeWork,
            (uv_after_work_cb)freeAfter);
        assert(status == 0);*/
#if defined(_WIN32) || defined(_WIN64)
        _beginthreadex(NULL,
                       0,
                       freeConnection,
                       reinterpret_cast<void *>(dbcapi_conn_ptr),
                       0, 0);
#else // defined(__linux__) || defined(__APPLE__)
        pthread_t freeConnThread;
        pthread_create(&freeConnThread, NULL, freeConnection, reinterpret_cast<void*>(dbcapi_conn_ptr));
#endif
    }
};

Persistent<Function> Connection::constructor;

void Connection::Init(Isolate *isolate)
/***************************************/
{
    HandleScope scope(isolate);
    // Prepare constructor template
    Local<FunctionTemplate> tpl = FunctionTemplate::New(isolate, New);
    tpl->SetClassName(String::NewFromUtf8(isolate, "Connection"));
    tpl->InstanceTemplate()->SetInternalFieldCount(1);

    // Prototype
    NODE_SET_PROTOTYPE_METHOD(tpl, "exec", exec);
    NODE_SET_PROTOTYPE_METHOD(tpl, "execute", exec);
    NODE_SET_PROTOTYPE_METHOD(tpl, "prepare", prepare);
    NODE_SET_PROTOTYPE_METHOD(tpl, "connect", connect);
    NODE_SET_PROTOTYPE_METHOD(tpl, "disconnect", disconnect);
    NODE_SET_PROTOTYPE_METHOD(tpl, "close", disconnect);
    NODE_SET_PROTOTYPE_METHOD(tpl, "end", disconnect);
    NODE_SET_PROTOTYPE_METHOD(tpl, "commit", commit);
    NODE_SET_PROTOTYPE_METHOD(tpl, "rollback", rollback);
    NODE_SET_PROTOTYPE_METHOD(tpl, "abort", abort);
    NODE_SET_PROTOTYPE_METHOD(tpl, "getAutoCommit", getAutoCommit);
    NODE_SET_PROTOTYPE_METHOD(tpl, "setAutoCommit", setAutoCommit);
    NODE_SET_PROTOTYPE_METHOD(tpl, "getClientInfo", getClientInfo);
    NODE_SET_PROTOTYPE_METHOD(tpl, "setClientInfo", setClientInfo);
    NODE_SET_PROTOTYPE_METHOD(tpl, "setRowSetSize", setRowSetSize);
    NODE_SET_PROTOTYPE_METHOD(tpl, "setWarningCallback", setWarningCallback);
    NODE_SET_PROTOTYPE_METHOD(tpl, "state", state);
    NODE_SET_PROTOTYPE_METHOD(tpl, "clearPool", clearPool);
    NODE_SET_PROTOTYPE_METHOD(tpl, "getWarnings", getWarnings);

    Local<Context> context = isolate->GetCurrentContext();
    constructor.Reset(isolate, tpl->GetFunction(context).ToLocalChecked());
}

void Connection::New(const FunctionCallbackInfo<Value> &args)
/*************************************************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope(isolate);

    if (args.IsConstructCall()) {
        Connection *conn = new Connection(args);
        conn->Wrap(args.This());
        args.GetReturnValue().Set(args.This());
    }
    else {
        const int argc = 1;
        Local<Value> argv[argc] = {args[0]};
        Local<Context> context = isolate->GetCurrentContext();
        Local<Function> cons = Local<Function>::New(isolate, constructor);
        Local<Object> instance = cons->NewInstance(context, argc, argv).ToLocalChecked();
        args.GetReturnValue().Set(instance);
    }
}

void Connection::NewInstance(const FunctionCallbackInfo<Value> &args)
/*********************************************************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope(isolate);
    const unsigned argc = 1;
    Local<Value> argv[argc] = {args[0]};
    Local<Context> context = isolate->GetCurrentContext();
    Local<Function> cons = Local<Function>::New(isolate, constructor);
    Local<Object> instance = cons->NewInstance(context, argc, argv).ToLocalChecked();
    args.GetReturnValue().Set(instance);
}

NODE_API_FUNC( Connection::exec )
/*******************************/
{
    Isolate *isolate = args.GetIsolate();
    Local<Context> context = isolate->GetCurrentContext();
    HandleScope scope( isolate );
    Local<Value> undef = Local<Value>::New( isolate, Undefined( isolate ) );

    int  num_args = args.Length();
    int  cbfunc_arg = -1;
    int  options_arg = -1;
    bool invalidArg = false;
    bool bind_required = false;
    executeOptions execOptions;

    args.GetReturnValue().SetUndefined();

    if ( num_args == 0 || !args[0]->IsString() ) {
        invalidArg = true;
    } else if (num_args == 2) {
        if (args[1]->IsArray()) {
            bind_required = true;
        } else if (args[1]->IsFunction()) {
            cbfunc_arg = 1;
        } else if (args[1]->IsObject()) {
            if (getExecuteOptions(isolate, args[1]->ToObject(context), &execOptions)) {
                options_arg = 1;
            } else {
                bind_required = true;
            }
        } else if (!args[1]->IsNull() && !args[1]->IsUndefined()) {
            invalidArg = true;
        }
    } else if (num_args == 3) {
        if (args[1]->IsArray()) {
            bind_required = true;
            if (args[2]->IsFunction()) {
                cbfunc_arg = 2;
            } else if (args[2]->IsObject()) {
                if (getExecuteOptions(isolate, args[2]->ToObject(context), &execOptions)) {
                    options_arg = 2;
                }  else {
                    invalidArg = true;
                }
            } else if (!args[2]->IsNull() && !args[2]->IsUndefined()) {
                invalidArg = true;
            }
        } else if (args[1]->IsObject()) {
            if (getExecuteOptions(isolate, args[1]->ToObject(context), &execOptions)) {
                options_arg = 1;
            } else {
                bind_required = true;
            }
            if (args[2]->IsFunction()) {
                cbfunc_arg = 2;
            } else if (args[2]->IsObject()) {
                bind_required = true;
                options_arg = 2;
            } else if (!args[2]->IsNull() && !args[2]->IsUndefined()) {
                invalidArg = true;
            }
        } else if (args[1]->IsFunction()) {
            cbfunc_arg = 1;
            if (!args[2]->IsNull() && !args[2]->IsUndefined()) {
                invalidArg = true;
            }
        } else if (args[1]->IsNull() || args[1]->IsUndefined()) {
            if (args[2]->IsFunction()) {
                cbfunc_arg = 2;
            } else if (args[2]->IsObject()) {
                if (getExecuteOptions(isolate, args[2]->ToObject(context), &execOptions)) {
                    options_arg = 0;
                } else {
                    invalidArg = true;
                }
            } else if (!args[2]->IsNull() && !args[2]->IsUndefined()) {
                invalidArg = true;
            }
        }
    } else if (num_args >= 4) {
        if (args[1]->IsArray() || args[1]->IsObject() || args[1]->IsNull() || args[1]->IsUndefined()) {
            bind_required = args[1]->IsArray() || args[1]->IsObject();
            if (args[2]->IsObject() || args[2]->IsNull() || args[2]->IsUndefined()) {
                options_arg = args[2]->IsObject() ? 2 : -1;
                if (args[3]->IsFunction() || args[3]->IsNull() || args[3]->IsUndefined()) {
                    cbfunc_arg = args[3]->IsFunction() ? 3 : -1;
                }
            }
        } else {
            invalidArg = true;
        }
    }

    if (invalidArg) {
        std::string sqlState = "HY000";
        std::string errText = "Invalid parameter for function 'exec[ute](sql[, params][, options][, callback])'";
        throwError(JS_ERR_INVALID_ARGUMENTS, errText, sqlState);
        return;
    }

    bool callback_required = (cbfunc_arg >= 0);
    Connection *conn = ObjectWrap::Unwrap<Connection>( args.This() );

    if( conn == NULL || conn->dbcapi_conn_ptr == NULL ) {
        int error_code;
	std::string error_msg;
        std::string sql_state;
        getErrorMsg( JS_ERR_INVALID_OBJECT, error_code, error_msg, sql_state );
        if( callback_required ) {
            callBack(error_code, &error_msg, &sql_state, args[cbfunc_arg], undef, callback_required);
        } else {
            throwError(error_code, error_msg, sql_state);
        }
	args.GetReturnValue().SetUndefined();
	return;
    }

#if NODE_MAJOR_VERSION >= 12
    String::Utf8Value param0( isolate, (args[0]->ToString(context)).ToLocalChecked() );
#else
    String::Utf8Value param0( (args[0]->ToString(context)).ToLocalChecked() );
#endif

    executeBaton *baton = new executeBaton();
    baton->dbcapi_stmt_ptr = NULL;
    baton->conn = conn;
    baton->callback_required = callback_required;
    baton->stmt_str = std::string(*param0);
    baton->del_stmt_ptr = true;

    if ( options_arg >= 0 ) {
        getExecuteOptions(isolate, args[options_arg]->ToObject(context), &baton->exec_options);
    }

    if( bind_required ) {
        if (!getInputParameters(isolate, args[1], baton->provided_params, baton->dbcapi_stmt_ptr,
            baton->error_code, baton->error_msg, baton->sql_state)) {
            if (callback_required) {
                callBack( baton->error_code, &baton->error_msg, &baton->sql_state, args[cbfunc_arg], undef, callback_required );
            } else {
                throwError(baton->error_code, baton->error_msg, baton->sql_state);
            }
            args.GetReturnValue().SetUndefined();
            delete baton;
            return;
	}
    }

    uv_work_t *req = new uv_work_t();
    req->data = baton;

    if( callback_required ) {
	Local<Function> callback = Local<Function>::Cast(args[cbfunc_arg]);
	baton->callback.Reset( isolate, callback );
	int status;
        status = uv_queue_work( uv_default_loop(), req, executeWork,
				(uv_after_work_cb)executeAfter );
	assert(status == 0);

	args.GetReturnValue().SetUndefined();
	return;
    }

    Persistent<Value> ResultSet;

    executeWork( req );
    bool success = fillResult( baton, ResultSet );

    if( baton->dbcapi_stmt_ptr != NULL ) {
        ConnectionLock lock( conn );
        if( lock.isValid() ) {
            api.dbcapi_free_stmt( baton->dbcapi_stmt_ptr );
        }
    }

    delete baton;
    delete req;

    if( !success ) {
	args.GetReturnValue().SetUndefined();
	return;
    }
    Local<Value> local_result = Local<Value>::New( isolate, ResultSet );
    args.GetReturnValue().Set( local_result );
    ResultSet.Reset();
}

struct prepareBaton {
    Persistent<Function> 	callback;
    bool 			err;
    int                         error_code;
    std::string 		error_msg;
    std::string                 sql_state;
    bool 			callback_required;

    ConnectionPointer           conn;
    StatementPointer	       	stmt;
    std::string 		stmt_str;
    Persistent<Value> 		stmtObj;

    prepareBaton() {
	err = false;
	callback_required = false;
    }

    ~prepareBaton() {
	callback.Reset();
	stmtObj.Reset();
    }
};

void Connection::prepareWork( uv_work_t *req )
/*********************************************/
{
    prepareBaton *baton = static_cast<prepareBaton*>(req->data);
    ConnectionLock lock(baton->conn);

    if( !lock.isValid() ) {
        baton->err = true;
        getErrorMsg(JS_ERR_NOT_CONNECTED, baton->error_code, baton->error_msg, baton->sql_state);
        return;
    }

    if( !baton->stmt ) {
	baton->err = true;
	getErrorMsg( JS_ERR_INVALID_OBJECT, baton->error_code , baton->error_msg, baton->sql_state );
	return;
    }

    baton->stmt->dbcapi_stmt_ptr = api.dbcapi_prepare( baton->conn->dbcapi_conn_ptr,
                                                       baton->stmt_str.c_str() );

    if( baton->stmt->dbcapi_stmt_ptr == NULL ) {
	baton->err = true;
	getErrorMsg( baton->conn->dbcapi_conn_ptr, baton->error_code, baton->error_msg, baton->sql_state );
	return;
    }

    int num_params = api.dbcapi_num_params( baton->stmt->dbcapi_stmt_ptr );

    for (int i = 0; i < num_params; i++) {
        dbcapi_bind_param_info info;
        api.dbcapi_get_bind_param_info(baton->stmt->dbcapi_stmt_ptr, i, &info);
        baton->stmt->param_infos.push_back(info);
    }
}

void Connection::prepareAfter( uv_work_t *req )
/**********************************************/
{
    Isolate *isolate = Isolate::GetCurrent();
    HandleScope scope( isolate );
    prepareBaton *baton = static_cast<prepareBaton*>(req->data);
    Local<Value> undef = Local<Value>::New( isolate, Undefined( isolate ) );

    if( baton->err ) {
	callBack( baton->error_code, &( baton->error_msg ), &( baton->sql_state),
                  baton->callback, undef, baton->callback_required );
	delete baton;
	delete req;
	return;
    }

    if( baton->callback_required ) {
	Local<Value> stmtObj = Local<Value>::New( isolate, baton->stmtObj );
	callBack( 0, NULL, NULL, baton->callback, stmtObj,  baton->callback_required );
	baton->stmtObj.Reset();
    }

    delete baton;
    delete req;
}


// NOT THREAD-SAFE; DO NOT CALL FROM WORKER THREAD OR WHILE DISCONNECTING
void Connection::addStatement( Statement *stmt )
/**********************************************/
{
    assert(!is_disconnecting);

    stmt->connection = this;
    statements.push_back( stmt );
}

// NOT THREAD-SAFE; DO NOT CALL FROM WORKER THREAD
void Connection::removeStatement( Statement *stmt )
/*************************************************/
{
    // if disconnecting, don't modify the statements list; disconnectWork will take care of clearing the list
    if( !is_disconnecting ) {
        statements.remove( stmt );
        stmt->connection = NULL;
    }
}

NODE_API_FUNC( Connection::prepare )
/**********************************/
{
    Isolate *isolate = args.GetIsolate();
    Local<Context> context = isolate->GetCurrentContext();
    HandleScope scope( isolate );
    int cbfunc_arg = -1;
    Local<Value> undef = Local<Value>::New( isolate, Undefined( isolate ) );

    args.GetReturnValue().SetUndefined();

    // check parameters
    unsigned int expectedTypes[] = { JS_STRING, JS_FUNCTION };
    bool isOptional[] = { false, true };
    if (!checkParameters(isolate, args, "prepare(sql, [callback])", 2, expectedTypes, &cbfunc_arg, isOptional)) {
        return;
    }
    bool callback_required = (cbfunc_arg == 1);

    Connection *conn = ObjectWrap::Unwrap<Connection>( args.This() );

    if( conn == NULL || conn->dbcapi_conn_ptr == NULL ) {
        int error_code;
	std::string error_msg;
        std::string sql_state;
	getErrorMsg( JS_ERR_NOT_CONNECTED, error_code, error_msg, sql_state );
        if (callback_required) {
            callBack(error_code, &error_msg, &sql_state, args[cbfunc_arg], undef, callback_required);
        } else {
            throwError(error_code, error_msg, sql_state);
        }
	args.GetReturnValue().SetUndefined();
	return;
    }

    Persistent<Object> p_stmt;
    Statement::CreateNewInstance( args, p_stmt );
    Local<Object> l_stmt = Local<Object>::New( isolate, p_stmt );
    Statement *stmt = ObjectWrap::Unwrap<Statement>( l_stmt );

    if( stmt == NULL ) {
        int error_code;
        std::string error_msg;
        std::string sql_state;
	getErrorMsg( JS_ERR_GENERAL_ERROR, error_code, error_msg, sql_state );
        if (callback_required) {
            callBack( error_code, &error_msg, &sql_state, args[cbfunc_arg], undef, callback_required );
        } else {
            throwError(error_code, error_msg, sql_state);
        }
        p_stmt.Reset();
	return;
    }

    conn->addStatement( stmt );

#if NODE_MAJOR_VERSION >= 12
    String::Utf8Value param0( isolate, (args[0]->ToString(context)).ToLocalChecked());
#else
    String::Utf8Value param0( (args[0]->ToString(context)).ToLocalChecked() );
#endif

    prepareBaton *baton = new prepareBaton();
    baton->conn = conn;
    baton->stmt = stmt;
    baton->stmt->sql = std::string(*param0);
    baton->callback_required = callback_required;
    baton->stmt_str =  std::string(*param0);

    uv_work_t *req = new uv_work_t();
    req->data = baton;

    if( callback_required ) {
	Local<Function> callback = Local<Function>::Cast(args[cbfunc_arg]);
	baton->callback.Reset( isolate, callback );
	baton->stmtObj.Reset( isolate, p_stmt );
	int status;
        status = uv_queue_work( uv_default_loop(), req, prepareWork,
				(uv_after_work_cb)prepareAfter );
	assert(status == 0);
	p_stmt.Reset();
	return;
    }

    prepareWork( req );
    bool err = baton->err;
    prepareAfter( req );

    if( err ) {
	return;
    }

    args.GetReturnValue().Set( p_stmt );
    p_stmt.Reset();
}

// Connect and disconnect
// Connect Function
struct connectBaton {
    Persistent<Function> 	callback;
    bool 			err;
    int                         error_code;
    std::string 		error_msg;
    std::string                 sql_state;
    bool 			callback_required;

    ConnectionPointer 	       	conn;
    bool 			external_connection;
    std::string 		conn_string;
    void 			*external_conn_ptr;

    connectBaton() {
	external_conn_ptr = NULL;
	external_connection = false;
	err = false;
	callback_required = false;
    }

    ~connectBaton() {
	external_conn_ptr = NULL;
	callback.Reset();
    }
};

void Connection::connectWork( uv_work_t *req )
/*********************************************/
{
    connectBaton *baton = static_cast<connectBaton*>(req->data);
    ConnectionLock lock(baton->conn);

    if( lock.isValid() ) {
        // we don't want the lock to be valid here; cannot connect twice!
        baton->err = true;
        getErrorMsg(JS_ERR_CONNECTION_ALREADY_EXISTS, baton->error_code, baton->error_msg, baton->sql_state);
        return;
    }

    if( !baton->external_connection ) {
        if (baton->conn->is_pooled && baton->conn->dbcapi_conn_ptr == NULL) {
            // if setClientInfo was called prior to connection, there
            // may already be a dbcapi_conn_ptr set and options will
            // already be set on it, so we cannot pick up a pooled connection
            baton->conn->dbcapi_conn_ptr = connPoolManager.allocate(baton->conn_string);
        }

        if (!baton->conn->is_pooled || baton->conn->dbcapi_conn_ptr == NULL) {
            if( baton->conn->dbcapi_conn_ptr == NULL ) {
                // if setClientInfo was called prior to connection, there
                // may already be a dbcapi_conn_ptr set; use it
                baton->conn->dbcapi_conn_ptr = api.dbcapi_new_connection();
            }
            if( baton->conn->dbcapi_conn_ptr == NULL ) {
                baton->err = true;
                getErrorMsg(JS_ERR_ALLOCATION_FAILED, baton->error_code, baton->error_msg, baton->sql_state);
                return;
            }
            if (baton->conn->use_props_to_connect) {
                dbcapi_bool succeeded = true;
                for (size_t i = 0; i < baton->conn->conn_prop_keys.size(); i++) {
                    succeeded = api.dbcapi_set_connect_property(baton->conn->dbcapi_conn_ptr,
                        baton->conn->conn_prop_keys[i]->c_str(), baton->conn->conn_prop_values[i]->c_str());
                    if (!succeeded) {
                        break;
                    }
                }
                if (succeeded) {
                    succeeded = api.dbcapi_set_connect_property(baton->conn->dbcapi_conn_ptr, "CHARSET", "UTF-8");
                }
                if (succeeded) {
                    succeeded = api.dbcapi_set_connect_property(baton->conn->dbcapi_conn_ptr, "SCROLLABLERESULT", "0");
                }
                if (succeeded) {
                    succeeded = api.dbcapi_connect2(baton->conn->dbcapi_conn_ptr);
                }
                if (!succeeded) {
                    getErrorMsg(baton->conn->dbcapi_conn_ptr, baton->error_code, baton->error_msg, baton->sql_state);
                    baton->err = true;
                    api.dbcapi_free_connection(baton->conn->dbcapi_conn_ptr);
                    baton->conn->dbcapi_conn_ptr = NULL;
                    return;
                }
            } else {
                if (!api.dbcapi_connect(baton->conn->dbcapi_conn_ptr, baton->conn_string.c_str())) {
                    getErrorMsg(baton->conn->dbcapi_conn_ptr, baton->error_code, baton->error_msg, baton->sql_state);
                    baton->err = true;
                    api.dbcapi_free_connection(baton->conn->dbcapi_conn_ptr);
                    baton->conn->dbcapi_conn_ptr = NULL;
                    return;
                }
            }
        }
        api.dbcapi_set_autocommit(baton->conn->dbcapi_conn_ptr, baton->conn->autoCommit);
    } else {
	baton->conn->dbcapi_conn_ptr = api.dbcapi_make_connection( baton->external_conn_ptr );
        api.dbcapi_set_autocommit( baton->conn->dbcapi_conn_ptr, baton->conn->autoCommit );
	if( baton->conn->dbcapi_conn_ptr == NULL ) {
	    getErrorMsg( baton->conn->dbcapi_conn_ptr, baton->error_code, baton->error_msg, baton->sql_state );
	    return;
	}
    }

    api.dbcapi_register_warning_callback(baton->conn->dbcapi_conn_ptr, ::warningCallback, baton->conn);

    baton->conn->is_connected = true;
    baton->conn->is_disconnected = false;
    baton->conn->is_disconnecting = false;
    baton->conn->external_connection = baton->external_connection;

    openConnections++;

    // we should be connected now!
    if( !lock.isValid() ) {
        baton->err = true;
        getErrorMsg(JS_ERR_NOT_CONNECTED, baton->error_code, baton->error_msg, baton->sql_state);
        return;
    }
}

void Connection::connectAfter( uv_work_t *req )
/**********************************************/
{
    Isolate *isolate = Isolate::GetCurrent();
    HandleScope scope( isolate );
    connectBaton *baton = static_cast<connectBaton*>(req->data);
    Local<Value> undef = Local<Value>::New( isolate, Undefined( isolate ) );

    if( baton->err ) {
	callBack( baton->error_code, &( baton->error_msg ), &( baton->sql_state ),
                  baton->callback, undef, baton->callback_required );
	delete baton;
	delete req;
	return;
    }

    callBack( 0, NULL, NULL, baton->callback, undef, baton->callback_required, false );

    delete baton;
    delete req;
}

NODE_API_FUNC( Connection::connect )
/**********************************/
{
    Isolate      *isolate = args.GetIsolate();
    Local<Context> context = isolate->GetCurrentContext();
    HandleScope  scope( isolate );
    int		 num_args = args.Length();
    int		 cbfunc_arg = -1;
    bool	 arg_is_string = false;
    bool	 arg_is_object = false;
    bool	 external_connection = false;
    int          invalidArg = -1;
    unsigned int expectedTypes[] = { JS_STRING | JS_OBJECT, JS_FUNCTION };

    args.GetReturnValue().SetUndefined();

    if( num_args == 1 ) {
        if( args[0]->IsFunction() ) {
	    cbfunc_arg = 0;
        } else if( args[0]->IsNumber() ){
	    external_connection = true;
        } else if( args[0]->IsString() ) {
            arg_is_string = true;
        } else if( args[0]->IsObject() ) {
	    arg_is_object = true;
        } else if( !args[0]->IsUndefined() && !args[0]->IsNull() ){
            invalidArg = 0;
        }
    } else if( num_args >= 2 ) {
        if( args[0]->IsString() || args[0]->IsObject() || args[0]->IsNumber() || args[0]->IsUndefined() || args[0]->IsNull() ) {
            if( args[1]->IsFunction() || args[1]->IsUndefined() || args[1]->IsNull()) {
                cbfunc_arg = (args[1]->IsFunction()) ? 1 : -1;
                if( args[0]->IsNumber() ) {
                    external_connection = true;
                } else if( args[0]->IsString() ) {
                    arg_is_string = true;
                }  else if( args[0]->IsObject() ) {
                    arg_is_object = true;
                }
            } else {
                invalidArg = 1;
            }
        } else {
            invalidArg = 0;
        }
    }

    if( invalidArg >= 0 ) {
        throwErrorIP(invalidArg, "connect(conn_params[, callback])",
                     getJSTypeName(expectedTypes[invalidArg]).c_str(),
                     getJSTypeName(getJSType(args[invalidArg])).c_str());
	return;
    }

    bool callback_required = (cbfunc_arg >= 0);
    Connection *conn = ObjectWrap::Unwrap<Connection>(args.This());
    conn->isolate = isolate;
    connectBaton *baton = new connectBaton();
    baton->conn = conn;
    baton->callback_required = callback_required;

    baton->external_connection = external_connection;

    if( external_connection ) {
	baton->external_conn_ptr = (void *)(long long)(args[0]->NumberValue(context)).FromJust();
    } else {
	Local<String> localArg = Local<String>::New( isolate, conn->_arg );
	if( localArg->Length() > 0 ) {
#if NODE_MAJOR_VERSION >= 12
            String::Utf8Value param0( isolate, localArg );
#else
            String::Utf8Value param0( localArg );
#endif
	    conn->conn_string = std::string(*param0);
	} else {
	    conn->conn_string = std::string();
	}
	if( arg_is_string ) {
#if NODE_MAJOR_VERSION >= 12
            String::Utf8Value param0( isolate, (args[0]->ToString(context)).ToLocalChecked() );
#else
            String::Utf8Value param0( (args[0]->ToString(context)).ToLocalChecked() );
#endif
            if ( conn->conn_string.length() > 0 )
	        conn->conn_string.append( ";" );
	    conn->conn_string.append(*param0);
	} else if( arg_is_object ) {
	    Persistent<String> arg_string;
            getConnectionString( isolate, (args[0]->ToObject(context)).ToLocalChecked(), arg_string, conn->conn_prop_keys, conn->conn_prop_values );
            conn->use_props_to_connect = true;
	    Local<String> local_arg_string =
		Local<String>::New( isolate, arg_string );
#if NODE_MAJOR_VERSION >= 12
            String::Utf8Value param0( isolate, local_arg_string );
#else
            String::Utf8Value param0( local_arg_string );
#endif
            if(conn->conn_string.length() > 0)
                conn->conn_string.append( ";" );
	    conn->conn_string.append(*param0);
	    arg_string.Reset();
	}
	conn->conn_string.append( ";CHARSET=UTF-8;SCROLLABLERESULT=0" );
        conn->getConnectionProperties();
        baton->conn_string = conn->conn_string;
    }

    uv_work_t *req = new uv_work_t();
    req->data = baton;

    if( callback_required ) {
	Local<Function> callback = Local<Function>::Cast(args[cbfunc_arg]);
	baton->callback.Reset( isolate, callback );

	int status;
        status = uv_queue_work( uv_default_loop(), req, connectWork,
				(uv_after_work_cb)connectAfter );
	assert(status == 0);
	args.GetReturnValue().SetUndefined();
	return;
    }

    connectWork( req );
    connectAfter( req );
    args.GetReturnValue().SetUndefined();
    return;
}

// Disconnect Function
void Connection::disconnectWork( uv_work_t *req )
/************************************************/
{
    noParamBaton *baton = static_cast<noParamBaton*>(req->data);
    ConnectionLock lock(baton->conn);

    if( !baton->conn || baton->conn->dbcapi_conn_ptr == NULL || !baton->conn->is_connected ) {
        baton->err = true;
        getErrorMsg(JS_ERR_NOT_CONNECTED, baton->error_code, baton->error_msg, baton->sql_state);
        return;
    }

    if (!baton->conn->is_disconnecting) {
        baton->err = true;
        getErrorMsg(JS_ERR_INVALID_OBJECT, baton->error_code, baton->error_msg, baton->sql_state);
        return;
    }

    // we are now disconnecting
    // it is safe to traverse statements list
    for( std::list<Statement*>::iterator it = baton->conn->statements.begin(); it != baton->conn->statements.end(); ++it ) {
        (*it)->_drop();
    }
    baton->conn->statements.clear();

    baton->conn->is_disconnected = true;

    api.dbcapi_register_warning_callback(baton->conn->dbcapi_conn_ptr, NULL, baton->conn);

    if (baton->conn->is_pooled) {
        if (baton->conn->autoCommit) {
            // Commit transaction
            if (!api.dbcapi_commit(baton->conn->dbcapi_conn_ptr)) {
                baton->err = true;
                getErrorMsg(baton->conn->dbcapi_conn_ptr, baton->error_code, baton->error_msg, baton->sql_state);
                return;
            }
        } else {
            // Rollback transaction
            if (!api.dbcapi_rollback(baton->conn->dbcapi_conn_ptr)) {
                baton->err = true;
                getErrorMsg(baton->conn->dbcapi_conn_ptr, baton->error_code, baton->error_msg, baton->sql_state);
                return;
            }
        }

        // Unset session variables
        for (size_t i = 0; i < baton->conn->client_infos.size(); i++) {
            api.dbcapi_set_clientinfo(baton->conn->dbcapi_conn_ptr, baton->conn->client_infos[i]->c_str(), NULL);
        }

        // Set Isolation level
        if (!api.dbcapi_set_transaction_isolation(baton->conn->dbcapi_conn_ptr, baton->conn->isolation_level)) {
            baton->err = true;
            getErrorMsg(baton->conn->dbcapi_conn_ptr, baton->error_code, baton->error_msg, baton->sql_state);
            return;
        }

        // Set locale
        if (baton->conn->locale.length() > 0) {
            api.dbcapi_set_clientinfo(baton->conn->dbcapi_conn_ptr, "LOCALE", baton->conn->locale.c_str());
        }

        // Set client
        if (baton->conn->client.length() > 0) {
            api.dbcapi_set_clientinfo(baton->conn->dbcapi_conn_ptr, "CLIENT", baton->conn->client.c_str());
        }

        // Set schema
        if (baton->conn->current_schema.length() > 0) {
            std::string sql = "SET SCHEMA " + baton->conn->current_schema;
            if (!api.dbcapi_execute_direct(baton->conn->dbcapi_conn_ptr, sql.c_str())) {
                baton->err = true;
                getErrorMsg(baton->conn->dbcapi_conn_ptr, baton->error_code, baton->error_msg, baton->sql_state);
                return;
            }
        }

        connPoolManager.add(baton->conn->conn_string, baton->conn->dbcapi_conn_ptr);
    } else {
        if (!baton->conn->external_connection) {
            api.dbcapi_disconnect(baton->conn->dbcapi_conn_ptr);
        }
        // Must free the connection object or there will be a memory leak
        api.dbcapi_free_connection(baton->conn->dbcapi_conn_ptr);
    }

    baton->conn->is_connected = false;
    baton->conn->dbcapi_conn_ptr = NULL;

    openConnections--;
    if( openConnections <= 0 ) {
	openConnections = 0;
    }
}

NODE_API_FUNC( Connection::disconnect )
/*************************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope( isolate );
    int cbfunc_arg = -1;

    args.GetReturnValue().SetUndefined();

    // check parameters
    unsigned int expectedTypes[] = { JS_FUNCTION };
    bool isOptional[] = { true };
    if (!checkParameters(isolate, args, "disconnect([callback])", 1, expectedTypes, &cbfunc_arg, isOptional)) {
        return;
    }
    bool callback_required = (cbfunc_arg >= 0);

    Connection *conn = ObjectWrap::Unwrap<Connection>( args.This() );

    if( conn->is_disconnected || conn->is_disconnecting || !conn->is_connected ) {
        if (callback_required) {
            Local<Value> undef = Local<Value>::New(isolate, Undefined(isolate));
            Local<Function> callback = Local<Function>::Cast(args[cbfunc_arg]);
            callBack(0, NULL, NULL, callback, undef, true);
        }
        return;
    }

    conn->is_disconnecting = true;

    noParamBaton *baton = new noParamBaton();
    baton->callback_required = callback_required;
    baton->conn = conn;

    for( std::list<Statement*>::iterator it = baton->conn->statements.begin(); it != baton->conn->statements.end(); ++it ) {
        (*it)->_setDropping();
    }

    uv_work_t *req = new uv_work_t();
    req->data = baton;

    if( callback_required ) {
	Local<Function> callback = Local<Function>::Cast(args[cbfunc_arg]);
	baton->callback.Reset( isolate, callback );

	int status;
        status = uv_queue_work( uv_default_loop(), req, disconnectWork,
				(uv_after_work_cb)noParamAfter );
	assert(status == 0);

	args.GetReturnValue().SetUndefined();
	return;
    }

    disconnectWork( req );
    noParamAfter( req );
}

void Connection::commitWork( uv_work_t *req )
/********************************************/
{
    noParamBaton *baton = static_cast<noParamBaton*>(req->data);
    ConnectionLock lock(baton->conn);

    if( !lock.isValid() ) {
        baton->err = true;
        getErrorMsg(JS_ERR_NOT_CONNECTED, baton->error_code, baton->error_msg, baton->sql_state);
        return;
    }

    if( !api.dbcapi_commit( baton->conn->dbcapi_conn_ptr ) ) {
	baton->err = true;
	getErrorMsg( baton->conn->dbcapi_conn_ptr, baton->error_code, baton->error_msg, baton->sql_state );
	return;
    }
}

NODE_API_FUNC( Connection::commit )
/*********************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope( isolate );
    int cbfunc_arg = -1;

    args.GetReturnValue().SetUndefined();

    // check parameters
    unsigned int expectedTypes[] = { JS_FUNCTION };
    bool isOptional[] = { true };
    if (!checkParameters(isolate, args, "commit([callback])", 1, expectedTypes, &cbfunc_arg, isOptional)) {
        return;
    }
    bool callback_required = (cbfunc_arg >= 0);

    Connection *conn = ObjectWrap::Unwrap<Connection>( args.This() );

    noParamBaton *baton = new noParamBaton();
    baton->conn = conn;
    baton->callback_required = callback_required;

    uv_work_t *req = new uv_work_t();
    req->data = baton;

    if( callback_required ) {
	Local<Function> callback = Local<Function>::Cast(args[cbfunc_arg]);
	baton->callback.Reset( isolate, callback );

	int status;
        status = uv_queue_work( uv_default_loop(), req, commitWork,
				(uv_after_work_cb)noParamAfter );
	assert(status == 0);

	args.GetReturnValue().SetUndefined();
	return;
    }

    commitWork( req );
    noParamAfter( req );
}

void Connection::rollbackWork( uv_work_t *req )
/**********************************************/
{
    noParamBaton *baton = static_cast<noParamBaton*>(req->data);
    ConnectionLock lock(baton->conn);

    if( !lock.isValid() ) {
        baton->err = true;
        getErrorMsg(JS_ERR_NOT_CONNECTED, baton->error_code, baton->error_msg, baton->sql_state);
        return;
    }

    if( !api.dbcapi_rollback( baton->conn->dbcapi_conn_ptr ) ) {
	baton->err = true;
	getErrorMsg( baton->conn->dbcapi_conn_ptr, baton->error_code, baton->error_msg, baton->sql_state );
	return;
    }
}

NODE_API_FUNC( Connection::rollback )
/***********************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope( isolate );
    int cbfunc_arg = -1;

    args.GetReturnValue().SetUndefined();

    // check parameters
    unsigned int expectedTypes[] = { JS_FUNCTION };
    bool isOptional[] = { true };
    if (!checkParameters(isolate, args, "rollback([callback])", 1, expectedTypes, &cbfunc_arg, isOptional)) {
        return;
    }
    bool callback_required = (cbfunc_arg >= 0);

    Connection *conn = ObjectWrap::Unwrap<Connection>( args.This() );

    noParamBaton *baton = new noParamBaton();
    baton->conn = conn;
    baton->callback_required = callback_required;

    uv_work_t *req = new uv_work_t();
    req->data = baton;

    if( callback_required ) {
	Local<Function> callback = Local<Function>::Cast(args[cbfunc_arg]);
	baton->callback.Reset( isolate, callback );

	int status;
        status = uv_queue_work( uv_default_loop(), req, rollbackWork,
				(uv_after_work_cb)noParamAfter );
	assert(status == 0);

	args.GetReturnValue().SetUndefined();
	return;
    }

    rollbackWork( req );
    noParamAfter( req );
}

NODE_API_FUNC(Connection::getAutoCommit)
/***********************************************************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope(isolate);

    Connection *conn = ObjectWrap::Unwrap<Connection>(args.This());
    ConnectionLock lock( conn );

    args.GetReturnValue().Set(Boolean::New(isolate, conn->autoCommit));
}

NODE_API_FUNC(Connection::setAutoCommit)
/***********************************************************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope(isolate);

    args.GetReturnValue().SetUndefined();

    // check parameters
    unsigned int expectedTypes[] = { JS_BOOLEAN };
    if (!checkParameters(isolate, args, "setAutoCommit(flag)", 1, expectedTypes)) {
        return;
    }

    Connection *conn = ObjectWrap::Unwrap<Connection>(args.This());
    ConnectionLock lock( conn );

    // don't check for lock.isValid() as we want to support this prior to connect

    convertToBool(isolate, args[0], conn->autoCommit);

    if (conn->dbcapi_conn_ptr != NULL) {
        if (!api.dbcapi_set_autocommit(conn->dbcapi_conn_ptr, conn->autoCommit)) {
            throwError(conn->dbcapi_conn_ptr);
        }
    }
}

NODE_API_FUNC(Connection::getClientInfo)
/***********************************************************************/
{
    Isolate *isolate = args.GetIsolate();
    Local<Context> context = isolate->GetCurrentContext();
    HandleScope scope(isolate);

    args.GetReturnValue().Set(Null(isolate));

    // check parameters
    unsigned int expectedTypes[] = { JS_STRING };
    if (!checkParameters(isolate, args, "getClientInfo(key)", 1, expectedTypes)) {
        return;
    }

#if NODE_MAJOR_VERSION >= 12
    String::Utf8Value prop(isolate, (args[0]->ToString(context)).ToLocalChecked());
#else
    String::Utf8Value prop((args[0]->ToString(context)).ToLocalChecked());
#endif
    Connection *conn = ObjectWrap::Unwrap<Connection>(args.This());
    ConnectionLock lock( conn );
    if( !lock.isValid() ) {
        throwError(JS_ERR_NOT_CONNECTED);
        return;
    }

    const char* val = api.dbcapi_get_clientinfo(conn->dbcapi_conn_ptr, *prop);
    if (val != NULL) {
        args.GetReturnValue().Set(String::NewFromUtf8(isolate, val, NewStringType::kNormal, (int)strlen(val)).ToLocalChecked());
    }
}

NODE_API_FUNC(Connection::setClientInfo)
/***********************************************************************/
{
    Isolate *isolate = args.GetIsolate();
    Local<Context> context = isolate->GetCurrentContext();
    HandleScope scope(isolate);

    args.GetReturnValue().SetUndefined();

    // check parameters
    unsigned int expectedTypes[] = { JS_STRING, JS_STRING | JS_UNDEFINED | JS_NULL };
    if (!checkParameters(isolate, args, "setClientInfo(key, value)", 2, expectedTypes)) {
        return;
    }

    Connection *conn = ObjectWrap::Unwrap<Connection>(args.This());
    ConnectionLock lock(conn);

    if (conn->dbcapi_conn_ptr == NULL) {
        // create initial dbcapi_conn_ptr
        // it is not yet connected, but we need it for dbcapi_set_clientinfo
        conn->dbcapi_conn_ptr = api.dbcapi_new_connection();
    }

    if (conn->dbcapi_conn_ptr == NULL) {
        throwError(JS_ERR_ALLOCATION_FAILED);
        return;
    }

#if NODE_MAJOR_VERSION >= 12
    String::Utf8Value prop( isolate, (args[0]->ToString(context)).ToLocalChecked() );
#else
    String::Utf8Value prop((args[0]->ToString(context)).ToLocalChecked());
#endif

    bool found = false;
    for (size_t i = 0; i < conn->client_infos.size(); i++) {
        if (compareString(*conn->client_infos[i], *prop, true)) {
            found = true;
            break;
        }
    }
    if (!found) {
        conn->client_infos.push_back(new std::string(*prop));
    }

    if (args[1]->IsNull() || args[1]->IsUndefined()) {
        api.dbcapi_set_clientinfo(conn->dbcapi_conn_ptr, *prop, NULL);
    } else {
#if NODE_MAJOR_VERSION >= 12
        String::Utf8Value val(isolate, (args[1]->ToString(context)).ToLocalChecked());
#else
        String::Utf8Value val((args[1]->ToString(context)).ToLocalChecked());
#endif
        if (val.length() == 0) {
            api.dbcapi_set_clientinfo(conn->dbcapi_conn_ptr, *prop, NULL);
        }
        else {
            api.dbcapi_set_clientinfo(conn->dbcapi_conn_ptr, *prop, *val);
        }
    }
}

// Generic Baton and Callback (After) Function
// Use this if the function does not have any return values and
// Does not take any parameters.
// Create custom Baton and Callback (After) functions otherwise

void Connection::noParamAfter(uv_work_t *req)
/*********************************************/
{
    Isolate *isolate = Isolate::GetCurrent();
    HandleScope	scope(isolate);
    noParamBaton *baton = static_cast<noParamBaton*>(req->data);
    Local<Value> undef = Local<Value>::New(isolate, Undefined(isolate));

    if (baton->err) {
        callBack(baton->error_code, &(baton->error_msg), &(baton->sql_state),
                 baton->callback, undef, baton->callback_required, false);
        return;
    }

    callBack(0, NULL, NULL, baton->callback, undef, baton->callback_required, false);

    delete baton;
    delete req;
}

NODE_API_FUNC(Connection::setWarningCallback)
/***********************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope(isolate);
    int cbfunc_arg = -1;

    args.GetReturnValue().SetUndefined();

    // check parameters
    unsigned int expectedTypes[] = { JS_FUNCTION };
    bool isOptional[] = { true };
    if (!checkParameters(isolate, args, "setWarningCallback(callback)", 1, expectedTypes, &cbfunc_arg, isOptional)) {
        return;
    }

    Connection *conn = ObjectWrap::Unwrap<Connection>(args.This());

    conn->warningCallback.Reset();

    if (args[0]->IsUndefined() || args[0]->IsNull()) {
        return;
    }

    Local<Function> callback = Local<Function>::Cast(args[cbfunc_arg]);
    conn->warningCallback.Reset(isolate, callback);
}

NODE_API_FUNC(Connection::getWarnings)
/*******************************************************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope(isolate);
    Connection *conn = ObjectWrap::Unwrap<Connection>(args.This());
    scoped_lock lock(conn->warning_mutex);

    Local<Array> warnings = Array::New(isolate);
    for (size_t i = 0; i < conn->warnings.size(); i++) {
        Local<Object> warning = Object::New(isolate);
        warning->Set(String::NewFromUtf8(isolate, "code"),
                     Integer::New(isolate, conn->warnings[i]->error_code));
        warning->Set(String::NewFromUtf8(isolate, "message"),
                     String::NewFromUtf8(isolate, conn->warnings[i]->message->c_str()));
        warning->Set(String::NewFromUtf8(isolate, "sqlState"),
                     String::NewFromUtf8(isolate, conn->warnings[i]->sql_state->c_str()));
        warnings->Set((uint32_t)i, warning);
    }
    clearVector(conn->warnings);

    args.GetReturnValue().Set(warnings);
}

NODE_API_FUNC(Connection::setRowSetSize)
/***********************************************************************/
{
    Isolate *isolate = args.GetIsolate();
    Local<Context> context = isolate->GetCurrentContext();
    HandleScope scope(isolate);

    args.GetReturnValue().SetUndefined();

    // check parameters
    unsigned int expectedTypes[] = { JS_INTEGER };
    if (!checkParameters(isolate, args, "setRowSetSize(rowSetSize)", 1, expectedTypes)) {
        return;
    }

    int rowSetSize = (args[0]->Int32Value(context)).FromMaybe(1);
    if (rowSetSize <= 0) {
        std::string sqlState = "HY000";
        std::string errText = "Invalid row set size.";
        throwError(JS_ERR_INVALID_ARGUMENTS, errText, sqlState);
        return;
    }

    Connection *conn = ObjectWrap::Unwrap<Connection>(args.This());
    ConnectionLock lock(conn);
    conn->row_set_size = rowSetSize;
}

NODE_API_FUNC(Connection::state)
/*****************************************************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope(isolate);

    Connection *obj = ObjectWrap::Unwrap<Connection>(args.This());
    if (obj->is_connected) {
        args.GetReturnValue().Set(String::NewFromUtf8(isolate, "connected"));
    } else {
        args.GetReturnValue().Set(String::NewFromUtf8(isolate, "disconnected"));
    }
}

void Connection::abortWork( uv_work_t *req )
/**********************************************/
{
    noParamBaton *baton = static_cast<noParamBaton*>(req->data);

    // Avoid calling functions which lock the connection
    // because the connection could be already locked.
    if (baton->conn->dbcapi_conn_ptr == NULL) {
        baton->err = true;
        getErrorMsg(JS_ERR_NOT_CONNECTED, baton->error_code, baton->error_msg, baton->sql_state);
        return;
    }

    api.dbcapi_abort(baton->conn->dbcapi_conn_ptr);
}

NODE_API_FUNC( Connection::abort )
/***********************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope( isolate );
    int cbfunc_arg = -1;

    args.GetReturnValue().SetUndefined();

    // check parameters
    unsigned int expectedTypes[] = { JS_FUNCTION };
    bool isOptional[] = { true };
    if (!checkParameters(isolate, args, "abort([callback])", 1, expectedTypes, &cbfunc_arg, isOptional)) {
        return;
    }
    bool callback_required = (cbfunc_arg >= 0);

    Connection *conn = ObjectWrap::Unwrap<Connection>( args.This() );

    noParamBaton *baton = new noParamBaton();
    baton->conn = conn;
    baton->callback_required = callback_required;

    uv_work_t *req = new uv_work_t();
    req->data = baton;

    if( callback_required ) {
	Local<Function> callback = Local<Function>::Cast(args[cbfunc_arg]);
	baton->callback.Reset( isolate, callback );

	int status;
        status = uv_queue_work( uv_default_loop(), req, abortWork,
				(uv_after_work_cb)noParamAfter );
	assert(status == 0);

	args.GetReturnValue().SetUndefined();
	return;
    }

    abortWork( req );
    noParamAfter( req );
}

struct clearPoolBaton {
    Persistent<Function> 	callback;
    bool 			err;
    int                         error_code;
    std::string 		error_msg;
    std::string                 sql_state;
    bool 			callback_required;

    ConnectionPointer 	       	conn;
    int                         count;

    clearPoolBaton() {
        err = false;
        callback_required = false;
    }

    ~clearPoolBaton() {
        callback.Reset();
    }
};

void Connection::clearPoolAfter(uv_work_t *req)
/*****************************************/
{
    Isolate *isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);
    clearPoolBaton *baton = static_cast<clearPoolBaton*>(req->data);

    if (baton->err) {
        Local<Value> undef = Local<Value>::New(isolate, Undefined(isolate));
        callBack(baton->error_code, &(baton->error_msg), &(baton->sql_state),
                 baton->callback, undef, baton->callback_required, true);
        delete baton;
        delete req;
        return;
    }

    Local<Value> ret = Local<Value>::New(isolate, Integer::New(isolate, baton->count));
    callBack(0, NULL, NULL, baton->callback, ret, baton->callback_required, true);
    delete baton;
    delete req;
}

void Connection::clearPoolWork(uv_work_t *req)
/****************************************/
{
    clearPoolBaton *baton = static_cast<clearPoolBaton*>(req->data);

    if (!connPoolManager.clearPool(baton->count)) {
        baton->err = true;
        connPoolManager.getError(baton->error_code, baton->error_msg, baton->sql_state);
    }
}

NODE_API_FUNC(Connection::clearPool)
/*************************************************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope(isolate);
    int cbfunc_arg = -1;

    args.GetReturnValue().SetUndefined();

    // check parameters
    unsigned int expectedTypes[] = { JS_FUNCTION };
    bool isOptional[] = { true };
    if (!checkParameters(isolate, args, "next([callback])", 1, expectedTypes, &cbfunc_arg, isOptional)) {
        return;
    }
    bool callback_required = (cbfunc_arg >= 0);

    Connection *conn = ObjectWrap::Unwrap<Connection>(args.This());
    clearPoolBaton *baton = new clearPoolBaton();
    baton->conn = conn;
    baton->callback_required = callback_required;

    uv_work_t *req = new uv_work_t();
    req->data = baton;

    if (callback_required) {
        Local<Function> callback = Local<Function>::Cast(args[cbfunc_arg]);
        baton->callback.Reset(isolate, callback);

        int status = uv_queue_work(uv_default_loop(), req, clearPoolWork,
            (uv_after_work_cb)clearPoolAfter);
        assert(status == 0);
        _unused(status);

        args.GetReturnValue().SetUndefined();
        return;
    }

    clearPoolWork(req);

    bool err = baton->err;
    int count = baton->count;

    clearPoolAfter(req);

    if (err) {
        args.GetReturnValue().SetUndefined();
    } else {
        args.GetReturnValue().Set(Integer::New(isolate, count));
    }
}

void Connection::getConnectionProperties()
/***************************************/
{
    size_t       connLen = this->conn_string.length();
    const char * connStr = this->conn_string.c_str();
    char         keyString[512];
    char *       valueString = new char[connLen + 1];

    if (use_props_to_connect) {
        const char* keyPtr;
        const char* valuePtr;
        size_t len;
        for (size_t i = 0; i < conn_prop_keys.size(); i++) {
            keyPtr = conn_prop_keys[i]->c_str();
            len = strlen(keyPtr);
            strncpy(keyString, keyPtr, len);
            keyString[len] = '\0';
            valuePtr = conn_prop_values[i]->c_str();
            len = strlen(valuePtr);
            strncpy(valueString, valuePtr, len);
            valueString[len] = '\0';
            toUpper(keyString);
            checkProperty(keyString, valueString);
        }
    } else {
        size_t offset = 0;
        size_t attrLen = 0;
        do {
            if (!searchAttribute(connStr + offset, attrLen, keyString, valueString)) {
                break;
            } else {
                offset += attrLen;
                checkProperty(keyString, valueString);
            }
        } while (true);
    }

    if (valueString != NULL) {
        delete[] valueString;
    }
}

void Connection::checkProperty(char * keyString,
                               char * valueString)
/*****************************************************************/
{
    if (strcmp("POOLING", keyString) == 0) {
        toUpper(valueString);
        if (strcmp("TRUE", valueString) == 0) {
            this->is_pooled = true;
        }
    } else if (strcmp("ISOLATIONLEVEL", keyString) == 0) {
        toUpper(valueString);
        if (strcmp("READ UNCOMMITTED", valueString) == 0) {
            this->isolation_level = 0;
        } else if (strcmp("READ COMMITTED", valueString) == 0) {
            this->isolation_level = 1;
        } else if (strcmp("REPEATABLE READ", valueString) == 0) {
            this->isolation_level = 2;
        } else if (strcmp("SERIALIZABLE", valueString) == 0) {
            this->isolation_level = 3;
        } else {
            this->isolation_level = std::atol(valueString);
        }
    } else if (strcmp("CURRENTSCHEMA", keyString) == 0) {
        this->current_schema = valueString;
    } else if (strcmp("LOCALE", keyString) == 0) {
        this->locale = valueString;
    } else if (strcmp("CLIENT", keyString) == 0) {
        this->client = valueString;
    }
}

bool Connection::searchAttribute( const char *     connStr,
                                  size_t &         attrLen,
                                  char *           keyStr,
                                  char *           valueStr )
/*****************************************************************/
{
    const char * p1;
    const char * p2;
    const char * p3;
    const char * p4;
    const char * cStr;
    size_t len;

    if( connStr == NULL || keyStr == NULL || valueStr == NULL || *connStr == '\0' ) {
        return( false );
    }

    p1 = strchr( connStr, '=' );

    if( p1 == NULL ) {
        return( false );
    }

    len = p1 - connStr;
    strncpy( keyStr, connStr, len );
    keyStr[len] = '\0';
    removeLeadingBlanks( keyStr );
    removeTrailingBlanks( keyStr );
    toUpper( keyStr );

    cStr = p1 + 1;
    while( *cStr == ' ' ) {
        cStr++;
    }

    if( *cStr == '\0' ) {
        return( false );
    }

    if( *cStr == '"') { // Double quoted
        p2 = strchr( cStr + 1, '"' );
        if( p2 == NULL ) {
            return( false );
        } else {
            len = p2 - cStr - 1;
            strncpy( valueStr, cStr + 1, len );
            valueStr[len] = '\0';
            cStr = p2 + 1;
            while( *cStr == ' ' ) {
                cStr++;
            }
            if( *cStr == ';' || *cStr == '\0' ) {
                attrLen = cStr - connStr + 1;
            } else {
                return( false );
            }
        }
    } else if( *cStr == '\'' ) { // Single quoted
        p2 = strchr( cStr + 1, '\'' );
        if( p2 == NULL ) {
            return( false );
        } else {
            len = p2 - cStr - 1;
            strncpy( valueStr, cStr + 1, len );
            valueStr[len] = '\0';
            cStr = p2 + 1;
            while( *cStr == ' ' ) {
                cStr++;
            }
            if( *cStr == ';' || *cStr == '\0' ) {
                attrLen = cStr - connStr + 1;
            } else {
                return( false );
            }
        }
    } else {
        p2 = strchr( cStr, ';' );
        if( p2 == NULL ) {
            attrLen = strlen( connStr );
            len = attrLen - ( cStr - connStr );
            strncpy( valueStr, cStr, len );
            valueStr[len] = '\0';
        } else {
            p3 = strchr( cStr, '{' );
            if( p3 == NULL || p3 > p2 ) {
                len = p2 - cStr;
                strncpy( valueStr, cStr, len );
                valueStr[len] = '\0';
                attrLen = p2 - connStr + 1;
            } else {
                p4 = strchr( cStr, '}' );
                if( p4 == NULL ) {
                    return( false );
                } else {
                    len = p4 - cStr + 1;
                    strncpy( valueStr, cStr, len );
                    valueStr[len] = '\0';
                }
                cStr = p4 + 1;
                while( *cStr == ' ' ) {
                    cStr++;
                }
                attrLen = cStr - connStr + 1;
                if( *cStr != ';' && *cStr != '\0' ) {
                    return( false );
                }
            }
        }
    }

    removeLeadingBlanks( valueStr );
    removeTrailingBlanks( valueStr );
    removeCurlyBrackets( valueStr );

    return( true );
}

void Connection::removeLeadingBlanks(char * string)
/*****************************************************/
{
    char * p = string;
    char * q = string;

    if (string != NULL) {
        while (*p == ' ') {
            p++;
        }
        if (p > string) {
            while (*p) {
                *q++ = *p++;
            }
            *q = '\0';
        }
    }
}

void Connection::removeTrailingBlanks(char * string)
/******************************************************/
{
    if (string != NULL && string[0] != '\0') {
        char *p = string + strlen(string) - 1;
        while (p - string >= 0 && *p == ' ') {
            p--;
        }
        *++p = '\0';
    }
}

void Connection::removeCurlyBrackets(char * string)
/*****************************************************/
{
    char * p = string;
    char * q = string;

    if (string != NULL) {
        size_t length = strlen(string);

        if (*string != '{' && *(string + length) != '}') {
            return;
        }
        p++;
        while (*p) {
            *q++ = *p++;
        }
        string[length - 2] = '\0';
    }
}

void Connection::toUpper(char * string)
/*****************************************/
{
    char * p = string;

    if (string != NULL) {
        while (*p) {
            *p = toupper(*p);
            p++;
        }
    }
}

void ConnectionPoolManager::add(std::string & connStr, dbcapi_connection* conn)
/*****************************************************************/
{
    scoped_lock lock(conn_pool_mutex);

    PooledConnection* pooled_conn = new PooledConnection(connStr, conn, connections);
    connections = pooled_conn;
}

dbcapi_connection* ConnectionPoolManager::allocate(std::string & connStr)
/*****************************************************************/
{
    scoped_lock lock(conn_pool_mutex);
    PooledConnection* pooled_conn = NULL;

    if (NULL != connections) {
        if (compareString(connStr, *connections->conn_string, true)) {
            pooled_conn = connections;
            connections = connections->next;
        } else {
            PooledConnection* prev = connections;
            PooledConnection* curr = prev->next;
            while (curr != NULL) {
                if (compareString(connStr, *curr->conn_string, true)) {
                    pooled_conn = curr;
                    prev->next = curr->next;
                    break;
                } else {
                    prev = curr;
                    curr = curr->next;
                }
            }
        }
    }

    if (pooled_conn != NULL) {
        dbcapi_connection* dbcapi_conn = pooled_conn->dbcapi_conn;
        delete pooled_conn;
        return dbcapi_conn;
    }

    return NULL;
}

bool ConnectionPoolManager::clearPool(int & count)
/*****************************************************************/
{
    scoped_lock lock(conn_pool_mutex);

    this->err = false;
    count = 0;

    PooledConnection* curr = connections;
    PooledConnection* conn = NULL;

    while (curr != NULL) {
        conn = curr;
        curr = curr->next;

        if (!api.dbcapi_disconnect(conn->dbcapi_conn) && !this->err) {
            this->err = true;
            getErrorMsg(conn->dbcapi_conn, this->error_code, this->error_msg, this->sql_state);
        }
        api.dbcapi_free_connection(conn->dbcapi_conn);

        delete conn;
        count++;
    }

    connections = NULL;

    return !this->err;
}
