// ***************************************************************************
// Copyright (c) 2016 SAP SE or an SAP affiliate company. All rights reserved.
// ***************************************************************************
using namespace v8;
using namespace node;

#include "hana_node.h"
#include "nodever_cover.h"
#include <list>

class Connection;
class Statement;

struct warningCallbackBaton {
    Persistent<Function> 	callback;
    bool                        callback_required;
    bool 			err;
    int                         error_code;
    std::string 		error_msg;
    std::string                 sql_state;
    ConnectionPointer 	       	conn;

    warningCallbackBaton() {
        err = false;
        callback_required = true;
    }

    ~warningCallbackBaton() {
        callback.Reset();
    }
};

struct warningMessage {
    int             error_code;
    std::string*    message;
    std::string*    sql_state;

    warningMessage( dbcapi_i32 errorCode, const char *warning, const char *sqlState ) {
        error_code = errorCode;
        message = new std::string( warning );
        sql_state = new std::string( sqlState );
    }

    ~warningMessage() {
        if ( message != NULL ) {
            delete message;
        }
        if ( sql_state != NULL ) {
            delete sql_state;
        }
    }
};

/** Represents the connection to the database.
 * @class Connection
 *
 * The following example uses synchronous calls to create a new connection to
 * the database server, issue a SQL query against the server, display the
 * result set, and then disconnect from the server.
 *
 * <p><pre>
 * var hana = require( '@sap/hana-client' );
 * var client = hana.createConnection();
 * client.connect( { serverNode: 'myserver:30015', UserID: 'system', Password: 'manager' } )
 * console.log('Connected');
 * result = client.exec("SELECT * FROM Customers");
 * console.log( result );
 * client.disconnect()
 * console.log('Disconnected');
 * </pre></p>
 *
 * The following example does essentially the same thing using callbacks
 * to perform asynchronous calls.
 * Error checking is included.
 *
 * <p><pre>
 * var hana = require( '@sap/hana-client' );
 * var client = hana.createConnection();
 * client.connect( "serverNode=myserver:30015;uid=system;pwd=manager",
 *     function( err )
 *     {
 *         if( err )
 *         {
 *             console.error( "Connect error: ", err );
 *         }
 *         else
 *         {
 *             console.log( "Connected" )
 *
 *             client.exec( "SELECT * FROM Customers",
 *                 function( err, rows )
 *                 {
 *                     if( err )
 *                     {
 *                         console.error( "Error: ", err );
 *                     }
 *                     else
 *                     {
 *                         console.log(rows)
 *                     }
 *                 }
 *             );
 *
 *             client.disconnect(
 *                 function( err )
 *                 {
 *                     if( err )
 *                     {
 *                         console.error( "Disconnect error: ", err );
 *                     }
 *                     else
 *                     {
 *                         console.log( "Disconnected" )
 *                     }
 *                 }
 *             );
 *         }
 *     }
 * );
 * </pre></p>
 *
 * The following example also uses callbacks but the functions
 * are not inlined and the code is easier to understand.
 * <p><pre>
 * var hana = require( '@sap/hana-client' );
 * var client = hana.createConnection();
 * client.connect( "serverNode=myserver:30015;uid=system;pwd=manager", async_connect );
 *
 * function async_connect( err )
 * {
 *     if( err )
 *     {
 * 	console.error( "Connect error: ", err );
 *     }
 *     else
 *     {
 * 	console.log( "Connected" )
 *
 * 	client.exec( "SELECT * FROM Customers", async_results );
 *
 * 	client.disconnect( async_disco );
 *     }
 * }
 *
 * function async_results( err, rows )
 * {
 *     if( err )
 *     {
 * 	console.error( "Error: ", err );
 *     }
 *     else
 *     {
 * 	console.log(rows)
 *     }
 * }
 *
 * function async_disco( err )
 * {
 *     if( err )
 *     {
 * 	console.error( "Disconnect error: ", err );
 *     }
 *     else
 *     {
 * 	console.log( "Disconnected" )
 *     }
 * }
 * </pre></p>
 *
 * You can also pass connection parameters into the createConnection function,
 * and those parameters are combined with those in the connect() function call
 * to get the connection string used for the connection. You can use a hash of
 * connection parameters or a connection string fragment in either call.
 * <p><pre>
 * var hana = require( '@sap/hana-client' );
 * var client = hana.createConnection( { uid: 'system'; pwd: 'manager' } );
 * client.connect( 'serverNode=myserver:30015;uid=system;pwd=manager' );
 * // the connection string that will be used is
 * // "uid=system;pwd=manager;serverNode=myserver:30015"
 * </pre></p>
 */
class Connection : public ObjectWrap
{
    friend class Statement;
    friend class RefPointer<Connection>;
  public:
    /// @internal
    static void Init( Isolate * );

    /// @internal
    static NODE_API_FUNC( NewInstance );

    void Release() {
        this->Unref();
    }

  private:
    /// @internal
    // NOT THREAD-SAFE; DO NOT CALL FROM WORKER THREAD OR WHILE DISCONNECTING
    void addStatement( Statement * );

    /// @internal
    // NOT THREAD-SAFE; DO NOT CALL FROM WORKER THREAD UNLESS DISCONNECTING
    void removeStatement( Statement * );

    /// @internal
    Connection( const FunctionCallbackInfo<Value> &args );
    /// @internal
    ~Connection();

    /// @internal
    void getConnectionProperties();
    /// @internal
    void checkProperty(char * keyString, char * valueString);
    /// @internal
    bool searchAttribute(const char * connStr, size_t & attrLen, char * keyStr, char * valueStr);
    /// @internal
    void removeLeadingBlanks(char * string);
    /// @internal
    void removeTrailingBlanks(char * string);
    /// @internal
    void removeCurlyBrackets(char * string);
    /// @internal
    void toUpper(char * string);

    /// @internal
    static Persistent<Function> constructor;

    /// @internal
    static void noParamAfter( uv_work_t *req );
    /// @internal
    static void connectAfter( uv_work_t *req );
    /// @internal
    static void connectWork( uv_work_t *req );
    /// @internal
    static NODE_API_FUNC( New );

    /// Connect using an existing connection.
    //
    // This method connects to the database using an existing connection.
    // The DBCAPI connection handle obtained from the JavaScript External
    // environment needs to be passed in as a parameter. The disconnect
    // method should be called before the end of the program to free
    // up resources.
    //
    // @fn Connection::connect( Number DBCAPI_Handle, Function callback )
    //
    // @param DBCAPI_Handle The connection Handle ( type: Number )
    // @param callback The optional callback function. ( type: Function )
    //
    // <p><pre>
    // // In a method written for the JavaScript External Environment
    // var hana = require( '@sap/hana-client' );
    // var client = hana.createConnection();
    // client.connect( dbcapi_handle, callback );
    // </pre></p>
    //
    // This method can be either synchronous or asynchronous depending on
    // whether or not a callback function is specified.
    // The callback function is of the form:
    //
    // <p><pre>
    // function( err )
    // {
    //
    // };
    // </pre></p>
    //
    // @internal
    ///

    /** Creates a new connection.
     *
     * This method creates a new connection using either a connection string
     * or a hash of connection parameters passed in as a parameter. Before
     * the end of the program, the connection should be disconnected using
     * the disconnect method to free up resources.
     *
     * The CharSet (CS) connection parameter CS=UTF-8 is always appended
     * to the end of the connection string by the driver since it is
     * required that all strings are sent in that encoding.
     *
     * This method can be either synchronous or asynchronous depending on
     * whether or not a callback function is specified.
     * The callback function is of the form:
     *
     * <p><pre>
     * function( err )
     * {
     *
     * };
     * </pre></p>
     *
     * The following synchronous example shows how to use the connect method.
     * It is not necessary to specify the CHARSET=UTF-8 connection parameter
     * since it is always added automatically.
     *
     * <p><pre>
     * var hana = require( '@sap/hana-client' );
     * var client = hana.createConnection();
     * client.connect( "serverNode=myserver:30015;uid=system;pwd=manager" );
     * </pre></p>
     *
     * @fn Connection::connect( String conn_string, Function callback )
     *
     * @param conn_string A valid connection string ( type: String )
     * @param callback The optional callback function. ( type: Function )
     *
     * @see Connection::disconnect
     */
    static NODE_API_FUNC( connect );

    /** Closes the current connection.
     *
     * This method closes the current connection and should be
     * called before the program ends to free up resources.
     *
     * This method can be either synchronous or asynchronous depending on
     * whether or not a callback function is specified.
     * The callback function is of the form:
     *
     * <p><pre>
     * function( err )
     * {
     *
     * };
     * </pre></p>
     *
     * The following synchronous example shows how to use the disconnect method.
     *
     * <p><pre>
     * var hana = require( '@sap/hana-client' );
     * var client = hana.createConnection();
     * client.connect( "serverNode=myserver:30015;uid=system;pwd=manager" );
     * client.disconnect()
     * </pre></p>
     *
     * @fn Connection::disconnect( Function callback )
     *
     * @param callback The optional callback function. ( type: Function )
     *
     * @see Connection::connect
     */
    static NODE_API_FUNC( disconnect );

    /// @internal
    static void disconnectWork( uv_work_t *req );

    /// @internal
    static void freeAfter(uv_work_t *req);
    /// @internal
    static void freeWork(uv_work_t *req);

    /** Executes the specified SQL statement.
     *
     * This method takes in a SQL statement and an optional array of bind
     * parameters to execute.
     *
     * This method can be either synchronous or asynchronous depending on
     * whether or not a callback function is specified.
     * The callback function is of the form:
     *
     * <p><pre>
     * function( err, result )
     * {
     *
     * };
     * </pre></p>
     *
     * For queries producing result sets, the result set object is returned
     * as the second parameter of the callback.
     * For insert, update and delete statements, the number of rows affected
     * is returned as the second parameter of the callback.
     * For other statements, result is undefined.
     *
     * The following synchronous example shows how to use the exec method.
     *
     * <p><pre>
     * var hana = require( '@sap/hana-client' );
     * var client = hana.createConnection();
     * client.connect( "serverNode=myserver:30015;uid=system;pwd=manager" );
     * result = client.exec("SELECT * FROM Customers");
     * console.log( result );
     * client.disconnect()
     * </pre></p>
     *
     * The following synchronous example shows how to specify bind parameters.
     *
     * <p><pre>
     * var hana = require( '@sap/hana-client' );
     * var client = hana.createConnection();
     * client.connect( "serverNode=myserver:30015;uid=system;pwd=manager" );
     * result = client.exec(
     *     "SELECT * FROM Customers WHERE ID >=? AND ID <?",
     *     [300, 400] );
     * console.log( result );
     * client.disconnect()
     * </pre></p>
     *
     * @fn Result Connection::exec( String sql, Array params, Function callback )
     *
     * @param sql The SQL statement to be executed. ( type: String )
     * @param params Optional array of bind parameters. ( type: Array )
     * @param callback The optional callback function. ( type: Function )
     *
     * @return If no callback is specified, the result is returned.
     *
     */
    static NODE_API_FUNC( exec );

    /** Prepares the specified SQL statement.
     *
     * This method prepares a SQL statement and returns a Statement object
     * if successful.
     *
     * This method can be either synchronous or asynchronous depending on
     * whether or not a callback function is specified.
     * The callback function is of the form:
     *
     * <p><pre>
     * function( err, Statement )
     * {
     *
     * };
     * </pre></p>
     *
     * The following synchronous example shows how to use the prepare method.
     *
     * <p><pre>
     * var hana = require( '@sap/hana-client' );
     * var client = hana.createConnection();
     * client.connect( "serverNode=myserver:30015;uid=system;pwd=manager" )
     * stmt = client.prepare( "SELECT * FROM Customers WHERE ID >= ? AND ID < ?" );
     * result = stmt.exec( [200, 300] );
     * console.log( result );
     * client.disconnect();
     * </pre></p>
     *
     * @fn Statement Connection::prepare( String sql, Function callback )
     *
     * @param sql The SQL statement to be executed. ( type: String )
     * @param callback The optional callback function. ( type: Function )
     *
     * @return If no callback is specified, a Statement object is returned.
     *
     */
    static NODE_API_FUNC( prepare );

    /// @internal
    static void prepareAfter( uv_work_t *req );
    /// @internal
    static void prepareWork( uv_work_t *req );

    /** Performs a commit on the connection.
     *
     * This method performs a commit on the connection.
     * By default, inserts, updates, and deletes are not committed
     * upon disconnection from the database server.
     *
     * This method can be either synchronous or asynchronous depending on
     * whether or not a callback function is specified.
     * The callback function is of the form:
     *
     * <p><pre>
     * function( err ) {
     *
     * };
     * </pre></p>
     *
     * The following synchronous example shows how to use the commit method.
     *
     * <p><pre>
     * var hana = require( '@sap/hana-client' );
     * var client = hana.createConnection();
     * client.connect( "serverNode=myserver:30015;uid=system;pwd=manager" )
     * stmt = client.prepare(
     *     "INSERT INTO Departments "
     *     + "( DepartmentID, DepartmentName, DepartmentHeadID )"
     *     + "VALUES (?,?,?)" );
     * result = stmt.exec( [600, 'Eastern Sales', 902] );
     * result += stmt.exec( [700, 'Western Sales', 902] );
     * stmt.drop();
     * console.log( "Number of rows added: " + result );
     * result = client.exec( "SELECT * FROM Departments" );
     * console.log( result );
     * client.commit();
     * client.disconnect();
     * </pre></p>
     *
     * @fn Connection::commit( Function callback )
     *
     * @param callback The optional callback function. ( type: Function )
     *
     */
    static NODE_API_FUNC( commit );

    /// @internal
    static void commitWork( uv_work_t *req );

    /** Performs a rollback on the connection.
     *
     * This method performs a rollback on the connection.
     *
     * This method can be either synchronous or asynchronous depending on
     * whether or not a callback function is specified.
     * The callback function is of the form:
     *
     * <p><pre>
     * function( err ) {
     *
     * };
     * </pre></p>
     *
     * The following synchronous example shows how to use the rollback method.
     *
     * <p><pre>
     * var hana = require( '@sap/hana-client' );
     * var client = hana.createConnection();
     * client.connect( "serverNode=myserver:30015;uid=system;pwd=manager" )
     * stmt = client.prepare(
     *     "INSERT INTO Departments "
     *     + "( DepartmentID, DepartmentName, DepartmentHeadID )"
     *     + "VALUES (?,?,?)" );
     * result = stmt.exec( [600, 'Eastern Sales', 902] );
     * result += stmt.exec( [700, 'Western Sales', 902] );
     * stmt.drop();
     * console.log( "Number of rows added: " + result );
     * result = client.exec( "SELECT * FROM Departments" );
     * console.log( result );
     * client.rollback();
     * client.disconnect();
     * </pre></p>
     *
     * @fn Connection::rollback( Function callback )
     *
     * @param callback The optional callback function. ( type: Function )
     *
     */
    static NODE_API_FUNC( rollback );

    /// @internal
    static void rollbackWork( uv_work_t *req );

    /** Retrieve the current setting of the autocommit from the connection.
     *
     * If autocommit is enabled, a commit is executed following
     * every execute() call.
     *
     * @fn Connection::setAutoCommit( Boolean flag )
     *
     * @param flag Value to enable (1) or disable (0) autocommit. ( type: Boolean )
     *
     */
    static NODE_API_FUNC( getAutoCommit );

    /** Changes the autocommit setting on the connection.
     *
     * If autocommit is enabled, a commit is executed following
     * every execute() call.
     *
     * @fn Connection::setAutoCommit( Boolean flag )
     *
     * @param flag Value to enable (1) or disable (0) autocommit. ( type: Boolean )
     *
     */
    static NODE_API_FUNC( setAutoCommit );

    /** Sets a client info property on the connection.
    *
    * @fn Connection::setClientInfo( String key, String value )
    *
    * @param key The property to be set. ( type: String )
    * @param value The property value to be set. ( type: String )
    *
    */
    static NODE_API_FUNC(setClientInfo);

    /** Retrieves a client info property on the connection.
    *
    * @fn Connection::getClientInfo( String key )
    *
    * @param key The property name to be requested. ( type: String )
    *
    * @return The value found in the property set. ( type: String )
    *
    */
    static NODE_API_FUNC(getClientInfo);

    /** Sets a callback function for warnings.
    *
    * @fn Connection::setWarningCallback( Function callback )
    *
    * @param callback The callback function. ( type: Function )
    *
    */
    static NODE_API_FUNC(setWarningCallback);

    /** Gets the warnings collected by the connection.
    *
    * @fn Array Connection::getWarnings()
    *
    * @return Returns an array of Objects. Each Object contains properties (code, message, sqlState)
    * of each warning. ( type: Array )
    *
    * After calling this function, the connection will empty the warning list.
    */
    static NODE_API_FUNC(getWarnings);

    /** Sets the desired rowset size used by Connection.execute and Statement.execute.
    *
    * The default rowset size is 1. Setting the rowset size can improve the fetching performance.
    *
    * @fn Connection::setRowSetSize( Integer rowsetSize )
    *
    * @param rowsetSize The rowset size. ( type: Integer )
    *
    */
    static NODE_API_FUNC(setRowSetSize);

    /** Retrieves a value indicates the state of the connection.
    *
    * @fn Connection::state()
    *
    * @return The value indicates the state of the connection. ( type: String )
    *
    */
    static NODE_API_FUNC(state);

    /** Empties the connection pool.
    *
    * This method Empties the connection pool.
    *
    * This method can be either synchronous or asynchronous depending on
    * whether or not a callback function is specified.
    * The callback function is of the form:
    *
    * <p><pre>
    * function( err ) {
    *
    * };
    * </pre></p>
    * @fn Connection::clearPool( Function callback )
    *
    * @param callback The optional callback function. ( type: Function )
    *
    */
    static NODE_API_FUNC(clearPool);

    /// @internal
    static void clearPoolWork(uv_work_t *req);
    /// @internal
    static void clearPoolAfter(uv_work_t *req);

    /** Aborts the running database request that being executed on the connection.
    *
    * @fn Connection::abort( Function callback )
    *
    * @param callback The optional callback function. ( type: Function )
    *
    */
    static NODE_API_FUNC(abort);

    /// @internal
    static void abortWork(uv_work_t *req);

  public:
    /// @internal
    static void warningCallbackAfter(uv_work_t *req);
    /// @internal
    static void warningCallbackWork(uv_work_t *req);

    /// @internal
    dbcapi_connection	*dbcapi_conn_ptr;
    /// @internal
    unsigned int	max_api_ver;
    /// @internal
    bool 		external_connection;
    /// @internal
    bool		autoCommit;
    /// @internal
    uv_mutex_t 		conn_mutex;
    /// @internal
    uv_mutex_t 		warning_mutex;
    /// @internal
    Persistent<String>	_arg;
    /// @internal
    Persistent<Function>  warningCallback;
    /// @internal
    std::vector<warningMessage*> warnings;
    /// @internal
    Isolate             *isolate;
    /// @internal
    bool 		is_connected;
    /// @internal
    bool 		is_disconnected;
    /// @internal
    bool 		is_disconnecting;
    /// @internal
    std::string 	conn_string;
    /// @internal
    bool                is_pooled;
    /// @internal
    int                 isolation_level;
    /// @internal
    std::string         locale;
    /// @internal
    std::string         client;
    /// @internal
    std::string         current_schema;
    /// @internal
    std::vector<std::string*> client_infos;
    /// @internal
    std::vector<std::string*> conn_prop_keys;
    /// @internal
    std::vector<std::string*> conn_prop_values;
    /// @internal
    bool                      use_props_to_connect;
    /// @internal
    int                 row_set_size;

    /// @internal
    std::list<Statement*> statements;
};

struct PooledConnection
{
    std::string*        conn_string;
    dbcapi_connection*  dbcapi_conn;
    PooledConnection*   next;

    PooledConnection(std::string & connStr, dbcapi_connection* dbcapiConn, PooledConnection* nextConn) {
        conn_string = new std::string(connStr.c_str());
        dbcapi_conn = dbcapiConn;
        next = nextConn;
    }

    ~PooledConnection() {
        delete conn_string;
    }
};

class ConnectionPoolManager
{
public:
    /// @internal
    ConnectionPoolManager() {
        uv_mutex_init(&conn_pool_mutex);
        connections = NULL;
    }

    /// @internal
    ~ConnectionPoolManager() {
        int count = 0;
        clearPool(count);
    }

    /// @internal
    void add(std::string & connStr, dbcapi_connection* conn);

    /// @internal
    dbcapi_connection* allocate(std::string & connStr);

    /// @internal
    bool clearPool(int & count);

    /// @internal
    void getError(int& errCode, std::string& errText, std::string& sqlState) {
        errCode = error_code;
        errText = error_msg;
        sqlState = sql_state;
    }

private:
    PooledConnection*   connections;
    uv_mutex_t          conn_pool_mutex;
    bool 		err;
    int                 error_code;
    std::string 	error_msg;
    std::string         sql_state;
};
