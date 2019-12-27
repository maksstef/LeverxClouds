// ***************************************************************************
// Copyright (c) 2016 SAP SE or an SAP affiliate company. All rights reserved.
// ***************************************************************************

#include "nodever_cover.h"

/** A callable statement object.
 *
 * @class ResultSet
 *
 * <p><pre>
 * var hana = require( '@sap/hana-client' );
 * var conn = hana.createConnection();
 * conn.connect( "serverNode=myserver:30015;uid=system;pwd=manager" );
 *
 * var stmt = conn.prepareStatement( "SELECT * FROM Customers" );
 * stmt.execute();
 * var result = stmt.getResultSet();
 * while ( result.next() )
 * {
 *     console.log( result.getInteger(1) +
 * 	    " " + result.getString(2) );
 * }
 * stmt.close();
 *
 * conn.close();
 * </pre></p>
 */
class ResultSet : public node::ObjectWrap
{
    friend class RefPointer<ResultSet>;
    friend class Statement;
  public:
    /// @internal
    static void Init( Isolate * );
    /// @internal
    static NODE_API_FUNC( NewInstance );
    /// @internal
    ResultSet();
    /// @internal
    ~ResultSet();

    static void CreateNewInstance( const FunctionCallbackInfo<Value> &args,
				   Persistent<Object> &ret );

  private:
    /// @internal
    // ONLY CALL FROM MAIN THREAD
    void _setClosing();

    /// @internal
    void _close();
    
    /// @internal
    static bool validate( Isolate *isolate,
                          ResultSet *obj,
                          const FunctionCallbackInfo<Value> &args,
                          int &colIndex);

    /// @internal
    static bool checkColumnIndex( Isolate *isolate,
                                  ResultSet *obj,
                                  const FunctionCallbackInfo<Value> &args,
                                  int &colIndex);
    /// @internal
    static bool isInvalid( ResultSet *obj );
    /// @internal
    static bool getSQLValue( Isolate *isolate, ResultSet *obj, dbcapi_data_value &value,
			     const FunctionCallbackInfo<Value> &args );
    /// @internal
    static bool getDataInfo( Isolate *isolate, ResultSet *obj, dbcapi_data_info &data_info,
                             const FunctionCallbackInfo<Value> &args );
    /// @internal
    static Persistent<Function> constructor;
    /// @internal
    static NODE_API_FUNC( New );

    /** This method checks if the result set is closed.
     *
     * @fn Boolean ResultSet::isClosed()
     *
     * @return Returns true if closed, false otherwise. ( type: Boolean )
     */
    static NODE_API_FUNC( isClosed );

    /** This method closes the ResultSet object and frees up resources.
     *
     * This method supports asynchronous callbacks.
     *
     * @fn ResultSet::close()
     */
    static NODE_API_FUNC( close );
    /// @internal
    static void closeAfter(uv_work_t *req);
    /// @internal
    static void closeWork(uv_work_t *req);

    /// @internal
    static void freeAfter(uv_work_t *req);
    /// @internal
    static void freeWork(uv_work_t *req);

    /** Fetches the next row of the result set.
     *
     * This method attempts to fetch the next row of the result set,
     * returning true when successful and false otherwise.
     *
     * This method supports asynchronous callbacks.
     *
     * @fn Boolean ResultSet::next()
     *
     * @return Returns true on success. ( type: Boolean )
     */
    static NODE_API_FUNC( next );
    /// @internal
    static void nextAfter( uv_work_t *req );
    /// @internal
    static void nextWork( uv_work_t *req );

    /** Gets the next result set.
    *
    * This method checks to see if there are more result sets and fetches
    * the next available result set.
    *
    * This method supports asynchronous callbacks.
    *
    * @fn Boolean ResultSet::nextResult()
    *
    * @return Returns true if there are more result sets and false
    *         otherwise. ( type: Boolean )
    *
    */
    static NODE_API_FUNC(nextResult);

    /// @internal
    static void nextResultAfter(uv_work_t *req);
    /// @internal
    static void nextResultWork(uv_work_t *req);

    /** Gets the number of rows in the current result set.
    *
    * @fn Integer ResultSet::getRowCount()
    *
    * @return Returns the number of rows in the current result set. ( type: Integer )
    *
    */
    static NODE_API_FUNC(getRowCount);

    /** Gets the number of columns in the current result set.
    *
    * @fn Integer ResultSet::getColumnCount()
    *
    * @return Returns the number of columns in the current result set. ( type: Integer )
    *
    */
    static NODE_API_FUNC(getColumnCount);

    /** Gets the name of the column, given the zero-based column index.
    *
    * @fn String ResultSet::getColumnName( Integer colIndex )
    *
    * @param colIndex The zero-based column index. ( type: Integer )
    *
    * @return Returns the name of the column. ( type: String )
    *
    */
    static NODE_API_FUNC(getColumnName);

    /** Gets the the column information.
    *
    * @fn Array ResultSet::getColumnInfo()
    *
    * @return Returns an Array of Objects. Each Object contains properties of each column. ( type: Array )
    *
    */
    static NODE_API_FUNC(getColumnInfo);

    /** Gets the length of a LOB or a character string type for the specified column.
    *
    * @fn Object ResultSet::getValueLength( Integer colIndex )
    *
    * @param colIndex The zero-based column index. ( type: Integer )
    *
    * @return Returns the length of a LOB or a charcater string type, or -1 if that information
    *         is not available. If the column is a NULL value, 0 is returned. ( type: Integer )
    *
    */
    static NODE_API_FUNC(getValueLength);

    /** Returns a Boolean indicates whether the specified column is NULL.
    *
    * @fn Boolean ResultSet::isNull( Integer colIndex )
    *
    * @param colIndex The zero-based column index. ( type: Integer )
    *
    * @return Returns a Boolean indicates whether the specified column is NULL. ( type: Boolean )
    *
    */
    static NODE_API_FUNC(isNull);

    /** Gets the value of the specified column.
    *
    * @fn Object ResultSet::getValue( Integer colIndex )
    *
    * @param colIndex The zero-based column index. ( type: Integer )
    *
    * @return Returns the value of the column. ( type: Object )
    *
    */
    static NODE_API_FUNC(getValue);

    /** Gets an array of objects with the column values of the current row.
    *
    * @fn Array ResultSet::getValues()
    *
    * @return Returns an array of objects with the column values of the current row. ( type: Array )
    *
    */
    static NODE_API_FUNC(getValues);

    /** Reads a stream of bytes from the specified LOB column, starting at location indicated
    * by dataOffset, into the buffer, starting at the location indicated by bufferOffset.
    *
    * This method supports asynchronous callbacks.
    *
    * @fn Array ResultSet::getData( Integer colIndex, Integer dataOffset, Buffer buffer, Integer bufferOffset, Integer length )
    *
    * @param colIndex The zero-based column index. ( type: Integer )
    * @param dataOffset The index within the column from which to begin the read operation. ( type: Integer )
    * @param buffer The buffer into which to copy the data. ( type: Buffer )
    * @param bufferOffset The index within the buffer to which the data will be copied. ( type: Integer )
    * @param length The maximum number of bytes to read. ( type: Integer )
    *
    * @return Returns The actual number of bytes read. ( type: Integer )
    *
    */
    static NODE_API_FUNC(getData);

    /// @internal
    static void getDataAfter(uv_work_t *req);
    /// @internal
    static void getDataWork(uv_work_t *req);

    /** Gets the String value of the specified column of the result set.
     *
     * This method returns the result set value as a string.
     *
     * @fn String ResultSet::getString( Integer colIndex )
     *
     * @param colIndex The index of the column of the
     *                    result set starting from 1. ( type: Integer )
     * @return Returns the value as a String.
     */

    /** Gets the String value of the specified column of the result set.
     *
     * This method returns the result set value as a string.
     *
     * @fn String ResultSet::getText( Integer colIndex )
     *
     * @param colIndex The index of the column of the
     *                    result set starting from 1. ( type: Integer )
     * @return Returns the value as a String.
     *
     * @see ResultSet::getString
     */

    /** Gets the String value of the specified column of the result set.
     *
     * This method returns the result set value as a string.
     *
     * @fn String ResultSet::getClob( Integer colIndex )
     *
     * @param colIndex The index of the column of the
     *                    result set starting from 1. ( type: Integer )
     * @return Returns the value as a String.
     *
     * @see ResultSet::getString
     */
    static NODE_API_FUNC( getString );

    /** Gets the Integer value of the specified column of the result set.
     *
     * @fn Integer ResultSet::getSmallInt( Integer colIndex )
     *
     * @param colIndex The index of the column of the
     *                    result set starting from 1. ( type: Integer )
     * @return Returns the value as an Integer.
     *
     * @see ResultSet::getInteger
     */
    static NODE_API_FUNC( getInteger );

    /** Gets the Numeric value of the specified column of the result set.
     *
     * Any numeric SQL types, including BIGINT, DOUBLE, REAL, FLOAT and
     * DOUBLE can be retrieved using this method.
     *
     * @fn Number ResultSet::getDecimal( Integer colIndex )
     *
     * @param colIndex The index of the column of the
     *                    result set starting from 1. ( type: Integer )
     * @return Returns the value as a numeric value.
     *
     * @see ResultSet::getDouble
     */

    static NODE_API_FUNC( getDouble );

    /** Gets the Date value of the specified column of the result set.
     *
     * This method returns the value in the specified column as a
     * JavaScript Date object. The local time zone is always assumed.
     *
     * This method is used to retrieve TIMESTAMP, TIME, and DATE
     * SQL types.
     *
     * @fn Date ResultSet::getDate( Integer colIndex )
     *
     * @param colIndex The index of the column of the
     *                    result set starting from 1. ( type: Integer )
     * @return Returns the value as a Date object.
     */
    static NODE_API_FUNC( getTimestamp );

    /** Gets the Binary value of the specified column of the result set.
     *
     * This method returns the value in the specified column as a Node.js
     * Buffer.
     *
     * @fn Buffer ResultSet::getBlob( Integer colIndex )
     *
     * @param colIndex The index of the column of the
     *                    result set starting from 1. ( type: Integer )
     * @return Returns the value as a Node.js Buffer object.
     */
    static NODE_API_FUNC( getBinary );

    /** Gets the Server CPU Time for ResultSet fetch since the result
     *  set was created or the last nextResult call
    *
    * @fn Integer ResultSet::getServerCPUTime()
    *
    * @return Returns Server CPU Time in microseconds for ResultSet fetch
    *
    */
    static NODE_API_FUNC(getServerCPUTime);

    /** Gets the Server Memory Usage for ResultSet fetch since the result
     *  set was created or the last nextResult call
    *
    * @fn Integer ResultSet::getServerMemoryUsage()
    *
    * @return Returns Server Memory Usage in bytes for ResultSet fetch
    *
    */
    static NODE_API_FUNC(getServerMemoryUsage);

    /** Gets the Server Processing Time for ResultSet fetch since the result
     *  set was created or the last nextResult call
    *
    * @fn Integer ResultSet::getServerProcessingTime()
    *
    * @return Returns Server Processing Time in microseconds for ResultSet fetch
    *
    */
    static NODE_API_FUNC(getServerProcessingTime);

    void deleteColumnInfos();

    /// @internal
    int num_cols;
    /// @internal
    std::vector<dbcapi_column_info*> column_infos;

  public:
    void getColumnInfos();

    /// @internal
    StatementPointer	stmt;
    /// @internal
    dbcapi_stmt         *dbcapi_stmt_ptr;
    /// @internal
    bool		is_closed;
    /// @internal
    bool		is_closing;
    /// @internal
    bool		fetched_first;
};
