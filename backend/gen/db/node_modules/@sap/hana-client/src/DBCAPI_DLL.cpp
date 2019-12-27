// ***************************************************************************
// Copyright (c) 2016 SAP SE or an SAP affiliate company. All rights reserved.
// ***************************************************************************
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//
// See the License for the specific language governing permissions and
// limitations under the License.
//
// While not a requirement of the license, if you do modify this file, we
// would appreciate hearing about it. Please email
// dbcapi_interfaces@sap.com
//
// ***************************************************************************

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#if defined( _WIN32 )
    #include <windows.h>
    #define DEFAULT_LIBRARY_NAME "libdbcapiHDB.dll"
#else
    #include <dlfcn.h>
    /* assume we are running on a UNIX platform */
    #if defined( __APPLE__ )
	#define LIB_EXT "dylib"
    #else
	#define LIB_EXT "so"
    #endif
    #define DEFAULT_LIBRARY_NAME "libdbcapiHDB." LIB_EXT
#endif

#include "DBCAPI_DLL.h"


static
void * loadLibrary( const char * name )
/*************************************/
{
    static void * handle = NULL;

    // only load dbcapi once and save handle
    if( handle == NULL ) {
#if defined( _WIN32 )
        handle = LoadLibrary( name );
#else
        handle = dlopen( name, RTLD_LAZY );
#endif
    }
    return handle;
}

static
void unloadLibrary( void * handle )
/*********************************/
{
    return;
}

static
void *
findSymbol( void * dll_handle, const char * name )
/************************************************/
{
#if defined( _WIN32 )
    return (void*) GetProcAddress( (HMODULE)dll_handle, name );
#else
    return dlsym( dll_handle, name );
#endif
}

#define LookupSymbol( api, sym )					\
	api->sym = (sym ## _func)findSymbol( api->dll_handle, #sym );

#define LookupSymbolAndCheck( api, sym )				\
	api->sym = (sym ## _func)findSymbol( api->dll_handle, #sym );	\
	if( api->sym == NULL ) {					\
	    unloadLibrary( api->dll_handle );				\
	    return 0;							\
	}

int
dbcapi_initialize_interface( DBCAPIInterface * api, const char * path )
/*********************************************************************/
{
    memset( api, 0, sizeof(*api) );

    if( path != NULL ) {
	api->dll_handle = loadLibrary( path );
	if( api->dll_handle != NULL ) {
	    goto loaded;
	}
    }
    api->dll_handle = loadLibrary( DEFAULT_LIBRARY_NAME );
    if( api->dll_handle != NULL ) {
	goto loaded;
    }
    return 0;

loaded:
    LookupSymbolAndCheck( api, dbcapi_init );
    LookupSymbolAndCheck( api, dbcapi_fini );
    LookupSymbolAndCheck( api, dbcapi_new_connection );
    LookupSymbolAndCheck( api, dbcapi_free_connection );
    LookupSymbolAndCheck( api, dbcapi_make_connection );
    LookupSymbolAndCheck( api, dbcapi_connect );
    LookupSymbolAndCheck( api, dbcapi_connect2 );
    LookupSymbolAndCheck( api, dbcapi_disconnect );
    LookupSymbolAndCheck( api, dbcapi_set_connect_property );
    LookupSymbolAndCheck( api, dbcapi_set_clientinfo );
    LookupSymbolAndCheck( api, dbcapi_get_clientinfo );
    LookupSymbolAndCheck( api, dbcapi_execute_immediate );
    LookupSymbolAndCheck( api, dbcapi_prepare );
    LookupSymbolAndCheck( api, dbcapi_abort );
    LookupSymbolAndCheck( api, dbcapi_get_function_code );
    LookupSymbolAndCheck( api, dbcapi_free_stmt );
    LookupSymbolAndCheck( api, dbcapi_num_params );
    LookupSymbolAndCheck( api, dbcapi_describe_bind_param );
    LookupSymbolAndCheck( api, dbcapi_bind_param );
    LookupSymbolAndCheck( api, dbcapi_send_param_data );
    LookupSymbolAndCheck( api, dbcapi_reset );
    LookupSymbolAndCheck( api, dbcapi_get_bind_param_info );
    LookupSymbolAndCheck( api, dbcapi_execute );
    LookupSymbolAndCheck( api, dbcapi_execute_direct );
    LookupSymbolAndCheck( api, dbcapi_fetch_absolute );
    LookupSymbolAndCheck( api, dbcapi_fetch_next );
    LookupSymbolAndCheck( api, dbcapi_get_next_result );
    LookupSymbolAndCheck( api, dbcapi_affected_rows );
    LookupSymbolAndCheck( api, dbcapi_num_cols );
    LookupSymbolAndCheck( api, dbcapi_num_rows );
    LookupSymbolAndCheck( api, dbcapi_get_column );
    LookupSymbolAndCheck( api, dbcapi_get_data );
    LookupSymbolAndCheck( api, dbcapi_get_data_info );
    LookupSymbolAndCheck( api, dbcapi_get_column_info );
    LookupSymbolAndCheck( api, dbcapi_commit );
    LookupSymbolAndCheck( api, dbcapi_rollback );
    LookupSymbolAndCheck( api, dbcapi_client_version );
    LookupSymbolAndCheck( api, dbcapi_error );
    LookupSymbolAndCheck( api, dbcapi_sqlstate );
    LookupSymbolAndCheck( api, dbcapi_clear_error );

    LookupSymbol( api, dbcapi_init_ex );
    LookupSymbol( api, dbcapi_fini_ex );
    LookupSymbol( api, dbcapi_new_connection_ex );
    LookupSymbol( api, dbcapi_make_connection_ex );
    LookupSymbol( api, dbcapi_client_version_ex );
    LookupSymbolAndCheck( api, dbcapi_cancel );

    LookupSymbol( api, dbcapi_set_batch_size );
    LookupSymbol( api, dbcapi_set_param_bind_type );
    LookupSymbol( api, dbcapi_get_batch_size );
    LookupSymbol( api, dbcapi_set_rowset_size );
    LookupSymbol( api, dbcapi_get_rowset_size );
    LookupSymbol( api, dbcapi_set_column_bind_type );
    LookupSymbol( api, dbcapi_bind_column );
    LookupSymbol( api, dbcapi_clear_column_bindings );
    LookupSymbol( api, dbcapi_fetched_rows );
    LookupSymbol( api, dbcapi_set_rowset_pos );

    LookupSymbol( api, dbcapi_finish_param_data );
    LookupSymbol( api, dbcapi_get_param_data );
    LookupSymbol( api, dbcapi_reset_param_data );
    LookupSymbol( api, dbcapi_error_length );
    LookupSymbol( api, dbcapi_set_autocommit );
    LookupSymbol( api, dbcapi_get_autocommit );
    LookupSymbol( api, dbcapi_set_transaction_isolation );
    LookupSymbol( api, dbcapi_set_query_timeout );
    LookupSymbol( api, dbcapi_register_warning_callback );
    LookupSymbol( api, dbcapi_get_print_line );
    LookupSymbol( api, dbcapi_get_row_status );
    LookupSymbol( api, dbcapi_get_stmt_server_cpu_time);
    LookupSymbol( api, dbcapi_get_stmt_server_memory_usage);
    LookupSymbol( api, dbcapi_get_stmt_server_processing_time);
    LookupSymbol( api, dbcapi_get_resultset_server_cpu_time);
    LookupSymbol( api, dbcapi_get_resultset_server_memory_usage);
    LookupSymbol( api, dbcapi_get_resultset_server_processing_time);

    api->initialized = 1;
    return 1;
}
#undef LookupSymbolAndCheck

void
dbcapi_finalize_interface( DBCAPIInterface * api )
/************************************************/
{
    if( api->initialized ) {
	unloadLibrary( api->dll_handle );
	memset( api, 0, sizeof(*api));
    }
}
