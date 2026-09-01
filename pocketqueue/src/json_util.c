/* json_util.c - cJSON wrappers. */
#include "json_util.h"

#include <stdio.h>
#include <stdlib.h>

#include "cJSON.h"
#include "str_util.h"
#include <string.h>

void pq_error_clear(pq_error *err)
{
    if (err == NULL) return;
    err->code[0] = '\0';
    err->message[0] = '\0';
    err->details_json[0] = '\0';
}

void pq_error_set(pq_error *err, const char *code, const char *message)
{
    if (err == NULL) return;
    pq_str_copy(err->code, sizeof(err->code), code ? code : "");
    pq_str_copy(err->message, sizeof(err->message), message ? message : "");
    err->details_json[0] = '\0';
}

cJSON *pq_json_parse(const char *data, size_t length, bool strict_top_level,
                     pq_error *err)
{
    (void)strict_top_level; /* enforced at call sites where needed */
    pq_error_clear(err);
    if (data == NULL) {
        pq_error_set(err, "invalid_json", "missing request body");
        return NULL;
    }
    cJSON *root = cJSON_ParseWithLength(data, length);
    if (root == NULL) {
        const char *p = cJSON_GetErrorPtr();
        char msg[PQ_ERROR_MESSAGE_MAX];
        snprintf(msg, sizeof(msg), "JSON parse error near: %.40s",
                 p ? p : "(unknown)");
        pq_error_set(err, "invalid_json", msg);
        return NULL;
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        pq_error_set(err, "invalid_request", "request body must be a JSON object");
        return NULL;
    }
    return root;
}

char *pq_json_dump(const cJSON *value, size_t *out_length)
{
    if (value == NULL) return NULL;
    char *s = cJSON_PrintUnformatted(value);
    if (s != NULL && out_length != NULL) {
        *out_length = strlen(s);
    }
    return s;
}

cJSON *pq_json_new_object(void)
{
    return cJSON_CreateObject();
}

cJSON *pq_json_new_error(const char *code, const char *message,
                         const cJSON *details)
{
    cJSON *err_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(err_obj, "code", code ? code : "");
    cJSON_AddStringToObject(err_obj, "message", message ? message : "");
    if (details != NULL) {
        cJSON_AddItemToObject(err_obj, "details", cJSON_Duplicate(details, 1));
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "error", err_obj);
    return root;
}