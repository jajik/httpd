/**
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mod_lua.h"
#include "lua_apr.h"
#include "lua_dbd.h"
#include "scoreboard.h"
#include "util_md5.h"
#include "util_script.h"
#include "util_varbuf.h"
#include "apr_pools.h"
#include "apr_thread_mutex.h"

#include <lua.h>

extern apr_thread_mutex_t* lua_ivm_mutex;

APLOG_USE_MODULE(lua);
#define POST_MAX_VARS 500

#ifndef MODLUA_MAX_REG_MATCH
#define MODLUA_MAX_REG_MATCH 25
#endif

typedef char *(*req_field_string_f) (request_rec * r);
typedef int (*req_field_int_f) (request_rec * r);
typedef apr_table_t *(*req_field_apr_table_f) (request_rec * r);


void ap_lua_rstack_dump(lua_State *L, request_rec *r, const char *msg)
{
    int i;
    int top = lua_gettop(L);
    ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, APLOGNO(01484) "Lua Stack Dump: [%s]", msg);
    for (i = 1; i <= top; i++) {
        int t = lua_type(L, i);
        switch (t) {
        case LUA_TSTRING:{
                ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
                              "%d:  '%s'", i, lua_tostring(L, i));
                break;
            }
        case LUA_TUSERDATA:{
                ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "%d:  userdata",
                              i);
                break;
            }
        case LUA_TLIGHTUSERDATA:{
                ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
                              "%d:  lightuserdata", i);
                break;
            }
        case LUA_TNIL:{
                ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "%d:  NIL", i);
                break;
            }
        case LUA_TNONE:{
                ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "%d:  None", i);
                break;
            }
        case LUA_TBOOLEAN:{
                ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
                              "%d:  %s", i, lua_toboolean(L,
                                                          i) ? "true" :
                              "false");
                break;
            }
        case LUA_TNUMBER:{
                ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
                              "%d:  %g", i, lua_tonumber(L, i));
                break;
            }
        case LUA_TTABLE:{
                ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
                              "%d:  <table>", i);
                break;
            }
        case LUA_TFUNCTION:{
                ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
                              "%d:  <function>", i);
                break;
            }
        default:{
                ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
                              "%d:  unknown: -[%s]-", i, lua_typename(L, i));
                break;
            }
        }
    }
}

/**
 * Verify that the thing at index is a request_rec wrapping
 * userdata thingamajig and return it if it is. if it is not
 * lua will enter its error handling routine.
 */
static request_rec *ap_lua_check_request_rec(lua_State *L, int index)
{
    request_rec *r;
    luaL_checkudata(L, index, "Apache2.Request");
    r = (request_rec *) lua_unboxpointer(L, index);
    return r;
}

/* ------------------ request methods -------------------- */
/* helper callback for req_parseargs */
static int req_aprtable2luatable_cb(void *l, const char *key,
                                    const char *value)
{
    int t;
    lua_State *L = (lua_State *) l;     /* [table<s,t>, table<s,s>] */
    /* rstack_dump(L, RRR, "start of cb"); */
    /* L is [table<s,t>, table<s,s>] */
    /* build complex */

    lua_getfield(L, -1, key);   /* [VALUE, table<s,t>, table<s,s>] */
    /* rstack_dump(L, RRR, "after getfield"); */
    t = lua_type(L, -1);
    switch (t) {
    case LUA_TNIL:
    case LUA_TNONE:{
            lua_pop(L, 1);      /* [table<s,t>, table<s,s>] */
            lua_newtable(L);    /* [array, table<s,t>, table<s,s>] */
            lua_pushnumber(L, 1);       /* [1, array, table<s,t>, table<s,s>] */
            lua_pushstring(L, value);   /* [string, 1, array, table<s,t>, table<s,s>] */
            lua_settable(L, -3);        /* [array, table<s,t>, table<s,s>]  */
            lua_setfield(L, -2, key);   /* [table<s,t>, table<s,s>] */
            break;
        }
    case LUA_TTABLE:{
            /* [array, table<s,t>, table<s,s>] */
            int size = lua_objlen(L, -1);
            lua_pushnumber(L, size + 1);        /* [#, array, table<s,t>, table<s,s>] */
            lua_pushstring(L, value);   /* [string, #, array, table<s,t>, table<s,s>] */
            lua_settable(L, -3);        /* [array, table<s,t>, table<s,s>] */
            lua_setfield(L, -2, key);   /* [table<s,t>, table<s,s>] */
            break;
        }
    }

    /* L is [table<s,t>, table<s,s>] */
    /* build simple */
    lua_getfield(L, -2, key);   /* [VALUE, table<s,s>, table<s,t>] */
    if (lua_isnoneornil(L, -1)) {       /* only set if not already set */
        lua_pop(L, 1);          /* [table<s,s>, table<s,t>]] */
        lua_pushstring(L, value);       /* [string, table<s,s>, table<s,t>] */
        lua_setfield(L, -3, key);       /* [table<s,s>, table<s,t>]  */
    }
    else {
        lua_pop(L, 1);
    }
    return 1;
}


/*
 =======================================================================================================================
    lua_read_body(request_rec *r, const char **rbuf, apr_off_t *size): Reads any additional form data sent in POST/PUT
    requests. Used for multipart POST data.
 =======================================================================================================================
 */
static int lua_read_body(request_rec *r, const char **rbuf, apr_off_t *size)
{
    int rc = OK;

    if ((rc = ap_setup_client_block(r, REQUEST_CHUNKED_ERROR))) {
        return (rc);
    }
    if (ap_should_client_block(r)) {

        /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
        char         argsbuffer[HUGE_STRING_LEN];
        apr_off_t    rsize, len_read, rpos = 0;
        apr_off_t length = r->remaining;
        /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

        *rbuf = (const char *) apr_pcalloc(r->pool, (apr_size_t) (length + 1));
        *size = length;
        while ((len_read = ap_get_client_block(r, argsbuffer, sizeof(argsbuffer))) > 0) {
            if ((rpos + len_read) > length) {
                rsize = length - rpos;
            }
            else {
                rsize = len_read;
            }

            memcpy((char *) *rbuf + rpos, argsbuffer, (size_t) rsize);
            rpos += rsize;
        }
    }

    return (rc);
}


/*
 * =======================================================================================================================
 * lua_write_body: Reads any additional form data sent in POST/PUT requests
 * and writes to a file.
 * =======================================================================================================================
 */
static apr_status_t lua_write_body(request_rec *r, apr_file_t *file, apr_off_t *size)
{
    apr_status_t rc = OK;

    if ((rc = ap_setup_client_block(r, REQUEST_CHUNKED_ERROR)))
        return rc;
    if (ap_should_client_block(r)) {
        char argsbuffer[HUGE_STRING_LEN];
        apr_off_t rsize,
                  len_read,
                  rpos = 0;
        apr_off_t length = r->remaining;
        apr_size_t written;

        *size = length;
        while ((len_read =
                    ap_get_client_block(r, argsbuffer,
                                        sizeof(argsbuffer))) > 0) {
            if ((rpos + len_read) > length)
                rsize = (apr_size_t) length - rpos;
            else
                rsize = len_read;

            rc = apr_file_write_full(file, argsbuffer, (apr_size_t) rsize,
                                     &written);
            if (written != rsize || rc != OK)
                return APR_ENOSPC;
            rpos += rsize;
        }
    }

    return rc;
}

/* r:parseargs() returning a lua table */
static int req_parseargs(lua_State *L)
{
    apr_table_t *form_table;
    request_rec *r = ap_lua_check_request_rec(L, 1);
    lua_newtable(L);
    lua_newtable(L);            /* [table, table] */
    ap_args_to_table(r, &form_table);
    apr_table_do(req_aprtable2luatable_cb, L, form_table, NULL);
    return 2;                   /* [table<string, string>, table<string, array<string>>] */
}

/* r:parsebody(): Parses regular (url-enocded) or multipart POST data and returns two tables*/
static int req_parsebody(lua_State *L)
{
    apr_array_header_t          *pairs;
    apr_off_t len;
    int res;
    apr_size_t size;
    apr_size_t max_post_size;
    char *multipart;
    const char *contentType;
    request_rec *r = ap_lua_check_request_rec(L, 1);
    max_post_size = (apr_size_t) luaL_optint(L, 2, MAX_STRING_LEN);
    multipart = apr_pcalloc(r->pool, 256);
    contentType = apr_table_get(r->headers_in, "Content-Type");
    lua_newtable(L);
    lua_newtable(L);            /* [table, table] */    
    if (contentType != NULL && (sscanf(contentType, "multipart/form-data; boundary=%250c", multipart) == 1)) {
        char        *buffer, *key, *filename;
        char        *start = 0, *end = 0, *crlf = 0;
        const char  *data;
        int         i;
        size_t      vlen = 0;
        size_t      len = 0;
        if (lua_read_body(r, &data, (apr_off_t*) &size) != OK) {
            return 2;
        }
        len = strlen(multipart);
        i = 0;
        for
        (
            start = strstr((char *) data, multipart);
            start != start + size;
            start = end
        ) {
            i++;
            if (i == POST_MAX_VARS) break;
            end = strstr((char *) (start + 1), multipart);
            if (!end) end = start + size;
            crlf = strstr((char *) start, "\r\n\r\n");
            if (!crlf) break;
            key = (char *) apr_pcalloc(r->pool, 256);
            filename = (char *) apr_pcalloc(r->pool, 256);
            vlen = end - crlf - 8;
            buffer = (char *) apr_pcalloc(r->pool, vlen+1);
            memcpy(buffer, crlf + 4, vlen);
            sscanf(start + len + 2,
                "Content-Disposition: form-data; name=\"%255[^\"]\"; filename=\"%255[^\"]\"",
                key, filename);
            if (strlen(key)) {
                req_aprtable2luatable_cb(L, key, buffer);
            }
        }
    }
    else {
        char *buffer;
        res = ap_parse_form_data(r, NULL, &pairs, -1, max_post_size);
        if (res == OK) {
            while(pairs && !apr_is_empty_array(pairs)) {
                ap_form_pair_t *pair = (ap_form_pair_t *) apr_array_pop(pairs);
                apr_brigade_length(pair->value, 1, &len);
                size = (apr_size_t) len;
                buffer = apr_palloc(r->pool, size + 1);
                apr_brigade_flatten(pair->value, buffer, &size);
                buffer[len] = 0;
                req_aprtable2luatable_cb(L, pair->name, buffer);
            }
        }
    }
    return 2;                   /* [table<string, string>, table<string, array<string>>] */
}


/*
 * lua_ap_requestbody; r:requestbody([filename]) - Reads or stores the request
 * body
 */
static int lua_ap_requestbody(lua_State *L)
{
    const char     *filename;
    request_rec    *r;
    apr_off_t      maxSize;
    
    r = ap_lua_check_request_rec(L, 1);
    filename = luaL_optstring(L, 2, 0);
    maxSize = luaL_optint(L, 3, 0);

    if (r) {
        apr_off_t size;
        if (maxSize > 0 && r->remaining > maxSize) {
            lua_pushnil(L);
            lua_pushliteral(L, "Request body was larger than the permitted size.");
            return 2;
        }
        if (r->method_number != M_POST && r->method_number != M_PUT)
            return (0);
        if (!filename) {
            const char     *data;

            if (lua_read_body(r, &data, &size) != OK)
                return (0);

            lua_pushlstring(L, data, (size_t) size);
            lua_pushinteger(L, (lua_Integer) size);
            return (2);
        } else {
            apr_status_t rc;
            apr_file_t     *file;

            rc = apr_file_open(&file, filename, APR_CREATE | APR_FOPEN_WRITE,
                               APR_FPROT_OS_DEFAULT, r->pool);
            lua_settop(L, 0);
            if (rc == APR_SUCCESS) {
                rc = lua_write_body(r, file, &size);
                apr_file_close(file);
                if (rc != OK) {
                    lua_pushboolean(L, 0);
                    return 1;
                }
                lua_pushinteger(L, (lua_Integer) size);
                return (1);
            } else
                lua_pushboolean(L, 0);
            return (1);
        }
    }

    return (0);
}

/* wrap ap_rputs as r:puts(String) */
static int req_puts(lua_State *L)
{
    request_rec *r = ap_lua_check_request_rec(L, 1);

    int argc = lua_gettop(L);
    int i;

    for (i = 2; i <= argc; i++) {
        ap_rputs(luaL_checkstring(L, i), r);
    }
    return 0;
}

/* wrap ap_rwrite as r:write(String) */
static int req_write(lua_State *L)
{
    request_rec *r = ap_lua_check_request_rec(L, 1);
    size_t n;
    int rv;
    const char *buf = luaL_checklstring(L, 2, &n);

    rv = ap_rwrite((void *) buf, n, r);
    lua_pushinteger(L, rv);
    return 1;
}

/* r:addoutputfilter(name|function) */
static int req_add_output_filter(lua_State *L)
{
    request_rec *r = ap_lua_check_request_rec(L, 1);
    const char *name = luaL_checkstring(L, 2);
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, APLOGNO(01485) "adding output filter %s",
                  name);
    ap_add_output_filter(name, L, r, r->connection);
    return 0;
}

/* wrap ap_construct_url as r:construct_url(String) */
static int req_construct_url(lua_State *L)
{
    request_rec *r = ap_lua_check_request_rec(L, 1);
    const char *name = luaL_checkstring(L, 2);
    lua_pushstring(L, ap_construct_url(r->pool, name, r));
    return 1;
}

/* wrap ap_escape_html r:escape_html(String) */
static int req_escape_html(lua_State *L)
{
    request_rec *r = ap_lua_check_request_rec(L, 1);
    const char *s = luaL_checkstring(L, 2);
    lua_pushstring(L, ap_escape_html(r->pool, s));
    return 1;
}

/* wrap optional ssl_var_lookup as  r:ssl_var_lookup(String) */
static int req_ssl_var_lookup(lua_State *L)
{
    request_rec *r = ap_lua_check_request_rec(L, 1);
    const char *s = luaL_checkstring(L, 2);
    const char *res = ap_lua_ssl_val(r->pool, r->server, r->connection, r, 
                                     (char *)s);
    lua_pushstring(L, res);
    return 1;
}

/* BEGIN dispatch mathods for request_rec fields */

/* not really a field, but we treat it like one */
static const char *req_document_root(request_rec *r)
{
    return ap_document_root(r);
}

static const char *req_context_prefix(request_rec *r)
{
    return ap_context_prefix(r);
}

static const char *req_context_document_root(request_rec *r)
{
    return ap_context_document_root(r);
}

static char *req_uri_field(request_rec *r)
{
    return r->uri;
}

static const char *req_method_field(request_rec *r)
{
    return r->method;
}
static const char *req_handler_field(request_rec *r)
{
    return r->handler;
}
static const char *req_proxyreq_field(request_rec *r)
{
    switch (r->proxyreq) {
        case PROXYREQ_NONE:     return "PROXYREQ_NONE";
        case PROXYREQ_PROXY:    return "PROXYREQ_PROXY";
        case PROXYREQ_REVERSE:  return "PROXYREQ_REVERSE";
        case PROXYREQ_RESPONSE: return "PROXYREQ_RESPONSE";
        default: return NULL;
    }
}
static const char *req_hostname_field(request_rec *r)
{
    return r->hostname;
}

static const char *req_args_field(request_rec *r)
{
    return r->args;
}

static const char *req_path_info_field(request_rec *r)
{
    return r->path_info;
}

static const char *req_canonical_filename_field(request_rec *r)
{
    return r->canonical_filename;
}

static const char *req_filename_field(request_rec *r)
{
    return r->filename;
}

static const char *req_user_field(request_rec *r)
{
    return r->user;
}

static const char *req_unparsed_uri_field(request_rec *r)
{
    return r->unparsed_uri;
}

static const char *req_ap_auth_type_field(request_rec *r)
{
    return r->ap_auth_type;
}

static const char *req_content_encoding_field(request_rec *r)
{
    return r->content_encoding;
}

static const char *req_content_type_field(request_rec *r)
{
    return r->content_type;
}

static const char *req_range_field(request_rec *r)
{
    return r->range;
}

static const char *req_protocol_field(request_rec *r)
{
    return r->protocol;
}

static const char *req_the_request_field(request_rec *r)
{
    return r->the_request;
}

static const char *req_log_id_field(request_rec *r)
{
    return r->log_id;
}

static const char *req_useragent_ip_field(request_rec *r)
{
    return r->useragent_ip;
}

static int req_remaining_field(request_rec *r)
{
    return r->remaining;
}

static int req_status_field(request_rec *r)
{
    return r->status;
}

static int req_assbackwards_field(request_rec *r)
{
    return r->assbackwards;
}

static apr_table_t* req_headers_in(request_rec *r)
{
    return r->headers_in;
}

static apr_table_t* req_headers_out(request_rec *r)
{
    return r->headers_out;
}

static apr_table_t* req_err_headers_out(request_rec *r)
{
  return r->err_headers_out;
}

static apr_table_t* req_subprocess_env(request_rec *r)
{
  return r->subprocess_env;
}

static apr_table_t* req_notes(request_rec *r)
{
  return r->notes;
}

static int req_ssl_is_https_field(request_rec *r)
{
    return ap_lua_ssl_is_https(r->connection);
}

static int req_ap_get_server_port(request_rec *r)
{
    return (int) ap_get_server_port(r);
}

static int lua_ap_rflush (lua_State *L) {

    int returnValue;
    request_rec *r;
    luaL_checktype(L, 1, LUA_TUSERDATA);
    r = ap_lua_check_request_rec(L, 1);
    returnValue = ap_rflush(r);
    lua_pushboolean(L, (returnValue == 0));
    return 1;
}


static const char* lua_ap_options(request_rec* r) 
{
    int opts;
    opts = ap_allow_options(r);
    return apr_psprintf(r->pool, "%s %s %s %s %s %s", (opts&OPT_INDEXES) ? "Indexes" : "", (opts&OPT_INCLUDES) ? "Includes" : "", (opts&OPT_SYM_LINKS) ? "FollowSymLinks" : "", (opts&OPT_EXECCGI) ? "ExecCGI" : "", (opts&OPT_MULTI) ? "MultiViews" : "", (opts&OPT_ALL) == OPT_ALL ? "All" : "" );
}

static const char* lua_ap_allowoverrides(request_rec* r) 
{
    int opts;
    opts = ap_allow_overrides(r);
    return apr_psprintf(r->pool, "%s %s %s %s %s %s", (opts&OR_NONE) ? "None" : "", (opts&OR_LIMIT) ? "Limit" : "", (opts&OR_OPTIONS) ? "Options" : "", (opts&OR_FILEINFO) ? "FileInfo" : "", (opts&OR_AUTHCFG) ? "AuthCfg" : "", (opts&OR_INDEXES) ? "Indexes" : "" );
}

static int lua_ap_started(request_rec* r) 
{
    return (int)(ap_scoreboard_image->global->restart_time / 1000000);
}

static const char* lua_ap_basic_auth_pw(request_rec* r) 
{
    const char* pw = NULL;
    ap_get_basic_auth_pw(r, &pw);
    return pw ? pw : "";
}

static int lua_ap_limit_req_body(request_rec* r) 
{
    return (int) ap_get_limit_req_body(r);
}

static int lua_ap_is_initial_req(request_rec *r)
{
    return ap_is_initial_req(r);
}

static int lua_ap_some_auth_required(request_rec *r)
{
    return ap_some_auth_required(r);
}

static int lua_ap_sendfile(lua_State *L)
{

    apr_finfo_t file_info;
    const char  *filename;
    request_rec *r;

    luaL_checktype(L, 1, LUA_TUSERDATA);
    luaL_checktype(L, 2, LUA_TSTRING);
    r = ap_lua_check_request_rec(L, 1);
    filename = lua_tostring(L, 2);
    apr_stat(&file_info, filename, APR_FINFO_MIN, r->pool);
    if (file_info.filetype == APR_NOFILE || file_info.filetype == APR_DIR) {
        lua_pushboolean(L, 0);
    }
    else {
        apr_size_t      sent;
        apr_status_t    rc;
        apr_file_t      *file;

        rc = apr_file_open(&file, filename, APR_READ, APR_OS_DEFAULT,
                            r->pool);
        if (rc == APR_SUCCESS) {
            ap_send_fd(file, r, 0, (apr_size_t)file_info.size, &sent);
            apr_file_close(file);
            lua_pushinteger(L, sent);
        }
        else {
            lua_pushboolean(L, 0);
        }
    }

    return (1);
}


/*
 * lua_apr_b64encode; r:encode_base64(string) - encodes a string to Base64
 * format
 */
static int lua_apr_b64encode(lua_State *L)
{
    const char     *plain;
    char           *encoded;
    size_t          plain_len, encoded_len;
    request_rec    *r;

    r = ap_lua_check_request_rec(L, 1);
    luaL_checktype(L, 2, LUA_TSTRING);
    plain = lua_tolstring(L, 2, &plain_len);
    encoded_len = apr_base64_encode_len(plain_len);
    if (encoded_len) {
        encoded = apr_palloc(r->pool, encoded_len);
        encoded_len = apr_base64_encode(encoded, plain, plain_len);
        if (encoded_len > 0 && encoded[encoded_len - 1] == '\0')
            encoded_len--; 
        lua_pushlstring(L, encoded, encoded_len);
        return 1;
    }
    return 0;
}

/*
 * lua_apr_b64decode; r:decode_base64(string) - decodes a Base64 string
 */
static int lua_apr_b64decode(lua_State *L)
{
    const char     *encoded;
    char           *plain;
    size_t          encoded_len, decoded_len;
    request_rec    *r;

    r = ap_lua_check_request_rec(L, 1);
    luaL_checktype(L, 2, LUA_TSTRING);
    encoded = lua_tolstring(L, 2, &encoded_len);
    decoded_len = apr_base64_decode_len(encoded);
    if (decoded_len) {
        plain = apr_palloc(r->pool, decoded_len);
        decoded_len = apr_base64_decode(plain, encoded);
        if (decoded_len > 0 && plain[decoded_len - 1] == '\0')
            decoded_len--; 
        lua_pushlstring(L, plain, decoded_len);
        return 1;
    }
    return 0;
}

/*
 * lua_ap_unescape; r:unescape(string) - Unescapes an URL-encoded string
 */
static int lua_ap_unescape(lua_State *L)
{
    const char     *escaped;
    char           *plain;
    size_t x,
           y;
    request_rec    *r;
    r = ap_lua_check_request_rec(L, 1);
    luaL_checktype(L, 2, LUA_TSTRING);
    escaped = lua_tolstring(L, 2, &x);
    plain = apr_pstrdup(r->pool, escaped);
    y = ap_unescape_urlencoded(plain);
    if (!y) {
        lua_pushstring(L, plain);
        return 1;
    }
    return 0;
}

/*
 * lua_ap_escape; r:escape(string) - URL-escapes a string
 */
static int lua_ap_escape(lua_State *L)
{
    const char     *plain;
    char           *escaped;
    size_t x;
    request_rec    *r;
    r = ap_lua_check_request_rec(L, 1);
    luaL_checktype(L, 2, LUA_TSTRING);
    plain = lua_tolstring(L, 2, &x);
    escaped = ap_escape_urlencoded(r->pool, plain);
    lua_pushstring(L, escaped);
    return 1;
}

/*
 * lua_apr_md5; r:md5(string) - Calculates an MD5 digest of a string
 */
static int lua_apr_md5(lua_State *L)
{
    const char     *buffer;
    char           *result;
    size_t len;
    request_rec    *r;

    r = ap_lua_check_request_rec(L, 1);
    luaL_checktype(L, 2, LUA_TSTRING);
    buffer = lua_tolstring(L, 2, &len);
    result = ap_md5_binary(r->pool, (const unsigned char *)buffer, len);
    lua_pushstring(L, result);
    return 1;
}

/*
 * lua_apr_sha1; r:sha1(string) - Calculates the SHA1 digest of a string
 */
static int lua_apr_sha1(lua_State *L)
{
    unsigned char digest[APR_SHA1_DIGESTSIZE];
    apr_sha1_ctx_t sha1;
    const char     *buffer;
    char           *result;
    size_t len;
    request_rec    *r;

    r = ap_lua_check_request_rec(L, 1);
    luaL_checktype(L, 2, LUA_TSTRING);
    result = apr_pcalloc(r->pool, sizeof(digest) * 2 + 1);
    buffer = lua_tolstring(L, 2, &len);
    apr_sha1_init(&sha1);
    apr_sha1_update(&sha1, buffer, len);
    apr_sha1_final(digest, &sha1);
    ap_bin2hex(digest, sizeof(digest), result);
    lua_pushstring(L, result);
    return 1;
}


/*
 * lua_ap_mpm_query; r:mpm_query(info) - Queries for MPM info
 */
static int lua_ap_mpm_query(lua_State *L)
{
    int x,
        y;

    x = lua_tointeger(L, 1);
    ap_mpm_query(x, &y);
    lua_pushinteger(L, y);
    return 1;
}

/*
 * lua_ap_expr; r:expr(string) - Evaluates an expr statement.
 */
static int lua_ap_expr(lua_State *L)
{
    request_rec    *r;
    int x = 0;
    const char     *expr,
    *err;
    ap_expr_info_t res;

    luaL_checktype(L, 1, LUA_TUSERDATA);
    luaL_checktype(L, 2, LUA_TSTRING);
    r = ap_lua_check_request_rec(L, 1);
    expr = lua_tostring(L, 2);


    res.filename = NULL;
    res.flags = 0;
    res.line_number = 0;
    res.module_index = APLOG_MODULE_INDEX;

    err = ap_expr_parse(r->pool, r->pool, &res, expr, NULL);
    if (!err) {
        x = ap_expr_exec(r, &res, &err);
        lua_pushboolean(L, x);
        if (x < 0) {
            lua_pushstring(L, err);
            return 2;
        }
        return 1;
    } else {
        lua_pushboolean(L, 0);
        lua_pushstring(L, err);
        return 2;
    }
    lua_pushboolean(L, 0);
    return 1;
}


/*
 * lua_ap_regex; r:regex(string, pattern [, flags])
 * - Evaluates a regex and returns captures if matched
 */
static int lua_ap_regex(lua_State *L)
{
    request_rec    *r;
    int i,
        rv,
        flags;
    const char     *pattern,
    *source;
    char           *err;
    ap_regex_t regex;
    ap_regmatch_t matches[MODLUA_MAX_REG_MATCH+1];

    luaL_checktype(L, 1, LUA_TUSERDATA);
    luaL_checktype(L, 2, LUA_TSTRING);
    luaL_checktype(L, 3, LUA_TSTRING);
    r = ap_lua_check_request_rec(L, 1);
    source = lua_tostring(L, 2);
    pattern = lua_tostring(L, 3);
    flags = luaL_optinteger(L, 4, 0);

    rv = ap_regcomp(&regex, pattern, flags);
    if (rv) {
        lua_pushboolean(L, 0);
        err = apr_palloc(r->pool, 256);
        ap_regerror(rv, &regex, err, 256);
        lua_pushstring(L, err);
        return 2;
    }

    if (regex.re_nsub > MODLUA_MAX_REG_MATCH) {
        lua_pushboolean(L, 0);
        err = apr_palloc(r->pool, 64);
        apr_snprintf(err, 64,
                     "regcomp found %d matches; only %d allowed.",
                     regex.re_nsub, MODLUA_MAX_REG_MATCH);
        lua_pushstring(L, err);
        return 2;
    }

    rv = ap_regexec(&regex, source, MODLUA_MAX_REG_MATCH, matches, 0);
    if (rv == AP_REG_NOMATCH) {
        lua_pushboolean(L, 0);
        return 1;
    }
    
    lua_newtable(L);
    for (i = 0; i <= regex.re_nsub; i++) {
        lua_pushinteger(L, i);
        if (matches[i].rm_so >= 0 && matches[i].rm_eo >= 0)
            lua_pushstring(L,
                           apr_pstrndup(r->pool, source + matches[i].rm_so,
                                        matches[i].rm_eo - matches[i].rm_so));
        else
            lua_pushnil(L);
        lua_settable(L, -3);

    }
    return 1;
}




/*
 * lua_ap_scoreboard_process; r:scoreboard_process(a) - returns scoreboard info
 */
static int lua_ap_scoreboard_process(lua_State *L)
{
    int i;
    process_score  *ps_record;

    luaL_checktype(L, 1, LUA_TUSERDATA);
    luaL_checktype(L, 2, LUA_TNUMBER);
    i = lua_tointeger(L, 2);
    ps_record = ap_get_scoreboard_process(i);
    if (ps_record) {
        lua_newtable(L);

        lua_pushstring(L, "connections");
        lua_pushnumber(L, ps_record->connections);
        lua_settable(L, -3);

        lua_pushstring(L, "keepalive");
        lua_pushnumber(L, ps_record->keep_alive);
        lua_settable(L, -3);

        lua_pushstring(L, "lingering_close");
        lua_pushnumber(L, ps_record->lingering_close);
        lua_settable(L, -3);

        lua_pushstring(L, "pid");
        lua_pushnumber(L, ps_record->pid);
        lua_settable(L, -3);

        lua_pushstring(L, "suspended");
        lua_pushnumber(L, ps_record->suspended);
        lua_settable(L, -3);

        lua_pushstring(L, "write_completion");
        lua_pushnumber(L, ps_record->write_completion);
        lua_settable(L, -3);

        lua_pushstring(L, "not_accepting");
        lua_pushnumber(L, ps_record->not_accepting);
        lua_settable(L, -3);

        lua_pushstring(L, "quiescing");
        lua_pushnumber(L, ps_record->quiescing);
        lua_settable(L, -3);

        return 1;
    }
    return 0;
}

/*
 * lua_ap_scoreboard_worker; r:scoreboard_worker(proc, thread) - Returns thread
 * info
 */
static int lua_ap_scoreboard_worker(lua_State *L)
{
    int i,
        j;
    worker_score   *ws_record;

    luaL_checktype(L, 1, LUA_TUSERDATA);
    luaL_checktype(L, 2, LUA_TNUMBER);
    luaL_checktype(L, 3, LUA_TNUMBER);
    i = lua_tointeger(L, 2);
    j = lua_tointeger(L, 3);
    ws_record = ap_get_scoreboard_worker_from_indexes(i, j);
    if (ws_record) {
        lua_newtable(L);

        lua_pushstring(L, "access_count");
        lua_pushnumber(L, ws_record->access_count);
        lua_settable(L, -3);

        lua_pushstring(L, "bytes_served");
        lua_pushnumber(L, (lua_Number) ws_record->bytes_served);
        lua_settable(L, -3);

        lua_pushstring(L, "client");
        lua_pushstring(L, ws_record->client);
        lua_settable(L, -3);

        lua_pushstring(L, "conn_bytes");
        lua_pushnumber(L, (lua_Number) ws_record->conn_bytes);
        lua_settable(L, -3);

        lua_pushstring(L, "conn_count");
        lua_pushnumber(L, ws_record->conn_count);
        lua_settable(L, -3);

        lua_pushstring(L, "generation");
        lua_pushnumber(L, ws_record->generation);
        lua_settable(L, -3);

        lua_pushstring(L, "last_used");
        lua_pushnumber(L, (lua_Number) ws_record->last_used);
        lua_settable(L, -3);

        lua_pushstring(L, "pid");
        lua_pushnumber(L, ws_record->pid);
        lua_settable(L, -3);

        lua_pushstring(L, "request");
        lua_pushstring(L, ws_record->request);
        lua_settable(L, -3);

        lua_pushstring(L, "start_time");
        lua_pushnumber(L, (lua_Number) ws_record->start_time);
        lua_settable(L, -3);

        lua_pushstring(L, "status");
        lua_pushnumber(L, ws_record->status);
        lua_settable(L, -3);

        lua_pushstring(L, "stop_time");
        lua_pushnumber(L, (lua_Number) ws_record->stop_time);
        lua_settable(L, -3);

        lua_pushstring(L, "tid");

        lua_pushinteger(L, (lua_Integer) ws_record->tid);
        lua_settable(L, -3);

        lua_pushstring(L, "vhost");
        lua_pushstring(L, ws_record->vhost);
        lua_settable(L, -3);
#ifdef HAVE_TIMES
        lua_pushstring(L, "stimes");
        lua_pushnumber(L, ws_record->times.tms_stime);
        lua_settable(L, -3);

        lua_pushstring(L, "utimes");
        lua_pushnumber(L, ws_record->times.tms_utime);
        lua_settable(L, -3);
#endif
        return 1;
    }
    return 0;
}

/*
 * lua_ap_clock; r:clock() - Returns timestamp with microsecond precision
 */
static int lua_ap_clock(lua_State *L)
{
    apr_time_t now;
    now = apr_time_now();
    lua_pushnumber(L, (lua_Number) now);
    return 1;
}

/*
 * lua_ap_add_input_filter; r:add_input_filter(name) - Adds an input filter to
 * the chain
 */
static int lua_ap_add_input_filter(lua_State *L)
{
    request_rec    *r;
    const char     *filterName;
    ap_filter_rec_t *filter;

    luaL_checktype(L, 1, LUA_TUSERDATA);
    luaL_checktype(L, 2, LUA_TSTRING);
    r = ap_lua_check_request_rec(L, 1);
    filterName = lua_tostring(L, 2);
    filter = ap_get_input_filter_handle(filterName);
    if (filter) {
        ap_add_input_filter_handle(filter, NULL, r, r->connection);
        lua_pushboolean(L, 1);
    } else
        lua_pushboolean(L, 0);
    return 1;
}


/*
 * lua_ap_module_info; r:module_info(mod_name) - Returns information about a
 * loaded module
 */
static int lua_ap_module_info(lua_State *L)
{
    const char     *moduleName;
    module         *mod;

    luaL_checktype(L, 1, LUA_TSTRING);
    moduleName = lua_tostring(L, 1);
    mod = ap_find_linked_module(moduleName);
    if (mod) {
        const command_rec *cmd;
        lua_newtable(L);
        lua_pushstring(L, "commands");
        lua_newtable(L);
        for (cmd = mod->cmds; cmd->name; ++cmd) {
            lua_pushstring(L, cmd->name);
            lua_pushstring(L, cmd->errmsg);
            lua_settable(L, -3);
        }
        lua_settable(L, -3);
        return 1;
    }
    return 0;
}

/*
 * lua_ap_runtime_dir_relative: r:runtime_dir_relative(file): Returns the
 * filename as relative to the runtime dir
 */
static int lua_ap_runtime_dir_relative(lua_State *L)
{
    request_rec    *r;
    const char     *file;

    luaL_checktype(L, 1, LUA_TUSERDATA);
    r = ap_lua_check_request_rec(L, 1);
    file = luaL_optstring(L, 2, ".");
    lua_pushstring(L, ap_runtime_dir_relative(r->pool, file));
    return 1;
}

/*
 * lua_ap_set_document_root; r:set_document_root(path) - sets the current doc
 * root for the request
 */
static int lua_ap_set_document_root(lua_State *L)
{
    request_rec    *r;
    const char     *root;

    luaL_checktype(L, 1, LUA_TUSERDATA);
    luaL_checktype(L, 2, LUA_TSTRING);
    r = ap_lua_check_request_rec(L, 1);
    root = lua_tostring(L, 2);
    ap_set_document_root(r, root);
    return 0;
}

/*
 * lua_ap_getdir; r:get_direntries(directory) - Gets all entries of a
 * directory and returns the directory info as a table
 */
static int lua_ap_getdir(lua_State *L)
{
    request_rec    *r;
    apr_dir_t      *thedir;
    apr_finfo_t    file_info;
    apr_status_t   status;
    const char     *directory;

    luaL_checktype(L, 1, LUA_TUSERDATA);
    luaL_checktype(L, 2, LUA_TSTRING);
    r = ap_lua_check_request_rec(L, 1);
    directory = lua_tostring(L, 2);
    if (apr_dir_open(&thedir, directory, r->pool) == APR_SUCCESS) {
        int i = 0;
        lua_newtable(L);
        do {
            status = apr_dir_read(&file_info, APR_FINFO_NAME, thedir);
            if (APR_STATUS_IS_INCOMPLETE(status)) {
                continue; /* ignore un-stat()able files */
            }
            else if (status != APR_SUCCESS) {
                break;
            }
            lua_pushinteger(L, ++i);
            lua_pushstring(L, file_info.name);
            lua_settable(L, -3);

        } while (1);
        apr_dir_close(thedir);
        return 1;
    }
    else {
        return 0;
    }
}

/*
 * lua_ap_stat; r:stat(filename [, wanted]) - Runs stat on a file and
 * returns the file info as a table
 */
static int lua_ap_stat(lua_State *L)
{
    request_rec    *r;
    const char     *filename;
    apr_finfo_t file_info;
    apr_int32_t wanted;

    luaL_checktype(L, 1, LUA_TUSERDATA);
    luaL_checktype(L, 2, LUA_TSTRING);
    r = ap_lua_check_request_rec(L, 1);
    filename = lua_tostring(L, 2);
    wanted = luaL_optinteger(L, 3, APR_FINFO_MIN);
    if (apr_stat(&file_info, filename, wanted, r->pool) == OK) {
        lua_newtable(L);
        if (wanted & APR_FINFO_MTIME) {
            lua_pushstring(L, "mtime");
            lua_pushnumber(L, (lua_Number) file_info.mtime);
            lua_settable(L, -3);
        }
        if (wanted & APR_FINFO_ATIME) {
            lua_pushstring(L, "atime");
            lua_pushnumber(L, (lua_Number) file_info.atime);
            lua_settable(L, -3);
        }
        if (wanted & APR_FINFO_CTIME) {
            lua_pushstring(L, "ctime");
            lua_pushnumber(L, (lua_Number) file_info.ctime);
            lua_settable(L, -3);
        }
        if (wanted & APR_FINFO_SIZE) {
            lua_pushstring(L, "size");
            lua_pushnumber(L, (lua_Number) file_info.size);
            lua_settable(L, -3);
        }
        if (wanted & APR_FINFO_TYPE) {
            lua_pushstring(L, "filetype");
            lua_pushinteger(L, file_info.filetype);
            lua_settable(L, -3);
        }
        if (wanted & APR_FINFO_PROT) {
            lua_pushstring(L, "protection");
            lua_pushinteger(L, file_info.protection);
            lua_settable(L, -3);
        }
        return 1;
    }
    else {
        return 0;
    }
}

/*
 * lua_ap_loaded_modules; r:loaded_modules() - Returns a list of loaded modules
 */
static int lua_ap_loaded_modules(lua_State *L)
{
    int i;
    lua_newtable(L);
    for (i = 0; ap_loaded_modules[i] && ap_loaded_modules[i]->name; i++) {
        lua_pushinteger(L, i + 1);
        lua_pushstring(L, ap_loaded_modules[i]->name);
        lua_settable(L, -3);
    }
    return 1;
}

/*
 * lua_ap_server_info; r:server_info() - Returns server info, such as the
 * executable filename, server root, mpm etc
 */
static int lua_ap_server_info(lua_State *L)
{
    lua_newtable(L);

    lua_pushstring(L, "server_executable");
    lua_pushstring(L, ap_server_argv0);
    lua_settable(L, -3);

    lua_pushstring(L, "server_root");
    lua_pushstring(L, ap_server_root);
    lua_settable(L, -3);

    lua_pushstring(L, "scoreboard_fname");
    lua_pushstring(L, ap_scoreboard_fname);
    lua_settable(L, -3);

    lua_pushstring(L, "server_mpm");
    lua_pushstring(L, ap_show_mpm());
    lua_settable(L, -3);

    return 1;
}


/*
 * === Auto-scraped functions ===
 */


/**
 * ap_set_context_info: Set context_prefix and context_document_root.
 * @param r The request
 * @param prefix the URI prefix, without trailing slash
 * @param document_root the corresponding directory on disk, without trailing
 * slash
 * @note If one of prefix of document_root is NULL, the corrsponding
 * property will not be changed.
 */
static int lua_ap_set_context_info(lua_State *L)
{
    request_rec    *r;
    const char     *prefix;
    const char     *document_root;
    luaL_checktype(L, 1, LUA_TUSERDATA);
    r = ap_lua_check_request_rec(L, 1);
    luaL_checktype(L, 2, LUA_TSTRING);
    prefix = lua_tostring(L, 2);
    luaL_checktype(L, 3, LUA_TSTRING);
    document_root = lua_tostring(L, 3);
    ap_set_context_info(r, prefix, document_root);
    return 0;
}


/**
 * ap_os_escape_path (apr_pool_t *p, const char *path, int partial)
 * convert an OS path to a URL in an OS dependant way.
 * @param p The pool to allocate from
 * @param path The path to convert
 * @param partial if set, assume that the path will be appended to something
 *        with a '/' in it (and thus does not prefix "./")
 * @return The converted URL
 */
static int lua_ap_os_escape_path(lua_State *L)
{
    char           *returnValue;
    request_rec    *r;
    const char     *path;
    int partial = 0;
    luaL_checktype(L, 1, LUA_TUSERDATA);
    r = ap_lua_check_request_rec(L, 1);
    luaL_checktype(L, 2, LUA_TSTRING);
    path = lua_tostring(L, 2);
    if (lua_isboolean(L, 3))
        partial = lua_toboolean(L, 3);
    returnValue = ap_os_escape_path(r->pool, path, partial);
    lua_pushstring(L, returnValue);
    return 1;
}


/**
 * ap_escape_logitem (apr_pool_t *p, const char *str)
 * Escape a string for logging
 * @param p The pool to allocate from
 * @param str The string to escape
 * @return The escaped string
 */
static int lua_ap_escape_logitem(lua_State *L)
{
    char           *returnValue;
    request_rec    *r;
    const char     *str;
    luaL_checktype(L, 1, LUA_TUSERDATA);
    r = ap_lua_check_request_rec(L, 1);
    luaL_checktype(L, 2, LUA_TSTRING);
    str = lua_tostring(L, 2);
    returnValue = ap_escape_logitem(r->pool, str);
    lua_pushstring(L, returnValue);
    return 1;
}

/**
 * ap_strcmp_match (const char *str, const char *expected)
 * Determine if a string matches a patterm containing the wildcards '?' or '*'
 * @param str The string to check
 * @param expected The pattern to match against
 * @param ignoreCase Whether to ignore case when matching
 * @return 1 if the two strings match, 0 otherwise
 */
static int lua_ap_strcmp_match(lua_State *L)
{
    int returnValue;
    const char     *str;
    const char     *expected;
    int ignoreCase = 0;
    luaL_checktype(L, 1, LUA_TSTRING);
    str = lua_tostring(L, 1);
    luaL_checktype(L, 2, LUA_TSTRING);
    expected = lua_tostring(L, 2);
    if (lua_isboolean(L, 3))
        ignoreCase = lua_toboolean(L, 3);
    if (!ignoreCase)
        returnValue = ap_strcmp_match(str, expected);
    else
        returnValue = ap_strcasecmp_match(str, expected);
    lua_pushboolean(L, (!returnValue));
    return 1;
}


/**
 * ap_set_keepalive (request_rec *r)
 * Set the keepalive status for this request
 * @param r The current request
 * @return 1 if keepalive can be set, 0 otherwise
 */
static int lua_ap_set_keepalive(lua_State *L)
{
    int returnValue;
    request_rec    *r;
    luaL_checktype(L, 1, LUA_TUSERDATA);
    r = ap_lua_check_request_rec(L, 1);
    returnValue = ap_set_keepalive(r);
    lua_pushboolean(L, returnValue);
    return 1;
}

/**
 * ap_make_etag (request_rec *r, int force_weak)
 * Construct an entity tag from the resource information.  If it's a real
 * file, build in some of the file characteristics.
 * @param r The current request
 * @param force_weak Force the entity tag to be weak - it could be modified
 *                   again in as short an interval.
 * @return The entity tag
 */
static int lua_ap_make_etag(lua_State *L)
{
    char           *returnValue;
    request_rec    *r;
    int force_weak;
    luaL_checktype(L, 1, LUA_TUSERDATA);
    r = ap_lua_check_request_rec(L, 1);
    luaL_checktype(L, 2, LUA_TBOOLEAN);
    force_weak = luaL_optint(L, 2, 0);
    returnValue = ap_make_etag(r, force_weak);
    lua_pushstring(L, returnValue);
    return 1;
}



/**
 * ap_send_interim_response (request_rec *r, int send_headers)
 * Send an interim (HTTP 1xx) response immediately.
 * @param r The request
 * @param send_headers Whether to send&clear headers in r->headers_out
 */
static int lua_ap_send_interim_response(lua_State *L)
{
    request_rec    *r;
    int send_headers = 0;
    luaL_checktype(L, 1, LUA_TUSERDATA);
    r = ap_lua_check_request_rec(L, 1);
    if (lua_isboolean(L, 2))
        send_headers = lua_toboolean(L, 2);
    ap_send_interim_response(r, send_headers);
    return 0;
}


/**
 * ap_custom_response (request_rec *r, int status, const char *string)
 * Install a custom response handler for a given status
 * @param r The current request
 * @param status The status for which the custom response should be used
 * @param string The custom response.  This can be a static string, a file
 *               or a URL
 */
static int lua_ap_custom_response(lua_State *L)
{
    request_rec    *r;
    int status;
    const char     *string;
    luaL_checktype(L, 1, LUA_TUSERDATA);
    r = ap_lua_check_request_rec(L, 1);
    luaL_checktype(L, 2, LUA_TNUMBER);
    status = lua_tointeger(L, 2);
    luaL_checktype(L, 3, LUA_TSTRING);
    string = lua_tostring(L, 3);
    ap_custom_response(r, status, string);
    return 0;
}


/**
 * ap_exists_config_define (const char *name)
 * Check for a definition from the server command line
 * @param name The define to check for
 * @return 1 if defined, 0 otherwise
 */
static int lua_ap_exists_config_define(lua_State *L)
{
    int returnValue;
    const char     *name;
    luaL_checktype(L, 1, LUA_TSTRING);
    name = lua_tostring(L, 1);
    returnValue = ap_exists_config_define(name);
    lua_pushboolean(L, returnValue);
    return 1;
}

static int lua_ap_get_server_name_for_url(lua_State *L)
{
    const char     *servername;
    request_rec    *r;
    luaL_checktype(L, 1, LUA_TUSERDATA);
    r = ap_lua_check_request_rec(L, 1);
    servername = ap_get_server_name_for_url(r);
    lua_pushstring(L, servername);
    return 1;
}

/* ap_state_query (int query_code) item starts a new field  */
static int lua_ap_state_query(lua_State *L)
{

    int returnValue;
    int query_code;
    luaL_checktype(L, 1, LUA_TNUMBER);
    query_code = lua_tointeger(L, 1);
    returnValue = ap_state_query(query_code);
    lua_pushinteger(L, returnValue);
    return 1;
}

/*
 * lua_ap_usleep; r:usleep(microseconds)
 * - Sleep for the specified number of microseconds.
 */
static int lua_ap_usleep(lua_State *L)
{
    apr_interval_time_t msec;
    luaL_checktype(L, 1, LUA_TNUMBER);
    msec = (apr_interval_time_t)lua_tonumber(L, 1);
    apr_sleep(msec);
    return 0;
}

/* END dispatch methods for request_rec fields */

static int req_dispatch(lua_State *L)
{
    apr_hash_t *dispatch;
    req_fun_t *rft;
    request_rec *r = ap_lua_check_request_rec(L, 1);
    const char *name = luaL_checkstring(L, 2);
    lua_pop(L, 2);

    lua_getfield(L, LUA_REGISTRYINDEX, "Apache2.Request.dispatch");
    dispatch = lua_touserdata(L, 1);
    lua_pop(L, 1);

    rft = apr_hash_get(dispatch, name, APR_HASH_KEY_STRING);
    if (rft) {
        switch (rft->type) {
        case APL_REQ_FUNTYPE_TABLE:{
                apr_table_t *rs;
                req_field_apr_table_f func = (req_field_apr_table_f)rft->fun;
                ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, APLOGNO(01486)
                              "request_rec->dispatching %s -> apr table",
                              name);
                rs = (*func)(r);
                ap_lua_push_apr_table(L, rs);
                return 1;
            }

        case APL_REQ_FUNTYPE_LUACFUN:{
                lua_CFunction func = (lua_CFunction)rft->fun;
                ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, APLOGNO(01487)
                              "request_rec->dispatching %s -> lua_CFunction",
                              name);
                lua_pushcfunction(L, func);
                return 1;
            }
        case APL_REQ_FUNTYPE_STRING:{
                req_field_string_f func = (req_field_string_f)rft->fun;
                char *rs;
                ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, APLOGNO(01488)
                              "request_rec->dispatching %s -> string", name);
                rs = (*func) (r);
                lua_pushstring(L, rs);
                return 1;
            }
        case APL_REQ_FUNTYPE_INT:{
                req_field_int_f func = (req_field_int_f)rft->fun;
                int rs;
                ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, APLOGNO(01489)
                              "request_rec->dispatching %s -> int", name);
                rs = (*func) (r);
                lua_pushinteger(L, rs);
                return 1;
            }
        case APL_REQ_FUNTYPE_BOOLEAN:{
                req_field_int_f func = (req_field_int_f)rft->fun;
                int rs;
                ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, APLOGNO(01490)
                              "request_rec->dispatching %s -> boolean", name);
                rs = (*func) (r);
                lua_pushboolean(L, rs);
                return 1;
            }
        }
    }

    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, APLOGNO(01491) "nothing for %s", name);
    return 0;
}

/* helper function for the logging functions below */
static int req_log_at(lua_State *L, int level)
{
    const char *msg;
    request_rec *r = ap_lua_check_request_rec(L, 1);
    lua_Debug dbg;

    lua_getstack(L, 1, &dbg);
    lua_getinfo(L, "Sl", &dbg);

    msg = luaL_checkstring(L, 2);
    ap_log_rerror(dbg.source, dbg.currentline, APLOG_MODULE_INDEX, level, 0,
                  r, "%s", msg);
    return 0;
}

/* r:debug(String) and friends which use apache logging */
static int req_emerg(lua_State *L)
{
    return req_log_at(L, APLOG_EMERG);
}
static int req_alert(lua_State *L)
{
    return req_log_at(L, APLOG_ALERT);
}
static int req_crit(lua_State *L)
{
    return req_log_at(L, APLOG_CRIT);
}
static int req_err(lua_State *L)
{
    return req_log_at(L, APLOG_ERR);
}
static int req_warn(lua_State *L)
{
    return req_log_at(L, APLOG_WARNING);
}
static int req_notice(lua_State *L)
{
    return req_log_at(L, APLOG_NOTICE);
}
static int req_info(lua_State *L)
{
    return req_log_at(L, APLOG_INFO);
}
static int req_debug(lua_State *L)
{
    return req_log_at(L, APLOG_DEBUG);
}

static int lua_ivm_get(lua_State *L) 
{
    const char *key, *raw_key;
    lua_ivm_object *object = NULL;
    request_rec *r = ap_lua_check_request_rec(L, 1);
    key = luaL_checkstring(L, 2);
    raw_key = apr_pstrcat(r->pool, "lua_ivm_", key, NULL);
    apr_thread_mutex_lock(lua_ivm_mutex);
    apr_pool_userdata_get((void **)&object, raw_key, r->server->process->pool);
    if (object) {
        if (object->type == LUA_TBOOLEAN) lua_pushboolean(L, (int) object->number);
        else if (object->type == LUA_TNUMBER) lua_pushnumber(L, object->number);
        else if (object->type == LUA_TSTRING) lua_pushlstring(L, object->vb.buf, object->size);
        apr_thread_mutex_unlock(lua_ivm_mutex);
        return 1;
    }
    else {
        apr_thread_mutex_unlock(lua_ivm_mutex);
        return 0;
    }
}


static int lua_ivm_set(lua_State *L) 
{
    const char *key, *raw_key;
    const char *value = NULL;
    size_t str_len;
    lua_ivm_object *object = NULL;
    request_rec *r = ap_lua_check_request_rec(L, 1);
    key = luaL_checkstring(L, 2);
    luaL_checkany(L, 3);
    raw_key = apr_pstrcat(r->pool, "lua_ivm_", key, NULL);
    
    apr_thread_mutex_lock(lua_ivm_mutex);
    apr_pool_userdata_get((void **)&object, raw_key, r->server->process->pool);
    if (!object) {
        object = apr_pcalloc(r->server->process->pool, sizeof(lua_ivm_object));
        ap_varbuf_init(r->server->process->pool, &object->vb, 2);
        object->size = 1;
        object->vb_size = 1;
    }
    object->type = lua_type(L, 3);
    if (object->type == LUA_TNUMBER) object->number = lua_tonumber(L, 3);
    else if (object->type == LUA_TBOOLEAN) object->number = lua_tonumber(L, 3);
    else if (object->type == LUA_TSTRING) {
        value = lua_tolstring(L, 3, &str_len);
        str_len++; /* add trailing \0 */
        if ( str_len > object->vb_size) {
            ap_varbuf_grow(&object->vb, str_len);
            object->vb_size = str_len;
        }
        object->size = str_len-1;
        memset(object->vb.buf, 0, str_len);
        memcpy(object->vb.buf, value, str_len-1);
    }
    apr_pool_userdata_set(object, raw_key, NULL, r->server->process->pool);
    apr_thread_mutex_unlock(lua_ivm_mutex);
    return 0;
}

#define APLUA_REQ_TRACE(lev) static int req_trace##lev(lua_State *L)  \
{                                                               \
    return req_log_at(L, APLOG_TRACE##lev);                     \
}

APLUA_REQ_TRACE(1)
APLUA_REQ_TRACE(2)
APLUA_REQ_TRACE(3)
APLUA_REQ_TRACE(4)
APLUA_REQ_TRACE(5)
APLUA_REQ_TRACE(6)
APLUA_REQ_TRACE(7)
APLUA_REQ_TRACE(8)

/* handle r.status = 201 */
static int req_newindex(lua_State *L)
{
    const char *key;
    /* request_rec* r = lua_touserdata(L, lua_upvalueindex(1)); */
    /* const char* key = luaL_checkstring(L, -2); */
    request_rec *r = ap_lua_check_request_rec(L, 1);
    key = luaL_checkstring(L, 2);

    if (0 == strcmp("args", key)) {
        const char *value = luaL_checkstring(L, 3);
        r->args = apr_pstrdup(r->pool, value);
        return 0;
    }

    if (0 == strcmp("content_type", key)) {
        const char *value = luaL_checkstring(L, 3);
        ap_set_content_type(r, apr_pstrdup(r->pool, value));
        return 0;
    }

    if (0 == strcmp("filename", key)) {
        const char *value = luaL_checkstring(L, 3);
        r->filename = apr_pstrdup(r->pool, value);
        return 0;
    }

    if (0 == strcmp("handler", key)) {
        const char *value = luaL_checkstring(L, 3);
        r->handler = apr_pstrdup(r->pool, value);
        return 0;
    }

    if (0 == strcmp("proxyreq", key)) {
        int value = luaL_checkinteger(L, 3);
        r->proxyreq = value;
        return 0;
    }

    if (0 == strcmp("status", key)) {
        int code = luaL_checkinteger(L, 3);
        r->status = code;
        return 0;
    }

    if (0 == strcmp("uri", key)) {
        const char *value = luaL_checkstring(L, 3);
        r->uri = apr_pstrdup(r->pool, value);
        return 0;
    }

    if (0 == strcmp("user", key)) {
        const char *value = luaL_checkstring(L, 3);
        r->user = apr_pstrdup(r->pool, value);
        return 0;
    }

    lua_pushstring(L,
                   apr_psprintf(r->pool,
                                "Property [%s] may not be set on a request_rec",
                                key));
    lua_error(L);
    return 0;
}

static const struct luaL_Reg request_methods[] = {
    {"__index", req_dispatch},
    {"__newindex", req_newindex},
    /*   {"__newindex", req_set_field}, */
    {NULL, NULL}
};


static const struct luaL_Reg connection_methods[] = {
    {NULL, NULL}
};

static const char* lua_ap_auth_name(request_rec* r)
{
    const char *name;
    name = ap_auth_name(r);
    return name ? name : "";
}

static const char* lua_ap_get_server_name(request_rec* r)
{
    const char *name;
    name = ap_get_server_name(r);
    return name ? name : "localhost";
}



static const struct luaL_Reg server_methods[] = {
    {NULL, NULL}
};


static req_fun_t *makefun(const void *fun, int type, apr_pool_t *pool)
{
    req_fun_t *rft = apr_palloc(pool, sizeof(req_fun_t));
    rft->fun = fun;
    rft->type = type;
    return rft;
}

void ap_lua_load_request_lmodule(lua_State *L, apr_pool_t *p)
{

    apr_hash_t *dispatch = apr_hash_make(p);

    apr_hash_set(dispatch, "puts", APR_HASH_KEY_STRING,
                 makefun(&req_puts, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "write", APR_HASH_KEY_STRING,
                 makefun(&req_write, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "document_root", APR_HASH_KEY_STRING,
                 makefun(&req_document_root, APL_REQ_FUNTYPE_STRING, p));
    apr_hash_set(dispatch, "context_prefix", APR_HASH_KEY_STRING,
                 makefun(&req_context_prefix, APL_REQ_FUNTYPE_STRING, p));
    apr_hash_set(dispatch, "context_document_root", APR_HASH_KEY_STRING,
                 makefun(&req_context_document_root, APL_REQ_FUNTYPE_STRING, p));
    apr_hash_set(dispatch, "parseargs", APR_HASH_KEY_STRING,
                 makefun(&req_parseargs, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "parsebody", APR_HASH_KEY_STRING,
                 makefun(&req_parsebody, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "debug", APR_HASH_KEY_STRING,
                 makefun(&req_debug, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "info", APR_HASH_KEY_STRING,
                 makefun(&req_info, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "notice", APR_HASH_KEY_STRING,
                 makefun(&req_notice, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "warn", APR_HASH_KEY_STRING,
                 makefun(&req_warn, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "err", APR_HASH_KEY_STRING,
                 makefun(&req_err, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "crit", APR_HASH_KEY_STRING,
                 makefun(&req_crit, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "alert", APR_HASH_KEY_STRING,
                 makefun(&req_alert, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "emerg", APR_HASH_KEY_STRING,
                 makefun(&req_emerg, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "trace1", APR_HASH_KEY_STRING,
                 makefun(&req_trace1, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "trace2", APR_HASH_KEY_STRING,
                 makefun(&req_trace2, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "trace3", APR_HASH_KEY_STRING,
                 makefun(&req_trace3, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "trace4", APR_HASH_KEY_STRING,
                 makefun(&req_trace4, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "trace5", APR_HASH_KEY_STRING,
                 makefun(&req_trace5, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "trace6", APR_HASH_KEY_STRING,
                 makefun(&req_trace6, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "trace7", APR_HASH_KEY_STRING,
                 makefun(&req_trace7, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "trace8", APR_HASH_KEY_STRING,
                 makefun(&req_trace8, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "add_output_filter", APR_HASH_KEY_STRING,
                 makefun(&req_add_output_filter, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "construct_url", APR_HASH_KEY_STRING,
                 makefun(&req_construct_url, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "escape_html", APR_HASH_KEY_STRING,
                 makefun(&req_escape_html, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "ssl_var_lookup", APR_HASH_KEY_STRING,
                 makefun(&req_ssl_var_lookup, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "is_https", APR_HASH_KEY_STRING,
                 makefun(&req_ssl_is_https_field, APL_REQ_FUNTYPE_BOOLEAN, p));
    apr_hash_set(dispatch, "assbackwards", APR_HASH_KEY_STRING,
                 makefun(&req_assbackwards_field, APL_REQ_FUNTYPE_BOOLEAN, p));
    apr_hash_set(dispatch, "status", APR_HASH_KEY_STRING,
                 makefun(&req_status_field, APL_REQ_FUNTYPE_INT, p));
    apr_hash_set(dispatch, "protocol", APR_HASH_KEY_STRING,
                 makefun(&req_protocol_field, APL_REQ_FUNTYPE_STRING, p));
    apr_hash_set(dispatch, "range", APR_HASH_KEY_STRING,
                 makefun(&req_range_field, APL_REQ_FUNTYPE_STRING, p));
    apr_hash_set(dispatch, "content_type", APR_HASH_KEY_STRING,
                 makefun(&req_content_type_field, APL_REQ_FUNTYPE_STRING, p));
    apr_hash_set(dispatch, "content_encoding", APR_HASH_KEY_STRING,
                 makefun(&req_content_encoding_field, APL_REQ_FUNTYPE_STRING,
                         p));
    apr_hash_set(dispatch, "ap_auth_type", APR_HASH_KEY_STRING,
                 makefun(&req_ap_auth_type_field, APL_REQ_FUNTYPE_STRING, p));
    apr_hash_set(dispatch, "unparsed_uri", APR_HASH_KEY_STRING,
                 makefun(&req_unparsed_uri_field, APL_REQ_FUNTYPE_STRING, p));
    apr_hash_set(dispatch, "user", APR_HASH_KEY_STRING,
                 makefun(&req_user_field, APL_REQ_FUNTYPE_STRING, p));
    apr_hash_set(dispatch, "filename", APR_HASH_KEY_STRING,
                 makefun(&req_filename_field, APL_REQ_FUNTYPE_STRING, p));
    apr_hash_set(dispatch, "canonical_filename", APR_HASH_KEY_STRING,
                 makefun(&req_canonical_filename_field,
                         APL_REQ_FUNTYPE_STRING, p));
    apr_hash_set(dispatch, "path_info", APR_HASH_KEY_STRING,
                 makefun(&req_path_info_field, APL_REQ_FUNTYPE_STRING, p));
    apr_hash_set(dispatch, "args", APR_HASH_KEY_STRING,
                 makefun(&req_args_field, APL_REQ_FUNTYPE_STRING, p));
    apr_hash_set(dispatch, "handler", APR_HASH_KEY_STRING,
                 makefun(&req_handler_field, APL_REQ_FUNTYPE_STRING, p));
    apr_hash_set(dispatch, "hostname", APR_HASH_KEY_STRING,
                 makefun(&req_hostname_field, APL_REQ_FUNTYPE_STRING, p));
    apr_hash_set(dispatch, "uri", APR_HASH_KEY_STRING,
                 makefun(&req_uri_field, APL_REQ_FUNTYPE_STRING, p));
    apr_hash_set(dispatch, "the_request", APR_HASH_KEY_STRING,
                 makefun(&req_the_request_field, APL_REQ_FUNTYPE_STRING, p));
    apr_hash_set(dispatch, "log_id", APR_HASH_KEY_STRING,
                 makefun(&req_log_id_field, APL_REQ_FUNTYPE_STRING, p));
    apr_hash_set(dispatch, "useragent_ip", APR_HASH_KEY_STRING,
                 makefun(&req_useragent_ip_field, APL_REQ_FUNTYPE_STRING, p));
    apr_hash_set(dispatch, "method", APR_HASH_KEY_STRING,
                 makefun(&req_method_field, APL_REQ_FUNTYPE_STRING, p));
    apr_hash_set(dispatch, "proxyreq", APR_HASH_KEY_STRING,
                 makefun(&req_proxyreq_field, APL_REQ_FUNTYPE_STRING, p));
    apr_hash_set(dispatch, "headers_in", APR_HASH_KEY_STRING,
                 makefun(&req_headers_in, APL_REQ_FUNTYPE_TABLE, p));
    apr_hash_set(dispatch, "headers_out", APR_HASH_KEY_STRING,
                 makefun(&req_headers_out, APL_REQ_FUNTYPE_TABLE, p));
    apr_hash_set(dispatch, "err_headers_out", APR_HASH_KEY_STRING,
                 makefun(&req_err_headers_out, APL_REQ_FUNTYPE_TABLE, p));
    apr_hash_set(dispatch, "notes", APR_HASH_KEY_STRING,
                 makefun(&req_notes, APL_REQ_FUNTYPE_TABLE, p));
    apr_hash_set(dispatch, "subprocess_env", APR_HASH_KEY_STRING,
                 makefun(&req_subprocess_env, APL_REQ_FUNTYPE_TABLE, p));
    apr_hash_set(dispatch, "flush", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_rflush, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "port", APR_HASH_KEY_STRING,
                 makefun(&req_ap_get_server_port, APL_REQ_FUNTYPE_INT, p));
    apr_hash_set(dispatch, "banner", APR_HASH_KEY_STRING,
                 makefun(&ap_get_server_banner, APL_REQ_FUNTYPE_STRING, p));
    apr_hash_set(dispatch, "options", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_options, APL_REQ_FUNTYPE_STRING, p));
    apr_hash_set(dispatch, "allowoverrides", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_allowoverrides, APL_REQ_FUNTYPE_STRING, p));
    apr_hash_set(dispatch, "started", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_started, APL_REQ_FUNTYPE_INT, p));
    apr_hash_set(dispatch, "basic_auth_pw", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_basic_auth_pw, APL_REQ_FUNTYPE_STRING, p));
    apr_hash_set(dispatch, "limit_req_body", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_limit_req_body, APL_REQ_FUNTYPE_INT, p));
    apr_hash_set(dispatch, "server_built", APR_HASH_KEY_STRING,
                 makefun(&ap_get_server_built, APL_REQ_FUNTYPE_STRING, p));
    apr_hash_set(dispatch, "is_initial_req", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_is_initial_req, APL_REQ_FUNTYPE_BOOLEAN, p));
    apr_hash_set(dispatch, "remaining", APR_HASH_KEY_STRING,
                 makefun(&req_remaining_field, APL_REQ_FUNTYPE_INT, p));
    apr_hash_set(dispatch, "some_auth_required", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_some_auth_required, APL_REQ_FUNTYPE_BOOLEAN, p));
    apr_hash_set(dispatch, "server_name", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_get_server_name, APL_REQ_FUNTYPE_STRING, p));
    apr_hash_set(dispatch, "auth_name", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_auth_name, APL_REQ_FUNTYPE_STRING, p));
    apr_hash_set(dispatch, "sendfile", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_sendfile, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "dbacquire", APR_HASH_KEY_STRING,
                 makefun(&lua_db_acquire, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "stat", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_stat, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "get_direntries", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_getdir, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "regex", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_regex, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "usleep", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_usleep, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "base64_encode", APR_HASH_KEY_STRING,
                 makefun(&lua_apr_b64encode, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "base64_decode", APR_HASH_KEY_STRING,
                 makefun(&lua_apr_b64decode, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "md5", APR_HASH_KEY_STRING,
                 makefun(&lua_apr_md5, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "sha1", APR_HASH_KEY_STRING,
                 makefun(&lua_apr_sha1, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "escape", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_escape, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "unescape", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_unescape, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "mpm_query", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_mpm_query, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "expr", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_expr, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "scoreboard_process", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_scoreboard_process, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "scoreboard_worker", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_scoreboard_worker, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "clock", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_clock, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "requestbody", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_requestbody, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "add_input_filter", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_add_input_filter, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "module_info", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_module_info, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "loaded_modules", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_loaded_modules, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "runtime_dir_relative", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_runtime_dir_relative, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "server_info", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_server_info, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "set_document_root", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_set_document_root, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "set_context_info", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_set_context_info, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "os_escape_path", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_os_escape_path, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "escape_logitem", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_escape_logitem, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "strcmp_match", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_strcmp_match, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "set_keepalive", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_set_keepalive, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "make_etag", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_make_etag, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "send_interim_response", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_send_interim_response, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "custom_response", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_custom_response, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "exists_config_define", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_exists_config_define, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "state_query", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_state_query, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "get_server_name_for_url", APR_HASH_KEY_STRING,
                 makefun(&lua_ap_get_server_name_for_url, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "ivm_get", APR_HASH_KEY_STRING,
                 makefun(&lua_ivm_get, APL_REQ_FUNTYPE_LUACFUN, p));
    apr_hash_set(dispatch, "ivm_set", APR_HASH_KEY_STRING,
                 makefun(&lua_ivm_set, APL_REQ_FUNTYPE_LUACFUN, p));
    
    lua_pushlightuserdata(L, dispatch);
    lua_setfield(L, LUA_REGISTRYINDEX, "Apache2.Request.dispatch");

    luaL_newmetatable(L, "Apache2.Request");    /* [metatable] */
    lua_pushvalue(L, -1);

    lua_setfield(L, -2, "__index");
    luaL_register(L, NULL, request_methods);    /* [metatable] */

    lua_pop(L, 2);

    luaL_newmetatable(L, "Apache2.Connection"); /* [metatable] */
    lua_pushvalue(L, -1);

    lua_setfield(L, -2, "__index");
    luaL_register(L, NULL, connection_methods); /* [metatable] */

    lua_pop(L, 2);

    luaL_newmetatable(L, "Apache2.Server");     /* [metatable] */
    lua_pushvalue(L, -1);

    lua_setfield(L, -2, "__index");
    luaL_register(L, NULL, server_methods);     /* [metatable] */

    lua_pop(L, 2);

}

void ap_lua_push_connection(lua_State *L, conn_rec *c)
{
    lua_boxpointer(L, c);
    luaL_getmetatable(L, "Apache2.Connection");
    lua_setmetatable(L, -2);
    luaL_getmetatable(L, "Apache2.Connection");

    ap_lua_push_apr_table(L, c->notes);
    lua_setfield(L, -2, "notes");

    lua_pushstring(L, c->client_ip);
    lua_setfield(L, -2, "client_ip");

    lua_pop(L, 1);
}


void ap_lua_push_server(lua_State *L, server_rec *s)
{
    lua_boxpointer(L, s);
    luaL_getmetatable(L, "Apache2.Server");
    lua_setmetatable(L, -2);
    luaL_getmetatable(L, "Apache2.Server");

    lua_pushstring(L, s->server_hostname);
    lua_setfield(L, -2, "server_hostname");

    lua_pop(L, 1);
}

void ap_lua_push_request(lua_State *L, request_rec *r)
{
    lua_boxpointer(L, r);
    luaL_getmetatable(L, "Apache2.Request");
    lua_setmetatable(L, -2);
}
