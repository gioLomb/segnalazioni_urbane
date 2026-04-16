#include "route_handler.h"
#include "server_functions.h"
/*PRIVATE FUNCTIONS DECLARATIONS*/

/* Returns 1 if the input contains only printable characters, decoding
   percent-encoded sequences before checking. Returns 0 otherwise.*/
static int is_sanitized(const char *input);

/**
 * Escape some special chars in the value to avoid injection.
 * It returns 0 if the escaped value is longer than dest size, 1 otherwise.
 */
static int value_escaping(const char *src, char *dest, size_t destSize);

/* Extracts the URL path and query string from the first line of an HTTP request
   into dest. Writes an empty string if the line is malformed or the
   result would exceed maxLen. */
static void extract_url(char *firstLineRequest,char *dest,size_t maxLen);

/* Extracts the value of the specified query parameter from the URL into bufferDest.
 Writes an empty string if the parameter is not found or the value exceeds maxLen.*/
static void get_query_param(const char *url,const char* paramName,char* destBuffer,size_t maxLen);

/* wrapper functions for hash table interfaces */
static int handler_get(Hash_Table *table, const char *url, char *responseBuffer);
static int handler_set(Hash_Table *table, const char *url, char *responseBuffer);
static int handler_delete(Hash_Table *table, const char *url, char *responseBuffer);
static int handler_stats(Hash_Table *table, const char *url, char *responseBuffer);

/**
 * Serves the compressed index.html.gz using mmap.
 * Returns (10000 + file_size) to signal GZIP encoding and binary safe length 
 * to the response sender, or 500 on failure.
 */
static int handler_home(Hash_Table *table, const char *url, char *responseBuffer);

/* ROUTES */
static Route routes[]={
    {"/",handler_home},
    {"/get",handler_get},
    {"/set",handler_set},
    {"/delete",handler_delete},
    {"/stats",handler_stats},
};

/*DEFINITIONS*/

int handle_request(Hash_Table* db, char *requestBuffer, char* responseBuffer,int *keepAlive) {
    char url[URL_BUFFER_SIZE] = {0};
    char localCopy[BUFFER_SIZE];

    strncpy(localCopy, requestBuffer, BUFFER_SIZE - 1);
    localCopy[BUFFER_SIZE - 1] = '\0';
    
    *keepAlive = (strstr(requestBuffer, "Connection: keep-alive") != NULL) ? 1 : 0;

    //define thread local memory for thread-safety strtok
    //operate on localCopy, not requestBuffer, to avoid corrupting ctx->buffer
    char* saverPtr;
    char *firstLine = strtok_r(localCopy, "\n",&saverPtr);

    if(!firstLine) {
        snprintf(responseBuffer, BUFFER_SIZE, "Bad Request\n");
        return 400;
    }

    //search for GET method
    if (strncmp(firstLine, "GET ", 4) != 0) {
        snprintf(responseBuffer, BUFFER_SIZE, "Method Not Allowed\n");
        return 405;
    }

    extract_url(firstLine, url,URL_BUFFER_SIZE);

    // isolate path component (stop at '?' or end of string)
    const char *qmark = strchr(url, '?');
    size_t pathLen = qmark ? (size_t)(qmark - url) : strlen(url);

    // iterate over the static routes array
    for(size_t i = 0; i < (sizeof(routes)/sizeof(routes[0])); i++){
        size_t routeLen = strlen(routes[i].path);
        // exact path match: lengths must be equal before comparing bytes
        if(pathLen == routeLen && strncmp(url, routes[i].path, routeLen) == 0){
            return routes[i].handler(db, url, responseBuffer);
        }
    }
    
    //if comparation turned out bad
    //printf("Bad routes url: %s",url);
    snprintf(responseBuffer, BUFFER_SIZE, "route does not exist\n");
    return 404;
}


static int value_escaping(const char *src, char *dest, size_t destSize) {
    size_t i = 0;
    while (*src) {
        char c = *src++;
        // every escaped char is max 2 bytes
        if (i + 2 >= destSize) return 0;

        switch (c) {
            case '"':  dest[i++] = '\\'; dest[i++] = '"';  break;
            case '\\': dest[i++] = '\\'; dest[i++] = '\\'; break;
            case '\n': dest[i++] = '\\'; dest[i++] = 'n';  break;
            case '\r': dest[i++] = '\\'; dest[i++] = 'r';  break;
            case '\t': dest[i++] = '\\'; dest[i++] = 't';  break;
            default:
                // ctrl chars mapped to \uXXXX
                if ((unsigned char)c < 0x20) {
                    if (i + 6 >= destSize) return 0;
                    i += snprintf(dest + i, destSize - i, "\\u%04x", (unsigned char)c);
                } else {
                    dest[i++] = c;
                }
                break;
        }
    }
    dest[i] = '\0';
    return 1;
}

static int handler_home(Hash_Table *table, const char *url, char *responseBuffer){
    (void)url;(void)table;
    //static variables for index.html mapping
    static char *mapped_data = NULL;
    static size_t mapped_size = 0;
    static int init_failed = 0;

    if (mapped_data == NULL && !init_failed) {
        int fd = open("index.html.gz", O_RDONLY);
        if (fd != -1) {
            struct stat st;
            if (fstat(fd, &st) == 0) {
                mapped_size = st.st_size;
                mapped_data = mmap(NULL, mapped_size, PROT_READ, MAP_PRIVATE, fd, 0);
            }
            close(fd);
        }

        if (mapped_data == MAP_FAILED || mapped_data == NULL) {
            init_failed = 1;
            fprintf(stderr, "Error: logical mapping of index.html failed\n");
        }
    }

    if (init_failed) {
        snprintf(responseBuffer, RESPONSE_BUFFER_SIZE, "500 Internal Server Error - Interface Missing\n");
        return 500;
    }

    //write file content
    memcpy(responseBuffer, mapped_data, mapped_size);
    responseBuffer[mapped_size] = '\0';

    //special status code for gzip encoding
    return 10000 + (int)mapped_size;
}

static int handler_stats(Hash_Table *table, const char *url, char *responseBuffer) {
    (void)url; //ignore param
    time_t uptime = time(NULL) - stats.startTime;

    snprintf(responseBuffer, RESPONSE_BUFFER_SIZE,
        "{"
        "\"uptime_seconds\":%ld,"
        "\"totalRequests\":%lu,"
        "\"totalConnections\":%lu,"
        "\"total_keys\":%zu"
        "}\n",
        //stats taken without lock
        (long)uptime, stats.totalRequests, stats.totalConnections,table->size
    );
    return 200;
}

static int handler_get(Hash_Table *table, const char *url, char *responseBuffer) {
    char key[PARAM_KEY_SIZE]     = {0};
    char value[PARAM_VALUE_SIZE] = {0};    
    char escaped[PARAM_VALUE_SIZE * 2];    

    get_query_param(url, "key=", key, PARAM_KEY_SIZE);

    if (!key[0]) {
        snprintf(responseBuffer, RESPONSE_BUFFER_SIZE, "missing key\n");
        return 400;
    }

    if (!ht_get(table, key, strlen(key) + 1, value, PARAM_VALUE_SIZE)) {
        snprintf(responseBuffer, RESPONSE_BUFFER_SIZE, "key not exists\n");
        return 404;
    }

    if (!value_escaping(value, escaped, sizeof(escaped))) {
        snprintf(responseBuffer, RESPONSE_BUFFER_SIZE, "value too large to encode\n");
        return 500;
    }

    snprintf(responseBuffer, RESPONSE_BUFFER_SIZE, "{\"value\":\"%s\"}\n", escaped);
    return 200;
}

static int is_sanitized(const char *input) {
    if (!input) return 0;
    
    for (const char *p = input; *p; p++) {
        if (!isprint((unsigned char)*p)) {
            return 0;
        }
    }
    return 1;
}

static int handler_set(Hash_Table* table, const char* url, char* responseBuffer){
    char key[PARAM_KEY_SIZE] = {0};
    char val[PARAM_VALUE_SIZE] = {0};

    get_query_param(url, "key=", key, PARAM_KEY_SIZE);
    get_query_param(url, "val=", val, PARAM_VALUE_SIZE);

    if(key[0] && val[0] && is_sanitized(key) && is_sanitized(val)) {

        if(ht_set(table, key,strlen(key)+1, val, strlen(val) + 1)) {
            stats.keysModifiedSinceSnapshot++;
            snprintf(responseBuffer, BUFFER_SIZE, "stored\n");
            return 200;
        } else {
            snprintf(responseBuffer, BUFFER_SIZE, "ht_set failed\n");
            return 500;
        }

    }

    snprintf(responseBuffer, BUFFER_SIZE, "Bad params\n");
    return 400;

}

static int handler_delete(Hash_Table* table, const char* url, char* responseBuffer){
    char key[PARAM_KEY_SIZE] = {0};

    get_query_param(url, "key=", key, PARAM_KEY_SIZE);

    if(key[0]) {

        if(ht_delete(table, key,strlen(key)+1)) {
            stats.keysModifiedSinceSnapshot++;
            snprintf(responseBuffer, BUFFER_SIZE, "value deleted\n");
            return 200;
        } else {
            snprintf(responseBuffer, BUFFER_SIZE, "key not exists\n");
            return 404;
        }

    } else {
        snprintf(responseBuffer, BUFFER_SIZE, "missing key\n");
        return 400;
    }  
}

static inline int hex_to_int(char c) {
    int is_digit = (c >> 6) ^ 1;
    return (c & 0xF) + (is_digit ^ 1) * 9;
}


static void get_query_param(const char *url, const char *paramName, char *destBuffer, size_t maxLen) {
    if (!url || !paramName || !destBuffer || maxLen == 0) return;
    destBuffer[0] = '\0';

    //jump to ?
    const char *ptr = strchr(url, '?');
    if (!ptr) return;
    ptr++; 

    size_t paramLen = strlen(paramName);
    const char *found = NULL;

    // find the param key
    const char *curr = ptr;
    while ((found = strstr(curr, paramName)) != NULL) {
        //validate candidate key found
        if (found == ptr || *(found - 1) == '&') {
            ptr = found + paramLen; 
            break; 
        }
        curr = found + 1;
        found = NULL;
    }

    if (!found) return;

    size_t copied = 0;
    while (*ptr && *ptr != '&' && *ptr != ' ' && copied < (maxLen - 1)) {
        //decode hex bytes
        if (*ptr == '%' && isxdigit(ptr[1]) && isxdigit(ptr[2])) {
            
            //(high * 16) + low
            char decoded = (char)((hex_to_int(ptr[1]) << 4) | hex_to_int(ptr[2]));
            if (decoded == '\0') { destBuffer[0] = '\0'; return; } //blocks Null-embedded strings

            destBuffer[copied++] = decoded;
            ptr += 3;
        } else {
            if (*ptr == '+') destBuffer[copied++] = ' ';
            else destBuffer[copied++] = *ptr;
            ptr++;
        }
    }
    destBuffer[copied] = '\0';
}

static void extract_url(char *firstLineRequest,char *dest,size_t maxLen){
    dest[0] = '\0';
    
    char *ptr = firstLineRequest;
    
    //jump to the first space
    while(*ptr && *ptr != ' ') ptr++;

    //ignores multiple spaces
    while(*ptr && *ptr == ' ') ptr++;

    //copy effective url
    size_t copied = 0;
    while (*ptr && *ptr != ' ' && *ptr != '\r' && *ptr != '\n' && copied < (maxLen - 1)) {
        dest[copied++] = *ptr++;
    }
    dest[copied] = 0;
}