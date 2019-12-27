// ***************************************************************************
// Copyright (c) 2016 SAP SE or an SAP affiliate company. All rights reserved.
// ***************************************************************************
#include "nodever_cover.h"
#include "hana_utils.h"

using namespace v8;

class TempStmt {
public:
    TempStmt( executeBaton *baton )
        : baton( baton )
        {}
    ~TempStmt() {
        if( baton->dbcapi_stmt_ptr != NULL && baton->prepared_stmt && baton->del_stmt_ptr ) {
            api.dbcapi_free_stmt( baton->dbcapi_stmt_ptr );
            baton->dbcapi_stmt_ptr = NULL;
        }
    }

private:
    executeBaton *baton;
};

void executeWork( uv_work_t *req )
/********************************/
{
    executeBaton *baton = static_cast<executeBaton*>(req->data);
    ConnectionLock lock(baton->conn);
    if( !lock.isValid() ) {
        baton->err = true;
        getErrorMsg(JS_ERR_NOT_CONNECTED, baton->error_code, baton->error_msg, baton->sql_state);
        return;
    }

    TempStmt tempStmt( baton );
    if( baton->dbcapi_stmt_ptr == NULL && baton->stmt_str.length() > 0 ) {
	baton->dbcapi_stmt_ptr = api.dbcapi_prepare( baton->conn->dbcapi_conn_ptr, baton->stmt_str.c_str() );
	if( baton->dbcapi_stmt_ptr == NULL ) {
	    baton->err = true;
	    getErrorMsg( baton->conn->dbcapi_conn_ptr, baton->error_code, baton->error_msg, baton->sql_state );
	    return;
	}
	baton->prepared_stmt = true;
    } else if( baton->dbcapi_stmt_ptr == NULL ) {
	baton->err = true;
	getErrorMsg( JS_ERR_INVALID_OBJECT, baton->error_code, baton->error_msg, baton->sql_state );
	return;
    }

    if( !api.dbcapi_reset( baton->dbcapi_stmt_ptr) ) {
	baton->err = true;
	getErrorMsg( baton->conn->dbcapi_conn_ptr, baton->error_code, baton->error_msg, baton->sql_state );
	return;
    }

    if (!checkParameterCount(baton->error_code, baton->error_msg, baton->sql_state,
        baton->provided_params, baton->dbcapi_stmt_ptr)) {
        baton->err = true;
        return;
    }

    int invalidParam = getBindParameters(baton->provided_params, baton->params, baton->dbcapi_stmt_ptr);
    if (invalidParam >= 0) {
        getErrorMsgInvalidParam(baton->error_code, baton->error_msg, baton->sql_state, invalidParam);
        baton->err = true;
        return;
    }

    bool sendParamData = false;
    if (!bindParameters(baton->conn->dbcapi_conn_ptr, baton->dbcapi_stmt_ptr, baton->params,
        baton->error_code, baton->error_msg, baton->sql_state, sendParamData)) {
        baton->err = true;
        return;
    }

    if (sendParamData && baton->stmt) {
        baton->stmt->send_data_total_chunk = 0;
        baton->stmt->send_data_current_chunk = 0;
        baton->send_param_data = true;
        baton->stmt->send_param_data_started.clear();
        int num_params = api.dbcapi_num_params(baton->dbcapi_stmt_ptr);
        baton->stmt->send_param_data_cols = 0;
        baton->stmt->send_param_data_cols_finished = 0;
        for (int i = 0; i < (int)(baton->params.size()); i++) {
            if (baton->params[i]->value.type == A_INVALID_TYPE) { // LOB
                baton->stmt->send_param_data_cols++;
            }
        }
        for (int i = 0; i < num_params; i++) {
            baton->stmt->send_param_data_started.push_back(false);
        }
    }

    dbcapi_bool success_execute = api.dbcapi_execute( baton->dbcapi_stmt_ptr );

    if (success_execute && baton->stmt) {
        copyParameters(baton->stmt->params, baton->params);
    }

    if( !success_execute ) {
	baton->err = true;
	getErrorMsg( baton->conn->dbcapi_conn_ptr, baton->error_code, baton->error_msg, baton->sql_state );
	return;
    }

    baton->function_code = api.dbcapi_get_function_code( baton->dbcapi_stmt_ptr );

    fetchColumnInfos(baton->dbcapi_stmt_ptr, baton->col_infos);

    if( !fetchResultSet( baton->dbcapi_stmt_ptr, baton->conn->row_set_size, baton->rows_affected, baton->col_names,
			 baton->string_vals, baton->int_vals, baton->long_vals, baton->ulong_vals, baton->double_vals,
			 baton->date_vals, baton->time_vals, baton->timestamp_vals,
			 baton->col_types, baton->col_native_types) ) {
	baton->err = true;
	getErrorMsg( baton->conn->dbcapi_conn_ptr, baton->error_code, baton->error_msg, baton->sql_state );
        return;
    }
}

void executeAfter( uv_work_t *req )
/*********************************/
{
    Isolate *isolate = Isolate::GetCurrent();
    HandleScope scope( isolate );
    executeBaton *baton = static_cast<executeBaton*>( req->data );
    Persistent<Value> ResultSet;
    fillResult( baton, ResultSet );
    ResultSet.Reset();

    if (!baton->stmt || !baton->send_param_data) {
        delete baton;
    } else {
        // For sendParameterData, don't delete the baton because
        // the non-lob parameters are still needed.
        if (baton->stmt->execBaton != NULL) {
            delete baton->stmt->execBaton;
        }
        baton->stmt->execBaton = baton;
        baton->conn = NULL; // Unref conn
        baton->stmt = NULL; // Unref stmt
    }

    delete req;
}

bool cleanAPI()
/*************/
{
    if (openConnections == 0) {
        if (api.initialized) {
            api.dbcapi_fini();
            dbcapi_finalize_interface(&api);
            return true;
        }
    }
    return false;
}

void init( Local<Object> exports )
/********************************/
{
    uv_mutex_init(&api_mutex);
    Isolate *isolate = exports->GetIsolate();
    Statement::Init( isolate );
    Connection::Init( isolate );
    ResultSet::Init( isolate );
    NODE_SET_METHOD( exports, "createConnection", Connection::NewInstance );
    NODE_SET_METHOD( exports, "createClient", Connection::NewInstance );

    if (api.initialized == false) {
        scoped_lock api_lock(api_mutex);
        if (api.initialized == false) {
            unsigned int max_api_ver;
            char * env = getenv("DBCAPI_API_DLL");
            if (!dbcapi_initialize_interface(&api, env) ||
                !api.dbcapi_init("Node.js", _DBCAPI_VERSION, &max_api_ver)) {
                std::string sqlState = "HY000";
                std::string errText = "Failed to load DBCAPI.";
                throwError(JS_ERR_INITIALIZING_DBCAPI, errText, sqlState);
            }
        }
    }
}

NODE_MODULE( DRIVER_NAME, init )
