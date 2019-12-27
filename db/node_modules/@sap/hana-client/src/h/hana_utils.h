// ***************************************************************************
// Copyright (c) 2016 SAP SE or an SAP affiliate company. All rights reserved.
// ***************************************************************************
#include <iostream>
#include <locale>
#include <string>
#include <string.h>
#include <sstream>
#include <vector>
#include "DBCAPI_DLL.h"
#include "DBCAPI.h"

#if 0
#if defined(_MSC_VER)
#pragma warning (disable : 4100)
#pragma warning (disable : 4506)
#pragma warning (disable : 4068)
#pragma warning (disable : 4800)
#endif
#endif

#include "node.h"
#include "v8.h"
#include "node_buffer.h"
#include "node_object_wrap.h"
#include "uv.h"

#if 0
#if defined(_MSC_VER)
#pragma warning (default : 4100)
#pragma warning (default : 4506)
#pragma warning (default : 4068)
#pragma warning (default : 4800)
#endif
#endif

#if defined(_WIN32) || defined(_WIN64)
#include <process.h>
#else // defined(__linux__) || defined(__APPLE__)
#include <pthread.h>
#endif

#include "nodever_cover.h"
#include "errors.h"
#include "connection.h"
#include "stmt.h"
#include "resultset.h"

using namespace v8;

extern DBCAPIInterface api;
extern unsigned openConnections;
extern uv_mutex_t api_mutex;
extern ConnectionPoolManager connPoolManager;

// JavaScriptType
#define JS_UNDEFINED           1
#define JS_NULL                2
#define JS_STRING              4
#define JS_BOOLEAN             8
#define JS_NUMBER             16
#define JS_FUNCTION           32
#define JS_ARRAY              64
#define JS_OBJECT            128
#define JS_BUFFER            256
#define JS_SYMBOL            512
#define JS_INTEGER          1024
#define JS_UNKNOWN_TYPE     2048

#define DEFAULT_ROW_SET_SIZE  1

#define DEFAULT_DATE_FORMAT             "YYYY-MM-DD"
#define DEFAULT_TIME_FORMAT             "MM:HM:SS"
#define DEFAULT_TIMESTAMP_FORMAT        "YYYY-MM-DD MM:HM:SS.SSSSSSSSS"

typedef struct a_date {
    char val[sizeof(DEFAULT_DATE_FORMAT) + 1];
    int len;
} a_date;

typedef struct a_time {
    char val[sizeof(DEFAULT_TIME_FORMAT) + 1];
    int len;
} a_time;

typedef struct a_timestamp {
    char val[sizeof(DEFAULT_TIMESTAMP_FORMAT) + 1];
    int len;
} a_timestamp;

enum TimeType {
    T_NONE,
    T_TIMESTAMP,
    T_DATE,
    T_TIME
};

enum NumType {
    NUM_TYPE_DOUBLE,
    NUM_TYPE_LONG
};

#if !defined( _unused )
#define _unused( x ) ((void)x)
#endif

void clearParameter(dbcapi_bind_data* param, bool free);
void clearParameters(std::vector<dbcapi_bind_data*> & params);
void copyParameters(std::vector<dbcapi_bind_data*> & paramsDest,
                    std::vector<dbcapi_bind_data*> & paramsSrc);

int fetchColumnInfos(dbcapi_stmt* dbcapi_stmt_ptr,
                     std::vector<dbcapi_column_info*>& column_infos);
void freeColumnInfos(std::vector<dbcapi_column_info*>& column_infos);

template <class T>
void clearVector(std::vector<T*>& vector)
{
    for (size_t i = 0; i < vector.size(); i++) {
        if (vector[i] != NULL) {
            delete vector[i];
        }
    }
    vector.clear();
}

template <class T>
void clearVector(std::vector<T>& vector)
{
    vector.clear();
}

class scoped_lock
{
    public:
	scoped_lock( uv_mutex_t &mtx ) : _mtx( mtx )
	{
	    uv_mutex_lock( &_mtx );
	}
	~scoped_lock()
	{
	    uv_mutex_unlock( &_mtx );
	}

    private:
	uv_mutex_t &_mtx;
};

class ConnectionLock {
  public:
    ConnectionLock( Connection *conn );
    ConnectionLock( Statement *stmt );
    ConnectionLock( ResultSet *rs );

    bool isValid() const;
  private:
    Connection *conn;
    scoped_lock lock;
};

class dataValueCollection
{
public:
    dataValueCollection()
    {
    }
    ~dataValueCollection()
    {
        for (size_t i = 0; i < vals.size(); i++) {
            if (vals[i]->is_null != NULL)
                delete vals[i]->is_null;
            if (vals[i]->length != NULL)
                delete vals[i]->length;
            if (vals[i]->buffer != NULL)
                delete vals[i]->buffer;
        }
        vals.clear();
    }
    void push_back(dbcapi_data_value* val)
    {
        vals.push_back(val);
    }
    dbcapi_data_value* operator[](const int index)
    {
        return vals[index];
    }

private:
    std::vector<dbcapi_data_value*> vals;
};

struct executeOptions
{
    bool nestTables;
    bool rowsAsArray;

    void init() {
        nestTables = false;
        rowsAsArray = false;
    }
};

struct executeBaton
{
    Persistent<Function>		callback;
    bool 				err;
    int                                 error_code;
    std::string 			error_msg;
    std::string                         sql_state;
    bool 				callback_required;
    bool 				send_param_data;
    bool 				del_stmt_ptr;

    ConnectionPointer                   conn;
    StatementPointer                    stmt;
    dbcapi_stmt 			*dbcapi_stmt_ptr;
    bool				prepared_stmt;
    std::string				stmt_str;
    std::vector<char*> 			string_vals;
    std::vector<int> 			int_vals;
    std::vector<long long> 		long_vals;
    std::vector<unsigned long long> 	ulong_vals;
    std::vector<double> 		double_vals;
    std::vector<a_date> 		date_vals;
    std::vector<a_time> 		time_vals;
    std::vector<a_timestamp> 		timestamp_vals;
    std::vector<dbcapi_bind_data*> 	params;
    std::vector<dbcapi_bind_data*> 	provided_params;

    std::vector<char*> 			col_names;
    int 				rows_affected;
    std::vector<dbcapi_data_type> 	col_types;
    std::vector<dbcapi_native_type> 	col_native_types;
    std::vector<dbcapi_column_info*> 	col_infos;
    int                                 function_code;

    executeOptions                      exec_options;

    executeBaton()
    {
        err = false;
        callback_required = false;
        dbcapi_stmt_ptr = NULL;
        rows_affected = -1;
        prepared_stmt = false;
        send_param_data = false;
        del_stmt_ptr = false;

        exec_options.init();
    }

    ~executeBaton()
    {
        // the Statement will free dbcapi_stmt_ptr
        dbcapi_stmt_ptr = NULL;
        string_vals.clear(); // Allocated memory already freed in getResulySet
        clearVector(col_names);
        clearVector(int_vals);
        clearVector(long_vals);
        clearVector(ulong_vals);
        clearVector(double_vals);
        clearVector(date_vals);
        clearVector(time_vals);
        clearVector(timestamp_vals);
        col_types.clear();
        col_native_types.clear();
        callback.Reset();

        clearParameters(params);
        clearParameters(provided_params);

        freeColumnInfos(col_infos);
    }
};

bool cleanAPI (); // Finalizes the API and frees up resources

void getErrorMsg( int code, int& errCode, std::string& errText, std::string& sqlState );
void getErrorMsg( dbcapi_connection *conn, int& errCode, std::string& errText, std::string& sqlState );
void getErrorMsgBindingParam( int& errCode, std::string& errText, std::string& sqlState, int invalidParam );
void getErrorMsgInvalidParam( int& errCode, std::string& errText, std::string& sqlState, int invalidParam );
void setErrorMsg( Local<Object>& error, int errCode, std::string& errText, std::string& sqlState );
void throwError( int errCode, std::string& errText, std::string& sqlState );
void throwError( int code );
void throwError( dbcapi_connection *conn );
void throwErrorIP( int paramIndex, const char *function, const char *expectedType, const char *receivedType );

unsigned int getJSType( Local<Value> value );
std::string getJSTypeName( unsigned int type );

bool checkParameters( Isolate* isolate,
                      const FunctionCallbackInfo<Value> &args,
                      const char *function,
                      int argCount,
                      unsigned int *expectedType,
                      int *cbFunArg = NULL,
                      bool *isOptional = NULL,
                      bool *foundOptionalArg = NULL );

void callBack( int                      errCode,
               std::string *		errText,
               std::string *            sqlState,
               Persistent<Function> &	callback,
	       Local<Value> &		Result,
	       bool			callback_required,
	       bool			has_result = true );

void callBack( int                      errCode,
               std::string *		errText,
               std::string *            sqlState,
               const Local<Value> &	callback,
	       Local<Value> &		Result,
	       bool			callback_required,
	       bool			has_result = true );

void callBack( int                      errCode,
               std::string *		errText,
               std::string *            sqlState,
               Persistent<Function> &	callback,
	       Persistent<Value> &	Result,
	       bool			callback_required,
	       bool			has_result = true );

int getReturnValue(Isolate *isolate, dbcapi_data_value & value, dbcapi_native_type nativeType, Local<Value> & localValue);

dbcapi_bind_data* getBindParameter( Isolate *isolate, Local<Value> element );

/*bool getBindParameters( std::vector<char*>			&string_params,
		        std::vector<double*>			&num_params,
		        std::vector<int*>			&int_params,
		        std::vector<size_t*>			&string_len,
		        Local<Value> 				arg,
		        std::vector<dbcapi_bind_data>           &params );*/

bool getBindParameters( Isolate *                               isolate,
                        Local<Value> 				arg,
                        int                                     row_param_count,
                        std::vector<dbcapi_bind_data*> 	        &params,
                        std::vector<size_t> &	                buffer_size,
                        std::vector<unsigned int> &             param_types );

int getBindParameters( std::vector<dbcapi_bind_data*> &    inputParams,
                       std::vector<dbcapi_bind_data*> &    params,
                       dbcapi_stmt *                       stmt );

bool getInputParameters( Isolate *                          isolate,
                         Local<Value>                       arg,
                         std::vector<dbcapi_bind_data*> &   params,
                         dbcapi_stmt *                      stmt,
                         int &                              errCode,
                         std::string &                      errText,
                         std::string &                      sqlState );

bool bindParameters( dbcapi_connection *                 conn,
                     dbcapi_stmt *                       stmt,
                     std::vector<dbcapi_bind_data*> &    params,
                     int &                               errCode,
                     std::string &                       errText,
                     std::string &                       sqlState,
                     bool &                              sendParamData);

bool checkParameterCount( int &                             errCode,
                          std::string &                     errText,
                          std::string &                     sqlState,
                          std::vector<dbcapi_bind_data*> &  providedParams,
                          dbcapi_stmt *                     dbcapi_stmt_ptr);

bool fillResult( executeBaton *baton,
                 Persistent<Value> &ResultSet );

bool getResultSet( Persistent<Value> 		&Result
		 , int 				&rows_affected
		 , std::vector<char *> 		&colNames
		 , std::vector<char*> 		&string_vals
		 , std::vector<int> 		&int_vals
		 , std::vector<long long>	&long_vals
		 , std::vector<unsigned long long> &ulong_vals
		 , std::vector<double>	        &double_vals
                 , std::vector<a_date> 		&date_vals
                 , std::vector<a_time> 		&time_vals
                 , std::vector<a_timestamp> 	&timestamp_vals
		 , std::vector<dbcapi_data_type>    &col_types
                 , std::vector<dbcapi_native_type>  &col_native_types
                 , executeBaton *                   baton );

bool fetchResultSet( dbcapi_stmt 			*dbcapi_stmt_ptr
		   , int 				fetch_size
		   , int 				&rows_affected
		   , std::vector<char *> 		&colNames
		   , std::vector<char*> 		&string_vals
		   , std::vector<int> 			&int_vals
                   , std::vector<long long> 		&long_vals
                   , std::vector<unsigned long long> 	&ulong_vals
                   , std::vector<double> 		&double_vals
                   , std::vector<a_date> 		&date_vals
                   , std::vector<a_time> 		&time_vals
                   , std::vector<a_timestamp> 		&timestamp_vals
		   , std::vector<dbcapi_data_type> 	&col_types
                   , std::vector<dbcapi_native_type> 	&col_native_types );

struct noParamBaton {
    Persistent<Function> 	callback;
    bool 			err;
    int                         error_code;
    std::string 		error_msg;
    std::string                 sql_state;
    bool 			callback_required;

    ConnectionPointer conn;

    noParamBaton() {
	err = false;
    }

    ~noParamBaton() {
        callback.Reset();
    }
};

void executeAfter( uv_work_t *req );
void executeWork( uv_work_t *req );

bool compareString( const std::string &str1, const std::string &str2, bool caseSensitive );
bool compareString( const std::string &str1, const char* str2, bool caseSensitive );

int getHostsString(Isolate* isolate, Local<Array> arHosts, std::string& strHosts);
int getString(Isolate* isolate, Local<Value> value, std::string& str);
void getConnectionString(Isolate* isolate,
                         Local<Object> obj, Persistent<String> & ret,
                         std::vector<std::string*> & conn_prop_keys,
                         std::vector<std::string*> & conn_prop_values);

inline std::string convertToString(Isolate *isolate, Local<String> & str)
{
    if (str.IsEmpty()) {
        return std::string("");
    } else {
#if NODE_MAJOR_VERSION >= 12
        String::Utf8Value utf8(isolate, str);
#else
        String::Utf8Value utf8(str);
#endif
        return std::string(*utf8);
    }
}

bool convertToBool(Isolate *isolate, Local<Value> val, bool &out);

bool convertToString(dbcapi_data_value value, std::ostringstream &out,
                     bool do_throw_error = true);
bool convertToDouble(dbcapi_data_value value, double &number,
                     bool do_throw_error = true);
bool convertToInt(dbcapi_data_value value, int &number,
                  bool do_throw_error = true);

void DatetoTimestamp(const Local<Value> &val, std::ostringstream &out,
                     TimeType type);
bool StringtoDate(const char *str, Local<Value> &val, dbcapi_connection *conn);

const char* getTypeName(dbcapi_data_type type);
const char* getNativeTypeName(dbcapi_native_type nativeType);

void setColumnInfo(Isolate*             isolate,
                   Local<Object>&       object,
                   dbcapi_column_info*  columnInfo);

bool getExecuteOptions(Isolate* isolate, MaybeLocal<Object> mbObj, executeOptions* options);

char* copyString(const char* src);
