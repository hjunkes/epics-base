/*************************************************************************\
* Copyright (c) 2016 UChicago Argonne LLC, as Operator of Argonne
*     National Laboratory.
* EPICS BASE is distributed subject to a Software License Agreement found
* in file LICENSE that is included with this distribution. 
\*************************************************************************/
/* dbJLink.c */

#include <stdio.h>
#include <string.h>

#include "epicsAssert.h"
#include "dbmf.h"
#include "errlog.h"
#include "yajl_alloc.h"
#include "yajl_parse.h"

#define epicsExportSharedSybols
#include "dbAccessDefs.h"
#include "dbCommon.h"
#include "dbLink.h"
#include "dbJLink.h"
#include "dbStaticLib.h"
#include "link.h"

/* Change 'undef' to 'define' to turn on debug statements: */
#undef DEBUG_JLINK

#ifdef DEBUG_JLINK
    int jlinkDebug = 10;
#   define IFDEBUG(n) \
        if (jlinkDebug >= n) /* block or statement */
#else
#   define IFDEBUG(n) \
        if(0) /* Compiler will elide the block or statement */
#endif


typedef struct parseContext {
    jlink *pjlink;
    jlink *product;
    short dbfType;
    short jsonDepth;
    unsigned key_is_link:1;
} parseContext;

#define CALL_OR_STOP(routine) !(routine) ? jlif_stop : (routine)

static int dbjl_return(parseContext *parser, jlif_result result) {
    jlink *pjlink = parser->pjlink;

    IFDEBUG(10) {
        printf("dbjl_return(%s@%p, %d)\t", pjlink->pif->name, pjlink, result);
        printf("    jsonDepth=%d, parseDepth=%d, key_is_link=%d\n",
            parser->jsonDepth, pjlink->parseDepth, parser->key_is_link);
    }

    if (result == jlif_stop && pjlink) {
        jlink *parent;

        while ((parent = pjlink->parent)) {
            pjlink->pif->free_jlink(pjlink);
            pjlink = parent;
        }
        pjlink->pif->free_jlink(pjlink);
    }

    return result;
}

static int dbjl_value(parseContext *parser, jlif_result result) {
    jlink *pjlink = parser->pjlink;
    jlink *parent;

    IFDEBUG(10) {
        printf("dbjl_value(%s@%p, %d)\t", pjlink->pif->name, pjlink, result);
        printf("    jsonDepth=%d, parseDepth=%d, key_is_link=%d\n",
            parser->jsonDepth, pjlink->parseDepth, parser->key_is_link);
    }

    if (result == jlif_stop || pjlink->parseDepth > 0)
        return dbjl_return(parser, result);

    parent = pjlink->parent;
    if (!parent)
        parser->product = pjlink;
   else if (parent->pif->end_child)
        parent->pif->end_child(parent, pjlink);

    parser->pjlink = parent;

    IFDEBUG(8)
        printf("dbjl_value: product = %p\n", pjlink);

    return jlif_continue;
}

static int dbjl_null(void *ctx) {
    parseContext *parser = (parseContext *) ctx;
    jlink *pjlink = parser->pjlink;

    IFDEBUG(10)
        printf("dbjl_null(%s@%p)\n", pjlink->pif->name, pjlink);

    assert(pjlink);
    return dbjl_value(parser,
        CALL_OR_STOP(pjlink->pif->parse_null)(pjlink));
}

static int dbjl_boolean(void *ctx, int val) {
    parseContext *parser = (parseContext *) ctx;
    jlink *pjlink = parser->pjlink;

    assert(pjlink);
    return dbjl_value(parser,
        CALL_OR_STOP(pjlink->pif->parse_boolean)(pjlink, val));
}

static int dbjl_integer(void *ctx, long num) {
    parseContext *parser = (parseContext *) ctx;
    jlink *pjlink = parser->pjlink;

    IFDEBUG(10)
        printf("dbjl_integer(%s@%p, %ld)\n",
            pjlink->pif->name, pjlink, num);

    assert(pjlink);
    return dbjl_value(parser,
        CALL_OR_STOP(pjlink->pif->parse_integer)(pjlink, num));
}

static int dbjl_double(void *ctx, double num) {
    parseContext *parser = (parseContext *) ctx;
    jlink *pjlink = parser->pjlink;

    IFDEBUG(10)
        printf("dbjl_double(%s@%p, %g)\n",
            pjlink->pif->name, pjlink, num);

    assert(pjlink);
    return dbjl_value(parser,
        CALL_OR_STOP(pjlink->pif->parse_double)(pjlink, num));
}

static int dbjl_string(void *ctx, const unsigned char *val, unsigned len) {
    parseContext *parser = (parseContext *) ctx;
    jlink *pjlink = parser->pjlink;

    IFDEBUG(10)
        printf("dbjl_string(%s@%p, \"%.*s\")\n",
            pjlink->pif->name, pjlink, len, val);

    assert(pjlink);
    return dbjl_value(parser,
        CALL_OR_STOP(pjlink->pif->parse_string)(pjlink, (const char *) val, len));
}

static int dbjl_start_map(void *ctx) {
    parseContext *parser = (parseContext *) ctx;
    jlink *pjlink = parser->pjlink;
    jlif_key_result result;

    if (!pjlink) {
        IFDEBUG(10) {
            printf("dbjl_start_map(NULL)\t");
            printf("    jsonDepth=%d, parseDepth=00, key_is_link=%d\n",
                parser->jsonDepth, parser->key_is_link);
        }

        assert(parser->jsonDepth == 0);
        parser->jsonDepth++;
        parser->key_is_link = 1;
        return jlif_continue; /* Opening '{' */
    }

    IFDEBUG(10) {
        printf("dbjl_start_map(%s@%p)\t", pjlink->pif->name, pjlink);
        printf("    jsonDepth=%d, parseDepth=%d, key_is_link=%d\n",
            parser->jsonDepth, pjlink->parseDepth, parser->key_is_link);
    }

    pjlink->parseDepth++;
    parser->jsonDepth++;

    result = CALL_OR_STOP(pjlink->pif->parse_start_map)(pjlink);
    if (result == jlif_key_child_link) {
        parser->key_is_link = 1;
        result = jlif_continue;
    }

    IFDEBUG(10)
        printf("dbjl_start_map -> %d\n", result);

    return dbjl_return(parser, result);
}

static int dbjl_map_key(void *ctx, const unsigned char *key, unsigned len) {
    parseContext *parser = (parseContext *) ctx;
    jlink *pjlink = parser->pjlink;
    char *link_name;
    linkSup *linkSup;
    jlif *pjlif;

    if (!parser->key_is_link) {
        if (!pjlink) {
            errlogPrintf("dbJLinkInit: Illegal second link key '%.*s'\n",
                len, key);
            return dbjl_return(parser, jlif_stop);
        }

        IFDEBUG(10) {
            printf("dbjl_map_key(%s@%p, \"%.*s\")\t",
                pjlink->pif->name, pjlink, len, key);
            printf("    jsonDepth=%d, parseDepth=%d, key_is_link=%d\n",
                parser->jsonDepth, pjlink->parseDepth, parser->key_is_link);
        }

        assert(pjlink->parseDepth > 0);
        return dbjl_return(parser,
            CALL_OR_STOP(pjlink->pif->parse_map_key)(pjlink,
                (const char *) key, len));
    }

    IFDEBUG(10) {
        printf("dbjl_map_key(NULL, \"%.*s\")\t", len, key);
        printf("    jsonDepth=%d, parseDepth=00, key_is_link=%d\n",
            parser->jsonDepth, parser->key_is_link);
    }

    link_name = dbmfStrndup((const char *) key, len);

    linkSup = dbFindLinkSup(pdbbase, link_name);
    if (!linkSup) {
        errlogPrintf("dbJLinkInit: Link type '%s' not found\n",
            link_name);
        dbmfFree(link_name);
        return dbjl_return(parser, jlif_stop);
    }

    pjlif = linkSup->pjlif;
    if (!pjlif) {
        errlogPrintf("dbJLinkInit: Support for Link type '%s' not loaded\n",
            link_name);
        dbmfFree(link_name);
        return dbjl_return(parser, jlif_stop);
    }

    dbmfFree(link_name);

    pjlink = pjlif->alloc_jlink(parser->dbfType);
    if (!pjlink) {
        errlogPrintf("dbJLinkInit: Out of memory\n");
        return dbjl_return(parser, jlif_stop);
    }
    pjlink->pif = pjlif;

    if (parser->pjlink) {
        /* We're starting a child link, save its parent */
        pjlink->parent = parser->pjlink;
    }
    parser->pjlink = pjlink;
    parser->key_is_link = 0;

    IFDEBUG(8)
        printf("dbjl_map_key: New %s@%p\n", pjlink->pif->name, pjlink);

    return jlif_continue;
}

static int dbjl_end_map(void *ctx) {
    parseContext *parser = (parseContext *) ctx;
    jlink *pjlink = parser->pjlink;
    jlif_result result;

    IFDEBUG(10) {
        printf("dbjl_end_map(%s@%p)\t",
            pjlink ? pjlink->pif->name : "NULL", pjlink);
        printf("    jsonDepth=%d, parseDepth=%d, key_is_link=%d\n",
            parser->jsonDepth, pjlink ? pjlink->parseDepth : 0,
            parser->key_is_link);
    }

    parser->jsonDepth--;
    if (pjlink) {
        pjlink->parseDepth--;

        result = dbjl_value(parser,
            CALL_OR_STOP(pjlink->pif->parse_end_map)(pjlink));
    }
    else {
        result = jlif_continue;
    }
    return result;
}

static int dbjl_start_array(void *ctx) {
    parseContext *parser = (parseContext *) ctx;
    jlink *pjlink = parser->pjlink;

    IFDEBUG(10) {
        printf("dbjl_start_array(%s@%p)\t", pjlink->pif->name, pjlink);
        printf("    jsonDepth=%d, parseDepth=%d, key_is_link=%d\n",
            parser->jsonDepth, pjlink->parseDepth, parser->key_is_link);
    }

    assert(pjlink);
    pjlink->parseDepth++;
    parser->jsonDepth++;

    return dbjl_return(parser,
        CALL_OR_STOP(pjlink->pif->parse_start_array)(pjlink));
}

static int dbjl_end_array(void *ctx) {
    parseContext *parser = (parseContext *) ctx;
    jlink *pjlink = parser->pjlink;

    IFDEBUG(10) {
        printf("dbjl_end_array(%s@%p)\t", pjlink->pif->name, pjlink);
        printf("    jsonDepth=%d, parseDepth=%d, key_is_link=%d\n",
            parser->jsonDepth, pjlink->parseDepth, parser->key_is_link);
    }

    assert(pjlink);
    pjlink->parseDepth--;
    parser->jsonDepth--;

    return dbjl_value(parser,
        CALL_OR_STOP(pjlink->pif->parse_end_array)(pjlink));
}


static yajl_callbacks dbjl_callbacks = {
    dbjl_null, dbjl_boolean, dbjl_integer, dbjl_double, NULL, dbjl_string,
    dbjl_start_map, dbjl_map_key, dbjl_end_map, dbjl_start_array, dbjl_end_array
};

static const yajl_parser_config dbjl_config =
    { 0, 0 }; /* allowComments = NO, checkUTF8 = NO */

long dbJLinkParse(const char *json, size_t jlen, short dbfType,
    jlink **ppjlink)
{
    parseContext context, *parser = &context;
    yajl_alloc_funcs dbjl_allocs;
    yajl_handle yh;
    yajl_status ys;
    long status;

    IFDEBUG(10)
        printf("dbJLinkInit(\"%.*s\", %d, %p)\n",
            (int) jlen, json, dbfType, ppjlink);

    parser->pjlink = NULL;
    parser->product = NULL;
    parser->dbfType = dbfType;
    parser->jsonDepth = 0;
    parser->key_is_link = 0;

    IFDEBUG(10)
        printf("dbJLinkInit: jsonDepth=%d, key_is_link=%d\n",
            parser->jsonDepth, parser->key_is_link);

    yajl_set_default_alloc_funcs(&dbjl_allocs);
    yh = yajl_alloc(&dbjl_callbacks, &dbjl_config, &dbjl_allocs, parser);
    if (!yh)
        return S_db_noMemory;

    ys = yajl_parse(yh, (const unsigned char *) json, (unsigned) jlen);
    if (ys == yajl_status_insufficient_data)
        ys = yajl_parse_complete(yh);

    switch (ys) {
        unsigned char *err;

    case yajl_status_ok:
        assert(parser->jsonDepth == 0);
        *ppjlink = parser->product;
        status = 0;
        break;

    case yajl_status_error:
        err = yajl_get_error(yh, 1, (const unsigned char *) json, (unsigned) jlen);
        errlogPrintf("dbJLinkInit: %s\n", err);
        yajl_free_error(yh, err);
        /* fall through */
    default:
        status = S_db_badField;
    }

    yajl_free(yh);
    return status;
}

long dbJLinkInit(struct link *plink)
{
    jlink *pjlink;

    assert(plink);
    pjlink = plink->value.json.jlink;

    if (pjlink)
        plink->lset = pjlink->pif->get_lset(pjlink);

    dbLinkOpen(plink);
    return 0;
}

void dbJLinkFree(jlink *pjlink) {
    if (pjlink)
        pjlink->pif->free_jlink(pjlink);
}
