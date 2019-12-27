// ***************************************************************************
// Copyright (c) 2016 SAP SE or an SAP affiliate company. All rights reserved.
// ***************************************************************************
#include "hana_utils.h"

using namespace v8;
using namespace node;

// ResultSet Object Functions

ConnectionLock::ConnectionLock( ResultSet *rs ) :
    conn( rs->stmt->connection ),
    lock( conn->conn_mutex )
{}

ResultSet::ResultSet()
/********************/
{
    dbcapi_stmt_ptr = NULL;
    is_closed = false;
    is_closing = false;
    fetched_first = false;
}

struct freeBaton
{
    dbcapi_stmt     *dbcapi_stmt_ptr;
    StatementPointer stmt;
};

ResultSet::~ResultSet()
/*********************/
{
    // Can only get here during garbage collection
    // Therefore, nothing is happening concurrently
    // with this ResultSet, as all active operations
    // have a Ref() to the ResultSet
    
    deleteColumnInfos();

    if( !is_closed ) {
        if( stmt ) {
            stmt->removeResultSet(this);
        }
        // dbcapi_stmt_ptr is freed by the Statement
        /*if (dbcapi_stmt_ptr != NULL) {
            // freeing the stmt pointer uses a mutex; don't do this on the main thread
            freeBaton *baton = new freeBaton();
            baton->stmt = stmt;
            baton->dbcapi_stmt_ptr = dbcapi_stmt_ptr;
            dbcapi_stmt_ptr = NULL;
            uv_work_t *req = new uv_work_t();
            req->data = baton;
            int status = uv_queue_work(uv_default_loop(), req, freeWork,
                                       (uv_after_work_cb)freeAfter);
            assert(status == 0);
        }*/
        is_closed = true;
    }
}

void ResultSet::freeWork(uv_work_t *req)
/******************************************/
{
    freeBaton *baton = static_cast<freeBaton*>(req->data);
    if (baton->stmt && baton->dbcapi_stmt_ptr != NULL) {
        ConnectionLock lock(baton->stmt);
        if( lock.isValid() ) {
            api.dbcapi_free_stmt(baton->dbcapi_stmt_ptr);
        }
    }
}

void ResultSet::freeAfter(uv_work_t *req)
/*******************************************/
{
    Isolate *isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);
    freeBaton *baton = static_cast<freeBaton*>(req->data);
    delete baton;
    delete req;
}

void ResultSet::deleteColumnInfos()
/*********************/
{
    freeColumnInfos(column_infos);
    num_cols = 0;
}

Persistent<Function> ResultSet::constructor;

void ResultSet::Init( Isolate *isolate )
/**************************************/
{
    // Prepare constructor template
    Local<FunctionTemplate> tpl = FunctionTemplate::New( isolate, New );
    tpl->SetClassName( String::NewFromUtf8( isolate, "ResultSet" ) );
    tpl->InstanceTemplate()->SetInternalFieldCount( 1 );

    NODE_SET_PROTOTYPE_METHOD( tpl, "close",		close );
    NODE_SET_PROTOTYPE_METHOD( tpl, "isClosed",	        isClosed );

    // Accessor Functions
    NODE_SET_PROTOTYPE_METHOD( tpl, "getRowCount",      getRowCount );
    NODE_SET_PROTOTYPE_METHOD( tpl, "getColumnCount",	getColumnCount );
    NODE_SET_PROTOTYPE_METHOD( tpl, "getColumnName",	getColumnName );
    NODE_SET_PROTOTYPE_METHOD( tpl, "getColumnInfo",	getColumnInfo );
    NODE_SET_PROTOTYPE_METHOD( tpl, "getValue",	        getValue );
    NODE_SET_PROTOTYPE_METHOD( tpl, "getValues",        getValues );
    NODE_SET_PROTOTYPE_METHOD( tpl, "getValueLength",	getValueLength );
    NODE_SET_PROTOTYPE_METHOD( tpl, "getData",          getData );
    NODE_SET_PROTOTYPE_METHOD( tpl, "isNull",	        isNull );

    NODE_SET_PROTOTYPE_METHOD( tpl, "next",		next );
    NODE_SET_PROTOTYPE_METHOD( tpl, "nextResult",	nextResult );
    NODE_SET_PROTOTYPE_METHOD( tpl, "getServerCPUTime", getServerCPUTime);
    NODE_SET_PROTOTYPE_METHOD( tpl, "getServerMemoryUsage",    getServerMemoryUsage);
    NODE_SET_PROTOTYPE_METHOD( tpl, "getServerProcessingTime", getServerProcessingTime);

    Local<Context> context = isolate->GetCurrentContext();
    constructor.Reset( isolate, tpl->GetFunction( context ).ToLocalChecked() );
}

// Utility Functions and Macros

bool ResultSet::getSQLValue( Isolate *                          isolate,
                             ResultSet *			rs,
			     dbcapi_data_value &		value,
			     const FunctionCallbackInfo<Value> &args )
/********************************************************************/
{
    int colIndex;
    ConnectionLock lock(rs);
    if( !lock.isValid() ) {
        throwError(JS_ERR_NOT_CONNECTED);
        return false;
    }
    
    if ( !validate(isolate, rs, args, colIndex) ) {
        // throwError handled by validate() function
        return false;
    }

    memset(&value, 0, sizeof(dbcapi_data_value));
    if( !api.dbcapi_get_column( rs->dbcapi_stmt_ptr, colIndex, &value ) ) {
	throwError( rs->stmt->connection->dbcapi_conn_ptr );
	return false;
    }

    return true;
}

bool ResultSet::validate( Isolate *                         isolate,
                          ResultSet *			    rs,
			  const FunctionCallbackInfo<Value> &args,
                          int &colIndex )
/********************************************************************/
{
    if( isInvalid( rs ) ) {
	throwError( JS_ERR_RESULTSET_CLOSED );
	return false;
    }

    return checkColumnIndex(isolate, rs, args, colIndex);
}

bool ResultSet::checkColumnIndex( Isolate *isolate,
                                  ResultSet *rs,
			          const FunctionCallbackInfo<Value> &args,
                                  int &colIndex )
/********************************************************************/
{
    Local<Context> context = isolate->GetCurrentContext();

    if (args[0]->IsInt32()) {
        colIndex = args[0]->Int32Value(context).FromMaybe(-1);
        if (colIndex >= 0 && colIndex < rs->num_cols) {
            return true;
        }
    }

    std::string sqlState = "HY000";
    std::string errText = "Invalid column index.";
    throwError(JS_ERR_INVALID_INDEX, errText, sqlState);

    return false;
}

bool ResultSet::isInvalid( ResultSet *rs )
/****************************************/
{
    return rs == NULL
        || rs->is_closed
        || rs->is_closing
        || rs->dbcapi_stmt_ptr == NULL;
}

void ResultSet::getColumnInfos()
/*****************************************/
{
    num_cols = fetchColumnInfos(dbcapi_stmt_ptr, column_infos);
}

struct nextBaton {
    Persistent<Function> 	callback;
    bool 			err;
    int                         error_code;
    std::string 		error_msg;
    std::string                 sql_state;
    bool 			callback_required;

    ResultSetPointer		rs;
    bool 			retVal;

    nextBaton() {
	err = false;
	callback_required = false;
    }

    ~nextBaton() {
	callback.Reset();
    }
};

void ResultSet::nextAfter( uv_work_t *req )
/*****************************************/
{
    Isolate *isolate = Isolate::GetCurrent();
    HandleScope scope( isolate );
    nextBaton *baton = static_cast<nextBaton*>(req->data);
    Local<Value> undef = Local<Value>::New( isolate, Undefined( isolate ) );

    if( baton->err ) {
	callBack( baton->error_code, &( baton->error_msg ), &( baton->sql_state ),
                 baton->callback, undef, baton->callback_required );
	delete baton;
	delete req;
	return;
    }

    Local<Value> ret = Local<Value>::New( isolate,
					  Boolean::New( isolate, baton->retVal ) );
    callBack( 0, NULL, NULL, baton->callback, ret, baton->callback_required );
    delete baton;
    delete req;
}

void ResultSet::nextWork( uv_work_t *req )
/****************************************/
{
    nextBaton *baton = static_cast<nextBaton*>(req->data);

    if (isInvalid(baton->rs)) {
        baton->err = true;
        getErrorMsg(JS_ERR_RESULTSET_CLOSED, baton->error_code, baton->error_msg, baton->sql_state);
        return;
    }

    ConnectionLock lock(baton->rs);

    if( !lock.isValid() ) {
        baton->err = true;
        getErrorMsg(JS_ERR_NOT_CONNECTED, baton->error_code, baton->error_msg, baton->sql_state);
        return;
    }

    if (baton->rs->column_infos.size() > 0) {
        baton->retVal = (api.dbcapi_fetch_next(baton->rs->dbcapi_stmt_ptr) != 0);
    } else {
        baton->retVal = false;
    }
    baton->rs->fetched_first = true;
}

void ResultSet::next( const FunctionCallbackInfo<Value> &args )
/*************************************************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope( isolate );
    int cbfunc_arg = -1;

    args.GetReturnValue().SetUndefined();

    // check parameters
    unsigned int expectedTypes[] = { JS_FUNCTION };
    bool isOptional[] = { true };
    if (!checkParameters(isolate, args, "next([callback])", 1, expectedTypes, &cbfunc_arg, isOptional)) {
        return;
    }
    bool callback_required = (cbfunc_arg >= 0);

    ResultSet *rs = ObjectWrap::Unwrap<ResultSet>(args.This());

    if (isInvalid(rs)) {
        throwError( JS_ERR_RESULTSET_CLOSED);
        return;
    }

    nextBaton *baton = new nextBaton();
    baton->rs = rs;
    baton->callback_required = callback_required;

    uv_work_t *req = new uv_work_t();
    req->data = baton;

    if( callback_required ) {
	Local<Function> callback = Local<Function>::Cast(args[cbfunc_arg]);
	baton->callback.Reset( isolate, callback );

        int status = uv_queue_work( uv_default_loop(), req, nextWork,
				    (uv_after_work_cb)nextAfter );
	assert(status == 0);
	_unused( status );

	args.GetReturnValue().SetUndefined();
	return;
    }

    nextWork( req );
    bool err = baton->err;
    bool retVal = baton->retVal;
    nextAfter( req );

    if( err ) {
	args.GetReturnValue().SetUndefined();
	return;
    }
    args.GetReturnValue().Set( Boolean::New( isolate, retVal ) );
}

void ResultSet::getRowCount(const FunctionCallbackInfo<Value> &args)
/*******************************************************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope(isolate);
    ResultSet *rs = ObjectWrap::Unwrap<ResultSet>(args.This());

    if (isInvalid(rs)) {
        args.GetReturnValue().SetUndefined();
        throwError(JS_ERR_RESULTSET_CLOSED);
    } else {
        int rows = api.dbcapi_num_rows(rs->dbcapi_stmt_ptr);
        args.GetReturnValue().Set(Integer::New(isolate, rows));
    }
}

void ResultSet::getColumnCount(const FunctionCallbackInfo<Value> &args)
/*******************************************************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope(isolate);
    ResultSet *rs = ObjectWrap::Unwrap<ResultSet>(args.This());

    if (isInvalid(rs)) {
        args.GetReturnValue().SetUndefined();
    } else {
        args.GetReturnValue().Set(Integer::New(isolate, rs->num_cols));
    }
}

void ResultSet::getColumnName(const FunctionCallbackInfo<Value> &args)
/*******************************************************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope(isolate);

    args.GetReturnValue().SetUndefined();

    // check parameters
    unsigned int expectedTypes[] = { JS_INTEGER };
    if (!checkParameters(isolate, args, "getColumnName(colIndex)", 1, expectedTypes)) {
        return;
    }

    int colIndex;
    ResultSet *rs = ObjectWrap::Unwrap<ResultSet>(args.This());
    if (!isInvalid(rs) && checkColumnIndex(isolate, rs, args, colIndex)) {
        args.GetReturnValue().Set(String::NewFromUtf8(isolate, rs->column_infos[colIndex]->name));
    }
}

void ResultSet::getColumnInfo(const FunctionCallbackInfo<Value> &args)
/*******************************************************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope(isolate);
    ResultSet *rs = ObjectWrap::Unwrap<ResultSet>(args.This());

    if (isInvalid(rs)) {
        args.GetReturnValue().SetUndefined();
    } else {
        Local<Array> columnInfos = Array::New(isolate);
        for (int i = 0; i < rs->num_cols; i++) {
            Local<Object> columnInfo = Object::New(isolate);
            setColumnInfo(isolate, columnInfo, rs->column_infos[i]);
            columnInfos->Set(i, columnInfo);
        }

        args.GetReturnValue().Set(columnInfos);
    }
}

void ResultSet::getValue(const FunctionCallbackInfo<Value> &args)
/*********************************************************************/
{
    Isolate *isolate = args.GetIsolate();
    Local<Context> context = isolate->GetCurrentContext();
    HandleScope scope(isolate);

    args.GetReturnValue().SetUndefined();

    // check parameters
    unsigned int expectedTypes[] = { JS_INTEGER };
    if (!checkParameters(isolate, args, "getValue(colIndex)", 1, expectedTypes)) {
        return;
    }

    ResultSet *rs = ObjectWrap::Unwrap<ResultSet>(args.This());

    if (isInvalid(rs)) {
        throwError(JS_ERR_RESULTSET_CLOSED);
        return;
    }

    dbcapi_data_value value;
    if (getSQLValue(isolate, rs, value, args)) {
        int colIndex = (args[0]->Int32Value(context)).FromJust();
        Local<Value> localVal;
        int retCode = getReturnValue(isolate, value, rs->column_infos[colIndex]->native_type, localVal);
        if (retCode == 0) {
            args.GetReturnValue().Set(localVal);
        } else {
            int errorCode;
            std::string errorMsg;
            std::string sqlState;
            getErrorMsg(retCode, errorCode, errorMsg, sqlState);
            throwError(errorCode, errorMsg, sqlState);
        }
    }
}

void ResultSet::getValues(const FunctionCallbackInfo<Value> &args)
/*********************************************************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope(isolate);
    ResultSet *rs = ObjectWrap::Unwrap<ResultSet>(args.This());
    Local<Object> row = Object::New(isolate);
    int int_number;

    args.GetReturnValue().SetUndefined();

    if (isInvalid(rs)) {
        throwError(JS_ERR_RESULTSET_CLOSED);
        return;
    }

    ConnectionLock lock(rs);

    if( !lock.isValid() ) {
        throwError(JS_ERR_NOT_CONNECTED);
        return;
    }

    for (int i = 0; i < rs->num_cols; i++) {
        dbcapi_data_value value;
        memset(&value, 0, sizeof(dbcapi_data_value));

        if (!api.dbcapi_get_column(rs->dbcapi_stmt_ptr, i, &value)) {
            throwError(rs->stmt->connection->dbcapi_conn_ptr);
            return;
        }

        Local<String> col_name = String::NewFromUtf8(isolate, rs->column_infos[i]->name);

        if (value.is_null != NULL && *(value.is_null)) {
            row->Set(col_name, Null(isolate));
            continue;
        }

        switch (value.type) {
            case A_INVALID_TYPE:
                args.GetReturnValue().Set(Null(isolate));
                break;
            case A_VAL32:
            case A_VAL16:
            case A_UVAL16:
            case A_VAL8:
            case A_UVAL8:
                convertToInt(value, int_number, true);
                if (rs->column_infos[i]->native_type == DT_BOOLEAN) {
                    row->Set(col_name, Boolean::New(isolate, int_number > 0 ? true : false));
                }  else {
                    row->Set(col_name, Integer::New(isolate, int_number));
                }
                break;
            case A_UVAL32:
            case A_UVAL64:
            {
                unsigned long long int64_number = *(unsigned long long*)(value.buffer);
                if (int64_number > kMaxSafeInteger) {
                    std::ostringstream strstrm;
                    strstrm << int64_number;
                    std::string str = strstrm.str();
                    row->Set(col_name, String::NewFromUtf8(isolate, str.c_str(),
                             NewStringType::kNormal, (int)str.length()).ToLocalChecked());
                } else {
                    row->Set(col_name, Number::New(isolate, (double)int64_number));
                }
                break;
            }
            case A_VAL64:
            {
                long long int64_number = *(long long*)(value.buffer);
                if (int64_number > kMaxSafeInteger || int64_number < kMinSafeInteger) {
                    std::ostringstream strstrm;
                    strstrm << int64_number;
                    std::string str = strstrm.str();
                    row->Set(col_name, String::NewFromUtf8(isolate, str.c_str(),
                             NewStringType::kNormal, (int)str.length()).ToLocalChecked());
                } else {
                    row->Set(col_name, Number::New(isolate, static_cast<double>(int64_number)));
                }
                break;
            }
            case A_DOUBLE:
                row->Set(col_name, Number::New(isolate, *(double*)(value.buffer)));
                break;
            case A_FLOAT:
                row->Set(col_name, Number::New(isolate, *(float*)(value.buffer)));
                break;
            case A_BINARY: {
                    MaybeLocal<Object> mbuf = node::Buffer::Copy(
                        isolate, (char *)value.buffer,
                        (int)*(value.length));
                    row->Set(col_name, mbuf.ToLocalChecked());
                }
                break;
            case A_STRING:
                row->Set(col_name, String::NewFromUtf8(isolate,
                         (char *)value.buffer,
                         NewStringType::kNormal,
                         (int)*(value.length)).ToLocalChecked());
                break;
        }
    }

    args.GetReturnValue().Set(row);
}

NODE_API_FUNC(ResultSet::getServerCPUTime)
/*************************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope(isolate);
    ResultSet *rs = ObjectWrap::Unwrap<ResultSet>(args.This());
    dbcapi_i64 cpu_time = api.dbcapi_get_resultset_server_cpu_time(rs->dbcapi_stmt_ptr);
    args.GetReturnValue().Set( Number::New( isolate, (double)cpu_time) );
}

NODE_API_FUNC(ResultSet::getServerMemoryUsage)
/*************************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope(isolate);
    ResultSet *rs = ObjectWrap::Unwrap<ResultSet>(args.This());
    dbcapi_i64 memory_usage = api.dbcapi_get_resultset_server_memory_usage(rs->dbcapi_stmt_ptr);
    args.GetReturnValue().Set( Number::New( isolate, (double)memory_usage) );
}

NODE_API_FUNC(ResultSet::getServerProcessingTime)
/*************************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope(isolate);
    ResultSet *rs = ObjectWrap::Unwrap<ResultSet>(args.This());
    dbcapi_i64 processing_time = api.dbcapi_get_resultset_server_processing_time(rs->dbcapi_stmt_ptr);
    args.GetReturnValue().Set( Number::New( isolate, (double)processing_time) );
}

#if 0
void ResultSet::getString( const FunctionCallbackInfo<Value> &args )
/******************************************************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope( isolate );
    ResultSet *rs = ObjectWrap::Unwrap<ResultSet>( args.This() );
    dbcapi_data_value value;

    getSQLValue( rs, value, args );

    if( *(value.is_null) ) {
	args.GetReturnValue().Set( Null( isolate ) );
	return;
    }

    if( value.type == A_BINARY || value.type == A_STRING ) {
	// Early Exit
	args.GetReturnValue().Set( String::NewFromUtf8( isolate,
							value.buffer,
							NewStringType::kNormal,
							(int)*(value.length) ).ToLocalChecked());
	return;
    }

    std::ostringstream out;

    if( !convertToString( value, out ) ) {
	args.GetReturnValue().SetUndefined();
	return;
    }

    args.GetReturnValue().Set( String::NewFromUtf8( isolate, out.str().c_str() ) );
}

void ResultSet::getDouble( const FunctionCallbackInfo<Value> &args )
/******************************************************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope( isolate );
    ResultSet *rs = ObjectWrap::Unwrap<ResultSet>( args.This() );
    dbcapi_data_value value;
    double retVal;

    getSQLValue( rs, value, args );

    if( *(value.is_null) ) {
	args.GetReturnValue().Set( Null( isolate ) );
	return;
    }

    if( !convertToDouble( value, retVal ) ) {
	args.GetReturnValue().SetUndefined();
	return;
    }

    args.GetReturnValue().Set( Number::New( isolate, retVal ) );
}
#endif

struct getDataBaton {
    Persistent<Function> 	callback;
    bool 			err;
    int                         error_code;
    std::string 		error_msg;
    std::string                 sql_state;
    bool 			callback_required;

    ResultSetPointer		rs;
    int 			retVal;

    int                         column_index;
    int                         data_offset;
    int                         length;
    void                        *buffer;

    getDataBaton() {
        err = false;
        callback_required = false;
    }

    ~getDataBaton() {
        callback.Reset();
    }
};

void ResultSet::getDataAfter(uv_work_t *req)
/****************************************************/
{
    Isolate *isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);
    getDataBaton *baton = static_cast<getDataBaton*>(req->data);
    Local<Value> undef = Local<Value>::New(isolate, Undefined(isolate));

    if (baton->err) {
        callBack(baton->error_code, &(baton->error_msg), &(baton->sql_state),
                 baton->callback, undef, baton->callback_required);
    }
    else {
        Local<Value> retObj = Local<Value>::New(isolate, Integer::New(isolate, baton->retVal));
        callBack(0, NULL, NULL, baton->callback, retObj, baton->callback_required);
    }

    delete baton;
    delete req;
}

void ResultSet::getDataWork(uv_work_t *req)
/***************************************************/
{
    getDataBaton *baton = static_cast<getDataBaton*>(req->data);

    if (isInvalid(baton->rs)) {
        baton->err = true;
        getErrorMsg(JS_ERR_RESULTSET_CLOSED, baton->error_code, baton->error_msg, baton->sql_state);
        return;
    }

    ConnectionLock lock(baton->rs);

    if( !lock.isValid() ) {
        baton->err = true;
        getErrorMsg(JS_ERR_NOT_CONNECTED, baton->error_code, baton->error_msg, baton->sql_state);
        return;
    }

    //dbcapi_data_info	dinfo;
    //bool ret = api.dbcapi_get_data_info(baton->obj->dbcapi_stmt_ptr, 0, &dinfo);

    baton->retVal = api.dbcapi_get_data(baton->rs->dbcapi_stmt_ptr, baton->column_index, baton->data_offset, baton->buffer, baton->length);

    if (baton->retVal == -1) {
        baton->err = true;
        getErrorMsg(baton->rs->stmt->connection->dbcapi_conn_ptr, baton->error_code, baton->error_msg, baton->sql_state);
    }
}

NODE_API_FUNC(ResultSet::getData)
/*******************************/
{
    Isolate *isolate = args.GetIsolate();
    Local<Context> context = isolate->GetCurrentContext();
    HandleScope scope(isolate);
    int cbfunc_arg = -1;

    args.GetReturnValue().SetUndefined();

    // JavaScript Parameters
    // int columnIndex  - zero-based column ordinal.
    // int dataOffset   - index within the LOB column from which to begin the read operation.
    // byte[] buffer    - buffer into which to copy the data.
    // int bufferOffset - index with the buffer to which the data will be copied.
    // int length       - maximum number of bytes/characters to read.
    // function cb      - callback function

    unsigned int expectedTypes[] = { JS_INTEGER, JS_INTEGER, JS_BUFFER, JS_INTEGER, JS_INTEGER, JS_FUNCTION };
    bool isOptional[] = { false, false, false, false, false, true };
    if (!checkParameters(isolate, args, "getData(colIndex, dataOffset, buffer, bufferOffset, length[, callback])",
        6, expectedTypes, &cbfunc_arg, isOptional)) {
        return;
    }
    bool callback_required = (cbfunc_arg == 5);

    int column_index;
    ResultSet *rs = ObjectWrap::Unwrap<ResultSet>(args.This());

    if (isInvalid(rs)) {
        throwError(JS_ERR_RESULTSET_CLOSED);
        return;
    }

    // Check column index
    if (!validate(isolate, rs, args, column_index)) {
        return;
    }

    int data_offset = (args[1]->Int32Value(context)).FromJust();
    int buffer_offset = (args[3]->Int32Value(context)).FromJust();
    int length = (args[4]->Int32Value(context)).FromJust();

    // Check data_offset, buffer_offset, length
    std::string errText;
    if (data_offset < 0) {
        errText = "Invalid dataOffset.";
    } else if (buffer_offset < 0) {
        errText = "Invalid bufferOffset.";
    } else if (length < 0) {
        errText = "Invalid length.";
    }
    if (errText.length() > 0) {
        std::string sqlState = "HY000";
        throwError(JS_ERR_INVALID_ARGUMENTS, errText, sqlState);
        return;
    }

    getDataBaton *baton = new getDataBaton();
    baton->rs = rs;
    baton->callback_required = callback_required;
    baton->column_index = column_index;
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

void ResultSet::isClosed( const FunctionCallbackInfo<Value> &args )
/*****************************************************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope( isolate );

    ResultSet *rs = ObjectWrap::Unwrap<ResultSet>( args.This() );
    args.GetReturnValue().Set( Boolean::New( isolate, rs->is_closing || rs->is_closed ) );
}

struct closeBaton {
    Persistent<Function> 	callback;
    bool 			err;
    int                         error_code;
    std::string 		error_msg;
    std::string                 sql_state;
    bool 			callback_required;
    ResultSetPointer		rs;

    closeBaton() {
        err = false;
        callback_required = false;
    }

    ~closeBaton() {
        callback.Reset();
    }
};

void ResultSet::closeAfter(uv_work_t *req)
/****************************************************/
{
    Isolate *isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);
    closeBaton *baton = static_cast<closeBaton*>(req->data);
    Local<Value> undef = Local<Value>::New(isolate, Undefined(isolate));

    if (baton->rs->stmt) {
        baton->rs->stmt->removeResultSet(baton->rs);
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

void ResultSet::closeWork(uv_work_t *req)
/***************************************/
{
    closeBaton *baton = static_cast<closeBaton*>(req->data);
    ConnectionLock lock(baton->rs);

    if( !lock.isValid() ) {
        baton->err = true;
        getErrorMsg(JS_ERR_NOT_CONNECTED, baton->error_code, baton->error_msg, baton->sql_state);
        return;
    }
    
    if (!baton->rs->is_closing) {
        baton->err = true;
        getErrorMsg(JS_ERR_RESULTSET_CLOSED, baton->error_code, baton->error_msg, baton->sql_state);
        return;
    }

    baton->rs->_close();
}

void ResultSet::_setClosing()
/***************************/
{
    if( is_closing ) {
        return;
    }

    is_closing = true;
    Ref();
}

void ResultSet::_close()
/**********************/
{
    assert(is_closing);

    if (is_closed) {
        return;
    }

    is_closed = true;

    if (dbcapi_stmt_ptr) {
        // dbcapi_stmt_ptr is freed by the Statement
        dbcapi_stmt_ptr = NULL;
    }

    Unref();
}

void ResultSet::close(const FunctionCallbackInfo<Value> &args)
/************************************************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope(isolate);
    int cbfunc_arg = -1;

    args.GetReturnValue().SetUndefined();

    // check parameters
    unsigned int expectedTypes[] = { JS_FUNCTION };
    bool isOptional[] = { true };
    if (!checkParameters(isolate, args, "close([callback])", 1, expectedTypes, &cbfunc_arg, isOptional)) {
        return;
    }
    bool callback_required = (cbfunc_arg >= 0);

    ResultSet *rs = ObjectWrap::Unwrap<ResultSet>(args.This());

    if (rs->is_closed || rs->is_closing) {
        if (callback_required) {
            Local<Value> undef = Local<Value>::New(isolate, Undefined(isolate));
            Local<Function> callback = Local<Function>::Cast(args[cbfunc_arg]);
            callBack(0, NULL, NULL, callback, undef, true);
        }
        return;
    }

    rs->_setClosing();

    closeBaton *baton = new closeBaton();
    baton->rs = rs;
    baton->callback_required = callback_required;

    uv_work_t *req = new uv_work_t();
    req->data = baton;

    if (callback_required) {
        Local<Function> callback = Local<Function>::Cast(args[cbfunc_arg]);
        baton->callback.Reset(isolate, callback);

        int status = uv_queue_work(uv_default_loop(), req, closeWork,
            (uv_after_work_cb)closeAfter);
        assert(status == 0);
        _unused(status);
        return;
    }

    closeWork(req);
    closeAfter(req);
}

struct nextResultBaton {
    Persistent<Function> 	callback;
    bool 			err;
    int                         error_code;
    std::string 		error_msg;
    std::string                 sql_state;
    bool 			callback_required;

    ResultSetPointer		rs;
    bool 			retVal;

    nextResultBaton() {
        err = false;
        callback_required = false;
    }

    ~nextResultBaton() {
        callback.Reset();
    }
};

void ResultSet::nextResultAfter(uv_work_t *req)
/****************************************************/
{
    Isolate *isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);
    nextResultBaton *baton = static_cast<nextResultBaton*>(req->data);
    Local<Value> undef = Local<Value>::New(isolate, Undefined(isolate));

    if (baton->err) {
        callBack(baton->error_code, &(baton->error_msg), &(baton->sql_state),
                 baton->callback, undef, baton->callback_required);
        delete baton;
        delete req;
        return;
    }

    Local<Value> retObj = Local<Value>::New(isolate, Boolean::New(isolate, baton->retVal));
    callBack(0, NULL, NULL, baton->callback, retObj, baton->callback_required);

    delete baton;
    delete req;
}

void ResultSet::nextResultWork(uv_work_t *req)
/***************************************************/
{
    nextResultBaton *baton = static_cast<nextResultBaton*>(req->data);

    if (isInvalid(baton->rs)) {
        baton->err = true;
        getErrorMsg(JS_ERR_RESULTSET_CLOSED, baton->error_code, baton->error_msg, baton->sql_state);
        return;
    }

    ConnectionLock lock(baton->rs);

    if( !lock.isValid() ) {
        baton->err = true;
        getErrorMsg(JS_ERR_NOT_CONNECTED, baton->error_code, baton->error_msg, baton->sql_state);
        return;
    }

    baton->retVal = (api.dbcapi_get_next_result(baton->rs->dbcapi_stmt_ptr) != 0);
    freeColumnInfos(baton->rs->column_infos);
    if (baton->retVal) {
        baton->rs->getColumnInfos();
    }
}

void ResultSet::nextResult(const FunctionCallbackInfo<Value> &args)
/************************************************************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope(isolate);
    int cbfunc_arg = -1;

    args.GetReturnValue().SetUndefined();

    // check parameters
    unsigned int expectedTypes[] = { JS_FUNCTION };
    bool isOptional[] = { true };
    if (!checkParameters(isolate, args, "nextResult([callback])", 1, expectedTypes, &cbfunc_arg, isOptional)) {
        return;
    }
    bool callback_required = (cbfunc_arg >= 0);

    ResultSet *rs = ObjectWrap::Unwrap<ResultSet>(args.This());

    if (isInvalid(rs)) {
        throwError(JS_ERR_RESULTSET_CLOSED);
        return;
    }

    rs->fetched_first = false;

    nextResultBaton *baton = new nextResultBaton();
    baton->rs = rs;
    baton->callback_required = callback_required;

    uv_work_t *req = new uv_work_t();
    req->data = baton;

    if (callback_required) {
        Local<Function> callback = Local<Function>::Cast(args[cbfunc_arg]);
        baton->callback.Reset(isolate, callback);

        int status = uv_queue_work(uv_default_loop(), req, nextResultWork,
                                   (uv_after_work_cb)nextResultAfter);
        assert(status == 0);
        _unused(status);

        args.GetReturnValue().SetUndefined();
        return;
    }

    nextResultWork(req);
    bool retVal = baton->retVal;
    nextResultAfter(req);

    args.GetReturnValue().Set(Boolean::New(isolate, retVal));
}

void ResultSet::New( const FunctionCallbackInfo<Value> &args )
/************************************************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope( isolate );
    ResultSet* rs = new ResultSet();
    rs->Wrap( args.This() );
    args.GetReturnValue().Set( args.This() );
}

void ResultSet::NewInstance( const FunctionCallbackInfo<Value> &args )
/********************************************************************/
{
    Persistent<Object> obj;
    CreateNewInstance( args, obj );
    args.GetReturnValue().Set( obj );
}

void ResultSet::CreateNewInstance(
    const FunctionCallbackInfo<Value> &	args,
    Persistent<Object> &		obj )
/*******************************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope( isolate );

    const unsigned argc = 1;
    Local<Value> argv[argc] = { args[0] };
    Local<Context> context = isolate->GetCurrentContext();
    Local<Function> cons = Local<Function>::New( isolate, constructor );
    Local<Object> instance = cons->NewInstance(context, argc, argv).ToLocalChecked();
    obj.Reset(isolate, instance);
}

void ResultSet::getValueLength(const FunctionCallbackInfo<Value> &args)
/*********************************************************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope(isolate);

    args.GetReturnValue().SetUndefined();

    // check parameters
    unsigned int expectedTypes[] = { JS_INTEGER };
    if (!checkParameters(isolate, args, "getValueLength(colIndex)", 1, expectedTypes)) {
        return;
    }

    ResultSet *rs = ObjectWrap::Unwrap<ResultSet>(args.This());

    if (isInvalid(rs)) {
        throwError(JS_ERR_RESULTSET_CLOSED);
        return;
    }

    dbcapi_data_info dataInfo;
    if (getDataInfo(isolate, rs, dataInfo, args)) {
        args.GetReturnValue().Set(Integer::New(isolate, (int)(dataInfo.data_size)));
    }
}

void ResultSet::isNull(const FunctionCallbackInfo<Value> &args)
/*********************************************************************/
{
    Isolate *isolate = args.GetIsolate();
    HandleScope scope(isolate);

    args.GetReturnValue().SetUndefined();

    // check parameters
    unsigned int expectedTypes[] = { JS_INTEGER };
    if (!checkParameters(isolate, args, "isNull(colIndex)", 1, expectedTypes)) {
        return;
    }

    ResultSet *rs = ObjectWrap::Unwrap<ResultSet>(args.This());

    if (isInvalid(rs)) {
        throwError(JS_ERR_RESULTSET_CLOSED);
        return;
    }

    dbcapi_data_info dataInfo;
    if (getDataInfo(isolate, rs, dataInfo, args)) {
        args.GetReturnValue().Set(Boolean::New(isolate, (dataInfo.is_null != 0)));
    }
}

bool ResultSet::getDataInfo(Isolate *                   isolate,
                            ResultSet *			rs,
                            dbcapi_data_info &		dataInfo,
                            const FunctionCallbackInfo<Value> &args)
/********************************************************************/
{
    Local<Context> context = isolate->GetCurrentContext();
    ConnectionLock lock(rs);

    if (!lock.isValid()) {
        throwError(JS_ERR_NOT_CONNECTED);
        return false;
    }

    int colIndex = (args[0]->Int32Value(context)).FromJust();
    if (!validate(isolate, rs, args, colIndex)) {
        // throwError handled by validate() function
        return false;
    }

    memset(&dataInfo, 0, sizeof(dbcapi_data_info));
    if (!api.dbcapi_get_data_info(rs->dbcapi_stmt_ptr, colIndex, &dataInfo)) {
        throwError(rs->stmt->connection->dbcapi_conn_ptr);
        return false;
    }

    return true;
}
