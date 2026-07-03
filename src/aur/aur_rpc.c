/* aur_rpc.c - AUR RPC client implementation
 *
 * Rewrites paru's raur crate in C. Uses libcurl for HTTP,
 * cJSON for JSON parsing.
 *
 * AUR RPC v5: https://aur.archlinux.org/rpc
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

#include "cJSON.h"
#include "aur_rpc.h"

#define AUR_RPC_URL "https://aur.archlinux.org/rpc/v5"

/* ── Curl write callback ─────────────────────────────────────────── */

typedef struct {
	char *data;
	size_t size;
} curl_buf_t;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	curl_buf_t *buf = (curl_buf_t *)userdata;
	size_t total = size * nmemb;
	char *new_data = realloc(buf->data, buf->size + total + 1);
	if (!new_data) return 0;
	buf->data = new_data;
	memcpy(buf->data + buf->size, ptr, total);
	buf->size += total;
	buf->data[buf->size] = '\0';
	return total;
}

/* ── HTTP GET helper ─────────────────────────────────────────────── */

static char *http_get(CURL *curl, const char *url)
{
	curl_buf_t buf = { .data = NULL, .size = 0 };

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "2O9/0.0.1");

	CURLcode res = curl_easy_perform(curl);
	if (res != CURLE_OK) {
		free(buf.data);
		return NULL;
	}

	return buf.data;
}

/* ── Parse string array from JSON ────────────────────────────────── */

static char **parse_string_array(const cJSON *arr, size_t *count)
{
	if (!cJSON_IsArray(arr)) {
		*count = 0;
		return NULL;
	}
	int n = cJSON_GetArraySize(arr);
	char **result = calloc(n + 1, sizeof(char *));
	for (int i = 0; i < n; i++) {
		cJSON *item = cJSON_GetArrayItem(arr, i);
		if (cJSON_IsString(item)) {
			result[i] = strdup(item->valuestring);
		}
	}
	*count = n;
	return result;
}

/* ── Parse a single AUR package from JSON ─────────────────────────── */

static aur_pkg_t *parse_pkg(const cJSON *obj)
{
	if (!cJSON_IsObject(obj)) return NULL;

	aur_pkg_t *pkg = calloc(1, sizeof(*pkg));
	if (!pkg) return NULL;

	cJSON *v;

	v = cJSON_GetObjectItem(obj, "Name");
	pkg->name = v && cJSON_IsString(v) ? strdup(v->valuestring) : NULL;

	v = cJSON_GetObjectItem(obj, "PackageBase");
	pkg->pkgbase = v && cJSON_IsString(v) ? strdup(v->valuestring) : NULL;

	v = cJSON_GetObjectItem(obj, "Version");
	pkg->version = v && cJSON_IsString(v) ? strdup(v->valuestring) : NULL;

	v = cJSON_GetObjectItem(obj, "Description");
	pkg->description = v && cJSON_IsString(v) ? strdup(v->valuestring) : NULL;

	v = cJSON_GetObjectItem(obj, "URL");
	pkg->url = v && cJSON_IsString(v) ? strdup(v->valuestring) : NULL;

	v = cJSON_GetObjectItem(obj, "URLPath");
	pkg->url_path = v && cJSON_IsString(v) ? strdup(v->valuestring) : NULL;

	v = cJSON_GetObjectItem(obj, "NumVotes");
	pkg->num_votes = v ? v->valueint : 0;

	v = cJSON_GetObjectItem(obj, "Popularity");
	pkg->popularity = v ? (int)v->valuedouble : 0;

	v = cJSON_GetObjectItem(obj, "OutOfDate");
	pkg->out_of_date = v ? v->valueint : 0;

	v = cJSON_GetObjectItem(obj, "Maintainer");
	pkg->maintainer = v && cJSON_IsString(v) ? strdup(v->valuestring) : NULL;

	pkg->depends = parse_string_array(cJSON_GetObjectItem(obj, "Depends"),
	                                  &pkg->depends_count);
	pkg->makedepends = parse_string_array(cJSON_GetObjectItem(obj, "MakeDepends"),
	                                      &pkg->makedepends_count);
	pkg->checkdepends = parse_string_array(cJSON_GetObjectItem(obj, "CheckDepends"),
	                                       &pkg->checkdepends_count);
	pkg->optdepends = parse_string_array(cJSON_GetObjectItem(obj, "OptDepends"),
	                                     &pkg->optdepends_count);
	pkg->conflicts = parse_string_array(cJSON_GetObjectItem(obj, "Conflicts"),
	                                    &pkg->conflicts_count);
	pkg->provides = parse_string_array(cJSON_GetObjectItem(obj, "Provides"),
	                                   &pkg->provides_count);
	pkg->replaces = parse_string_array(cJSON_GetObjectItem(obj, "Replaces"),
	                                   &pkg->replaces_count);
	pkg->groups = parse_string_array(cJSON_GetObjectItem(obj, "Groups"),
	                                 &pkg->groups_count);
	pkg->licenses = parse_string_array(cJSON_GetObjectItem(obj, "License"),
	                                   &pkg->licenses_count);
	pkg->keywords = parse_string_array(cJSON_GetObjectItem(obj, "Keywords"),
	                                   &pkg->keywords_count);

	return pkg;
}

/* ── Parse RPC response ──────────────────────────────────────────── */

static aur_rpc_result_t parse_response(const char *json)
{
	aur_rpc_result_t result = {0};

	cJSON *root = cJSON_Parse(json);
	if (!root) {
		result.success = 0;
		result.error = strdup("failed to parse JSON");
		return result;
	}

	cJSON *type = cJSON_GetObjectItem(root, "type");
	if (type && cJSON_IsString(type) &&
	    strcmp(type->valuestring, "error") == 0) {
		result.success = 0;
		cJSON *err = cJSON_GetObjectItem(root, "error");
		result.error = err && cJSON_IsString(err)
		               ? strdup(err->valuestring)
		               : strdup("unknown error");
		cJSON_Delete(root);
		return result;
	}

	result.success = 1;

	cJSON *results = cJSON_GetObjectItem(root, "results");
	if (cJSON_IsArray(results)) {
		int n = cJSON_GetArraySize(results);
		result.count = n;
		aur_pkg_t **tail = &result.packages;
		for (int i = 0; i < n; i++) {
			aur_pkg_t *pkg = parse_pkg(cJSON_GetArrayItem(results, i));
			if (pkg) {
				*tail = pkg;
				tail = &pkg->next;
			}
		}
	}

	cJSON_Delete(root);
	return result;
}

/* ── Public API ──────────────────────────────────────────────────── */

aur_cache_t *aur_cache_open(const char *base_url)
{
	aur_cache_t *cache = calloc(1, sizeof(*cache));
	if (!cache) return NULL;

	cache->base_url = base_url ? strdup(base_url)
	                           : strdup(AUR_RPC_URL);

	curl_global_init(CURL_GLOBAL_DEFAULT);
	cache->curl_handle = curl_easy_init();

	if (!cache->curl_handle) {
		free(cache->base_url);
		free(cache);
		return NULL;
	}

	return cache;
}

void aur_cache_close(aur_cache_t *cache)
{
	if (!cache) return;
	if (cache->curl_handle)
		curl_easy_cleanup((CURL *)cache->curl_handle);
	curl_global_cleanup();
	free(cache->base_url);
	free(cache);
}

aur_rpc_result_t aur_search(aur_cache_t *cache, const char *query,
                            const char *by_field)
{
	aur_rpc_result_t empty = {0};
	if (!cache || !query) return empty;

	char url[2048];
	if (by_field) {
		snprintf(url, sizeof(url), "%s?type=search&arg=%s&by=%s",
		         cache->base_url, query, by_field);
	} else {
		snprintf(url, sizeof(url), "%s?type=search&arg=%s",
		         cache->base_url, query);
	}

	char *json = http_get((CURL *)cache->curl_handle, url);
	if (!json) {
		empty.error = strdup("HTTP request failed");
		return empty;
	}

	aur_rpc_result_t result = parse_response(json);
	free(json);
	return result;
}

aur_rpc_result_t aur_info(aur_cache_t *cache, const char *pkg_name)
{
	const char *names[] = { pkg_name };
	return aur_info_batch(cache, names, 1);
}

aur_rpc_result_t aur_info_batch(aur_cache_t *cache,
                                const char **names, size_t count)
{
	aur_rpc_result_t empty = {0};
	if (!cache || !names || count == 0) return empty;

	/* Build URL: /rpc/v5?type=info&arg[]=pkg1&arg[]=pkg2... */
	char url[8192] = {0};
	int pos = snprintf(url, sizeof(url), "%s?type=info", cache->base_url);

	for (size_t i = 0; i < count && pos < (int)sizeof(url) - 256; i++) {
		/* URL-encode the name (basic: just handle spaces/special) */
		char *encoded = curl_easy_escape((CURL *)cache->curl_handle,
		                                names[i], 0);
		if (encoded) {
			pos += snprintf(url + pos, sizeof(url) - pos,
			                "&arg[]=%s", encoded);
			curl_free(encoded);
		}
	}

	char *json = http_get((CURL *)cache->curl_handle, url);
	if (!json) {
		empty.error = strdup("HTTP request failed");
		return empty;
	}

	aur_rpc_result_t result = parse_response(json);
	free(json);
	return result;
}

/* ── Free ────────────────────────────────────────────────────────── */

static void free_string_array(char **arr, size_t count)
{
	if (!arr) return;
	for (size_t i = 0; i < count; i++)
		free(arr[i]);
	free(arr);
}

void aur_pkg_free(aur_pkg_t *pkg)
{
	if (!pkg) return;
	free(pkg->name);
	free(pkg->pkgbase);
	free(pkg->version);
	free(pkg->description);
	free(pkg->url);
	free(pkg->url_path);
	free(pkg->maintainer);
	free_string_array(pkg->depends, pkg->depends_count);
	free_string_array(pkg->makedepends, pkg->makedepends_count);
	free_string_array(pkg->checkdepends, pkg->checkdepends_count);
	free_string_array(pkg->optdepends, pkg->optdepends_count);
	free_string_array(pkg->conflicts, pkg->conflicts_count);
	free_string_array(pkg->provides, pkg->provides_count);
	free_string_array(pkg->replaces, pkg->replaces_count);
	free_string_array(pkg->groups, pkg->groups_count);
	free_string_array(pkg->licenses, pkg->licenses_count);
	free_string_array(pkg->keywords, pkg->keywords_count);
	free(pkg);
}

void aur_rpc_result_free(aur_rpc_result_t *r)
{
	if (!r) return;
	free(r->error);
	aur_pkg_t *pkg = r->packages;
	while (pkg) {
		aur_pkg_t *next = pkg->next;
		aur_pkg_free(pkg);
		pkg = next;
	}
}
