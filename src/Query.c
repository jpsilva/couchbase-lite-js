#include <assert.h>
#include <node_api.h>
#include <stdio.h>
#include "cbl/CouchbaseLite.h"
#include "Listener.h"

#define CHECK(expr)                                                                 \
  {                                                                                 \
    napi_status status = (expr);                                                    \
    if (status != napi_ok)                                                          \
    {                                                                               \
      fprintf(stderr, "%s:%d: failed assertion `%s'\n", __FILE__, __LINE__, #expr); \
      fflush(stderr);                                                               \
      abort();                                                                      \
    }                                                                               \
  }

// enum QueryLanguage

// CBLDatabase_CreateQuery
napi_value Database_CreateQuery(napi_env env, napi_callback_info info)
{
  size_t argc = 3;
  napi_value args[argc]; // [database, language, query]

  CBLError err;
  CBLDatabase *database;
  uint32_t language;

  CHECK(napi_get_cb_info(env, info, &argc, args, NULL, NULL));
  CHECK(napi_get_value_external(env, args[0], (void *)&database));
  CHECK(napi_get_value_int32(env, args[1], (int32_t *)&language));
  size_t str_size;
  CHECK(napi_get_value_string_utf8(env, args[2], NULL, 0, &str_size));
  char *queryString;
  queryString = (char *)calloc(str_size + 1, sizeof(char));
  str_size = str_size + 1;
  CHECK(napi_get_value_string_utf8(env, args[2], queryString, str_size, NULL));

  CBLQuery *query = CBLDatabase_CreateQuery(database, language, FLStr(queryString), NULL, &err);

  napi_value res;
  CHECK(napi_create_external(env, (void *)query, NULL, NULL, &res));

  return res;
}

FLStringResult ResultSet_ToJSON(CBLResultSet *results)
{
  FLMutableArray resultsArray = FLMutableArray_New();

  while (CBLResultSet_Next(results))
  {
    FLDict result = CBLResultSet_ResultDict(results);
    FLMutableArray_AppendDict(resultsArray, result);
  }

  FLStringResult json = FLValue_ToJSON((FLValue)resultsArray);

  FLMutableArray_Release(resultsArray);

  return json;
}

// CBLQuery_Execute
napi_value Query_Execute(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value args[argc]; // [database, language, query]

  CBLError err;
  CBLQuery *query;

  CHECK(napi_get_cb_info(env, info, &argc, args, NULL, NULL));
  CHECK(napi_get_value_external(env, args[0], (void *)&query));

  CBLResultSet *results = CBLQuery_Execute(query, &err);
  FLStringResult json = ResultSet_ToJSON(results);
  CBLResultSet_Release(results);
  napi_value res;
  CHECK(napi_create_string_utf8(env, json.buf, json.size, &res));
  FLSliceResult_Release(json);

  return res;
}

// CBLQuery_Explain
napi_value Query_Explain(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value args[argc]; // [database, language, query]

  CBLQuery *query;

  CHECK(napi_get_cb_info(env, info, &argc, args, NULL, NULL));
  CHECK(napi_get_value_external(env, args[0], (void *)&query));

  napi_value res;
  FLSliceResult explanation = CBLQuery_Explain(query);
  CHECK(napi_create_string_utf8(env, explanation.buf, explanation.size, &res));

  return res;
}

// CBLQuery_Parameters
napi_value Query_Parameters(napi_env env, napi_callback_info info)
{
  napi_value res;
  CHECK(napi_get_undefined(env, &res));
  return res;
}

// CBLQuery_SetParameters
napi_value Query_SetParameters(napi_env env, napi_callback_info info)
{
  napi_value res;
  CHECK(napi_get_undefined(env, &res));
  return res;
}

static void QueryChangeListener(void *cb, CBLQuery *query, CBLListenerToken *token)
{
  CBLError err;
  CBLResultSet *results = CBLQuery_CopyCurrentResults(query, token, &err);
  FLStringResult json = ResultSet_ToJSON(results);
  char *data;
  data = malloc(json.size + 1);
  assert(FLSlice_ToCString(FLSliceResult_AsSlice(json), data, json.size + 1) == true);

  FLSliceResult_Release(json);
  CBLResultSet_Release(results);

  CHECK(napi_acquire_threadsafe_function((napi_threadsafe_function)cb));
  CHECK(napi_call_threadsafe_function((napi_threadsafe_function)cb, data, napi_tsfn_nonblocking));
  CHECK(napi_release_threadsafe_function((napi_threadsafe_function)cb, napi_tsfn_release));
}

static void QueryChangeListenerCallJS(napi_env env, napi_value js_cb, void *context, void *data)
{
  napi_value undefined;
  CHECK(napi_get_undefined(env, &undefined));
  char *json = (char *)data;

  napi_value args[1];
  napi_value jsonString;
  CHECK(napi_create_string_utf8(env, json, NAPI_AUTO_LENGTH, &jsonString));
  args[0] = jsonString;

  CHECK(napi_call_function(env, undefined, js_cb, 1, args, NULL));

  free(data);
}

// CBLQuery_AddChangeListener
napi_value Query_AddChangeListener(napi_env env, napi_callback_info info)
{
  size_t argc = 2;
  napi_value args[argc]; // [database, language, query]

  CBLQuery *query;

  CHECK(napi_get_cb_info(env, info, &argc, args, NULL, NULL));
  CHECK(napi_get_value_external(env, args[0], (void *)&query));

  napi_value async_resource_name;
  assert(napi_create_string_utf8(env,
                                 "couchbase-lite query change listener",
                                 NAPI_AUTO_LENGTH,
                                 &async_resource_name) == napi_ok);
  napi_threadsafe_function listenerCallback;
  CHECK(napi_create_threadsafe_function(env, args[1], NULL, async_resource_name, 0, 1, NULL, NULL, NULL, QueryChangeListenerCallJS, &listenerCallback));
  CHECK(napi_unref_threadsafe_function(env, listenerCallback));

  CBLListenerToken *token = CBLQuery_AddChangeListener(query, QueryChangeListener, listenerCallback);

  if (!token)
  {
    napi_throw_error(env, "", "Error adding change listener\n");
    CHECK(napi_release_threadsafe_function(listenerCallback, napi_tsfn_abort));
  }

  struct StopListenerData *stopListenerData = newStopListenerData(listenerCallback, token);
  napi_value stopListener;
  assert(napi_create_function(env, "stopDatabaseChangeListener", NAPI_AUTO_LENGTH, StopChangeListener, stopListenerData, &stopListener) == napi_ok);

  return stopListener;
}
